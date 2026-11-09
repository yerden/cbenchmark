# Writing benchmarks

This guide is for people who write benchmarks with the framework. It covers how to declare a benchmark, build and run it, and read the results. The framework's own design (timing, calibration, statistics) is in `C Benchmark Package Implementation Guide.md` at the top of the tree. Terms are explained in `GLOSSARY.md`.

A benchmark measures a small piece of code (a *snippet*: a hash, a parser step, a lookup) in core cycles per call. You supply the inputs and the snippet; the runner owns the clock, the CPU, the memory and the output.

> **Status.** Throughput mode, chained mode (pointer and word in/out), `ops_per_call`, sample rejection, layout-randomized epochs and environment checks work. The report tools (`tools/bm-report`, `bm-go`, `bm-compare`) work too.

## Quick start

A complete benchmark file, `benchmarks/bench_crc32.c` (shortened):

```c
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

/* fill the corpus: c->items has room for count items of c->item_size bytes */
static int
gen_random_buffers(flw_corpus *c, uint64_t seed, size_t count, void *arg)
{
	uint8_t *p = c->items;
	size_t i, n = c->item_size * count;

	(void)arg;
	for (i = 0; i < n; i += 8) {
		uint64_t v = splitmix64(&seed);

		memcpy(p + i, &v, n - i < 8 ? n - i : 8);
	}
	return 0;
}

/* one call: one input item in, one word out */
static uint64_t
crc_table(const void *in, void *state)
{
	const uint8_t *p = in;
	const uint32_t *table = state;
	uint32_t crc = ~0u;
	int i;

	for (i = 0; i < 64; i++)
		crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xff];
	return ~crc;
}

static const flw_impl crc_impls[] = {
	{.name = "bitwise", .fn = crc_bitwise},
	{.name = "table", .fn = crc_table, .setup = crc_table_setup, .teardown = free},
};

static const flw_bench crc_bench = {
	.name = "Crc32",
	.mode = FLW_THROUGHPUT,
	.gen = gen_random_buffers,
	.item_size = 64,
	.count = 1 << 14,
	.impls = crc_impls,
	.nimpls = sizeof(crc_impls) / sizeof(crc_impls[0]),
};
FLW_REGISTER(crc_bench);

int
main(int argc, char **argv)
{
	return flw_main(argc, argv);
}
```

Declare it in `benchmarks/meson.build` with one `executable()` and one `benchmark()`:

