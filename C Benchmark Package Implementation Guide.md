# C Benchmark Package: Implementation Guide

Nov 7, 2026 · @Yerden Zhumabekov

> **Status.** Design agreed, not implemented. This local file is the source of truth. The online Claude Docs copy is stale: only its Goals section matches. The "Decision log" at the end records why each choice was made and what was rejected; read it before proposing changes.

## Goals and non-goals

The package measures the per-iteration cost of a C snippet called through a function pointer, in core cycles, in two modes (throughput and chained). It runs inside a DPDK process on a pinned worker lcore, writes one raw JSONL sample stream, and leaves statistics to small Python wrappers.

**Goals**

- Per-op cost in core cycles, reference cycles (TSC) and nanoseconds, from one sampling loop.
- Two modes: throughput (independent iterations, as many as fit) and chained (each output feeds the next input).
- Calibrated harness cost: call, loop and glue overhead measured on reference snippets, reported next to every result, and subtracted only where it is additive.
- Detection and rejection of contaminated samples: frequency swings, interrupts, preemption.
- Honest uncertainty: several epochs per process, each with a fresh memory layout, so the CI reflects layout and state variance, not only batch-to-batch noise.
- Before/after comparison across Git revisions: one JSONL file per build, compared by a script.
- Runs on developer machines (unprivileged, noisy) and on a dedicated CI runner (tuned), selected by a profile.

**Non-goals**

- Not a profiler: no call graphs, no sampling of hot spots inside the snippet.
- Not a macro-benchmark harness: snippets that run for milliseconds per call are out of scope.
- No inline variant: snippets are always called through a pointer, and the call cost is calibrated instead.
- No verdicts in the runner: it writes samples. The compare script reports effects with confidence intervals.
- x86-64 only, any vendor (Intel or AMD) and generation. No per-model tuning tables.
- Version 1 measures implementations one after another. Interleaving several implementations in paired rounds comes later.

## Architecture

The benchmark file declares data and implementations; the runner owns everything that touches the clock. The runner is a DPDK application: EAL starts once per process, measurement runs on one worker lcore, and the main lcore waits.

```
meson benchmark()  or  shell
  └─ bm_<name> <EAL args> -- <bench args>
       main lcore    rte_eal_init, environment checks, launch worker, wait
       worker lcore  calibrate → for each epoch: allocate, warm up, K batches
       main lcore    write records to JSONL (jansson)
  └─ <name>.jsonl  →  bm-report (human) | bm-go (benchstat) | bm-compare base new
```

> The architecture diagram in the online doc still shows the earlier design (three outputs written by the runner) and needs redrawing.

**Layers**

- **Measurement core** (`bm_core.c`, `bm_calib.c`): counters, fenced windows, timed loops, batch sizing, calibration, sample flags (`bm_thread_mark`). Plain C plus `perf_event_open`, no DPDK. Unit-testable on its own.
- **Runner** (`bm_run.c`, `bm_bench.c`, `bm_check.c`, `bm_env.c`, `bm_out.c`): EAL init, argument parsing and lcore launch (`bm_run.c`); benchmark selection, epochs, corpus placement, the pointer sweep and per-result verdicts (`bm_bench.c`); environment checks and profiles (`bm_check.c`); the env record (`bm_env.c`) and the other JSONL records via jansson (`bm_out.c`). Internals shared through `bm_internal.h`.
- **Wrappers** (`tools/`): `bm_stats.py` (statistics, a library), `bm-report`, `bm-go`, `bm-compare` (scripts), `test_bm_stats.py`. Python standard library only; Meson already guarantees `python3`.

Meson starts the process and sees only its exit code and stdout. Results travel through the JSONL file, and the wrappers read that, never Meson's log.

## Timing and counters

TSC counts reference cycles at a constant rate, so it cannot see frequency changes. Read core cycles from the PMU next to it, and use their ratio to judge each sample.

**Clock sources**

1. **TSC** for wall-time-like ticks. At startup check invariant TSC (`cpuid` leaf `0x80000007`, EDX bit 8). Take the frequency from `rte_get_tsc_hz()` and cross-check it once against `CLOCK_MONOTONIC_RAW` over about 100 ms; warn when they differ by more than 0.1%. Store `tsc_hz` in the env record.
2. **Core cycles** via `perf_event_open` with `PERF_COUNT_HW_CPU_CYCLES`, `pinned=1`, `exclude_kernel=1`, `exclude_hv=1`. Read it from userspace with `rdpmc` using the mmap'd `perf_event_mmap_page` (the seqlock loop on `lock`, `index`, `offset`, `pmc_width`). Keep a `read()` syscall fallback for when `cap_user_rdpmc` is 0. On Intel hybrid CPUs open the event on the PMU of the pinned core's type (`/sys/bus/event_source/devices/cpu_core` or `cpu_atom`, its `type` file), and record the core type.
3. **No PMU** (`perf_event_paranoid` of 3 or more, as on recent Ubuntu, or a VM without a virtual PMU): TSC-only mode. No ratio-based rejection, a loud warning, and `"pmu": false` in the env record. The tool still runs; the report says how far to trust it.
4. Optional group members read in the same group: instructions, branch misses, L1D/LLC misses, context switches (software event). One group read per sample, never one syscall per counter.

Newer DPDK releases carry an experimental `lib/pmu` (`rte_pmu_read`) built on the same perf mmap and `rdpmc` mechanism. Check the shipped DPDK version for x86 support and grouped reads before using it in place of our own code; ours is about 100 lines either way.

**Per-sample record**

Each sample is a batch of N iterations with start and end readings:

```c
typedef struct {
    uint64_t tsc;        /* reference cycles, end - start */
    uint64_t cycles;     /* core cycles, end - start; 0 without PMU */
    uint64_t instr;      /* optional */
    uint64_t iters;      /* N for this batch */
    uint32_t ctx_sw;     /* context switches inside the sample */
    uint32_t flags;      /* FLW_REJ_RATIO | FLW_REJ_CTX | FLW_REJ_MIGRATE ... */
    uint32_t impl_id;    /* which implementation ran */
    uint32_t epoch_id;   /* layout epoch, see Epochs */
    uint32_t batch_id;   /* batch index inside the epoch */
    uint16_t cpu_begin;  /* sched_getcpu() before and after */
    uint16_t cpu_end;
} flw_sample;
```

**Fencing**

