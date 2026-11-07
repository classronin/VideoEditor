#include "text.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>

#include "types.h"

namespace app {
void trace(const std::wstring &s);
}

namespace txt {

// ---------------------------------------------------------------- DirectWrite 单例
// 工厂、GdiInterop、渲染参数都是进程级一份：它们不含文字内容，跨条带复用没有一致性问题。
namespace {

IDWriteFactory *g_factory = nullptr;
IDWriteRenderingParams *g_params = nullptr;
std::wstring g_err;

// 渲染参数一次定死：自然描边（grayscale 的 ClearType 邻居）+ BGR 像素序 + gamma 1.0。
// 实测（本机，MinGW 的 DWrite 头）：这个组合下 B/G/R 三个通道的非零像素数完全相等
// ⇒ 灰度覆盖，取 B 当 alpha 用；换成 GDI_NATURAL 就带上 ClearType 的彩色边（G 通道独多），
// 那一版直接否掉，因为文本层之后要和视频叠加，彩色边会在边缘发色。
bool ensure_factory() {
  if (g_factory) return true;
  HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&g_factory));
  if (FAILED(hr)) {
    g_factory = nullptr;
    g_err = L"DirectWrite 初始化失败";
    return false;
  }
  hr = g_factory->CreateCustomRenderingParams(1.0f, 0.0f, 0.0f, DWRITE_PIXEL_GEOMETRY_BGR,
                                              DWRITE_RENDERING_MODE_NATURAL, &g_params);
  if (FAILED(hr)) {
    g_params = nullptr;
    g_err = L"DirectWrite 渲染参数创建失败";
    return false;
  }
  return true;
}

// 字形一趟：把 layout 走过的每一串字形原封不动转给渲染目标，颜色恒白。
// 白的用意：DrawGlyphRun 的 COLORREF 是「按通道乘」的，彩色会破坏灰度覆盖；覆盖场先取回来，
// 上色在下面自己算。
// 本头的 IDWriteTextRenderer 是 1.0 版：除 IUnknown 的三个方法外，先 IDWritePixelSnapping 的
// 三个（IsPixelSnappingDisabled / GetCurrentTransform / GetPixelsPerDip），再 DrawGlyphRun、
// DrawUnderline、DrawStrikethrough、DrawInlineObject。DrawGlyphRun 收的是 DWrite 自己那份
// DWRITE_GLYPH_RUN 指针 —— 不许自己拼这个结构：MinGW 头里的字段顺序与真正的 OS 布局不同
// （缺 languageDownlevel，且索引/advance 数组排在 isSideways 之前）。
struct GlyphSink : IDWriteTextRenderer {
  IDWriteBitmapRenderTarget *rt = nullptr;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **obj) override {
    if (!obj) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextRenderer) ||
        riid == __uuidof(IDWritePixelSnapping)) {
      *obj = static_cast<IDWriteTextRenderer *>(this);
      return S_OK;
    }
    *obj = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }

  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *disabled) override {
    if (disabled) *disabled = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *t) override {
    if (t) *t = DWRITE_MATRIX{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *v) override {
    if (v) *v = 1.0f;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode,
                                        const DWRITE_GLYPH_RUN *run,
                                        const DWRITE_GLYPH_RUN_DESCRIPTION *,
                                        IUnknown *) override {
    RECT bb{};
    return rt->DrawGlyphRun(x, y, mode, run, g_params, RGB(255, 255, 255), &bb);
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT, const DWRITE_UNDERLINE *,
                                         IUnknown *) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH *,
                                              IUnknown *) override {
    return S_OK;
  }
  // MinGW 的头到 1.0 为止：DrawInlineObject 没有 signing agent 那一个参数，本工程也不用它。
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL,
                                             BOOL, IUnknown *) override {
    return S_OK;
  }
};

// 直方：Blender 的 straight_to_premul_v4 同一条算式（effect_text.cc 用的那个）。
inline void premul(const float c[4], float cov, float out[4]) {
  out[0] = c[0] * c[3] * cov;
  out[1] = c[1] * c[3] * cov;
  out[2] = c[2] * c[3] * cov;
  out[3] = c[3] * cov;
}

