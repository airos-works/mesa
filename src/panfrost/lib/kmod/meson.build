# Copyright © 2023 Collabora
# SPDX-License-Identifier: MIT

libpankmod_lib_files = files(
  'pan_kmod.c',

)
if with_platform_haiku
  libpankmod_lib_files += files('haiku_kmod.c', 'haiku_csf_native.cpp', 'haiku_csf_sync.cpp')
else
  libpankmod_lib_files += files('panfrost_kmod.c', 'panthor_kmod.c')
endif

libpankmod_lib = static_library(
  'pankmod_lib',
  [libpankmod_lib_files],
  include_directories : [inc_include, inc_src, inc_panfrost, include_directories('haiku-abi')],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  dependencies: [dep_libdrm, idep_mesautil, idep_pan_packers, libpanfrost_model_dep],
  build_by_default : false,
)

libpankmod_dep = declare_dependency(
  include_directories: [inc_include, inc_src],
  dependencies: [dep_libdrm],
)
