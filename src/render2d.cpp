#include "render2d.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <string.h>
#include <algorithm>
#include <cmath>

#include "theme.h"

namespace app {
void trace(const std::wstring &s);
}

namespace {

const char *kVs = R"HLSL(
struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
cbuffer View : register(b0) { float2 extent; float2 pad0; };
VS_OUT VSMain(VS_IN i) {
  VS_OUT o;
  o.pos = float4(i.pos.x / extent.x * 2.0 - 1.0, -(i.pos.y / extent.y * 2.0 - 1.0), 0.0, 1.0);
  o.uv = i.uv;
  o.col = i.col;
  return o;
})HLSL";

const char *kPs = R"HLSL(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 PSMain(float4 pos : SV_POSITION, float2 uv : TEXCOORD0, float4 col : COLOR0) : SV_TARGET {
  return tex.Sample(samp, uv) * col;
})HLSL";

constexpr uint32_t kMaxVerts = 3072;  // 512 矩形

inline void uv_rect(float uv[8], float u0, float v0, float u1, float v1) {
  uv[0] = u0; uv[1] = v0;
  uv[2] = u1; uv[3] = v0;
  uv[4] = u1; uv[5] = v1;
  uv[6] = u0; uv[7] = v1;
}

inline void pts_rect(float pts[8], float x, float y, float w, float h) {
  pts[0] = x; pts[1] = y;
  pts[2] = x + w; pts[3] = y;
  pts[4] = x + w; pts[5] = y + h;
  pts[6] = x; pts[7] = y + h;
}

}  // namespace