```meson
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

Run it:

```
meson setup build
meson test -C build --benchmark crc32
```

For CI, also give it a regression test next to its `benchmark()` (see [Regression checks in CI](#regression-checks-in-ci)).

`meson test` builds the executable and runs it. The results land in `build/benchmarks/crc32.jsonl`; the console line only says OK or FAIL.

## Concepts

| Term | Meaning |
| --- | --- |
| benchmark | one `flw_bench`: a name, a corpus generator and 1 to N implementations of the same operation |
| implementation | one `flw_impl`: a snippet plus optional setup and teardown. Every implementation of a benchmark reads the same corpus bytes and should compute the same thing |
| corpus | the inputs, generated once before timing into a flat array of `count` items |
| call | one invocation of the snippet on one corpus item |
| batch | N calls timed as one window. N is sized so a batch takes about 1 ms, and fixed per implementation |
| sample | one batch's readings: TSC ticks, core cycles, instructions |
| epoch | K batches (default 30); a run has E epochs (default 10) per implementation |

Results are named `Name/mode/impl`, for example `Crc32/throughput/table`.

## The API

Everything is in `bench.h`.

### `flw_bench`

| Field | Required | Meaning |
| --- | --- | --- |
| `name` | yes | benchmark name, the first part of `Name/mode/impl` |
| `mode` | yes | `FLW_THROUGHPUT` or `FLW_CHAINED`; `--mode` can override it at run time |
| `gen` | yes | corpus generator, see below |
| `gen_arg` | no | passed to `gen` and to every `setup` |
| `item_size` | yes | bytes per corpus item, at least 1 |
| `count` | yes | number of items, a **power of two** (the loop wraps the index with a mask) |
| `impls`, `nimpls` | yes | the implementations, at least one |

A definition that breaks these rules stops the run with exit code 99 and a message naming the benchmark.

### `flw_impl`

| Field | Meaning |
| --- | --- |
| `name` | implementation name, the last part of `Name/mode/impl` |
| `fn` | `uint64_t fn(const void *in, void *state)`. `in` points at one corpus item. Required in throughput mode |
| `chain` | `uint64_t chain(uint64_t in, void *state)`: chained mode, word in/out; takes over from `fn` in chained mode. Needs `item_size >= 8` |
| `setup` | `void *setup(const flw_corpus *c, void *arg)`, called at the start of every epoch, returns `state`. `arg` is the benchmark's `gen_arg`. Returning NULL means failure |
| `teardown` | `void teardown(void *state)`, called after the last batch. `free` works for a `malloc`ed state |
| `ops_per_call` | R, operations per call; 0 means 1. See [Several operations per call](#several-operations-per-call) |

`setup` and `teardown` are not timed. They run once per epoch (10 times by default), because every epoch moves the corpus to a new address and wants fresh state; keep them repeatable and free everything in `teardown`. If `setup` returns NULL, that implementation is skipped with a warning and the run exits 1. An implementation without `setup` gets `state == NULL`.

An implementation with `fn` serves both modes: in chained mode the runner passes `fn` each corpus item made to depend on the previous result. Give it `chain` as well to run chained as word in/out instead. The benchmark's `mode` must be one it supports, or the run stops with exit 99; under `--mode` an implementation that lacks the functions for a mode is skipped with a warning.

### Corpus generator

```c
typedef int (*flw_gen_fn)(flw_corpus *c, uint64_t seed, size_t count, void *arg);
```

The runner allocates `c->items` (`count * item_size` bytes, cache-line aligned, on the worker's NUMA node) and calls `gen` with the run's seed (`--seed`, default 1). Fill the items and return 0; a nonzero return skips the implementation with a warning and the run exits 1.

`gen` runs once per epoch of every implementation: each epoch places the corpus at a new address (a random page and cache line) so results do not depend on one placement. It must give the **same bytes from the same seed** every time, at any address. If items hold pointers, point into `c->items` as it is now, not to an address saved from an earlier call.

`c->bytes_total` starts as `count * item_size`. If items point at other memory (strings, nodes) that the snippet touches, set `bytes_total` to the whole working set; it is reported as `corpus_bytes`.

Make the corpus deterministic from `seed`, so runs reproduce. Do not call the C library's `rand()`: its state is global and its sequence differs between libcs. A splitmix64 or xorshift generator in the file is enough.

### Helpers

| Helper | Use |
| --- | --- |
| `flw_use(x)` | sink: forces `x` to be computed. The runner already applies it to every return value of `fn` |
| `flw_clobber()` | compiler memory barrier, for snippets whose result is a store rather than a return value |
| `flw_zero()` | a zero the compiler cannot see through |
| `flw_chain_dep(in, out)` | `in`, unchanged, made to depend on `out` (chained mode) |

### `flw_main`

`main` must call `flw_main(argc, argv)` and return its value. It initializes DPDK's EAL, parses the options, runs everything and returns the exit code.

## Writing a good snippet

**Return the result.** The runner sinks the return value of every call, so the compiler cannot drop the work. A snippet that returns a constant, or a value that does not depend on its input, measures nothing. If the result is a store (into a buffer in `state`), return something derived from it, or call `flw_clobber()` after the store.

**Compute from the input.** Each call gets a different corpus item. Work that does not depend on `in` can be hoisted out of the loop by the compiler, or predicted perfectly by the CPU.

**Keep the snippet in its own function.** The runner calls it through a function pointer, and benchmark executables are built without LTO, so the call boundary is real. Do not mark the snippet `static inline` and expect it to be inlined into the loop; it will not be.

**Put tables and other read-only data in `setup`.** Building a table in the snippet measures the build. Allocating in the snippet measures the allocator.

**Make all implementations agree.** They read the same corpus and should return the same values. The framework does not check this; a quick self-check in a unit test catches an implementation that is fast because it is wrong.

### Specific to memcpy: copy benchmarks

These traps apply when you benchmark `memcpy` or a hand-written copy loop, as in `benchmarks/bench_memcpy.c` (libc `memcpy` against a byte-by-byte loop, 16 to 1024 bytes, both modes). They are not general rules for other snippets.

**A fixed-size `memcpy` is not a libc call.** With a size the compiler can see, such as `memcpy(dst, src, 64)`, it writes the copy itself as a few vector moves, so you measure the compiler, not libc. Keep the size in `state` (set in `setup`), so it is only known at run time; then the snippet calls libc's `memcpy`.

**GCC turns a byte loop into `memcpy`.** At `-O3` (`buildtype=release`), GCC recognizes `for (i = 0; i < n; i++) d[i] = s[i];` and replaces it with a call to `memcpy`, so both implementations measure the same code. Turn that off on the loop's function with `__attribute__((optimize("no-tree-loop-distribute-patterns")))`.

**GCC vectorizes a byte loop.** Even without the `memcpy` call, `-O3` copies 16 or 32 bytes per step. If "naive" means one byte at a time, also add `"no-tree-vectorize"` to the attribute. Clang ignores this GCC attribute (and says so), so under clang check the code again.

**Copy the destination and size into locals.** A byte store may write to any memory, including the state struct, so a loop over `st->dst[i]` and `st->n` reads both back from memory on every byte. Measured: that made the byte loop about 40% slower than the same loop with locals. Read `st->dst` and `st->n` into local variables before the loop.

**Return a byte of the copy, not the size.** The copy's result is a store. Returning `st->n` gives the runner a value that does not depend on the copy, and in chained mode the next call then does not wait for the copy: calls overlap, and the result is throughput under a chained name. Return the last copied byte, `d[n - 1]`. The chained result is then the time until the copied data can be read, including the store-to-load forwarding of that last byte (about 4 to 5 cycles).

Check the result in the disassembly (`objdump -d build/benchmarks/bm_memcpy`): the libc version should contain `call memcpy@plt`, and the byte loop one byte load and one byte store per step.

## Sizing the corpus

The corpus size decides what you measure:

| Working set | Measures |
| --- | --- |
| fits in L1 (up to about 32 KiB) | the hot path: instruction latency and throughput |
| fits in L2 or LLC | the snippet plus cache hits at that level |
| larger than LLC | memory behaviour; results depend on the memory system and on page size |

The CRC benchmark uses 16384 items of 64 bytes (1 MiB), so it runs out of L2 on most cores.

Match the data to production. Branch outcomes, string lengths, key skew and match rates move the numbers more than most code changes. A corpus of 1024 identical items lets the branch predictor learn it; random data with the production distribution does not.

## Throughput mode

The runner calls the snippet on items `0, 1, 2, ...` (wrapping at `count`), back to back, with nothing linking one call to the next. The out-of-order core overlaps consecutive calls, so the result is how many calls the core sustains per unit of time: **cycles per call at a full pipeline**, not the latency of one call.

Use it for code that runs in a loop over independent inputs: hashing a burst of packets, parsing a batch of headers.

**The floor.** The harness itself (the indirect call, return, loop, index and load) costs about 5 cycles per call on a recent Intel core. A snippet cheaper than that reads as the floor. The calibration at startup measures this floor, and an implementation whose median is below it or within 3% (plus half a cycle) above it is marked `harness_bound` in its `bench` record and in the console line:

```
Ref/throughput/nop: N=617225, 5.01 cycles/call, harness-bound
```

A `harness_bound` result says the snippet costs *at most* the floor, nothing more. To measure it, do more work per call (see below).

## Chained mode

In chained mode every call waits for the result of the previous one, so calls cannot overlap and the result is **latency per call**. Use it for code on a dependency chain, where the next piece of work cannot start before this one ends.

The runner keeps feeding corpus items, as in throughput mode, and only makes each call wait: it combines the item with the previous result through a zero the CPU cannot see (`out & flw_zero()`), so the input stays the corpus item but its timing depends on the previous call. There are two shapes, and an implementation picks one with a field:

| Field | Snippet gets | Use for | Load from the item |
| --- | --- | --- | --- |
| `fn` | the item's address | snippets that read a buffer (CRC, parser, copy) | on the chain, counts in the result |
| `chain` | the item's first 8 bytes | kernels on a number (hash finalizer, arithmetic) | done by the runner, off the chain |

In chained mode `chain` runs when it is set, else `fn`; throughput mode always runs `fn`.

**Pointer (`fn`).** An implementation with only `.fn`, the same function as in throughput mode, runs chained as it is:

```c
static const flw_impl crc_impls[] = {
	{.name = "table", .fn = crc_table, .setup = crc_table_setup, .teardown = free},
};
```

The runner passes `fn` the corpus item's address made to depend on the previous result (the **pointer dependency**). `fn` must return a value computed from what it read, or the next call does not wait (see the memcpy pitfall above).

**Word in, word out (`chain`).** Set `.chain`:

```c
static uint64_t
mix_chain(uint64_t in, void *state)
{
	(void)state;
	return mix(in);
}

