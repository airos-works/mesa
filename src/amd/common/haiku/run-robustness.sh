#!/bin/sh
set -eu
root=${HAIKU_AMDGPU_TEST_ROOT:-/boot/home/wx5100/mesa-amdgpu}
export LIBRARY_PATH="$root/prefix/lib:%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:/boot/system/non-packaged/lib:/boot/system/lib"
export __EGL_VENDOR_LIBRARY_FILENAMES="$root/50_mesa.json"
export MESA_SHADER_CACHE_DIR="$root/cache"
unset LIBGL_ALWAYS_SOFTWARE MESA_LOADER_DRIVER_OVERRIDE HAIKU_MALI_DEVICE HAIKU_CSF_DEVICE
case "${1:-}" in
 --software) unset HAIKU_AMDGPU_DEVICE; export GALLIUM_DRIVER=softpipe ;;
 --expect-no-device|--baseline|--manual|--auto) unset GALLIUM_DRIVER; export HAIKU_AMDGPU_DEVICE=/dev/dri/amdgpu/0 ;;
 *) echo 'usage: run-robustness.sh --software|--expect-no-device|--baseline|--manual|--auto' >&2; exit 2 ;;
esac
exec "$root/native_robustness_probe" "$@"
