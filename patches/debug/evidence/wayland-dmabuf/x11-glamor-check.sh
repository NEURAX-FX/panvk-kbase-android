#!/usr/bin/bash
# X11 no-regression on the user's real X server: xlibre Xvfb with glamor
# (DRI3 + Present), which itself renders through Zink/PanVK.
# Config "old": server+clients on the deployed ICD; "new": both on the candidate.
set -uo pipefail
NEW=/root/panvk-wsi-debug/icd-wldmabuf2/panvk-kbase-wldmabuf.json
XVFB=/opt/xlibre/25.2.3/bin/Xvfb
out=/root/panvk-wsi-debug/vgem-probe/x11g
mkdir -p "$out"
export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink
unset WAYLAND_DISPLAY VK_DRIVER_FILES PANVK_KBASE_DRI3 PANVK_WSI_WAYLAND_DMABUF
export XDG_RUNTIME_DIR=$(mktemp -d /tmp/x11g.XXXXXX)
for v in ${ORDER:-old new}; do
    if [[ $v == new ]]; then
        export VK_DRIVER_FILES=$NEW VK_LOADER_DRIVERS_SELECT=panvk-kbase-wldmabuf.json
    else
        unset VK_DRIVER_FILES; export VK_LOADER_DRIVERS_SELECT=panvk-kbase.json
    fi
    echo "===== $v (server + clients)"
    $XVFB :57 -screen 0 1920x1080x24 -glamor -nolisten tcp -verbose 3 >"$out/xvfb-$v.log" 2>&1 &
    x=$!
    for _ in $(seq 100); do xdpyinfo -display :57 >/dev/null 2>&1 && break; sleep .1; done
    export DISPLAY=:57
    grep -E 'glamor\(0\)' "$out/xvfb-$v.log" | grep -v 'eglInitialize' | head -2
    xdpyinfo | grep -oE 'DRI3|Present|MIT-SHM' | sort -u | tr '\n' ' '; echo
    ( sleep 4; import -window root "$out/glmark2-$v.png" ) &
    MESA_VK_WSI_CREATE_LOG=1 timeout 30 glmark2-es2 --size 640x360 \
        -b build:use-vbo=false:duration=8 >"$out/glmark2-$v.log" 2>&1
    wait $!
    grep -E 'GL_RENDERER|FPS|Score|sw readback|rror' "$out/glmark2-$v.log" | sort -u
    timeout 12 glxgears -info >"$out/glxgears-$v.log" 2>&1
    grep -E 'GL_RENDERER|frames in' "$out/glxgears-$v.log" | head -3
    timeout 15 vkcube --c 300 >"$out/vkcube-$v.log" 2>&1; echo "vkcube exit=$?"
    grep -iE 'error|fail|selected' "$out/vkcube-$v.log" | head -3
    python3 - "$out/glmark2-$v.png" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
px = im.get_flattened_data() if hasattr(im, 'get_flattened_data') else im.getdata()
lit = sum(1 for p in px if sum(p) > 60)
print(f"capture {im.size} lit_pixels={lit}")
EOF
    grep -ciE '\(EE\)|error' "$out/xvfb-$v.log" | sed 's/^/xvfb errors: /'
    kill $x; wait $x 2>/dev/null
done
