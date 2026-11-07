// mfreader.cpp — Media Foundation 取帧路。见 mfreader.h 里那条「为什么值得多这一条路」的账。
//
// 三个必须做对的地方（都是实测出来的，不是文档读来的）：
// 1. 解码器输出纹理默认只带 DECODER|VIDEO_ENCODER（bind=0x600），没有 SHADER_RESOURCE 那位，
//    所以任何平面 SRV 都是 E_INVALIDARG。要把它换成 0x208 得在建 reader 前设
//    MF_SOURCE_READER_D3D11_BIND_FLAGS。
// 2. NV12 纹理数组取平面：同一个 slice 开两只 SRV，靠 Format 选平面 —— Y=R8_UNORM、
//    UV=R8G8_UNORM，ArraySize 必须是 1（填 2 直接 E_INVALIDARG）。slice 就是
//    IMFDXGIBuffer 给的子资源号（mip=1，两者数值相同）。
// 3. 色彩矩阵按媒体类型上标的 MF_MT_YUV_MATRIX / MF_MT_VIDEO_NOMINAL_RANGE 现算，不写死
//    709：素材标 601 或全范围时写死会整幅偏色。没标的按行数判 HD/SD（720x576 用宽度判会被
//    误判成 709）。对账门必须拿标了 bt709+tv 的彩条量：中性帧测不出色度错。
//
// 线程：ReadSample 不在调用它的线程上解码 —— 挂了 device manager 之后，MF 用自己的工作线程
// 往同一只 immediate context 提交命令。实测不给这把锁上，第二帧起两边命令交错，驱动直接摘设备
// （Map 返回 0x887A0005 = DXGI_ERROR_DEVICE_REMOVED）。所以建引擎时要把这把锁上上。
#include "mfreader.h"
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <d3d10.h>  // ID3D10Multithread：D3D11 的上下文也认这个接口
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <set>

// ops.cpp 里的 VSE_TRACE 落盘函数（mfreader.cpp 不引 app.h，同 export.cpp 的处理）。
namespace app {
void trace(const std::wstring &s);
}
using app::trace;

namespace {

// 把「哪一步拒了 + HRESULT」打成一行，写进 VSE_TRACE。
bool mf_bail(const wchar_t *stage, HRESULT hr) {
  wchar_t buf[160];
  swprintf(buf, 160, L"mf open 拒: %ls hr=0x%08X", stage, unsigned(hr));
  trace(buf);
  return false;
}

}  // namespace

namespace {

// MinGW 的 mfobjects.h 没有 MFVideoTransferMatrix，数值抄自 MS 的同名枚举。
enum { kMatrixUnknown = 0, kMatrix709 = 2, kMatrix601 = 4, kMatrix2020Light = 6 };

// 与 HLSL 里 `cbuffer C{float y0; float3x3 m;}` 的打包一致：标量独占一格，每格 16 字节。
// out[k] 是第 k 个输出分量（R/G/B）对 (Y', Cb', Cr') 的系数。这个方向是量出来的：GPU 实际算的
// 是 out[k] = dot(v, out[k])，按「输入分量→RGB」填会把中性像素推成品红（白帧实测 255,1,255）。
struct ColorCB {
  float y0 = 16.0f / 255.0f;
  float pad0[3] = {0, 0, 0};
  float out[3][4] = {{1.1643f, 0.0f, 1.5960f, 0},
                     {1.1643f, -0.3918f, -0.8114f, 0},
                     {1.1643f, 2.0172f, 0.0f, 0}};
};

const char *kHlsl =
    "struct VOut{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
    "VOut VSMain(uint id:SV_VertexID){VOut o;"
    "float2 uv=float2((id&1)*2.0,(id&2)*1.0);"  // 退化到 (0,0)(2,0)(0,2) 的全屏三角
    "o.p=float4(uv.x*2.0-1.0,1.0-uv.y*2.0,0.0,1.0);o.uv=uv;return o;}"
    "Texture2D<float> yT:register(t0);Texture2D<float2> cT:register(t1);"
    "SamplerState ss:register(s0);"
    "cbuffer C:register(b0){float y0;float3x3 m;};"
    "float4 PSMain(VOut i):SV_Target{"
    "float y=yT.Sample(ss,i.uv);float2 cc=cT.Sample(ss,i.uv);"
    "float3 v=float3(y-y0,cc.x-0.5,cc.y-0.5);"
    "return float4(mul(v,m),1.0);}";

struct Engine {
  bool ok = false;
  HRESULT last_hr = S_OK;
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  IMFDXGIDeviceManager *dm = nullptr;
  UINT cookie = 0;
  ID3D11VertexShader *vs = nullptr;
  ID3D11PixelShader *ps = nullptr;
  ID3D11SamplerState *smp = nullptr;
  ID3D11Buffer *cb = nullptr;
  ID3D11Texture2D *rt = nullptr;  // 转换目标 + 回读暂存，按当前预览档位尺寸复用
  ID3D11RenderTargetView *rtv = nullptr;
  ID3D11Texture2D *stage = nullptr;
  int rw = 0, rh = 0;

