# Scratchpad

Test benchmarks and analysis scripts used while implementing steps 4 to 10. Not part of the build (no `meson.build` entry); kept as examples and for re-running the acceptance checks.

| File | What it shows |
| --- | --- |
| `bench_ref.c` | Throughput mode: `nop` (lands on the floor, `harness-bound`) and `imul-indep-s` for s = 4, 16, 32 (about 1.15 to 1.3 cycles per `imul`) |
| `bench_chain.c` | Chained mode: word in/out `imul` chains k = 1 to 16 (3k after subtracting `a`), `ops_per_call = 8`, two chains on a pointer (pointer dependency), an adapter that walks the corpus through `state`, and an adapter that collapses (fails the run, exit 1) |
| `bench_plant.c` | A snippet whose work is set at build time with `-DWORK=N`; build with 100 and 105 for a planted slowdown that `bm-compare` must flag |
| `epochs.py` | Per implementation: median, spread of epoch medians, within-epoch MAD (all samples, also rejected ones) |
| `permed.py` | Per-epoch medians and cycles/TSC ratios, side by side |

Build one (in Docker, after `meson compile -C build benchmarks/bm_crc32` has built `libbm.a`):

```
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp --cap-add=PERFMON -v "$PWD":/src -w /src e92e702e5027 sh -c '
  cc -O2 -Wall -falign-functions=64 -falign-loops=32 -Ibenchmarks scratchpad/bench_chain.c \
     build/benchmarks/libbm.a $(pkg-config --cflags --libs libdpdk jansson) -o build/bm_chain &&
  build/bm_chain -l 2,3 --no-huge --no-pci --no-telemetry -- --out=build/chain.jsonl'
```

For `bench_plant.c` add `-DWORK=100` (or 105). Then read the results with `tools/bm-report build/chain.jsonl`, or `python3 scratchpad/epochs.py build/chain.jsonl`.

Planted slowdown:

```
# build bm_plant100 and bm_plant105 as above, run each with --out=..., then
tools/bm-compare plant100.jsonl plant105.jsonl --allow-same    # REGRESSION, exit 1
```