// over：dst = src + (1 - src.a) * dst。预乘域，一条通道都不除。
inline void blend_over(float dst[4], const float src[4]) {
  const float m = 1.0f - src[3];
  dst[0] = src[0] + m * dst[0];
  dst[1] = src[1] + m * dst[1];
  dst[2] = src[2] + m * dst[2];
  dst[3] = src[3] + m * dst[3];
}

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 距离变换：种子 = 字形覆盖 >= 128 的像素，其余像素拿到最近种子的坐标。
// 这就是 Blender 的那一份（effect_text.cc:600-660 的 JFA：JFA_INVALID=0xFFFF、先 step=1 再按
// outline 宽度的 2 的幂折半）。种子阈值也照抄它的 `tmp_buf[i].w >= 128`。
// 只关心 outline_px 以内的距离，所以最大步长取不小于 outline_px 的 2 的幂，够用且更便宜。
void jfa_seeds(const uint8_t *cov, int w, int h, int outline_px, std::vector<uint16_t> &sx,
               std::vector<uint16_t> &sy) {
  constexpr uint16_t kInvalid = 0xFFFF;
  const size_t n = size_t(w) * size_t(h);
  sx.assign(n, kInvalid);
  sy.assign(n, kInvalid);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      if (cov[size_t(y) * w + x] < 128) continue;
      sx[size_t(y) * w + x] = uint16_t(x);
      sy[size_t(y) * w + x] = uint16_t(y);
    }
  int step = 1;
  while (step < outline_px) step <<= 1;
  std::vector<uint16_t> bx(sx), by(sy);
  for (; step >= 1; step >>= 1) {
    bx.swap(sx);
    by.swap(sy);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        // 空格子也要参加：它这一趟就是从邻居那里「采纳」一个种子，跳过它则永远填不满。
        uint16_t best_x = kInvalid, best_y = kInvalid;
        float best = 1e30f;
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            const int nx = x + dx * step, ny = y + dy * step;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const size_t j = size_t(ny) * w + nx;
            if (bx[j] == kInvalid) continue;
            const float d = std::hypotf(float(x) - bx[j], float(y) - by[j]);
            if (d < best) {
              best = d;
              best_x = bx[j];
              best_y = by[j];
            }
          }
        }
        sx[size_t(y) * w + x] = best_x;
        sy[size_t(y) * w + x] = best_y;
      }
    }
  }
}

