#!/usr/bin/bash
# Isolated Wayland verification of a PanVK ICD under a private niri-anland +
# private Anland consumer. niri keeps the system stack; only clients change.
# usage: verify.sh <label> <icd-json|system> [extra client env...]
set -euo pipefail
umask 077
label=$1 icd=$2; shift 2
H=/root/panvk-wsi-debug/stripes/private-anland.bKSBY3
root=$(cd -- "$(dirname -- "$0")" && pwd -P)
work=$(mktemp -d "$root/v-$label.XXXXXX")
runtime=$(mktemp -d /tmp/pvk-verify.XXXXXX)
pids=()
cleanup() {
    touch "$work/stop" 2>/dev/null || true
    sleep .3
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
unset DISPLAY WAYLAND_DISPLAY WAYLAND_SOCKET NIRI_SOCKET NOTIFY_SOCKET NOTIFY_FD LD_PRELOAD PANVK_KBASE_DRI3 PANVK_WSI_WAYLAND_DMABUF VK_DRIVER_FILES
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

cat >v.kdl <<'EOF'
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
/usr/bin/niri-anland validate -c "$work/v.kdl" >validate.log 2>&1 || { cat validate.log >&2; exit 2; }
"$H/display-daemon" "$ANLAND_SOCKET" >daemon.log 2>&1 &
pids+=($!)
deadline=$((SECONDS+5))
until [[ -S $ANLAND_SOCKET ]]; do (( SECONDS < deadline )) || { cat daemon.log >&2; exit 2; }; sleep .05; done
"$root/consumer-sync" "$ANLAND_SOCKET" /dev/dma_heap/system 100000 >consumer.log 2>&1 &
consumer=$!; pids+=($consumer)
wait_for_log "$consumer" consumer.log CONSUMER_READY
/usr/bin/niri-anland -c "$work/v.kdl" >niri.log 2>&1 &
niri=$!; pids+=($niri)
wait_for_log "$niri" niri.log 'listening on Wayland socket:'
export WAYLAND_DISPLAY=$(sed -n 's/^.*listening on Wayland socket: //p' niri.log | tail -1)

client=()
if [[ $icd != system ]]; then
    client+=(VK_DRIVER_FILES=$icd VK_LOADER_DRIVERS_SELECT=$(basename "$icd"))
fi
client+=("$@")

echo "== protocol"
env "${client[@]}" WAYLAND_DEBUG=client timeout -s INT 4 glmark2-es2-wayland --fullscreen \
    -b build:use-vbo=false:duration=2 >proto.log 2>&1 || true
grep -Eo '(wl_shm_pool|zwp_linux_buffer_params_v1)#[0-9]+\.create(_buffer|_immed)|wl_surface#[0-9]+\.attach' proto.log \
    | sed 's/#[0-9]*//' | sort | uniq -c
grep -m1 GL_RENDERER proto.log || true

echo "== glmark2 fullscreen"
env "${client[@]}" glmark2-es2-wayland --fullscreen \
    -b build:use-vbo=false:duration=6 -b texture:texture-filter=linear:duration=6 \
    -b shading:shading=phong:duration=6 >glmark2.log 2>&1 || echo "GLMARK2_EXIT=$?"
grep -E 'FPS|Score|rror' glmark2.log

echo "== sync stress"
( sleep 2; touch "$work/sample" ) &
env "${client[@]}" "$root/sync-stress" ${STRESS_FRAMES:-600} ${STRESS_ITERS:-200} >stress.log 2>&1 || echo "STRESS_EXIT=$?"
wait $!
rm -f "$work/sample"
cat stress.log | grep -E 'STRESS|rror'
touch "$work/stop"
for _ in $(seq 50); do grep -q RESULT consumer.log && break; sleep .1; done
grep -E 'BAD|RESULT|ERROR' consumer.log | tail -6
grep -Ei ' (error|warn)' niri.log | grep -v 'XDG_SESSION_ID\|LockedHint\|not a conformant' | sed 's/^[^ ]* //' | sort | uniq -c | head -8 || true
exit 0
