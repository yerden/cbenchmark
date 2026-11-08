# Glossary

Terms used in this project, its guides and our discussions, in plain words. Grouped by topic; alphabetical inside each group.

## What is measured

**Benchmark.** One `flw_bench` in a benchmark file: a name, a way to make inputs (the corpus generator) and one or more implementations of the same operation. Example: `Crc32`.

**Call.** One run of the snippet on one input. The basic unit of all results: "cycles per call".

**Corpus.** The inputs of a benchmark, made once before any timing into one flat array. The snippet only reads from it, so making inputs costs nothing during measurement.

**Corpus generator.** The benchmark's function that fills the corpus (`gen`), from a seed, so every run gets the same inputs.

**Implementation.** One way of doing the benchmark's operation: one `flw_impl`, e.g. `bitwise` or `table` for CRC. All implementations of a benchmark read the same corpus.

**Item.** One element of the corpus, `item_size` bytes. Each call gets one item.

**Mode.** How calls are linked: *throughput* or *chained* (below). Part of every result name: `Crc32/throughput/table`.

**Operation (op).** One unit of the work the user cares about. Usually one call is one operation; with `ops_per_call` a call can do several.

**Per call / per op.** Per call is what is measured. Per op is per call divided by `ops_per_call` (after subtracting the harness cost, in chained mode). Always work with per-call values first and divide last.

**Snippet.** The piece of code being measured: the function in an implementation (`fn` or `chain`).

**Working set.** All memory the snippet touches during a run: the corpus plus anything it points to. Its size decides which cache level is measured. Reported as `corpus_bytes`.

## The two modes

**Chained mode.** Each call's output is fed into the next call, so the CPU must finish one call before starting the next. Measures **latency**: how long one call takes when something waits for its result.

**Latency.** Time from a call's input being ready to its output being ready.

**Throughput.** How many calls the CPU completes per unit of time when the calls do not depend on each other. Usually much better than 1 / latency, because the CPU overlaps independent calls.

**Throughput mode.** Calls on consecutive corpus items, independent of each other. The CPU overlaps them. Measures throughput, reported as cycles per call.

### Chained mode details

