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

On the build host, package the `stage/` tree produced by `--with-egl-zink`, then
copy the archive to the target through the available device-transfer path:

```bash
tar -czf /tmp/egl-zink-fix.tar.gz \
  -C /home/neurax/panvk-wsi-debug/artifacts/egl-zink-fix/stage .
```

On the target, unpack it without touching `/usr`:

```bash
mkdir -p /root/panvk-wsi-debug/egl
tar -xzf /tmp/egl-zink-fix.tar.gz -C /root/panvk-wsi-debug/egl
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

The original GDB/Wayland trace captures a stale-surface request. Noctalia sends `wl_surface#221.destroy()`; the server returns `delete_id(221)`, and libwayland then reuses ID 221 for the callback created by `wl_surface#265.frame`. Mesa's Vulkan WSI subsequently sends `attach` to that ID, so the compositor rejects the surface opcode on a `wl_callback`. The allocator is behaving correctly; the remaining question is why Mesa still has a present to submit after the native surface's destroy request.

In the callback-221 run, Niri logs a refresh-mode change at 19:09:32.585949, the rejected `wl_surface#221.attach` at 19:09:32.590203, then consumer-session generation 47 at 19:09:33.251451. The generation change follows the protocol error; the preceding output change may trigger surface recreation, but timestamps alone do not prove that. Static source tracing shows Zink disables its asynchronous-present path for `KOPPER_WAYLAND` and drains its flush queue before synchronous presentation when threaded submission is active. That rules out the optional Zink background-present path, not every EGL/Zink lifetime race.

The EINVAL run has a paired Anland log (`noctalia-patched-live.log` local time is UTC+8; `niri-anland-gpu.log` is UTC). Niri reports 1080x2392@120Hz at 02:21:31.373491Z, then @60Hz at 02:21:32.085336Z. Within about 1 ms, Noctalia logs `invalid object 216`; `display_error=22` follows at 10:21:32.155, about 70 ms after the mode change. Niri also reports client communication failure for Noctalia PID 14936. Consumer-session generations 18 and 19 occur later, at 02:21:36.546815Z and 02:21:38.746547Z. Thus the observed EINVAL belongs to an output-transition/protocol-error window, not the later logged session generations; this temporal alignment does not identify which surface teardown is wrong. The fatal log shows Noctalia exits, so the shell did not remain running in this capture. The trace still lacks EGL/Vulkan destroy call order and proxy addresses; no PanVK lifetime workaround is justified.

The 120-frame numeric screen scan found no blue-to-gray pollution in idle groups 0–3 or notification groups 4–7. It found one isolated pixel in panel group 8. Groups 10–11 reported a stable 90566-pixel region after the panel-close phase; because this is a persistent post-transition region, not transient contamination during idle/notifications, it is not counted as a confirmed shell flash. Raw frames remain on the target in `/root/panvk-wsi-debug/evidence/cv10-20261108-104441/`; they were analyzed numerically without viewing screenshots.

Kitty's saved log records OpenGL context creation and a desktop OpenGL 4.6 Mesa 26.3.0-devel version string, alongside PanVK/kbase initialization. It also reports a missing portal Settings method and `No directories to watch provided`. The log has no `GL_RENDERER` line or Kitty pixel capture, so it verifies context creation but not visible terminal drawing. The compositor was unavailable for a fresh capture during resumed inspection.

## Wayland displaytarget lifetime fix (2026-11-10)

`0003-zink-wayland-displaytarget-lifetime.patch` is a separate, opt-in Zink/DRI
fix for the stale native surface, not an ICD cache-coherency change. Apply it
to the same p0175 Mesa tree after the EGL routing patch. It changes
`libgallium-26.3.0-devel.so`; replacing only `libvulkan_panfrost.so` cannot
activate this fix.

### Root cause and scope