  bool scratch(int w, int h) {
    if (rt && rw == w && rh == h) return true;
    if (rtv) { rtv->Release(); rtv = nullptr; }
    if (rt) { rt->Release(); rt = nullptr; }
    if (stage) { stage->Release(); stage = nullptr; }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w);
    d.Height = UINT(h);
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &rt);
    if (FAILED(hr)) { last_hr = hr; return false; }
    hr = dev->CreateRenderTargetView(rt, nullptr, &rtv);
    if (FAILED(hr)) { last_hr = hr; return false; }
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.BindFlags = 0;
    hr = dev->CreateTexture2D(&d, nullptr, &stage);
    if (FAILED(hr)) { last_hr = hr; return false; }
    rw = w;
    rh = h;
    return true;
  }
};

Engine &eng() {
  static Engine e;
  static std::once_flag once;
  std::call_once(once, [&] {
    const wchar_t *stage = L"start";
    auto fail = [&] {
      wchar_t buf[160];
      swprintf(buf, 160, L"mf 引擎没建起来: %ls hr=0x%08X", stage, unsigned(e.last_hr));
      trace(buf);
    };
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // 已按别的方式初始化过也算可用
    if ((e.last_hr = MFStartup(MF_VERSION)) != S_OK) { stage = L"MFStartup"; fail(); return; }
    // flags 带 VIDEO_SUPPORT 是 MF 硬解的正路；实测不带也能同设备解码，且逐字节同图。
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if ((e.last_hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                       D3D11_SDK_VERSION, &e.dev, nullptr, &e.ctx)) != S_OK) {
      stage = L"D3D11CreateDevice"; fail(); return;
    }
    ID3D10Multithread *mt = nullptr;
    e.last_hr = e.ctx->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt);
    if (FAILED(e.last_hr)) { stage = L"ID3D10Multithread"; fail(); return; }
    mt->SetMultithreadProtected(TRUE);
    mt->Release();
    e.last_hr = MFCreateDXGIDeviceManager(&e.cookie, &e.dm);
    if (SUCCEEDED(e.last_hr)) e.last_hr = e.dm->ResetDevice(e.dev, e.cookie);
    if (FAILED(e.last_hr)) { stage = L"device manager"; fail(); return; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    e.last_hr = D3DCompile(kHlsl, strlen(kHlsl), "mf", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err);
    if (SUCCEEDED(e.last_hr))
      e.last_hr = D3DCompile(kHlsl, strlen(kHlsl), "mf", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(e.last_hr)) { stage = L"D3DCompile"; fail(); return; }
    bool good = SUCCEEDED(e.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &e.vs)) &&
                SUCCEEDED(e.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &e.ps));
    vsb->Release();
    psb->Release();
    if (!good) { stage = L"CreateShader"; e.last_hr = E_FAIL; fail(); return; }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if ((e.last_hr = e.dev->CreateSamplerState(&sd, &e.smp)) != S_OK) { stage = L"CreateSamplerState"; fail(); return; }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ColorCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if ((e.last_hr = e.dev->CreateBuffer(&bd, nullptr, &e.cb)) != S_OK) { stage = L"CreateBuffer"; fail(); return; }
    e.ok = true;
    trace(L"mf 引擎就绪");
  });
  return e;
}

