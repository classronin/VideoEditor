#pragma once
// 文本条带的排版器：DirectWrite 排好字，输出一张以墨迹外接框为界的直通 BGRA 位图。
//
// 全工程的「素材内容文字」只有这一处排版（界面自己的按钮文字在 render2d 的 GDI 那条路上，
// 它不属于任何条带）。Blender 把文本做成 effectdata + 它自带的 BLF 字体层
// （sequencer/intern/effects/effect_text.cc）；本工程用 DirectWrite 取代 BLF，
// 但盒子/轮廓/字形三层的算式逐条抄 Blender，见 text.cpp。
//
// 单位：位图的像素 = 输出画布像素（Blender 的 text_size 也是画布像素，
// 只在预览渲染比例那一处折算一次，text_effect_line_size_get :797-807）。
#include <stdint.h>
#include <string>
#include <vector>

struct TextVars;

namespace txt {

struct Bitmap {
  int w = 0, h = 0;
  // 四边对称留白 = 轮廓宽度 + 方框边距。对称是必需的：位图中心 = 文本块中心，
  // 「中心锚 + 位置XY」才不随留白大小漂移。
  int pad = 0;
  // w*h*4，直通（非预乘）BGRA，内存第 0 行 = 顶行。三层算式内部按预乘叠加（Blender 的字节缓冲
  // 就是预乘），交出去之前除回 alpha —— Render2D 的混合态是 SrcAlpha·InvSrcAlpha，它自己会乘。
  std::vector<uint8_t> px;
  // 这张图的身份：每次真正排版过就发一个新号，永不复用。绘制层拿它当「同一帧内容」的判据
  // （Render2D::image_affine 的 token），相等就可以跳过上传 —— 所以它不能是地址，
  // 地址会被回收后再拿来装别的内容。
  uint64_t serial = 0;
};

// 当前样式对应的那张位图。内容/样式/画布宽完全没变就返回缓存里那一张（同一指针），
// 变了才重排一趟。画布宽参与排版只因为方框边距是它的比例（Blender：box_margin * image->x）；
// 画布高与文字无关，所以不做键。content 为空或字号小于 1 ⇒ nullptr
// （Blender 的 early_out_text / effects_can_render_text 同一道门）。
const Bitmap *bitmap(const TextVars &t, int canvas_w);

// 把这张位图原样写成一只 PNG。导出交给 ffmpeg 的只有文件，而透明只有 PNG/TIFF/WebP 这类
// 容器装得下（JPG 没有 alpha 通道，字边缘会被填成一圈黑），所以排版器的图落盘只有这一条路。
// 尺寸、像素、行序都不改（内存第 0 行 = 顶行）。失败返回 false，原因进 last_error。
bool save_png(const Bitmap &b, const std::wstring &path);

// 系统字体家族名，按 DirectWrite 的字体收集枚举一次（读得到 .ttc 与中文家族）。
const std::vector<std::wstring> &families();

// 排版失败的原因（状态栏）；成功时为空串。
const std::wstring &last_error();

}  // namespace txt
