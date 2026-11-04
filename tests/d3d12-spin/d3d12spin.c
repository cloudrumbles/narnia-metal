/*
 * d3d12spin: minimal D3D12 test for the Proton path (vkd3d-proton -> Venus).
 * Same picture as d3d11spin: a spinning, tinted triangle. The constant
 * buffer lives in a persistently mapped upload heap and is rewritten every
 * frame, frames in flight are tracked with a fence.
 *
 *   x86_64-w64-mingw32-gcc -O2 -o d3d12spin.exe d3d12spin.c \
 *       -ld3d12 -ldxgi -ld3dcompiler -ldxguid -luuid -lm
 *   proton run d3d12spin.exe [frames] [width] [height]
 *
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS /* correct ABI for methods returning structs */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES_IN_FLIGHT 2
#define CB_STRIDE 256

static const char shader_src[] =
   "cbuffer cb : register(b0) { float4 rot; float4 tint; };\n"
   "struct vs_out { float4 pos : SV_Position; float4 col : COLOR; };\n"
   "vs_out vs(float2 p : POSITION, float3 c : COLOR) {\n"
   "  vs_out o;\n"
   "  float2 r = float2(p.x * rot.x - p.y * rot.y, p.x * rot.y + p.y * rot.x);\n"
   "  o.pos = float4(r.x * rot.w, r.y, 0.0, 1.0);\n"
   "  o.col = float4(c * tint.rgb, 1.0);\n"
   "  return o;\n"
   "}\n"
   "float4 ps(vs_out i) : SV_Target { return i.col; }\n";

struct vertex {
   float x, y;
   float r, g, b;
};

struct constants {
   float rot[4];
   float tint[4];
};