// NV12 的一个 slice 取一个平面：ArraySize 必须为 1，平面靠 Format 选。
HRESULT plane_srv(ID3D11Texture2D *tex, UINT slice, DXGI_FORMAT fmt, ID3D11ShaderResourceView **out) {
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = fmt;
  d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  d.Texture2DArray.MostDetailedMip = 0;
  d.Texture2DArray.MipLevels = 1;
  d.Texture2DArray.FirstArraySlice = slice;
  d.Texture2DArray.ArraySize = 1;
  HRESULT hr = eng().dev->CreateShaderResourceView(tex, &d, out);
  if (FAILED(hr)) *out = nullptr;
  return hr;
}

void color_from_type(IMFMediaType *mt, ColorCB *cb) {
  UINT32 matrix = kMatrixUnknown, range = 0;
  UINT32 fw = 0, fh = 0;
  if (mt) {
    mt->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
    mt->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &fw, &fh);
  }
  const bool limited = range != MFNominalRange_0_255;  // 未标也按有限范围（HD/SD 的默认）
  bool bt709 = matrix == kMatrix709 || matrix == kMatrix2020Light;
  // 没标的按行数判：720 行及以上算 HD 走 709。用行数而不是列数，竖屏素材（如 480x854）
  // 才不会被宽度过窄骗回 601。实测 MF 不肯把 MP4 的 colr/VUI 交给 MF_MT_YUV_MATRIX，
  // 所以没标的那一档只能靠行数，读不到矩阵不是意外。
  if (matrix == kMatrixUnknown) bt709 = int(fh) >= 720;
  const float kY = limited ? 1.1643f : 1.0f;
  cb->y0 = limited ? 16.0f / 255.0f : 0.0f;
  // 四个非零色度系数：Cr 进 R、Cb/Cr 进 G、Cb 进 B。列序 = (Y', Cb', Cr')，行序 = R/G/B。
  float cr_r = 0, cb_g = 0, cr_g = 0, cb_b = 0;
  if (bt709) {
    cr_r = limited ? 1.7927f : 1.5748f;
    cb_g = limited ? -0.2132f : -0.1873f;
    cr_g = limited ? -0.5329f : -0.4681f;
    cb_b = limited ? 2.1124f : 1.8556f;
  } else {
    cr_r = limited ? 1.5960f : 1.4020f;
    cb_g = limited ? -0.3910f : -0.3441f;
    cr_g = limited ? -0.8130f : -0.7141f;
    cb_b = limited ? 2.0180f : 1.7720f;
  }
  cb->out[0][0] = kY; cb->out[0][1] = 0.0f; cb->out[0][2] = cr_r;
  cb->out[1][0] = kY; cb->out[1][1] = cb_g; cb->out[1][2] = cr_g;
  cb->out[2][0] = kY; cb->out[2][1] = cb_b; cb->out[2][2] = 0.0f;
}

double ms_now() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

// 一帧在 MF 这路的四段耗时累加（只在工作线程碰，无需锁）。分这么细要判的就是「画+读回」和
// 「换算」这两段值不值得用免读回的结构换掉，decoded/pool 那种计数分不出来。
struct Split {
  double read = 0, srv = 0, readback = 0, cvt = 0;
  int n = 0;
};
Split &split() {
  static Split s;
  return s;
}

}  // namespace

struct MfStream::Impl {
  IMFSourceReader *rd = nullptr;
  ColorCB cb{};
  bool dead = false;
  bool rejected = false;  // 流进了错误态（解不出这路编码），不是走到素材末尾
  int nsamp = 0;          // 这条流交过几只样本：只为「首样报一行」数到 3 就停
};

MfStream::MfStream() = default;
MfStream::~MfStream() {
  if (p_ && p_->rd) p_->rd->Release();
}

bool MfStream::enabled() {
  static const bool on = [] {
    wchar_t buf[8] = {0};
    GetEnvironmentVariableW(L"VSE_MF", buf, 8);
    // 默认走 MF：实测同一只 build 双臂 0.87 vs 1.81 CPU 秒/挂钟秒（见 mfreader.h 的账）。
    // VSE_MF=0 才退回「只用 ffmpeg 子进程」，用来在别的机器上做对照。
    return buf[0] != L'0';
  }();
  return on;
}

