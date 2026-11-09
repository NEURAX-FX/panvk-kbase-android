#!/bin/sh
# check-egl-zink-wsi.sh — require logged PanVK Vulkan WSI and matching SHM copies.
# A setup queue name alone does not identify the presentation route.
set -eu

log="${1:?usage: $0 glmark2-log}"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -s "$log" ] || fail "log missing or empty: $log"

grep -q 'GL_RENDERER:.*MESA_PANVK' "$log" ||
  fail "renderer is not PanVK: $(grep -m1 'GL_RENDERER' "$log" || echo '<none>')"
grep -q 'wsi: CreateSwapchain' "$log" ||
  fail "no Vulkan WSI swapchain creation record"
grep -q 'PANVK_WSI_SHM_DIAG' "$log" ||
  fail "no SHM readback diagnostic records"
awk '/PANVK_WSI_SHM_DIAG/ {
  if ($0 !~ /(^|[[:space:]])equal=1([[:space:]]|$)/) bad = 1
} END { exit bad }' "$log" ||
  fail "at least one SHM readback record did not report equal=1"

echo "PASS: PanVK Vulkan WSI swapchain logged; every SHM diagnostic matched"