// 量版心 → 排版 → 从画布上抄出墨迹 → 三层合成。成功返回 true，位图填好。
bool render(const TextVars &t, int canvas_w, Bitmap &out) {
  out = Bitmap{};
  g_err.clear();
  if (t.content.empty() || t.size < 1.0) return true;  // 空内容/非法字号：没有位图，也不算失败
  if (!ensure_factory()) return false;

  // 轮廓宽度、方框边距、留白都要等行高量出来才算（下面第二趟排版的行格就是 Blender 的
  // line_height），所以这一段排在排版之后。
  IDWriteTextFormat *fmt = nullptr;
  HRESULT hr = g_factory->CreateTextFormat(
      t.family.c_str(), nullptr,
      t.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
      t.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
      FLOAT(t.size), L"", &fmt);
  if (FAILED(hr)) {
    g_err = L"字体创建失败：" + t.family;
    return false;
  }
  // 手动换行（用户令：不做自动换行）⇒ 关掉折行。
  fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  // 行距是倍数，乘在字体自己的自然行高上 —— 这就是 Blender 的行前进量 line_height *
  // space_line（effect_text.cc 逐行往下走的那一步），出厂 space_line = 1.0 即「一格行高」。
  // 方法要 PROPORTIONAL：UNIFORM 是把行格钉成一个绝对值，用户改字号时行格不跟着长，
  // 那个 1.0 的倍数就名不副实了。
  fmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, float(t.line_spacing), 0.0f);
  // 每行在版心内居中 = Blender 的默认对齐（init_text_effect 的 align = SEQ_TEXT_ALIGN_X_CENTER，
  // horizontal_alignment_offset_get）。
  // 「居中」是 DWrite 在**版心宽度**内推的，不是画布也不是墨迹：版心给成无限宽，每行就被推到
  // 半个无限宽那里去，缓冲区里一个像素都落不下（实测面 344x104、墨 0）。所以版心必须先量：
  // 第一趟左对齐 + 不折行，量出来的宽度就是最宽那一行；第二趟把版心正好设成它再居中，
  // 短行才真的落到中间来。
  fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  IDWriteTextLayout *meas = nullptr;
  hr = g_factory->CreateTextLayout(t.content.c_str(), UINT32(t.content.size()), fmt, 100000.0f,
                                   100000.0f, &meas);
  if (FAILED(hr)) {
    fmt->Release();
    g_err = L"文本排版失败";
    return false;
  }
  DWRITE_TEXT_METRICS mm{};
  meas->GetMetrics(&mm);
  const float content_w = std::max(1.0f, mm.width);
  meas->Release();
  fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

  IDWriteTextLayout *layout = nullptr;
  hr = g_factory->CreateTextLayout(t.content.c_str(), UINT32(t.content.size()), fmt, content_w,
                                   100000.0f, &layout);
  fmt->Release();
  if (FAILED(hr)) {
    g_err = L"文本排版失败";
    return false;
  }
  DWRITE_TEXT_METRICS m{};
  layout->GetMetrics(&m);
  DWRITE_LINE_METRICS lm[4]{};
  UINT32 lmc = 0;
  layout->GetLineMetrics(lm, 4, &lmc);
  // 轮廓宽度：Blender 的换算与门槛（effect_text.cc:600 起，
  // outline_width_px = int(line_height * 0.5 * data->outline_width)，门槛 `outline_width < 1`）。
  // 这里的 line_height 取排版出来的那一格 = 自然行高 × 行距倍数。
  const float line_h = lmc ? lm[0].height : FLOAT(t.size);
  const int outline_px = (t.outline_color[3] > 0.0f) ? int(line_h * 0.5f * float(t.outline_width))
                                                     : 0;
  // 方框边距：Blender 用画布宽（box_margin * out.image->x，:1152）。alpha 为 0 = 关。
  const bool box_on = t.box_color[3] > 0.0f;
  const int margin_px = box_on ? int(float(t.box_margin) * float(canvas_w)) : 0;
  // 交出去的图 = 量出来的墨迹外接框，四周再让出「轮廓 + 边距」那么多。图是墨迹的对称外框，
  // 所以条带按「图的中心对画幅的中心」画出来，字就正落在画面正中（用户要的就是这一条）。
  const int pad = outline_px + margin_px;
  // 画布要比版心大一圈，好让墨迹怎么落都不被缓冲区边缘裁掉。为什么要留这一圈：DWrite 把
  // 我们给的绘制原点当成**第一行的基线**发给我们（实测两笔：原点 22 ⇒ 墨迹到第 30 行为止，
  // 原点 124 ⇒ 墨迹 74..132；两点斜率 1、截距 0，即第一行的升部整个跑到版心之上），
  // 而它回给的位置又只有行格没有墨迹（ink* 字段是 1.1 才有；这份工具链的
  // DWRITE_LINE_METRICS 字段序与系统不一致，它的 baseline 成员其实是系统的 codepointLength）。
  // ⇒ 竖向位置不靠算行格，靠下面从缓冲区里量回来的墨迹。
  const int slack = int(t.size * 2.0) + 4;
  const int draw_pad = pad + slack;
  const int bw = std::max(1, int(std::ceil(m.width)));
  const int bh = std::max(1, int(std::ceil(m.height)));
  const int CW = bw + 2 * draw_pad, CH = bh + 2 * draw_pad;

  bool ok = false;
  std::vector<uint8_t> cov(size_t(CW) * size_t(CH), 0);
  IDWriteGdiInterop *gi = nullptr;
  IDWriteBitmapRenderTarget *rt = nullptr;
  if (SUCCEEDED(g_factory->GetGdiInterop(&gi))) {
    if (SUCCEEDED(gi->CreateBitmapRenderTarget(nullptr, UINT32(CW), UINT32(CH), &rt)) &&
        SUCCEEDED(PatBlt((HDC)rt->GetMemoryDC(), 0, 0, CW, CH, BLACKNESS) ? S_OK : E_FAIL)) {
      // 目标自己的位图由 GDI 拿：GetCurrentObject 是 gdi32 的全局函数（这个头里
      // IDWriteBitmapRenderTarget 没有同名方法），参数是它的内存 DC。
      // 它的 MinGW 原型把 DC 写成 win_hdc*，与 gdi32 的 HDC_ 不是同一个 typedef 名，故显式转。
      GlyphSink sink;
      sink.rt = rt;
      // pixelsPerDip 保持默认的 1.0（它一旦设成 96，字形被放大 96 倍画到缓冲区外，实测全黑）。
      if (SUCCEEDED(layout->Draw(&sink, &sink, FLOAT(draw_pad), FLOAT(draw_pad)))) {
        DIBSECTION ds{};
        HGDIOBJ ho = GetCurrentObject((HDC)rt->GetMemoryDC(), OBJ_BITMAP);
        if (ho && GetObject(ho, sizeof(ds), &ds) == sizeof(ds) && ds.dsBm.bmBits) {
          const uint8_t *bits = static_cast<const uint8_t *>(ds.dsBm.bmBits);
          const int stride = int(ds.dsBm.bmWidthBytes);
          // 实测：内存第 0 行 = DC 的顶行（虽然 biHeight/bmHeight 报正数）。
          // alpha 字节恒 0，覆盖度在 B 通道 ⇒ 只抄 B，颜色与预乘全在下面自己算。
          const int rows = std::min(CH, int(ds.dsBm.bmHeight));
          for (int y = 0; y < rows; y++)
            for (int x = 0; x < CW; x++)
              cov[size_t(y) * CW + x] = bits[size_t(y) * size_t(stride) + x * 4 + 2];
          ok = rows == CH;
        }
      }
    }
  }
  if (rt) rt->Release();
  if (gi) gi->Release();
  layout->Release();
  if (!ok) {
    g_err = L"字形绘制或取回位图失败";
    return false;
  }
  // 墨迹外接框：从覆盖场里量，量完就决定交出去的图有多大、从画布的哪一格开始抄。
  int r0 = CH, r1 = -1, c0 = CW, c1 = -1;
  for (int y = 0; y < CH; y++) {
    for (int x = 0; x < CW; x++) {
      if (!cov[size_t(y) * CW + x]) continue;
      if (y < r0) r0 = y;
      if (y > r1) r1 = y;
      if (x < c0) c0 = x;
      if (x > c1) c1 = x;
    }
  }
  // 每次真正排版落一行：图幅与墨迹是「DirectWrite 到底画了多少、画在哪」的读数，
  // 预览和导出对账时都读这一行（命中缓存的样式不重排，也就不写）。墨 0x0 = 一个字都没画出来。
  {
    wchar_t tb[200];
    swprintf(tb, 200, L"排版: 字数=%d 字体=%ls 号=%.0f 行高=%.1f 图=%dx%d 墨=%dx%d 留白=%d",
             int(t.content.size()), t.family.c_str(), t.size, line_h,
             r1 >= r0 ? (c1 - c0 + 1 + 2 * pad) : 0, r1 >= r0 ? (r1 - r0 + 1 + 2 * pad) : 0,
             r1 >= r0 ? (c1 - c0 + 1) : 0, r1 >= r0 ? (r1 - r0 + 1) : 0, pad);
    app::trace(tb);
  }
  if (r1 < r0) return true;  // 全是空格/空白：没有墨迹，也就没有位图，但不算失败
  const int W = (c1 - c0 + 1) + 2 * pad, H = (r1 - r0 + 1) + 2 * pad;
  // 把这一圈墨连同四周的留白一起从画布上抄下来 ⇒ 后面三层合成只管最终这张图，
  // 距离变换、盒子都用它自己的坐标，没有第二套偏移要记。
  std::vector<uint8_t> crop(size_t(W) * size_t(H));
  for (int y = 0; y < H; y++)
    memcpy(crop.data() + size_t(y) * W,
           cov.data() + size_t(y + r0 - pad) * CW + size_t(c0 - pad), size_t(W));
  cov.swap(crop);

  out.w = W;
  out.h = H;
  out.pad = pad;
  out.px.assign(size_t(W) * size_t(H) * 4, 0);

  std::vector<uint16_t> sx, sy;
  if (outline_px >= 1) jfa_seeds(cov.data(), W, H, outline_px, sx, sy);
  constexpr uint16_t kInvalid = 0xFFFF;

  // Blender 的合成顺序：轮廓（over 空底）→ 字形（over）→ 方框（under，只在已经透明的地方上色）。
  // 一层一趟，逐像素，因为三层用的现有 alpha 各不相同。
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const size_t i = size_t(y) * W + x;
      float dst[4] = {0, 0, 0, 0};
      const float c = cov[i] / 255.0f;

      if (outline_px >= 1 && sx[i] != kInvalid) {
        const float d = std::hypotf(float(x) - sx[i], float(y) - sy[i]);
        // 它那条 fading：alpha = clamp(outline_width - distance + 1, 0, 1)（:686）。
        float alpha = clampf(float(outline_px) - d + 1.0f, 0.0f, 1.0f);
        if (alpha > 0.0f) {
          // 「轮廓不侵入字形」的那一条（:697-701）：全不透明的字里轮廓为 0，
          // 半透明的字边缘按字自己的反 alpha 收，两者按文字 alpha 插值。
          const float mul_opaque = c >= 1.0f ? 0.0f : 1.0f;
          const float mul_transparent = 1.0f - c;
          const float mul = mul_transparent + (mul_opaque - mul_transparent) * t.color[3];
          alpha *= mul;
          float src[4];
          premul(t.outline_color, alpha, src);
          blend_over(dst, src);
        }
      }
      if (c > 0.0f) {
        float src[4];
        premul(t.color, c, src);
        blend_over(dst, src);
      }
      if (box_on) {
        // 盒子的界 = 墨迹外接框 ± margin。这张图就是墨迹外框再让出 margin + 轮廓宽，
        // 所以往里收 outline_px 那一圈就是盒子（Blender 的 boundbox ± margin，:1153-1157；
        // 它那份 boundbox 是行格，这里是墨迹，差的就是行格里那点空白）。
        const float x1 = float(outline_px), y1 = float(outline_px);
        const float x2 = float(W - outline_px), y2 = float(H - outline_px);
        if (float(x) >= x1 && float(x) < x2 && float(y) >= y1 && float(y) < y2) {
          float cr = clampf(float(t.box_roundness) * (y2 - y1) / 2.0f, 0.0f,
                            std::min(x2 - x1, y2 - y1) / 2.0f);
          float alpha = 1.0f;
          if (cr > 0.0f) {
            // 四角用 Blender 的超椭圆（curve_pow = 2.1，:777-786），连原点的 -1 偏移一起抄。
            float ox = 0, oy = 0;
            bool corner = false;
            if (float(x) < x1 + cr && float(y) < y1 + cr) {
              corner = true; ox = x1 + cr - 1; oy = y1 + cr - 1;
            } else if (float(x) >= x2 - cr && float(y) < y1 + cr) {
              corner = true; ox = x2 - cr; oy = y1 + cr - 1;
            } else if (float(x) < x1 + cr && float(y) >= y2 - cr) {
              corner = true; ox = x1 + cr - 1; oy = y2 - cr;
            } else if (float(x) >= x2 - cr && float(y) >= y2 - cr) {
              corner = true; ox = x2 - cr; oy = y2 - cr;
            }
            if (corner) {
              const float p = 2.1f;
              const float r = std::powf(std::powf(std::fabs(float(x) - ox), p) +
                                            std::powf(std::fabs(float(y) - oy), p),
                                        1.0f / p);
              alpha = clampf(cr - r, 0.0f, 1.0f);
            }
          }
          if (alpha > 0.0f) {
            // under：fac = 1 - 已有 alpha（fill_rect_alpha_under :752-753、:789）。
            float src[4];
            premul(t.box_color, alpha, src);
            const float fac = 1.0f - dst[3];
            dst[0] = fac * src[0] + dst[0];
            dst[1] = fac * src[1] + dst[1];
            dst[2] = fac * src[2] + dst[2];
            dst[3] = fac * src[3] + dst[3];
          }
        }
      }

      uint8_t *p = out.px.data() + i * 4;
      // 三层按预乘合成（Blender 的字节缓冲就是预乘，算式照抄），交出去之前换成直通：
      // Render2D 的混合态是 SrcAlpha·InvSrcAlpha（render2d.cpp:167-171），它自己乘 alpha。
      const float a = clampf(dst[3], 0.0f, 1.0f);
      const float inv = a > 0.0f ? 1.0f / a : 0.0f;
      p[0] = uint8_t(clampf(dst[2] * inv, 0.0f, 1.0f) * 255.0f + 0.5f);  // B
      p[1] = uint8_t(clampf(dst[1] * inv, 0.0f, 1.0f) * 255.0f + 0.5f);  // G
      p[2] = uint8_t(clampf(dst[0] * inv, 0.0f, 1.0f) * 255.0f + 0.5f);  // R
      p[3] = uint8_t(a * 255.0f + 0.5f);
    }
  }
  return true;
}