**Adapter.** An optional benchmark function `next(out, item, state)` for when the previous output really picks the next input (a table walk, a parser's next offset). It turns the previous output, the next corpus item and the implementation's state into the pointer the next call reads. Pointer snippets that just need chaining use the *pointer dependency* instead.

**Collapse.** A chain stuck feeding the snippet the same few inputs again and again (for example a value that becomes 0 and stays 0). The CPU then learns the pattern and the numbers are too good. The runner checks adapters for it before timing.

**Distinct inputs.** The collapse check's measure: how many different pointers `next` returned in 65,536 untimed calls. Too few means collapse.

**Fake dependency.** A trick to chain calls without changing the inputs: the next input is `item | (out & 0)`, where the 0 is hidden from the CPU and the compiler (`flw_zero()`). The value is the corpus item, but the CPU must wait for `out`. Done by `flw_chain_dep()`.

**Pointer dependency.** The runner's chaining for snippets that take a pointer (`fn`) and have no adapter: the fake dependency applied to the corpus item's address. The snippet reads the corpus item unchanged, but the call waits for the previous one. Calibrated once per run (`ref/ptr-*`).

**Word in, word out.** The simple chained shape: the snippet takes a 64-bit number and returns one (`chain`). The runner supplies the first 8 bytes of each corpus item as input.

## Timing loop

**Batch.** N calls in a row, timed as one window. N is chosen so a batch takes about 1 ms. One batch gives one sample.

**Batch size (N).** Calls per batch, found once per implementation by doubling until a batch takes 1 ms (like Go's `b.N`), then kept fixed.

**Epoch.** A group of K batches (default 30). A run has E epochs (default 10) per implementation. Each epoch moves the corpus and the stack to new memory addresses (*layout*), regenerates the corpus from the same seed, runs `setup` again and warms up again, so results do not depend on one lucky or unlucky placement.

**Layout.** Where code and data sit in memory. Different addresses can change speed by a few percent (cache sets, alignment), which is why epochs vary it.

**Fenced window.** The start and end readings of a batch, with `lfence` instructions around them so the CPU cannot move work into or out of the window.

**Sample.** The readings of one batch: TSC ticks, core cycles, instructions, and N. Written raw to the output.

**Rejected sample.** A batch the runner caught being disturbed, marked with *flags*: `ratio` (frequency changed or an interrupt took time), `ctx` (the thread was switched out), `migrate` (it moved to another CPU). It stays in the output, but results leave it out. A high rejection rate means a noisy machine.

**Sink.** Something that forces the compiler to compute a value it would otherwise drop as unused: `flw_use(x)`. The runner sinks every snippet result.

**Timer overhead.** What an empty window (start and end readings with nothing in between) costs, about 176 core cycles. Below 0.01% of a 1 ms batch, so it is reported, not subtracted.

**Unroll (U).** How many calls the timing loop body contains before it jumps back. A build-time constant, now 1. Results at different U are not comparable.

**Warm-up.** Batches run and thrown away before measuring, so caches and the branch predictor settle. 5 batches at the start of every epoch.

## Harness cost and calibration

**a (intercept).** The harness cost of one chained call, in cycles: call, return, loop and the fake dependency (on a word or a pointer) or adapter. About 2 cycles. In chained mode it is subtracted from results. The name comes from the fitted line `cost = a + 3k`.

**Calibration.** At startup, the runner times reference snippets of known cost through the same loops as user snippets. This tells it the harness cost (`a`), the floors, and whether the counters work.

**Corrected value.** Raw value minus `a`. The headline in chained mode.

**Fit.** Drawing the best straight line through the calibration points (cost against k), starting from the largest k and adding smaller ones while they stay on the line. The points that fall off at small k are where the harness hides the snippet.

**Floor.** The lowest cost per call the harness can show. Throughput: about 5 cycles (the cost of calling a do-nothing snippet). Chained: about 8 cycles (the smallest calibration point still on the line). A snippet cheaper than the floor reads as the floor.

**Glue.** The code between two snippet calls: the call itself, loop counter, index, load, fake dependency or adapter. Its cost is the harness cost.

**Harness.** The runner's code around the snippet: the timing loop and its glue.

**Harness-bound.** A result at (throughput) or below (chained) the floor. It says only "the snippet costs at most this much"; it is not a measurement. Marked `harness_bound` in the output.

**k.** Chain length of a reference snippet: `ref/imul-chain-4` has k = 4 multiplications.

**ops_per_call (R).** Declared by an implementation that does R operations in one call. Lets a snippet cheaper than the floor be measured: the call does R times more work, so the per-call cost rises above the floor, and the tools divide by R at the end. Declare R; never divide by it yourself.

**Raw value.** What was measured, before subtracting anything.

**Reference snippets.** Snippets with a known cost, written in assembly:
- `ref/nop`: does nothing; its cost is the floor.
- `ref/imul-chain-k`: k multiplications, each waiting for the previous; costs 3k cycles (a multiply takes 3 cycles on modern x86).
- `ref/add-chain-k`: k additions in a chain; k cycles.
- `ref/imul-indep-s`: s multiplications that do not wait for each other; about s cycles (one multiply per cycle).
- `ref/ptr-nop`, `ref/ptr-imul-chain-k`: the same, called on a pointer through the pointer dependency.
- `ref/adapter-nop`, `ref/adapter-imul-chain-k`: the same, called through an implementation's adapter.

**Residual.** How far a calibration point lies from the fitted line.

**Confirmed / uncertain calibration.** Calibration computes `a` and the floors twice: from each reference snippet's fastest round, and again from its second-fastest. If both give the same answer, calibration is *confirmed*. If not, it is *uncertain*, and the warning says what is at risk: how far corrected results may be off, and which results may be wrongly marked harness-bound. Such results are marked in the console (`+- x`, `harness-bound uncertain`) and in their `bench` record (`a_alt`, `floor_alt`).

**Gap (of a calibration point).** Second-fastest round minus the fastest.

**Round.** One pass of calibration over all reference snippets. Calibration does 3 rounds; each point keeps its fastest one, because interference only makes things slower.

**Self-test.** Checks after calibration: the chained slope is 3 ± 3% (the counters measure what we expect), the cycles/TSC ratio is steady (frequency did not move), timer overhead is stable, and calibration is *confirmed* (see above). Only the slope test can fail a run (exit 1, `ci` profile only); the others warn.

**Spread (of a calibration point).** How much its 3 rounds differed: slowest minus fastest. A large spread means calibration was disturbed.

**Slope.** Cycles added per extra step of k. Should be 3 for the `imul` chain. A wrong slope means the counters, pinning or frequency control are wrong, or the core was disturbed.

**Subtract.** Whether `a` is subtracted from results (`--subtract`, default: chained mode only). Recorded per result.

**Sweep.** Running a reference snippet at several sizes (k = 1, 2, 4, 8, 16), to fit the line.

## Clocks and counters

**Core cycles.** Cycles of the CPU core itself; they speed up and slow down with the core's frequency. The main unit of results, because code needs about the same number of core cycles at any frequency.

**Instructions (instr).** How many instructions the core completed. Constant for the same code and inputs, so a changing count means the code did different work.

**Invariant TSC.** A TSC that ticks at a constant rate whatever the core frequency. Checked at startup.

**IPC.** Instructions per core cycle.

**lfence.** An x86 instruction that makes the CPU finish earlier instructions before starting later ones. Used to fence the timing window.

**PMU (performance monitoring unit).** The CPU's hardware counters (core cycles, instructions and more). Opened through Linux `perf_event_open`.

**Ratio.** Core cycles divided by TSC ticks: the core's speed relative to the TSC (1.14 on the laptop). A ratio that moves means the frequency changed or the core was interrupted.

**rdpmc.** The instruction that reads a PMU counter directly from user code, without a system call. Fast; the fallback is the slower `read()` system call.

**rdtsc / rdtscp.** Instructions that read the TSC.

**TSC (time stamp counter).** A counter that ticks at a fixed rate (2.5 GHz on the laptop), like a wall clock. Used for real time (ns) and as a fallback when the PMU is unavailable.

**TSC-only mode.** A run without PMU access: everything is in TSC ticks, frequency changes go unnoticed, and the self-tests are skipped. Works, with a warning.

## The machine

**CAP_PERFMON.** A Linux permission that allows reading the PMU without root. In Docker: `--cap-add=PERFMON`.

**Core isolation.** Telling Linux at boot (`isolcpus=`, `nohz_full=`) not to run other work on a core, so a benchmark has it alone.

**Core type.** On hybrid Intel CPUs (12th generation and later), a core is either a fast P-core (`core`) or an efficient E-core (`atom`). They need different PMU setup and give different numbers.

**Disturbance / interference.** Anything else using the core while we measure: other programs, interrupts, the SMT sibling. It only ever adds cycles. On the laptop it came in bursts of tens of milliseconds.

**Execution port.** One of the core's internal units that run instructions (e.g. the multiplier). Two threads on one core compete for them.

**Governor.** The Linux policy that sets CPU frequency (`powersave`, `performance`). `performance` keeps it high and steady.

**Hugepages.** Large memory pages (2 MiB or 1 GiB instead of 4 KiB) that DPDK prefers. Without them, add `--no-huge` to the EAL arguments. Results with and without hugepages are not compared.

**Kaby Lake.** The Intel core generation of the laptop used for testing (i5-7200U). Numbers quoted "on Kaby Lake" come from it.

**perf_event_paranoid.** A Linux setting that decides who may use the PMU. At 2 or less a normal user may; at 3 or more only privileged users (or CAP_PERFMON).

**SMT sibling.** Two hardware threads that share one physical core (Intel calls it Hyper-Threading). On the laptop cpu 1 and cpu 3 share a core. A busy sibling slows the benchmark thread, so it should be idle or offline (`echo 0 > /sys/devices/system/cpu/cpu1/online`).

**Turbo.** The CPU raising its frequency above the base rate when it can. Makes TSC-based numbers move; core cycles mostly not. The turbo frequency also drops a step when more cores become busy (*turbo bins*), which shows up as `ratio` rejections.

## DPDK

**DPDK.** The packet-processing library the target project is built on. The runner is a DPDK application, for pinning and memory.

**EAL (environment abstraction layer).** DPDK's startup part. Its arguments come before `--` on the command line, e.g. `-l 2,3 --no-huge`.

**lcore.** A DPDK worker thread pinned to one CPU. `-l 2,3` gives two: the main lcore (2) waits, the benchmark runs on lcore 3.

**Pinning.** Fixing a thread to one CPU so Linux cannot move it. EAL does it for every lcore.

## Statistics and results

**A/A comparison.** Comparing a build with itself (run twice). It should find no difference; it tests that the method is not fooled by noise.

**Baseline.** The earlier results a new run is compared against. For CI: a JSONL file taken on the CI runner and committed as `benchmarks/baseline/<name>.jsonl`.

**bench-regression.** The Meson test suite that compares the latest results of each benchmark with its baseline (`meson test --suite bench-regression`) and fails on a regression.

**Bootstrap CI.** A confidence interval computed by resampling the epoch medians many times. Says how sure we are of a result or a difference.

**Bootstrap.** Estimating how uncertain a number is by recomputing it many times (2000) from random re-draws of the data (here: the epoch medians) and looking at how much it varies.

**Confidence interval (CI).** A range that very likely contains the true value. A change counts only when its CI excludes "no change".

**Headline.** The one number reported for a result: the median of the epoch medians.

**Layout noise.** Small result changes caused by code or data landing at different memory addresses, not by the code itself. Code alignment flags and epochs reduce it.

**Layout-sensitive.** A result whose epochs disagree much more (over 3 times) than the batches inside each epoch: the memory placement changes its speed. `bm-report` marks it.

**MAD (median absolute deviation).** A robust measure of spread: the median distance of values from their median.

**Median.** The middle value. Used instead of the mean because a few disturbed batches do not move it.

**Spread.** How far apart values are; here usually (max − min) / median of the epoch medians, in percent.

**Steady state.** The pattern a short loop settles into inside the core. A loop can settle into slightly different patterns, which showed up as epochs sitting at distinct levels about 2% apart.

## Build, run and output

**benchmark() (Meson).** Meson's function that declares something to run with `meson test --benchmark`. Each of our benchmarks has one, with all its arguments written out.

**bm-report, bm-go, bm-compare.** The Python tools in `tools/` that read the JSONL files: a human summary, Go benchmark lines for `benchstat`, and the comparison of two runs with a verdict. Their statistics live in `tools/bm_stats.py`.

**Exit code.** The number the runner returns: 0 OK, 1 FAIL (a check or the snippet failed), 99 ERROR (bad arguments or setup). `bm-compare` also uses 77 (SKIP: nothing to compare). Meson shows them as OK, FAIL, SKIP or ERROR.

**Regression.** A slowdown that `bm-compare` is sure of (its CI excludes 1.0) and that is at least the threshold (3% by default).

**Significant.** A change whose CI excludes 1.0 and that is at least the minimum effect (1%).

**JSONL.** The output file: one JSON object per line, one *record* per line.

**LTO (link-time optimization).** A compiler mode that optimizes across files. Turned off for benchmarks, because it could inline the snippet into the loop and measure something else.

**Aliasing.** Two pointers that may point at the same memory. A store through a byte pointer may change anything, so the compiler must read other values back from memory after it, for example a size kept in a struct.

**Auto-vectorization.** The compiler rewriting a loop to handle 16 or 32 bytes per step with vector instructions. A "byte at a time" loop is often not one after `-O3`; `bench_memcpy.c` turns it off for its byte loop.

**Store-to-load forwarding.** When a load reads bytes that a recent store has not yet written to the cache, the CPU passes them straight from the store. It costs a few cycles (about 4 to 5 on recent Intel cores) and is part of a chain that reads back what it just wrote.

**Environment check.** At startup (and, for the SMT sibling, at the end) the runner looks at the machine: governor, turbo, sibling load, core isolation, core type, PMU, page size, build. Findings go into the `env` record; problems warn, or fail the run under the `ci` profile.

**Profile.** How strict the run is about the machine: `dev` (default) warns about problems and finishes with exit 0; `ci` also exits 1, so a badly set-up CI runner cannot pass.

**Records.** The kinds of lines in the JSONL file:
- `env`: the machine, build, commit and every warning.
- `timer`: the timer overhead.
- `calibration`: harness cost and floors, one for each mode.
- `selftest`: the self-test verdicts.
- `bench`: one per implementation and mode; its settings and calibration values.
- `sample`: one per batch, raw readings.

**Runner.** The framework code that does everything except the snippet: starting DPDK, calibrating, timing, writing output. Benchmark files call it through `flw_main()`.

**Seed.** The number the corpus generator starts from (`--seed`, default 1). Same seed, same inputs.

**test-args (Meson).** `meson test --test-args='...'` adds arguments to every benchmark run, to change e.g. the profile without editing the build file.