Kopper caches displaytargets by the `wl_surface *` address. Destroying the DRI
drawable drops its texture references, but state-tracker framebuffers and Zink
batches may retain the resource object and therefore its displaytarget. The
old cache entry remains until the last resource reference is released. If
libwayland allocates a new surface at the same address first, Kopper returns
the old displaytarget and Vulkan WSI presents through the old surface wrapper.
Wayland object-ID reuse itself is legal; the stale cache association is not.

The isolated hardware reproduction used native pointer `0x1284dfc0` first for
ID 6, then ID 22. The second submission still addressed destroyed ID 6:
`invalid object 6`, `Wayland=22`, one consumed frame. Both diagnostic SHM
copies matched. Moving state-tracker framebuffer purging before validation
still failed at the second frame; that experiment was reverted.

The fix removes the Wayland native-key cache entry when its DRI drawable is
destroyed, without destroying or waiting on GPU resources at that point.
The existing resource reference counts still control swapchain destruction.
Delayed destruction removes a cache entry only if it belongs to the exact
displaytarget being destroyed; it must not destroy a replacement registered
under the same native address. Ordinary `MakeCurrent` unbind/rebind does not
destroy the drawable or remove its entry. X11 and Win32 drawable teardown are
unchanged.

The isolated consumer implements real Wayland SHM buffer consumption and
checks every pixel of each 64x64 red/green/blue frame. It is not niri and does
not exercise Android scanout, dma-buf, explicit synchronization, or optional
Wayland surface extensions. This is a debug-branch fix with measured SHM
coverage, not a claim of validation across those other WSI paths. No live
Noctalia/niri restart, panel interaction, or system-driver replacement was
performed for this follow-up. The original callback-221 trace has the same
stale-wrapper symptom; a full Noctalia session with this fix remains unverified.

### Measured results on MT6989

All passing runs report `zink Vulkan 1.4(Mali-G720-Immortalis MC12 (MESA_PANVK))`.
`bad=0` means the consumer checked all 4096 pixels in every consumed frame,
not merely the GL readback, a nonzero buffer, or a process exit code.

| Library / scenario | Consumed frames | Bad frames | Repeated native address | Result |
|---|---:|---:|---:|---|
| Original, native surface recreation | 1 | 0 | 1 | Protocol error on second frame |
| Purge-order experiment, native recreation | 1 | 0 | 1 | Same error; experiment reverted |
| Fixed, native surface recreation | 128 | 0 | 84 | Pass |
| Fixed, EGLSurface recreation on one native surface | 128 | 0 | 127 | Pass |
| Fixed, same EGLSurface, MakeCurrent unbind/rebind | 128 | 0 | 127 | Pass |
| Fixed, destroy current EGLSurface, then unbind/recreate | 128 | 0 | 84 | Pass |

Raw logs are committed under `patches/debug/evidence/lifetime-*.log`.
The original same-native EGLSurface recreation control also passes (128
frames); the failure requires native-surface destruction/address reuse.

The tested standalone AArch64 glibc library is
`/root/panvk-wsi-debug/lifetime-fix/libgallium-26.3.0-devel.so` on the target,
SHA-256 `5dc11b6ca6ee8f76be9b2fdb96d5913013f177ac67ca3d05efd853fc49b22715`.
Its build-host copy is `artifacts/libgallium-lifetime.so`. Dynamic-loader
inspection confirmed this library alongside the existing patched
`egl-desktop/usr/lib64/libEGL.so.1`. The PanVK ICD was unchanged.

### Build and run the isolated regression

From this repository root, with the existing Mesa, sysroot, and EGL build:

```bash
git -C "$MESA" apply "$PWD/patches/debug/0003-zink-wayland-displaytarget-lifetime.patch"
BUILD=/home/neurax/panvk-wsi-debug/build/cross-egl-zink-fix
LIBS=/home/neurax/panvk-wsi-debug/artifacts/egl-zink-desktop/usr/lib64
ninja -C "$BUILD" -j12 src/gallium/targets/dri/libgallium-26.3.0-devel.so
aarch64-linux-gnu-strip --strip-unneeded -o libgallium-26.3.0-devel.so \
  "$BUILD/src/gallium/targets/dri/libgallium-26.3.0-devel.so"
aarch64-linux-gnu-gcc --sysroot="$SYSROOT" \
  -I"$MESA/include" -isystem "$SYSROOT/usr/include" -O2 -g -Wall -Wextra \
  patches/debug/surface-lifetime-probe.c -L"$LIBS" \
  -Wl,-rpath-link,"$LIBS:$SYSROOT/usr/lib64" \
  -lEGL -lGLESv2 -lwayland-client -lwayland-server -lwayland-egl \
  -o surface-lifetime-probe
```

Stage the new Gallium library in a separate `lifetime-fix/` directory; keep
the matching EGL/GLES runtime in `egl-desktop/` and the previously verified
PanVK ICD in `icd/`. Do not install over `/usr/lib64`. The probe creates its
own private socketpair/server and requires neither a desktop compositor nor
`WAYLAND_DISPLAY`:

```bash
base=/root/panvk-wsi-debug
for mode in recreate rebind make-current destroy-current; do
  env -u DISPLAY -u LD_PRELOAD \
    LD_LIBRARY_PATH="$base/lifetime-fix:$base/egl-desktop/usr/lib64" \
    LIBGL_DRIVERS_PATH="$base/egl-desktop/usr/lib64/dri" \
    VK_DRIVER_FILES="$base/icd/panvk-kbase-debug.json" \
    MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
    LIBGL_KOPPER_DISABLE=0 PANVK_WSI_SHM_DIAG=1 \
    timeout 65 "$base/ab/surface-lifetime-probe-v2" "--$mode" \
    > "$base/logs/lifetime-v2-$mode.log" 2>&1 || exit 1
done
```

The tested executable is named `surface-lifetime-probe-v2` to retain the
original reproduction binary on the target; copy the freshly built probe
to that path. Each log must end in `PASS`, `commits=128`, and `bad=0`.
The `recreate` run must also record native address reuse to exercise the
original collision. Omitting the `lifetime-fix` directory from
`LD_LIBRARY_PATH` restores the previous Gallium runtime for the test process.

## Installed as the MT6989 default (2026-11-10)

At the user's request, the patched stack is now the default EGL vendor and
PanVK selection. Fedora's GLVND `libEGL.so.1`, `libGLESv2.so.2`,
`libOpenGL.so.0`, and `libGLdispatch.so.0` were retained. The non-GLVND test
EGL library was **not** copied over them: the same patched source was rebuilt
with `-Dglvnd=enabled -Dopengl=true`, using target-side Fedora 45
`libglvnd-devel` and `libglvnd-core-devel` headers/pkg-config metadata.
The other EGL cross-build options are unchanged; the build directory is
`build/cross-egl-glvnd-default`. The final target was
`src/egl/libEGL_mesa.so.0.0.0`, built with Ninja `-j12`.

Installation on the target:

- `/opt/panvk/current` points to `/opt/panvk/f803bcc-glvnd`.
- Its `lib64/` contains the new `libEGL_mesa.so.0`, matching
  `libgallium-26.3.0-devel.so`, and the previously tested PanVK ICD.
- `patchelf --set-rpath '$ORIGIN'` on the installed EGL vendor replaces its
  build-tree RPATH so it finds its matching Gallium library beside itself.
- `/usr/share/glvnd/egl_vendor.d/50_mesa.json` selects
  `/opt/panvk/current/lib64/libEGL_mesa.so.0`.
- `/etc/vulkan/icd.d/panvk-kbase.json` selects
  `/opt/panvk/current/lib64/libvulkan_panfrost.so`.
- `/etc/environment.d/90-panvk-zink.conf` sets
  `MESA_LOADER_DRIVER_OVERRIDE=zink`, `GALLIUM_DRIVER=zink`, and
  `VK_LOADER_DRIVERS_SELECT=panvk-kbase.json`.