// ---------------------------------------------------------------- 缓存
// 键是样式的全部输入（哈希只是查找用的桶号，命中还要逐字段比原值，所以不存在「哈希撞了
// 就用错图」）。文本条带数量是个位数，一帧里要画的样式远少于预算；但字号、边距这些是拖动
// 能连改的，一路拖过去就是一串新样式，所以这张表要有上限（见 bitmap 里那一次清空）。
uint64_t sig_of(const TextVars &t, int canvas_w) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  };
  mix(t.content.data(), t.content.size() * sizeof(wchar_t));
  mix(t.family.data(), t.family.size() * sizeof(wchar_t));
  mix(&t.bold, sizeof(t.bold));
  mix(&t.italic, sizeof(t.italic));
  mix(&t.size, sizeof(t.size));
  mix(&t.line_spacing, sizeof(t.line_spacing));
  mix(t.color, sizeof(t.color));
  mix(t.outline_color, sizeof(t.outline_color));
  mix(&t.outline_width, sizeof(t.outline_width));
  mix(t.box_color, sizeof(t.box_color));
  mix(&t.box_margin, sizeof(t.box_margin));
  mix(&t.box_roundness, sizeof(t.box_roundness));
  mix(&canvas_w, sizeof(canvas_w));
  return h;
}

