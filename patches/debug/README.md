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

**MT6989 delivery policy (user instruction, 2026-11-11):** after a bug fix
passes isolated device validation, deploy it as the device's default graphics
stack and verify a fresh process through the normal default selectors. Do not
stop at a test-only launcher. Keep the previous version and a rollback command;
preserve Fedora GLVND dispatch libraries and do not restart an active user
desktop without permission. Failed or unverified candidates stay isolated.

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

## Firefox CSF call-address truncation (2026-11-11)

Firefox 159.0.1 on the MT6989 target failed at startup with CSF group fatal
status `0x7d0002c1`, sideband `0x00000000fffee000`, then queue timeouts and
`zink: DEVICE LOST!`. Exception `0xc1` is `TRANSLATION_FAULT_L1`, not fd
exhaustion. An isolated diagnostic with `PANVK_TLS_SPARSE=0` still hit the
same fault; sparse TLS was not changed by this fix.

The user identified [issue #9](https://github.com/wonderkast02/panvk-g720-kbase-csf/issues/9).
Its [root-cause correction](https://github.com/wonderkast02/panvk-g720-kbase-csf/issues/9#issuecomment-5977983155)
describes the same fault signature. The locally reconstructed p0175 source
still had the defect in `get_fb_descs()`:

```c
uint32_t fn_addr = dev->draw_ctx->fns_bo->addr.dev + fn_idx * fn_stride;
cs_move64_to(b, addr_reg, fn_addr);
```

The provoking-vertex helper path truncates a 64-bit GPU function address
before loading it into a 64-bit CS register. kbase uses high SAME_VA GPU
addresses, so CS CALL can jump to an unmapped low address. This path is used
when framebuffer descriptors are allocated before provoking-vertex mode is
known; successful gears tests did not establish coverage of that path.

`0005-panvk-csf-helper-address-width.patch` changes only `fn_addr` to
`uint64_t`, matching the [fix published by @emir34-34](https://github.com/emir34-34/DroidDeck/blob/mediatek-mali-support/tools/panvk/patches/0002-fix-provoking-vertex-fn-address-truncation.patch).
No queue throttling, synchronization-scope changes, software rendering, or
TLS-layout override is part of the fix. Existing WSI, surface-lifetime and
sync-fd fixes remain applied.

Build the same reconstructed Mesa tree after applying patch 0005:

```bash
ninja -C build/cross-panvk-wayland-final -j12 \
  src/panfrost/vulkan/libvulkan_panfrost.so
aarch64-linux-gnu-strip --strip-unneeded \
  -o artifacts/libvulkan_panfrost-calladdr.so \
  build/cross-panvk-wayland-final/src/panfrost/vulkan/libvulkan_panfrost.so
```

The rebuilt library passed `file`, `readelf -h`, `readelf -d`, and
`readelf --version-info`: ELF64 AArch64, glibc (maximum required version
`GLIBC_2.38`), Wayland/X11 dependencies, no Bionic dependencies. Patch 0005
passed reverse application checking against the edited source.

Standalone artifact: `artifacts/panvk-calladdr-aarch64.tar.zst`, containing
the ICD and relative `panvk-calladdr.json` manifest.

```text
ICD SHA-256: cc9041bebad2eab642fe2c4f408d3b2aaa46aa0307487c06d02948c87af6eb50
Archive SHA-256: d1a68249a57ce606ab5888f796e7a48e80013488330d502558634e87b7de952c
```

### Hardware validation and installation

With the isolated fixed ICD and the original default sparse-TLS setting,
native Firefox 159.0.1 initialized on `wayland-1`. Its privileged graphics
snapshot reported `windowProtocol=wayland`, `windowLayerManagerType=WebRender`,
`numAcceleratedWindows=1`, and Zink/Mali-G720/MESA_PANVK for the adapter and
both WebGL renderers. No graphics acceleration or sandbox preference was
disabled. A disposable profile left the user's existing profile untouched.

A local page exercised CSS transforms/gradients/shadows and a WebGL2 canvas
for 60 seconds. It rendered 954 frames with zero readback pixel mismatches and
no context loss; all six samples advanced. Browser diagnostics still showed
hardware WebRender afterward. The browser mapped the fixed ICD and its log
contained no CSF fatal error, subqueue timeout, or `DEVICE LOST`. Its captured
page visibly contains the gradient/shadow and colored WebGL canvas, retained
at `/root/panvk-wsi-debug/logs/firefox-calladdr-isolated.png`.

The temporary automation needed three harness corrections, not driver changes:
this Firefox requires `MOZ_REMOTE_ALLOW_SYSTEM_ACCESS=1` for privileged
Marionette diagnostics; page globals must be accessed through its Xray wrapper;
and the web-exposed renderer string is `Mali-T760, or similar`, so hardware
identity was checked using privileged graphics diagnostics and process maps,
not that web-facing string. The diagnostic access environment was restricted
to the disposable test process and was not installed globally.

`/opt/panvk/current` now selects `/opt/panvk/calladdr-cc9041be`. This directory
contains the new ICD and byte-for-byte copies of the previously deployed
EGL/GBM/Gallium components. Existing default Zink settings and manifests are
unchanged. The previous version remains available. Running niri/Noctalia were
not restarted; new application processes use the new ICD.

Reproduce the original application path with the installed defaults:

```sh
WAYLAND_DISPLAY=wayland-1 firefox
```

For an isolated profile, create a separate directory and launch with
`firefox --no-remote --profile /absolute/path/to/profile`. Check `about:support`
for Wayland, hardware WebRender and MESA_PANVK, not merely process survival.
An isolated ICD override must also unset `VK_LOADER_DRIVERS_SELECT`, since the
global default selects the filename `panvk-kbase.json`.

Evidence in `patches/debug/evidence/`:

- `firefox-calladdr-before.log`: old ICD still faults with full TLS allocation.
- `firefox-calladdr-after.log`: graphics snapshots, six pixel samples and maps.
- `firefox-calladdr-driver.log`: fixed ICD's application log.

Rollback to the immediately previous driver selection:

```sh
sh /root/panvk-wsi-debug/backup-calladdr-cc9041be/rollback.sh
```

This restores the `e7bfef6-gbm` selector and refreshes the linker cache; restart
affected applications afterward. The script was syntax-checked but not executed
after the successful cutover. It retains default Zink and both versioned stacks.

The installed-default path was then exercised separately for 10 seconds, with
neither `VK_DRIVER_FILES` nor `VK_ICD_FILENAMES` set and the normal
`VK_LOADER_DRIVERS_SELECT=panvk-kbase.json` selection intact. Native Wayland,
hardware WebRender and MESA_PANVK WebGL2 checks passed; 11 animation frames had
zero pixel mismatches and no context loss. Process maps resolved EGL, Gallium
and the ICD to `/opt/panvk/calladdr-cc9041be/lib64/`.
`evidence/firefox-calladdr-default.log` retains this separate verification.
These short runs establish the startup/rendering fix, not long-duration
stability or a performance benchmark.

## Wayland XR30 negotiation (2026-11-11)

`0006-wayland-kopper-format-negotiation.patch` fixes the EGL/Kopper presentation
capability mismatch and adds a defensive SHM-only WSI check. Apply it after the
existing debug fixes to the same patched p0175 Mesa tree. It neither changes
PanVK rendering/CSF/synchronization nor replaces the requested image format.
`0007-wsi-colorspace-diagnostic.patch` only adds the colorspace to the existing
opt-in `MESA_VK_WSI_CREATE_LOG=1` output; it is separate from the core fix.

### Root cause and ownership

The retained original trace, `evidence/xr30-before-wayland.log`, shows:

1. EGL's `mesa egl display queue` binds linux-dmabuf v4 and consumes default
   feedback. The DMA-BUF format set includes XR30.
2. `eglChooseConfig()` receives minimum RGB8 sizes. Its normal descending
   color-bit sort can legitimately choose RGB10; Alpha8 excludes RGB10X2/A2.
3. Kopper uses the config-derived pipe format in
   `zink_kopper_displaytarget_create()` and `kopper_CreateSwapchain()`, without
   querying Vulkan SurfaceFormats.
4. PanVK's actual Wayland WSI uses SHM. Its queue receives only ARGB8888,
   XRGB8888, ABGR8888 and XBGR8888, then sends
   `wl_shm_pool.create_buffer(..., 808669784)` (`0x30335258`, XR30).
5. The compositor rejects XR30 and the connection enters EPROTO (71). The next
   EGL surface fails. Blender's unchecked secondary-context NULL is a separate
   application bug, not fixed or worked around here.

Responsibility is the **EGL/Kopper handoff**: EGL used the initializer's
DMA-BUF capabilities while the Vulkan WSI chose SHM. This is not a blit,
cache-coherency, FourCC byte-order, or HDR/shader issue.

| Investigation question | Finding |
|---|---|
| Are SHM advertisements saved? | `shm_handle_format()` maps supported/renderable entries into `display->formats`, with separate alpha/opaque flags. Unknown/unrenderable entries are intentionally not exposed. |
| Does SurfaceFormats filter correctly? | Yes. `wsi_wl_surface_get_formats()` and `...get_formats2()` discover the selected transport and omit formats lacking both required alpha/opaque variants. The SHM8 test exposes only formats 44/50, not 58. |
| Are EGL configs compatible? | Previously not necessarily: `dri2_wl_add_configs_for_visuals()` used EGL's initializer-dependent bitmap, not the actual Vulkan surface capability list. |
| Did Kopper request an unlisted format? | Yes, it directly converted the EGL drawable pipe format to VkFormat without a surface-format query. |
| Is `wl_shm_format_for_vk_format()` a capability check? | No. Mapping A2R10G10B10 to XR30 is correct, but the mapping alone does not establish compositor support. |
| Was a final check missing? | Yes. The SHM creation branch could reach `wl_shm_pool_create_buffer()` for an unadvertised format. |
| Did debug 0001–0003 inject XR30? | No. 0001 observes copied bytes; 0002 preserves explicit Zink/Kopper on EGL fallback; 0003 fixes drawable lifetime. None adds XR30 formats. |
| Was this a dynamic DMA-BUF→SHM fallback? | No fallback inside swapchain creation was observed. The disagreement already exists between EGL's format discovery and the independently selected Vulkan WSI transport. |

### Minimal correction

- Wayland EGL obtains a real temporary `wl_surface` and queries the actual
  Vulkan SurfaceFormats through a direct frontend→Zink helper. Candidate pipe
  formats use the same `zink_get_format()` mapping and PASS_THROUGH colorspace
  as Kopper presentation. This runs during display initialization, not per frame.
- Kopper window-config availability uses that result instead of EGL's
  DMA-BUF/SHM bitmap. No format substitution or PRIME conversion is introduced.
  Non-Kopper selection is unchanged. Pbuffer-only Kopper configs use native
  visual ID zero so `_eglValidateConfig()` accepts them; exact RGB10 pbuffers
  remain available when RGB10 windows are unsupported.
- `EGL_PRESENT_OPAQUE_EXT` no longer rechecks the unrelated EGL transport's
  modifier bitmap for a Kopper window. Temporary Vulkan/Wayland objects are
  released; `wl_fixes` registry destruction and removal acknowledgements use
  the existing conventions when available. A non-Zink build has a matching stub.
- `wsi_wl_surface_create_swapchain()` checks the selected SHM format and requested
  alpha/opaque variant before allocating images or sending buffer requests.
  Unsupported requests return `VK_ERROR_INITIALIZATION_FAILED`. The DMA-BUF
  branch is untouched, and `oldSwapchain` retirement ordering is preserved.

Vulkan requires `imageFormat`/`imageColorSpace` to match an enumerated pair
([VUID 01273](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html)).
The direct invalid-format case below intentionally violates that input rule to
test implementation robustness without a validation layer intercepting it;
it is not a Vulkan conformance test. `VK_ERROR_FORMAT_NOT_SUPPORTED` is not a
listed `vkCreateSwapchainKHR` return code, so the guard uses initialization failure.

### Cross-build and independent deployment

The user explicitly superseded the attachment's native-build requirement:
**compile on Fedora x86_64; the phone is for short validation only**. The native
build was terminated and its Ninja/Clang PIDs were verified gone. No compiler
ran on the phone for the verified artifacts below.

The existing cross build was reused, preserving its complete p0175/debug stack:

```sh
# From /home/neurax/panvk-wsi-debug on the x86_64 host:
ninja -C build/cross-egl-glvnd-gbm -j6
meson install -C build/cross-egl-glvnd-gbm \
  --destdir "$PWD/artifacts/xr30-aarch64" --no-rebuild
tar -I 'zstd -T4 -3' -cf artifacts/xr30-aarch64.tar.zst \
  -C artifacts/xr30-aarch64 usr
```

The existing `cross/aarch64-cross.ini` selects aarch64-linux-gnu GCC/G++, the
Fedora ARM64 glibc sysroot, target-only pkg-config directories and a host
wayland-scanner. Existing build options include X11/Wayland, Zink, PanVK kbase,
GLVND EGL/GBM, desktop OpenGL and GLES2, with system host precompilers. No
software renderer is selected in any passing test. The unrelated existing
`zink_descriptors.c` maybe-uninitialized warning was not suppressed or changed.

The new ICD, Gallium, EGL and probe were checked with `file`, `readelf -h`,
`readelf -d` and `readelf --version-info`: ELF64 AArch64, glibc dependencies,
no Android/Bionic dependency or host-library runtime search path. Full output
is retained locally at `artifacts/xr30-elf-check.log`.

The initially tested directory was `/root/panvk-wsi-debug/xr30/runtime/usr/lib64`.
During isolated validation, `/opt/panvk/current` remained the prior
`calladdr-cc9041be` stack; no system manifest, default selector, kernel or
compositor was changed. The test launcher `/root/panvk-wsi-debug/xr30/xr30-run`
selects only that directory via
`LD_LIBRARY_PATH`, an independent GLVND vendor JSON, and an independent
`panvk-kbase.json`. The manifest name retains compatibility with the installed
`VK_LOADER_DRIVERS_SELECT=panvk-kbase.json` selection. Ending those isolated
test processes was their rollback. The subsequent default installation below
supersedes that test-only deployment status.

### Reproducible controlled regression

`wayland-format-probe.c` uses the existing private fork/socketpair test pattern,
real libwayland-server SHM validation, and the real PanVK ICD. It checks actual
SHM requests and GPU pixels consumed by the server. The `mixed` fixture sends
DMA-BUF v4 default/surface feedback containing AR24/XR24/AR30/XR30, using the
existing render node's device ID; it does not import DMA-BUF buffers. SHM is
independently restricted to the selected 8- or 10-bit set. A successful mixed
EGL test requires default feedback and two surface-feedback requests, so a
silent fallback to the SHM-only EGL initializer cannot make it pass.

Cross-compile the probe on the host (no compilation on the phone):

```sh
root=/home/neurax/panvk-wsi-debug
sysroot="$root/cross/sysroot-aarch64"
out="$root/build/xr30-probe"
mkdir -p "$out"
xml="$sysroot/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"
wayland-scanner server-header "$xml" "$out/linux-dmabuf-server-protocol.h"
wayland-scanner private-code "$xml" "$out/linux-dmabuf-protocol.c"
aarch64-linux-gnu-gcc --sysroot="$sysroot" -isystem "$sysroot/usr/include" \
  -I"$out" -I"$root/mesa/include" -Wall -Wextra -Wno-unused-parameter -O2 -g \
  "$root/panvk-kbase-android/patches/debug/wayland-format-probe.c" \
  "$out/linux-dmabuf-protocol.c" -o "$root/artifacts/wayland-format-probe" \
  -L"$sysroot/usr/lib64" -Wl,-rpath-link="$sysroot/usr/lib64" \
  -lwayland-client -lwayland-server -lwayland-egl -lEGL \
  -l:libGL.so.1 -l:libvulkan.so.1
```

This sysroot's GLVND libGL link emits a missing transitive `libXext.so.6`
link-time warning; the target supplies it and the executable loaded successfully.
After transferring the cross-built probe and libraries, run each case briefly:

```sh
base=/root/panvk-wsi-debug/xr30
mkdir -p "$base/runtime"
tar --zstd -xf "$base/xr30-aarch64.tar.zst" -C "$base/runtime"
printf '{"file_format_version":"1.0.1","ICD":{"library_path":"%s","api_version":"1.4.363","library_arch":"64"}}\n' \
  "$base/runtime/usr/lib64/libvulkan_panfrost.so" > "$base/panvk-kbase.json"
printf '{"file_format_version":"1.0.0","ICD":{"library_path":"%s"}}\n' \
  "$base/runtime/usr/lib64/libEGL_mesa.so.0" > "$base/egl.json"
for spec in 'egl 8 mixed' 'egl 10 shm' 'vulkan 8 mixed' 'vulkan 10 shm'; do
  set -- $spec
  env -u DISPLAY -u LD_PRELOAD \
    LD_LIBRARY_PATH="$base/runtime/usr/lib64" \
    __EGL_VENDOR_LIBRARY_FILENAMES="$base/egl.json" \
    VK_DRIVER_FILES="$base/panvk-kbase.json" \
    VK_ICD_FILENAMES="$base/panvk-kbase.json" \
    VK_LOADER_DRIVERS_SELECT=panvk-kbase.json \
    MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink \
    LIBGL_DRIVERS_PATH="$base/runtime/usr/lib64/dri" \
    GBM_BACKENDS_PATH="$base/runtime/usr/lib64/gbm" \
    WAYLAND_DEBUG=1 EGL_LOG_LEVEL=debug MESA_VK_WSI_CREATE_LOG=1 \
    timeout 20s "$base/wayland-format-probe" "$@"
done
```

`WAYLAND_FORMAT_DRM_DEVICE` can select another existing render node for the
mixed fixture (default `/dev/dri/renderD128`). No render node is fabricated.
The positive SHM10 fixture adds **both XR30 and AR30** because the current
WSI's enumeration requires alpha and opaque variants; it does not advertise
XR30 in the negative SHM8 fixture.

### Live desktop follow-up

The target's isolated `xr30-run` launcher exports the runtime/ICD variables
shown above and clears `DISPLAY` and `LD_PRELOAD`. With a live Niri session,
the application entry points used for the desktop follow-up were:

```sh
base=/root/panvk-wsi-debug/xr30
export XDG_RUNTIME_DIR=/run/user/0 WAYLAND_DISPLAY=wayland-1
"$base/xr30-run" env DISPLAY=:0 eglinfo -B
"$base/xr30-run" glmark2-es2-wayland -b build:duration=8:use-vbo=false --size=640x480
"$base/xr30-run" blender --factory-startup --gpu-backend opengl --window-geometry 0 0 800 600
"$base/xr30-run" env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET DISPLAY=:0 \
  blender --factory-startup --gpu-backend opengl --window-geometry 0 0 800 600
```

The Blender smoke additionally opened a second 3D Viewport, captured both
windows, closed the second window and quit. Firefox used a temporary profile
with native Wayland enabled, two WebGL canvases with RGB bands, band-centre
`readPixels` checks, and a privileged graphics snapshot. Four samples were
taken two seconds apart. All captures came from Niri's `screenshot-window`.
Temporary automation scripts and the Firefox profile were removed afterward;
the isolated runtime and the recorded evidence were retained.

### Measured results and limits

| Case | Result | Evidence |
|---|---|---|
| EGL, DMA-BUF10 + SHM8 | PASS: selected RGB8/Alpha0; two sequential 1×1 red/green frames; 8 XR24 buffers, 0 XR30, 0 bad pixels, Wayland error 0 | `evidence/xr30-egl-8-mixed.log` |
| RGB10 pbuffer with SHM8 | PASS: exact 10/10/10 config retained, PBUFFER bit set and WINDOW bit clear; pbuffer creation succeeds | Same log |
| EGL, SHM10 | PASS: selected RGB10/Alpha0; 8 XR30 buffers, two correct RGB10 frames; Wayland error 0 | `evidence/xr30-egl-10-shm.log` |
| Vulkan, DMA-BUF10 + SHM8 | PASS: queried formats 44/50 only; forced format 58 returns -3 without a buffer request; format 44 then succeeds on the same surface/connection | `evidence/xr30-vulkan-8-mixed.log` |
| Vulkan, SHM10 | PASS: format 58 enumerated; XR30 swapchain and subsequent 8-bit swapchain create successfully | `evidence/xr30-vulkan-10-shm.log` |
| eglinfo GBM/Surfaceless | PASS: EGL 1.5, PanVK/Zink OpenGL 4.6 core/compatibility and GLES 3.2 | `evidence/xr30-eglinfo.log` |
| eglinfo Wayland/X11, desktop follow-up | PASS: EGL 1.5, PanVK/Zink OpenGL 4.6 core/compatibility and GLES 3.2; GBM and Surfaceless also pass | `evidence/xr30-desktop/eglinfo.log` |
| glmark2 native Wayland | PASS: visible model in compositor capture, 640×480, GLES 3.2, 8-second build scene; process exit 0 | `evidence/xr30-desktop/glmark-wayland.log`, `glmark-capture.log`, `glmark-wayland.png` |
| Blender native Wayland | PASS: OpenGL/Zink main window and second viewport rendered; second window closed; application quit normally | `evidence/xr30-desktop/blender-wayland.log`, `blender-wayland-primary-0.png`, `blender-wayland-two-windows-0.png` |
| Blender XWayland | PASS: main window and second viewport rendered; second window closed; application quit normally; system Zink 26.2.6 with new PanVK ICD | `evidence/xr30-desktop/blender-xwayland.log`, `blender-xwayland-primary-0.png`, `blender-xwayland-two-windows-1.png` |
| Firefox native Wayland | PASS: hardware WebRender, WebGL1 and WebGL2 on MESA_PANVK; four samples over approximately 8 seconds, final frame count 10, zero bad pixels/GL errors, page contexts not lost; normal exit | `evidence/xr30-desktop/firefox-check.log`, `firefox-driver.log`, `firefox-wayland.png` |

Both rendered EGL cases used a real OpenGL 4.6 core context and reported
`zink Vulkan 1.4(Mali-G720-Immortalis MC12 (MESA_PANVK))`. The Vulkan-only probe
checks creation/rejection and protocol safety, not rendering/presentation;
GPU presentation is checked through EGL/Zink. An initial v3 fixture fell back
to SHM EGL and failed the pbuffer assertion; those issues were diagnosed and
corrected before the four passing runs. No full Blender rendering or
long-duration stability claim follows from these short probes.

The desktop follow-up started a Niri-Anland/XWayland session for these checks;
the earlier unavailable-session failures remain in `evidence/xr30-eglinfo.log`.
The native Wayland applications' process maps identify the isolated
`/root/panvk-wsi-debug/xr30/runtime/usr/lib64` EGL, Gallium and PanVK libraries.
Blender XWayland instead used system GLX/Zink 26.2.6 with the isolated PanVK ICD:
that result does not validate a new GLX build. The screenshots are actual Niri
window captures, not independently captured Android scanout. No llvmpipe
fallback was used for these application checks.
The compositor retained the existing GPU stack under
`/opt/panvk/calladdr-cc9041be/lib64`; `evidence/xr30-desktop/niri-maps.log`
records its PanVK, Zink and EGL mappings. The test did not replace the
compositor's working libraries with the XR30 candidate.

The dedicated session was closed after checking that Niri had no windows.
The quit IPC client reported EOF; a subsequent process check confirmed that
the Niri launcher, Niri, xwayland-satellite, XWayland and test applications had
exited. Both the request output and the final check are preserved in
`evidence/xr30-desktop/session-cleanup.log` and `session-final.log`.

Firefox's graphics snapshot additionally emitted unsupported-extension probe
errors and `Troubleshoot.sys.mjs` context-lost warnings, preserved verbatim in
`firefox-driver.log`; the test page's four samples all reported `lost: false`.
The recorded frame count is not a performance measurement. WebGPU, video decode,
full Blender scene rendering and long-duration stability are outside this smoke
test's coverage.

The fix is technically generic to Mesa EGL/Kopper/Wayland WSI and can be
proposed upstream as separate capability-negotiation and SHM-safety changes;
it contains no PanVK/compositor-name blacklist. No upstream MR was opened.
Actual native DMA-BUF10 presentation, alpha-only format sets, wl_fixes-v2
global-removal events, non-Zink builds and rare query-error paths have not been
runtime-validated here. The DMA-BUF WSI branch and PanVK CSF/synchronization are
unchanged. These limits must accompany any upstream proposal or wider deployment.

## XR30 installed as the device default (2026-11-11)

At the user's request, validated fixes are now delivered with device-default
installation and a fresh default-path check, not just an isolated test launcher.
The existing XR30 fix from `39e82f3` has been promoted accordingly:

- `/opt/panvk/current` now points to `/opt/panvk/xr30-39e82f3`.
- Its PanVK and Gallium libraries are byte-identical to the cross-built XR30
  artifacts validated above. The complete matching EGL/GBM/DRI stack is included.
- As in the previous default deployment, installed EGL uses RPATH `$ORIGIN`,
  and the GBM backend/DRI module use `$ORIGIN/..`. This removes dependence on
  the test launcher's `LD_LIBRARY_PATH`; the three RPATH-adjusted files have
  their installed hashes recorded in `evidence/xr30-default/default-install.log`.
- The existing EGL/Vulkan manifests, canonical environment, profile hook and
  linker configuration were unchanged. Their hashes and the Fedora GLVND
  dispatch-library hashes were checked after the atomic symlink switch and
  `ldconfig`. No system GLVND library was overwritten.
- The prior `/opt/panvk/calladdr-cc9041be` directory remains available. Niri
  PID 12138 was preserved throughout; already-running applications retain
  their old library mappings. New default-launched applications use XR30.

Verification ran from `env -i ... /bin/bash -lc` with the normal login policy.
`LD_LIBRARY_PATH`, `LD_PRELOAD`, `VK_DRIVER_FILES`, `VK_ICD_FILENAMES` and
`__EGL_VENDOR_LIBRARY_FILENAMES` were all unset:

- `wayland-format-probe egl 8 mixed`: RGB8 window, RGB10 pbuffer retained,
  zero XR30 SHM requests, correct red/green pixels, zero Wayland errors.
- `wayland-format-probe egl 10 shm`: RGB10 window, eight XR30 buffers,
  correct red/green pixels, zero Wayland errors.
- `wayland-format-probe vulkan 8 mixed`: unsupported XR30 rejected with -3
  before any buffer request; the supported 8-bit swapchain then succeeded.
- A fresh 320×240 native Wayland glmark2 window rendered the build model for
  five seconds and exited normally. Its process maps identify EGL, Gallium,
  GBM and PanVK under `/opt/panvk/xr30-39e82f3/lib64`, without `xr30-run`.
  The actual Niri window capture was visually checked.

Evidence: `evidence/xr30-default/default-verify.log`, `default-glmark.log`
and `default-glmark.png`. These are short default-selection checks, not a
new long-duration stability or compositor-restart validation. `ldconfig`
reported `/lib64/libvulkan_wrapper.so is not a symbolic link`; it exited
successfully, and the loaded ICD path above confirms the intended selection.

Configuration backups and the guarded rollback script are retained at
`/root/panvk-wsi-debug/backup-xr30-39e82f3`. Restore the immediately previous
default with:

```sh
sh /root/panvk-wsi-debug/backup-xr30-39e82f3/rollback.sh
```

The script switches `current` back to `calladdr-cc9041be` and refreshes the
linker cache; it refuses to overwrite an unexpected newer selection. Its shell
syntax was checked, but rollback was not executed after the passing deployment.
Restart affected applications after rollback. Temporary deployment/verification
scripts were removed; the versioned stacks, logs, capture and rollback remain.


## Wayland presents through linux-dmabuf by default (2026-11-13)

`0008-panvk-wayland-dmabuf-default.patch` applies after `0001`–`0007`. It
changes how kbase Wayland clients present frames; X11 is unchanged.

Before this patch, kbase devices were always WSI "sw" devices. On Wayland
each frame took three copies: GPU blit to a linear readback buffer, CPU
`memcpy` into `wl_shm` (about 10 MB per frame at 2392x1080), then a texture
upload in the compositor. kbase can export dma-heap BOs, so Wayland now
creates GPU images and attaches them with `zwp_linux_buffer_params_v1`. It
uses the existing dma-buf `EXPORT/IMPORT_SYNC_FILE` implicit-sync path; the
kbase sync_file export already existed. X11 keeps CPU readback and MIT-SHM,
because Xvfb and Termux:X11 cannot import these buffers.

- `wsi_device_options::wayland_dmabuf` sets `wsi_device::wayland.dmabuf`.
  Every Wayland `wsi->sw` decision goes through `wsi_wl_use_sw()`.
  `MESA_VK_WSI_DEBUG=sw` still forces `wl_shm`.
- `GetMemoryFdKHR` is resolved for these devices. The present-time CPU fence
  wait now applies only to CPU images. `wsi_drm_init_swapchain_implicit_sync()`
  skips non-DRM images, so X11 CPU swapchains no longer create a SYNC_FD
  semaphore they cannot use.
- PanVK turns the path on for kbase unless `PANVK_KBASE_DRI3` already selects
  the full dma-buf WSI. **`PANVK_WSI_WAYLAND_DMABUF=0` restores `wl_shm`.**

vgem was evaluated and rejected as an allocator. A PanVK GPU fill imported
from a vgem dumb BO reached the CPU only 20–207 ms after the Vulkan fence,
and never completed without `COHERENT_SYSTEM`. dma-heap BOs were complete
immediately (`evidence/wayland-dmabuf/vgem-probe.c`). vgem could later supply
only the DRM device identity that would remove the remaining GPU blit; that
is not part of this patch.

### Verification on MT6989

Setup: private `niri-anland` with a private Anland consumer
(`evidence/wayland-dmabuf/verify.sh`). The user session was not touched. The
client was fullscreen 2392x1080 Zink `glmark2-es2-wayland`. The compositor
kept the deployed stack; only the client ICD changed.

| ICD / mode | buffers | glmark2 score | stress frames checked | torn/mixed |
|---|---|---|---|---|
| deployed XR30 (`wl_shm`) | `wl_shm_pool.create_buffer` | 103, 124 | 519 | 0 |
| 0008, `PANVK_WSI_WAYLAND_DMABUF=0` | `wl_shm_pool.create_buffer` | 134 | 510 | 0 |
| 0008 default | `create_immed` | 332, 282, 420, 324 | 310+289+409+420 | 0 |
| 0008 installed default | `create_immed` | 358 | 353 | 0 |
| 0008 + `PANVK_KBASE_DRI3=1` | `create_immed` | 496, 651 | 383+361 | 0 |

The stress client `sync-stress.c` runs a long full-screen fragment pass each
frame and alternates solid colors. `consumer-sync.c` checks a 64x32 grid on
every frame the Anland consumer receives. A buffer read before the GPU
finished would contain mixed or foreign colors. None were found.

X11 check: the user's real `/opt/xlibre/25.2.3/bin/Xvfb -glamor` server
(DRI3/Present over Zink+PanVK), with server and clients on the same ICD
(`x11-glamor-check.sh`). `glmark2-es2` stayed on `wsi/x11 sw readback`. Scores:
old 975/959, new 730/1057 (order-alternated runs). The adaptive readback
mode makes these runs noisy. `vkcube` exited 0 on both, and the root-window
captures matched (lit pixels 15428–15480). `glxgears` fails identically on
old and new (`DRI3 not available`, no double-buffered visual); this is a
pre-existing limitation of this server, not a regression.

### Installed as the device default

- `/opt/panvk/current` points to `/opt/panvk/wldmabuf-c63caad0`.
- That directory is a copy of `xr30-39e82f3`. The deploy script checked that
  every file except the ICD matches.
- ICD SHA-256:
  `c63caad047a7b59e9903f13b36698b446d6ea24e191defb34eee4fccbb867ffa`.
  It is ELF64 AArch64, glibc (maximum required `GLIBC_2.38`), with no Bionic
  `NEEDED` entries.
- System manifests, `graphics.env`, and `00-panvk.conf` were hash-checked as
  unchanged.
- A clean login shell inherits the default selectors.
- A default-launched `glmark2-es2-wayland` maps EGL, Gallium, and PanVK from
  `/opt/panvk/wldmabuf-c63caad0/lib64`
  (`evidence/wayland-dmabuf/def-*.out`, `deploy.log`).
- Running processes keep their old mappings until restarted.

Rollback switches `current` back to `xr30-39e82f3`. It refuses an unexpected
selection, and its syntax was checked; it was not executed:

```sh
sh /root/panvk-wsi-debug/backup-wldmabuf-c63caad0/rollback.sh
```

Open items:
- The extra GPU blit remains. PanVK reports no DRM node, so `same_gpu` is
  false and WSI blits to a linear exportable buffer.
- Noctalia and Firefox were not re-run on the new default.
- Long-duration stability was not measured.