static const flw_impl impls[] = {
	{.name = "mix", .chain = mix_chain},
};
```

The runner reads the first 8 bytes of each corpus item, makes it depend on the previous output with `flw_chain_dep()`, and passes it in. The corpus items must be at least 8 bytes. The load happens before the chain, so a cheap kernel is measured without it: 4 `imul` read 12.3 cycles as `chain` and 17.1 as `fn` on Kaby Lake. Add `.fn` too if the implementation should also run in throughput mode.

**When the result picks the next input.** A table walk, or a parser whose next token starts where the last one ended, needs each result to choose the next input. The runner does not do that; walk inside one call instead. Take the corpus word as the starting point, walk R steps through a table in `state`, and declare R (see [Several operations per call](#several-operations-per-call)). `setup` receives the corpus, so the table can be the corpus itself:

```c
struct walk {
	const uint64_t *items;
	size_t mask;
};

static uint64_t
walk_8(uint64_t in, void *state)
{
	const struct walk *w = state;
	int i;

	for (i = 0; i < 8; i++)
		in = w->items[(in >> 32) & w->mask]; /* this item picks the next */
	return in;
}

static const flw_impl impls[] = {
	{.name = "walk", .chain = walk_8, .setup = walk_setup, .teardown = free,
	 .ops_per_call = 8},
};
```

Each call starts from a fresh corpus word, and the step to the next item counts in the result (measured: 7.2 cycles per step, an L1 load plus a shift and a mask). **Keep the walk varied**: if it falls into a short loop (always the same few items), the CPU learns it and the numbers are too good. The runner cannot see inside a call, so it does not check this.

**Harness cost.** The runner subtracts the calibrated glue latency `a` (about 2 cycles on a recent Intel core) from chained results. Word in/out uses the startup calibration. The pointer dependency is calibrated once per run, before the first implementation that uses it (about 0.3 s, skipped when none does). The pointer is not read during that calibration, so loads in your snippet stay in its result.

**Floor.** Below a certain latency the harness hides the chain; the floor is about 8 cycles per call on Kaby Lake, for both shapes. A raw result below the floor by more than 3% (plus half a cycle) is marked `harness_bound`, and its corrected value means nothing. Use `ops_per_call` to measure such snippets.

The rule differs from throughput mode on purpose. The throughput floor is what an empty snippet reads, so a result at it could be anything cheaper and is marked. The chained floor is the shortest calibration chain that still measures right, so a result at it is a real measurement; only below it does the harness hide the chain.

Chained and throughput results of the same code measure different things (latency against throughput). The mode is part of the name (`Crc32/chained/table`); never compare across modes.

## Several operations per call

The floor is per call, and a larger batch does not lower it. To measure a snippet cheaper than the floor, apply the operation R times inside one call and declare it:

```c
static uint64_t
mix8(const void *in, void *state)
{
	uint64_t x;
	int i;

	(void)state;
	memcpy(&x, in, sizeof(x));
	for (i = 0; i < 8; i++)
		x = mix(x); /* each step uses the previous result */
	return x;
}