bool Render2D::init(HWND hwnd) {
  auto bail = [&](const wchar_t *what, HRESULT h) {
    wchar_t b[128];
    swprintf(b, 128, L"%ls（hr=0x%08X）", what, unsigned(h));
    fail_ = b;
    return false;
  };
  RECT rc;
  GetClientRect(hwnd, &rc);
  width_ = rc.right - rc.left;
  height_ = rc.bottom - rc.top;

  D3D_FEATURE_LEVEL fl{};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device_, &fl, &ctx_);
  if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                         D3D11_SDK_VERSION, &device_, &fl, &ctx_);
  if (FAILED(hr)) return bail(L"D3D11CreateDevice 失败", hr);

  IDXGIDevice *dxgi_dev = nullptr;
  IDXGIAdapter *adapter = nullptr;
  IDXGIFactory2 *factory = nullptr;
  if (FAILED(hr = device_->QueryInterface(IID_PPV_ARGS(&dxgi_dev)))) return bail(L"QueryInterface(IDXGIDevice)", hr);
  if (FAILED(hr = dxgi_dev->GetAdapter(&adapter))) { dxgi_dev->Release(); return bail(L"GetAdapter", hr); }
  if (FAILED(hr = adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    adapter->Release(); dxgi_dev->Release(); return bail(L"GetParent(IDXGIFactory2)", hr);
  }
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Width = UINT(width_ > 0 ? width_ : 1);
  sd.Height = UINT(height_ > 0 ? height_ : 1);
  hr = factory->CreateSwapChainForHwnd(device_, hwnd, &sd, nullptr, nullptr, &swap_);
  factory->Release();
  adapter->Release();
  dxgi_dev->Release();
  if (FAILED(hr)) return bail(L"CreateSwapChainForHwnd 失败", hr);

  ID3DBlob *err = nullptr;
  auto compile = [&](const char *src, const char *entry, const char *target, ID3DBlob **out) {
    HRESULT h = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(h)) {
      wchar_t w[256] = L"";
      if (err) MultiByteToWideChar(CP_UTF8, 0, static_cast<const char *>(err->GetBufferPointer()),
                                   -1, w, 256);
      wchar_t b[512];
      swprintf(b, 512, L"着色器 %hs 编译失败（hr=0x%08X）%ls", target, unsigned(h), w);
      fail_ = b;
    }
    return SUCCEEDED(h);
  };
  ID3DBlob *vsb = nullptr, *psb = nullptr;
  if (!compile(kVs, "VSMain", "vs_5_0", &vsb) || !compile(kPs, "PSMain", "ps_5_0", &psb)) {
    if (err) { err->Release(); }
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    return false;
  }
  device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
  device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
  // 顶点格式必须与 VS 签名匹配：没有输入布局时 D3D11 不会从顶点缓冲取数据。
  const D3D11_INPUT_ELEMENT_DESC elems[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT,
       D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(hr = device_->CreateInputLayout(elems, 3, vsb->GetBufferPointer(),
                                             vsb->GetBufferSize(), &layout_))) {
    vsb->Release();
    psb->Release();
    return bail(L"CreateInputLayout 失败", hr);
  }
  vsb->Release();
  psb->Release();
  if (!vs_ || !ps_) return bail(L"创建着色器失败", E_FAIL);

  ID3D11Texture2D *back = nullptr;
  if (FAILED(hr = swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return bail(L"GetBuffer", hr);
  device_->CreateRenderTargetView(back, nullptr, &rtv_);
  back->Release();
  if (!rtv_) return bail(L"CreateRenderTargetView 失败", E_FAIL);

  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = kMaxVerts * sizeof(R2v);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  device_->CreateBuffer(&bd, nullptr, &vb_);

  struct Extent { float x, y, pad[2]; } ext{float(width_), float(height_), 0, 0};
  D3D11_BUFFER_DESC cd{};
  cd.ByteWidth = sizeof(Extent);
  cd.Usage = D3D11_USAGE_DYNAMIC;
  cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  D3D11_SUBRESOURCE_DATA cdr{&ext, sizeof(Extent), 0};
  device_->CreateBuffer(&cd, &cdr, &cb_);

  D3D11_SAMPLER_DESC sm{};
  sm.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sm.MaxLOD = D3D11_FLOAT32_MAX;
  device_->CreateSamplerState(&sm, &sampler_);

  D3D11_BLEND_DESC bl{};
  bl.RenderTarget[0].BlendEnable = TRUE;
  bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  hr = device_->CreateBlendState(&bl, &blend_);
  if (FAILED(hr) || !blend_) return bail(L"CreateBlendState 失败", hr);

  D3D11_RASTERIZER_DESC rs{};
  rs.FillMode = D3D11_FILL_SOLID;
  rs.CullMode = D3D11_CULL_NONE;
  rs.ScissorEnable = TRUE;
  hr = device_->CreateRasterizerState(&rs, &rs_);
  if (FAILED(hr) || !rs_) return bail(L"CreateRasterizerState 失败", hr);

  D3D11_TEXTURE2D_DESC wd{};
  wd.Width = wd.Height = 1;
  wd.MipLevels = wd.ArraySize = 1;
  wd.SampleDesc.Count = 1;
  wd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  wd.Usage = D3D11_USAGE_IMMUTABLE;
  wd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  static const uint8_t white[4] = {255, 255, 255, 255};
  D3D11_SUBRESOURCE_DATA wrd{const_cast<uint8_t *>(white), 4, 0};
  hr = device_->CreateTexture2D(&wd, &wrd, &white_tex_);
  if (FAILED(hr) || !white_tex_) return bail(L"CreateTexture2D(white) 失败", hr);
  hr = device_->CreateShaderResourceView(white_tex_, nullptr, &white_srv_);
  if (FAILED(hr) || !white_srv_) return bail(L"白纹理 SRV 失败", hr);
  if (!vb_) return bail(L"CreateBuffer(顶点) 失败", E_FAIL);
  if (!cb_) return bail(L"CreateBuffer(常量) 失败", E_FAIL);
  if (!sampler_) return bail(L"CreateSamplerState 失败", E_FAIL);

  vp_ = {0, 0, float(width_), float(height_), 0, 1};

  mem_dc_ = CreateCompatibleDC(nullptr);
  verts_.reserve(kMaxVerts);
  return true;
}

void Render2D::shutdown() {
  for (auto &kv : scratch_) { if (kv.second.srv) kv.second.srv->Release(); if (kv.second.tex) kv.second.tex->Release(); }
  for (auto &kv : text_srv_) { if (kv.second.srv) kv.second.srv->Release(); if (kv.second.tex) kv.second.tex->Release(); }
  scratch_.clear();
  text_srv_.clear();
  text_w_.clear();
  for (auto &kv : fonts_) DeleteObject(kv.second);
  fonts_.clear();
  if (mem_dc_) { DeleteDC(mem_dc_); mem_dc_ = nullptr; }
#define REL(p)              \
  if (p) {                  \
    (p)->Release();         \
    (p) = nullptr;          \
  }
  REL(swap_) REL(rtv_) REL(vs_) REL(ps_) REL(vb_) REL(cb_) REL(sampler_) REL(blend_)
  REL(layout_) REL(white_srv_) REL(white_tex_) REL(ctx_) REL(device_)
#undef REL
}

void Render2D::resize(int w, int h) {
  if (!swap_ || w <= 0 || h <= 0 || (w == width_ && h == height_)) return;
  width_ = w;
  height_ = h;
  if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
  swap_->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, 0);
  ID3D11Texture2D *back = nullptr;
  if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) {
    device_->CreateRenderTargetView(back, nullptr, &rtv_);
    back->Release();
  }
  vp_ = {0, 0, float(w), float(h), 0, 1};
  struct Extent { float x, y, pad[2]; } ext{float(w), float(h), 0, 0};
  if (cb_) {
    // DYNAMIC 常量缓冲只能靠 Map 更新。
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      memcpy(ms.pData, &ext, sizeof(ext));
      ctx_->Unmap(cb_, 0);
    }
  }
}