- `/etc/profile.d/90-panvk-zink.sh` imports and exports those settings for
  login/interactive shells. No global `LD_LIBRARY_PATH`, `VK_DRIVER_FILES`,
  or `__EGL_VENDOR_LIBRARY_FILENAMES` override was installed.

Installed SHA-256 values:

```text
be88cefb2801524842adc4952469c2d63dbc92102108bb08dc4ddf6b4fe43c89  libEGL_mesa.so.0
788944ae5f4d7307140a98ae40bdf076c549af1bd11cce84130b72d0251c0a6f  libgallium-26.3.0-devel.so
3ba2d5328fd13bd05329cbfad0af91b018c570d42d373a3bd7e9bb253d6fac1c  libvulkan_panfrost.so
```

Verification used a cleared environment and a normal Bash login shell. It
asserted that the three test-only path overrides above were unset, then ran
all four lifecycle modes: each consumed 128 frames with `bad=0`; native
recreation reused an address 84 times and destroy-current reused it 82 times.
An additional throwaway desktop-GL smoke used the system `libEGL.so.1` and
`libOpenGL.so.0`, requested a GL 3.3 core context, obtained OpenGL 4.6
`MESA_PANVK`, and verified all 256 pbuffer pixels as opaque red. Its process
maps confirm the system GLVND dispatch libraries plus all three libraries
under `/opt/panvk/f803bcc-glvnd/lib64`. Logs are in
`patches/debug/evidence/default-*.log`. A clean Zsh login also inherited all
three default settings with `LD_LIBRARY_PATH` unset. Original Fedora EGL,
GLES, OpenGL, EGL-vendor, and `/root/PanKbase` ICD hashes remained unchanged.

No compositor/session restart was performed. Existing processes keep their
old mappings; existing shells can load the selection with
`. /etc/profile.d/90-panvk-zink.sh`, and restarted applications then use the
new vendor. The live Noctalia/niri output and Kitty window remain outside
this verification; the desktop-GL smoke checks API dispatch and pixels, not
a visible terminal. System GLX remains Fedora's existing vendor. Package
updates may replace `50_mesa.json` and thereby reset the EGL vendor selection.

The original manifests, wrapper, and original-library checksum list are in
`/root/panvk-wsi-debug/backup-default-S1Touj`. Roll back with:

```bash
sh /root/panvk-wsi-debug/backup-default-S1Touj/rollback.sh
```

This restores the original EGL and Vulkan manifests, removes the two added
environment files and the `current` selector, and leaves the installed
versioned libraries available for inspection. Restart affected applications
and the login session to discard their old mappings/environment. The rollback
script was syntax-checked; it was not executed after the successful default
deployment.

## EGL/GBM-enabled build only (2026-11-10)

At the user's request, GBM/vgem initialization and Xvfb acceleration diagnosis
were stopped. This build enables the existing EGL GBM platform without adding
a device-matching workaround or changing the target's installed graphics stack.
The existing patched Mesa source and cross file are retained; no Mesa source
changes were made for this build. Relative to `cross-egl-glvnd-default`, the
graphics option change is `-Dgbm=enabled`.

From `/home/neurax/panvk-wsi-debug`, with the existing native code generators:

```bash
PATH="$PWD/host-tools/bin:$PATH" env -u PKG_CONFIG_PATH \
  meson setup build/cross-egl-glvnd-gbm mesa \
  --cross-file cross/aarch64-cross.ini --prefix=/usr --libdir=lib64 \
  --buildtype=release --wrap-mode=nofallback \
  -Dplatforms=x11,wayland -Dgallium-drivers=zink,softpipe \
  -Dvulkan-drivers=panfrost -Dpanfrost-kmds=kbase \
  -Dmesa-clc=system -Dprecomp-compiler=system \
  -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dglx=disabled -Dgbm=enabled -Dglvnd=enabled \
  -Dlibunwind=disabled -Dllvm=disabled -Dzstd=disabled
PATH="$PWD/host-tools/bin:$PATH" ninja -C build/cross-egl-glvnd-gbm -j12
meson install -C build/cross-egl-glvnd-gbm --no-rebuild --strip \
  --destdir "$PWD/artifacts/egl-gbm-aarch64"
```

