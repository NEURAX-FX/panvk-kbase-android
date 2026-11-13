#!/usr/bin/bash
# SHM vs linux-dmabuf A/B for PanVK+Zink Wayland clients under a private
# niri-anland + private Anland consumer. Does not touch the user session.
# usage: bench.sh shm|dmabuf
set -euo pipefail
umask 077
mode=$1
[[ $mode == shm || $mode == dmabuf ]] || { echo "usage: $0 shm|dmabuf" >&2; exit 2; }
H=/root/panvk-wsi-debug/stripes/private-anland.bKSBY3
root=$(cd -- "$(dirname -- "$0")" && pwd -P)
work=$(mktemp -d "$root/run-$mode.XXXXXX")
runtime=$(mktemp -d /tmp/pvk-bench.XXXXXX)
pids=()
cleanup() {
    local pid
    for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf -- "$runtime"
}
trap cleanup EXIT
cd -- "$work"
echo "RUN_DIR=$work"
mkdir home config cache
export HOME="$work/home" XDG_CONFIG_HOME="$work/config" XDG_CACHE_HOME="$work/cache"
export XDG_RUNTIME_DIR="$runtime" ANLAND=1 ANLAND_SOCKET="$runtime/anland.sock"
export ANLAND_DRM_DEVICE=/dev/dri/renderD128 NIRI_DISABLE_SYSTEM_MANAGER_NOTIFY=1
export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink VK_LOADER_DRIVERS_SELECT=panvk-kbase.json
export LIBGL_DRIVERS_PATH=/opt/panvk/current/lib64/dri GBM_BACKENDS_PATH=/opt/panvk/current/lib64/gbm
unset DISPLAY WAYLAND_DISPLAY WAYLAND_SOCKET NIRI_SOCKET NOTIFY_SOCKET NOTIFY_FD LD_PRELOAD PANVK_KBASE_DRI3
export RUST_LOG=niri=info,smithay=warn

wait_for_log() {
    local pid=$1 file=$2 text=$3 deadline=$((SECONDS+15))
    until grep -Fq -- "$text" "$file" 2>/dev/null; do
        if ! kill -0 "$pid" 2>/dev/null || (( SECONDS >= deadline )); then
            echo "ERROR waiting for '$text' in $file" >&2; tail -25 "$file" >&2; return 1
        fi
        sleep .05
    done
}

cat >bench.kdl <<'EOF'
layout {
    background-color "#202020"
    focus-ring {
        off
    }
    border {
        off
    }
}
animations {
    off
}
hotkey-overlay {
    skip-at-startup
}
xwayland-satellite {
    off
}
prefer-no-csd
EOF
/usr/bin/niri-anland validate -c "$work/bench.kdl" >validate.log 2>&1 || { cat validate.log >&2; exit 2; }

"$H/display-daemon" "$ANLAND_SOCKET" >daemon.log 2>&1 &
pids+=($!)
deadline=$((SECONDS+5))
until [[ -S $ANLAND_SOCKET ]]; do (( SECONDS < deadline )) || { cat daemon.log >&2; exit 2; }; sleep .05; done
# Old consumer: samples (and keeps running) only while ./sample exists; must
# exist within 20 s of start. 3000 samples at 120 Hz = 25 s.
"$H/consumer" "$ANLAND_SOCKET" /dev/dma_heap/system 3000 >consumer.log 2>&1 &
consumer=$!; pids+=($consumer)
wait_for_log "$consumer" consumer.log CONSUMER_READY
/usr/bin/niri-anland -c "$work/bench.kdl" >niri.log 2>&1 &
niri=$!; pids+=($niri)
wait_for_log "$niri" niri.log 'listening on Wayland socket:'
export WAYLAND_DISPLAY=$(sed -n 's/^.*listening on Wayland socket: //p' niri.log | tail -1)

client_env=(GLMARK_MODE=$mode)
[[ $mode == dmabuf ]] && client_env+=(PANVK_KBASE_DRI3=1)

# 1) Protocol check: which buffer path the client really uses.
env "${client_env[@]}" WAYLAND_DEBUG=client timeout -s INT 4 glmark2-es2-wayland --fullscreen \
    -b build:use-vbo=false:duration=2 >proto.log 2>&1 || true
echo "PROTOCOL $mode:"
grep -Eo '(wl_shm_pool|zwp_linux_dmabuf_v1|zwp_linux_buffer_params_v1|wp_linux_drm_syncobj_surface_v1)#[0-9]+\.[a-z_]+|wl_surface#[0-9]+\.attach' proto.log \
    | sed 's/#[0-9]*//' | sort | uniq -c
grep -E 'MESA|rror|ZINK' proto.log | grep -v 'kbase: \(timestamp\|coherency\|CSF driver\|tiler\|mem_alloc\|queue_group\)' | sort | uniq -c | head -12

# 2) FPS run, fullscreen 2392x1080. Consumer samples from 2 s in; first
# sampled frame is saved as reference.ppm.
( sleep 2; touch "$work/sample" ) &
sampler=$!
env "${client_env[@]}" glmark2-es2-wayland --fullscreen \
    -b build:use-vbo=false:duration=6 \
    -b texture:texture-filter=linear:duration=6 \
    -b shading:shading=phong:duration=6 >glmark2.log 2>&1 || echo "GLMARK2_EXIT=$?"
wait "$sampler"
echo "GLMARK2 $mode:"
grep -E 'OpenGL|Surface Size|FPS|Score|rror' glmark2.log
grep -E 'ERROR|RESULT' consumer.log || true
echo "CONSUMER_FRAMES=$(grep -c '^FRAME' consumer.log)"
grep -Ei 'error|warn' niri.log | grep -v 'XDG_SESSION_ID\|LockedHint\|not a conformant' | sed 's/^[^ ]* //' | sort | uniq -c | head -10
ls -1 "$work"/*.ppm 2>/dev/null || true
