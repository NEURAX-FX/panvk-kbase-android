#!/bin/sh
# capture-window-ab.sh — run glmark2-es2-wayland against one EGL variant,
# screenshot the focused window through niri, and report pixel statistics.
# Usage: capture-window-ab.sh EGL_LIB_DIR LOG OUT_PNG [EXTRA_ENV...]
#   EGL_LIB_DIR  directory holding libEGL.so plus the staged Mesa lib64 dir
#   LOG          glmark2 log destination
#   OUT_PNG      window capture destination (requires niri + PIL)
set -eu

egl_dir="${1:?usage: $0 EGL_LIB_DIR LOG OUT_PNG [NAME=VALUE...]}"
log="${2:?usage: $0 EGL_LIB_DIR LOG OUT_PNG [NAME=VALUE...]}"
out="${3:?usage: $0 EGL_LIB_DIR LOG OUT_PNG [NAME=VALUE...]}"
shift 3

base="${PANVK_WSI_BASE:-/root/panvk-wsi-debug}"
libs="$egl_dir:$base/egl/usr/lib64"
shots="${NIRI_SHOTS:-/root/Pictures/Screenshots}"
socket="${NIRI_SOCKET:-/run/user/0/niri.wayland-1.13969.sock}"

mkdir -p "$(dirname "$log")" "$(dirname "$out")"
: >"$log"

env -u DISPLAY \
  XDG_RUNTIME_DIR=/run/user/0 \
  WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}" \
  LD_LIBRARY_PATH="$libs" \
  LIBGL_DRIVERS_PATH="$base/egl/usr/lib64/dri" \
  VK_DRIVER_FILES="$base/icd/panvk-kbase-debug.json" \
  VK_LOADER_DRIVERS_SELECT=panvk-kbase-debug.json \
  MESA_LOADER_DRIVER_OVERRIDE=zink \
  GALLIUM_DRIVER=zink \
  LIBGL_ALWAYS_SOFTWARE=0 \
  LIBGL_KOPPER_DISABLE=0 \
  MESA_VK_WSI_CREATE_LOG=1 \
  PANVK_WSI_SHM_DIAG=1 \
  "$@" \
  glmark2-es2-wayland -b build:use-vbo=false --size=320x240 --run-forever >>"$log" 2>&1 &
pid=$!

n=0
while [ "$n" -lt 300 ]; do
  grep -q 'GL_RENDERER' "$log" && break
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.1
  n=$((n + 1))
done
grep -q 'GL_RENDERER' "$log" || {
  echo "FAIL: no renderer line in $log" >&2
  kill "$pid" 2>/dev/null || true
  exit 2
}

sleep 3
NIRI_SOCKET="$socket" niri msg action screenshot-window
sleep 1
kill -INT "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

newest="$(ls -t "$shots"/*.png 2>/dev/null | head -1)"
[ -n "$newest" ] || { echo "FAIL: no screenshot in $shots" >&2; exit 3; }
cp "$newest" "$out"

python3 - "$out" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
sampled = [px[x, y] for y in range(0, im.height, 2) for x in range(0, im.width, 2)]
black = sum(1 for pixel in sampled if pixel == (0, 0, 0))
print("capture=%s size=%dx%d" % (sys.argv[1], im.width, im.height))
print("black_ratio=%.4f" % (black / len(sampled)))
print("distinct_colors=%d" % len(set(sampled)))
PY

printf 'renderer=%s\n' "$(grep -m1 'GL_RENDERER' "$log" | sed 's/^ *//')"
printf 'create_swapchain_lines=%s\n' "$(grep -c 'CreateSwapchain' "$log" || true)"
printf 'diag_lines=%s\n' "$(grep -c 'PANVK_WSI_SHM_DIAG' "$log" || true)"
printf 'swrast_queue_lines=%s\n' "$(grep -c 'mesa egl swrast display queue' "$log" || true)"
