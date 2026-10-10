#!/bin/sh
# Private qualification launcher. This does not change any system defaults.
set -eu
root=${HAIKU_AMDGPU_TEST_ROOT:-/boot/home/wx5100/mesa-amdgpu}
prefix=$root/prefix
export LIBRARY_PATH="$prefix/lib:%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:/boot/system/non-packaged/lib:/boot/system/lib"
export __EGL_VENDOR_LIBRARY_FILENAMES="$root/50_mesa.json"
export EGL_LOG_LEVEL=debug
export MESA_SHADER_CACHE_DIR="$root/cache"
unset LIBGL_ALWAYS_SOFTWARE MESA_LOADER_DRIVER_OVERRIDE HAIKU_MALI_DEVICE
case "${1:-}" in
  --software) unset HAIKU_AMDGPU_DEVICE; export GALLIUM_DRIVER=softpipe ;;
  --hardware|--expect-no-device)
    export HAIKU_AMDGPU_DEVICE=/dev/dri/amdgpu/0
    unset GALLIUM_DRIVER
    ;;
  *) echo 'usage: run-probe.sh --software | --hardware | --expect-no-device' >&2; exit 2 ;;
esac
exec "$root/native_egl_probe" "$@"
