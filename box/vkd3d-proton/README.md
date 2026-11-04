# vkd3d-proton for KosmicKrisp hosts

Proton's D3D12 (vkd3d-proton) refuses to create a device on KosmicKrisp:
"Lacking support for transform feedback". These patches, applied to the
vkd3d-proton commit GE-Proton11-12 ships (`PIN`, upstream 8293130eb), make it
start and pick sensible shader models there. `build.sh` builds the 64-bit
d3d12.dll / d3d12core.dll with mingw and installs them into a Proton dir.

Patches (LGPL-2.1-or-later, like vkd3d-proton) come from NutsEmu 0.0.1 build
1382 (Evan Bolton) via Madeira (github.com/willfaust/Madeira,
build/vkd3d-proton/patches), each with its origin in the header:

- 0001: no transform feedback / single-texel alignment is a warning, not a
  device-creation failure (D3D12 stream output is then unavailable).
- 0002: don't query VK_KHR_maintenance7 layered-API properties (winevulkan
  faulted converting them back).
- 0003: on VK_DRIVER_ID_MESA_KOSMICKRISP only: feature level 12_0 without
  tiled resources, shader model past 6.0 without float32 denormal control,
  the compute-derivatives fallback for 6.6.