struct Entry {
  TextVars v;
  int canvas_w = 0;
  Bitmap b;
};

std::map<uint64_t, Entry> g_cache;
uint64_t g_serial = 0;
std::vector<std::wstring> g_families;
bool g_families_done = false;

}  // namespace

const Bitmap *bitmap(const TextVars &t, int canvas_w) {
  if (t.content.empty() || t.size < 1.0) return nullptr;
  const uint64_t key = sig_of(t, canvas_w);
  auto it = g_cache.find(key);
  // 命中还要比原值：哈希相同但字段不同（不可能，但这条比一次的成本是零）⇒ 认字段不认哈希。
  const bool same = it != g_cache.end() && it->second.canvas_w == canvas_w &&
                    it->second.v.content == t.content && it->second.v.family == t.family &&
                    it->second.v.bold == t.bold && it->second.v.italic == t.italic &&
                    it->second.v.size == t.size && it->second.v.line_spacing == t.line_spacing &&
                    it->second.v.outline_width == t.outline_width &&
                    it->second.v.box_margin == t.box_margin &&
                    it->second.v.box_roundness == t.box_roundness &&
                    std::memcmp(it->second.v.color, t.color, sizeof(t.color)) == 0 &&
                    std::memcmp(it->second.v.outline_color, t.outline_color, sizeof(t.outline_color)) == 0 &&
                    std::memcmp(it->second.v.box_color, t.box_color, sizeof(t.box_color)) == 0;
  if (same) return &it->second.b;
  Entry e;
  e.v = t;
  e.canvas_w = canvas_w;
  if (!render(t, canvas_w, e.b)) return nullptr;
  if (e.b.px.empty()) return nullptr;
  e.b.serial = ++g_serial;
  // 预算 64 张：一帧里要用的样式最多等于屏幕上的文本条带数，用不到这么多；超了就整张清空
  // —— 拖一次滑杆留下的那几十张旧样式已经没有条带在指它们了。
  if (g_cache.size() >= 64) g_cache.clear();
  auto res = g_cache.insert_or_assign(key, std::move(e));
  return &res.first->second.b;
}

