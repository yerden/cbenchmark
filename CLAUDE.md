# CLAUDE.md

A DPDK-based C micro-benchmark package. Steps 1 to 10 of the guide's **Implementation order** are implemented (each step has a status line; open checks are listed under **Open items**). Next: writing benchmarks with it; `benchmarks/README.md` is the author's guide, `benchmarks/bench_crc32.c` the example.

## Start here

Read `C Benchmark Package Implementation Guide.md` before any work. It is the source of truth for the design. Its **Decision log** (at the end) records what was decided, why, and what was rejected; don't reopen those points unless the user does. The online Claude Docs copy of the guide is stale: edit the local file only, and don't call Claude Docs tools.

## Documentation

| File | For | Contents |
| --- | --- | --- |
| `C Benchmark Package Implementation Guide.md` | framework developers | the design, record formats, implementation order (step list with acceptance criteria), pitfalls, decision log, open items with measured results |
| `benchmarks/README.md` | benchmark authors | how to write, declare in Meson, build, run and read a benchmark; marks what is still planned |
| `GLOSSARY.md` | everyone | every term in plain words, grouped by topic |
| `scratchpad/README.md` | framework developers | test benchmarks (reference chains, collapse, planted slowdown) and analysis scripts from the acceptance checks, outside the build |

Keep them in step with the code: when a change affects behaviour, flags or output, update the guide (and its decision log for a design choice), the README for anything a benchmark author sees, and the glossary for any new term. Use the glossary's terms in replies, and explain plainly.

## Working rules

- **Discuss before implementing.** Don't write code until the user asks for it.
- **Build and test only in Docker.** The host has no DPDK. Ask the user for the image name before the first build or test run; never fall back to a native build.
- **Code style:** `.clang-format` is the rule (DPDK-like: hard tabs, width 8). Run `clang-format -i` on every C file you touch. clang-format is the one tool that runs natively, outside Docker.
- **Naming:** public API is snake case with the `flw_` prefix (`flw_main`, `flw_impl`); macros and enum constants use `FLW_`. Internal functions are `static`, no prefix.
- **Dependencies:** DPDK and jansson are already in the target project; add nothing else to the C side. Wrappers in `tools/` are Python with the standard library only.
