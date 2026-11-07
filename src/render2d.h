#pragma once
// D3D11 上的即时 2D 层：矩形、纹理四边形、UTF-16 文字（GDI 生成灰度纹理并缓存）。
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <string>
#include <map>
#include <vector>

struct Color {
  uint8_t r = 255, g = 255, b = 255, a = 255;
  Color() = default;
  Color(uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) : r(R), g(G), b(B), a(A) {}
  uint32_t pack() const { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24); }
};

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  Rect() = default;
  Rect(float X, float Y, float W, float H) : x(X), y(Y), w(W), h(H) {}
  bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  Rect inset(float l, float t, float r, float b) const { return {x + l, y + t, w - l - r, h - t - b}; }
};

class Render2D {
 public:
  bool init(HWND hwnd);
  const std::wstring &fail_reason() const { return fail_; }
  void shutdown();
  void resize(int w, int h);
  void begin_frame();
  void end_frame(bool vsync);

  void rect(const Rect &r, Color c);
  void rect_border(const Rect &r, Color fill, Color border);
  // Blender 的控件都是圆角矩形（圆角 = wcol.roundness * widget_unit）。
  void rrect(const Rect &r, float radius, Color c);
  // 圆角描边环：沿 r 边缘画 thickness 宽的环（选中条带的橙色框就是它）。
  void rring(const Rect &r, float radius, float thickness, Color c);
  void tri(float x0, float y0, float x1, float y1, float x2, float y2, Color c);
  void line_seg(float x0, float y0, float x1, float y1, float width, Color c);
  // BGRA 缓冲上传后按矩形绘制；crop_* 为源像素裁切量。预览交回的字节就是这个格式，
  // 上传只做一次 memcpy（以前中间过一遍 RGB24，两头各付一次逐像素换序）。
  // token 非 0 时：与上次相同即视为同一帧内容，跳过转换与上传（对应 Blender 预览 GPU 纹理复用）。
  void image(const Rect &r, const uint8_t *bgra, int w, int h, Color tint = Color{},
             int crop_l = 0, int crop_r = 0, int crop_t = 0, int crop_b = 0, uint64_t token = 0);
  // 仿射绘制：围绕 pivot 平移 (dx,dy)、按 (scale_x, scale_y) 缩放、旋转 degrees。
  // 两轴独立缩放对应 StripTransform::scale_x/scale_y（DNA_sequence_types.h:269-270）；
  // flip_x/flip_y 对应 Strip::flag 的 SEQ_FLIPX/SEQ_FLIPY，只交换纹素坐标。
  // pivot = transform.origin（render.cc:425-455 的三步：
  //   q = om + loc + R·S·(p − om)，om = 显示框尺寸 * (origin − 0.5)）。
  // origin 用 Blender 的归一化写法：(0,0) = 图片左下角、(0.5,0.5) = 中心。
  void image_affine(const Rect &r, float dx, float dy, float scale_x, float scale_y, float degrees,
                    const uint8_t *bgra, int w, int h, Color tint = Color{},
                    int crop_l = 0, int crop_r = 0, int crop_t = 0, int crop_b = 0,
                    uint64_t token = 0, bool flip_x = false, bool flip_y = false,
                    float origin_x = 0.5f, float origin_y = 0.5f);

  float text(float x, float y, const std::wstring &s, int size, Color c);
  // 量尺寸。它们会填字形/度量缓存，所以缓存成员是 mutable，方法本身是 const：
  // 布局与绘制代码里大量 const App& 的函数需要量文字宽度。
  float text_width(const std::wstring &s, int size) const;
  int text_height(int size) const;

  // 裁剪矩形栈（像素，左上原点）：对应 Blender 每个 region 只在自己矩形内绘制。
  void push_clip(const Rect &r);
  void pop_clip();

  int width() const { return width_; }
  int height() const { return height_; }
  // 读回当前后备缓冲并写 PNG（用于自动化验证）。
  bool capture_png(const wchar_t *path);

 private:
  struct R2v { float x, y, u, v; uint32_t color; };
  struct Tex {
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    int w = 0, h = 0;
    uint64_t token = 0;  // 当前纹理内容对应的帧标识
  };
  Tex *scratch(int w, int h);            // 图像用纹理（按尺寸复用）
  Tex *text_tex(const std::wstring &s, int size, float *advance);
  void upload_bgra(Tex *t, const uint8_t *bgra, int w, int h);
  void flush();
  void apply_clip();
  void push_vert(float x, float y, float u, float v, uint32_t col);
  void quad(const float pts[8], const float uv[8], uint32_t col, Tex *tex);
  void rect_uv(const Rect &r, const float uv[8], uint32_t col, Tex *tex);

  ID3D11Device *device_ = nullptr;
  std::wstring fail_;
  ID3D11DeviceContext *ctx_ = nullptr;
  IDXGISwapChain1 *swap_ = nullptr;
  ID3D11RenderTargetView *rtv_ = nullptr;
  ID3D11VertexShader *vs_ = nullptr;
  ID3D11InputLayout *layout_ = nullptr;
  ID3D11PixelShader *ps_ = nullptr;
  ID3D11Buffer *vb_ = nullptr;
  ID3D11Buffer *cb_ = nullptr;
  ID3D11SamplerState *sampler_ = nullptr;
  ID3D11BlendState *blend_ = nullptr;
  // 2D 渲染没有「背面」概念：镜像条带（SEQ_FLIPX/Y 把整个画布翻面）会把三角形绕序
  // 反过来，用默认光栅器状态（CULL_BACK）就会被剔掉、画面直接消失。
  ID3D11RasterizerState *rs_ = nullptr;
  ID3D11ShaderResourceView *white_srv_ = nullptr;
  ID3D11Texture2D *white_tex_ = nullptr;
  D3D11_VIEWPORT vp_{};
  int width_ = 0, height_ = 0;
  std::vector<uint8_t> staging_;       // 文字用：GDI 灰度 -> bgra8 缓冲
  std::vector<R2v> verts_;             // 当前批次
  std::vector<Rect> clips_;            // 裁剪栈，空 = 整个渲染目标
  D3D11_RECT scissor_set_{-1, -1, -1, -1};  // 已提交给 GPU 的裁剪矩形，变化时才 flush
  ID3D11ShaderResourceView *batch_srv_ = nullptr;
  std::map<uint64_t, Tex> scratch_;      // key = (w<<32)|h，引用稳定
  std::map<std::wstring, Tex> text_srv_;
  mutable std::map<std::wstring, float> text_w_;
  HDC mem_dc_ = nullptr;
  HBITMAP mem_bmp_ = nullptr;
  mutable std::map<int, HFONT> fonts_;
  mutable std::map<int, int> font_h_;
};
