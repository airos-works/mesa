# Copyright © 2017 Dylan Baker
# SPDX-License-Identifier: MIT
#
# The Zink renderer add-on: the same Haiku GL renderer as the software one,
# but rendering through Vulkan.

libzinkpipe = shared_library(
  'zinkpipe',
  files('ZinkRenderer.cpp', 'GalliumContext.cpp'),
  include_directories : [
    inc_include, inc_src, inc_mapi, inc_mesa, inc_gallium, inc_gallium_aux,
    inc_util, inc_haikugl, inc_gallium_drivers, inc_gallium_winsys,
    include_directories('../../frontends/hgl'),
    include_directories('/boot/system/develop/headers/private')
  ],
  gnu_symbol_visibility : 'hidden',
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  link_with : [
    libglapi, libswhgl, libsthgl, libcompiler,
    libmesa, libglsl, libgallium, libgl
  ],
  dependencies : [
    driver_zink, driver_swrast, cpp.find_library('be'),
    cpp.find_library('translation'), cpp.find_library('network'),
    dep_unwind, idep_mesautil, idep_nir, idep_xmlconfig,
  ],
  install : true,
)