Ninja completed all 1837 steps. The staged package contains `libEGL_mesa.so.0`,
`libgbm.so.1`, `gbm/dri_gbm.so`, matching Gallium and PanVK libraries, DRI loader
modules, GBM headers/pkg-config metadata, and EGL/Vulkan manifests. The staged
Vulkan manifest's `library_path` was made relative:
`../../../lib64/libvulkan_panfrost.so`. The archive is
`artifacts/egl-gbm-aarch64.tar.zst`, with SHA-256:

```text
c749d20376c90bfe9aaf6fee4c2338ebbe61ba2f7144877ee3fca4d8fc897de0
```

This checksum includes the later kbase sync-file lifetime fix below; the EGL,
GBM, and Gallium libraries are unchanged from the original GBM build.

All six installed ELF libraries were checked with `file`, `readelf -h`,
`readelf -d`, and `readelf --version-info`: ELF64 AArch64, glibc symbol versions,
and no Bionic dependencies. The bundled Gallium library requires
`GLIBC_2.43`; this is not a package for older glibc systems.

A throwaway AArch64 probe under QEMU loaded Fedora's target GLVND dispatcher
and this package's EGL, GBM, GBM backend, and Gallium libraries. Both
`EGL_KHR_platform_gbm` and `EGL_MESA_platform_gbm` were returned by
`eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS)`. Neither `gbm_create_device`
nor `eglInitialize` was called. This verifies compiled platform availability
and library loading, not vgem initialization or hardware rendering. Evidence:
`patches/debug/evidence/egl-gbm-build.log`; full ELF output remains at
`cross/egl-gbm-verification.log` in the parent workspace.

The archive passed `zstd -t` and includes per-library/manifest `SHA256SUMS`.
It is staging-only: do not extract it over `/`. For a future isolated run,
select its `usr/lib64` with `LD_LIBRARY_PATH`, `usr/lib64/gbm` with
`GBM_BACKENDS_PATH`, `usr/lib64/dri` with `LIBGL_DRIVERS_PATH`, and its EGL
manifest with `__EGL_VENDOR_LIBRARY_FILENAMES`. Keep Fedora's GLVND dispatch
libraries. No device installation, default-selector change, or display-server
restart was performed for this build.

## kbase imported sync-file descriptor leak (2026-11-10)

`0004-panvk-kbase-sync-file-lifetime.patch` fixes descriptor and waiter leaks in
`src/panfrost/vulkan/panvk_physical_device.c`. Apply it to the same fully patched
p0175 Mesa tree; it does not require GBM or alter Zink/device matching.

The reported `DMA_HEAP_IOCTL_ALLOC failed: Too many open files` was `EMFILE`,
not GPU memory exhaustion. Inspection of the failing Xvfb `:96` process found
1023 open descriptors against a soft limit of 1024; 992 were `sync_file`.
Picom's sync-file count grew from 9153 to 11068 while it held only three
dma-bufs. Both loaded the existing PanVK ICD, not the newly staged GBM build.

The kbase sync-file import duplicates the input descriptor and stores it in a
heap-allocated waiter. Mesa's common Vulkan import code closes the original
descriptor on success. The kbase wait-success path instead discarded
`pending_data`/`pending_wait` without releasing its duplicate or waiter; reset
and signal had the same ownership loss. The patch releases the old imported
payload under the existing mutex before completed waits, resets, signals,
pending-payload replacement, and destination replacement during a move. Move
still transfers the source payload; timeout retains it for a subsequent wait.
Import replacement now uses the same locked replacement path instead of a
separate free/unlock/relock sequence. Queue-owned payloads are not freed.

### Reproduction and regression

