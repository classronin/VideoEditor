#pragma once
// Blender 默认主题的字面量，全部照抄 release/datafiles/userdef/userdef_default_theme.c
// （它是 U_theme_default，resources.cc:1102-1118 直接 memcpy 进 U.themes，不从 .blend 读）。
// 该文件的宏 RGBA(0xRRGGBBAA) 按 {r,g,b,a} 展开，所以下面的十六进制即像素值本身。
// 字段含义见 source/blender/makesdna/DNA_theme_types.h。
#include <algorithm>
#include <cmath>

#include "render2d.h"

namespace btheme {

// ---- UI 度量 ---------------------------------------------------------------
// 全部照抄 windowmanager/intern/wm_window.cc:765-780（WM_window_dpi_set_userdef）：
//   U.dpi        = 系统DPI * ui_scale * (72/96)          // Blender 以 72dpi 为基准
//   pixelsize    = max(1, U.dpi / 64)                    // 高 DPI 时线宽加倍
//   scale_factor = U.dpi / 72  ->  UI_SCALE_FAC
//   widget_unit  = round(18 * scale_factor) + 2 * pixelsize   // 100% 时 = 20
// Blender 默认字号三档都是 11.0 点（UI_interface_c.hh:426/429/432），点 -> 像素按 U.dpi/72；
// 按需求整档提到 12 号 —— 全界面只有 font_pt 这一个字号旋钮，改它 = 改所有文字。
inline float scale = 1.0f;          // UI_SCALE_FAC
inline float pixelsize = 1.0f;      // U.pixelsize
inline int widget_unit = 20;        // UI_UNIT_X == UI_UNIT_Y
inline constexpr float font_pt = 12.0f;
inline int font_px = int(font_pt);  // @ U.dpi=72 时点 == 像素
inline void set_dpi(int system_dpi, float ui_scale = 1.0f) {
  float dpi = float(system_dpi) * ui_scale * (72.0f / 96.0f);
  pixelsize = float(std::max(1, int(dpi / 64)));
  scale = dpi / 72.0f;
  widget_unit = int(std::lround(18.0f * scale)) + int(2 * pixelsize);
  font_px = int(std::lround(font_pt * scale));
}
// Blender 里到处是 UI_UNIT_Y 的倍数：行高 1.0、面板标题 1.25、菜单内边距 0.2。
inline float u(float mult) { return float(widget_unit) * mult; }
// 圆角 = roundness * widget_unit（interface_widgets.cc:3420）。Blender 出厂这一档是 0.2
// （≈4px），面板上的方块、按钮、滚动条滑块都读它；按需求整档归 0 = 全界面方角。
// 只有这一个旋钮：把它拆成「面板内方角、面板外圆角」需要给每个控件再传一次作用域，
// 那就是同一个值两处写。
inline float radius(float roundness = 0.0f) { return roundness * float(widget_unit); }
inline float icon_size() { return 16.0f * scale; }  // UI_ICON_SIZE（DNA_theme_types.h:27）

// 主题的 shade/mix：resources.cc:1283-1298 的 get_color_shade_3ubv 是「加法偏移」
// （col = theme + offset，逐通道夹紧到 0..255），不是乘法；GPU 端
// gpu_shader_sequencer_widget.bsl.hh:54 的 color_shade(rgb, shade) = rgb + shade/255
// 也是同一套语义。mix = get_color_blend_3ubv 的线性插值。
inline Color shade(Color c, int s) {
  auto add = [s](uint8_t v) {
    int x = int(v) + s;
    return uint8_t(x < 0 ? 0 : (x > 255 ? 255 : x));
  };
  return Color{add(c.r), add(c.g), add(c.b), c.a};
}
inline Color mix(Color a, Color b, float f) {
  auto l = [f](uint8_t x, uint8_t y) {
    return uint8_t(std::lround(std::min(255.0f, float(x) + (float(y) - float(x)) * f)));
  };
  return Color{l(a.r, b.r), l(a.g, b.g), l(a.b, b.b), l(a.a, b.a)};
}

// ---- 控件配色（wcol_*） ----------------------------------------------------
struct Wcol {
  Color outline, outline_sel, inner, inner_sel, item, text, text_sel;
  // Blender 出厂各档 roundness 是 0.1~0.5；按需求全归 0（方角），见上面 radius() 的说明。
  float roundness = 0.0f;
};

// userdef_default_theme.c:22-30
inline const Wcol wcol_regular{{0x3d, 0x3d, 0x3d}, {0x3d, 0x3d, 0x3d}, {0x54, 0x54, 0x54},
                               {0x47, 0x72, 0xb3}, {0x1d, 0x1d, 0x1d, 0x80}, {0xe6, 0xe6, 0xe6},
                               {0xff, 0xff, 0xff}};
// :32-40 图标/开关按钮：item 是白色（图标着色用）
inline const Wcol wcol_tool{{0x3d, 0x3d, 0x3d}, {0x3d, 0x3d, 0x3d}, {0x54, 0x54, 0x54},
                            {0x47, 0x72, 0xb3}, {0xff, 0xff, 0xff},    {0xe6, 0xe6, 0xe6},
                            {0xff, 0xff, 0xff}};
// :52-60 文本/数值输入：底 0x1d1d1d，编辑态 0x181818，item 是光标色
inline const Wcol wcol_text{{0x3d, 0x3d, 0x3d}, {0x3d, 0x3d, 0x3d}, {0x1d, 0x1d, 0x1d},
                            {0x18, 0x18, 0x18}, {0xff, 0xff, 0xff, 0x33}, {0xe6, 0xe6, 0xe6},
                            {0xff, 0xff, 0xff}};
// :92-100 纯数值（无滑块）
inline const Wcol wcol_num{{0x3d, 0x3d, 0x3d}, {0x3d, 0x3d, 0x3d}, {0x54, 0x54, 0x54},
                           {0x22, 0x22, 0x22}, {0x47, 0x72, 0xb3}, {0xe6, 0xe6, 0xe6},
                           {0xff, 0xff, 0xff}};
// :102-110 数值滑块：item 即进度条填充色
inline const Wcol wcol_numslider{{0x3d, 0x3d, 0x3d}, {0x3d, 0x3d, 0x3d}, {0x54, 0x54, 0x54},
                                 {0x22, 0x22, 0x22}, {0x47, 0x72, 0xb3}, {0xe6, 0xe6, 0xe6},
                                 {0xff, 0xff, 0xff}};
// :112-121 属性区竖排 tab（列宽 = UI_UNIT_Y * 1.25，见 space_properties.cc 的
// REGION_TYPE_TABS prefsizey；选中的 tab 用 inner_sel，和面板同色）
inline const Wcol wcol_tab{{0x1d, 0x1d, 0x1d}, {0x1d, 0x1d, 0x1d}, {0x1d, 0x1d, 0x1d},
                           {0x30, 0x30, 0x30}, {0x1d, 0x1d, 0x1d}, {0x98, 0x98, 0x98},
                           {0xff, 0xff, 0xff}};
// :132-140 头部菜单按钮
inline const Wcol wcol_menu{{0x3d, 0x3d, 0x3d}, {0x3d, 0x3d, 0x3d}, {0x28, 0x28, 0x28},
                             {0x47, 0x72, 0xb3, 0xb3}, {0xd9, 0xd9, 0xd9}, {0xe6, 0xe6, 0xe6},
                             {0xff, 0xff, 0xff}};
// :142-150 头部下拉（无底、悬停 0xffffff1a）
inline const Wcol wcol_pulldown{{0x3d, 0x3d, 0x3d, 0}, {0x3d, 0x3d, 0x3d, 0}, {0x22, 0x22, 0x22, 0},
                                 {0xff, 0xff, 0xff, 0x1a}, {0xff, 0xff, 0xff, 0x8f},
                                 {0xd9, 0xd9, 0xd9},
                                 {0xff, 0xff, 0xff}};
// :152-160 弹出菜单本体
inline const Wcol wcol_menu_back{{0x24, 0x24, 0x24}, {0x24, 0x24, 0x24}, {0x18, 0x18, 0x18},
                                  {0x47, 0x72, 0xb3}, {0xd9, 0xd9, 0xd9}, {0x99, 0x99, 0x99},
                                  {0xff, 0xff, 0xff}};
// :162-170 弹出菜单条目
inline const Wcol wcol_menu_item{{0x3d, 0x3d, 0x3d, 0}, {0x3d, 0x3d, 0x3d, 0}, {0x18, 0x18, 0x18, 0},
                                  {0x47, 0x72, 0xb3},    {0xff, 0xff, 0xff, 0x8f},
                                  {0xdd, 0xdd, 0xdd},
                                  {0xff, 0xff, 0xff}};
// :192-201 右侧滚动条。view2d.cc:1566-1576 在画之前把 outline 的 alpha 和 widget_emboss 的
// alpha 都清 0，而 wcol_scroll.inner 的 alpha 出厂就是 0，于是走「轨道看不见」那一支：
// inner[3] = 0.25 * alpha_vert（alpha_vert 初始化后恒为 255，view2d.cc:371）= 63。
// 圆角 = roundness * 条宽（interface_widgets.cc:4464，vertical 用 size_x）：Blender 出厂
// 0.5 = 一头一尾的半圆，这里按需求归 0 = 方头，draw_scrollbar 读的就是这一档。
// 滑块颜色是 item（内部先 copy_v4_v4_uchar(inner, item)）。
inline const Wcol wcol_scroll{{0x3d, 0x3d, 0x3d}, {0x3d, 0x3d, 0x3d}, {0x22, 0x22, 0x22, 63},
                              {0xff, 0xff, 0xff}, {0x54, 0x54, 0x54}, {0xe6, 0xe6, 0xe6},
                              {0xff, 0xff, 0xff}, 0.0f};
// :281-287 面板。panel_back 出厂 0x3d3d3d，按需求压到和界面背景（window_back 0x1d1d1d）
// 差不多的一档：面板不再是一块灰板，只留 1px 描边分出边界。
inline const Color panel_back{0x22, 0x22, 0x22},
    panel_sub_back{0x00, 0x00, 0x00, 0x1f}, panel_outline{0xff, 0xff, 0xff, 0x11},
    panel_title{0xe6, 0xe6, 0xe6}, panel_text{0xe6, 0xe6, 0xe6}, panel_active{0x47, 0x72, 0xb3};
inline const float panel_roundness = 0.0f;  // 出厂 :280 是 0.4，按需求方角
// :232-248 wcol_state —— 动画状态色。控件底色按 .blend=0.5 混进去
// （interface_widgets.cc:3344-3364 的 widget_color_blend_from_flags + color_blend_v3_v3）：
// 有曲线=绿底，当前帧有键=黄底，被驱动=紫，值改过没打键=橙。这里只用得到前两档
// （本工程没有驱动，「改过未打键」在 Blender 里要拿求值值和 RNA 原值比，我们的存储
// 只有标量+曲线两套，比不出来，所以不自创那一档）。
inline const Color state_anim{0x53, 0x99, 0x2e}, state_key{0xb3, 0xae, 0x36};
inline const float state_blend = 0.5f;
// :249-261 边框与光标
inline const Color widget_emboss{0, 0, 0, 38}, editor_border{0x16, 0x16, 0x16},
    editor_outline{0xff, 0xff, 0xff, 21}, editor_outline_active{0xff, 0xff, 0xff, 42},
    widget_text_cursor{0x71, 0xa8, 0xff};

// ---- 时间线 / 通道 / 标尺 --------------------------------------------------
// :517-547 space_sequencer
// 条带区底色：出厂 .back 的 alpha 是 0（:518），但取值那一头在非 overlap 区把 alpha
// 钉成 255（resources.cc:216-218 的 get_color_ptr），frame_buffer_clear 又是
// 「只取 RGB、alpha 写 1.0」（:1548-1554）—— 两头合起来 = 不透明的 0x181818。
// 所以它比窗口底色 0x1d1d1d 暗一档，这就是 Blender 时间线看起来更「沉」的那一格。
inline const Color seq_back{0x18, 0x18, 0x18}, seq_text{0xa6, 0xa6, 0xa6}, seq_text_hi{0xff, 0xff, 0xff},
    seq_grid{0x30, 0x30, 0x30}, row_alternate{0xff, 0xff, 0xff, 0x05};
// 帧范围（开始帧/结束帧）的两笔：draw_timeline_sfra_efra
// （sequencer_timeline_draw.cc:1779-1794）。
// 范围外两块 = shade_alpha(TH_BACK, -10, -100)：TH_BACK 在非 overlap 区的 alpha 被
// get_color_ptr 钉成 255（resources.cc:216-218），所以是 {0x18-10, 0x18-10, 0x18-10, 255-100}
// = 比条带区更沉一档的半透明黑；范围本身不铺色（那要靠 preview range 工具打开，默认关）。
// 结束边界 = shade(TH_BACK, -60)：24-60 夹到 0、alpha 不动 = 不透明纯黑，画在 x = efra+1。
inline const Color seq_range_outside{0x0e, 0x0e, 0x0e, 0x9b}, seq_range_edge{0, 0, 0, 255};
inline const Color strip_movie{0x62, 0x82, 0xa3}, strip_movieclip{0x8f, 0x6e, 0x56},
    strip_mask{0x8f, 0x56, 0x56}, strip_image{0x8c, 0x54, 0x8c}, strip_scene{0x99, 0x99, 0x99},
    strip_audio{0x56, 0x8f, 0x6d}, strip_effect{0x7b, 0x5f, 0x80}, strip_transition{0x86, 0x7a, 0xcc},
    strip_meta{0x7b, 0x99, 0x5c}, strip_text{0x9f, 0x92, 0x6f}, strip_color{0x99, 0x99, 0x5c},
    strip_active{0xff, 0xff, 0xff}, strip_selected{0xff, 0x8f, 0x0d};
// :294-304 channels / scrubbing 区
inline const Color chan_back{0x1d, 0x1d, 0x1d}, chan_text{0xb8, 0xb8, 0xb8},
    chan_text_selected{0xff, 0xaf, 0x23}, scrub_back{0x1d, 0x1d, 0x1d}, scrub_text{0x80, 0x80, 0x80};
// 标记线/图标色：anim_markers.cc:490-498 用 TH_TIME_MARKER_LINE(_SELECTED)，
// 对应 scrubbing.time_marker(_selected)（:302-303）。
inline const Color scrub_time_marker{0xff, 0xff, 0xff, 0x80},
    scrub_time_marker_selected{0xff, 0xff, 0xff, 0xff};
// :312 common.anim.playhead
inline const Color playhead{0x47, 0x72, 0xb3};
// 条带 1px 外描边 = TH_BACK 加 -40 偏移（sequencer_strips_batch.cc:58）。
// shade 是加法：0x18(24) - 40 夹紧到 0，所以描边就是纯黑（透明度按 255 提交，
// 与批量绘制的其它颜色一样走 opaque 顶点色）。
inline const Color strip_outline{0, 0, 0, 255};
// 预览区的取景框：Blender 在选中条带外围画的是一圈 2px 轮廓
// （strip_draw_image_origin_and_outline，sequencer_preview_draw.cc:1058-1084，
// GPU_line_width(2) + TH_SEQ_ACTIVE/TH_SEQ_SELECTED 的 LINE_LOOP）。
// 本工程把它做成可拖的取景框，颜色取主题里唯一的绿（.gizmo_a = 0x4da84d，:270），
// 与「只读的选中色」区分开：绿色的一看就是能上手拖的东西。
inline const Color framing{0x4d, 0xa8, 0x4d};
// 画幅的蚂蚁线（draw.cpp 的 draw_canvas_ants）：暗底 + 亮段两档缺一不可。
// 亮段单独用会消失在白色画面里，所以先铺一条不透明的暗底，亮段的空档露出的就是它 ——
// 这就是「黑白相间」的蚂蚁线，只是暗底整条只画 4 次矩形，比逐格交替省一半顶点。
inline const Color ants_base{0x00, 0x00, 0x00, 0xcc}, ants_light{0xff, 0xff, 0xff, 0xff};
// 预览网格（顶栏「网络」，draw.cpp 的 draw_preview_grid）：格线 + 中心十字两档。
// 都要在半黑半白的画面上认得出，所以不用纯黑也不用纯白，靠 alpha 压住对比。
inline const Color grid_line{0xff, 0xff, 0xff, 0x33}, grid_center{0xff, 0xff, 0xff, 0x99};
// 顶部/底部条：:712,722 topbar/statusbar = 0x181818b3
inline const Color topbar{0x18, 0x18, 0x18, 0xb3};
// 铺满客户区的那一层底。Blender 那里窗口先清成纯黑（wm_draw.cc:1134/1169 的
// GPU_clear_color(0,0,0,0)），每块 region 再自己 frame_buffer_clear(TH_BACK) 盖出底色，
// 所以「界面背景」其实是各区域底色本身：channels(:295) 与 scrubbing(:300) = 0x1d1d1d，
// 条带区(:518 经 get_color_ptr 钉成不透明) = 0x181818。这一档取轨道头/标尺带那一组，
// 面板、预览区都落在它上面。
inline const Color window_back{0x1d, 0x1d, 0x1d};

}  // namespace btheme
