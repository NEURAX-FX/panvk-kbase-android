# muic

This directory packages the two `muic` changes for the Android Vulkan staging path. The patches target separate source repositories; do not apply them to this PanVK packaging repository.

| Patch | Target and required base | Original commit |
| --- | --- | --- |
| `0001-panvk-kbase-anland-staging.patch` | Mesa `13c98a0341a8c0cc093347b76c1b34a451deaf8e` (p0175) | `3be654701a9c11f3d5ba15bc049f548068960f2e` |
| `0002-anland-android-vulkan-output.patch` | Anland `65639aee19e3a744b311c7d0638b617216cff49d` | `727f71196e5da12d9aba03091e030530a4a9e440` |
SHA-256:
`0001-panvk-kbase-anland-staging.patch`: `d9b46488a97c43ba11775795d37fd292a664742dc9b59883821610138220dda7`
`0002-anland-android-vulkan-output.patch`: `1efc20bb821681ef38506b777d480b6bee35c2e210d597315d5b7b255b13a4f6`

Apply each patch only to its matching checkout, at the listed base (or a compatible descendant):

```sh
git -C "$MESA" am "$PWD/muic/0001-panvk-kbase-anland-staging.patch"
git -C "$ANLAND" am "$PWD/muic/0002-anland-android-vulkan-output.patch"
```

The Mesa change scopes GPU-uncached imports to Anland staging slots and exports the opt-in import API. The Anland change adds Android Vulkan output and the producer/consumer staging and fence handoff. These are source patches, not a built artifact or a claim of hardware validation.