#define CHECK(x)                                                              \
   do {                                                                       \
      HRESULT hr_ = (x);                                                      \
      if (FAILED(hr_)) {                                                      \
         fprintf(stderr, "d3d12spin: %s failed: 0x%08lx\n", #x,              \
                 (unsigned long)hr_);                                         \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

static LRESULT CALLBACK
wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   if (msg == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProcA(hwnd, msg, wp, lp);
}

static ID3DBlob *
compile(const char *entry, const char *target)
{
   ID3DBlob *code = NULL, *errors = NULL;
   HRESULT hr = D3DCompile(shader_src, sizeof(shader_src) - 1, "spin", NULL,
                           NULL, entry, target, 0, 0, &code, &errors);
   if (FAILED(hr)) {
      fprintf(stderr, "d3d12spin: %s compile failed: %s\n", entry,
              errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
      exit(1);
   }
   return code;
}

static ID3D12Resource *
upload_buffer(ID3D12Device *dev, UINT64 size)
{
   const D3D12_HEAP_PROPERTIES heap = { .Type = D3D12_HEAP_TYPE_UPLOAD };
   const D3D12_RESOURCE_DESC desc = {
      .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
      .Width = size,
      .Height = 1,
      .DepthOrArraySize = 1,
      .MipLevels = 1,
      .SampleDesc = { .Count = 1 },
      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
   };
   ID3D12Resource *res;
   CHECK(ID3D12Device_CreateCommittedResource(
      dev, &heap, D3D12_HEAP_FLAG_NONE, &desc,
      D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
      (void **)&res));
   return res;
}

static void
transition(ID3D12GraphicsCommandList *list, ID3D12Resource *res,
           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
   const D3D12_RESOURCE_BARRIER barrier = {
      .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Transition = {
         .pResource = res,
         .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
         .StateBefore = before,
         .StateAfter = after,
      },
   };
   ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

int
main(int argc, char **argv)
{
   const int frames = argc > 1 ? atoi(argv[1]) : 300;
   const int width = argc > 2 ? atoi(argv[2]) : 640;
   const int height = argc > 3 ? atoi(argv[3]) : 480;
   setvbuf(stdout, NULL, _IONBF, 0);

   WNDCLASSA wc = {
      .lpfnWndProc = wndproc,
      .hInstance = GetModuleHandleA(NULL),
      .lpszClassName = "d3d12spin",
   };
   RegisterClassA(&wc);
   RECT rect = { 0, 0, width, height };
   AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
   HWND hwnd = CreateWindowA("d3d12spin", "d3d12spin", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top,
                             NULL, NULL, wc.hInstance, NULL);
   ShowWindow(hwnd, SW_SHOW);

   IDXGIFactory4 *factory;
   CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
   ID3D12Device *dev;
   CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                           (void **)&dev));

   const D3D12_COMMAND_QUEUE_DESC queue_desc = {
      .Type = D3D12_COMMAND_LIST_TYPE_DIRECT,
   };
   ID3D12CommandQueue *queue;
   CHECK(ID3D12Device_CreateCommandQueue(dev, &queue_desc,
                                         &IID_ID3D12CommandQueue,
                                         (void **)&queue));

   const DXGI_SWAP_CHAIN_DESC1 sc_desc = {
      .Width = width,
      .Height = height,
      .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
      .SampleDesc = { .Count = 1 },
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = FRAMES_IN_FLIGHT,
      .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
   };
   IDXGISwapChain1 *sc1;
   CHECK(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue,
                                              hwnd, &sc_desc, NULL, NULL,
                                              &sc1));
   IDXGISwapChain3 *swapchain;
   CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3,
                                        (void **)&swapchain));

   const D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {
      .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
      .NumDescriptors = FRAMES_IN_FLIGHT,
   };
   ID3D12DescriptorHeap *rtv_heap;
   CHECK(ID3D12Device_CreateDescriptorHeap(dev, &rtv_heap_desc,
                                           &IID_ID3D12DescriptorHeap,
                                           (void **)&rtv_heap));
   const UINT rtv_size = ID3D12Device_GetDescriptorHandleIncrementSize(
      dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
   const D3D12_CPU_DESCRIPTOR_HANDLE rtv_base =
      ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);

   ID3D12Resource *backbuffers[FRAMES_IN_FLIGHT];
   ID3D12CommandAllocator *allocators[FRAMES_IN_FLIGHT];
   UINT64 frame_fence_values[FRAMES_IN_FLIGHT] = { 0 };
   for (UINT i = 0; i < FRAMES_IN_FLIGHT; i++) {
      CHECK(IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource,
                                      (void **)&backbuffers[i]));
      const D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv_base.ptr + i * rtv_size };
      ID3D12Device_CreateRenderTargetView(dev, backbuffers[i], NULL, h);
      CHECK(ID3D12Device_CreateCommandAllocator(
         dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
         (void **)&allocators[i]));
   }

   /* root signature: one root CBV at b0 */
   const D3D12_ROOT_PARAMETER param = {
      .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
      .Descriptor = { .ShaderRegister = 0 },
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
   };
   const D3D12_ROOT_SIGNATURE_DESC rs_desc = {
      .NumParameters = 1,
      .pParameters = &param,
      .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT,
   };
   ID3DBlob *rs_blob, *rs_err = NULL;
   CHECK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &rs_blob, &rs_err));
   ID3D12RootSignature *root_sig;
   CHECK(ID3D12Device_CreateRootSignature(
      dev, 0, ID3D10Blob_GetBufferPointer(rs_blob),
      ID3D10Blob_GetBufferSize(rs_blob), &IID_ID3D12RootSignature,
      (void **)&root_sig));

   ID3DBlob *vs = compile("vs", "vs_5_0");
   ID3DBlob *ps = compile("ps", "ps_5_0");
   const D3D12_INPUT_ELEMENT_DESC layout[] = {
      { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
      { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8,
        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
   };
   D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {
      .pRootSignature = root_sig,
      .VS = { ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs) },
      .PS = { ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps) },
      .SampleMask = UINT_MAX,
      .RasterizerState = {
         .FillMode = D3D12_FILL_MODE_SOLID,
         .CullMode = D3D12_CULL_MODE_NONE,
         .DepthClipEnable = TRUE,
      },
      .InputLayout = { layout, 2 },
      .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
      .NumRenderTargets = 1,
      .RTVFormats = { DXGI_FORMAT_R8G8B8A8_UNORM },
      .SampleDesc = { .Count = 1 },
   };
   pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
   ID3D12PipelineState *pso;
   CHECK(ID3D12Device_CreateGraphicsPipelineState(
      dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));

   const struct vertex verts[] = {
      { 0.0f, 0.8f, 1.0f, 0.2f, 0.2f },
      { 0.7f, -0.4f, 0.2f, 1.0f, 0.2f },
      { -0.7f, -0.4f, 0.2f, 0.4f, 1.0f },
   };
   ID3D12Resource *vb = upload_buffer(dev, sizeof(verts));
   void *vb_map;
   CHECK(ID3D12Resource_Map(vb, 0, NULL, &vb_map));
   memcpy(vb_map, verts, sizeof(verts));
   ID3D12Resource_Unmap(vb, 0, NULL);
   const D3D12_VERTEX_BUFFER_VIEW vbv = {
      .BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vb),
      .SizeInBytes = sizeof(verts),
      .StrideInBytes = sizeof(struct vertex),
   };

   ID3D12Resource *cb = upload_buffer(dev, CB_STRIDE * FRAMES_IN_FLIGHT);
   uint8_t *cb_map;
   CHECK(ID3D12Resource_Map(cb, 0, NULL, (void **)&cb_map));
   const D3D12_GPU_VIRTUAL_ADDRESS cb_va =
      ID3D12Resource_GetGPUVirtualAddress(cb);

   ID3D12GraphicsCommandList *list;
   CHECK(ID3D12Device_CreateCommandList(
      dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], pso,
      &IID_ID3D12GraphicsCommandList, (void **)&list));
   ID3D12GraphicsCommandList_Close(list);

   ID3D12Fence *fence;
   CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE,
                                  &IID_ID3D12Fence, (void **)&fence));
   UINT64 fence_value = 0;
   HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);

   const D3D12_VIEWPORT vp = {
      .Width = (float)width,
      .Height = (float)height,
      .MaxDepth = 1.0f,
   };
   const D3D12_RECT scissor = { 0, 0, width, height };
   const float aspect = (float)height / (float)width;

   LARGE_INTEGER freq, start, now;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&start);

   for (int i = 0; i < frames; i++) {
      MSG msg;
      while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
         TranslateMessage(&msg);
         DispatchMessageA(&msg);
      }

      const UINT bb = IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
      if (ID3D12Fence_GetCompletedValue(fence) < frame_fence_values[bb]) {
         ID3D12Fence_SetEventOnCompletion(fence, frame_fence_values[bb],
                                          event);
         WaitForSingleObject(event, INFINITE);
      }

      const float a = i * 0.05f;
      struct constants *c = (struct constants *)(cb_map + bb * CB_STRIDE);
      c->rot[0] = cosf(a);
      c->rot[1] = sinf(a);
      c->rot[2] = 1.0f;
      c->rot[3] = aspect;
      c->tint[0] = 0.6f + 0.4f * sinf(a * 0.7f);
      c->tint[1] = 0.6f + 0.4f * sinf(a * 0.9f + 2.0f);
      c->tint[2] = 0.6f + 0.4f * sinf(a * 1.1f + 4.0f);
      c->tint[3] = 1.0f;

      CHECK(ID3D12CommandAllocator_Reset(allocators[bb]));
      CHECK(ID3D12GraphicsCommandList_Reset(list, allocators[bb], pso));
      transition(list, backbuffers[bb], D3D12_RESOURCE_STATE_PRESENT,
                 D3D12_RESOURCE_STATE_RENDER_TARGET);
      const D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtv_base.ptr + bb * rtv_size };
      const float clear[4] = { 0.1f, 0.05f, 0.05f + 0.1f * (i % 60) / 60.0f,
                               1.0f };
      ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0,
                                                      NULL);
      ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE,
                                                   NULL);
      ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root_sig);
      ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
         list, 0, cb_va + bb * CB_STRIDE);
      ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
      ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &scissor);
      ID3D12GraphicsCommandList_IASetPrimitiveTopology(
         list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
      ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
      transition(list, backbuffers[bb], D3D12_RESOURCE_STATE_RENDER_TARGET,
                 D3D12_RESOURCE_STATE_PRESENT);
      CHECK(ID3D12GraphicsCommandList_Close(list));

      ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
      ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
      CHECK(IDXGISwapChain3_Present(swapchain, 1, 0));
      CHECK(ID3D12CommandQueue_Signal(queue, fence, ++fence_value));
      frame_fence_values[bb] = fence_value;
   }

   /* drain */
   CHECK(ID3D12CommandQueue_Signal(queue, fence, ++fence_value));
   ID3D12Fence_SetEventOnCompletion(fence, fence_value, event);
   WaitForSingleObject(event, INFINITE);

   QueryPerformanceCounter(&now);
   const double secs = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
   printf("d3d12spin: %d frames in %.2f s (%.1f fps)\n", frames, secs,
          frames / secs);
   return 0;
}