`sync-fd-lifetime-probe.c` uses real Vulkan calls and sync files exported from
a 4 KiB dma-heap buffer. The timeout case attaches an unsignaled vgem fence,
checks `VK_TIMEOUT`/`VK_NOT_READY`, signals it, and waits again. It opens only
`/dev/dri/renderD128`, never a primary/KMS node. Compile from the workspace:

```bash
aarch64-linux-gnu-gcc --sysroot="$SYSROOT" -isystem "$SYSROOT/usr/include" \
  -I mesa/include -O2 -Wall -Wextra -Werror \
  panvk-kbase-android/patches/debug/sync-fd-lifetime-probe.c \
  -Wl,-rpath-link,"$SYSROOT/usr/lib64" -l:libvulkan.so.1 \
  -o artifacts/sync-fd-lifetime-probe
```

The target needs the Vulkan loader and PanVK; the probe rejects other drivers.
On MT6989, use the isolated manifest and preserve the original 1024-fd limit:

```bash
ulimit -n 1024
for mode in wait reset signal replace destroy semaphore timeout; do
  env -u VK_ICD_FILENAMES -u VK_LOADER_DRIVERS_SELECT \
    VK_DRIVER_FILES=/root/panvk-wsi-debug/sync-fd-fix/panvk-sync-fd.json \
    /root/panvk-wsi-debug/sync-fd-lifetime-probe "$mode" 4096 || exit 1
done
```

| Scenario | Before | Fixed |
|---|---|---|
| Fence wait | 128 iterations, fd 9 → 137 | 4096 iterations, fd 9 → 9 |
| Reset imported fence | 32 iterations, fd 9 → 41 | 4096 iterations, fd 9 → 9 |
| Replace with signaled `fd=-1` | 32 iterations, fd 9 → 41 | 4096 iterations, fd 9 → 9 |
| Replace imported payload | 32 iterations, fd 9 → 9 | 4096 iterations, fd 9 → 9 |
| Destroy without waiting | 32 iterations, fd 9 → 9 | 4096 iterations, fd 9 → 9 |
| Queue wait on imported semaphore | 32 iterations, fd 9 → 41 | 4096 iterations, fd 9 → 9 |
| Timeout, signal, then wait | 32 iterations, fd 10 → 42 | 4096 iterations, fd 10 → 10 |

Baseline/fixed logs are `evidence/sync-fd-before.log` and
`evidence/sync-fd-after.log`. The initial 128-wait reproduction was captured in
the session output; the baseline file records the subsequent six modes.

An isolated Xvfb + Picom GLX session then performed 1152 alternating red/green
window fills and checked each readback pixel. At every 128-frame sample, Xvfb
held 22 descriptors and Picom 13, with zero sync-file descriptors in both.
Their process maps selected the fixed ICD; `glxinfo` reported Zink/PanVK
Mali-G720, not llvmpipe. This is window readback and descriptor-lifetime
verification, not an Android screen capture. Evidence:
`evidence/sync-fd-xvfb-picom.log`. The first throwaway harness run failed because
closing its display-number pipe allowed Xvfb's last-client reset to hit EPIPE;
holding the client connection and pipe open corrected the harness. The known
initial `ZINK: failed to choose pdev` remains outside this fix's scope.

### Build, artifacts, and default selection

The fixed ICD was rebuilt with `ninja -C build/cross-panvk-wayland-final -j12
src/panfrost/vulkan/libvulkan_panfrost.so`, then stripped with
`aarch64-linux-gnu-strip --strip-unneeded`. `file` and all three `readelf`
checks confirm ELF64 AArch64, glibc (maximum required version `GLIBC_2.38`),
and no Bionic dependencies. The GBM build's ICD was rebuilt as well and the
EGL/GBM archive and checksums above refreshed.

- Standalone archive: `artifacts/panvk-sync-fd-aarch64.tar.zst` (ICD, relative
  manifest, and regression executable).