bool MfStream::open(const std::wstring &path, double seconds, int out_w, int out_h) {
  Engine &e = eng();
  if (!e.ok || out_w <= 0 || out_h <= 0) return mf_bail(L"engine", e.last_hr);
  p_ = std::make_unique<Impl>();
  IMFAttributes *attrs = nullptr;
  if (FAILED(MFCreateAttributes(&attrs, 4))) return mf_bail(L"attrs", E_FAIL);
  attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, e.dm);
  attrs->SetUINT32(MF_SOURCE_READER_D3D11_BIND_FLAGS,
                   D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE);
  HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs, &p_->rd);
  attrs->Release();
  if (FAILED(hr) || !p_->rd) return mf_bail(L"reader", hr);
  p_->rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
  p_->rd->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
  // 不设当前类型的话 ReadSample 给的是压缩原样；把 native 类型的 subtype 改成 NV12，
  // MF 会自己把 H.264 解码器插进来。
  IMFMediaType *want = nullptr;
  if (FAILED(p_->rd->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &want)))
    return mf_bail(L"native type", E_FAIL);
  want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  hr = p_->rd->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want);
  want->Release();
  IMFMediaType *cur = nullptr;
  if (FAILED(hr)) return mf_bail(L"set NV12", hr);
  if (FAILED(p_->rd->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
    return mf_bail(L"get current", E_FAIL);
  GUID sub{};
  cur->GetGUID(MF_MT_SUBTYPE, &sub);
  const bool decoded = sub == MFVideoFormat_NV12;
  color_from_type(cur, &p_->cb);
  cur->Release();
  if (!decoded) return mf_bail(L"NV12 没谈成（系统解码器不认这路编码）", S_OK);
  if (!e.scratch(out_w, out_h)) return mf_bail(L"scratch rt", e.last_hr);
  // MF 自己的时钟原点：带 elst 的素材，MF 把第 0 只样本 stamp 在 2×media_time（实测 0.266 /
  // 0.1333），画面却是完整的第 0 只 —— 是标号晚了一个常数，不是丢帧。先定位到 0 读一只样本，
  // 它的标号就是这只流的原点；之后所有定位加它、所有读回减它，MF 的钟就和导出/子进程同一把尺。
  // 无 elst 的素材第一只标号是 0 ⇒ 原点 0 ⇒ 行为与原来逐字相同。
  origin_ = 0.0;
  {
    PROPVARIANT pv{};
    pv.vt = VT_I8;
    pv.hVal.QuadPart = 0;
    p_->rd->SetCurrentPosition(GUID_NULL, pv);
    PropVariantClear(&pv);
    IMFSample *s0 = nullptr;
    DWORD f0 = 0;
    if (SUCCEEDED(p_->rd->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &f0, nullptr, &s0)) && s0) {
      LONGLONG ts0 = 0;
      s0->GetSampleTime(&ts0);
      origin_ = double(ts0) / 1e7;
      s0->Release();
    }
  }
  {
    wchar_t ob[128];
    swprintf(ob, 128, L"mf 原点: %.6f 秒（定位加它、读回减它）", origin_);
    trace(ob);
  }
  {
    PROPVARIANT pv{};
    pv.vt = VT_I8;
    pv.hVal.QuadPart = LONGLONG((seconds + origin_) * 1e7);
    p_->rd->SetCurrentPosition(GUID_NULL, pv);  // 落点由 read() 里的真实样本时间说话
    PropVariantClear(&pv);
  }
  w_ = out_w;
  h_ = out_h;
  return true;
}

bool MfStream::seek(double seconds) {
  Impl *p = p_.get();
  if (!p || !p->rd) return false;
  PROPVARIANT pv{};
  pv.vt = VT_I8;
  pv.hVal.QuadPart = LONGLONG((seconds + origin_) * 1e7);
  HRESULT hr = p->rd->SetCurrentPosition(GUID_NULL, pv);
  PropVariantClear(&pv);
  if (FAILED(hr)) return false;
  p->dead = false;  // 走到头之后再退回来时，到头标记得跟着清掉
  return true;
}