static const flw_impl impls[] = {
	{.name = "mix", .fn = mix8, .ops_per_call = 8},
};
```

Rules:

- **Declare R; never divide by it yourself.** Samples stay per call. The tools subtract the harness cost per call and then divide by R. Dividing first gives wrong numbers.
- In chained mode the R operations must form a chain (each feeds the next, as above). Independent operations inside one call measure throughput, and the runner cannot tell.
- R is per implementation, so `table` can do 1 op per call and `pclmul` 8; per-op results stay comparable.
- The floor check stays per call, so R lifts a cheap snippet above the floor. Measured: a 16-`imul` chain with `.ops_per_call = 8` reads 47.7 cycles per call after subtracting `a`, so 5.96 per op.

## Building

Each benchmark is declared by hand in `benchmarks/meson.build`, as in the quick start. Nothing is global: the sources, the EAL arguments, the output file, the profile and the timeout of a benchmark are all in its two calls.

**`executable()`**

| Argument | Value | Why |
| --- | --- | --- |
| name | `'bm_<name>'` | |
| sources | your benchmark file(s) | the snippets and `main` |
| `dependencies` | `[bm_dep, project_deps]` | `bm_dep` is the framework (runner, DPDK, jansson, alignment flags); `project_deps` is the code under test |
| `override_options` | `['b_lto=false']` | **required**: LTO would inline the snippet into the timed loop and measure something else. A dependency cannot set this, so every benchmark states it |
| `build_by_default` | `false` | `meson test --benchmark` builds what it runs; a plain `meson compile` skips benchmarks |

**`benchmark()`**

| Argument | Value | Why |
| --- | --- | --- |
| name | `'<name>'` | what `meson test --benchmark <name>` selects |
| executable | the `executable()` above | |
| `args` | EAL arguments, then `'--'`, then runner arguments | see below |
| `suite` | `'bench'` | lets `meson test --benchmark --suite bench` select all of them |
| `timeout` | seconds, e.g. `1800` | Meson's default of 30 s is too short for E × K batches over several implementations |
| `protocol` | `'exitcode'` | the runner's exit code is the result; numbers are in the JSONL file |

The `args`:

- **EAL arguments**, before `'--'`. `-l 2,3` gives two lcores: the benchmark runs on the second (3), the first only waits. Pick cores that are idle on the machines you run on. Add `--no-huge` for machines without hugepages, and `--no-pci --no-telemetry` so EAL does not probe devices.
- **`'--'`**, required.
- **Runner arguments**: always `--out=` with a path in `meson.current_build_dir()`, so results land next to the build; usually `--profile=dev` and a fixed `--seed=`. Any option from [Running](#running) can go here (`--epochs=`, `--bench=`, ...).

Build rules that `bm_dep` and the top-level project take care of:

- Build optimized: `buildtype=release` (the default here) or `debugoptimized`. The build type is recorded and checked.
- Code is aligned with `-falign-functions=64 -falign-loops=32` (carried by `bm_dep`), so unrelated edits move hot code less.
- Keep sanitizers out of the benchmark build.

## Running

`meson test -C build --benchmark` runs all benchmarks, one at a time; add a name to run one, `-v` to see the runner's stderr live.

To change runner arguments for one run without editing the build file, append them with `--test-args`. They go after the `args` from `benchmark()`, and the last occurrence of an option wins:

```
meson test -C build --benchmark --test-args='--profile=ci --epochs=20' crc32
```

Meson keeps only the last `--test-args`, so put all extra arguments in one string. EAL arguments cannot be changed this way (they sit before `--`); a machine that needs other cores needs its own `benchmark()` entry.

To run an executable by hand:

```
build/benchmarks/bm_crc32 -l 2,3 --no-huge --no-pci --no-telemetry -- --out=crc.jsonl --bench=table
```

Everything before `--` goes to DPDK's EAL; everything after it to the runner.

| Option | Default | Meaning |
| --- | --- | --- |
| `--bench=REGEX` | all | select implementations by full name (POSIX extended regex): `--bench=table`, `--bench='^Crc32/'` |
| `--list` | | print the selected names and exit |
| `--out=PATH` | `-` (stdout) | JSONL output; the file is truncated |
| `--seed=S` | 1 | seed for the corpus |
| `--epochs=E` | 10 | epochs per implementation |
| `--samples=K` | 30 | batches per epoch |
| `--batch-time=MS` | 1 | target batch duration |
| `--profile=dev\|ci` | `dev` | environment policy |
| `--pmu=auto\|read\|none` | `auto` | how core cycles are read; `none` forces TSC-only mode |
| `--mode=throughput\|chained\|both` | each benchmark's own | run benchmarks in another mode, or in both |
| `--subtract=chained\|all\|none` | `chained` | where the harness cost `a` is subtracted; recorded in each `bench` record |
| `--ref-latency=imul:N\|add:N` | 3, 1 | expected latencies of the calibration references, for an unusual core |

A run takes a fraction of a second per implementation (E × K batches of 1 ms, plus sizing and warm-up), plus about a second of calibration at startup.

### Exit codes

| Code | Meson shows | Meaning |
| --- | --- | --- |
| 0 | OK | finished, file written |
| 1 | FAIL | a corpus generator or `setup` failed, an I/O error, or under `--profile=ci` the calibration self-test failed |
| 99 | ERROR | bad options, EAL init failure, an invalid benchmark definition, out of memory |

An unknown EAL option makes DPDK exit the process itself with 234 (Meson shows FAIL).

## Machine setup

The numbers are only as good as the machine. At startup the runner checks the machine and records what it finds in the `env` record. Every problem is a warning under `--profile=dev` (the default) and makes the run exit 1 under `--profile=ci`:

| Warning | Means | Fix |
| --- | --- | --- |
| `governor of cpu 3 is powersave, not performance` | the frequency moves; expect `ratio` rejections | `cpupower frequency-set -g performance` |
| `turbo is on` | the frequency steps when other cores wake | `echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo` (or `cpufreq/boost` to 0) |
| `cpu 3 is not isolated` | other tasks and interrupts may run on the benchmark core | `isolcpus=` or `nohz_full=` on the kernel command line |
| `SMT sibling cpu 1 of the worker was busy N% of the run` | the other hardware thread of the core was working, which slows the benchmark without any rejected samples; a fully busy sibling made `bitwise` 35% slower | keep it idle or take it offline |
| `cpu N is an E-core` | hybrid CPU, efficiency core | pick a P-core with `-l` |
| `no PMU ...` | TSC-only mode | see PMU access below |
| `build type debug`, `sanitizer`, `without optimization` | the build distorts timings | build with `buildtype=release` and no sanitizer |

The run always finishes and writes its file. On a laptop, expect the first three warnings every time. For numbers you want to compare, set up:

- **PMU access**, for core cycles: `perf_event_paranoid` at 2 or less, or `CAP_PERFMON`. In Docker, `--cap-add=PERFMON`. Without it the run is TSC-only: it works, but cannot see frequency changes, and the warning says so.
- **An idle SMT sibling.** A busy hyper-thread on the benchmark core shifts results by several percent, often in bursts. Take the sibling offline (`echo 0 > /sys/devices/system/cpu/cpuN/online`, with N from `/sys/devices/system/cpu/cpu3/topology/thread_siblings_list`) or keep it idle.
- **A quiet core.** Ideally isolated (`isolcpus=` or `nohz_full=` on the kernel command line), with the `performance` governor.
- Nothing heavy running elsewhere (games, builds, browsers playing video).

On a laptop with desktop load, expect epoch medians to scatter by 2 to 5% for short snippets, even with the sibling offline.

## Reading the output

The JSONL file has one record per line, in this order:

| Record | Contents |
| --- | --- |
| `env` | commit (`git describe --dirty`), CPU, core, kernel, DPDK version, PMU mode, build type, seed, every warning |
| `timer` | cost of an empty timing window |
| `calibration` (two or three) | harness cost and floors: chained (word in/out) and throughput; `pointer` when an implementation ran chained through the pointer dependency |
| `selftest` | whether the counters measured the reference snippets correctly |
| `bench` | one per implementation and mode: name, mode, batch size `iters`, `ops_per_call`, epochs, samples, corpus size, the harness cost `a` and `floor` that apply and where they come from (`calibration`: `startup` or `pointer`), `subtract`, `harness_bound` |
| `sample` | one per batch, after its `bench` record: `epoch`, `batch`, `iters`, `tsc`, `cycles`, `instr`, `flags` |

Samples are raw: cycles per call is `cycles / iters`. A sample whose `flags` is not 0 was rejected: 1 `ratio` (the core's frequency changed, or an interrupt took time), 2 `ctx` (the thread was switched out), 4 `migrate` (it moved to another CPU). Rejected samples stay in the file but are left out of results; the console line says `(x of y samples rejected)`, and more than 10% rejected gives a warning. Lots of `ratio` rejections usually mean the governor is not `performance` or turbo is on; see [Rejected samples and layout sensitivity](#rejected-samples-and-layout-sensitivity). Per op is `(per_call - a) / ops_per_call` when the `bench` record says `subtract`, else `per_call / ops_per_call`; work on per-call values first (medians, the floor) and divide last. Read `cycles` (core cycles) rather than `tsc` (reference ticks, which move with the CPU frequency). In TSC-only mode `cycles` and `instr` are absent.

To read a file, use `tools/bm-report`:

```
tools/bm-report build/benchmarks/crc32.jsonl
```

It prints the environment, every warning, the calibration, then one line per implementation: the median of epoch medians per call with its 95% confidence interval, raw and corrected per op, kept and rejected samples, and notes (`HARNESS-BOUND`, `harness-bound uncertain`, `layout-sensitive` when epochs disagree much more than batches within an epoch; see [below](#rejected-samples-and-layout-sensitivity)).

`tools/bm-go file.jsonl` prints the kept samples in Go benchmark format, for `benchstat`:

```
tools/bm-go base.jsonl > base.txt
tools/bm-go new.jsonl > new.txt
benchstat base.txt new.txt
```

`benchstat` is handy for familiar tables, but it treats every batch as an independent measurement and so is overconfident: comparing a build with itself, it once called a 0.2% difference significant. Decide with `bm-compare` (below), which works per epoch.

The console also prints one line per implementation, with batch size, the raw median per call and the per-op value (`corrected` when `a` was subtracted):

```
Crc32/throughput/bitwise: N=1434, 2147.69 cycles/call, 2147.69/op
Crc32/throughput/table: N=7128, 438.22 cycles/call, 438.22/op
Crc32/chained/bitwise: N=1393, 2185.45 cycles/call, 2180.00/op corrected
Crc32/chained/table: N=5848, 528.82 cycles/call, 526.63/op corrected
```

**Check the warnings first.** They are on stderr and in the `env` record. A failed self-test (`chained imul slope 3.162, expected 3.00 +- 3%`) means the core was disturbed during calibration; the warning names the likely cause. Results from such a run are suspect. Another warning says how far to trust the calibration:

```
warning: calibration uncertain (chained mode), interference on the core:
  harness cost a = 2.03 cycles, could be 2.71
  -> corrected results may be off by up to 0.68 cycles per call
  floor = 8.00 cycles, could be 26.27
  -> results between 8.00 and 26.27 cycles per call may be wrongly marked, or not marked, harness-bound
