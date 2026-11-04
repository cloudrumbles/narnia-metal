# narnia-metal

Vulkan API remoting from a GPU-less cloud box to an iPad. The game runs under
Proton on the box, DXVK/VKD3D turn D3D into Vulkan, Mesa's Venus driver
serialises the calls, and a network backend sends them to the iPad, which runs
virglrenderer's Venus renderer (vkr) on its own GPU and shows frames on a
`CAMetalLayer`.

Status: early. On a Linux test host (lavapipe), Venus over TCP runs
`vulkaninfo`, `vkcube` and a D3D11 app under Proton (DXVK), with presented
frames copied out on the host (remote WSI). No iOS app yet. See `docs/notes.md` for the running
log and `docs/design.md` for how it works.

## Layout

| path | what |
|---|---|
| `common/nm_proto.h` | wire protocol shared by both sides |
| `mesa/` | Venus `net` backend (`files/`) and patches against `mesa-26.2.6` |
| `virglrenderer/` | patches against `virglrenderer-1.3.0` (net hooks, host-pointer blobs, Vulkan loader override) |
| `host/` | host core (`nm_host.c`, transport- and driver-agnostic), driver modules (`nm_driver_*.c`), Linux test host (`main_linux.c`) |
| `box/` | box daemon (planned) |
| `ios/` | iPad app (planned) |
| `tests/d3d11-spin/` | minimal D3D11 app for the Proton/DXVK path |
| `scripts/dev-box-setup.sh` | set up a Debian 13 build box and build everything |

## Try it on Linux

```bash
scripts/dev-box-setup.sh all          # deps, clone, patch, build mesa + vkr + host

# host side (lavapipe)
VK_DRIVER_FILES=/opt/mesa/share/vulkan/icd.d/lvp_icd.x86_64.json host/nm-host tcp:7397

# guest side (Venus over the network)
VK_DRIVER_FILES=/opt/mesa/share/vulkan/icd.d/virtio_icd.x86_64.json \
  VN_NET=127.0.0.1:7397 vulkaninfo --summary
```

`VN_NET` takes `host:port`, `tcp:host:port` or `unix:/path`.

## Swapping the host Vulkan driver

The host core only ever sees a `vkGetInstanceProcAddr`, handed to vkr through
`vkr_renderer_set_get_instance_proc_addr`. A driver module
(`host/nm_driver.h`) provides it, so moving from MoltenVK to KosmicKrisp is a
matter of linking a different module.

## License

MIT for our code. Mesa and virglrenderer are MIT as well.