- Archive SHA-256: `c5c0da904ddb60ecaeaee516e1ed6809d29940de00d446a370aa36b689495038`.
- Installed library: `/opt/panvk/sync-fd-4fda984e/lib64/libvulkan_panfrost.so`.
- Library SHA-256: `4fda984ea4d8633430cd27910a7d6d969a8728351784830bf7887b54200f3e47`.
- `/etc/vulkan/icd.d/panvk-kbase.json` now selects that isolated library.
- Previous manifest: `/root/panvk-wsi-debug/backup-sync-fd-4fda984e/panvk-kbase.json`.

A clean invocation using `VK_LOADER_DRIVERS_SELECT=panvk-kbase.json`, without
`VK_DRIVER_FILES`/`VK_ICD_FILENAMES`, passed 4096 semaphore cycles with fd 9 → 9
after the selector change (`evidence/sync-fd-default.log`). No limit was raised,
no synchronization was disabled, and no running desktop process was killed or
restarted. Existing Xvfb/Picom processes retain their old mappings and leaked
descriptors until restarted; changing the manifest cannot repair them in place.
The EGL vendor selection, existing libraries, and `/opt/panvk/current` remain
unchanged. Restore the previous ICD selection if needed:

```bash
cp /root/panvk-wsi-debug/backup-sync-fd-4fda984e/panvk-kbase.json \
   /etc/vulkan/icd.d/panvk-kbase.json
```

Restart affected applications after changing the selection. The rollback copy
is retained but was not executed after successful verification.

## Default redeployment with EGL/GBM and Zink (2026-11-10)

This deployment supersedes the selectors in the earlier installation sections.
The user requested the fixed driver as the default, default Zink OpenGL, and
fresh hardware verification. No driver source was changed during redeployment.

### Installed selection

- `/opt/panvk/current` now points to `/opt/panvk/e7bfef6-gbm`.
- That directory contains the fixed PanVK ICD, the GBM-enabled EGL vendor,
  GBM core/backend, DRI modules, and the matching patched Gallium library.
- The ICD is byte-identical to the previously verified `sync-fd-4fda984e` ICD.
  Gallium is byte-identical to the previous GLVND deployment. EGL, GBM, and
  backend modules come from `artifacts/egl-gbm-aarch64` above.
- The installed EGL vendor uses RPATH `$ORIGIN`; the GBM backend and DRI
  module use `$ORIGIN/..` to find their matching Gallium library.
- `/etc/vulkan/icd.d/panvk-kbase.json` and the existing GLVND Mesa manifest
  select the libraries through `/opt/panvk/current/lib64`.
- `/etc/ld.so.conf.d/00-panvk.conf` adds `/opt/panvk/current/lib64`; `ldconfig`
  activates its GBM library. Fedora's GLVND dispatch libraries are retained.
- Fedora's GLX vendor/Gallium 26.2.6 remains in use for GLX applications;
  it selects Zink and the fixed PanVK ICD. EGL applications and Xvfb glamor
  use the patched EGL/Gallium 26.3.0-devel libraries.

The existing canonical `/etc/opt/mt6989/graphics.env` now contains:

```sh
MESA_LOADER_DRIVER_OVERRIDE=zink
GALLIUM_DRIVER=zink
VK_LOADER_DRIVERS_SELECT=panvk-kbase.json
LIBGL_DRIVERS_PATH=/opt/panvk/current/lib64/dri
GBM_BACKENDS_PATH=/opt/panvk/current/lib64/gbm
```

`/etc/environment.d/90-panvk-zink.conf` links to this file for systemd user
services. `/etc/profile.d/90-panvk-zink.sh` sources and exports it for login
shells. The existing `with-panvk-zink` wrapper reads the same canonical file.
Clean Bash and Zsh logins, and a fresh `systemd-run --user` process, all
reported the five values above. This does not inject environment variables
into arbitrary system services or already-running processes. No global
`LD_LIBRARY_PATH`, `VK_DRIVER_FILES`, `VK_ICD_FILENAMES`, or
`__EGL_VENDOR_LIBRARY_FILENAMES` override was installed.

