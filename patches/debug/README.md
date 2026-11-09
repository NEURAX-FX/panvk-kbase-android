# Wayland SHM WSI black-window diagnostics

This is an **opt-in development-only patch**, not part of the release series.
It is for the Mesa 26.3.0-devel source pinned in `sources.lock` **after**
applying the project patch stack through `jm-v7-wip/0175`.

The patch instruments `wsi_wl_swapchain_queue_present()` when the active
Wayland present path is `WSI_WL_BUFFER_SHM_MEMCPY`. It runs *after* the
existing cache invalidation and SHM `memcpy`, so it does not change ordering
or fix any synchronization bug.

Use from the project root on an already patched Mesa checkout:

```bash
git -C "$MESA" apply --check patches/debug/0001-wayland-shm-readback-diagnostics.patch
git -C "$MESA" apply patches/debug/0001-wayland-shm-readback-diagnostics.patch
```

Build the **ARM64 glibc Wayland ICD**, not the stock Android/Bionic build
(`scripts/build-android.sh` defaults to `-Dplatforms=android,x11`, so it
cannot reproduce this Wayland issue). Rebuild and install the debug ICD
without overwriting the known-good system driver.

Then reproduce with the *new* ICD:

```bash
PANVK_WSI_SHM_DIAG=1 glmark2-es2-wayland -b build:use-vbo=false --size=320x240 2>&1 | tee /tmp/panvk-shm-diag.log
```

Inspect at most eight `PANVK_WSI_SHM_DIAG` lines. They include hashes,
nonzero-byte counts, first pixel, pitch, noncoherent state and equality
of source (`cpu_map`) and destination (`shm_ptr`) **at the moment of copy**.

- `src_nonzero=0`: source is all zeros at sampling time; inspect WSI
  GPU-to-CPU blit completion, barriers and mapped memory handling.
- `src_nonzero>0`, `equal=1`, black on screen: memcpy succeeded; inspect
  Wayland SHM contents at commit, buffer lifetime and compositor sampling.
- `equal=0`: unexpected copy mismatch; investigate size/stride/mapping.

A nonzero source does **not** by itself prove it contains the expected GL
image. Compare hashes across frames and capture pixels to validate image
contents. Zero hashes are not equivalent to zero data; use nonzero count.

Note: `glmark2 --validate` checks the GL readback path, not these WSI SHM
bytes. The observed `wl_shm_pool.create_buffer` and repeated
`wl_surface.attach(wl_buffer)` calls confirm software SHM presentation in
the existing reproduction; merely advertising dmabuf v3 is not evidence
of dmabuf submission.

## Reproducible AArch64 glibc cross-build

Run on the Fedora x86_64 build host. Install an AArch64 cross compiler and
populate a separate Fedora ARM64 sysroot matching the target Fedora major
release. For a Fedora 45 target, the target development packages include:

```bash
sudo dnf --forcearch=aarch64 --installroot="$SYSROOT" --releasever=45 \
  --setopt=install_weak_deps=False install \
  glibc-devel libstdc++ libstdc++-devel libdrm-devel \
  wayland-devel wayland-protocols-devel systemd-libs systemd-devel \
  libxcb-devel libX11-devel libX11-xcb-devel libxshmfence-devel \
  xcb-util-keysyms-devel
```

`libstdc++-devel` is required in the *target* sysroot: Fedora's x86_64-hosted
cross-G++ package does not ship AArch64 C++ headers. `systemd-devel` and
`systemd-libs` provide the AArch64 `libudev` header, pkg-config file, and runtime.
The script rejects missing target pkg-config dependencies and checks the Meson
machine, C/C++ sysroot flags, and generated Wayland WSI object before Ninja.
Build Mesa's host-native code generators separately; `HOST_TOOLS` must point at
those x86_64 tools. Parallelism defaults to six jobs to keep the 12-core build
host responsive.

