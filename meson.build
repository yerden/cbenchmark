project('c_benchmark', 'c',
  version: '0.1',
  meson_version: '>=0.63',
  default_options: ['buildtype=release', 'c_std=gnu11', 'warning_level=2'])

# dependencies of the code under test; empty in this standalone tree
project_deps = []

# the tools in tools/ and their tests
python3 = find_program('python3')

subdir('benchmarks')

subdir('tools')