void Render2D::flush() {
  if (verts_.empty() || !vb_) return;
  D3D11_MAPPED_SUBRESOURCE map{};
  if (FAILED(ctx_->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { verts_.clear(); return; }
  size_t bytes = verts_.size() * sizeof(R2v);
  if (bytes > map.RowPitch) bytes = map.RowPitch;
  memcpy(map.pData, verts_.data(), bytes);
  ctx_->Unmap(vb_, 0);
  UINT stride = sizeof(R2v), offset = 0;
  ctx_->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
  ID3D11ShaderResourceView *srv = batch_srv_ ? batch_srv_ : white_srv_;
  ctx_->PSSetShaderResources(0, 1, &srv);
  ctx_->Draw(UINT(verts_.size()), 0);
  verts_.clear();
}

void Render2D::begin_frame() {
  if (!ctx_ || !rtv_) return;
  verts_.clear();
  batch_srv_ = nullptr;
  clips_.clear();
  scissor_set_ = {-1, -1, -1, -1};
  // ScissorEnable 打开后，「没绑定裁剪矩形」不等于不裁剪，而是整个目标都裁掉：
  // 0 个矩形时 D3D11 用的是空区域，一帧像素都画不出来。所以每帧先显式铺满。
  D3D11_RECT full{0, 0, LONG(width_), LONG(height_)};
  ctx_->RSSetScissorRects(1, &full);
  scissor_set_ = full;
  float clear[4] = {btheme::window_back.r / 255.f, btheme::window_back.g / 255.f,
                    btheme::window_back.b / 255.f, 1.f};
  ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
  ctx_->ClearRenderTargetView(rtv_, clear);
  ctx_->RSSetViewports(1, &vp_);
  ctx_->IASetInputLayout(layout_);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(vs_, nullptr, 0);
  ctx_->PSSetShader(ps_, nullptr, 0);
  ctx_->VSSetConstantBuffers(0, 1, &cb_);
  ctx_->PSSetSamplers(0, 1, &sampler_);
  if (blend_) ctx_->OMSetBlendState(blend_, nullptr, 0xffffffff);
  // 默认光栅器状态是 CULL_BACK + ScissorEnable=FALSE：不绑自己那份，裁剪栈等于没做，
  // 镜像过的条带还会因为绕序反转被整个剔掉。
  if (rs_) ctx_->RSSetState(rs_);
}

void Render2D::end_frame(bool vsync) {
  if (!swap_) return;
  flush();
  swap_->Present(vsync ? 1 : 0, 0);
}

// 裁剪栈：Blender 的每个 region 只在自己的矩形内绘制（预览画布缩放后不会溢到时间线上），
// 这里用 D3D11 scissor 达到同样效果。
void Render2D::apply_clip() {
  if (!ctx_) return;
  Rect r = clips_.empty() ? Rect{0, 0, float(width_), float(height_)} : clips_.back();
  D3D11_RECT s{LONG(std::max(0.0f, r.x)), LONG(std::max(0.0f, r.y)),
               LONG(std::min(float(width_), r.x + r.w)),
               LONG(std::min(float(height_), r.y + r.h))};
  if (s.right <= s.left) s.right = s.left + 1;   // D3D 不接受空矩形
  if (s.bottom <= s.top) s.bottom = s.top + 1;
  if (memcmp(&s, &scissor_set_, sizeof(s)) == 0) return;
  flush();  // 批次里已入队的顶点按旧裁剪绘制，改裁剪前必须先出队
  ctx_->RSSetScissorRects(1, &s);
  scissor_set_ = s;
}

void Render2D::push_clip(const Rect &r) {
  Rect cur = clips_.empty() ? Rect{0, 0, float(width_), float(height_)} : clips_.back();
  float l = std::max(cur.x, r.x), t = std::max(cur.y, r.y);
  float rr = std::min(cur.x + cur.w, r.x + r.w), bb = std::min(cur.y + cur.h, r.y + r.h);
  clips_.push_back({l, t, std::max(0.0f, rr - l), std::max(0.0f, bb - t)});
  apply_clip();
}

void Render2D::pop_clip() {
  if (!clips_.empty()) clips_.pop_back();
  apply_clip();
}

void Render2D::push_vert(float x, float y, float u, float v, uint32_t col) {
  if (verts_.size() + 1 > kMaxVerts) flush();
  verts_.push_back({x, y, u, v, col});
}

void Render2D::quad(const float pts[8], const float uv[8], uint32_t col, Tex *tex) {
  ID3D11ShaderResourceView *srv = tex ? tex->srv : white_srv_;
  if (srv != batch_srv_ && !verts_.empty()) flush();
  batch_srv_ = srv;
  push_vert(pts[0], pts[1], uv[0], uv[1], col);
  push_vert(pts[2], pts[3], uv[2], uv[3], col);
  push_vert(pts[4], pts[5], uv[4], uv[5], col);
  push_vert(pts[0], pts[1], uv[0], uv[1], col);
  push_vert(pts[4], pts[5], uv[4], uv[5], col);
  push_vert(pts[6], pts[7], uv[6], uv[7], col);
}

void Render2D::rect_uv(const Rect &r, const float uv[8], uint32_t col, Tex *tex) {
  float pts[8];
  pts_rect(pts, r.x, r.y, r.w, r.h);
  quad(pts, uv, col, tex);
}

void Render2D::rect(const Rect &r, Color c) {
  float uv[8];
  uv_rect(uv, 0, 0, 1, 1);
  rect_uv(r, uv, c.pack(), nullptr);
}

void Render2D::rect_border(const Rect &r, Color fill, Color border) {
  float uv[8];
  uv_rect(uv, 0, 0, 1, 1);
  uint32_t f = fill.pack(), b = border.pack();
  rect_uv({r.x, r.y, r.w, 1}, uv, b, nullptr);
  rect_uv({r.x, r.y + r.h - 1, r.w, 1}, uv, b, nullptr);
  rect_uv({r.x, r.y, 1, r.h}, uv, b, nullptr);
  rect_uv({r.x + r.w - 1, r.y, 1, r.h}, uv, b, nullptr);
  rect_uv(r.inset(1, 1, 1, 1), uv, f, nullptr);
}

void Render2D::tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
  if (white_srv_ != batch_srv_ && !verts_.empty()) flush();
  batch_srv_ = white_srv_;
  uint32_t col = c.pack();
  push_vert(x0, y0, 0, 0, col);
  push_vert(x1, y1, 0, 0, col);
  push_vert(x2, y2, 0, 0, col);
}

// 圆角半径不能超过短边的一半（Blender 在 shader 里也把半径夹到半宽，见
// gpu_shader_sequencer_widget.bsl.hh:47）。
static float clamp_rad(float radius, float w, float h) {
  return std::min(radius, std::min(w, h) * 0.5f);
}

void Render2D::rrect(const Rect &r, float radius, Color c) {
  constexpr float kPi = 3.14159265358979f;
  float rad = clamp_rad(radius, r.w, r.h);
  if (rad <= 0.5f) {
    rect(r, c);
    return;
  }
  rect({r.x + rad, r.y, r.w - 2 * rad, r.h}, c);
  rect({r.x, r.y + rad, rad, r.h - 2 * rad}, c);
  rect({r.x + r.w - rad, r.y + rad, rad, r.h - 2 * rad}, c);
  // 四角扇形：左上 180 度起，顺时针每角 90 度。
  const float cen[4][2] = {{r.x + rad, r.y + rad},
                           {r.x + r.w - rad, r.y + rad},
                           {r.x + r.w - rad, r.y + r.h - rad},
                           {r.x + rad, r.y + r.h - rad}};
  constexpr int kSeg = 8;
  for (int q = 0; q < 4; ++q) {
    float a0 = (180.0f + q * 90.0f) * kPi / 180.0f;
    for (int i = 0; i < kSeg; ++i) {
      float s = a0 + float(i) * (kPi / 2) / kSeg, e = a0 + float(i + 1) * (kPi / 2) / kSeg;
      tri(cen[q][0], cen[q][1], cen[q][0] + rad * std::cos(s), cen[q][1] + rad * std::sin(s),
          cen[q][0] + rad * std::cos(e), cen[q][1] + rad * std::sin(e), c);
    }
  }
}

void Render2D::rring(const Rect &r, float radius, float t, Color c) {
  constexpr float kPi = 3.14159265358979f;
  float rad = clamp_rad(radius, r.w, r.h);
  if (rad <= 0.5f) {
    rect({r.x, r.y, r.w, t}, c);
    rect({r.x, r.y + r.h - t, r.w, t}, c);
    rect({r.x, r.y + t, t, r.h - 2 * t}, c);
    rect({r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c);
    return;
  }
  rect({r.x + rad, r.y, r.w - 2 * rad, t}, c);
  rect({r.x + rad, r.y + r.h - t, r.w - 2 * rad, t}, c);
  rect({r.x, r.y + rad, t, r.h - 2 * rad}, c);
  rect({r.x + r.w - t, r.y + rad, t, r.h - 2 * rad}, c);
  float ri = std::max(0.0f, rad - t);
  const float cen[4][2] = {{r.x + rad, r.y + rad},
                           {r.x + r.w - rad, r.y + rad},
                           {r.x + r.w - rad, r.y + r.h - rad},
                           {r.x + rad, r.y + r.h - rad}};
  constexpr int kSeg = 8;
  for (int q = 0; q < 4; ++q) {
    float a0 = (180.0f + q * 90.0f) * kPi / 180.0f;
    for (int i = 0; i < kSeg; ++i) {
      float s = a0 + float(i) * (kPi / 2) / kSeg, e = a0 + float(i + 1) * (kPi / 2) / kSeg;
      float ox0 = cen[q][0] + rad * std::cos(s), oy0 = cen[q][1] + rad * std::sin(s);
      float ox1 = cen[q][0] + rad * std::cos(e), oy1 = cen[q][1] + rad * std::sin(e);
      float ix0 = cen[q][0] + ri * std::cos(s), iy0 = cen[q][1] + ri * std::sin(s);
      float ix1 = cen[q][0] + ri * std::cos(e), iy1 = cen[q][1] + ri * std::sin(e);
      tri(ox0, oy0, ox1, oy1, ix0, iy0, c);
      tri(ox1, oy1, ix1, iy1, ix0, iy0, c);
    }
  }
}

void Render2D::line_seg(float x0, float y0, float x1, float y1, float width, Color c) {
  float dx = x1 - x0, dy = y1 - y0;
  float len = std::sqrt(dx * dx + dy * dy);
  if (len <= 1e-4f) return;
  float nx = -dy / len * (width * 0.5f), ny = dx / len * (width * 0.5f);
  tri(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
  tri(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

Render2D::Tex *Render2D::scratch(int w, int h) {
  uint64_t key = (uint64_t(w) << 32) | uint32_t(h);
  auto it = scratch_.find(key);
  if (it != scratch_.end()) return &it->second;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  Tex entry{};
  if (FAILED(device_->CreateTexture2D(&td, nullptr, &entry.tex))) return nullptr;
  if (FAILED(device_->CreateShaderResourceView(entry.tex, nullptr, &entry.srv))) {
    entry.tex->Release();
    return nullptr;
  }
  entry.w = w;
  entry.h = h;
  return &scratch_.emplace(key, entry).first->second;
}

void Render2D::upload_bgra(Tex *t, const uint8_t *bgra, int w, int h) {
  // 覆盖前若未提交的批次正采着这张纹理，先把那一笔画掉。scratch 按尺寸复用：同源两条带
  // 解码尺寸相同就共用一张纹理，批次又延迟到 flush 才画——不先出队，先入队的那条会采到
  // 后上传的内容，两幅画面变成同一帧（上面那条盖掉下面那条）。
  if (t->srv == batch_srv_ && !verts_.empty()) flush();
  static auto ms = [] {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t2;
    QueryPerformanceCounter(&t2);
    return double(t2.QuadPart) * 1000.0 / double(freq.QuadPart);
  };
  static double up = 0;
  static int n = 0;
  const double t0 = ms();
  ctx_->UpdateSubresource(t->tex, 0, nullptr, bgra, UINT(w * 4), 0);
  const double t1 = ms();
  up += t1 - t0;
  if (++n >= 100) {
    wchar_t buf[160];
    swprintf(buf, 160, L"上传分帧 %d 次: 上传 %.2f ms/次", n, up / n);
    app::trace(buf);
    up = 0;
    n = 0;
  }
}

void Render2D::image(const Rect &r, const uint8_t *bgra, int w, int h, Color tint,
                     int crop_l, int crop_r, int crop_t, int crop_b, uint64_t token) {
  if (!bgra || w <= 0 || h <= 0) return;
  Tex *tex = scratch(w, h);
  if (!tex) return;
  if (!token || token != tex->token) {
    upload_bgra(tex, bgra, w, h);
    tex->token = token;
  }
  float u0 = crop_l / float(w), u1 = 1.0f - crop_r / float(w);
  float v0 = crop_t / float(h), v1 = 1.0f - crop_b / float(h);
  float uv[8];
  uv_rect(uv, u0, v0, u1, v1);
  rect_uv(r, uv, tint.pack(), tex);
}

void Render2D::image_affine(const Rect &r, float dx, float dy, float scale_x, float scale_y,
                            float degrees, const uint8_t *bgra, int w, int h, Color tint,
                            int crop_l, int crop_r, int crop_t, int crop_b, uint64_t token,
                            bool flip_x, bool flip_y, float origin_x, float origin_y) {
  if (!bgra || w <= 0 || h <= 0) return;
  Tex *tex = scratch(w, h);
  if (!tex) return;
  if (!token || token != tex->token) {
    upload_bgra(tex, bgra, w, h);
    tex->token = token;
  }
  float u0 = crop_l / float(w), u1 = 1.0f - crop_r / float(w);
  float v0 = crop_t / float(h), v1 = 1.0f - crop_b / float(h);
  // 裁切越界到没有像素可留 = 什么都不画（Blender 的 BLI_rctf_isect 之后 is_empty 直接返回）。
  if (u0 >= u1 || v0 >= v1) return;
  float uv[8];
  uv_rect(uv, u0, v0, u1, v1);

  // 裁切是「源像素空间的遮罩」：sequencer_image_crop_init（render.cc:458）给出
  // (left, w-right, bottom, h-top)，should_discard（imbuf/intern/transform.cc:95）拿
  // 源 uv 直接比对丢弃。留下的像素位置与大小都不变 —— 所以顶点必须按裁切子矩形在整幅
  // 画面里的实际位置算，继续用整框顶点配子矩形 uv 就等于把剩余画面拉伸回原框。
  const float bx = r.x + r.w * 0.5f, by = r.y + r.h * 0.5f;  // 画布中心
  const float hw = r.w * 0.5f, hh = r.h * 0.5f;
  float corner[8] = {hw * (u0 * 2 - 1), hh * (v0 * 2 - 1), hw * (u1 * 2 - 1), hh * (v0 * 2 - 1),
                     hw * (u1 * 2 - 1), hh * (v1 * 2 - 1), hw * (u0 * 2 - 1), hh * (v1 * 2 - 1)};
  // 缩放/旋转绕 origin 指定的支点（render.cc:429-443 的
  // T(om) * (T(loc) * R * S) * T(-om)，om = 显示框尺寸*(origin−0.5)，y 轴取 Blender 的
  // 「0=图片下沿」）。展成「中心支点 + 同样的支点位移差」：结果比绕中心多走
  // E = om − R·S·om。
  const float omx = r.w * (origin_x - 0.5f), omy = r.h * (0.5f - origin_y);
  float rad = degrees * 3.14159265f / 180.0f;
  float co = cosf(rad), si = sinf(rad);
  float cx = bx + dx, cy = by + dy;
  {
    const float sx_ = omx * scale_x, sy_ = omy * scale_y;
    cx += omx - (sx_ * co - sy_ * si);
    cy += omy - (sx_ * si + sy_ * co);
  }
  float pts[8];
  for (int i = 0; i < 4; ++i) {
    float px = corner[i * 2] * scale_x, py = corner[i * 2 + 1] * scale_y;
    float qx = cx + px * co - py * si, qy = cy + px * si + py * co;
    // SEQ_FLIPX/Y 在 Blender 里是变换之后翻转整张结果图（render.cc:770-780 的 IMB_flipx
    // 作用在 rectx×recty 的画布上），所以裁掉的那块会跟着翻到对面去，偏移方向也一起翻。
    if (flip_x) qx = 2 * bx - qx;
    if (flip_y) qy = 2 * by - qy;
    pts[i * 2] = qx;
    pts[i * 2 + 1] = qy;
  }
  quad(pts, uv, tint.pack(), tex);
}

static HFONT get_font(std::map<int, HFONT> &cache, int size) {
  auto it = cache.find(size);
  if (it != cache.end()) return it->second;
  LOGFONTW lf{};
  lf.lfHeight = -size;
  lf.lfQuality = ANTIALIASED_QUALITY;
  lf.lfCharSet = DEFAULT_CHARSET;
  wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
  HFONT f = CreateFontIndirectW(&lf);
  cache.emplace(size, f);
  return f;
}

float Render2D::text_width(const std::wstring &s, int size) const {
  std::wstring key = std::to_wstring(size) + L'|' + s;
  auto it = text_w_.find(key);
  if (it != text_w_.end()) return it->second;
  HFONT f = get_font(fonts_, size);
  HGDIOBJ old = SelectObject(mem_dc_, f);
  SIZE sz{};
  GetTextExtentPoint32W(mem_dc_, s.c_str(), int(s.size()), &sz);
  SelectObject(mem_dc_, old);
  text_w_.emplace(key, float(sz.cx));
  return float(sz.cx);
}

int Render2D::text_height(int size) const {
  auto it = font_h_.find(size);
  if (it != font_h_.end()) return it->second;
  HFONT f = get_font(fonts_, size);
  HGDIOBJ old = SelectObject(mem_dc_, f);
  TEXTMETRICW tm{};
  GetTextMetricsW(mem_dc_, &tm);
  SelectObject(mem_dc_, old);
  int h = tm.tmHeight + tm.tmExternalLeading;
  font_h_.emplace(size, h);
  return h;
}

Render2D::Tex *Render2D::text_tex(const std::wstring &s, int size, float *advance) {
  std::wstring key = std::to_wstring(size) + L'|' + s;
  auto it = text_srv_.find(key);
  if (it != text_srv_.end()) {
    *advance = text_w_[key];
    return &it->second;
  }
  HFONT f = get_font(fonts_, size);
  HGDIOBJ old = SelectObject(mem_dc_, f);
  SIZE sz{};
  GetTextExtentPoint32W(mem_dc_, s.c_str(), int(s.size()), &sz);
  TEXTMETRICW tm{};
  GetTextMetricsW(mem_dc_, &tm);
  SelectObject(mem_dc_, old);
  int w = sz.cx + 2, h = tm.tmHeight + tm.tmExternalLeading + 2;
  if (w <= 2 || h <= 2) {
    text_srv_.emplace(key, Tex{});
    text_w_.emplace(key, 0.f);
    *advance = 0;
    return nullptr;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  HBITMAP dib = CreateDIBSection(mem_dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib) {
    text_srv_.emplace(key, Tex{});
    text_w_.emplace(key, 0.f);
    *advance = 0;
    return nullptr;
  }
  HGDIOBJ oldbmp = SelectObject(mem_dc_, dib);
  HGDIOBJ oldfont = SelectObject(mem_dc_, f);
  RECT full{0, 0, w, h};
  FillRect(mem_dc_, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  SetBkMode(mem_dc_, TRANSPARENT);
  SetTextColor(mem_dc_, RGB(255, 255, 255));
  TextOutW(mem_dc_, 1, 1, s.c_str(), int(s.size()));
  SelectObject(mem_dc_, oldfont);
  SelectObject(mem_dc_, oldbmp);

  // 亮度 -> alpha，白色字模由顶点色着色。
  staging_.assign(size_t(w) * h * 4, 0);
  auto *src = static_cast<uint8_t *>(bits);
  for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
    uint8_t lum = src[i * 4 + 2];  // 白字黑底：任一通道即覆盖度
    staging_[i * 4 + 0] = 255;
    staging_[i * 4 + 1] = 255;
    staging_[i * 4 + 2] = 255;
    staging_[i * 4 + 3] = lum;
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA srd{staging_.data(), UINT(w * 4), 0};
  Tex entry{};
  if (SUCCEEDED(device_->CreateTexture2D(&td, &srd, &entry.tex))) {
    device_->CreateShaderResourceView(entry.tex, nullptr, &entry.srv);
  }
  entry.w = w;
  entry.h = h;
  DeleteObject(dib);
  text_w_.emplace(key, float(sz.cx));
  *advance = float(sz.cx);
  auto ins = text_srv_.emplace(key, entry);
  return ins.first->second.srv ? &ins.first->second : nullptr;
}

float Render2D::text(float x, float y, const std::wstring &s, int size, Color c) {
  if (s.empty()) return 0;
  float adv = 0;
  Tex *tex = text_tex(s, size, &adv);
  if (!tex || !tex->srv) return 0;
  float uv[8];
  uv_rect(uv, 0, 0, 1, 1);
  // 纹理左右各留 1px 余量，绘制时裁掉。
  // 落点必须取整到设备像素：字模位图是按 1 像素 = 1 texel 生成的，一旦 x/y 带小数，
  // LINEAR 采样就把每个像素摊成相邻两个 texel 的均值（实测横向笔画从 1 行 230 变成
  // 2~3 行 129~179，即整屏字发糊）。Blender 同一件事的做法是每字形笔位 floor 到整数
  // （blf_font.cc:614-615 的 ft_pix_to_int_floor），小数相位由 FreeType 预烘进字形位图
  // （BLF_SUBPIXEL_AA，blf_internal.hh:44 + blf_glyph.cc:1429-1448），GPU 侧永不再重采样。
  // 整串缓存等价于「串起点取整」，串内像素是 GDI 已经落在像素网格上的结果。
  rect_uv({std::floor(x) - 1, std::floor(y) - 1, float(tex->w), float(tex->h)}, uv, c.pack(), tex);
  return adv;
}

bool Render2D::capture_png(const wchar_t *path) {
  if (!swap_ || !device_ || !ctx_) return false;
  flush();  // 未提交的批次先进入命令队列
  ID3D11Texture2D *back = nullptr;
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  D3D11_TEXTURE2D_DESC bd;
  back->GetDesc(&bd);
  D3D11_TEXTURE2D_DESC sd = bd;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  sd.MiscFlags = 0;
  ID3D11Texture2D *st = nullptr;
  if (FAILED(device_->CreateTexture2D(&sd, nullptr, &st))) { back->Release(); return false; }
  ctx_->CopyResource(st, back);
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx_->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
    st->Release(); back->Release(); return false;
  }

  bool ok = false;
  IWICImagingFactory *fac = nullptr;
  IWICStream *stream = nullptr;
  IWICBitmapEncoder *enc = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac));
  if (SUCCEEDED(hr)) hr = fac->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
  if (SUCCEEDED(hr)) hr = fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
  if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
  if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
  if (SUCCEEDED(hr)) {
    GUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetSize(UINT(bd.Width), UINT(bd.Height));
    frame->SetPixelFormat(&fmt);
    hr = frame->WritePixels(UINT(bd.Height), m.RowPitch, UINT(m.RowPitch) * UINT(bd.Height),
                            static_cast<BYTE *>(m.pData));
    if (SUCCEEDED(hr)) {
      hr = frame->Commit();
      if (SUCCEEDED(hr)) hr = enc->Commit();
      ok = SUCCEEDED(hr);
    }
  }
  ctx_->Unmap(st, 0);
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  if (fac) fac->Release();
  st->Release();
  back->Release();
  return ok;
}