```bash
MESA=/home/neurax/panvk-wsi-debug/mesa
SYSROOT=/home/neurax/panvk-wsi-debug/cross/sysroot-aarch64
HOST_TOOLS=/home/neurax/panvk-wsi-debug/host-tools/bin
CXX_LINK_DIR=/home/neurax/panvk-wsi-debug/cross/target-overlay/usr/lib/gcc/aarch64-redhat-linux/16
LD_LIBRARY_PATH=/home/neurax/panvk-wsi-debug/host-deps/usr/lib64
export MESA SYSROOT HOST_TOOLS CXX_LINK_DIR LD_LIBRARY_PATH

scripts/build-glibc-cross.sh \
  --mesa "$MESA" \
  --sysroot "$SYSROOT" \
  --host-tools "$HOST_TOOLS" \
  --build-dir /home/neurax/panvk-wsi-debug/build/cross-panvk-wayland-final \
  --out /home/neurax/panvk-wsi-debug/artifacts/panvk-wsi-debug \
  --jobs 6
```

Add `--with-egl-zink` to build the Wayland EGL/GLES2 runtime that black-screens
instead of the Vulkan-only ICD. It configures
`-Dplatforms=x11,wayland -Dgallium-drivers=zink,softpipe -Degl=enabled
-Dgles2=enabled -Dopengl=false -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled`
on the same cross file, then verifies every produced AArch64 ELF and stages
`meson install` output:

```bash
scripts/build-glibc-cross.sh \
  --mesa "$MESA" \
  --sysroot "$SYSROOT" \
  --host-tools "$HOST_TOOLS" \
  --build-dir /home/neurax/panvk-wsi-debug/build/cross-egl-zink-fix \
  --out /home/neurax/panvk-wsi-debug/artifacts/egl-zink-fix \
  --jobs 6 \
  --with-egl-zink
```

`softpipe` is required even though it renders nothing here: it produces
`swrast_dri.so`, the software DRI frontend that unpatched EGL falls back to
when kbase exposes no DRM render node. Without it the control run fails at
`eglInitialize` with `Could not initialize canvas` instead of producing the
black window. The EGL profile also needs `xrandr`, `xext`, and `xrender`
development metadata in the sysroot.

The output directory contains `libvulkan_panfrost.so`,
`panvk-kbase-debug.json` (relative library path), and the recorded dynamic
section. The build does not install into `/usr` and never touches the working
system ICD. Copy that directory to a separate location on the MT6989 container,
for example `/root/panvk-wsi-debug/icd/`, keeping the library beside the JSON.

```bash
env -u DISPLAY \
  XDG_RUNTIME_DIR=/run/user/0 \
  WAYLAND_DISPLAY=wayland-1 \
  VK_DRIVER_FILES=/root/panvk-wsi-debug/icd/panvk-kbase-debug.json \
  VK_LOADER_DRIVERS_SELECT=panvk-kbase-debug.json \
  MESA_LOADER_DRIVER_OVERRIDE=zink \
  GALLIUM_DRIVER=zink \
  MESA_VK_WSI_CREATE_LOG=1 \
  PANVK_WSI_SHM_DIAG=1 \
  glmark2-es2-wayland -b build:use-vbo=false --size=320x240 \
  2>&1 | tee /root/panvk-wsi-debug/logs/panvk-wsi-shm.log
```

Keep the renderer string, WSI create log, up to eight SHM diagnostic records,
and a compositor/output capture of the same run. `GL_RENDERER` must name
`zink Vulkan ... MESA_PANVK`; process exit status and GL `--validate` alone do
not establish visible Wayland output.

## Deploying the patched EGL runtime

Copy the staged tree to the target without touching `/usr`:

```bash
# on the target, from the staged tarball
mkdir -p /root/panvk-wsi-debug/egl
tar -xzf egl-zink-fix.tar.gz -C /root/panvk-wsi-debug/egl
```