// 位图 → PNG：WIC 的编码器收 32bppBGRA，和 Bitmap::px 的内存形状逐字相同，所以一格不重排、
// 一行不复制，整张直接交出去。
bool save_png(const Bitmap &b, const std::wstring &path) {
  g_err.clear();
  if (b.px.empty() || b.w < 1 || b.h < 1) {
    g_err = L"没有可写出的文本位图";
    return false;
  }
  IWICImagingFactory *fac = nullptr;
  IWICStream *stream = nullptr;
  IWICBitmapEncoder *enc = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac));
  if (SUCCEEDED(hr)) hr = fac->CreateStream(&stream);
  if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
  if (SUCCEEDED(hr)) hr = fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
  if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
  if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, nullptr);
  if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
  if (SUCCEEDED(hr)) {
    GUID pf = GUID_WICPixelFormat32bppBGRA;
    frame->SetSize(UINT(b.w), UINT(b.h));
    frame->SetPixelFormat(&pf);
    const UINT stride = UINT(b.w) * 4;
    hr = frame->WritePixels(UINT(b.h), stride, stride * UINT(b.h),
                            static_cast<BYTE *>(const_cast<uint8_t *>(b.px.data())));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
  }
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  if (fac) fac->Release();
  if (FAILED(hr)) {
    g_err = L"文本位图写出失败：" + path;
    return false;
  }
  return true;
}

