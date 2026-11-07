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

[executed on device: fedora (fe2b6f62-ce8b-4f66-8e11-197206b1fbed)]