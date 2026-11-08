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

bm_memcpy = executable('bm_memcpy', 'bench_memcpy.c',
  dependencies: [bm_dep, project_deps],
  override_options: ['b_lto=false'],
  build_by_default: false)

benchmark('memcpy', bm_memcpy,
  args: ['-l', '2,3', '--no-huge', '--no-pci', '--no-telemetry',
         '--',
         '--out=' + meson.current_build_dir() / 'memcpy.jsonl',
         '--profile=dev',
         '--seed=1'],
  suite: 'bench',
  timeout: 1800,
  protocol: 'exitcode')

# Regression checks against the baselines in baseline/, taken on the CI
# runner. Run them after the benchmarks, as a separate command:
#   meson test -C build --benchmark --test-args='--profile=ci'
#   meson test -C build --suite bench-regression --test-args='--profile=ci'
# Without a baseline or results the check is skipped (exit 77) under dev.
bm_compare = files('../tools/bm-compare')

test('regress-crc32', python3,
  args: [bm_compare,
         meson.current_source_dir() / 'baseline' / 'crc32.jsonl',
         meson.current_build_dir() / 'crc32.jsonl',
         '--threshold=3'],
  env: ['PYTHONDONTWRITEBYTECODE=1'],
  suite: 'bench-regression',
  is_parallel: false)

test('regress-memcpy', python3,
  args: [bm_compare,
         meson.current_source_dir() / 'baseline' / 'memcpy.jsonl',
         meson.current_build_dir() / 'memcpy.jsonl',
         '--threshold=3'],
  env: ['PYTHONDONTWRITEBYTECODE=1'],
  suite: 'bench-regression',
  is_parallel: false)