const std::vector<std::wstring> &families() {
  if (g_families_done) return g_families;
  g_families_done = true;
  if (!ensure_factory()) return g_families;
  IDWriteFontCollection *fc = nullptr;
  if (FAILED(g_factory->GetSystemFontCollection(&fc, FALSE))) return g_families;
  // 界面语言的 BCP-47 标签（"zh-CN"），DirectWrite 的字面量表用的就是同一个写法。
  static const std::wstring ui_locale = [] {
    wchar_t tag[LOCALE_NAME_MAX_LENGTH]{};
    if (!GetUserDefaultLocaleName(tag, LOCALE_NAME_MAX_LENGTH)) return std::wstring();
    return std::wstring(tag);
  }();
  const UINT32 n = fc->GetFontFamilyCount();
  g_families.reserve(n);
  for (UINT32 i = 0; i < n; i++) {
    // 1.0 的字面量表只有 IDWriteFontFamily::GetFamilyNames 这一条路（这个头里没有
    // GetFontFamilyNames，那是后一版加在集合上的）。同一家族按 UI 语言挑一条：
    // 中文系统上「微软雅黑」和「Microsoft YaHei」都在表里，取界面语言那条才对得上用户的心里的名字。
    IDWriteFontFamily *fam = nullptr;
    IDWriteLocalizedStrings *names = nullptr;
    if (FAILED(fc->GetFontFamily(i, &fam)) || FAILED(fam->GetFamilyNames(&names))) {
      if (fam) fam->Release();
      continue;
    }
    std::wstring picked;
    const UINT32 cnt = names->GetCount();
    for (UINT32 k = 0; k < cnt; k++) {
      UINT32 len = 0;
      names->GetStringLength(k, &len);
      std::wstring name(len + 1, L'\0');
      if (FAILED(names->GetString(k, &name[0], len + 1))) continue;
      name.resize(len);
      UINT32 llen = 0;
      names->GetLocaleNameLength(k, &llen);
      std::wstring loc(llen + 1, L'\0');
      names->GetLocaleName(k, &loc[0], llen + 1);
      if (picked.empty()) picked = name;  // 兜底：第一条
      // 界面语言命中就换掉它（GetUserDefaultUILanguage 的 "zh-CN" 形如那串 BCP-47 标签）。
      if (loc == ui_locale) picked = name;
    }
    if (!picked.empty()) g_families.push_back(std::move(picked));
    names->Release();
    fam->Release();
  }
  fc->Release();
  return g_families;
}

const std::wstring &last_error() { return g_err; }

}  // namespace txt