The run must select both the staged EGL stack and the debug ICD by path.
`glmark2-es2-wayland` does not link `libEGL` directly; it `dlopen()`s
`libEGL.so`, so `LD_LIBRARY_PATH` decides which EGL implementation is used:

```bash
base=/root/panvk-wsi-debug
libs=$base/egl/usr/lib64
env -u DISPLAY \
  XDG_RUNTIME_DIR=/run/user/0 \
  WAYLAND_DISPLAY=wayland-1 \
  LD_LIBRARY_PATH=$libs \
  LIBGL_DRIVERS_PATH=$libs/dri \
  VK_DRIVER_FILES=$base/icd/panvk-kbase-debug.json \
  VK_LOADER_DRIVERS_SELECT=panvk-kbase-debug.json \
  MESA_LOADER_DRIVER_OVERRIDE=zink \
  GALLIUM_DRIVER=zink \
  LIBGL_ALWAYS_SOFTWARE=0 \
  LIBGL_KOPPER_DISABLE=0 \
  MESA_VK_WSI_CREATE_LOG=1 \
  PANVK_WSI_SHM_DIAG=1 \
  glmark2-es2-wayland -b build:use-vbo=false --size=320x240 \
  2>&1 | tee $base/logs/glmark2-patched-egl.log
```

## A/B control for the EGL patch

The control uses the same Mesa tree, the same sysroot, and the same
`zink_dri.so`; only the `0002` hunk differs. Reverse just that patch, relink
`libEGL`, save the result, then re-apply:

```bash
P=$PWD/patches/debug/0002-zink-kopper-egl-fallback.patch
git -C "$MESA" apply --reverse "$P"
ninja -C "$BUILD" src/egl/libEGL.so.1.0.0
cp "$BUILD/src/egl/libEGL.so.1.0.0" "$OUT/libEGL-control.so.1.0.0"
git -C "$MESA" apply "$P"
ninja -C "$BUILD" src/egl/libEGL.so.1.0.0
cmp -s "$OUT/libEGL-control.so.1.0.0" "$BUILD/src/egl/libEGL.so.1.0.0" &&
  echo "control and patched binaries are identical (bad A/B)"
```

Install each variant in its own directory with a `libEGL.so` symlink, because
that is the name `glmark2` opens:

```bash
for v in control patched; do
  install -d "$base/egl-ab/$v"
  install -m755 "$OUT/libEGL-$v.so.1.0.0" "$base/egl-ab/$v/libEGL.so.1.0.0"
  ln -sf libEGL.so.1.0.0 "$base/egl-ab/$v/libEGL.so.1"
  ln -sf libEGL.so.1      "$base/egl-ab/$v/libEGL.so"
done
```

Run both with `LD_LIBRARY_PATH=$base/egl-ab/$v:$libs` and capture the window
with `niri msg action screenshot-window`. Analyse the capture rather than
eyeballing it:

```python
from PIL import Image
im = Image.open(path).convert("RGB")
px = im.load()
sampled = [px[x, y] for y in range(0, im.height, 2) for x in range(0, im.width, 2)]
black = sum(1 for p in sampled if p == (0, 0, 0))
print("black_ratio", black / len(sampled))
print("distinct_colors", len(set(sampled)))
```

A black window has `black_ratio=1.0` and `distinct_colors=1`.

## Diagnosis measured on MT6989

The Vulkan WSI diagnostic and an explicit color probe separate rendering/readback from EGL's fallback routing. On the patched ICD, `glmark2` reported `zink Vulkan ... MESA_PANVK` and created a Vulkan Wayland swapchain. Its eight `PANVK_WSI_SHM_DIAG` records were all `equal=1`; at 320x240, pitch 1280 and 307200 bytes, `src_nonzero` ranged from 101838 to 101895. This proves the sampled CPU readback bytes matched the SHM copy, but those counts alone do not prove expected pixel colors.

