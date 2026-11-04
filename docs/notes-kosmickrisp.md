# KosmicKrisp as the host driver (2026-11-04)

shah's call: switch the host Vulkan driver from MoltenVK to KosmicKrisp
(LunarG's Mesa Vulkan-on-Metal driver) and stop the MoltenVK work. The
MoltenVK state is kept on branch `dxvk-moltenvk` (d3d11spin did render on
the CI Mac through MoltenVK built from the geometry-shader PR #2812 plus our
pipeline-module fix; frames in docs/evidence/ there).

## Why KosmicKrisp

Mesa main (checked 2026-11-04) has, natively, what DXVK / vkd3d-proton need
and MoltenVK lacks: geometryShader ("kk: Implement geometry shaders with
poly", 2026-10-29, Mesa's shared GS-via-compute lowering), VK_EXT_depth_clip_enable,
robustness2 (robustBufferAccess2, nullDescriptor), logicOp, shaderCullDistance,
tessellation, depthBounds (Apple10+). Missing: pipelineStatisticsQuery,
transform feedback. Porting those back into MoltenVK isn't practical
(NIR/MSL compiler vs SPIRV-Cross), so we swap drivers behind host/nm_driver.h.

## iOS

Upstream: "No iOS support is present as of now" (docs/drivers/kosmickrisp.rst),
open MR !39186 "kk: fix build for iOS" (osy, 2026-01). Madeira
(github.com/willfaust/Madeira) ships KosmicKrisp on iOS 26 and offers its
Mesa patches under MIT; we carry its 0001-0003 (kosmickrisp/patches/README.md).
Its 0004/0005 (Metal 4 drawable sync, releasing presented drawables) are
already fixed on Mesa main.

KosmicKrisp needs Metal 4: macOS 26 / iOS 26 runtime and Xcode 26 SDKs.
shah's iPad: iPad13,8 (M1), iOS 26.5.2.

## Build

`ios/build-kk.sh macos` then `ios/build-kk.sh ios` (on a macOS 26 runner):
1. Homebrew llvm + spirv-llvm-translator + spirv-tools, python venv with
   meson/mako/pyyaml/packaging.
2. Mesa at kosmickrisp/PIN (shallow fetch of one commit) + patches.
3. Native build: macOS dylib + the build-machine tools mesa_clc,
   vtn_bindgen2, kk_clc.
4. iOS cross build (meson cross file, subsystem ios, min 26.0) using those
   tools (-Dmesa-clc=system -Dprecomp-compiler=system), LLVM off.

The app embeds the dylib in Frameworks/ and host/nm_driver_kosmickrisp.c
dlopen()s it (vkGetInstanceProcAddr exported by patch 0003). Build the app
with `NM_DRIVER=kosmickrisp ios/build.sh ios`. CI: `gh workflow run apple.yml
--ref <branch> -f driver=kosmickrisp [-f live_minutes=N]` (macos-26 runner,
cached Mesa build, live test via secret NM_LIVE_RELAY_KK -> relay on the box
port 7404, socket /tmp/nm-kk.sock, telemetry /tmp/nm-kk.log).

## Status / log

- First CI run (macos-26): the runner's GPU is "Apple Paravirtual device":
  `newMTL4CommandQueue` returns nil and it isn't Apple7 family, so
  **KosmicKrisp can't run on GitHub's Macs**; live tests must use the iPad.
  CI builds only the tools (native) and the iOS dylib.
- Mesa main's kk_bo.c uses `mach_vm_remap/map/deallocate`, which the iOS SDK
  doesn't have (mach_vm.h "unsupported"; Madeira's Mesa 26.2.4 predates that
  code). Patch 0004 uses the equivalent `vm_*` calls on iOS.
- Homebrew's llvm on the runner is 23.1.4 (llvm@20 is also installed); the
  native tools build fine with it.