bool MfStream::read() {
  Impl *p = p_.get();
  Engine &e = eng();
  if (!p || !p->rd || p->dead || bgra_.size() < size_t(w_) * h_ * 4) bgra_.resize(size_t(w_) * h_ * 4);
  if (!p || !p->rd || p->dead) return false;
  // 分帧账：decoded/pool 只说解出多少帧，说不清一帧的钱花在哪一段。攒 100 帧报一行均值。
  double t_a = ms_now(), t_b = t_a, t_c = t_a, t_d = t_a;
  for (int guard = 0; guard < 64; ++guard) {
    IMFSample *s = nullptr;
    DWORD flags = 0;
    HRESULT hr = p->rd->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, nullptr, &s);
    t_b = ms_now();
    if (FAILED(hr)) {
      wchar_t buf[160];
      swprintf(buf, 160, L"mf 读帧失败: ReadSample hr=0x%08X flags=0x%X", unsigned(hr), unsigned(flags));
      trace(buf);
      p->dead = true;
      // 错误态和解不出是两回事，但只有一种要回落：mpeg4 part2 这种「容器认、编码不认」的素材，
      // SetCurrentMediaType(NV12) 也点头，到 ReadSample 才给 flags=MF_SOURCE_READERF_ERROR
      // （hr=0xC00D36B4）。走到素材末尾给的是 ENDOFSTREAM，不该记成拒绝。
      if (flags & MF_SOURCE_READERF_ERROR) p->rejected = true;
      if (s) s->Release();
      return false;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      trace(L"mf 读帧失败: 流到头");
      p->dead = true;
      if (s) s->Release();
      return false;
    }
    if (!s) continue;  // 流时钟刻度，没有画面
    LONGLONG ts = 0;
    s->GetSampleTime(&ts);
    t_ = double(ts) / 1e7 - origin_;
    // 这条流交出的前几只样本：MF 报的是容器绝对 100ns 时间，预览那一路拿它当「第几只源帧」。
    // 「规则要第 0 只、画面给的是第 8 只」这类头部错位，只有在这里才看得见是流交的还是换算错的。
    if (++p->nsamp <= 3) {
      wchar_t buf[160];
      swprintf(buf, 160, L"mf 首样: 第%d只 t=%.6f 原始ts=%lld flags=0x%X",
               p->nsamp, t_, ts, unsigned(flags));
      trace(buf);
    }
    IMFMediaBuffer *mb = nullptr;
    IMFDXGIBuffer *db = nullptr;
    ID3D11Texture2D *tex = nullptr;
    UINT slice = 0;
    const wchar_t *why = L"unknown";
    bool got = SUCCEEDED(s->GetBufferByIndex(0, &mb)) && SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(&db)));
    if (!got) why = L"样本缓冲不是 IMFDXGIBuffer";
    if (got) {
      db->GetResource(__uuidof(ID3D11Texture2D), (void **)&tex);
      db->GetSubresourceIndex(&slice);  // MinGW 的头只声明一个参数（真实签名另有两个入参）
      got = tex != nullptr;
      if (!got) why = L"GetResource 没给纹理";
    }
    ID3D11ShaderResourceView *ys = nullptr, *cs = nullptr;
    if (got) {
      HRESULT hr2 = plane_srv(tex, slice, DXGI_FORMAT_R8_UNORM, &ys);
      if (SUCCEEDED(hr2)) hr2 = plane_srv(tex, slice, DXGI_FORMAT_R8G8_UNORM, &cs);
      if ((got = SUCCEEDED(hr2)) == false) {
        wchar_t buf[160];
        swprintf(buf, 160, L"mf 画帧失败: 平面 SRV hr=0x%08X slice=%u", unsigned(hr2), slice);
        trace(buf);
        why = L"平面 SRV";
      }
    }
    t_c = ms_now();
    // 共享的 rt/stage 只有一只，按「当前输出尺寸」复用。进程里同时活着两条流时（池里不同条带
    // 的解码档位不同），另一条可能已经把它换成它那档尺寸 —— 不核对就照本流的 w_/h_ 逐行搬，
    // 会从一只更小的暂存面外面读字节（实测拖回播放头时崩在 memcpy）。每帧先对回本流要的尺寸。
    if (got && !e.scratch(w_, h_)) {
      got = false;
      why = L"暂存面重建";
      wchar_t buf[160];
      swprintf(buf, 160, L"mf 画帧失败: 暂存面重建 %dx%d hr=0x%08X", w_, h_, unsigned(e.last_hr));
      trace(buf);
    }
    if (got) {
      D3D11_MAPPED_SUBRESOURCE mc{};
      if (SUCCEEDED(e.ctx->Map(e.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mc))) {
        memcpy(mc.pData, &p->cb, sizeof(ColorCB));
        e.ctx->Unmap(e.cb, 0);
      }
      const float black[4] = {0, 0, 0, 1};
      e.ctx->ClearRenderTargetView(e.rtv, black);
      e.ctx->OMSetRenderTargets(1, &e.rtv, nullptr);
      D3D11_VIEWPORT vp{};
      vp.Width = float(w_);
      vp.Height = float(h_);
      vp.MaxDepth = 1.0f;
      e.ctx->RSSetViewports(1, &vp);
      ID3D11ShaderResourceView *pair[2] = {ys, cs};
      e.ctx->PSSetShaderResources(0, 2, pair);
      e.ctx->PSSetSamplers(0, 1, &e.smp);
      e.ctx->PSSetConstantBuffers(0, 1, &e.cb);
      e.ctx->VSSetShader(e.vs, nullptr, 0);
      e.ctx->PSSetShader(e.ps, nullptr, 0);
      e.ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
      e.ctx->IASetInputLayout(nullptr);
      e.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      e.ctx->Draw(3, 0);
      ID3D11ShaderResourceView *nil[2] = {nullptr, nullptr};
      e.ctx->PSSetShaderResources(0, 2, nil);  // 立刻解绑：否则下一帧的纹理还挂着这条引用
      e.ctx->CopyResource(e.stage, e.rt);
      D3D11_MAPPED_SUBRESOURCE mp{};
      HRESULT hrMap = e.ctx->Map(e.stage, 0, D3D11_MAP_READ, 0, &mp);
      t_d = ms_now();
      if (SUCCEEDED(hrMap)) {
        // 暂存面就是 B8G8R8A8，和纹理要的格式同一个 —— 逐行搬走即可，不再逐像素换序。
        // 以前这里吐 RGB24（和 PPM 管道对齐），换完 UI 侧又要一格一格摊回 BGRA，两笔钱都为
        // 一个不存在的中间格式付。
        const int stride = int(mp.RowPitch);
        for (int y = 0; y < h_; ++y)
          memcpy(bgra_.data() + size_t(y) * w_ * 4, (const uint8_t *)mp.pData + size_t(y) * stride,
                 size_t(w_) * 4);
        e.ctx->Unmap(e.stage, 0);
        const double t_e = ms_now();
        Split &sp = split();
        sp.read += t_b - t_a;
        sp.srv += t_c - t_b;
        sp.readback += t_d - t_c;
        sp.cvt += t_e - t_d;
        if (++sp.n >= 100) {
          wchar_t buf[200];
          swprintf(buf, 200, L"mf 分帧 %d 帧: ReadSample %.2f 建SRV %.2f 画+读回 %.2f 换算 %.2f ms/帧",
                   sp.n, sp.read / sp.n, sp.srv / sp.n, sp.readback / sp.n, sp.cvt / sp.n);
          trace(buf);
          sp = Split{};
        }
      } else {
        got = false;
        why = L"Map 暂存面";
        HRESULT removed = e.dev->GetDeviceRemovedReason();
        wchar_t buf[200];
        swprintf(buf, 200, L"mf 画帧失败: Map 暂存面 hr=0x%08X 尺寸=%ux%u 设备状态=0x%08X",
                 unsigned(hrMap), e.rw, e.rh, unsigned(removed));
        trace(buf);
      }
    }
    if (ys) ys->Release();
    if (cs) cs->Release();
    if (tex) tex->Release();
    if (db) db->Release();
    if (mb) mb->Release();
    s->Release();
    if (!got) {
      wchar_t buf[160];
      swprintf(buf, 160, L"mf 画帧失败: %ls", why);
      trace(buf);
      p->dead = true;
      return false;
    }
    return true;
  }
  trace(L"mf 画帧失败: 64 次 ReadSample 没给出画面");
  p->dead = true;
  return false;
}

bool MfStream::rejected() const { return p_ && p_->rejected; }

namespace {
std::set<std::wstring> &banned_set() {
  static std::set<std::wstring> s;
  return s;
}
std::mutex &banned_mu() {
  static std::mutex m;
  return m;
}
}  // namespace

void MfStream::ban(const std::wstring &path) {
  {
    std::lock_guard<std::mutex> lk(banned_mu());
    banned_set().insert(path);
  }
  wchar_t buf[300];
  swprintf(buf, 300, L"mf 解不出这路素材，之后只走子进程: %ls", path.c_str());
  trace(buf);
}

bool MfStream::banned(const std::wstring &path) {
  std::lock_guard<std::mutex> lk(banned_mu());
  return banned_set().count(path) != 0;
}