### Fresh target verification

All graphics tests started from an empty environment followed by a normal
Bash login, without test-only driver paths. The fd soft limit stayed 1024.

| Check | Result |
|---|---|
| Seven sync-fd lifecycle modes | 4096 iterations each; fd delta 0 in every mode |
| Four Wayland surface lifetime modes | 128 commits each; consumer `bad=0` |
| Fresh Xvfb glamor + Picom GLX + glxgears | Zink / Mali-G720 / MESA_PANVK; accelerated OpenGL 4.6 |
| Alternating RGB window and composited overlay readbacks | Final run: 2048 frames passed in 27.15 seconds |
| Xvfb fd samples | 21 throughout |
| Picom fd samples | 14–16, returning to 14; temporary sync files did not accumulate |
| glxgears fd samples | 8 throughout |

Process mappings confirm Xvfb loaded the newly installed EGL, GBM core,
GBM backend, Gallium, and fixed PanVK ICD. Picom and glxgears loaded Fedora's
GLX/Gallium and the same fixed ICD. The captured Picom overlay shows the
colored window and rendered gears, not a black frame; it remains on the
device at `/root/panvk-wsi-debug/logs/redeploy-composited.png`.

Two earlier harness runs must not be hidden: one expected full-opacity red
but the existing Picom configuration intentionally uses inactive opacity
0.97 (the correct value over black is `0xf70000`). After correcting that
expectation, another run timed out at frame 184 after two seconds with an
older overlay color, although the source-window pixel was correct. A final
run with stall diagnostics completed all 2048 frames without a stall and
without a production code/configuration change. The isolated earlier stale
frame's cause is **not established**; this deployment proves the fd lifetime
fix and the successful rendering run, not the absence of all intermittent
compositor issues. Its failure output is retained in `redeploy-runtime.log`.

At deployment time no Xvfb, Picom, or niri session was running. The verification
started fresh temporary Xvfb/Picom/glxgears processes and stopped only those
processes afterward. The disabled `xlibre-headless.service` was not enabled or
started. No Android physical display/KMS device was touched. This is not a
new live niri/Noctalia/Android-output validation.

Evidence in `patches/debug/evidence/`:

- `redeploy-install.log`: installed library hashes and retained GLVND dispatch.
- `redeploy-regressions.log`: seven fd and four Wayland regression logs.
- `redeploy-graphics.log`: renderer, sampled fd counts, process maps, final pass.
- `redeploy-runtime.log`: Xvfb/Picom/glxgears logs and earlier overlay timeout.

Re-run the permanent regressions using the installed defaults:

```bash
env -i HOME=/root PATH=/usr/sbin:/usr/bin:/sbin:/bin \
  XDG_RUNTIME_DIR=/run/user/0 /bin/bash -lc '
    set -eu
    ulimit -n 1024
    for mode in wait reset signal replace destroy semaphore timeout; do
      /root/panvk-wsi-debug/sync-fd-lifetime-probe "$mode" 4096
    done
    for mode in recreate rebind make-current destroy-current; do
      /root/panvk-wsi-debug/ab/surface-lifetime-probe-v2 "--$mode"
    done
  '
```

### Rollback

The previous manifests, canonical graphics environment, and `current` link
are retained under `/root/panvk-wsi-debug/backup-redeploy-e7bfef6`. Restore
that immediately previous selection with:

```sh
sh /root/panvk-wsi-debug/backup-redeploy-e7bfef6/rollback.sh
```

The script restores the old EGL and ICD manifests, old canonical environment,
and old `current` link; removes this deployment's profile/environment links
and linker-cache configuration; and refreshes the linker cache and user
manager environment. Versioned libraries remain installed. Its shell syntax
was checked; rollback was not executed after deployment. Restart graphics
applications and log in again after rollback. Existing shells can instead
load the new deployment now with `. /etc/profile.d/90-panvk-zink.sh`.
