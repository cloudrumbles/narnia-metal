# KosmicKrisp patches

Applied in order to Mesa at `kosmickrisp/PIN` by `ios/build-kk.sh`.

0001-0003 are from Madeira (github.com/willfaust/Madeira,
`build/mesa-ios/patches/kosmickrisp-ios-000{1,2,3}-*.patch`), which offers
its Mesa patches under Mesa's licence (MIT). Thanks to its author.

- `0001-wsi-metal-no-display-sync-on-ios.patch`: `CAMetalLayer.displaySyncEnabled`
  doesn't exist on iOS; skip it there.
- `0002-link-system-frameworks.patch`: link Foundation, CoreGraphics and libobjc
  into the dylib instead of relying on `-undefined dynamic_lookup`.
- `0003-export-get-proc-addr-on-apple.patch`: export `vkGetInstanceProcAddr` /
  `vkGetDeviceProcAddr` on Apple platforms, so the dylib can be dlopen()ed and
  used without a Vulkan loader (how vkr uses it).

Madeira's 0004 (Metal 4 drawable sync) and 0005 (release presented
drawables) are not needed on Mesa main: upstream fixed both ("kk: Wait on
drawable availability before rendering to it", 2026-07-23, and the queue now
releases drawables on submit).