```

Calibration times each reference 3 times and keeps the fastest; this warning means the next-fastest timings give a different answer. The results it affects say so in their console line (`5.97/op corrected +- 0.68 (calibration uncertain), harness-bound uncertain (floor 8.00 or 26.27)`) and in their `bench` record (`a_alt`, `floor_alt`, `harness_bound_uncertain`). Results outside the risky range are not affected. Quiet the machine and run again to remove the doubt.

### Rejected samples and layout sensitivity

**How a sample gets rejected.** The runner checks every batch for three kinds of disturbance, from what the hardware and the OS report, not by throwing out numbers that look odd:

| Flag | Check | Catches |
| --- | --- | --- |
| `ratio` (1) | core cycles ÷ TSC ticks of the batch is more than 2% from the median of its epoch | the frequency changed during the batch, or an interrupt ran (its time counts in TSC ticks but not in core cycles, so the ratio drops) |
| `ctx` (2) | the thread's context-switch count went up during the batch | the OS gave the core to another task |
| `migrate` (4) | the CPU number before and after the batch differ | the thread moved to another CPU (it is pinned, so this should not happen) |

On an undisturbed batch the ratio of core cycles to TSC ticks stays the same, because the TSC ticks at a fixed rate and the core at its own; a change means something happened. The `ratio` check needs core cycles, so in TSC-only mode (no PMU access) it does not run. A batch can carry several flags.

**What to make of it.** Kept samples can be trusted even when many were rejected: on a laptop, a busy loop on the benchmark core rejected about a third of the samples, and the kept ones gave the same result as a quiet run within 0.1%. A high rejection rate says the machine is noisy, not that the result is wrong. Rejections that are almost all `ratio` mean frequency steps: set the `performance` governor and turn turbo off (see [Machine setup](#machine-setup)). Many `ctx` mean other tasks on the core: isolate it. What rejection cannot catch is a busy SMT sibling, which slows the benchmark without moving the ratio; that has its own warning.

**Layout-sensitive.** Each epoch places the corpus at a new address (a random page and cache line), while it stays put for the batches inside the epoch. `bm-report` compares two spreads, each measured as a MAD (median absolute deviation): of the epoch medians, and of the batches within each epoch. When the first is more than 3 times the second, moving the data changed the speed more than noise did, and the result is marked `layout-sensitive`.

The headline is still the median over all placements, so it remains a fair number, but its CI is wider, and one placement of your real code may run faster or slower than it. Usual causes are alignment: a buffer crossing a cache line or page boundary, or two buffers whose addresses share the same low 12 bits (the CPU may then wrongly hold a load back behind a store to the other buffer). If the production code controls its alignment, generate the corpus with the same alignment; otherwise give the median more epochs (`--epochs=20`) and report the spread along with it.

## Comparing builds

Compare two commits: build and run the base, keep its JSONL, build and run the change, then:

```
tools/bm-compare base.jsonl new.jsonl
```

```
benchmark                                  base        new    ratio              95% CI  verdict
Plant/throughput/work                    194.99     200.67   1.0362 [ 1.0228,  1.0696]  REGRESSION +3.6%
```

For each implementation it gives the ratio new/base of the median per call and its 95% confidence interval. A change counts (`slower`, `faster`) only when the interval excludes 1.0 and the change is at least 1% (`--min-effect`). A slowdown of 3% or more (`--threshold`) is a `REGRESSION` and makes it exit 1.

- It compares **raw** cycles per call. Each run measures its own harness cost, so subtracting two of them would only add noise.
- It refuses runs that differ in CPU, core type, build type, compiler flags, profile, page size, PMU mode, DPDK version, mode, unroll or `ops_per_call` (exit 77, or 1 with `--profile=ci`).
- It refuses two runs of the same commit unless you pass `--allow-same`, which is how you check the noise on your machine (an A/A comparison should say `no change`). It warns about a `-dirty` commit.
- On a laptop, the same binary run twice can differ by 1 to 2%; trust a verdict more on a quiet machine.

### Regression checks in CI

Each benchmark has a regression test next to its `benchmark()` in `benchmarks/meson.build`, in suite `bench-regression`. It runs `bm-compare` between a committed baseline, `benchmarks/baseline/<name>.jsonl`, and the latest results:

```
meson test -C build --benchmark --test-args='--profile=ci'
meson test -C build --suite bench-regression --test-args='--profile=ci'
```

The second command fails on a regression of 3% or more. Under `--profile=ci` it also fails when the baseline or the results are missing; without it, it is skipped.

To declare it for a new benchmark, copy the `regress-crc32` test and change the name. To take or refresh a baseline, run the benchmarks on the CI runner with `--profile=ci` and commit `build/benchmarks/<name>.jsonl` as `benchmarks/baseline/<name>.jsonl`, in a commit of its own.

## Checklist

- [ ] `count` is a power of two, and the working set is the size you meant to measure.
- [ ] The corpus is generated from `seed`, with a production-like distribution.
- [ ] Every implementation reads the same corpus and returns the same results.
- [ ] The snippet returns a value computed from its input.
- [ ] Tables and allocations are in `setup`, not in the snippet.
- [ ] The build is optimized, and benchmark targets keep LTO off.
- [ ] No warnings in the run, or you know why each one is there.
- [ ] A result marked `harness_bound` is not reported as a measurement.
- [ ] In chained mode, `fn` returns a value computed from what it read.
- [ ] A walk inside one call (each result picks the next input) keeps its steps varied.
- [ ] With `ops_per_call`, the R operations inside a call form a chain in chained mode.