Begin a window with `lfence; rdtsc`, end it with `rdtscp; lfence` (Intel's Paoloni whitepaper). Apply the same discipline to the `rdpmc` reads, and read the core-cycle counter immediately after TSC at the start and immediately before it at the end, so both windows nest the same work.

Use our own inline `asm` for the window. `rte_rdtsc()` is unfenced, and `rte_rdtsc_precise()` issues a full memory barrier, which is not the same ordering. `rte_get_tsc_hz()` is fine for unit conversion.

**Timer overhead**

Measure the cost of an empty window at startup, per counter, and again at the start of every epoch. Report it. With batches of 1 ms or more it is below 0.01% of a batch and is not subtracted. Subtract it only when batches are short (N close to 1, as in the later cold-cache modes), and record that in the config.

The call, loop and glue cost is a different quantity, handled by calibration (see Calibration and reference snippets).

**Sample filtering**

- `ratio = cycles / tsc`. A ratio more than 2% from the median of its epoch's samples marks the sample `ratio` (flag bit 1). With `exclude_kernel=1`, interrupt time counts in TSC but not in core cycles, so a drop means either a frequency change or an interrupt. Both are contamination. Skipped in TSC-only mode.
- Any context switch during the sample marks it `ctx` (bit 2). Counted with `getrusage(RUSAGE_THREAD)` (voluntary plus involuntary switches), read just outside the timed window. Not a software event in the perf group: a software event has no `rdpmc` index, so it would push every group read to `read()`, and `getrusage` needs no PMU access.
- The worker lcore is pinned by EAL, so migrations should not happen. Compare `sched_getcpu()` before and after anyway (outside the window), and mark `migrate` (bit 4) if they differ.
- Keep rejected samples in the JSONL with their flags and `ctx_sw` count. Results use kept samples only (all samples, with a warning, if none is kept). The rejection rate is a result in its own right: the `bench` record has `kept` and `rejected_ratio`, `rejected_ctx`, `rejected_migrate` (a sample can carry several), the console line shows `(x of y samples rejected)`, and more than 10% rejected warns that something on the machine disturbs the measurement.
- What filtering cannot see: a busy SMT sibling slows the thread without moving the ratio or switching it out (see the step 4 findings). That is for the environment checks (step 8).

## Environment control

The runner controls what it can, and reports what it cannot. Every finding goes into the env record so a result can be judged later.

**Control**

- **Pinning through EAL.** EAL pins every lcore thread. By default the benchmark runs on the first worker lcore from `-l` (with `-l 2,3`, the main lcore is 2 and the benchmark runs on 3). The main lcore sleeps in `rte_eal_wait_lcore`. With only one lcore, the runner uses the main lcore and warns.
- **Memory through DPDK.** Corpus and state come from `rte_malloc_socket` on the worker's socket (`rte_lcore_to_socket_id`). The runner is agnostic about page size: hugepages or `--no-huge` is chosen in the EAL arguments, and the code is the same either way. It touches every corpus page before the first sample (a no-op cost on hugepages) and records the page size it got.
- Optionally `SCHED_FIFO` on the worker (needs `CAP_SYS_NICE`).
- Run a warm-up at the start of every epoch (a fixed number of batches, discarded) so caches, branch predictors and the frequency governor settle.

**Profiles**

| | `--profile=dev` (default) | `--profile=ci` |
| --- | --- | --- |
| Environment problems | warn, keep running | fail (exit 1) |
| Main question | is my change faster on this machine? | did the build regress against the baseline? |
| Privileges | none assumed; PMU and FIFO optional, `--no-huge` in the EAL arguments if needed | the runner is tuned once: isolated core, `performance` governor, turbo off, idle SMT sibling, PMU readable |
| Baseline | the developer's own earlier run on the same machine | JSONL committed to the repo, taken on that runner |

**Checks** (`bm_check.c`, on the worker lcore)

Every finding is recorded in the env record. A *problem* warns under `dev` and also fails the run (exit 1) under `ci`; the run always finishes and writes its file.

| Check | Where | Problem when |
| --- | --- | --- |
| Governor | `/sys/devices/system/cpu/cpuN/cpufreq/scaling_governor` | readable and not `performance` |
| Turbo | `intel_pstate/no_turbo`, else `cpufreq/boost` (amd-pstate, acpi-cpufreq); `energy_performance_preference` recorded | on |
| SMT sibling | the other cpu in `topology/thread_siblings_list`; its busy share (all `/proc/stat` time but idle and iowait) from the start of calibration to the end of the last benchmark | busy more than 5% of the run. Checked at the end, because the load that matters is the load during the run. Sample filtering cannot see it: a busy sibling slows the thread without moving the cycles/TSC ratio (measured: 35% slower, few rejections). An offline sibling, or SMT off, is not listed and passes |
| Isolation | `/sys/devices/system/cpu/isolated` and `/sys/devices/system/cpu/nohz_full` | the worker cpu is in neither |
| Core type | `/sys/bus/event_source/devices/cpu_core/cpus`, `cpu_atom/cpus` | the worker is an E-core, or the type is unknown on a hybrid CPU |
| Invariant TSC | `cpuid` `0x80000007` | missing |
| PMU access | `perf_event_paranoid`, `cap_user_rdpmc` | no PMU (TSC-only mode, also when forced by `--pmu=none`). `rdpmc` unavailable only warns |
| Page size | `rte_mem_virt2memseg_list()` on the calibration corpus, its `page_sz` | never; recorded, because runs with different page sizes are not comparable |
| Build | `BM_BUILDTYPE`, compiler macros | `plain` or `debug`; built without optimization; a sanitizer (`__SANITIZE_ADDRESS__`, `__SANITIZE_THREAD__`) |

Calibration's own findings (slope, ratio, timer, confirmation) are separate: only the slope test fails under `ci`.

**Recorded in the env record**

CPU model and microcode, worker core, socket and core type, governor, turbo, energy-performance preference, SMT state, the sibling cpu and its busy share, isolation and `nohz_full`, kernel version, DPDK version (`rte_version()`), EAL arguments, page size, PMU mode, build type, compiler version, the project's `c_args`, whether control-flow protection (`-fcf-protection`, `__CET__`) is on, and `git describe --dirty`. Retpolines leave no compiler macro; they show only if they are in `c_args`.

## Corpus and input generation

Inputs are produced once, before timing, into a flat array. The timed loop only indexes it. This keeps generation cost out of the result and gives the branch predictor no tiny input to memorize.

**Corpus interface**

```c
typedef struct flw_corpus {
    void   *items;        /* contiguous array of item_size elements */
    size_t  item_size;
    size_t  count;        /* power of two, so index wrap is a mask */
    size_t  bytes_total;  /* working set, reported */
} flw_corpus;

typedef int (*flw_gen_fn)(flw_corpus *c, uint64_t seed, size_t count, void *arg);
```

The benchmark supplies a `flw_gen_fn`. The runner calls it with a fixed seed (reported), so a run reproduces. A count that is a power of two lets the loop use `i & mask` instead of a modulo. The runner owns the allocation (`rte_malloc_socket`), so it can place the corpus at a new offset every epoch.

**Rules for the corpus**

- Size it to the question. A corpus that fits in L1 measures the hot path. One larger than LLC measures memory behaviour. Hugepages remove most TLB misses, which is realistic for DPDK code but must be in the report.
- Make the data distribution match production: branch outcomes, string lengths and key skew change the numbers more than most code changes.
- Keep corpus items independent of the implementation. All implementations read the same corpus bytes.

**Later: cache modes and parametrized runs**

- Cache modes: `hot` (default, nothing before a batch), `cold-lines` (`clflush` over the batch's lines, then `mfence`), `cold-thrash` (read a buffer larger than LLC). Cold modes use small batches, so timer overhead is subtracted there.
- Parametrized runs like Go sub-benchmarks: `Name/param=value`. A working-set sweep from 4 KiB to 256 MiB in powers of two shows the cache steps.

## Epochs

An epoch is the unit of measurement inside one process. EAL init and teardown are too slow to repeat per sample, and a new process adds little: ASLR moves memory by whole pages, so the alignment of code within a page is the same in every process of a build. What a restart does vary (where heap and stack land, frequency history) an epoch reproduces in-process.

**Each epoch**

1. Allocates the corpus anew (page-aligned, with 17 pages of slack) and places it at a random offset: a random page out of 16 plus a random cache line inside the page. Calls the generator again with the run's seed, so the bytes are the same and only the address changes.
2. Calls the implementation's `setup` again on that corpus (and `teardown` at the end), so its state is allocated fresh. The runner does not control where `setup` allocates; a fresh allocation is the variation it gets.
3. Re-measures timer overhead (1000 empty windows).
4. Shifts the worker's stack down by a random cache-line multiple up to 4 KiB with `alloca`, then runs the warm-up batches (5) and collects K batches (default 30).
5. Frees the corpus and flags the epoch's samples by ratio. The epoch record (offsets, timer overhead) is written before its samples.

E epochs per implementation (default 10). The offsets come from a generator seeded by `--seed`, so a run reproduces its layouts. The pointer sweep (once per run, by the first implementation that needs it) and the batch size N come from the first epoch; N is the same in every epoch.

Consequence for benchmark authors: `gen`, `setup` and `teardown` run once per epoch, so they must be repeatable, and the generator must produce the same bytes from the same seed at any address.

**What epochs do not cover**

Code layout. An edit in one function can shift the alignment of an unrelated hot loop between two builds. Build benchmark code with `-falign-functions=64 -falign-loops=32` to reduce this, and treat small changes in untouched benchmarks as layout noise. The effect can be large and need not come from the hot loop itself: see the item on the runner's compiled code under Open items.

## Throughput mode

Throughput mode runs independent iterations back to back and reports cycles per operation with the pipeline kept full. It answers "how many of these can the core retire per unit time".

**Timed loop**

```c
for (size_t b = 0; b < nbatches; b++) {
    bm_mark t0 = bm_begin();          /* lfence; rdtsc + rdpmc */
    for (size_t i = 0; i < n; i++) {
        const void *in = bm_item(c, base + i);   /* (base + i) & mask */
        flw_use(fn(in, state));                    /* sink, see below */
    }
    bm_mark t1 = bm_end();            /* rdtscp; lfence + rdpmc */
    bm_record(&out, &t0, &t1, n);
    base += n;
}
```

**Keeping the work real**

- `flw_use(x)` is `asm volatile("" : : "g"(x) : "memory")`. For a value that must stay in a register use `"+r"(x)`. Together with a `flw_clobber()` memory barrier, this stops dead-code elimination and hoisting.
- Each iteration reads a different corpus item, so the compiler cannot prove two iterations equal.
- The snippet is called through the `flw_fn` pointer and lives in its own translation unit. LTO stays off for benchmark executables so the boundary is real.

**Batch sizing (like Go's `b.N`)**

1. Start at N = 1 and double N until one batch takes at least the target duration (default 1 ms).
2. Refine once: N = N \* target / measured, rounded up to a multiple of the unroll factor.
3. Fix N for that implementation for the rest of the process.
4. Check the clock only between batches. Never read it per iteration.

**Harness cost**

Reported, not subtracted by default. The out-of-order core overlaps the call, index, load and sink with the snippet, so their cost is not additive in throughput mode. The calibration gives a throughput floor (see Calibration); a result near it is marked `harness-bound`. The runner decides this once per implementation: the median per call over all its samples, in the calibration unit, lies within the fit tolerance of the floor (3% plus 0.5 cycle) or below it. The verdict goes into the `bench` record as `harness_bound`. The floor is per call; `ops_per_call` (see Chained mode) lifts a cheap snippet above it the same way. `--subtract=all` subtracts it anyway, and the output records that.

**Unrolling**

A harness loop unrolled by U amortizes the loop overhead and changes what the scheduler sees. Fix U as a build-time constant (start with 1, offer 4 and 8 via `#pragma GCC unroll`), and record it. Never compare numbers taken at different U.

**Reported metrics**

`ns/op`, `cycles/op` (core), `tsc/op`, and when the PMU group is available `instr/op` and IPC. A batch's value is its mean over N, which is one sample.

## Chained mode

In chained mode every iteration's output becomes the next iteration's input, so the CPU cannot overlap iterations. The result is latency per operation, not throughput.

**The glue problem**

Output type rarely equals input type, and an output is often not a valid input (an index needs masking, a pointer must stay in range). The runner therefore never turns an output into the next input. It keeps feeding corpus items and only makes each call wait for the previous one (the fake dependency below). Two shapes, one field each in `flw_impl`:

- **Pointer (`fn`):** the snippet is a `flw_fn`, `uint64_t fn(const void *in, void *state)`, the same function as in throughput mode. The runner passes it the corpus item's address made to depend on the previous output (the **pointer dependency**, `flw_chain_dep` on the address), so the loop is `out = fn(item | (out & zero), state)`. Every implementation with `fn` can run chained without extra code. The glue is the same for all of them, so it is calibrated once per run (see Pointer calibration below). The snippet's own loads from the item are on the chain and count in its cost. Its result must depend on what it read, or the next call does not wait (see the memcpy pitfall in the README).
- **Word-in/word-out (`chain`):** the snippet is a `flw_chain_fn`, `uint64_t chain(uint64_t in, void *state)`. The input is the first 8 bytes of a corpus item, loaded by the runner off the chain, so a kernel on a number is measured without a load (4 `imul` read 12.3 cycles this way, 17.1 as `fn`). The common case for hash finalizers and arithmetic kernels. The benchmark needs `item_size >= 8` (checked; a smaller item is a definition error).

Rules: in throughput mode only `fn` runs. In chained mode `chain` runs when it is set, else `fn`. An implementation without the function for a mode is a definition error in the benchmark's own mode (exit 99) and skipped with a warning under `--mode`.

**Inputs picked by the previous output.** A table walk, or a parser whose next token starts where the last ended, needs the output to choose the next input. The runner does not do that (see the decision log); the walk runs inside one call instead: a `chain` that takes the corpus word as its starting point, walks R steps through a table in `state` (`setup` receives the corpus, so the table can be the corpus itself) and declares `ops_per_call = R`. Each call starts from a fresh corpus word, and the step to the next input counts in the result. Nothing checks such a walk for collapse (below); that is the benchmark's job.

**Fake dependency**

To keep the corpus variety but still serialize iterations, fold a zero the compiler cannot see into the next input:

```c
static inline uint64_t flw_zero(void) {
    uint64_t z;
    asm volatile("xor %0, %0" : "=r"(z));   /* opaque zero, no input dependency */
    return z;
}
/* in the loop */
in = items[(base + i) & mask];
in = flw_chain_dep(in, out);   /* in | (out & zero): data unchanged, but depends on out */
out = fn(in, state);
```

`flw_chain_dep` takes the zero from an `asm` that yields 0 the compiler does not know, then computes `in | (out & zero)`. That adds a dependent AND and OR to the chain (about 2 cycles). The pointer dependency is the same trick on the item's address.

**Glue cost**

In chained mode latencies along the chain add up, so subtraction is valid. The calibration intercept `a` (call, loop and fake dependency together, see Calibration) is subtracted by default. Samples stay raw; the `bench` record carries the `a` and `floor` that apply and whether `a` is meant to be subtracted (`subtract`, from `--subtract`). A raw per-call median below the chained floor by more than the fit tolerance (3% plus 0.5 cycle) is marked `harness-bound`.

**Pointer calibration.** The startup sweep runs the word-in/word-out loop, whose glue is the AND and OR of `flw_chain_dep` plus the load of the item's first word, off the chain. The pointer loop has its own loop body, so it gets its own sweep: `ref/ptr-nop` and `ref/ptr-imul-chain-k` (k = 1 to 16) run as `fn` on the address the pointer dependency gives, fitted like the startup sweep. The references use the pointer's value and never read through it, so `a` holds the glue only and the snippet's own loads stay in its result. The glue never depends on the implementation, so the sweep runs once per run, in the first epoch of the first implementation that needs it (a run with none skips it, about 0.3 s). Any corpus does. It writes a `calibration` record with `mode: "pointer"`; a slope outside 3 ± 3% warns, and it is confirmed like the startup sweeps. On Kaby Lake: `a` about 2.2 cycles, floor about 8 at k = 2 (word in/out: 2.1 and 8.3), slope 3.07.

**The floor is per call and does not amortize**

Each iteration is bound by the slower of two limits: the latency along the data chain (`a + L`, with L the snippet's latency) and the rate at which the harness can issue one iteration at all (indirect call, return, loop branch, index and load; these are off the data chain but still pass through the front end and the ports). Both are per iteration, so a larger N does not help: N amortizes only per-batch costs such as the timer window (about 176 cycles per batch, under 0.001 cycles per iteration at 1 ms batches). Measured on Kaby Lake: chained `ref/nop` 6 cycles, `ref/imul-chain-1` 7 against 5 expected, and the first point on the line at k = 2 (8 cycles). A snippet with less latency than about `floor − a` reads as the floor, and subtracting `a` from it gives a wrong number; hence `harness-bound`, never corrected. Unrolling the harness loop (U) removes loop branches but not the call and return, so it lowers the floor only a little.

**Several operations per call (`ops_per_call`)**

To measure a snippet below the floor, do more work per call: the implementation applies its operation R times inside one call and declares `.ops_per_call = R` in its `flw_impl`. The call and return stay off the data chain, so once R ops of latency exceed the issue cost the harness hides behind them. This is the same mechanism as the k sweep in calibration.

- R is declared, never divided out by the user. The runner cannot discover it (the snippet is opaque), and the harness corrections are per call: the raw per-call value is `a + R·L`, the per-op result is `(per_call − a) / R`, and the floor check is `per_call < floor`. A user who divides by R first and lets the tools subtract `a` gets `per_call / R − a`, which over-corrects by a factor of R, and a floor check on a per-op value flags healthy results.
- In chained mode the R operations inside a call must form a chain themselves (each output feeds the next). Independent operations inside a call measure throughput, and the runner cannot detect it. This is the benchmark's contract, like keeping corpus items independent of the implementation.
- The same field applies in throughput mode, where the throughput floor (about 5 cycles per call on Kaby Lake) is also per call: the per-op result is `per_call / R`.
- R belongs to the implementation, not the benchmark: `table` may do one op per call and `pclmul` eight. Normalized per-op values remain comparable between implementations.

**Collapse**

A chain collapses when its inputs stop varying: a fixed point or a short cycle (`x = x / 2` reaches 0), after which branches are perfectly predictable and the numbers are fake. Neither shape can collapse: the fake dependency leaves the corpus item unchanged, so the snippet always sees the corpus. No check runs. A walk inside one call (above) can collapse, and the runner cannot see it; keep its steps varied, for example by starting each call from the corpus word and mixing it in.

**What chained mode means**

The numbers are latency, valid for code that sits in a dependency chain. They do not transfer to code running inside a busy pipeline. Put `chained` in the benchmark name (`BenchmarkFoo/chained`), and never mix chained and throughput samples in one comparison.

## Calibration and reference snippets

A snippet with a known cost does two jobs: it checks that the counters read sensible numbers, and it measures what the harness itself costs.

**Built-in references**

| Name | Body | Expected core cycles/op |
| --- | --- | --- |
| `ref/imul-chain-k` | k dependent `imul r64, r64` | 3k (latency 3 on modern Intel and AMD cores) |
| `ref/add-chain-k` | k dependent `add` | k |
| `ref/imul-indep-s` | s independent `imul` from the same input, into one register (`imul $imm, in, out`, so s is not limited by register count) | s per op at 1 `imul` per cycle, reached only at large s: the per-op harness uops also compete for the `imul` port (measured 1.1 to 1.25 per `imul` at s = 16, 32 on Kaby Lake) |
| `ref/nop` | returns its input | harness floor |

Write them in inline `asm` so the compiler cannot fold the chain. They are called through the same function pointer, loop and glue as user snippets. The expected latencies are the same on every modern x86 core; `--ref-latency=imul:N` overrides them.

**Calibration sweep (at startup, per mode)**

- **Chained:** run `ref/imul-chain-k` for k = 1, 2, 4, 8, 16 and fit `cycles/op = a + 3k` on the points where the chain dominates. The intercept `a` is the additive harness latency: call, loop and glue. The **floor** is the cost at the smallest k that lies on the line; below it the harness hides the chain, and subtraction means nothing.
- **Pointer** (not at startup): the same chained sweep through the pointer dependency, once per run, when a chained implementation without `chain` first runs; see Chained mode.
- **Throughput:** run `ref/imul-indep-s` for s = 1 to 32 in powers of two and `ref/nop`. The floor is the cost per op of `ref/nop`, which small s cannot go below. The fitted slope is recorded but not tested (see the table).
- **Fit.** Start from the two largest k (or s) and extend the line downward while the next point stays within 3% (plus 0.5 cycle) of it; the lowest point on the line is the floor. `a` is the intercept with the expected slope (`--ref-latency`), so it does not absorb slope noise; the free slope is what the self-test checks. `ref/add-chain-k` is fitted the same way and recorded as a cross-check, not tested.
- Each point: batch size N by the sizing rule (once), then 3 rounds of 5 warm-up batches and 15 kept batches, median per op per round. A round goes through the whole sweep before the next starts, so the rounds of one point are spread over the whole calibration (about a second) and one burst of interference reaches at most one of them. Interference only adds cycles, so each point keeps its **lowest** round; the difference between its highest and lowest round is its `spread`. The calibration corpus is 1024 words (8 KiB, in L1) from `rte_malloc_socket` on the worker's socket.
- The sweep writes a calibration record: `a`, floors, fitted slope and residuals.

**Self-test**

1. The fitted chained slope must be 3 ± 3%. Out of tolerance means the counters, pinning or frequency control are wrong. Print which. The `ci` profile fails; `dev` warns.
2. The TSC/core-cycles ratio should be steady across calibration batches (small MAD). A wide spread means frequency is moving.
3. Timer overhead must be stable across repeats (5 repeats of 1000 empty windows, spread at most 5% or 2 units). It runs after the sweeps, once the frequency has settled, and is judged in core cycles when there is a PMU: the TSC cost of a window moves with the core frequency. Its median goes into the calibration record.
4. **Confirmation.** `a` and the floors are fitted twice: on each point's fastest round (used) and on its second-fastest (`gap` above the fastest). When the two fits agree, calibration is confirmed. When `a` moves by more than 0.5 cycle, or a floor by more than the fit tolerance, the run warns with what that risks: how far corrected results may be off (`a` versus the alternative), and the range of per-call values that may get the wrong harness-bound verdict (between the two floors). Each result is then judged against both: its console line gets `+- x (calibration uncertain)` when its corrected value moves, and `harness-bound uncertain (floor F or F')` when the two floors give different verdicts; the `bench` record carries `a_alt`, `floor_alt` and `harness_bound_uncertain`. The throughput `a` counts only under `--subtract=all`. The pointer sweep is confirmed the same way (its record carries its own `a_uncertain` and `floor_uncertain`; the `selftest` record's `confirmed` covers the startup sweeps only). Recorded as `a_alt`, `floor_alt`, `a_uncertain` and `floor_uncertain` in the `calibration` records and as `confirmed` in the `selftest` record.

Only the slope test changes the exit code (1 in `ci`). The ratio, timer and confirmation tests warn. In TSC-only mode the slope and ratio tests are skipped and calibration runs in TSC ticks.

When the slope fails, the warning names the likely cause from the evidence at hand: the worker migrated (pinning), the ratio moved (frequency), or neither, which points at a busy SMT sibling or other load on the core. Verified on a laptop (i5-7200U, worker on cpu 3 whose sibling cpu 1 carries desktop load, `powersave`): 4 of 5 runs passed (slopes 2.92 to 3.08), and the failing run was slow across every point with a steady ratio.

## Statistics

The runner stores raw samples. All statistics live in `tools/bm_stats.py`, used by the other wrappers. Python standard library only.

**Summary per benchmark**

- **Unit is the epoch.** Per epoch: the median of kept batches' cycles/op, with the MAD as a within-epoch diagnostic.
- **Headline:** the median of epoch medians, with a bootstrap CI over epochs (2000 resamples, seeded, 2.5 and 97.5 percentiles).
- **Spread check:** compare between-epoch to within-epoch spread. A between-epoch spread several times larger means the benchmark is layout-sensitive, and the report says so.
- **Counts:** kept samples, rejected samples by reason (`ratio`, `ctx`, `migrate`).
- Raw and harness-corrected values side by side. Corrected is the headline only in chained mode.
- **Per call first, per op last.** Statistics and the floor check run on per-call values; the conversion to per op comes after: `(per_call − a) / R` in chained mode, `per_call / R` in throughput mode (and for raw values in both), with R = `ops_per_call` from the `bench` record.

**Outlier policy**

Filtering is done by hardware evidence, not by statistics. Flag-based rejection removes samples that are known to be contaminated. No statistical trimming.

**Comparing two runs (base and new)**

1. Take the epoch medians of the same benchmark from each file.
2. Bootstrap: resample epochs independently on each side, compute the ratio of medians of epoch medians, on a log scale.
3. Report the median ratio and its 95% CI.
4. Call a change significant only when the CI excludes 1.0 and the effect is at least the minimum size (default 1%).

The verdict uses core cycles/op (TSC ticks when either run has no PMU), **raw**: the harness cost `a` is measured anew in every run (runs are stateless), so subtracting it would add calibration noise to the comparison. For compute-bound code core cycles do not depend on CPU frequency, so drift between the two sessions mostly cancels. Memory-bound code is still affected by session drift; the report shows `ns/op` beside it.

**Implemented** (`tools/bm_stats.py`): epoch medians of kept samples (all samples if none is kept); headline the median of epoch medians with a percentile bootstrap CI (2000 resamples, seed 1); the comparison resamples epoch medians of each side independently and takes percentiles of the log ratio; *layout-sensitive* when the MAD of epoch medians is more than 3 times the median within-epoch MAD.

**Stopping**

Fixed E × K. Planned: an optional time budget (`--time`) that stops early and says so in the records.

**Testing**

`bm_stats.py` takes plain lists of numbers. Test it on synthetic data: known distributions, known shifts, A/A pairs. `tools/test_bm_stats.py` does, as a Meson `test()` in suite `tools` (plain `meson test` runs it): headline and CI, rejected samples left out, per op last, layout sensitivity, a planted 5% shift found, a 0.5% shift below the 1% minimum effect not called significant, and 100 A/A pairs (the CI contained 1.0 in 97).

## Comparing revisions with Git

Build one revision, run it, keep its JSONL; check out the other, rebuild, run again, compare.

```
git checkout main     && meson compile -C build && meson test -C build --benchmark
cp build/benchmarks/crc32.jsonl /tmp/base.jsonl
git checkout feature  && meson compile -C build && meson test -C build --benchmark
tools/bm-compare /tmp/base.jsonl build/benchmarks/crc32.jsonl
```

- **Revision in the data.** Meson's `vcs_tag()` captures `git describe --dirty` at build time and the runner writes it into the env record. `bm-compare` prints which commit is which, warns on a dirty build, and refuses two reports from the same commit unless `--allow-same` is given (that is the A/A check).
- **Matching environments.** `bm-compare` refuses to compare reports that differ in CPU model, worker core type, build type, compiler flags, profile, mode, unroll, `ops_per_call`, page size, PMU mode or DPDK version. Like U, R changes how much harness cost hides behind the snippet, so results at different R differ by more than the normalization. In `dev` it exits 77 (skip); in `ci` it exits 1.
- **Optional:** `git worktree` with two build directories saves rebuilds and lets you alternate base and new runs if session drift turns out to matter.

## Output format

The runner writes one JSONL file per run with jansson (`json_dumpf(..., JSON_COMPACT)` per line). It is the only output and the source of truth; everything else is derived from it by a wrapper.

**Record types, in order**

```json
{"type":"env","schema":1,"git":"v1.4-12-gabc123","cpu":"...","core":3,"socket":0,"core_type":"core","pmu":true,"page_size":2097152,"tsc_hz":0,"dpdk":"24.11.1","profile":"dev","governor":"performance","turbo":false,"epp":"performance","smt_active":true,"sibling":1,"sibling_busy":0.01,"isolated":true,"nohz_full":true,"microcode":"0xf6","compiler":"14.3.1","c_args":"","cf_protection":false,"warnings":[]}
{"type":"timer","n":1000,"tsc":250,"tsc_mad":2,"cycles":175,"cycles_mad":1,"instr":45,"instr_mad":0}
{"type":"calibration","mode":"chained","unit":"cycles","a":2.01,"floor":8.0,"floor_k":2,"slope":3.0,"expected_slope":3,"fit_points":4,"unroll":1,"batch_ms":1,"rounds":3,"a_alt":2.03,"floor_alt":8.0,"a_uncertain":false,"floor_uncertain":false,"timer_tsc":250,"timer_cycles":176,"points":[{"ref":"ref/imul-chain-1","k":1,"iters":440825,"per_op":7.0,"spread":0.1,"gap":0.0,"tsc":6.14,"instr":15,"resid":2.0,"on_line":false}],"add_a":2.12,"add_slope":1.0,"add_expected_slope":1}
{"type":"calibration","mode":"throughput","unit":"cycles","floor":5.0,"floor_k":0,"slope":1.13,"expected_slope":1,"points":[]}
{"type":"selftest","unit":"cycles","slope":"pass","slope_value":3.0,"slope_tol":0.03,"ratio":"pass","ratio_median":1.14,"ratio_mad":0.00002,"timer":"pass","timer_tsc":250,"timer_spread":5,"confirmed":"pass"}
{"type":"bench","name":"Crc32/throughput/table","mode":"throughput","impl":0,"iters":4096,"ops_per_call":1,"unroll":1,"epochs":10,"samples":30,"warmup":5,"seed":1,"corpus_bytes":1048576,"calibration":"startup","a":2.79,"floor":5.0,"a_alt":2.81,"floor_alt":5.0,"subtract":false,"harness_bound":false,"harness_bound_uncertain":false,"kept":300,"rejected_ratio":0,"rejected_ctx":0,"rejected_migrate":0}
{"type":"epoch","bench":"Crc32/throughput/table","epoch":0,"corpus_offset":47040,"stack_offset":2112,"timer_tsc":154,"timer_cycles":176}
{"type":"sample","bench":"Crc32/throughput/table","epoch":0,"batch":0,"iters":4096,"tsc":0,"cycles":0,"instr":0,"ctx_sw":0,"flags":0}
```

The examples are abbreviated. The `env` record also has `lcore`, `on_main_lcore`, `kernel`, `build_type`, `seed`, `eal_args`, `invariant_tsc`, `tsc_hz_measured`, `pmu_mode`, `pmu_name`, `counters` and `perf_event_paranoid`. The throughput `calibration` record has the same fields as the chained one, without the `add_*` fields, and its `points` are `ref/nop` and `ref/imul-indep-s`. A third `calibration` record, `mode: "pointer"`, with the same fields as the throughput one and `points` `ref/ptr-nop` and `ref/ptr-imul-chain-k`, is written only when the pointer sweep ran.

Calibration values are per op in `unit`: `cycles` (core cycles) with a PMU, `tsc` in TSC-only mode. The chained `floor` is a raw per-op cost: a raw result below it is harness-bound. In the `selftest` record each test is `pass`, `fail` or `skipped`.

The `timer` record holds the startup timer overhead (median and MAD of empty windows, per counter; counters absent in TSC-only mode). `core_type` is `core` or `atom` on hybrid CPUs, `uniform` on others, and `unknown` on a hybrid CPU whose worker core is in neither PMU's list.

Each `bench` record is followed by its epochs: an `epoch` record (`corpus_offset` from the start of the allocation, `stack_offset` in bytes, the epoch's timer overhead `timer_tsc` and, with a PMU, `timer_cycles`), then that epoch's samples. `mode` is the mode it ran in (the benchmark's own, or the `--mode` override). `a` and `floor` are the calibration values that apply (named by `calibration`: `startup` for throughput and word in/out, `pointer` for the pointer sweep), and `subtract` says whether the tools subtract `a`. `iters` is the batch size N, fixed for the implementation; `corpus_bytes` is the working set as the generator left it (`bytes_total`); `warmup` counts the discarded batches. A `sample` record leaves out the counters that are not open (`cycles` and `instr` in TSC-only mode, `instr` when only cycles opened), like the `timer` record.

Samples stay raw and per call: `iters` counts calls, never operations. The `bench` record carries `ops_per_call`, and the wrappers convert to per-op values.

Rejected samples are included with their `flags`: 1 `ratio`, 2 `ctx`, 4 `migrate`, 0 kept. A record type the reader does not know is skipped, so the schema can grow.

**Wrappers (`tools/`)**

| Tool | Reads | Produces |
| --- | --- | --- |
| `bm-report` | one or more JSONL files | env header with every warning, then one line per benchmark: median, CI, raw and corrected, kept and rejected counts, harness-bound marks |
| `bm-go` | one JSONL file | Go-format lines for `benchstat` |
| `bm-compare` | base and new JSONL | ratio, CI and verdict per benchmark; nonzero exit on a regression beyond the threshold |

`bm-compare base new [--threshold=3] [--min-effect=1] [--allow-same] [--profile=dev|ci]`. A change is *significant* when its CI excludes 1.0 and it is at least `--min-effect` percent; a significant slowdown of at least `--threshold` percent is a `REGRESSION` (exit 1). Runs from the same commit are refused unless `--allow-same` (A/A); a `-dirty` commit warns. A missing baseline, or runs whose env or bench fields do not match, exits 77 under `dev` and 1 under `ci` (the profile comes from the new run unless `--profile` is given). Implementations present in only one file are listed, without a verdict.

**Go-format lines (from `bm-go`)**

```
cpu: AMD Ryzen 9 7950X
dpdk: 24.11.1
git: v1.4-12-gabc123
BenchmarkCrc32/throughput/table-1   4096   123.4 ns/op   371 cycles/op   3.10 IPC
BenchmarkCrc32/chained/table-1      4096   301.8 ns/op   905 cycles/op
```

- One line per kept sample (one batch). Rejected samples are left out.
- Values are raw per op (per call divided by `ops_per_call`): `ns/op` from TSC ticks and `tsc_hz`, `cycles/op`, and IPC. N is calls times `ops_per_call`.
- `benchstat` takes every line as an independent measurement, so its p-values are too small: batches within an epoch share a layout and are not independent. Use it to view and tabulate; take verdicts from `bm-compare`.
- The name suffix is always `-1`. In Go it means GOMAXPROCS; a core number there would make `benchstat` treat runs on different cores as different benchmarks. Core, CPU and the rest go into the `key: value` config lines, which `benchstat` understands.
- Mode and implementation are in the name, so `benchstat` never compares unlike things.

## API sketch in C

A benchmark declares data and implementations. The runner owns timing, scheduling and output. Registration uses a linker section, so a benchmark file only needs a static definition.

**Naming.** Everything in the public header uses snake case with the `flw_` prefix: functions, types and inline helpers (`flw_main`, `flw_impl`, `flw_use`). Macros and enum constants use `FLW_` (`FLW_REGISTER`, `FLW_CHAINED`). Internal functions in the runner sources are `static` and need no prefix. File, executable and tool names (`bm_core.c`, `bm_crc32`, `bm-compare`) are not API and keep their names.

```c
/* bench.h */
typedef enum { FLW_THROUGHPUT, FLW_CHAINED } flw_mode;

typedef uint64_t (*flw_fn)(const void *in, void *state);              /* throughput, chained on a pointer */
typedef uint64_t (*flw_chain_fn)(uint64_t in, void *state);           /* chained, word in/out */

typedef struct {
    const char  *name;                      /* "table", "pclmul", ... */
    flw_fn        fn;                        /* throughput, or chained on a pointer */
    flw_chain_fn  chain;                     /* chained word in/out, takes over from fn; may be NULL */
    void      *(*setup)(const flw_corpus *c, void *arg);   /* may be NULL */
    void       (*teardown)(void *state);                  /* may be NULL */
    unsigned     ops_per_call;              /* operations per call, R; 0 means 1 */
} flw_impl;

typedef struct {
    const char    *name;                    /* "Crc32" */
    flw_mode        mode;                    /* default, overridable by flag */
    flw_gen_fn      gen;                     /* corpus generator */
    void          *gen_arg;
    size_t         item_size;
    size_t         count;                   /* corpus items, power of two */
    const flw_impl *impls;                   /* same semantics, 1..N */
    size_t         nimpls;
} flw_bench;

#define FLW_REGISTER(b) \
    static const flw_bench *const flw_reg_##b \
        __attribute__((used, section("flw_benches"))) = &(b)

int flw_main(int argc, char **argv);   /* rte_eal_init, then the whole runner */
```

The runner walks `__start_flw_benches` to `__stop_flw_benches`. A benchmark executable is its benchmark sources plus the runner, so the linker keeps the section. If the runner lives in a static library, link it with the benchmark objects directly, and avoid relying on archive extraction for registered objects.

In version 1 the implementations of a benchmark run one after another in the same process, each through its full set of epochs. Comparing them is unpaired, like comparing two builds.

**Example**

```c
static const flw_impl crc_impls[] = {
    { .name = "table",  .fn = crc_table  },
    { .name = "pclmul", .fn = crc_pclmul },
};
static const flw_bench crc_bench = {
    .name = "Crc32", .mode = FLW_THROUGHPUT,
    .gen = gen_random_buffers, .item_size = 64, .count = 1 << 14,
    .impls = crc_impls, .nimpls = 2,
};
FLW_REGISTER(crc_bench);

int main(int argc, char **argv) { return flw_main(argc, argv); }
```

**Command line**

`bm_crc32 [EAL args] -- [bench args]`. `flw_main` passes everything before `--` to `rte_eal_init` and parses the rest.

| Flag | Meaning |
| --- | --- |
| `--bench=REGEX` | select benchmarks by name, like `go test -bench` |
| `--mode=throughput\|chained\|both` | override the default mode; an implementation without the functions for a requested mode is skipped with a warning |
| `--epochs=E`, `--samples=K` | epochs per benchmark and batches per epoch |
| `--batch-time=MS` | target batch duration for sizing N (default 1) |
| `--time=SEC` | optional time budget (planned, not implemented) |
| `--out=PATH` | JSONL output, `-` for stdout |
| `--seed=S` | seed for corpus and layout offsets |
| `--profile=dev\|ci` | environment policy |
| `--subtract=chained\|all\|none` | where harness cost is subtracted (default `chained`) |
| `--ref-latency=imul:N` | override a reference latency |
| `--pmu=auto\|read\|none` | how core cycles are read: `rdpmc` with `read()` fallback (default), `read()` only, or TSC-only. For testing the fallback paths; recorded as `pmu_mode` |
| `--list` | print benchmark names and exit |

## Meson integration

Each benchmark executable is registered with Meson's `benchmark()`, so `meson test --benchmark` runs the whole set and the existing build, options and CI flow are reused. Meson only runs the process and records its exit status and output. Results live in the JSONL files the runner writes.

**What Meson gives and does not give**

- `benchmark()` targets are skipped by plain `meson test` and run only with `--benchmark`.
- Benchmarks run one at a time, never in parallel, which is what a timing run needs.
- Meson's log (`meson-logs/testlog.txt`; Meson 1.11 uses it for `--benchmark` runs too, not `benchmarklog.txt`) keeps stdout and the exit code. It does not parse numbers.
- The default timeout is 30 s. Set an explicit `timeout:` per benchmark, because E epochs of K batches over several implementations exceed it.
- `--repeat` is not needed: one process collects all epochs. Each run truncates its JSONL file.

**Layout**

```
meson.build                                    # project; finds python3; subdir benchmarks, tools
benchmarks/
  meson.build                                  # library, one executable() + benchmark() + regress test() each
  README.md                                    # guide for benchmark authors
  bench.h                                      # public API
  bm_core.c/.h  bm_calib.c/.h                  # measurement core, no DPDK
  bm_run.c  bm_bench.c  bm_check.c             # runner: EAL, benchmarks and epochs, environment checks
  bm_env.c  bm_out.c  bm_internal.h            # runner: JSONL records, shared internals
  bm_version.h.in                              # git describe, via vcs_tag
  bench_crc32.c                                # one executable per bench_*.c
  baseline/                                    # CI baselines, JSONL (created when the first is taken)
tools/
  meson.build                                  # the bm_stats unit test
  bm_stats.py  bm-report  bm-go  bm-compare  test_bm_stats.py
GLOSSARY.md                                    # terms in plain words
```

**`benchmarks/meson.build`**

The framework is a static library and a dependency. Each benchmark is one `executable()` and one `benchmark()` call with every argument spelled out: sources, EAL arguments, output file, profile, seed, timeout. There are no global Meson options for benchmarks and no table of benchmarks to loop over; what runs is what the file says.

```meson
dpdk_dep    = dependency('libdpdk')
jansson_dep = dependency('jansson')

bm_version = vcs_tag(command: ['git', 'describe', '--dirty', '--always'],
  fallback: 'unknown',
  input: 'bm_version.h.in', output: 'bm_version.h')

# code alignment for the runner's timed loops and for every benchmark source
bm_align = ['-falign-functions=64', '-falign-loops=32']

bm_inc = include_directories('.')
bm_lib = static_library('bm',
  ['bm_core.c', 'bm_calib.c', 'bm_bench.c', 'bm_check.c', 'bm_run.c', 'bm_env.c',
   'bm_out.c', bm_version],
  include_directories: bm_inc,
  dependencies: [dpdk_dep, jansson_dep],
  c_args: ['-DBM_BUILDTYPE="@0@"'.format(get_option('buildtype')),
           '-DBM_CARGS="@0@"'.format(' '.join(get_option('c_args')))] + bm_align,
  override_options: ['b_lto=false'])

# the framework: link it into a benchmark executable, which also gets the
# alignment flags; the executable itself must still set b_lto=false
bm_dep = declare_dependency(link_with: bm_lib, include_directories: bm_inc,
  compile_args: bm_align,
  dependencies: [dpdk_dep, jansson_dep])

# Benchmarks: one executable() and one benchmark() each, all arguments
# spelled out. EAL arguments go before '--', runner arguments after it.

bm_crc32 = executable('bm_crc32', 'bench_crc32.c',
  dependencies: [bm_dep, project_deps],
  override_options: ['b_lto=false'],
  build_by_default: false)

benchmark('crc32', bm_crc32,
  args: ['-l', '2,3', '--no-huge', '--no-pci', '--no-telemetry',
         '--',
         '--out=' + meson.current_build_dir() / 'crc32.jsonl',
         '--profile=dev',
         '--seed=1'],
  suite: 'bench',
  timeout: 1800,
  protocol: 'exitcode')
```

`BM_BUILDTYPE` and `BM_CARGS` record the build type and the project's `c_args` in the env record. The regression `test()` for each benchmark follows its `benchmark()` (see Regression check in CI). `bm_dep` carries the alignment flags, so a benchmark gets them by depending on it. `b_lto` is a per-target option that a dependency cannot set, so every benchmark `executable()` states `override_options: ['b_lto=false']` itself. `meson test --benchmark` builds the executables it runs, so `build_by_default: false` keeps them out of a plain `meson compile`.

The top-level `meson.build` adds `subdir('benchmarks')`, usually guarded by an option or by `get_option('buildtype')`.

Hugepages are an EAL argument, not code: a benchmark meant for machines without them has `--no-huge` in its `args`, and nothing else changes.

**Overriding runner arguments at run time.** `meson test --test-args='...'` appends to every selected benchmark's arguments. They land after `--`, and the last occurrence of a runner option wins, so `--test-args='--profile=ci'` switches a run to the `ci` profile without editing the file. Meson keeps only the last `--test-args`: put several arguments in one string. EAL arguments cannot be changed this way, because they sit before `--`; a machine that needs other lcores needs its own `benchmark()` entry.

**Build flags**

- Benchmarks must be built optimized. Use `buildtype=release` or `debugoptimized`; the runner records the build type and warns on `plain` or `debug`.
- `b_lto=false` for the benchmark executables keeps the function-pointer boundary real.
- `-falign-functions=64 -falign-loops=32` on benchmark sources, so unrelated edits move hot code less.
- Keep the benchmark and runner sources free of project-wide sanitizer flags (`b_sanitize`), which distort timings.

**Exit codes**

| Exit code | Meson shows | When |
| --- | --- | --- |
| 0 | OK | run finished, file written |
| 1 | FAIL | self-test out of tolerance, environment check failed in `ci`, I/O error, regression found by `bm-compare` |
| 77 | SKIP | only from `bm-compare` in `dev`: no baseline, or environments do not match |
| 99 | ERROR | bad arguments, EAL init failure, or another setup failure that is a harness bug |

**Running**

```
meson setup build -Dbuildtype=release
meson compile -C build
meson test -C build --benchmark                 # all benchmarks
meson test -C build --benchmark crc32           # one
meson test -C build --benchmark -v              # live output
tools/bm-report build/benchmarks/crc32.jsonl
tools/bm-go build/benchmarks/crc32.jsonl > crc32.bench.txt
```

**Regression check in CI**

`bm-compare` is an ordinary `test()` in its own suite, one per benchmark next to its `benchmark()`, run after the benchmarks, in a second command, since tests and benchmarks are separate invocations. `python3` is found once in the top-level `meson.build`.

```meson
bm_compare = files('../tools/bm-compare')

test('regress-crc32', python3,
  args: [bm_compare,
         meson.current_source_dir() / 'baseline' / 'crc32.jsonl',
         meson.current_build_dir() / 'crc32.jsonl',
         '--threshold=3'],
  env: ['PYTHONDONTWRITEBYTECODE=1'],
  suite: 'bench-regression',
  is_parallel: false)
```

The baseline is a plain path, not `files()`, so the build configures before any baseline exists. A plain `meson test` also runs the check; without a baseline or results it exits 77 (SKIP) under `dev`.

```
meson test -C build --benchmark --test-args='--profile=ci'
meson test -C build --suite bench-regression --test-args='--profile=ci'
```

Under `--profile=ci` a missing baseline or results file fails (exit 1), so a misconfigured runner cannot pass.

Baselines are JSONL files committed to the repo as `benchmarks/baseline/<name>.jsonl`, taken on the CI runner with `--profile=ci`: run the benchmarks there and copy `build/benchmarks/<name>.jsonl`. Refresh them deliberately, in their own commit. The baseline's commit differs from the one under test, so `--allow-same` is not needed in CI.

## Implementation order

Build the measurement core first and prove it on reference snippets, then add features. Each step has an acceptance check that must pass before the next one starts.

**Where to build and test.** The development host has no DPDK. Build and run everything, including acceptance checks, inside a Docker container from an image that has DPDK and jansson. Ask the user for the image name before the first run; never fall back to a native build. Pinning needs the worker core in the container's `--cpuset-cpus`.

**PMU inside Docker** (verified Nov 7, 2026, Docker 29, kernel 6.18, bare metal):

- No root is needed, inside the container or on the host. Run as the host user (`--user $(id -u):$(id -g)`) with `--cap-add=PERFMON`. `--privileged` is not needed.
- Docker's default seccomp profile blocks `perf_event_open` (EPERM) unless the container has `CAP_PERFMON` (or `CAP_SYS_ADMIN`); `--security-opt seccomp=unconfined` also lifts it. The capability only unlocks the syscall in the seccomp filter: the kernel then applies the host's `perf_event_paranoid` as usual.
- With a host `perf_event_paranoid` of 2 or less, an unprivileged user may count its own thread in user mode, which is all the runner opens (`exclude_kernel=1`, per thread). `rdpmc` from userspace then works (`cap_user_rdpmc=1`, host `/sys/bus/event_source/devices/cpu/rdpmc` at its default of 1).
- Hosts with `perf_event_paranoid` of 3 or 4 (Debian and Ubuntu defaults) need `sudo sysctl kernel.perf_event_paranoid=2` once, or root; otherwise the runner falls back to TSC-only mode.
- The TSC-only path can be tested without root: a container without `--cap-add=PERFMON` fails `perf_event_open` with EPERM, as paranoid 3 or 4 does.

1. **Skeleton and Meson wiring.** *(Status: done.)* EAL init, worker lcore launch, JSONL output through jansson, `vcs_tag`, one benchmark executable registered with `benchmark()`. *Accept:* the run shows as OK in Meson's log, and the JSONL file has an env record with the commit.
2. **Counters.** *(Status: done.)* Invariant TSC check, `rte_get_tsc_hz` cross-check, `rdpmc` core cycles with a `read()` fallback, hybrid PMU selection, TSC-only mode, fenced windows, timer overhead. *Accept:* `tsc_hz` agrees with `CLOCK_MONOTONIC_RAW` to 0.1%, timer overhead has a tight spread, and with `perf_event_paranoid=4` the run completes with a warning.
3. **Reference snippets and calibration.** *(Status: done; AMD, hybrid and the rdpmc fallback untested.)* `imul` and `add` chains, independent `imul` streams, `ref/nop`, the sweep and fit, the self-test. *Accept:* the chained slope reads 3 ± 3% on one Intel and one AMD machine, and `a` and the floors are recorded.
4. **Throughput mode.** *(Status: done; the 1% check open on a quiet machine.)* Corpus interface, timed loop, sink, batch sizing, floor marking. *Accept:* `ref/imul-indep-s` lands near its expected value, and epoch medians agree to within 1%.
5. **Chained mode.** *(Status: done; pointer dependency added Nov 8, adapters and the collapse check removed Nov 9, 2026.)* Word-in/word-out and pointer, fake dependency, intercept subtraction, `ops_per_call` in the `bench` record. *Accept:* `ref/imul-chain-k` through the full glue reads 3k ± 3% after subtracting `a`.
6. **Sample filtering.** *(Status: done; "almost none rejected" open on a tuned machine.)* Ratio, context switches, migrations. *Accept:* a busy neighbour on the worker core or a changed governor produces rejected samples, and a quiet run rejects almost none.
7. **Epochs.** *(Status: done; the A/A CI check open on a quiet machine.)* Re-allocation at random offsets, stack shift, per-epoch warm-up and epoch records. *Accept:* an A/A comparison (the same build run twice, `bm-compare --allow-same`) gives a CI that contains 1.0 in at least 95% of repeated pairs.
8. **Environment checks and profiles.** *(Status: done.)* The checks table, the env record, `dev` and `ci` policies. *Accept:* on a misconfigured machine `ci` exits 1 and `dev` warns and finishes.
9. **Wrappers.** *(Status: done.)* `bm_stats.py`, `bm-report`, `bm-go`, `bm-compare`. *Accept:* `benchstat` reads `bm-go` output, `bm_stats.py` passes tests on synthetic data with known shifts, and a planted 5% slowdown is flagged while an unchanged build passes.
10. **CI wiring.** *(Status: wired and tested locally; open on a CI runner.)* Baselines from the CI runner, the `bench-regression` suite. *Accept:* the suite runs on the runner and fails on the planted slowdown.
11. **Later.** Interleaving several implementations in randomized, paired rounds (see below); cache modes; parametrized sweeps; relative mode against a reference snippet; parallel mode with per-lcore runs.

**Later: interleaving several implementations**

When implementations of the same semantics share one process, run them in rounds: one batch per implementation per round, order shuffled per round with a seeded Fisher-Yates (or ABBA/BAAB for two). Use equal batch durations, keep per-implementation state separate and corpus bytes shared and read-only, and record `round_id`. Round r then gives a pair (A\_r, B\_r): compare the median of per-round log-ratios with a bootstrap CI over rounds, and drop a round from the paired analysis if any of its blocks was rejected. Drift then hits all implementations alike, which makes the comparison much tighter than unpaired runs.

## Pitfalls checklist

Check each before trusting a number.

- [ ] The build is optimized (`release` or `debugoptimized`), LTO is off and alignment flags are on for benchmark executables.
- [ ] The snippet's result is consumed by a sink, and a disassembly check shows the work is still in the loop.
- [ ] The corpus is generated outside the timed region, pre-faulted, and larger than a handful of items.
- [ ] The benchmark ran on a worker lcore, the governor is `performance`, and the SMT sibling is idle.
- [ ] Core cycles are read alongside TSC (or the report says TSC-only), and samples with a drifting ratio are rejected.
- [ ] Context switches are counted per sample, and the rejection rate is low.
- [ ] The calibration self-test passed, and results near the floor are marked `harness-bound`, not corrected.
- [ ] Harness cost is subtracted only in chained mode, unless `--subtract=all` is recorded.
- [ ] A snippet cheaper than the per-call floor declares `ops_per_call` (its R ops chained in chained mode) instead of dividing by R itself.
- [ ] Throughput and chained samples are never mixed in one comparison.
- [ ] In chained mode every snippet returns a value computed from what it read, and a walk inside one call keeps its steps varied.
- [ ] There are enough epochs (10 or more), and the between-epoch spread is not far above the within-epoch spread.
- [ ] Base and new runs come from different commits, neither dirty, with matching env records (CPU, page size, PMU mode, DPDK version, flags).
- [ ] The unroll factor, seed, profile and page size are in the env record.
- [ ] An A/A run (the same build twice) shows no difference.

## Decision log

Decisions taken during the design review (Nov 7, 2026), with the reason, so they are not reopened by accident.

| Decision | Why | Rejected alternative |
| --- | --- | --- |
| Statistics in Python wrappers; the C runner writes only JSONL | A C-only toolchain is not required. One canonical output with wrappers around it is simpler, and Meson already guarantees `python3`. | `bm_stats.c`, `bm_compare.c`, Go and human output written by the runner |
| Epoch is the unit of uncertainty, inside one process | EAL init and teardown are slow. ASLR is page-granular, so a new process does not vary code alignment; what it does vary (heap and stack placement, frequency history) an epoch reproduces. | Process as the unit (R processes per build); CI over batches of one layout, which is too narrow |
| Call through a function pointer only, with calibrated harness cost | Pointer calls are enough for the target snippets, provided call and glue cost can be calibrated and subtracted. | Inline variant generated per snippet |
| Calibrate with a reference sweep (`a + 3k` fit), subtract `a` only in chained mode | Latency along a chain is additive, so subtraction holds there. In throughput mode the out-of-order core overlaps harness and snippet, so subtracting an identity snippet over-corrects. | Subtracting an identity snippet in both modes; subtracting timer overhead on top of it (counts the timer twice) |
| Compare revisions by rebuilding with Git: one JSONL per build | Simplest workflow; core cycles cancel most frequency drift between sessions for compute-bound code. | Two builds in one binary; alternating processes from two build directories (kept as an option via `git worktree`) |
| Interleaved, paired rounds deferred | Only needed with several implementations in one binary; version 1 compares builds. | Interleaving in version 1 |
| Bootstrap CI over epochs (log ratio) plus a minimum effect size; no Wilcoxon | One decision rule, honest and simple. | Wilcoxon signed-rank on raw ratios |
| Collapse check in an untimed pass, adapters only: count distinct inputs from `next` (revised Nov 8, 2026; superseded Nov 9: adapters removed, nothing can collapse) | A rolling check in the timed loop adds work and branches to what is measured. Word-in/word-out and pointer inputs are corpus items and cannot collapse; an adapter's input sequence depends on the corpus as well as `out`, so it is not `x = f(x)` and Brent's algorithm does not apply. | Last-8-outputs check per iteration; Brent's algorithm on the output sequence (the original choice); reseeding from the corpus every 1024 iterations |
| Adapter implementations get their own chained sweep through their `next` (Nov 8, 2026; superseded Nov 9: adapters removed) | An adapter's glue (a call to `next`) differs from `flw_chain_dep`'s; subtracting the startup `a` would leave the adapter cost in the result. | Subtracting the word-mode `a` and recording that the adapter is not included |
| Pointer snippets get the runner's pointer dependency; `next` is optional and gets `state` (Nov 8, 2026; the `next` part superseded Nov 9) | Every adapter written so far was the same line, `item + (out & flw_zero())`. Built in, its glue is the same for every implementation, so one sweep per run covers them, no collapse check is needed, and the floor drops from about 14 to 8 cycles (no call to `next`). A custom `next` stays for inputs that `out` really picks; without `state` it could only reach globals. | Passing `out` to the next call through `state` (a store and a load on the chain, whose cost varies by CPU and cannot be calibrated or checked by the runner); dropping `next` altogether (walks then fit only inside one call with `ops_per_call`, unchecked for collapse); sharing the word in/out sweep (to be measured first, see Open items) |
| No adapter: `flw_impl` has `fn` and `chain` only; a walk where the output picks the next input runs inside one call (`chain` with `ops_per_call`) (Nov 9, 2026) | Three fields (`fn`, `chain`, `next`) were confusing, and no benchmark used a real `next`. `chain` stays because only it measures a kernel on a number without a load on the chain (about 5 cycles on Kaby Lake). Removing `next` also removes the per-implementation adapter sweep, the collapse check, the adapter loop and the `adapter` part of the `bench` record. Lost: the runner's collapse check for walks, subtracting the step to the next input as glue, and running the same single-step `fn` chained (a walk declares its R per implementation). `next` can come back if a real walk benchmark needs it. | Keeping `next` (with `state`); folding `chain` into `fn` with a flag saying the input is a word |
| No branch-predictor or L1 flush between blocks | 1 ms blocks retrain the predictor in microseconds; a flush makes block starts colder than reality. | Fixed flush between blocks |
| DPDK EAL for pinning and memory; benchmark on a worker lcore by default | The project ships inside DPDK software; DPDK and jansson are already dependencies, and a pinned busy lcore matches production. | `sched_setaffinity`, `mlockall`, a hand-written JSON writer |
| Page size is an EAL argument (`--no-huge` or not); the code is agnostic | DPDK falls back through its own arguments; no code path differs. Page size is recorded, and runs with different page sizes are not compared. | Code paths or warnings per page size |
| Own fenced TSC/PMU reads in the timed window | `rte_rdtsc()` is unfenced; `rte_rdtsc_precise()` uses a full barrier with different ordering. | Using DPDK's TSC helpers in the window |
| Generic x86-64 (Intel and AMD), no per-model tables | No specific platform is targeted; `imul` latency 3 and `add` latency 1 hold on modern cores of both vendors. | Latency table keyed by CPU model; aarch64 backend |
| TSC-only mode when no PMU is available | Developer machines may have `perf_event_paranoid` of 3 or 4, or run in VMs. | Treating a missing PMU as an error |
| `dev` and `ci` profiles; `ci` fails (exit 1) on environment problems | Used both on developer machines and on a dedicated, tuned CI runner. A SKIP in CI would hide regressions. | `--strict` mapped to exit 77 (SKIP) |
| Go-format names end in `-1`; environment goes in `key: value` lines | In Go the suffix is GOMAXPROCS; a core number there splits benchmarks in `benchstat`. | `-<cpu>` suffix with the core number |
| `ops_per_call` (R) declared in `flw_impl`; samples stay per call, wrappers convert | The chained floor is per call and does not amortize with N; doing R ops per call lifts a cheap snippet above it. Correction (`a`) and the floor check are per call, so the tools need R, and the runner cannot discover it. R is a match field in `bm-compare`, like U. | R known to the user only, who divides results by R (over-corrects by R and breaks the floor check); R per benchmark instead of per implementation |
| Each benchmark is an explicit `executable()` plus `benchmark()` with all its arguments; no `bench_eal` or `bench_profile` options | What a benchmark runs with is visible in one place, in its own build file; per-run changes to runner arguments go through `meson test --test-args`. | Global Meson options for EAL arguments and profile, and a `benches` table looped over with `foreach` |
| Calibration in 3 rounds over the whole sweep, each point keeping its lowest round; a second fit on the second-fastest rounds confirms `a` and the floors or quantifies the risk (Nov 8, 2026) | Interference comes in bursts and only adds cycles; one disturbed point moved the floor while the slope test passed. Spreading the rounds over time and taking the minimum removes a burst. A warning must say what a disturbance means for the results, so the person running the benchmark can judge the risk: how far corrected values may be off and which results may get the wrong harness-bound verdict. | Repeating each point back to back (one burst covers all repeats); a warning when the floor is beyond k = 2 (fires on cores where that is normal); a warning when any point's rounds disagree (fired in 3 of 8 quiet runs whose calibration was right, and gave no measure of harm) |
| Context switches counted with `getrusage(RUSAGE_THREAD)` outside the window (Nov 8, 2026) | A software event in the perf group has no `rdpmc` index, so every group read would fall back to `read()`; `getrusage` needs no PMU access and works in TSC-only mode. | `PERF_COUNT_SW_CONTEXT_SWITCHES` in the group (the original plan) |
| Each epoch regenerates the corpus with the same seed and runs `setup`/`teardown` again (Nov 8, 2026) | The generator may store pointers into the corpus, so copying a master corpus to a new address could break it; regenerating keeps the bytes and moves the address. A fresh `setup` per epoch gives the state a fresh allocation, the only placement variation the runner can give memory it does not allocate. | Copying one master corpus to each epoch's address; one `setup` per implementation |
| The SMT sibling's load is measured over the whole run, from `/proc/stat`, and judged at the end (Nov 8, 2026) | The load that hurts is the load during the measurement; a snapshot at startup misses a sibling that wakes up later. `/proc/stat` is host-wide, so it works inside Docker. | A short sample at startup only |
| `bm-compare` compares raw per-call values, not corrected ones (Nov 8, 2026) | Runs are stateless, so each has its own `a`; subtracting two noisy calibrations adds their noise to the ratio. Raw values contain the same harness cost on both sides of a like-for-like comparison. | Comparing corrected per-op values |
| Benchmark runs are stateless: every run calibrates itself; `a`, the floors and every other calibration value are measured in that run, never loaded from a file, a table or an earlier run (Nov 8, 2026) | A run depends only on the machine, the build and its arguments, so it can be judged and reproduced on its own; no file can go stale when the compiler, flags or runner change. Comparing builds already uses raw cycles, which do not contain `a`. | Per-CPU-model tables; a calibration measured once per machine and saved; a flag to reuse an earlier run's calibration so base and new subtract the same `a` |
| `-falign-functions=64 -falign-loops=32` on benchmark code | Code layout shifts between commits and is not randomized at runtime. | Relying on layout randomization alone |

**Open items to verify during implementation**

- ~~Does the shipped DPDK version have `lib/pmu` with x86 support and grouped reads?~~ Resolved in step 2: DPDK 25.11.2 ships `lib/pmu` with an x86 `rdpmc` path, but it is experimental, has no `read()` fallback (it returns a wrong value when `index` is 0), opens events by sysfs name only (no software events, so no context switches), and adds lcore and init branches to every read. We use our own code in `bm_core.c`.
- ~~Which DPDK call reports the page size behind an `rte_malloc` allocation?~~ Resolved in step 8: `rte_mem_virt2memseg_list(addr)->page_sz` (4096 with `--no-huge`).
- EAL init time on a developer machine, to size the default E × K.
- Redraw the architecture diagram, or keep the text sketch.
- Step 10 acceptance needs a CI runner; wired and tested locally: without a baseline the check is skipped under `dev` and fails under `ci`; against an earlier run of the same build (with `--allow-same`) it passes; with the planted slowdown as the new result it fails (exit 1). GitLab integration (artifacts, JUnit verdicts, where the baseline comes from) is not done: discussed as a detour, no decision yet.
- Step 9 acceptance: `bm_stats.py` passes its tests on synthetic data with known shifts. A planted slowdown (a throughput snippet with 105 instead of 100 dependent `imul`, built twice) was flagged `REGRESSION +3.6%`, CI [1.023, 1.070], exit 1: 5% more instructions cost 3.6% more time because the core overlaps independent calls; the same binary run twice gave no change (0.986, CI containing 1). A `crc32` A/A gave no change. `benchstat` (installed on the host, outside Docker) reads `bm-go` output: time/op, cycles/op and IPC tables per implementation; A/A `crc32` showed no change in cycles/op; the planted slowdown read +4.2% cycles/op (p = 0.000). Caveat: `benchstat` treats every kept sample as independent (n about 270), so it is overconfident: in the same A/A pair it called +0.22% time/op significant (p = 0.049). Verdicts come from `bm-compare`, whose unit is the epoch; `benchstat` is for viewing.
- Step 8 acceptance on the laptop: `dev` warns (governor `powersave`, turbo on, cpu 3 not isolated, sibling busy) and exits 0; `ci` exits 1. With Firefox restarting, the sibling was busy 83 to 100% of the run and `bitwise` read about 2900 cycles per call instead of 2147, with few rejected samples: the case the sibling check exists for. With Firefox closed: sibling 5 to 9% busy (desktop load), `bitwise` 2147.
- Step 7 acceptance (an A/A comparison whose CI contains 1.0 in 95% of pairs) needs `bm-compare` (step 9); open until then. First look on the laptop (cpu 1 online, light desktop load): with randomized layouts, epoch medians spread 1.7 to 2.7% for `Crc32`, 3 to 5% for the reference chains and 8.4% for `adapter-imul-4`, against 0.02 to 1.2% within an epoch. The environment probably dominates; repeat on a quiet machine to separate layout sensitivity from noise.
- Step 6 acceptance on the laptop (cpu 1 online, `powersave`, turbo on, light desktop load): a busy loop pinned to the worker's cpu rejected 81 to 102 of 300 samples, mostly `ratio` with 7 to 16 `ctx`; the kept samples gave the same result as a quiet run (`bitwise` 2148.9 against 2146.6 cycles per call). Bursts on the worker's cpu (20 ms every 100 ms) rejected 10 to 39. **Open:** a "quiet" run still rejected 14 to 47 of 300 (5 to 16%), all by ratio and all drops, clustered at 2.5 to 3.6%: frequency steps (turbo bins move when other cores wake), not interrupts (330 per second on cpu 3, about 0.2% of a batch). Correct by design; "almost none" needs the `performance` governor with turbo off (step 8), to be checked on a tuned machine.
- **The runner's compiled code moves a benchmark by 10%, with the same timed instructions** (Nov 10, 2026, laptop, `bm_memcpy`, `--mode=both --epochs=50 --samples=40`). `Memcpy64/chained/bytewise` read 77.2 cycles per call at 7256f1d and 85.2 at 184f41b, the same in every run, and `bm-compare` called it a REGRESSION with a tight CI. The commit did not touch the benchmark: `copy_bytewise` sits at the same address and `loop_pointer` runs the same instructions (341 per call). Experiments, all on 184f41b:
  - Moving `loop_pointer` by 0 to 960 bytes (filler functions around it) changed nothing.
  - Shifting the whole program, or the runner only, by 64 to 2048 bytes (filler before and after `bench_memcpy.c` at link time) changed nothing: 85.2 in all 15 builds.
  - Building one file at a time with `-fno-toplevel-reorder`: flagging `bm_core.c` (the timed loops) or `bench_memcpy.c` (the snippet) kept 85.2; flagging `bm_bench.c`, or the other runner files (`bm_calib.c`, `bm_check.c`, `bm_run.c`, `bm_env.c`, `bm_out.c`), gave 77.2. The flag does more than keep source order: in `bm_bench.c` GCC then stops inlining the static functions (`run_impl`, `report`, `check_bench` and others become 15 separate functions, and `bm_run_benches` shrinks from 5.4 KB to 1.2 KB). So the 77 build differs in the code that runs outside the timed window, not only in its order; the timed loop and the snippet are at the same addresses as in the 85 build.
  - Moving one function at a time (one section per function with `-ffunction-sections`, an ordering file for `ld --section-ordering-file`, the old slot filled so nothing else moves): `run_batches`, `run_batches_shifted`, `bm_job_run`, `loop_pointer`, `bm_thread_mark` and `copy_bytewise`, each moved 10 to 46 KiB to 4 positions, stayed at 85 in all 24 builds.
  - The 77 build's code linked in 11 different orders of `bm_bench.c`'s functions (as built, reversed, 8 seeded shuffles) stayed at 77 in all 11. With the code fixed, the function order does not matter; how the runner is compiled does.
  - Heap: the destination buffer of `copy_bytewise` sits at page offset `0x180` in the 85 build and `0x100` in the 77 build, but forcing it to each of the 64 line offsets of a page in the 85 build never gave 77.
  - A random burst once per epoch, before the warm-up (seeded, 256 branch sites taking random directions for 2048 to 4096 rounds, optionally with random loads and stores over 64 KiB), did not mix the levels: the 85 layout stayed at 85 in 48 to 50 of 50 epochs, the 77 layouts at 77 in 44 to 48. Leftover branch history is therefore not what picks the level.
  - Each level holds for whole epochs. The 77 layouts landed on 85 in 2 or 3 epochs out of 50, so one build can reach both, but rarely. Epochs (corpus and stack offsets) do not move it.
  - Hardware counters (raw events through the runner's own `perf_event_open`, in place of instructions), per call, 85 build against 77 build: cycles stalled on a full core resource 9.3 against 0.2 (store buffer full: 0 in both), operations dispatched to port 4 (stores) 75.7 against 65.4 for about 65 stores in the code, false 4 KiB store overlaps 0.08 against 0; mispredicted branches, pipeline flushes, blocked store forwarding and the decoded-instruction cache do not differ. So in the slow state about 10 of the 64 byte stores per call are dispatched twice (sent before the loaded byte is ready, then sent again), the waiting operations fill the scheduler, and the stalls account for the 8 cycles. Throughput mode shows the same, smaller: 2 extra stores per call, 1 cycle.
  - Every position experiment moved code by multiples of 64 bytes (functions are 64-byte aligned), while recompiling also changes where instructions sit inside a 64-byte block. But 1 to 32 `nop`s at the start of `run_batches` or of `bm_job_run` (13 builds) kept 85 (85.3 to 88.7 on a noisier machine, never 77), so the offsets inside those two functions are not it.
  - Keeping one static function of `bm_bench.c` out of line (`noinline`, 15 builds, one function each, rerun with 20 epochs on a noisy machine): `bench_count`, `run_modes`, `place_corpus`, `run_impl` and `run_bench` each brought 77 back (4 to 8 of 20 epochs at 77, most others between 78 and 83), while `full_name`, `report`, `summarize` and the unchanged build never went below 85. In the default build all of them are inlined into `bm_run_benches`; any of several unrelated changes to that one function's code flips the level, including `bench_count`, which runs once per process.
  - Optimization level (20 epochs each, noisy machine): `-O2` (`release` with `-Doptimization=2`, and `debugoptimized`) compiles the timed loop and the snippet to the same 341 instructions per call as `-O3` but the runner differently (`bm_run_benches` 5.0 KB instead of 5.4), and landed on 77 in 7 and 8 of 20 epochs, never on 85; `-O3` never went below 85.2. `-Os` compiles the snippet itself differently (404 instructions per call, about 261 cycles), so it is not comparable.

  Side finding: `Memcpy1024/throughput/memcpy` reads about 45 instead of 40 when `bench_memcpy.c` is reordered (`copy_libc` then sits before `copy_bytewise`); there the snippet file's own layout matters.

  Reading: how the runner is compiled outside the timed window changes the speed of a timed window that runs the same instructions at the same addresses. Inside the core, the slow state resends stores and stalls on a full scheduler; what in the runner's code selects that state is unknown: not code position, not function order, not leftover branch history, not the buffer's page offset, not instruction offsets in the functions next to the timed loop. One untested idea: the CPU's guess, per load instruction, whether a load may run ahead of older stores (no Kaby Lake counter shows it); a snippet with loads only or stores only would tell whether the gap needs both. The runner cannot vary this from inside one binary, so the CI over epochs does not cover it, and `bm-compare` cannot tell it from a real change.

  Workarounds, not built:
  - Compare only builds whose runner is the same. The false REGRESSION came from a commit that changed the runner. `bm-compare` could warn when the two runs' runners differ (for example a hash of the framework sources in the env record), so the results are re-baselined instead of compared. The framework changes rarely in the target project, so the guard costs little.
  - It does not cover changes elsewhere in the same benchmark binary: reordering `bench_memcpy.c` moved `Memcpy1024/throughput/memcpy` by about 12%. Keep one family of benchmarks per binary.
  - Builds with seeded function orders (one section per function, a shuffled ordering file) would not have varied this case, since order with the code fixed did not move it. Varying the layout across builds would mean varying how the code is compiled, and the two sides would then no longer run the same code.
- Pointer dependency (Nov 8, 2026), on the laptop (cpu 1 online, light desktop load): the pointer sweep gave `a` 2.22, floor 8.0 at k = 2, slope 3.065, confirmed; the word in/out sweep of the same run gave 2.08 and 8.29. `ptr-imul-4` (load plus 4 `imul`) read 17.14 corrected, as through the old adapter (16.9 to 17.1), and `ptr-imul-16` 52.84. A real adapter with `state` (`out` picks the item, removed Nov 9) passed the collapse check with 227 distinct inputs and read 17.50 on its own sweep (`a` 3.0, floor 15). Its replacement, a walk inside one call (`walk-R8`, 8 steps through the corpus, `chain` with `ops_per_call = 8`), read 7.21 per step: an L1 load plus a shift and a mask. One pointer sweep served all 8 chained `Memcpy` implementations. **Open:** whether the pointer and word in/out sweeps agree closely enough on a quiet machine, and on AMD, to share one sweep.
- Step 5 acceptance on the laptop (sibling offline, quiet desktop): word-in/word-out `imul` chains through the full glue read 11.97 to 12.11 (k = 4), 23.66 (k = 8) and 47.63 (k = 16) after subtracting `a` (2.0 to 2.4), within 1.4% of 3k; k = 1 and 2 are `harness-bound` in clean runs. Through the adapter: k = 4 reads 16.9 to 17.1 and k = 16 reads 53.1 after subtracting the adapter's `a`, since the test snippet also loads its input (about 5 cycles, part of its cost). A collapsing adapter (1 distinct input in 65,536 calls) fails the run with exit 1. `ops_per_call = 8` on a 16-`imul` chain reads 5.96 per op (47.7 / 8). **Found:** a disturbance during the startup sweep (an SMT sibling waking up) can push k = 2 and 4 off the line while the slope stays in tolerance (one run: slope 2.99, `floor_k` 8, floor 26). The floor then flags healthy results such as k = 4 as `harness-bound`; the self-test does not catch it. **Fixed** (Nov 8, 2026) with 3 rounds per point and the stability test. Tested on the laptop under heavy load (load average 3.9 on 4 threads, Firefox decoding video, cpu 1 online): over 8 runs the floor landed at k = 2 in 3, at k = 4 or 8 in 5, and the stability warning fired in all 8. The load was steady rather than bursty, so even the lowest of 3 rounds was disturbed (`ref/nop` read 11 to 12 cycles instead of 6); the rounds cannot fix that, but the run now says so instead of silently using a wrong floor. Calibration takes about 1 s; a whole run with no benchmark selected took 1.0 to 1.6 s. With the confirmation fit, on a quiet laptop: floor right in 8 of 8 runs; the warning fired in 1 of 4 test runs (floor 8.0 could be 14.8), and only the one result inside that range was marked uncertain. With 50 ms bursts on the SMT sibling every 250 ms: 1 of 4 runs warned (`a` 2.03 could be 2.71, floor 8.0 could be 26.3), and the affected results showed `+- 0.68` and `harness-bound uncertain`.
- Step 4 acceptance is partly met on the laptop, open on a stable machine. On the laptop with a game running (70% of a CPU), epoch medians spread 2 to 9%. With the desktop only (worker on cpu 3, its sibling cpu 1 about 10% busy with Firefox and Xorg), three runs gave: `Crc32/throughput/bitwise` spread 0.6 to 1.3%, 9 or 10 of 10 epochs within 1% of the median; `Crc32/throughput/table` 3.9 to 18%; `ref/imul-indep-16` and `-32` 3 to 9%. The cycles/TSC ratio was 1.140 in nearly every epoch and instructions per batch were constant, so neither frequency nor preemption explains it. Slow epochs come in runs of adjacent epochs (`table`: 432, then 512 for three epochs, then 432 again), which points to bursts of load on the SMT sibling: the L1-bound and port-bound snippets suffer, the long ALU-bound one barely does. Sample filtering (step 6) cannot see this, because the ratio does not move. With the sibling offline (cpu 1 down, worker alone on its physical core), three runs gave: `bitwise` spread 0.4 to 0.9%, 10 of 10 epochs within 1% in every run (headline 2149 to 2152 cycles, 0.13% apart); `table` 1.8 to 2.9%; `ref/imul-indep-s` and `ref/nop` 2.5 to 5.5%, 5 to 8 of 10 epochs within 1%. The remaining spread is not noise in the usual sense: within an epoch the MAD is 0.01 to 0.3%, the ratio stays 1.140, and epoch medians sit on discrete levels (`imul-indep-32` at 36.51 or 37.2 to 37.6, `table` at 439.1 or 445 to 452, `nop` at 5.00 or 5.10 to 5.14) that hold for a whole 30 ms epoch. These look like microarchitectural steady states (scheduling and port assignment) of short loops, which is what epochs and the CI over epoch medians exist to absorb. Headline run-to-run differences: `table` 2%, `imul-indep-4` 2.6%, `imul-indep-16` 1%, `imul-indep-32` 2.1%. The criterion stays at 1% (decided Nov 8, 2026): the laptop is not the target, and the check is to be repeated on a stable machine (the CI runner, worker core isolated, sibling offline or idle). `ref/imul-indep-s` lands where the calibration puts it (18.3 to 19 cycles at s = 16, 36.5 to 38 at s = 32, so 1.14 to 1.19 per `imul`); `ref/imul-indep-4` is bimodal at 6.0 and 7.0 cycles per call. Until steps 5 and 7, chained benchmarks are skipped with a warning and warm-up runs once per implementation, not per epoch.
- Found in step 1 (DPDK 25.11.2): an unknown EAL option (`--bogus`) makes the argparse-based EAL parser exit the process from inside `rte_eal_init()` with status 234 (`-EINVAL`), so the runner cannot map it to 99 and Meson shows FAIL, not ERROR. Invalid values of known options (`-l 2,9`) do return an error and exit 99. Decide whether to pre-validate EAL options or accept this.
