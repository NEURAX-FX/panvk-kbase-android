# muic

This directory packages the Anland Vulkan staging changes and the matching userspace PanVK synchronization fix. The patches target separate source repositories; do not apply them to this PanVK packaging repository. No kernel rebuild, kernel patch, module replacement, or flashing is required.

| Patch | Target and required base | Original commit |
| --- | --- | --- |
| `0001-panvk-kbase-anland-staging.patch` | Mesa `13c98a0341a8c0cc093347b76c1b34a451deaf8e` (p0175) | `3be654701a9c11f3d5ba15bc049f548068960f2e` |
| `0002-anland-android-vulkan-output.patch` | Anland `65639aee19e3a744b311c7d0638b617216cff49d` | `727f71196e5da12d9aba03091e030530a4a9e440` |
| `0003-panvk-csf-native-fence-waits.patch` | Mesa after `0001`, with the debug stack below | `e1801782bb1e4b16d7076d991d4f6fff0f6a1e4d` |

SHA-256:
`0001-panvk-kbase-anland-staging.patch`: `d9b46488a97c43ba11775795d37fd292a664742dc9b59883821610138220dda7`
`0002-anland-android-vulkan-output.patch`: `1efc20bb821681ef38506b777d480b6bee35c2e210d597315d5b7b255b13a4f6`
`0003-panvk-csf-native-fence-waits.patch`: `ac8ec4ce20ecb2b5d4e36ef1af94a0e469129db5844c3490007a8289431039e2`

Use Mesa p0175 with `patches/debug/0001`–`0009` applied, then apply Mesa `0001` and `0003` below. Apply Anland `0002` only to its matching checkout:

```sh
git -C "$MESA" am "$PWD/muic/0001-panvk-kbase-anland-staging.patch"
git -C "$MESA" am "$PWD/muic/0003-panvk-csf-native-fence-waits.patch"
git -C "$ANLAND" am "$PWD/muic/0002-anland-android-vulkan-output.patch"
```

The original Mesa change scopes GPU-uncached imports to Anland staging slots and exports the opt-in import API. The Anland change adds Android Vulkan output and the producer/consumer staging and fence handoff.

## Userspace native-fence synchronization

`0003` replaces the CSF submission-time CPU wait for imported `SYNC_FD` semaphores with the existing kbase KCPU `FENCE_WAIT` / `CQS_SET_OPERATION` commands and GPU event waits. The definitions added under Mesa's `include/drm-uapi/` describe the existing kernel ABI; they do not change the kernel. A rejected capability probe retains the CPU path. Failure after a real wait has been queued fails the Vulkan queue instead of publishing work with a missing dependency.

Each Vulkan queue owns separate wait and export KCPU queues. Pending waits on one Vulkan queue no longer hold up another queue's exported fences. Wait-only submits still advance GPU completion targets, and teardown keeps event memory alive until GPU consumers and KCPU commands are terminated. `PANVK_KBASE_GPU_SEMAPHORE_WAITS=0` retains the diagnostic CPU-wait override.

### Hardware verification

Tested on MT6989 / Mali-G720-Immortalis MC12 with the device's existing CSF uAPI 1.21 kernel. Only an isolated AArch64 glibc ICD was selected during the tests; no kernel/module change or desktop restart was performed.

| Check | Result |
| --- | --- |
| Pending imported wait, empty submit | 128 iterations; fd 10 → 10 |
| Pending imported wait and `vkCmdFillBuffer` readback | 128 iterations; fd 10 → 10 |
| Three consecutive pending waits | 128 iterations; fd 10 → 10 |
| Wait-only submit followed by GPU work | 128 iterations; fd 10 → 10 |
| Two independently gated Vulkan queues | 128 iterations; fd 10 → 10 |
| Existing wait/reset/signal/replace/destroy/semaphore/timeout lifecycle modes | 4096 iterations per mode; fd delta 0 |

`sync-fd-submit-probe.c` requires both submission and output-fence export to return while the original input fence remains unsignalled. It checks that completion is not early, signals the input, then checks the output fence and written pixels. Its rescue timeout is a failure, not a pass. These checks prove dependency ordering and asynchronous submission, not a measured desktop FPS improvement.

Cross-build and run from the workspace:

```sh
aarch64-linux-gnu-gcc --sysroot="$SYSROOT" -isystem "$SYSROOT/usr/include" \
  -I mesa/include -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  panvk-kbase-android/muic/sync-fd-submit-probe.c \
  -Wl,-rpath-link,"$SYSROOT/usr/lib64" -l:libvulkan.so.1 \
  -o sync-fd-submit-probe
VK_DRIVER_FILES=/path/to/panvk-native-wait.json ./sync-fd-submit-probe all 128
```

Verified ICD SHA-256: `c246599322813fbaf1e8fc755369c5e69c0a395cbdfef70dae12625c71eac1e5`. It was built before the source commit was recorded, so its version string retains `git-3be654701a`.

### Installed defaults and rollback

After the isolated checks passed, `/opt/panvk/current` was switched from `vgem-8bb86be9` to `native-wait-c2465993`. Anland's explicitly pinned userspace ICD under `/opt/anland/gpu-staging-619719b9/usr/lib64/anland-panvk/` was atomically replaced with the same verified library. The existing kernel, modules, GLVND dispatch and graphics selectors were not changed; no running desktop process was restarted.

A fresh normal login environment, without a test-specific ICD selector, passed all five pending-fence scenarios again (2 iterations each, fd 10 → 10). Existing mapped processes keep their old library until restarted. `ldconfig` reported that the existing `/lib64/libvulkan_wrapper.so` is not a symbolic link; deployment and the default-selected regression both completed successfully.

The previous default stack and the pinned Anland ICD backup are retained. Roll back with:

```sh
bash /root/panvk-wsi-debug/backup-native-wait-c2465993/rollback.sh
```

The rollback script was syntax-checked but not executed. It refuses an unexpected newer default or pinned ICD. This restores userspace libraries only and does not restart processes.