The color probe supplied that check. Its GL readback reported bottom-left red, bottom-right green, top-left blue and top-right yellow. Each captured Wayland window contained exactly those four colors, 19200 pixels per quadrant. The four WSI diagnostic records had identical source/destination hashes and the expected red first pixel. This verifies GPU rendering → WSI readback → SHM copy → compositor-visible pixels on MT6989. Evidence is under `/root/panvk-wsi-debug/evidence/color-20261108-103644/`.

The matched EGL A/B used the same Zink/PanVK renderer and ICD. The control selected `mesa egl swrast display queue`, produced no Vulkan `CreateSwapchain` or PanVK SHM records, and its captured window was fully black. The patched EGL selected Kopper/PanVK WSI; its capture contained non-black rendered content, with eight matching SHM diagnostics and a glmark2 score of 336. A test checker is provided at `patches/debug/check-egl-zink-wsi.sh`.

The failure is in EGL fallback state, not PanVK cache synchronization. `eglInitialize()` initializes `Options.Zink` from `MESA_LOADER_DRIVER_OVERRIDE`, but the fallback path cleared that flag after the hardware initializer failed. `GALLIUM_DRIVER=zink` still chose Zink, while the cleared EGL option selected the non-Kopper software Wayland frontend. In that mode, Zink's `zink_flush_frontbuffer()` returns without presenting resources that are not Kopper swapchain images. The control therefore has no PanVK Vulkan WSI submission. The observed software SHM protocol traffic by itself does not establish the contents of those buffers.

`0002-zink-kopper-egl-fallback.patch` preserves an explicit Zink selection from either environment variable when EGL falls back. That keeps the Kopper loader path active, so Zink creates and presents a PanVK Wayland Vulkan swapchain. It does not alter PanVK memory synchronization, disable validation, or select llvmpipe.

## Separate Noctalia stale-surface failure

The original GDB/Wayland trace shows a separate client/surface-lifetime error. Noctalia destroyed `wl_surface#221`; the compositor then sent `delete_id(221)`. The client reused ID 221 for a `wl_callback` requested by `wl_surface#265.frame`, after which Mesa's Vulkan display queue sent `wl_surface#221.attach`. The compositor rejected that request as `invalid method 1, object wl_callback#221`. Therefore the protocol allocator did not duplicate a still-live ID: Mesa submitted a stale surface request after Noctalia had destroyed that surface and the ID had been reused. The resulting display failure is downstream of that protocol error.

The trace also records a 2392x1080 refresh change immediately before surface teardown and an Anland consumer-session generation change at 19:09:33.251, about 0.66 seconds after the rejected request. This is temporal correlation, not proof that the output/session change caused the stale request. The trace does not include EGLSurface/VkSurface destruction calls or the relevant proxy addresses, so it cannot distinguish premature native-surface destruction by Noctalia from a retained Zink/PanVK surface past EGL teardown. No speculative lifetime workaround is included.

The separate `noctalia-patched-live.log` records `failed to flush Wayland display before poll: display_error=22`; its log is from a different run and contains `invalid object 418` / failed swapchain messages beforehand. Available evidence does not correlate that EINVAL to the callback-221 run or prove a consumer-session cause. Further source-level lifecycle instrumentation plus a controlled reproduction is required before a second code fix.

The 120-frame numeric screen scan found no blue-to-gray pollution in idle groups 0–3 or notification groups 4–7. It found one isolated pixel in panel group 8. Groups 10–11 reported a stable 90566-pixel region after the panel-close phase; because this is a persistent post-transition region, not transient contamination during idle/notifications, it is not counted as a confirmed shell flash. Raw frames remain on the target in `/root/panvk-wsi-debug/evidence/cv10-20261108-104441/`; they were analyzed numerically without viewing screenshots.

Kitty's saved launch log reports a missing portal Settings method and `No directories to watch provided`, not a GL renderer or EGL failure. The compositor/socket was no longer running during the resumed inspection, so Kitty's desktop-GL rendering could not be revalidated without restarting the user's session.
