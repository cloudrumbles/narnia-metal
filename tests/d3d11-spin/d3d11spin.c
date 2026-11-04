/*
 * d3d11spin: minimal D3D11 test for the Proton path (Wine + DXVK -> Venus).
 * Draws a spinning colored triangle with a constant buffer updated every
 * frame via Map(WRITE_DISCARD), presents N frames, prints the frame rate.
 *
 *   x86_64-w64-mingw32-gcc -O2 -o d3d11spin.exe d3d11spin.c \
 *       -ld3d11 -ldxgi -ld3dcompiler -luuid
 *   wine d3d11spin.exe [frames] [width] [height]
 *
 * SPDX-License-Identifier: MIT
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char shader_src[] =
   "cbuffer cb : register(b0) { float4 rot; float4 tint; };\n"
   "struct vs_out { float4 pos : SV_Position; float4 col : COLOR; };\n"
   "vs_out vs(float2 p : POSITION, float3 c : COLOR) {\n"
   "  vs_out o;\n"
   "  float2 r = float2(p.x * rot.x - p.y * rot.y, p.x * rot.y + p.y * rot.x);\n"
   "  o.pos = float4(r * rot.z, 0.0, 1.0);\n"
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
      fprintf(stderr, "d3d11spin: %s compile failed: %s\n", entry,
              errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
      exit(1);
   }
   return code;
}

#define CHECK(x)                                                              \
   do {                                                                       \
      HRESULT hr_ = (x);                                                      \
      if (FAILED(hr_)) {                                                      \
         fprintf(stderr, "d3d11spin: %s failed: 0x%08lx\n", #x,              \
                 (unsigned long)hr_);                                         \
         return 1;                                                            \
      }                                                                       \
   } while (0)

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
      .lpszClassName = "d3d11spin",
   };
   RegisterClassA(&wc);
   RECT rect = { 0, 0, width, height };
   AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
   HWND hwnd = CreateWindowA("d3d11spin", "d3d11spin", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top,
                             NULL, NULL, wc.hInstance, NULL);
   ShowWindow(hwnd, SW_SHOW);

   DXGI_SWAP_CHAIN_DESC scd = {
      .BufferDesc = {
         .Width = width,
         .Height = height,
         .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
      },
      .SampleDesc = { .Count = 1 },
      .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
      .BufferCount = 2,
      .OutputWindow = hwnd,
      .Windowed = TRUE,
      .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
   };
   IDXGISwapChain *swapchain;
   ID3D11Device *dev;
   ID3D11DeviceContext *ctx;
   D3D_FEATURE_LEVEL level;
   CHECK(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &scd,
                                       &swapchain, &dev, &level, &ctx));
   printf("d3d11spin: feature level 0x%x\n", level);

   ID3D11Texture2D *backbuffer;
   CHECK(IDXGISwapChain_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D,
                                  (void **)&backbuffer));
   ID3D11RenderTargetView *rtv;
   CHECK(ID3D11Device_CreateRenderTargetView(
      dev, (ID3D11Resource *)backbuffer, NULL, &rtv));

   ID3DBlob *vs_code = compile("vs", "vs_4_0");
   ID3DBlob *ps_code = compile("ps", "ps_4_0");
   ID3D11VertexShader *vs;
   ID3D11PixelShader *ps;
   CHECK(ID3D11Device_CreateVertexShader(
      dev, ID3D10Blob_GetBufferPointer(vs_code),
      ID3D10Blob_GetBufferSize(vs_code), NULL, &vs));
   CHECK(ID3D11Device_CreatePixelShader(
      dev, ID3D10Blob_GetBufferPointer(ps_code),
      ID3D10Blob_GetBufferSize(ps_code), NULL, &ps));

   const D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
      { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
        D3D11_INPUT_PER_VERTEX_DATA, 0 },
      { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8,
        D3D11_INPUT_PER_VERTEX_DATA, 0 },
   };
   ID3D11InputLayout *layout;
   CHECK(ID3D11Device_CreateInputLayout(
      dev, layout_desc, 2, ID3D10Blob_GetBufferPointer(vs_code),
      ID3D10Blob_GetBufferSize(vs_code), &layout));

   const struct vertex verts[] = {
      { 0.0f, 0.8f, 1.0f, 0.2f, 0.2f },
      { 0.7f, -0.4f, 0.2f, 1.0f, 0.2f },
      { -0.7f, -0.4f, 0.2f, 0.4f, 1.0f },
   };
   const D3D11_BUFFER_DESC vb_desc = {
      .ByteWidth = sizeof(verts),
      .Usage = D3D11_USAGE_IMMUTABLE,
      .BindFlags = D3D11_BIND_VERTEX_BUFFER,
   };
   const D3D11_SUBRESOURCE_DATA vb_data = { .pSysMem = verts };
   ID3D11Buffer *vb;
   CHECK(ID3D11Device_CreateBuffer(dev, &vb_desc, &vb_data, &vb));

   const D3D11_BUFFER_DESC cb_desc = {
      .ByteWidth = sizeof(struct constants),
      .Usage = D3D11_USAGE_DYNAMIC,
      .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
      .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
   };
   ID3D11Buffer *cb;
   CHECK(ID3D11Device_CreateBuffer(dev, &cb_desc, NULL, &cb));

   const D3D11_VIEWPORT vp = {
      .Width = (float)width,
      .Height = (float)height,
      .MaxDepth = 1.0f,
   };
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

      const float a = i * 0.05f;
      D3D11_MAPPED_SUBRESOURCE map;
      CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &map));
      struct constants *c = map.pData;
      c->rot[0] = cosf(a);
      c->rot[1] = sinf(a);
      c->rot[2] = 1.0f;
      c->rot[3] = aspect;
      c->tint[0] = 0.6f + 0.4f * sinf(a * 0.7f);
      c->tint[1] = 0.6f + 0.4f * sinf(a * 0.9f + 2.0f);
      c->tint[2] = 0.6f + 0.4f * sinf(a * 1.1f + 4.0f);
      c->tint[3] = 1.0f;
      ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);

      const float clear[4] = { 0.05f, 0.05f + 0.1f * (i % 60) / 60.0f, 0.1f,
                               1.0f };
      ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
      ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
      ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
      ID3D11DeviceContext_IASetInputLayout(ctx, layout);
      ID3D11DeviceContext_IASetPrimitiveTopology(
         ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      const UINT stride = sizeof(struct vertex), offset = 0;
      ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
      ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
      ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
      ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
      ID3D11DeviceContext_Draw(ctx, 3, 0);

      CHECK(IDXGISwapChain_Present(swapchain, 1, 0));
   }

   QueryPerformanceCounter(&now);
   const double secs = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
   printf("d3d11spin: %d frames in %.2f s (%.1f fps)\n", frames, secs,
          frames / secs);

   IDXGISwapChain_Release(swapchain);
   ID3D11DeviceContext_Release(ctx);
   ID3D11Device_Release(dev);
   return 0;
}
