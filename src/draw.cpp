// 区域绘制。与 main.cpp（输入）和 ops.cpp（数据/命令/菜单）分开，对应 Blender 里
// editors/space_sequencer/*_draw.cc 与 editors/animation/time_scrub_ui.cc 的职责。
// 每个数字都标了参考树 E:\GitHub\Blender-VSE\blender（Blender 5.4）里的出处。
#include "app.h"

#include "gesture.h"
#include "text.h"
#include "waveform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace app {

// ================================================================ 时间字符串
//
// BLI_timecode_string_from_time（source/blender/blenlib/intern/timecode.cc:24-155）。
// 用户偏好取 Blender 默认 USER_TIMECODE_MINIMAL，所以只实现 MINIMAL 分支；
// 其余样式（SMPTE_MSF / MILLISECONDS / SUBRIP / SECONDS_ONLY）本工程没有开关入口，
// 没有入口的分支不写，避免“看起来像但点了没反应”的东西。
static std::wstring timecode_minimal(double time_seconds, double fps, int brevity) {
  wchar_t buf[64];
  int hours = 0, minutes = 0, seconds = 0, frames = 0;
  double time = time_seconds;
  const wchar_t *neg = L"";
  if (time < 0) {  // 负时间的处理（timecode.cc:35-39）
    neg = L"-";
    time = -time;
  }
  if (time >= 3600.0) {
    hours = int(time) / 3600;
    time = std::fmod(time, 3600.0);
  }
  if (time >= 60.0) {
    minutes = int(time) / 60;
    time = std::fmod(time, 60.0);
  }
  if (brevity <= 0) {
    // 帧号由“这一秒的小数部分 * fps”再四舍五入得到（:62-64）。
    seconds = int(time);
    frames = int(std::lround((time - double(seconds)) * fps));
  } else {
    seconds = int(std::lround(time));
  }
  if (brevity <= 0) {
    if (hours) swprintf(buf, 64, L"%ls%02d:%02d:%02d+%02d", neg, hours, minutes, seconds, frames);
    else if (minutes) swprintf(buf, 64, L"%ls%02d:%02d+%02d", neg, minutes, seconds, frames);
    else swprintf(buf, 64, L"%ls00:%02d+%02d", neg, seconds, frames);
  } else {
    if (hours) swprintf(buf, 64, L"%ls%02d:%02d:%02d", neg, hours, minutes, seconds);
    else swprintf(buf, 64, L"%ls%02d:%02d", neg, minutes, seconds);
  }
  return buf;
}

// ED_time_scrub_frame_str_get（time_scrub_ui.cc:68-83）：播放头框与 HUD 用的当前帧字符串。
// 秒模式固定 brevity = -1（帧级精度）；帧模式是 int(frame) 截断。
std::wstring time_str(const App &a, double t) {
  double frame = t * a.fps();
  if (a.draw_frames) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", int(frame));
    return buf;
  }
  return timecode_minimal(frame / a.fps(), a.fps(), -1);
}

// 标尺刻度字符串：frame_to_time_string / frame_to_string（view2d_draw.cc:422-460）。
// 秒模式的 brevity 只在「步长 >= 帧率」时升到 1（即不再显示帧位）。
static std::wstring ruler_label(const App &a, float frame, float step) {
  if (a.draw_frames) {
    wchar_t buf[32];
    if (step >= 1.0f) swprintf(buf, 32, L"%d", int(frame));
    else if (step >= 0.5f) swprintf(buf, 32, L"%.1f", frame);
    else if (step >= 0.01f) swprintf(buf, 32, L"%.2f", frame);
    else swprintf(buf, 32, L"%.3f", frame);
    return buf;
  }
  int brevity = -1;
  if (step >= float(a.fps())) brevity = 1;
  return timecode_minimal(frame / float(a.fps()), a.fps(), brevity);
}

// ================================================================ 网格步长
//
// 全部照抄 source/blender/editors/interface/view2d/view2d_draw.cc：
//   get_divisor(:50-79)、calculate_grid_step(:88-120)、
//   get_min_line_distance_x(:488-502)、view2d_draw_lines_x(:530-552)。
// 时间线的调用参数是 show_fractions=false、draw_minor_lines=false
// （sequencer_timeline_draw.cc:1987-1993），所以走整数路径、只画主线。
static int get_divisor(int distance) {
  const int divisors[3] = {2, 3, 5};
  bool clean[3];
  for (int i = 0; i < 3; ++i) {
    int result = distance / divisors[i];
    bool no_remainder = result * divisors[i] == distance;
    // 商为 2 的优先（2 秒一格对动画最有用）。
    if (no_remainder && result == 2) return divisors[i];
    clean[i] = no_remainder;
  }
  for (int i = 0; i < 3; ++i)
    if (clean[i]) return divisors[i];
  return distance;
}

static int calculate_grid_step(int base, float pixel_width, float view_width, float min_distance) {
  if (view_width == 0.0f) return 1;
  float pixels_per_view_unit = pixel_width / view_width;
  int distance = std::max(base, 1);
  if (pixels_per_view_unit * distance > min_distance) {
    while (distance > 1) {
      int divisor = get_divisor(distance);
      int result = distance / divisor;
      if (pixels_per_view_unit * result < min_distance) break;
      distance = result;
    }
  } else {
    while (pixels_per_view_unit * distance < min_distance && distance < (1 << 30)) distance *= 2;
  }
  return distance == 0 ? 1 : distance;
}

// 主线最小间距 = max(MIN_MAJOR_LINE_DISTANCE, 最宽标签 + 6)。
// MIN_MAJOR_LINE_DISTANCE = U.v2d_min_gridsize * UI_SCALE_FAC，v2d_min_gridsize 的默认值
// 是 45（DNA_userdef_types.h:1002）。标签宽度用 step=0 测量（get_label_width:465-482 传的
// 就是 0），取视野两端绝对值里较宽的那个。
static float grid_min_distance(const App &a, float frame_lo, float frame_hi) {
  float w = std::max(a.r2d.text_width(ruler_label(a, std::fabs(frame_lo), 0.0f), btheme::font_px),
                     a.r2d.text_width(ruler_label(a, std::fabs(frame_hi), 0.0f), btheme::font_px));
  return std::max(45.0f * btheme::scale, w + 6.0f);
}

// 视野内的主网格步长（单位：帧）。base = round(fps)（view2d_draw.cc:560）。
static int grid_step_frames(const App &a) {
  float view_w = a.lanes.w;
  float view_frames = float(view_w / a.tl.pps * a.fps());
  float min_dist = grid_min_distance(a, float(x_time(a, a.lanes.x) * a.fps()),
                                     float(x_time(a, a.lanes.x + a.lanes.w) * a.fps()));
  return calculate_grid_step(int(std::lround(a.fps())), view_w, view_frames, min_dist);
}

// ================================================================ 矢量图标
//
// Blender 的图标是 SVG 字形（release/datafiles/icons_svg + 内置图标字体），本工程只有
// rect/tri/line_seg，所以按同名 SVG 的轮廓画等价几何：形状、朝向、粗细都对齐，
// 只有线条是真的、不是贴上去的装饰。
// CHECKBOX_HLT / CHECKBOX_DEHLT（通道头静音图标，sequencer_channels_draw.cc:104）：
// 圆角方框 + 勾；未勾选只有方框。
static void icon_checkbox(Render2D &r, const Rect &b, bool checked, Color c) {
  float t = std::max(1.0f, btheme::pixelsize);
  r.rring(b, btheme::radius(0.2f), t, c);
  if (!checked) return;
  float m = b.w * 0.22f;
  r.line_seg(b.x + m, b.y + b.h * 0.55f, b.x + b.w * 0.44f, b.y + b.h - m, t * 1.4f, c);
  r.line_seg(b.x + b.w * 0.44f, b.y + b.h - m, b.x + b.w - m, b.y + m, t * 1.4f, c);
}
// LOCKED / UNLOCKED（sequencer_channels_draw.cc:132）：锁体 + 弧形锁环。
static void icon_lock(Render2D &r, const Rect &b, bool locked, Color c) {
  float t = std::max(1.0f, btheme::pixelsize * 1.2f);
  Rect body{b.x + b.w * 0.22f, b.y + b.h * 0.48f, b.w * 0.56f, b.h * 0.40f};
  r.rrect(body, btheme::radius(0.2f), c);
  Rect shackle{b.x + b.w * 0.30f, b.y + b.h * 0.14f, b.w * 0.40f, b.h * 0.52f};
  if (locked) {
    r.rring(shackle, shackle.w * 0.5f, t, c);
  } else {  // 解锁：锁环只留半边，且整体偏移（icons_svg/unlocked.svg 的开口形状）
    r.rring({shackle.x + shackle.w * 0.5f, shackle.y - shackle.h * 0.12f, shackle.w, shackle.h},
            shackle.w * 0.5f, t, c);
  }
}
// LINKED（条带标题条上的连接角标，sequencer_timeline_draw.cc:1038-1043）：
// icons_svg/linked.svg 是「两枚小环 + 一根 45° 连杆」，环在左下与右上、连杆穿过两心。
static void icon_linked(Render2D &r, const Rect &b, Color c) {
  const float t = std::max(1.0f, btheme::pixelsize * 1.1f);
  const float d = b.w * 0.44f;  // 一枚环的直径
  const Rect lo{b.x + b.w * 0.06f, b.y + b.h * 0.50f, d, d};
  const Rect hi{b.x + b.w * 0.50f, b.y + b.h * 0.06f, d, d};
  r.line_seg(lo.x + lo.w * 0.5f, lo.y + lo.h * 0.5f, hi.x + hi.w * 0.5f, hi.y + hi.h * 0.5f, t, c);
  r.rring(lo, d * 0.5f, t, c);
  r.rring(hi, d * 0.5f, t, c);
}
// 面板折叠三角由 ui::panel_header 画（同一套 ui 层度量），这里不再重复一份。

// ================================================================ 前置声明
static void draw_playhead(App &a);
struct StripGeom;
static void draw_retiming_overlay(App &a, const Clip &c, const StripGeom &g);

// ================================================================ 条带几何
//
// sequencer_timeline_draw.cc:221-222 用 STRIP_OFSBOTTOM / STRIP_OFSTOP 内缩通道行
// （DNA_sequence_types.h:623/624 = 0.05 / 0.95），圆角由通道行高决定
// （sequencer_strips_batch.cc:30-43），把手宽 = pixelx * 5 * U.pixelsize 且不超过条带
// 长度的 1/4（sequencer_intern.hh:142 的实现）。
StripGeom strip_geom(const App &a, const Clip &c) {
  StripGeom g;
  const std::vector<TrackRow> &rows = track_rows(a);
  const TrackRow *row = nullptr;
  for (const TrackRow &r : rows)
    if (r.track == c.track) row = &r;
  if (!row || row->h <= 0.0f) return g;

  float x0 = time_x(a, c.timeline_start);
  float x1 = time_x(a, c.timeline_start + c.duration);
  // 完全不在视野里就不画（Blender 的批量绘制同样按 v2d->cur 剔除）。
  if (x1 < a.lanes.x || x0 > a.lanes.x + a.lanes.w) return g;

  g.body = {x0, row->y + row->h * 0.05f, x1 - x0, row->h * 0.90f};
  if (g.body.h < 1.0f) g.body.h = 1.0f;

  float hp = row->h;  // 圆角阈值看的是通道行高（1/pixely），不是条带高
  g.radius = hp < 16.0f ? 0.0f : (hp < 64.0f ? 4.0f : (hp < 128.0f ? 6.0f : 8.0f));
  g.radius = std::min(g.radius, std::min(g.body.h, g.body.w) * 0.5f);

  g.handle = std::min(5.0f * btheme::pixelsize, g.body.w * 0.25f);
  if (g.handle < 1.0f) g.handle = 0.0f;

  // 文字条：min(0.40, 20*UI_SCALE_FAC*pixely) 个 view 单位（:209-212），换算成像素就是
  // min(40% 条带高, 20*scale)。
  g.header_h = std::min(g.body.h * 0.40f, 20.0f * btheme::scale);

  // can_draw_text_overlay（:172-186）：条带高 >= 8*scale；「先占住文字条」的那一类
  // （有声波、纯色、文字）门槛换成 20*scale（strip_hides_text_overlay_first，:165-170）。
  // can_draw_strip_content（:188-197）恰好反过来：普通条带要 20*scale，有声波的那一类
  // 只要 8*scale —— 所以声带在很矮的行里仍然画得出波形，只是不配文字条。
  // seq_draw_waveforms_poll（:147-163）在本工程恒等于「这条是 SOUND」：条带是 SOUND 且有
  // 声带数据、叠加层总开关 SEQ_SHOW_OVERLAY 与「显示所有波形」都是默认值、静音不影响它。
  const bool wave = c.kind == kStripSound;
  g.text_overlay = g.body.h >= (wave ? 20.0f : 8.0f) * btheme::scale;
  g.content = g.body.h > (wave ? 8.0f : 20.0f) * btheme::scale;
  // 文字条的「预留」只看高度（Blender 的 strip_content_top 按 can_draw_text_overlay 定，
  // :270-275），但**上屏**还要过一道长度门槛（:741 那一类），所以再与一次宽度条件。
  g.show_text = g.text_overlay && g.body.w > 32.0f * btheme::scale;
  g.waveform = wave && (g.text_overlay || g.content);  // !strip_is_too_small，:245-246
  if (g.header_h > g.body.h) g.header_h = g.body.h;
  g.visible = true;
  return g;
}

// 变速键带的几何 = strip_retiming_keys_box_get（sequencer_retiming_draw.cc:86-95）：
// 横 = 条带矩形全宽，纵 = 键中心上下各半颗键；键尺寸 10*U.pixelsize（:74-78），中心在条带
// 下沿往上 4+size/2 个像素（:80-84）。绘制和命中都走这一份，保证「看到的 = 点到的」。
RetKeyBand retiming_band(const StripGeom &g) {
  RetKeyBand b;
  b.size = 10.0f * btheme::pixelsize;
  b.cy = g.body.y + g.body.h - (4.0f + b.size * 0.5f);
  b.left = g.body.x;
  b.right = g.body.x + g.body.w;
  return b;
}

// ================================================================ 通道头几何
//
// sequencer_channels_draw.cc:53-75：图标宽 = widget_unit*0.8*cs，绘制空间是
// wmOrtho2_pixelspace(winx/cs, winy/cs)，所以 1 个单位 = cs 个设备像素；行短的时候
// cs < 1，图标按 cs^2 收缩（照抄，不做“看起来更顺眼”的修正）。
// 与源码的两处不同（按需求）：
//   · 数字字号 = 图标高（源码的 label 框高是 font 1.5 倍，字本身仍是 UI 字号）；
//   · 数字右对齐、只留 0.25iw 的空 —— 源码把 label 列伸到 right-3.5iw，那是给
//     “Channel 12" 这种长名字用的，名字只剩一个数字时就在数字和打勾之间空出十几像素。
ChanWidgets chan_widgets(const App &a, const TrackRow &row) {
  ChanWidgets w;
  const float widget_unit = float(btheme::widget_unit);
  w.scale = std::min(row.h / (widget_unit * 0.6f), 1.0f);
  const float iw_units = widget_unit * 0.8f * w.scale;
  const float iw = iw_units * w.scale;             // 设备像素
  const float right = a.chan.x + a.chan.w;
  w.icon_w = iw;
  float y = row.y + (row.h - iw) * 0.5f;           // widget_y_offset * cs
  w.lock = {right - iw * 1.5f, y, iw, iw};
  w.mute = {right - iw * 2.5f, y, iw, iw};
  const float text_h = iw;                         // 数字与图标等高
  const float used = iw * 2.75f;                   // 图标 2.5iw + 数字与图标之间 0.25iw
  float margin_x = iw_units * 0.65f * w.scale;
  w.label = {a.chan.x + margin_x, row.y + (row.h - text_h) * 0.5f,
             std::max(0.0f, a.chan.w - used - margin_x), text_h};
  return w;
}

// ================================================================ 通用排布
// 头部控件：高 UI_UNIT_Y，在 HEADERY 区域内垂直居中（interface_layout 的行高规则）。
struct HeaderRow {
  App *a;
  Rect region;
  float y, h;
  float x;
  float right_x;
  explicit HeaderRow(App &ap, const Rect &rgn) : a(&ap) {
    region = rgn;
    h = std::min(btheme::u(1.0f), rgn.h);
    y = rgn.y + (rgn.h - h) * 0.5f;
    x = rgn.x + 0.25f * btheme::u(1.0f);
    right_x = rgn.x + rgn.w - 0.25f * btheme::u(1.0f);
  }
  Rect next(float w) {
    Rect r{x, y, w, h};
    x += w + 0.25f * btheme::u(1.0f);
    return r;
  }
  Rect prev(float w) {
    Rect r = align_prev(w);
    right_x -= 0.25f * btheme::u(1.0f);
    return r;
  }
  // align=True 的一行：按钮挨在一起，彼此不留间隔。
  Rect align_prev(float w) {
    right_x -= w;
    return {right_x, y, w, h};
  }
};

static float pad_h() { return 0.5f * btheme::u(1.0f); }

// 属性行：Blender 的 property 行在 use_property_split = True 时是
// 「标签列(40%) | 控件列(60%)」，标签右对齐；列宽比就是
// ui::Layout::PROPERTY_SPLIT_FACTOR = 0.4f（editors/include/UI_interface_layout.hh:123，
// interface_layout.cc:3541 的 layout_row->split(PROPERTY_SPLIT_FACTOR, true)）那一档是给
// 英文长标签留的；这里的标签只剩两三个汉字，四成宽度全是标签左边的死空。
// column(align=True) 里的行彼此紧贴（没有行距），列与列之间才留空隙。

// 所以分栏不按比例，按内容：标签列 = 所有参数里最长那条标签的字宽（props_label_w）。
// 这里不另立一张名单 —— 每条标签在真正画它那一行时被量一次、累进这个最大值，
// 读数永远跟着实际画出来的标签走。
static float g_label_w = 0.0f;
static int g_label_font = -1;

float props_label_w() {
  // 字号一变（改 DPI）旧读数作废；第一帧还没量到，先按「四条汉字 + 空格 + 一个拉丁
  // 字母」估一格，第二帧起就是实测值 —— 面板默认宽度跟着它走，所以只差第一帧。
  if (btheme::font_px != g_label_font) {
    g_label_font = btheme::font_px;
    g_label_w = 4.5f * float(btheme::font_px);
  }
  return g_label_w;
}

struct PropRows {
  // layout.active / layout.enabled 关掉的整列：条目置灰且点不动（id 登记成 0）。
  struct Active {
    bool v = true;
    Active() = default;
    explicit Active(bool on) : v(on) {}
    operator bool() const { return v; }
  };
  Rect box;
  float x, y, w, row_h, pad;
  Active active;
  // 展开态 —— 行才画、命中才登记；折叠的那一组只留标题。落在面板 region 外那一截照样画
  // 照样登记，只是被裁剪框切掉、被 Block 的裁剪判据挡掉（见 ui::Item::off_clip）。
  bool rows = false;
  PropRows(const Rect &area) {
    pad = 0.25f * btheme::u(1.0f);
    box = area.inset(pad, pad, pad, pad);
    x = box.x;
    y = box.y;
    w = box.w;
    row_h = btheme::u(1.0f);
  }
  Rect row() {
    Rect r{x, y, w, row_h};
    y += r.h;
    return r;
  }
  // 占 n 行的格子（内容框跟着行数长高）。
  Rect row_n(int n) {
    Rect r{x, y, w, row_h * float(n)};
    y += r.h;
    return r;
  }
  void gap(float h = 0.25f) { y += h * btheme::u(1.0f); }
  // 在已经拿到的那一格上画标签，返回控件应该占的右列矩形。行和占多行的格子共用它。
  Rect split_in(Render2D &r, const Rect &rr, const std::wstring &label,
                Color tc = btheme::panel_text) {
    float lx = split_x(rr);
    if (!label.empty()) {
      float lw = r.text_width(label, btheme::font_px);
      if (lw > g_label_w) g_label_w = lw;
      float tx = lx - lw - 0.25f * btheme::u(1.0f);
      // 面板拖窄到标签列放不下时这一行不画标签：整串文字往左溢出会爬出面板框，
      // 那比「这一行暂时没标签」更糟。
      if (tx >= rr.x) {
        r.text(tx, rr.y + (rr.h - btheme::font_px) * 0.5f, label, btheme::font_px, tc);
      }
    }
    return {lx, rr.y, rr.x + rr.w - lx, rr.h};
  }
  // 画标签，返回控件应该占的右列矩形。
  Rect split_row(Render2D &r, const std::wstring &label, Color tc = btheme::panel_text) {
    return split_in(r, row(), label, tc);
  }
  // 标签列与控件列之间那条竖线：整页所有面板共用同一个算法，行与行才对得齐。
  // 面板被拖窄到放不下「标签列 + 一格控件」时退成对半分，控件列至少留一半宽度。
  static float split_x(const Rect &rr) {
    const float gap = 0.25f * btheme::u(1.0f);
    return rr.x + std::min(props_label_w() + gap, rr.w * 0.5f);
  }
  // 一整行都是控件（use_property_split = False 时的行，比如条带名）。
  Rect full_row() { return row(); }
};

// 置灰：Blender 对 active/enabled 关掉的行只是降低对比（wcol 的文字从 text_sel 退到
// text，控件底压暗），条目仍然画出来、仍然有标签。这里用同构的做法：文字取
// panel_text 与 panel_back 的中点，控件上再压一层半透明的面板底。
static Color dim_text() { return btheme::mix(btheme::panel_text, btheme::panel_back, 0.5f); }
static Color inactive_wash() {
  return Color{btheme::panel_back.r, btheme::panel_back.g, btheme::panel_back.b, 0x80};
}

// 数值按钮上显示的串：编辑态显示键入的内容。进输入态时 edit_text 就是原值整串选中
// （见 begin_edit），删空之后就真的显示空 —— 和 Blender 的 TextField 一样。
static std::wstring shown_text(App &a, int id, Clip *c) {
  if (a.edit_id == id) return a.edit_text;
  return prop_text(a, id, c);
}

// 滚动条占掉的那一列。这一列恒在：Blender 的内容永远按「cur = tot 减掉整条槽」铺
// （view2d.cc:164-198），槽宽只随悬停淡出在两个数之间插值、从不归零（:1868-1891），
// 只有槽里那根条是画/不画。所以点折叠让条消失时，行不会往回挤一格，也不会再自动对齐。
// 宽度只有 props_scroll_slot 一个出处。
static float props_gutter(const App &a) { return a.props.w > 0.0f ? props_scroll_slot() : 0.0f; }

// ================================================================ 顶栏
//
// 四个下拉菜单 + 一颗导出：默认（保存/恢复启动默认）、面板（面板左侧/右侧）、
// 下载（检测/下载 FFmpeg）、网络（预览网格），最后是「导出」。
// 导出用的是现成的命令入口（kHitCmd | kCmdExport），左键分发本来就把 kHitCmd 整段
// 交给 run_cmd，顶栏不需要新加判定。打开、设置两个入口按要求撤下，命令和键位都还在：
// 打开 = Ctrl+O（或直接把文件拖进窗口）、导出 = Ctrl+F12、面板换边还有分界线右键。
static void draw_topbar(App &a) {
  Render2D &r = a.r2d;
  r.rect(a.topbar, btheme::topbar);
  HeaderRow hr(a, a.topbar);
  struct Item { const wchar_t *name; int menu; };
  const Item items[] = {{L"默认", kMDefault},
                        {L"面板", kMPanelSide},
                        {L"下载", kMDownload},
                        {L"网络", kMGrid}};
  for (const Item &it : items) {
    const std::wstring t = it.name;
    const float w = r.text_width(t, btheme::font_px) + 2.0f * pad_h();
    const int id = kHitMenu | it.menu;
    ui::but_pulldown(r, a.blk, id, hr.next(w), t, a.hover == id);
  }
  const std::wstring ex = L"导出";
  const int exid = kHitCmd | kCmdExport;
  ui::but_push(r, a.blk, exid,
               hr.next(r.text_width(ex, btheme::font_px) + 2.0f * pad_h()), ex,
               a.hover == exid);
}


// ================================================================ 预览区
// 画幅边线 = 蚂蚁线。它标的是「导出成片真正的那一圈」= 面板里格式的实际尺寸，
// 不是任何一条片段的画面边。片段自己的绿框压在它上面时，两条线挨着看不出谁才是
// 边界，多片段剪辑就容易把画幅外的内容一起剪出去（成片留白）。所以这条线要和绿框
// 长得不一样：黑白相间 + 亮段走动，并且画在绿框之上。
//
// 相位只读单调时钟，不新增计时器、不在空闲标脏：回放 / 拖动 / 悬停本来就在重绘，
// 那时蚂蚁线一直走；画面彻底静止时冻住。这是刻意的 —— 让蚂蚁线恒走要每帧强制重绘
// 整个窗口，正是逻辑.md「线程在无事件时休眠 / 杜绝忙等待」不许的那个消耗。
static void ants_edge(Render2D &r, bool horiz, float fixed, float from, float len, float dir,
                      long cum, int cell, int period, int walk) {
  if (len <= 0.0f) return;
  // 亮段条件：周长坐标 p 满足 (p − walk) mod period < cell。四边共用同一个 p，
  // 拐角才不会错相（每边各起各的相位会在角上出现两格连亮的断点）；dir 让四条边的
  // p 沿同一个绕向增长（顶→右→底→左），否则有两条边的蚂蚁线是倒着走的。
  const long base = ((cum - walk) % period + period) % period;
  for (long x = base - period; x < long(len); x += period) {
    const long a = x < 0 ? 0 : x;
    const long b = x + cell > long(len) ? long(len) : x + cell;
    if (b <= a) continue;
    const float t0 = from + dir * float(a), t1 = from + dir * float(b);
    const float s = t0 < t1 ? t0 : t1, w = float(b - a);
    if (horiz)
      r.rect({s, fixed, w, float(cell)}, btheme::ants_light);
    else
      r.rect({fixed, s, float(cell), w}, btheme::ants_light);
  }
}

static void draw_canvas_ants(App &a, const Rect &box) {
  Render2D &r = a.r2d;
  const int cell = int(btheme::pixelsize);  // 一格 = 1 设备像素，线宽 = 一格
  const int period = 2 * cell;              // 亮 1 / 暗 1：黑白等宽，两格一个循环
  if (cell <= 0 || box.w <= 0.0f || box.h <= 0.0f) return;
  // 边线落在画幅内侧：导出包含的就是这一圈像素，画在外侧会去标 letterbox。
  const long x0 = std::lroundf(box.x), y0 = std::lroundf(box.y);
  const long x1 = std::lroundf(box.x + box.w), y1 = std::lroundf(box.y + box.h);
  const long hw = x1 - x0, hh = y1 - y0;
  if (hw <= cell || hh <= cell) return;
  // 暗底整条只画 4 次：亮段的空档露出它，蚂蚁线在白色画面上同样看得见。
  r.rect({float(x0), float(y0), float(hw), float(cell)}, btheme::ants_base);
  r.rect({float(x0), float(y1 - cell), float(hw), float(cell)}, btheme::ants_base);
  r.rect({float(x0), float(y0), float(cell), float(hh)}, btheme::ants_base);
  r.rect({float(x1 - cell), float(y0), float(cell), float(hh)}, btheme::ants_base);
  const int walk = int((GetTickCount64() / 60) % uint64_t(period));  // ≈ 16 像素/秒
  ants_edge(r, true, float(y0), float(x0), float(hw), 1.0f, 0, cell, period, walk);
  ants_edge(r, false, float(x1 - cell), float(y0), float(hh), 1.0f, hw, cell, period, walk);
  ants_edge(r, true, float(y1 - cell), float(x1), float(hw), -1.0f, hw + hh, cell, period, walk);
  ants_edge(r, false, float(x0), float(y1), float(hh), -1.0f, hw + hh + hw, cell, period, walk);
}

// 每片段一只取景框：画的是那一条自己的画面四角（clip_screen_quad 与贴图同一套算式），
// 所以框永远贴着画面，缩放/旋转/裁切/镜像都跟着走。选中（面板正在编辑的那条）画亮绿，
// 其它条画半透明 —— 对应 Blender 给选中条带画轮廓时分的 TH_SEQ_ACTIVE / TH_SEQ_SELECTED
// 两档（sequencer_preview_draw.cc:1400-1401）。声音条带没有画面，rendered_clips 本来就把
// 它们滤掉了，所以不会画出框。
static void draw_framing(App &a) {
  Render2D &r = a.r2d;
  const Rect box = view_rect(a);
  if (box.w <= 0.0f || box.h <= 0.0f) return;
  const float w = 2.0f * btheme::pixelsize;  // GPU_line_width(2)
  const float hs = framing_handle_r();
  Clip *act = props_clip(a);
  for (Clip *c : rendered_clips(a, a.project.playhead)) {
    float pts[8], pv[2];
    clip_screen_quad(a, *c, box, pts, pv);
    const bool on = act == c;
    Color col = btheme::framing;
    col.a = on ? 255 : 120;
    for (int i = 0; i < 4; ++i) {
      const int j = (i + 1) & 3;
      r.line_seg(pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1], w, col);
    }
    // 把手只给正在编辑的那条：叠着的两只框都画方块会糊成一片，而能拖的角本来也只有
    // 光标点到那一条的角（framing_press 从最上层往下命中）。
    if (!on) continue;
    for (int i = 0; i < 4; ++i)
      r.rect({pts[i * 2] - hs, pts[i * 2 + 1] - hs, hs * 2.0f, hs * 2.0f}, col);
  }
}

// 预览网格（顶栏「网络」）：画幅八等到分，横竖各七条，中线亮一档，正中心再点一个像素。
// 线的依据只有画幅自己的边长（对象给的数），不随预览缩放变密，也不进导出。
static void draw_preview_grid(App &a, const Rect &box) {
  Render2D &r = a.r2d;
  if (!a.preview_grid || box.w <= 0.0f || box.h <= 0.0f) return;
  const float w = float(btheme::pixelsize);
  for (int i = 1; i < 8; ++i) {
    const float f = float(i) / 8.0f;
    // 中线（i=4）就是画幅的对称轴，比其余格线亮一档。
    const Color c = (i == 4) ? btheme::grid_center : btheme::grid_line;
    const float x = float(std::lroundf(box.x + box.w * f));
    const float y = float(std::lroundf(box.y + box.h * f));
    r.line_seg(x, box.y, x, box.y + box.h, w, c);
    r.line_seg(box.x, y, box.x + box.w, y, w, c);
  }
  // 交点本身再点一格：做跟踪要的是「中心在哪」，两条线交出来的那一个像素才是答案。
  const long cx = std::lroundf(box.x + box.w * 0.5f), cy = std::lroundf(box.y + box.h * 0.5f);
  r.rect({float(cx), float(cy), float(btheme::pixelsize), float(btheme::pixelsize)},
         btheme::grid_center);
}

static void draw_viewport(App &a) {
  Render2D &r = a.r2d;
  r.rect(a.viewport, btheme::window_back);
  Rect box = view_rect(a);
  r.push_clip(a.viewport);
  // 画幅之外是 TH_IMAGE_BORDER（0x161616）：预览区里渲染框以外的区域比窗口底更暗。
  r.rect(box, btheme::editor_border);
  r.rect_border(box, btheme::editor_border, btheme::editor_outline_active);

  // 叠画顺序照 Blender：query_rendered_strips_sorted 按 channel 升序（iterator.cc:254-257），
  // 逐条把画面画进同一张画布，上面的盖住下面的。
  std::vector<Clip *> stack = rendered_clips(a, a.project.playhead);
  if (!stack.empty()) {
    // offset_x/offset_y 是输出像素位移，y 轴向上为正（渲染坐标取反）。换算系数和取景框、
    // 和拖动用的是同一个函数：框跟着手走，画就得跟着框走。
    const ScreenPx sp = output_to_screen(a, box);
    // 画布以外什么都不该露出来：源比画布大时画面是被画布裁掉的（render.cc:451-452 的
    // center_in_canvas 只负责居中，缩放从不发生）。
    r.push_clip(box);
    bool waiting = false;
    for (Clip *c : stack) {
      // 画面只有两条来源：影片问解码器，文本问排版器（现排一张透明位图，样式没脏就复用同一张）。
      // 音带不产画面，所以它两条都不问。
      const txt::Bitmap *bm =
          c->kind == kStripText ? txt::bitmap(c->text, a.project.fmt.width) : nullptr;
      int fw = 0, fh = 0;
      const uint8_t *bgra = nullptr;
      uint64_t token = 0;
      if (bm) {
        bgra = bm->px.data();
        fw = bm->w;
        fh = bm->h;
        token = bm->serial;  // 同一张图 = 同一个号，纹理就不必重新上传
      } else if (c->kind == kStripMovie) {
        bgra = a.preview.frame(c->id, &fw, &fh);
        token = a.preview.frame_token(c->id);
      }
      if (!bgra) {
        // 只有解码那一路的迟到值得说「取帧中」；文本没内容就是没内容。
        if (c->kind == kStripMovie) waiting = true;
        continue;
      }
      Rect src = clip_base_rect(a, *c, box);
      int sw = 0, sh = 0;
      clip_source_size(a, *c, &sw, &sh);
      float sx = sw ? float(fw) / sw : 1.0f;
      float sy = sh ? float(fh) / sh : 1.0f;
      int cl = int(c->crop_l * sx), cr = int(c->crop_r * sx);
      int ct = int(c->crop_t * sy), cb = int(c->crop_b * sy);
      const XformAt x = xform_at(*c, a.project.fmt, a.project.playhead);
      r.image_affine(src, float(x.ofs_x) * sp.x, float(-x.ofs_y) * sp.y, float(x.scale_x),
                     float(x.scale_y), float(x.rotation), bgra, fw, fh, Color{}, cl, cr, ct, cb,
                     token, c->flip_x, c->flip_y, float(c->origin_x), float(c->origin_y));
    }
    r.pop_clip();
    if (waiting) {
      r.text(box.x + 12.0f * btheme::scale, box.y + 12.0f * btheme::scale, L"取帧中…",
             btheme::font_px, btheme::seq_text);
    }
  }
  // 取景框最后画：每条一只，画在画布的裁剪之外（画面被画幅裁掉时框还在，
  // 和 Blender 的预览轮廓一样是叠在图像之上的 UI 层）。
  draw_framing(a);
  // 网格压在绿框之上、蚂蚁线之下：绿框是要拖的东西，被格子糊住就找不到边界；导出边界
  // 那条线仍然要赢过一切。
  draw_preview_grid(a, box);
  // 画幅的蚂蚁线压在最上层（绿框之上）：片段铺满画幅时两条线重合，此时用户要看的
  // 恰好就是「导出到哪」，所以让边界赢。画在 r.pop_clip() 之前 = 仍在预览区内。
  draw_canvas_ants(a, box);
  // 预览区不画任何文字读数：Blender 5.4 的 preview overlays 只剩游标/选中轮廓/安全框/
  // 元数据/构图引导（DNA_space_enums.h:399-405 的 SEQ_PREVIEW_SHOW_*），
  // 3.x 那行 "Strip/Fit/Clip" 文字已经没有了。
  r.pop_clip();
}

// ================================================================ 标尺 / scrubbing 带
//
// ED_time_scrub_draw（time_scrub_ui.cc:340-368）：
//   draw_background = TH_TIME_SCRUB_BACKGROUND（scrub_back 0x1d1d1d）
//   numbers_rect.ymin = 居中行 - 4*scale，标签再往下 4*scale —— 净结果就是
//   文本盒底边落在「带中心 + 4*scale」处（本项目的 y 轴向下，故减去字高）。
static void draw_scrub(App &a) {
  Render2D &r = a.r2d;
  if (a.scrub.h <= 0) return;
  r.rect(a.scrub, btheme::scrub_back);

  // draw_timeline_grid（sequencer_timeline_draw.cc:1975-1980）：SEQ_TIMELINE_SHOW_GRID
  // 默认开着（space_sequencer.cc:85-89），叠加层分项开关按需求不做，所以恒画。
  const int step = grid_step_frames(a);
  // 可见帧数直接读对象的 pps —— 它在这一帧开头已经过 timeline_view_react，界是它自己
  // 收的，这里再收一遍就是两份真相。用 max(1.0, pps) 的话，pps<1 时可见帧数少算 1/pps
  // 倍（pps=0.1 时 13,940 秒被算成 1,394 秒），网格只画最左边一条窄带。
  const float view_frames = float(a.lanes.w / a.tl.pps * a.fps());
  const double f_lo = x_time(a, a.lanes.x) * a.fps();
  const double f_hi = f_lo + view_frames;
  // 尺子步长只在变化时留一行：它是「同一个界在不同窗口宽度下会跳一档」的唯一读数。
  // 步长 = fps×2^k（calculate_grid_step:114-116 只翻倍），k 由 最小距/每帧像素 决定，
  // 所以窗口一变宽，标签就可能整倍地变密 —— 想知道标签为什么差 2 倍，看这一行。
  static int ruler_step_last = -1;
  if (trace_fp && step != ruler_step_last) {
    wchar_t tb[128];
    swprintf(tb, 128, L"ruler step=%d 视野=%.0f帧 车道宽=%.0f 最小距=%.1f", step, view_frames,
             a.lanes.w, grid_min_distance(a, float(f_lo), float(f_hi)));
    trace(tb);
    ruler_step_last = step;
  }
  // get_parallel_lines_draw_steps(:167-174)：起点 = ceil(min/step)*step。
  double start = step > 0 ? std::ceil(f_lo / step) * double(step) : 0.0;
  int steps = step > 0 && start <= f_hi ? int(std::floor((f_hi - start) / step)) + 1 : 0;
  if (steps > int(a.lanes.w)) steps = 0;  // steps >= steps_max 时整条网格不画（:206-210）
  const float line_top = a.scrub.y;
  // 网格线贯穿整个 WINDOW region（含底部留白），Blender 的 cur->vrect 就是这个范围。
  const float line_bot = a.tl_area.y + a.tl_area.h;
  const double fd = a.frame_dur();
  for (int i = 0; i < steps; ++i) {
    double frame = start + i * step;
    float x = std::truncf(time_x(a, frame * fd)) + 0.5f * btheme::pixelsize;
    r.rect({x - 0.5f * btheme::pixelsize, line_top, btheme::pixelsize, line_bot - line_top},
           btheme::seq_grid);
    std::wstring label = ruler_label(a, float(frame), float(step));
    float lw = r.text_width(label, btheme::font_px);
    float ty = a.scrub.y + a.scrub.h * 0.5f + 4.0f * btheme::scale -
               float(r.text_height(btheme::font_px));
    r.text(std::truncf(x - lw * 0.5f), ty, label, btheme::font_px, btheme::scrub_text);
  }
  draw_playhead(a);
}


// 播放头：get_playhead_dimensions(:104-143) + draw_playhead_stalk/box/tip(:146-223, 259-313)。
// 顺序是 stalk -> box -> tip；框内文字 = TH_TEXT_HI，位置 = 带中心 - int(点*0.38)。
static void draw_playhead(App &a) {
  Render2D &r = a.r2d;
  if (a.scrub.h <= 0) return;
  const std::wstring str = time_str(a, a.project.playhead);
  const float text_w = r.text_width(str, btheme::font_px);
  const float outline = std::max(1.0f, std::round(btheme::scale));
  const float text_pad = 4.0f * btheme::scale;
  const float box_min_w = 24.0f * btheme::scale;
  const float box_w = std::max(text_w + 2.0f * text_pad, box_min_w);
  const float box_margin = 2.0f * btheme::scale;

  // clamp_playhead：框不出带（sequencer_scrubbing_region_draw 传的就是 true）。
  float region_x = time_x(a, a.project.playhead);
  float bound_l = a.scrub.x + text_w * 0.5f;
  float bound_r = a.scrub.x + a.scrub.w - text_w * 0.5f;
  region_x = bound_l > bound_r ? bound_r : std::clamp(region_x, bound_l, bound_r);

  const float stalk_x0 = std::floor(region_x - btheme::pixelsize) - outline;
  const float stalk_x1 = std::floor(region_x + btheme::pixelsize + 1.0f) + outline;
  // 竖线从带下缘一直画到时间线 window 区底（Blender 的 stalk_rect.ymax = scrub.ymin、ymin = 0）。
  const float stalk_top = a.scrub.y + a.scrub.h;
  const float stalk_bot = a.tl_area.y + a.tl_area.h;
  const float band_bot = a.scrub.y + a.scrub.h;
  const float box_top = a.scrub.y + box_margin - outline;
  const float box_bot = band_bot - box_margin + outline;
  const float tip_top = box_bot - outline;
  const float tip_bottom = band_bot + outline;
  const float tip_half = 6.0f * btheme::scale;

  const Color fg = btheme::playhead;
  const Color bg{0, 0, 0, 255};  // shade(TH_BACK, -20) 的加法结果被夹到 0

  // stalk：先画黑色投影框（bg），再画前景（draw_roundbox_4fv_ex 的 outline 语义）。
  Rect stalk{stalk_x0, stalk_top, stalk_x1 - stalk_x0, stalk_bot - stalk_top};
  r.rect(stalk, bg);
  r.rect({stalk.x + outline, stalk.y, stalk.w - outline * 2.0f, stalk.h}, fg);

  Rect box{region_x - box_w * 0.5f, box_top, box_w + 1.0f, box_bot - box_top};
  r.rect(box, bg);
  r.rrect(box.inset(outline, outline, outline, outline), 4.0f * btheme::scale, fg);
  const int th = r.text_height(btheme::font_px);
  float ty = a.scrub.y + a.scrub.h * 0.5f + int(btheme::font_px * 0.38f) - float(th);
  r.text(region_x - text_w * 0.5f, ty, str, btheme::font_px, btheme::seq_text_hi);

  // tip：从框底往下收口到竖线宽度（draw_playhead_tip 的四点三角带）。
  r.tri(region_x - tip_half, tip_top, region_x + tip_half, tip_top,
        stalk_x0 + outline, tip_bottom, fg);
  r.tri(region_x - tip_half, tip_top, stalk_x1 - outline, tip_bottom,
        region_x + tip_half, tip_top, fg);
  r.tri(region_x + tip_half, tip_top, stalk_x1 - outline, tip_bottom,
        stalk_x0 + outline, tip_bottom, fg);
}

// ================================================================ 通道头
static void draw_chan(App &a) {
  Render2D &r = a.r2d;
  r.rect(a.chan, btheme::chan_back);
  const std::vector<TrackRow> &rows = track_rows(a);
  // 半截在视图上/下缘外的行和条带区一样要被区域裁掉。
  r.push_clip(a.chan);
  for (const TrackRow &row : rows) {
    // 超出轨道数的是「还没有条带占用的空通道」，Blender 照样给它一行名字和图标
    // （channels.cc:30-40 通道恒存在，sequencer_channels_draw.cc:279-294 逐可见行画）。
    if (row.track < 0) continue;
    Track blank;
    Track &tr = row.track < int(a.project.tracks.size()) ? a.project.tracks[row.track] : blank;
    ChanWidgets w = chan_widgets(a, row);
    // 行底色：channels 区整块 0x1d1d1d，条带区的隔行亮纹（row_alternate）在 lanes 里画。
    // 图标是 emboss=None 的 Toggle（sequencer_channels_draw.cc:106/147），走
    // WidgetStyle::Icon，wcol 取 wcol_regular；RNA 布尔为真时 but_update_select_flag
    // （interface.cc:2600）置 UI_SELECT，widget_state 把 text 换成 text_sel
    // （interface_widgets.cc:5177-5183）。即「开=白、关=0xe6e6e6」。
    // 静音图标本身：muted 用 CHECKBOX_DEHLT（空格），未静音用 CHECKBOX_HLT（带勾的盒）
    // —— 图标画的是「有没有声音」，sequencer_channels_draw.cc:100。
    icon_checkbox(r, w.mute, !tr.muted,
                  tr.muted ? btheme::wcol_regular.text_sel : btheme::wcol_regular.text);
    icon_lock(r, w.lock, tr.locked, tr.locked ? btheme::wcol_regular.text_sel
                                              : btheme::wcol_regular.text);
    // 名字：重命名态是 wcol_text 输入框（draw_channel_labels:226-240）。
    if (a.rename_track == row.track) {
      // 改名框占整列（源码在重命名时把 margin_x 也让给输入框：label_rect_init:186-190），
      // 否则只有数字那一格，打不进一个词。
      Rect tb{a.chan.x + w.icon_w * 0.25f, row.y + (row.h - btheme::u(1.0f)) * 0.5f,
              a.chan.w - w.icon_w * 0.25f - w.icon_w * 2.75f, btheme::u(1.0f)};
      ui::but_text(r, a.blk, kHitChan | (row.track << 3) | kChanName, tb, a.rename_text, true,
                   &a.edit_ts);
    } else {
      std::wstring name = tr.name.empty() ? std::to_wstring(row.track + 1) : tr.name;
      // 通道名是 Emboss 块里的 Label 按钮：WidgetStyle::Label 把 text 换成 TH_TEXT，
      // 而 TH_TEXT 在 RGN_TYPE_CHANNELS 里取 regions.channels.text = 0xb8b8b8
      // （interface_widgets.cc:5237-5244 + resources.cc:235-240 +
      // userdef_default_theme.c:296）。text_selected（橙）只在 UI_SELECT 时用，
      // 无数据的 Label 永远不满足，所以通道名恒为灰色。
      Color tc = btheme::chan_text;
      const int fs = std::lround(w.icon_w);  // 数字与图标等高
      const float th = float(r.text_height(fs));
      r.text(w.label.x + w.label.w - r.text_width(name, fs),
             w.label.y + (w.label.h - th) * 0.5f, name, fs, tc);
      ui::Item it;
      it.rect = w.label;
      it.id = kHitChan | (row.track << 3) | kChanName;
      it.type = ui::Type::Push;
      a.blk.add(it);
    }
    for (int k = 0; k < 2; ++k) {
      ui::Item it;
      it.rect = k == 0 ? w.lock : w.mute;
      it.id = k == 0 ? (kHitChan | (row.track << 3) | kChanLock)
                     : (kHitChan | (row.track << 3) | kChanMute);
      it.type = ui::Type::Toggle;
      it.state = (k == 0 ? tr.locked : tr.muted) ? 1 : 0;
      a.blk.add(it);
    }
  }
  r.pop_clip();
}

// ================================================================ 声波
//
// draw_seq_waveform_overlay（sequencer_timeline_draw.cc:462-615）的逐列移植：
// 每个设备像素一列，列里取「这一列对应的源时刻」那一桶（250 桶/秒）的 min/max/rms，
// 跨过多个桶时合并成 min 的最小、max 的最大、rms 的最大（:537-547）。
static void draw_waveform(App &a, const Clip &c, const StripGeom &g, float content_top) {
  Render2D &r = a.r2d;
  // half_style（SEQ_TIMELINE_WAVEFORMS_HALF 是默认值，本工程没有关它的入口）：波形从
  // 条带底部往上长，零线贴在底部。Blender 的 y 轴向上，这里全部换成屏幕向下的 y。
  const float y_zero = g.body.y + g.body.h - 2.0f * btheme::pixelsize;  // bottom + pixely*2，:480
  float y_scale = y_zero - content_top;                                 // top - bottom，:485
  if (y_scale <= 0.0f || a.tl.pps <= 0) return;

  // 两端各内缩 3*pixelx（:489/:492），并且对齐到整像素（align_frame_with_pixel，:457-460）
  // —— 不整点对齐的话，滚动时每一列的取样点都在亚像素上漂，波形会抖。
  float x0 = float(std::lround(time_x(a, c.timeline_start) + 3.0f * btheme::pixelsize));
  float x1 = float(std::lround(time_x(a, c.timeline_start + c.duration) - 3.0f * btheme::pixelsize));
  x0 = std::max(x0, std::ceil(a.lanes.x));
  x1 = std::min(x1, std::floor(a.lanes.x + a.lanes.w));
  const int pixels_to_draw = int(std::lround(x1 - x0));
  if (pixels_to_draw < 2) return;  // :500，太短连 job 都不该起

  // 数据挂在文件上（bSound::runtime->waveform），同一 mp4 的两条声带共用一份包络。
  // 先算出几何再取数据，是为了把「这一帧看得见的源秒」一起报过去：
  // 后台只解这些块，没解到的桶读到 0 ⇒ 画出来就是那条零线，烘到了下一帧自己长出来。
  const double src0 = retiming_src_at(c, x_time(a, x0) - c.timeline_start);
  const double src1 = retiming_src_at(c, x_time(a, x1) - c.timeline_start);
  auto sp = WaveCache::instance().view(c.source_path, std::min(src0, src1), std::max(src0, src1));
  // 烘失败 ⇒ nullptr，这一趟什么都不画（:506-509 的 waveform == nullptr）。
  if (!sp || sp->base.empty()) return;
  // Blender 在整条烘完时把它除以峰值（sound.cc:1348-1352，只在峰值 >1 时才除）。这里存的是
  // 原始值、峰值是一块一块才知道的，所以除掉的是分母本身：整条曲线同幅缩放，和它逐字等价。
  y_scale /= sp->norm.load();

  // 按当前缩放挑一层来画：粗层的桶率随缩放走，每列的读数量就恒定落在 1..8 桶里
  // （base 层在 3h22m 素材上一屏铺满时要逐列扫 2,336 桶 = 一帧 4.10ms）。
  const double sec_per_px = 1.0 / a.tl.pps;
  double rate = double(kWaveBucketsPerSecond);
  const std::vector<float> &wave = sp->level_for(a.tl.pps, &rate);
  const int bucket_count = int(wave.size() / 3);
  const double buckets_per_px = rate * sec_per_px;  // = 桶率/pps，挑层后恒 ≤8

  const Color col{255, 255, 255, 127}, col_rms{255, 255, 255, 204};
  // 零线先画、横跨整条：样本接近 0 的那些列连一像素都盖不满，靠它保证条带里始终看得见东西。
  r.line_seg(x0, y_zero, x1, y_zero, 1.0f, col);

  float prev_y_mid = y_zero;
  for (int i = 0; i < pixels_to_draw; i++) {
    const float cx1 = x0 + float(i), cx2 = cx1 + 1.0f;
    // give_frame_index(...) + anim_startofs 在本工程就是「源内秒」：retiming_src_at 已经
    // 含 source_in，并且变速键也参与求值，所以慢放的条带取的是被拉长的那条时间轴上的桶。
    const double src = retiming_src_at(c, x_time(a, cx1) - c.timeline_start);
    const double sample = src * rate;
    const int idx = int(std::lround(sample));
    if (idx < 0) continue;
    if (idx >= bucket_count) break;
    float vmin = wave[idx * 3], vmax = wave[idx * 3 + 1], rms = wave[idx * 3 + 2];
    if (buckets_per_px > 1.0) {
      const int end_idx = int(std::lround(sample + buckets_per_px));
      for (int j = idx + 1; j < bucket_count && j < end_idx; j++) {
        vmin = std::min(vmin, wave[j * 3]);
        vmax = std::max(vmax, wave[j * 3 + 1]);
        rms = std::max(rms, wave[j * 3 + 2]);
      }
    }
    // strip->volume 恒 1（本工程没有音量字段），加上 y_scale 已经除掉归一分母（峰值 ≤1 时
    // 分母就是 1，>1 时除完正好落回 ±1，见上面 sound.cc:1348-1352 那段），所以画出来的
    // 值不可能越过 ±1 ⇒ :560-567 的削顶红柱不会触发，不写那一支。
    // 半幅折叠（:573-586）：同号就画在 [较小的绝对值, 较大的绝对值] 之间；跨过零则从
    // 零线画到最大的绝对值 —— 于是波形始终朝上，读的是「这一列有多响」。
    const bool pos_min = vmin > 0.0f, pos_max = vmax > 0.0f;
    const float abs_min = std::abs(vmin), abs_max = std::abs(vmax);
    if (pos_min == pos_max) {
      vmin = std::min(abs_min, abs_max);
      vmax = std::max(abs_min, abs_max);
    } else {
      vmin = 0.0f;
      vmax = std::max(abs_min, abs_max);
    }
    const float y_top = y_zero - vmax * y_scale;
    const float y_bot = y_zero - vmin * y_scale;
    const float y_mid = (y_top + y_bot) * 0.5f;
    if (y_bot - y_top < 2.0f * btheme::pixelsize) {
      // 不足 2px 高的柱子退化成一条横线，并和上一列的横线接起来（:595-603）。
      if (std::abs(y_mid - prev_y_mid) > btheme::pixelsize)
        r.line_seg(cx1 - 1.0f, prev_y_mid, cx1, y_mid, 1.0f, col);
      r.line_seg(cx1, y_mid, cx2, y_mid, 1.0f, col);
    } else {
      // rms 带画在样值柱之内、先提交，样值柱再盖上去（:605-610 的先后顺序照抄）。
      const float r_top = y_zero - std::min(rms, vmax) * y_scale;
      const float r_bot = y_zero - std::max(-rms, vmin) * y_scale;
      r.rect({cx1, r_top, 1.0f, std::max(0.0f, r_bot - r_top)}, col_rms);
      r.rect({cx1, y_top, 1.0f, y_bot - y_top}, col);
    }
    prev_y_mid = y_mid;
  }
}

// ================================================================ 条带区
//
// sequencer_strips_batch.cc + gpu_shader_sequencer_widget.bsl.hh：整块用条带类型色填充，
// 1px 外描边 = shade(TH_BACK,-40)（加法夹到 0，即纯黑），把手在两端，锁定态画斜纹。
static void draw_lanes(App &a) {
  Render2D &r = a.r2d;
  const std::vector<TrackRow> &rows = track_rows(a);

  // 隔行亮纹（draw_seq_timeline_channels:1300-1318：i 是 1 基通道号，i & 1 才画；
  // 所以条纹跟着通道走，滚动时不会在行间爬）。行可能半截在视图上下缘外，
  // 和 Blender 一样被区域裁掉。
  r.push_clip(a.lanes);
  for (const TrackRow &row : rows) {
    if ((row.track + 1) & 1)
      r.rect({a.lanes.x, row.y, a.lanes.w, row.h}, btheme::row_alternate);
    r.rect({a.lanes.x, row.y + row.h - btheme::pixelsize, a.lanes.w, btheme::pixelsize},
           btheme::editor_outline);
  }
  r.pop_clip();

  // 帧范围的两笔（draw_timeline_sfra_efra， sequencer_timeline_draw.cc:1779-1794）：
  // 范围外左右两块压暗、在 x = 结束帧+1 处一条 1px 黑线；范围本身不铺色。
  // 覆盖整个 WINDOW region 的竖向范围（含底部留白），Blender 那两块矩形用的就是
  // cur 的整个 y 区间；横向只到条带区左缘（CHANNELS 是另一块 region，不跟着压暗）。
  {
    const OutputFormat &f = a.project.fmt;
    const float x0 = time_x(a, a.frame_to_t(int64_t(f.frame_start)));
    const float x1 = time_x(a, a.frame_to_t(int64_t(f.frame_end)) + a.frame_dur());
    const float left = a.lanes.x, right = a.lanes.x + a.lanes.w;
    const float top = a.lanes.y, bot = a.tl_area.y + a.tl_area.h;
    if (x0 > left) r.rect({left, top, x0 - left, bot - top}, btheme::seq_range_outside);
    if (x1 < right) r.rect({x1, top, right - x1, bot - top}, btheme::seq_range_outside);
    if (x1 >= left && x1 <= right)
      r.rect({std::floor(x1), top, float(btheme::pixelsize), bot - top}, btheme::seq_range_edge);
  }

  // 网格已经画到整块（draw_scrub 里从带顶一直到区底），这里只画条带。
  r.push_clip(a.lanes);
  for (const Clip &c : a.project.clips) {
    StripGeom g = strip_geom(a, c);
    if (!g.visible) continue;
    bool sel = clip_selected(a, c.id);
    bool act = c.id == a.selected;
    // 条带类型色：影片 / 声音两档（userdef_default_theme.c 的 sequencer movie/audio；
    // STRIP_TYPE_SOUND 走 .audio 那一档，见 color3ubv_from_strip_type）。
    Color base = c.kind == kStripSound ? btheme::strip_audio : btheme::strip_movie;
    // SEQ_MUTE：向 {128,128,128} 混合 0.5，再压到 MUTE_ALPHA=120。
    if (c.muted) {
      base = btheme::mix(base, Color{128, 128, 128}, 0.5f);
      base.a = 120;
    }

    // 外描边 1px（col_back）：Shader 里是 rect 外扩 1px 的环。
    Rect outer = g.body;
    r.rring(outer, g.radius, btheme::pixelsize, btheme::strip_outline);
    r.rrect(g.body.inset(btheme::pixelsize, btheme::pixelsize, btheme::pixelsize,
                         btheme::pixelsize),
            g.radius, base);

    const float content_top = g.body.y + (g.text_overlay ? g.header_h : 0.0f);
    if (g.show_text) {
      // 文字条：名 | 源 | 时长（SEQ_TIMELINE_SHOW_STRIP_NAME/SOURCE/DURATION 默认全开，
      // space_sequencer.cc:85-89；分隔符与 x 边距见 draw_strip_text_overlay）。
      float m = std::max(2.0f, g.handle * 2.0f - 2.0f * btheme::scale);
      std::wstring txt;
      auto join = [&txt](const std::wstring &part) {
        if (part.empty()) return;
        if (!txt.empty()) txt += L" | ";
        txt += part;
      };
      // 叠加层分项开关按需求不做，三档恒为默认的开（SEQ_TIMELINE_SHOW_STRIP_NAME /
      // _SOURCE / _DURATION，DNA_space_enums.h:424-426；默认值见 space_sequencer.cc:85-89）。
      join(c.label);
      if (c.info) join(c.info->path);
      // SNPRINTF_UTF8("%d", int(strip_length))：就是帧数的整数部分，没有单位后缀。
      join(std::to_wstring(int(c.duration * a.fps())));
      Color tc = c.muted ? Color{192, 192, 192, uint8_t(255 * 0.66f)}
                         : (sel || act ? btheme::seq_text_hi : Color{0, 0, 0});
      int th = r.text_height(btheme::font_px);
      // 连接角标（draw_strip_icons，:1019-1043）：LINKED 占一个
      // ICON_SIZE(=12，:90 的 constexpr) * scale 的槽，右边跟 3*scale 的间距，
      // 图标位置从 icon_indent = 2*handle - 4*pixelx*scale 起（:1020）；
      // 文字起点再按「图标数 ×（宽 + 间距）」整体右移（:1115-1129）。
      // 标签的框。文字条（header_h）是它的归属，但框至少要装得下这份字号：文字条比字号
      // 窄时（行高缩到 ~25px，文字条只有 8px 而字要 15px）框往下长，长到条带下沿为止 ——
      // 和 Blender 在 rect.ymin 那一处「文字条分不出来就用整条条带」是同一族规则
      // （sequencer_timeline_draw.cc:1113），只是把它按「装不装得下」来触发，而不是按
      // 20px 那条阈值。角标和字都在这个框里居中、都被这个框裁，所以不会有半个字爬到条带外。
      const float band_h = std::clamp(float(th), g.header_h, g.body.h);
      const float band_y = g.body.y + (band_h - float(th)) * 0.5f;
      const bool linked = clip_connected(c);
      const float icon_w = 12.0f * btheme::scale;
      const float icon_gap = 3.0f * btheme::scale;
      const float x_text = g.body.x + m + (linked ? icon_w + icon_gap : 0.0f);
      if (linked) {
        float ix = g.body.x + std::max(2.0f, g.handle * 2.0f - 4.0f * btheme::scale);
        icon_linked(r, {ix, g.body.y + (band_h - icon_w) * 0.5f, icon_w, icon_w}, tc);
      }
      // 文字超出条带右缘是被**裁掉**的，不是省略号：Blender 把字符串交给
      // view2d_text_cache_add_rectf（sequencer_timeline_draw.cc:1137 → view2d.cc:2109），
      // 那个 rect 就是绘制时的裁剪框，画不下的字形直接不出。
      r.push_clip(
          {x_text, g.body.y, std::max(0.0f, g.body.x + g.body.w - m - x_text), band_h});
      r.text(x_text, band_y, txt, btheme::font_px, tc);
      r.pop_clip();
    }

    // 锁定斜纹：mod(frag_x+frag_y,12)>=8 的黑纹，α0.25，只在内容区（文字条以下）。
    if (c.locked) {
      const Color hatch{0, 0, 0, 64};
      r.push_clip({g.body.x, content_top, g.body.w, g.body.y + g.body.h - content_top});
      for (float sx = g.body.x - g.body.h; sx < g.body.x + g.body.w; sx += 12.0f * btheme::scale)
        r.line_seg(sx, g.body.y + g.body.h, sx + g.body.h, g.body.y, 4.0f * btheme::scale, hatch);
      r.pop_clip();
    } else {
      // 裁切把手：两端各 handle 宽，未选 = 黑 α0.2，选中 = 描边色。
      if (g.handle > 0.0f) {
        Color hc = sel ? btheme::strip_selected : Color{0, 0, 0, 51};
        r.rect({g.body.x + btheme::pixelsize, g.body.y + btheme::pixelsize, g.handle,
                g.body.h - btheme::pixelsize * 2.0f},
               hc);
        r.rect({g.body.x + g.body.w - g.handle - btheme::pixelsize, g.body.y + btheme::pixelsize,
                g.handle, g.body.h - btheme::pixelsize * 2.0f},
               hc);
      }
    }

    // 高亮：选中 = 1..3px TH_SEQ_SELECTED + 内缩 3..4px 的黑 α0.33；
    // 活动未选 = 1..2px shade(TH_SEQ_ACTIVE,-40)=0xd7d7d7。
    if (sel) {
      float t = std::max(1.0f, std::round(btheme::scale * 2.0f));
      r.rring(g.body, g.radius, t, btheme::strip_selected);
      r.rring(g.body.inset(3.0f * btheme::scale, 3.0f * btheme::scale, 3.0f * btheme::scale,
                           3.0f * btheme::scale),
              g.radius, std::max(1.0f, btheme::scale), Color{0, 0, 0, 85});
    } else if (act) {
      r.rring(g.body, g.radius, std::max(1.0f, btheme::scale), Color{0xd7, 0xd7, 0xd7});
    }
    // 声波：几何按 Blender（零线在条带下沿往上 2px，:480），但**画在描边之上**。
    // Blender 的 overlay 趟子排在 draw_strips_foreground 之前（:1719 vs :1731），选中条带的
    // 2px 描边（gpu_shader_sequencer_widget.bsl.hh:284 的 sdf 1..3px）正好把这条零线盖住
    // —— 安静素材整条就只剩这根线，本工程里它是「这段有没有声、声大还是声小」的唯一读数，
    // 所以把它提到描边之后画。除此之外顺序与 Blender 一致（文字条、把手仍在下面）。
    if (g.waveform) draw_waveform(a, c, g, content_top);
    // 变速叠加层：Blender 在条带批量绘制之后单独一趟（sequencer_retiming_keys_draw
    // 挂在 draw_strip_batch_directed 的后面），这里跟着每条画完再补。
    draw_retiming_overlay(a, c, g);
    // 条带不登记进 ui::Block：拖动要连续吃整个手势，main.cpp 直接按 strip_geom 的矩形判定。
  }

  // 框选矩形（sequencer.select_box）。矩形本身就是这一次手势记的账 —— 画的那一头来读，
  // 不在外壳上另存一份起点/终点。
  if (SelectBox *sb = gesture_as<SelectBox>(a)) {
    Rect b = sb->box();
    r.rect(b, Color{0x47, 0x72, 0xb3, 40});
    r.rring(b, 0.0f, btheme::pixelsize, Color{0x47, 0x72, 0xb3, 200});
  }
  r.pop_clip();
}

// ================================================================ 变速叠加层
//
// 照 sequencer_retiming_draw.cc 的三条：段高亮（:239-292）、键的菱形（:103-146）、
// 段速百分比文字（:294-374）。
//  - 键尺寸 = 10 * U.pixelsize（retiming_key_size:74-78），中心在条带下沿往上
//    4 + size/2 个像素（retiming_key_center:80-84，那个 4 是原样的区域像素）。
//  - 菱形：填充 = 主题 common.anim.keyframe 0xbfbfbf，描边 = SpaceSequencer.keyborder
//    黑（keyframes_draw.cc:109-137；handle_type 传 0 ⇒ GPU_KEYFRAME_SHAPE_DIAMOND）。
//  - 只有 SEQ_SHOW_RETIMING 打开的条带才画（can_draw_retiming:67 调
//    retiming_show_keys，strip_retiming.cc:124-127）；Blender 的全局叠加开关
//    （SEQ_TIMELINE_SHOW_STRIP_RETIMING + N 面板 Overlays）按需求删掉了，所以这里
//    只看条带自己这一位。
//  - 键落在条带边界外不画（:114 的 BLI_rctf_isect_x），贴边的键整体挪进来
//    （:130-134 的 CLAMP），所以最左/最右那两把永远看得见。
static void retiming_diamond(Render2D &r, float cx, float cy, float size, bool sel) {
  const float h = size * 0.5f;
  const float t = std::max(1.0f, btheme::pixelsize);  // 描边宽度 = 主题里那一圈
  auto shape = [&](float s, Color col) {
    r.tri(cx - s, cy, cx, cy - s, cx + s, cy, col);
    r.tri(cx - s, cy, cx, cy + s, cx + s, cy, col);
  };
  // 选中：fill = tset->strips.retimingsel 0xffbe33，outline = (1,1,1,0) 即完全不画
  // （keyframes_draw.cc:149-154）；未选中：fill = 主题 common.anim.keyframe 0xbfbfbf，
  // outline = tset->keyborder 黑（:156-160）。
  if (!sel) shape(h, Color{0, 0, 0, 255});
  shape(h - t, sel ? Color{0xff, 0xbe, 0x33} : Color{0xbf, 0xbf, 0xbf, 255});
}

static void draw_retiming_overlay(App &a, const Clip &c, const StripGeom &g) {
  if (!c.show_retiming) return;
  Render2D &r = a.r2d;
  const RetKeyBand band = retiming_band(g);
  const float size = band.size, cy = band.cy, left = band.left, right = band.right;
  // 键的绘制位置：把贴边的菱形整颗收进条带内（:130-134）。
  auto raw_px = [&](double x) { return time_x(a, c.timeline_start + x); };
  auto key_px = [&](double x) {
    return std::clamp(raw_px(x), left + size * 0.5f, right - size * 0.5f);
  };
  auto diamond = [&](double x, bool sel) {
    const float px = raw_px(x);
    if (px < left || px > right) return;  // 键在条带范围外（裁短之后）不画
    retiming_diamond(r, key_px(x), cy, size, sel);
  };
  const auto &keys = c.retiming;
  if (keys.size() < 2) {
    // 没有真键 ⇒ 画两端的两把假键（fake_keys_draw:150-169：条带两端各一把，
    // 键值就是恒速曲线的两端）。假键进不了键表，所以永远画成未选中（Blender 同样：
    // 点它一下先落成实键，之后才有选中态）。
    diamond(0.0, false);
    diamond(c.duration, false);
    return;
  }
  for (size_t i = 1; i < keys.size(); i++) {
    // 段高亮：给「以这把键结尾的那一段」画 prev→key 的一块（:252-290）。第一把键
    // （strip_frame_index==0）被 :254 跳过，从第二把起每一段都有；两端按 :265-266
    // 收进条带矩形里。段的两端任一把被选中就亮（:276-283 的 or）。
    const float pa = std::clamp(raw_px(keys[i - 1].strip_frame), left, right);
    const float pb = std::clamp(raw_px(keys[i].strip_frame), left, right);
    if (pb <= pa) continue;  // :261 的越界段不画
    const bool seg_sel = retiming_key_is_selected(a, c.id, i - 1) ||
                         retiming_key_is_selected(a, c.id, i);
    r.rect({pa, cy - size * 0.5f, pb - pa, size},
           seg_sel ? Color{166, 127, 51, 255} : Color{0, 0, 0, 25});
  }
  for (size_t i = 0; i + 1 < keys.size(); i++) {
    // 段速文字：round(speed*100) 后跟一个百分号（speed_label_str_get:309-311）；
    // 最后一把键没有下一段，所以循环天然到 i+1 为止（:350 的 retiming_is_last_key）。
    const double x0 = keys[i].strip_frame, x1 = keys[i + 1].strip_frame;
    wchar_t buf[32];
    swprintf(buf, 32, L"%d%%", int(std::lround(retiming_speed_at(c, 0.5 * (x0 + x1)) * 100.0)));
    const std::wstring label = buf;
    const float key_x = std::max(left, time_x(a, c.timeline_start + x0));
    const float next_x = std::min(right, time_x(a, c.timeline_start + x1));
    const float lw = r.text_width(label, btheme::font_px);
    // 可用宽 = 段宽 − 两颗半键（speed_label_pos_get:329-332）；放不下就不写。
    if (next_x - key_x - size < lw) continue;
    const int th = r.text_height(btheme::font_px);
    // 文字底边在条带下沿往上 5 个像素（:335-339 的 bottom_pad = pixely*5，翻到屏幕 y）。
    r.text(0.5f * (key_x + next_x - lw), g.body.y + g.body.h - 5.0f - float(th), label,
           btheme::font_px, Color{0, 0, 0, 255});
  }
  for (size_t i = 0; i < keys.size(); i++) diamond(keys[i].strip_frame,
                                                  retiming_key_is_selected(a, c.id, i));
}

// ================================================================ 右侧滚动条
//
// view2d.cc:1487-1590 的 view2d_scrollers_draw 竖条那一支：
//  - 轨道 = wcol_scroll.inner；出厂 alpha 是 0，于是走「轨道底色不可见」那一支，
//    inner[3] = 0.25 * alpha_vert（alpha_vert 初始化后恒为 255，view2d.cc:371）= 63。
//  - outline 与 widget_emboss 的 alpha 都被这里清 0，所以轨道和滑块都只是平色圆角块，
//    圆角 = roundness * 条宽 = 0.5 * 条宽（interface_widgets.cc:4464）→ 两头是半圆。
//  - 滑块 = 先把 item 拷进 inner 再画（:4484），按住时（SCROLL_PRESSED，来自
//    V2D_SCROLL_V_ACTIVE）三通道各 +5、到 250 就取 255（:4493-4497）。
//  - SCROLL_ARROWS：时间线的 keepzoom = V2D_KEEPZOOM 不含 V2D_LOCKZOOM_Y，
//    V2D_SCROLL_VERTICAL_HANDLES 也开着，所以只要滑块够长就画（:1581-1586）。两端这两枚
//    圆心距端边 0.5*条宽、半径 0.5*0.6*条宽（:4527-4528 + :805-807），形状和滑块本体一样
//    读 roundness 那一档；颜色按 item 灰度：>70 取黑 α128（:4505-4517）。
static void draw_scrollbar(App &a) {
  Render2D &r = a.r2d;
  const Rect t = scroll_trough(a);
  if (t.w <= 0.0f || t.h <= 0.0f) return;
  const Rect th = scroll_thumb(a);
  // 圆角就是 wcol_scroll.roundness * 条宽（interface_widgets.cc:4464），不从主题里再抄一份
  // 0.5：那样「圆形还是方形」就有两个地方各说一套。
  const float rad = btheme::wcol_scroll.roundness * t.w;
  Color track = btheme::wcol_scroll.inner;
  r.rrect(t, rad, track);

  Color knob = btheme::wcol_scroll.item;
  // 按在滑块身上（而不是两端把手或空白轨道）才提亮 —— 高亮哪一格由那一格按下自己回答。
  if (ScrollBar *sb = gesture_as<ScrollBar>(a); sb && sb->on_thumb()) {
    auto up = [](uint8_t v) { return uint8_t(v >= 250 ? 255 : v + 5); };
    knob = Color{up(knob.r), up(knob.g), up(knob.b), knob.a};
  }
  r.rrect(th, rad, knob);

  if (th.h > 0.6f * btheme::u(1.0f)) {
    const float cr = 0.3f * t.w, cx = t.x + t.w * 0.5f;
    const Color cc{0, 0, 0, 128};
    for (float cy : {th.y + 0.5f * t.w, th.y + th.h - 0.5f * t.w}) {
      r.rrect({cx - cr, cy - cr, 2.0f * cr, 2.0f * cr}, rad, cc);
    }
  }
}

// ================================================================ 属性区（左侧单页）
//
// 自上而下六组，就是需求列的顺序：格式（分辨率 X/Y/%、宽高比 X/Y、帧率）、变换、裁切、
// 帧范围、导出。组的内部行序与分列方式照抄 bl_ui：properties_output.py:109-132 的
// RENDER_PT_format、:143-153 的 RENDER_PT_frame_range、properties_strip.py:1009-1039 的
// STRIP_PT_adjust_transform 与 :116-137 的 STRIP_PT_adjust_crop。
// 本工程没有对应数据的行（filter、use_border、条带名/通道/时长…）整行不出现。

// 帧率行的标签与「是否自定义」= properties_output.py:67-89 的 _draw_framerate_label：
// fps_base 为 1 时取整，否则 round(fps/fps_base, 2)；命中预设集合显示 "X fps"，
// 否则显示 "Custom (X fps)" 并把 fps / fps_base 两行原始数值露出来（:95-99）。
static std::wstring fps_label(const App &a, bool *custom) {
  const double rate = a.project.fmt.fps_base == 1.0
                          ? double(std::lround(a.project.fmt.fps))
                          : std::round(a.project.fmt.fps_value() * 100.0) / 100.0;
  static const double presets[] = {6, 8, 12, 23.98, 24, 25, 29.97, 30, 50, 59.94, 60, 120, 240};
  bool is_custom = true;
  for (double p : presets) {
    if (rate == p) {
      is_custom = false;
      break;
    }
  }
  if (custom) *custom = is_custom;
  wchar_t buf[48];
  if (is_custom) swprintf(buf, 48, L"自定义 (%.4g fps)", rate);
  else swprintf(buf, 48, L"%.4g fps", rate);
  return buf;
}

// 面板里 layout.menu() 画出来的那颗按钮：wcol_menu 的底 + 居中文字 + 右侧下拉三角。
static void menu_btn(App &a, const Rect &area, int id, const std::wstring &text, bool hot) {
  Render2D &r = a.r2d;
  const btheme::Wcol &w = btheme::wcol_menu;
  const float rad = btheme::radius(w.roundness);
  r.rrect(area, rad, hot ? w.inner_sel : w.inner);
  r.rring(area, rad, btheme::pixelsize, w.outline);
  float tw = r.text_width(text, btheme::font_px);
  r.text(area.x + (area.w - tw) * 0.5f, area.y + (area.h - btheme::font_px) * 0.5f, text,
         btheme::font_px, hot ? w.text_sel : w.text);
  float s = 3.5f * btheme::scale, cx = area.x + area.w - 0.5f * btheme::u(1.0f),
       cy = area.y + area.h * 0.5f;
  r.tri(cx - s, cy - s * 0.6f, cx + s, cy - s * 0.6f, cx, cy + s * 0.9f,
        hot ? w.text_sel : w.text);
  ui::Item it;
  it.rect = area;
  it.id = id;
  it.type = ui::Type::Menu;
  it.hot = hot;
  a.blk.add(it);
}

// 一行属性：标签在左列、数值按钮在右列（与 Blender 的 property 行同构）。
// pr.active = false 对应 layout.active/enabled 关掉的那一列：条目置灰且点不动
// （条带被静音或锁定时，变换/裁切/时间三块就是这种状态）。
//
// 装饰点：Blender 给每个可动画属性在右边再挂一个 UI_UNIT_X×Y 的图标按钮
// （Layout::decorator，interface_layout.cc:3103-3153），图标由 button_anim_decorate_update_from_flag
// （interface_anim.cc:168-189）按状态换：没曲线 = ICON_DECORATE、有曲线 = ICON_DECORATE_ANIMATE、
// 当前帧有键 = ICON_DECORATE_KEYFRAME。这三枚图标是单色菱形（UI_icons.hh:317-324 全是 DEF_ICON，
// 不是 DEF_ICON_COLOR），5.x 的图标颜色就取 wcol 的 text，所以这里画的是一枚灰菱形：
// 空心 = 这一帧没有键（细线 = 没曲线，粗线 = 挂了曲线），实心 = 当前帧上有键。
// 点它 = button_anim_decorate_cb：有键就删、没键就按当前值插一个。
static void deco_dot(App &a, const Rect &box, int field, int anim) {
  Render2D &r = a.r2d;
  const float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
  const float d = 3.5f * btheme::scale;  // 菱形半对角线（图标在 unit 方格正中）
  const bool hot = box.contains(float(a.mouse_x), float(a.mouse_y));
  const Color col = hot ? btheme::wcol_regular.text_sel : btheme::wcol_regular.text;
  if (anim >= 2) {  // 实心
    r.tri(cx - d, cy, cx, cy - d, cx, cy + d, col);
    r.tri(cx + d, cy, cx, cy - d, cx, cy + d, col);
  } else {  // 空心：没曲线时线宽 1px，挂了曲线 2px（DECORATE 与 DECORATE_ANIMATE 的差别）
    const float w = (anim == 1 ? 2.0f : 1.0f) * btheme::pixelsize;
    r.line_seg(cx - d, cy, cx, cy - d, w, col);
    r.line_seg(cx, cy - d, cx + d, cy, w, col);
    r.line_seg(cx + d, cy, cx, cy + d, w, col);
    r.line_seg(cx, cy + d, cx - d, cy, w, col);
  }
  ui::Item it;
  it.rect = box;
  it.id = kHitDeco | field;
  it.type = ui::Type::Toggle;
  it.hot = hot;
  a.blk.add(it);
}

// XY 成对锁按钮：位置X/Y 共用一把，缩放X/Y 共用一把，画在关键帧装饰点的左边。
// 图标是一把小锁：锁着 = 锁环合在锁体上；解锁 = 环提起并歪到一侧（一眼看出这对断开）。
// 状态存在 App 里、是界面偏好而不是条带数据 ⇒ 两个片段共用同一套锁，
// 并且跟着「保存启动默认」走（settings.txt 的 lock 行）。
static void lock_box(App &a, const Rect &box, int field, bool locked) {
  Render2D &r = a.r2d;
  const float s = btheme::scale;
  const float cx = box.x + box.w * 0.5f, cy = box.y + box.h * 0.5f;
  const bool hot = box.contains(float(a.mouse_x), float(a.mouse_y));
  const Color col = hot ? btheme::wcol_regular.text_sel : btheme::wcol_regular.text;
  const float bw = 9.0f * s, bh = 6.0f * s;   // 锁体
  const float by = cy + 2.0f * s;             // 锁体上沿（整体往下沉一点，留出锁环）
  r.rrect({cx - bw * 0.5f, by, bw, bh}, 2.0f * s, col);
  // 环要比锁体窄一圈，两条腿之间才留得出一个能看见的洞；1x 下洞 ≈3.7×4 像素。
  const float lw = 6.5f * s, lh = 5.5f * s;   // 锁环
  const float lx = locked ? cx : cx + 2.5f * s;  // 解锁 = 歪出去
  const float w = 1.4f * btheme::pixelsize;
  r.line_seg(lx - lw * 0.5f, by + 0.5f * s, lx - lw * 0.5f, by - lh, w, col);
  r.line_seg(lx - lw * 0.5f, by - lh, lx + lw * 0.5f, by - lh, w, col);
  if (locked) r.line_seg(lx + lw * 0.5f, by - lh, lx + lw * 0.5f, by + 0.5f * s, w, col);
  ui::Item it;
  it.rect = box;
  it.id = kHitLock | field;
  it.type = ui::Type::Toggle;
  it.hot = hot;
  a.blk.add(it);
}

// row_active = false 对应 Blender 那一列被 layout.active 关掉：这一行置灰、点不动，
// 但控件本身还在（码率行就靠它，见输出那一组）。
// 一格数值按钮：prop_row 和颜色行都用它，所以「编辑态显示键入的串、置灰压一层」这套
// 规矩只写一遍。
static void num_cell(App &a, const Rect &v, int id, Clip *c, bool act, bool slider = false,
                     int anim = 0) {
  const PropSpec &s = prop_spec(id);
  const bool editing = act && a.edit_id == id;
  ui::but_num(a.r2d, a.blk, act ? id : 0, v, L"", shown_text(a, id, c), s.soft_min, s.soft_max,
              s.step, prop_value(a, id, c), s.precision, s.is_float, slider, editing, anim,
              editing ? &a.edit_ts : nullptr);
  if (!act) a.r2d.rrect(v, btheme::radius(0.2f), inactive_wash());
}

static void prop_row(App &a, PropRows &pr, const std::wstring &label, int id, bool slider,
                     bool row_active = true) {
  Clip *c = props_clip(a);
  const bool act = pr.active && row_active;
  Render2D &r = a.r2d;
  Rect v = pr.split_row(r, label, act ? btheme::panel_text : dim_text());
  // 变换那五行（位置XY/缩放XY/旋转）可动画，速度行的「曲线」是变速键 —— 都让出右边一格给装饰点。
  const bool deco = act && ((id >= kFOfsX && id <= kFRotation) || id == kFSpeed);
  // 位置/缩放这四行再多让一格给成对锁。
  const bool lock =
      act && (id == kFOfsX || id == kFOfsY || id == kFScaleX || id == kFScaleY);
  int anim = 0;
  Rect db, lb;
  if (deco) {
    const int st = key_field_state(a, id);
    anim = (st & kKeyAtFrame) ? 2 : (st & kKeyAnimated) ? 1 : 0;
    const float unit = btheme::u(1.0f);
    db = {v.x + v.w - unit, v.y, unit, v.h};
    v.w -= unit;
  }
  if (lock) {
    const float unit = btheme::u(1.0f);
    lb = {v.x + v.w - unit, v.y, unit, v.h};
    v.w -= unit;
  }
  num_cell(a, v, id, c, act, slider, anim);
  if (lock) lock_box(a, lb, id, id <= kFOfsY ? a.lock_pos : a.lock_scale);
  if (deco) deco_dot(a, db, id, anim);
}

static void mirror_row(App &a, const PropRows &pr, const Rect &area, const std::wstring &text,
                       int id);

// 内容那一格固定五行（裁决：加高度五行、移到组底部，改字时手不用抬到上面）。
// 行数不再跟着内容长：格子是有数的界，超过五行的部分裁在框内（but_text 那道 push_clip），
// 上下键和光标都能走到，只是看不见的那截。
constexpr int kContentRows = 5;

// 内容行：占 kContentRows 行的文本框，一行一格高。
static void content_row(App &a, PropRows &pr, Clip *c) {
  Render2D &r = a.r2d;
  const float unit = btheme::u(1.0f);
  const bool act = pr.active;
  Rect rr = pr.row_n(kContentRows);
  Rect v = pr.split_in(r, rr, L"内容", act ? btheme::panel_text : dim_text());
  const bool editing = act && a.edit_id == kFTextContent;
  ui::but_text(r, a.blk, act ? kFTextContent : 0, v, shown_text(a, kFTextContent, c), editing,
               editing ? &a.edit_ts : nullptr, unit);
  if (!act) r.rrect(v, btheme::radius(0.2f), inactive_wash());
}

// 一层的色块：整块就是这一层的颜色，点下去开系统的选色框。alpha 为 0 时这层本来就不出
// 画面（排版器读 alpha 判关），色块仍然按 RGB 显示 —— 它是这一层现在存的颜色，不是预览。
static void color_swatch(App &a, const Rect &area, int layer, Clip *c) {
  Render2D &r = a.r2d;
  const float *col = text_layer(text_target(a, c), layer);
  const Color sw = Color{uint8_t(col[0] * 255.0f + 0.5f), uint8_t(col[1] * 255.0f + 0.5f),
                         uint8_t(col[2] * 255.0f + 0.5f), 0xff};
  r.rrect(area, btheme::radius(btheme::wcol_text.roundness), sw);
  r.rring(area, btheme::radius(btheme::wcol_text.roundness), btheme::pixelsize,
           btheme::wcol_text.outline);
  ui::Item it;
  it.rect = area;
  it.id = kHitCmd | (kCmdColorGlyph + layer);
  it.type = ui::Type::Push;
  it.hot = area.contains(float(a.mouse_x), float(a.mouse_y));
  a.blk.add(it);
}

// 颜色行：色块 + 不透明度一格。RGB 那三格按裁决删了：颜色本身归色块（点它开系统选色框，
// 一套色轮就是给颜色立界，我们不自己画），这一格只管「这一层开多大、透不透」—— 排版器读
// 的就是 alpha。剩下这一格占满整列，左右拖好抓。id 还是那十二格的下标（app.h 的 Field 表：
// 每层四格连排，alpha 是第四个），读写走 num_cell 那一套，不另开路。
static void color_row(App &a, PropRows &pr, const std::wstring &label, int first_id,
                      Clip *c) {
  Render2D &r = a.r2d;
  const bool act = pr.active;
  Rect v = pr.split_row(r, label, act ? btheme::panel_text : dim_text());
  const float unit = btheme::u(1.0f);
  const float m = 0.25f * unit;
  const float sw = v.h;
  color_swatch(a, {v.x, v.y, sw, v.h}, text_color_row(first_id), c);
  v.x += sw + m;
  v.w -= sw + m;
  if (v.w <= 0) return;  // 面板拖到极窄：色块已经画下，那一格这一趟不登记
  num_cell(a, v, first_id + 3, c, act);
}

// 粗体 / 斜体：一行两颗开关（和变换那组的镜像 X/Y 同一个形状），写在 text_target 的 flag 位上。
static void style_row(App &a, PropRows &pr, Clip *c) {
  Render2D &r = a.r2d;
  const bool act = pr.active;
  TextVars &t = text_target(a, c);
  Rect v = pr.split_row(r, L"字形", act ? btheme::panel_text : dim_text());
  const float m = 0.25f * btheme::u(1.0f);
  const float bw = (v.w - m) / 2.0f;
  ui::but_toggle(r, a.blk, act ? kFTextBold : 0, {v.x, v.y, bw, v.h}, L"", t.bold, L"粗体");
  ui::but_toggle(r, a.blk, act ? kFTextItalic : 0, {v.x + bw + m, v.y, bw, v.h}, L"", t.italic,
                 L"斜体");
  if (!act) {
    r.rrect({v.x, v.y, bw, v.h}, btheme::radius(0.2f), inactive_wash());
    r.rrect({v.x + bw + m, v.y, bw, v.h}, btheme::radius(0.2f), inactive_wash());
  }
}

// 面板自己的滚动条：形状读同一档 wcol_scroll（时间线那条也是它），位置在面板内缘，
// 见 ops.cpp 的 props_scroll_trough。内容放得下时滑块是空矩形 = 一条都不画。
static void draw_props_scrollbar(App &a) {
  Render2D &r = a.r2d;
  const Rect t = props_scroll_trough(a);
  const Rect th = props_scroll_thumb(a);
  if (th.h <= 0.0f || t.w <= 0.0f) return;
  const float rad = btheme::wcol_scroll.roundness * t.w;
  r.rrect(t, rad, btheme::wcol_scroll.inner);
  Color knob = btheme::wcol_scroll.item;
  // 按在滑块身上才提亮，和时间线那条同一个判据（读正在进行的手势，不看地方）。
  if (gesture_as<PropsScroll>(a)) {
    auto up = [](uint8_t v) { return uint8_t(v >= 250 ? 255 : v + 5); };
    knob = Color{up(knob.r), up(knob.g), up(knob.b), knob.a};
  }
  r.rrect(th, rad, knob);
}

// 面板规格表：标题、行数、列间隙个数（每个间隙 0.25u）。这是「这一组有多高」唯一的出处 ——
// 画这一趟按它算高度，画之前量整页总高也按它算。以前总高是画完才从 y 推出来的，于是点折叠
// 的那一帧滚动条已经出现、行宽却还按上一趟的数据不让位，得滚一下才对齐。
struct PanelSpec {
  const wchar_t *title;
  int rows;
  int extra_rows;  // 帧率不在预设里时追加的行数（帧率、基数）
  float gaps;
};

static const PanelSpec kPanelSpecs[kPanelCount] = {
    {L"格式", 3 + 2 + 1, 2, 2.0f},
    {L"变换", 2 + 2 + 1 + 2, 0, 3.0f},
    {L"裁切", 4, 0, 0.0f},
    {L"速度", 1, 0, 0.0f},
    {L"帧范围", 3, 0, 0.0f},
    {L"输出", 4, 0, 0.0f},
    // 文本：字体 + 字形 + 字号 + 行距 + 三层颜色 + 轮廓宽 + 边距 + 圆度 + 内容。内容是
    // kContentRows 行的框，排在最下面（改字时手不用抬到上面），所以行数里单独把它列出来。
    // 组与组之间三个间隔：字号前、颜色前、内容前。
    {L"文本", 2 + 2 + 6 + kContentRows, 0, 3.0f},
};

// 这一组在这一趟出现吗。判据是面板此刻演哪一种（a.panel_kind），不是选中条带的类型：
// 两颗种类按钮放开以后，「演文本但选中是别的带」是合法状态，这时格式/输出/帧范围那些
// 非文本的控件必须不出现，否则用户照原样点得到导出。画的时候和画之前量总高的时候都问
// 这同一个函数 —— 各判一次就是两个写者，滚动条会先出现、行宽慢一帧才让位。
static bool panel_shown(const App &a, int idx) {
  const bool text = a.panel_kind == kStripText;
  switch (idx) {
    case kPCrop:
    case kPSpeed: return !text;
    case kPText: return text;
    case kPFormat:
    case kPOutput:
    case kPRange: return !text;
    default: return true;
  }
}

static int panel_rows(const App &a, int idx) {
  const PanelSpec &p = kPanelSpecs[idx];
  if (p.extra_rows == 0) return p.rows;
  bool custom = false;
  fps_label(a, &custom);
  return p.rows + (custom ? p.extra_rows : 0);
}

static float panel_h(const App &a, int idx) {
  const float pad = 0.25f * btheme::u(1.0f);
  const float header_h = btheme::u(1.25f);
  if (!a.panel_open[idx]) return header_h;
  return header_h + panel_rows(a, idx) * btheme::u(1.0f) + kPanelSpecs[idx].gaps * pad +
         pad * 2.0f;
}

// 整页内容总高 = 顶部那行种类按钮 + 每一组的高加上组与组之间那格 pad；末组下面多出来的
// 那格正好补第一组上面的那格，所以从 a.props.y + pad 起算的整段就是滚到底的画面。
static float props_content_h(const App &a) {
  const float pad = 0.25f * btheme::u(1.0f);
  float h = btheme::u(1.0f) + pad;  // 种类按钮那一行
  for (int i = 0; i < kPanelCount; i++)
    if (panel_shown(a, i)) h += panel_h(a, i) + pad;
  return h;
}

// 一个面板：按规格表算出高度、画底与标题，再返回内容区。
// 折叠时只留标题那么高，内容不画也不登记命中（点标题展开，对应 Blender 点面板头）。
// 内容比面板 region 高不再「放不下就退回只画标题」：整页由 draw_props 上下滚，
// 画和命中都只认 region 里那一段（裁剪框），滚过去才看得见。
static PropRows begin_panel(App &a, int idx, float &y) {
  Render2D &r = a.r2d;
  const float pad = 0.25f * btheme::u(1.0f);
  const float header_h = btheme::u(1.25f);
  const bool open = a.panel_open[idx];
  const float h = panel_h(a, idx);
  const float g = props_gutter(a);
  Rect area{a.props.x + pad + (a.props_side ? g : 0.0f), y, a.props.w - pad * 2.0f - g, h};
  ui::panel_box(r, area, open);
  Rect header{area.x, area.y, area.w, header_h};
  ui::panel_header(r, header, kPanelSpecs[idx].title, open, a.hover == (kHitPanel | idx));
  ui::Item it;
  it.rect = header;
  it.id = kHitPanel | idx;
  it.type = ui::Type::Push;
  a.blk.add(it);
  y += h + pad;
  PropRows pr({area.x, area.y + header_h, area.w, open ? h - header_h : 0.0f});
  pr.rows = open;
  return pr;
}

// 面板顶部那两颗种类按钮。亮的那颗读 a.panel_kind（面板演哪一种就这一个出处），按下去无条件
// 换它 —— 没有条件、没有拒绝：时间线里还没有这一种的带时，文本行落在 a.text_def 上照样改。
static void kind_buttons(App &a, float &y) {
  Render2D &r = a.r2d;
  const float pad = 0.25f * btheme::u(1.0f);
  const float unit = btheme::u(1.0f);
  const float g = props_gutter(a);
  const Rect row{a.props.x + pad + (a.props_side ? g : 0.0f), y, a.props.w - pad * 2.0f - g, unit};
  const float m = 0.25f * unit;
  const float bw = (row.w - m) * 0.5f;
  const bool text = a.panel_kind == kStripText;
  ui::but_toggle(r, a.blk, kHitCmd | kCmdPickVideoStrip, {row.x, row.y, bw, row.h}, L"", !text,
                 L"视频");
  ui::but_toggle(r, a.blk, kHitCmd | kCmdPickTextStrip, {row.x + bw + m, row.y, bw, row.h}, L"",
                 text, L"文本");
  y += unit + pad;
}

static void draw_props(App &a) {
  Render2D &r = a.r2d;
  if (a.props.w <= 0) return;
  r.rect(a.props, btheme::window_back);
  const float pad = 0.25f * btheme::u(1.0f);
  const float unit = btheme::u(1.0f);
  // 总高在这一趟画之前就算出来，后面三处（滚动的界、行宽的让位、滑块的长短）读同一个
  // 当帧的数。
  a.props_content_h = props_content_h(a);
  // 滚动的界收在这里，不收在按键那一头：内容放不下才滚得动，
  // 放得下就收回 0（滚轮和拖动写进去的那个数被原样抵掉，面板一动不动）。
  a.props_scroll =
      std::clamp(a.props_scroll, 0.0f, std::max(0.0f, a.props_content_h - a.props.h));
  float y = a.props.y + pad - a.props_scroll;
  // 画与命中同一道裁剪：region 外面那一截不画，登记在外的控件也不收事件
  // （ui::Block::add 读这道框）。
  r.push_clip(a.props);
  a.blk.set_clip(a.props);
  Clip *c = props_clip(a);
  const bool has_strip = c != nullptr;
  // layout.active = not strip.mute（properties_strip.py:1013）。没有活动条带时 Blender
  // 的这几个面板整组不出现（StripButtonsPanel.poll 返回 False）；单页不能空出一大截，
  // 改成和静音一样置灰：条目照常显示，prop_row 把 id 登记成 0 所以点下去不动作。
  PropRows::Active strip_active{has_strip && !c->muted};

  kind_buttons(a, y);

  // 演文本时这一组不出现：面板递到文本行照样一个字不改，但点得到就不是「移除」了。
  if (panel_shown(a, kPFormat)) {
    // ---- 格式（RENDER_PT_format，properties_output.py:109-132）
    // 三个 column(align=True)：分辨率 X/Y/% → 宽高比 X/Y → Frame Rate 一列。
    // use_border / use_crop_to_border 没有渲染边框概念，整组不列。
    bool custom_fps = false;
    std::wstring fps_text = fps_label(a, &custom_fps);
    PropRows pr = begin_panel(a, kPFormat, y);
    if (pr.rows) {
      prop_row(a, pr, L"分辨率 X", kFResX, false);
      prop_row(a, pr, L"Y", kFResY, false);
      prop_row(a, pr, L"%", kFResPct, true);
      pr.gap();
      prop_row(a, pr, L"宽高比 X", kFAspX, false);
      prop_row(a, pr, L"Y", kFAspY, false);
      pr.gap();
      {
        // 帧率：heading="Frame Rate" 的一列，内容是 RENDER_MT_framerate_presets 菜单按钮。
        Rect v = pr.split_row(r, L"帧率");
        menu_btn(a, v, kHitMenu | kMFramerate, fps_text, a.hover == (kHitMenu | kMFramerate));
        if (custom_fps) {
          prop_row(a, pr, L"帧率", kFFps, false);
          prop_row(a, pr, L"基数", kFFpsBase, false);
        }
      }
    }
  }
  // ---- 变换（STRIP_PT_adjust_transform，properties_strip.py:1009-1039）
  {
    // 原点（transform.origin）不在需求的变换清单里：变换 = 位置XY、缩放XY、旋转、镜像XY。
    // 数据仍然带着（默认 0.5/0.5 = 画面中心，等于没有偏移），渲染层按它算，只是不给控件。
    PropRows pr = begin_panel(a, kPXform, y);
    pr.active = strip_active;
    if (pr.rows) {
      prop_row(a, pr, L"位置 X", kFOfsX, false);
      prop_row(a, pr, L"Y", kFOfsY, false);
      pr.gap();
      prop_row(a, pr, L"缩放 X", kFScaleX, false);
      prop_row(a, pr, L"Y", kFScaleY, false);
      pr.gap();
      prop_row(a, pr, L"旋转", kFRotation, false);
      pr.gap();
      // column(heading="Mirror")：标签列只出现一次，右侧是两枚 X/Y 开关。
      {
        Rect first = pr.row();
        Rect second = pr.row();
        float lx = PropRows::split_x(first);
        const float mw = r.text_width(L"镜像", btheme::font_px);
        if (mw > g_label_w) g_label_w = mw;  // 这条也归标签列管
        r.text(lx - mw - 0.25f * unit,
               first.y + (first.h + second.h - btheme::font_px) * 0.5f, L"镜像",
               btheme::font_px, pr.active ? btheme::panel_text : dim_text());
        float wx = lx + 0.25f * unit;
        float ww = first.x + first.w - wx;
        mirror_row(a, pr, {wx, first.y, ww, first.h}, L"X", kFFlipX);
        mirror_row(a, pr, {wx, second.y, ww, second.h}, L"Y", kFFlipY);
      }
    }
  }
  // ---- 文本（本项目的文本条带样式：字体字形 / 字号行距 / 三层颜色与它们的参数 / 内容）
  // 面板演文本时才出现（panel_shown），位置在变换下面（裁决）。组内内容排在最下面：改字时
  // 鼠标停在左侧那一截，不用抬到上面去。每一行写的都是 text_target 的那份 TextVars ——
  // 选中不是文本带时落在 a.text_def，是文本带时落在这条带自己身上，两条路只有一个出处。
  if (panel_shown(a, kPText)) {
    PropRows pr = begin_panel(a, kPText, y);
    pr.active = strip_active;
    if (pr.rows) {
      {
        // 字体 = 系统枚举出来的家族（txt::families），当前值就是按钮文字，
        // 下拉里同名的那一行带勾（build_menus 读的是同一个 text_target）。
        Rect v = pr.split_row(r, L"字体");
        menu_btn(a, v, kHitMenu | kMFont, text_target(a, c).family,
                 a.hover == (kHitMenu | kMFont));
      }
      style_row(a, pr, c);
      pr.gap();
      prop_row(a, pr, L"字号", kFTextSize, true);
      prop_row(a, pr, L"行距", kFTextLineSpacing, true);
      pr.gap();
      color_row(a, pr, L"字色", kFGlyphR, c);
      color_row(a, pr, L"轮廓色", kFOutlineR, c);
      // 相对量按原值给：轮廓按行高、边距按画布宽、圆度按盒高的一半（算式都在 text.cpp）。
      prop_row(a, pr, L"轮廓宽", kFTextOutlineWidth, true);
      color_row(a, pr, L"方框色", kFBoxR, c);
      prop_row(a, pr, L"边距", kFTextBoxMargin, true);
      prop_row(a, pr, L"圆度", kFTextBoxRoundness, true);
      pr.gap();
      content_row(a, pr, c);
    }
  }
  // ---- 裁切（STRIP_PT_adjust_crop，properties_strip.py:116-137；DEFAULT_CLOSED）
  // 行序照抄：crop.min_x / max_x / max_y / min_y = 左 右 上 下。
  if (panel_shown(a, kPCrop)) {
    PropRows pr = begin_panel(a, kPCrop, y);
    pr.active = strip_active;
    if (pr.rows) {
      prop_row(a, pr, L"左", kFCropMinX, false);
      prop_row(a, pr, L"右", kFCropMaxX, false);
      prop_row(a, pr, L"顶", kFCropMaxY, false);
      prop_row(a, pr, L"底", kFCropMinY, false);
    }
  }
  // ---- 速度（对应 Blender 的 sequencer.retiming_segment_speed_set，R 键那个算子）
  // 一条 align 列：只有「速度」一格，百分数，100% = 原速。改它吃掉的源内容不变，
  // 条带在时间线上变长/变短（strip_retiming.cc:239 new_duration = 内容 / speed）。
  if (panel_shown(a, kPSpeed)) {
    PropRows pr = begin_panel(a, kPSpeed, y);
    pr.active = strip_active;
    if (pr.rows) {
      prop_row(a, pr, L"速度", kFSpeed, false);
    }
  }
  // ---- 帧范围（RENDER_PT_frame_range，properties_output.py:143-153）
  // 一个 align 列：Frame Start / End / Step，三行逐字照 Blender 的列序。
  // 开始/结束同时是播放范围（scene.cc:2607-2633 的 BKE_scene_get_playback_range = sfra..efra），
  // 所以 Ctrl+Home / Ctrl+End 跳的就是这两格；步长不进播放，只进导出（见 types.h 的 frame_step）。
  if (panel_shown(a, kPRange)) {
    PropRows pr = begin_panel(a, kPRange, y);
    if (pr.rows) {
      prop_row(a, pr, L"开始帧", kFFrameStart, false);
      prop_row(a, pr, L"结束帧", kFFrameEnd, false);
      prop_row(a, pr, L"步长", kFFrameStep, false);
    }
  }
  // ---- 输出（RENDER_PT_encoding_video/audio 的 FFmpeg 分支）
  // 四行 = properties_output.py:592 constant_rate_factor、:616-620 video_bitrate、
  // :604 ffmpeg_preset、:658 audio_codec。
  // 容器和文件名不在这一组里：容器只有一个（导出的就是 MP4，见 export.cpp），文件名的落点是
  // 导出对话框那颗「保存」，都无须在面板占一行。导出这颗按钮按要求摆在顶栏，不在这里。
  if (panel_shown(a, kPOutput)) {
    PropRows pr = begin_panel(a, kPOutput, y);
    if (pr.rows) {
      {
        // ffmpeg.constant_rate_factor：RNA 的枚举项按档显示，数值就是 CRF。
        Rect v = pr.split_row(r, L"输出质量");
        menu_btn(a, v, kHitMenu | kMQuality, crf_label(a.project.fmt.crf),
                 a.hover == (kHitMenu | kMQuality));
      }
      // video_bitrate 那一行只在 CRF = NONE（这里的「恒定码率」）时才是活的：
      // properties_output.py:616 的判据就是 `constant_rate_factor == 'NONE'`，
      // 因为 movie_write.cc:751 起两条路互斥 —— 发码率就不发 CRF。填 0 = 什么都不发，
      // 编码器按自己的默认码率控制走（Blender 没改过 video_bitrate 时正是这个状态）。
      prop_row(a, pr, L"码率", kFBitrate, false, a.project.fmt.crf < 0);
      {
        // ffmpeg.ffmpeg_preset：三项的名字和导出的 -preset 读同一张表（types.h speed_of）。
        Rect v = pr.split_row(r, L"编码速度");
        menu_btn(a, v, kHitMenu | kMSpeed, a.project.fmt.speed_preset().label,
                 a.hover == (kHitMenu | kMSpeed));
      }
      {
        // ffmpeg.audio_codec：AAC 与 NONE（RNA 第一项 "No Audio"）两档，当前值就是按钮文字。
        Rect v = pr.split_row(r, L"音频");
        menu_btn(a, v, kHitMenu | kMAudio, a.project.fmt.audio_name(),
                 a.hover == (kHitMenu | kMAudio));
      }
    }
  }
  a.blk.clear_clip();
  r.pop_clip();
  draw_props_scrollbar(a);
}

// 变换面板的 Mirror 行：toggle=True 的布尔按钮，文字就是 X / Y。
static void mirror_row(App &a, const PropRows &pr, const Rect &area, const std::wstring &text,
                       int id) {
  Render2D &r = a.r2d;
  Clip *c = props_clip(a);
  bool on = c != nullptr && (id == kFFlipX ? c->flip_x : c->flip_y);
  ui::but_toggle(r, a.blk, pr.active ? id : 0, area, L"", on, text.c_str());
  if (!pr.active) r.rrect(area, btheme::radius(0.2f), inactive_wash());
}

// ================================================================ 状态栏
//
// space_sequencer 的 FOOTER（bl_ui/space_sequencer.py 的 SEQUENCER_HT_footer）：
// 底色同顶栏 0x181818b3，左侧是算子提示信息（wm 的 operator 报告），右侧是任务进度。
static void draw_status(App &a) {
  Render2D &r = a.r2d;
  if (a.status.h <= 0) return;
  r.rect(a.status, btheme::topbar);
  const float unit = btheme::u(1.0f);
  r.text(a.status.x + 0.25f * unit, a.status.y + (a.status.h - btheme::font_px) * 0.5f,
         a.status_text, btheme::font_px, btheme::wcol_menu.text);
  if (a.exporter.running()) {
    // 进度条：Blender 的状态条右侧那一条（workspacer 的 job 进度）。宽度按 8 个控件单位。
    float pw = unit * 8.0f;
    float bar_h = std::max(2.0f * btheme::pixelsize, unit * 0.2f);
    Rect slot{a.status.x + a.status.w - pw - 0.25f * unit,
              a.status.y + (a.status.h - bar_h) * 0.5f, pw, bar_h};
    r.rect(slot, btheme::wcol_num.inner);
    double p = std::clamp(a.exporter.progress(), 0.0, 1.0);
    r.rect({slot.x, slot.y, slot.w * float(p), slot.h}, btheme::wcol_num.inner_sel);
    wchar_t buf[32];
    swprintf(buf, 32, L"%.0f%%", p * 100.0);
    float tw = r.text_width(buf, btheme::font_px);
    r.text(slot.x - tw - 0.25f * unit, a.status.y + (a.status.h - btheme::font_px) * 0.5f, buf,
           btheme::font_px, btheme::wcol_menu.text_sel);
  }
}

// ================================================================ 一帧
//
// 顺序 = Blender 的 region 绘制顺序（WINDOW 底 -> 各 region 的 draw -> 最后 popup）：
// 弹出菜单在 ui::Block 里最后登记，命中测试从后往前，所以上层菜单永远先吃事件。
void draw(App &a) {
  // 绘制 = 对象被「用到」，先让它自己把视图属性收进策略、按需摆位（Blender 的 view2d_get
  // 也是这个位置：view2d.cc:388，绘制和取坐标都得先过这一关）。按键只写属性。
  timeline_view_react(a);
  Render2D &r = a.r2d;
  r.begin_frame();
  r.rect({0, 0, float(r.width()), float(r.height())}, btheme::window_back);
  a.blk.clear();

  draw_topbar(a);
  draw_viewport(a);
  // PREVIEW 与 WINDOW 是同一个 area 里的两块 region，之间只有一条区域分界
  // （area.cc:3916-3927 各自清自己的底色，线宽 = U.pixelsize）。
  // 这一条不能照 editor_border 原色画：线上下两块底同为 0x1d1d1d，0x161616 比背景还暗
  // 一档，屏幕上就是「没有线」，而那 3px 命中带必须能靠指针找到。取亮的那一档 = 手柄带内侧边的同一组数。
  const bool split_hot = a.split_div_hot || gesture_as<DividerH>(a);
  r.rect(a.split, btheme::shade(btheme::window_back, split_hot ? 0x50 : 0x3a));
  // 时间线 area 的底色 = 它自己那一笔 frame_buffer_clear(TH_BACK)
  // （sequencer_timeline_draw.cc:2044），不透明 0x181818。这一笔必须在标尺带/网格之前：
  // Blender 先清底再画网格，网格线才会留在条带区里；放在 draw_lanes 里就变成网格画完再
  // 被整块盖掉，条带之间看不见格线。
  r.rect(a.tl_area, btheme::seq_back);
  draw_scrub(a);
  draw_chan(a);
  draw_lanes(a);
  draw_scrollbar(a);
  draw_props(a);
  // 属性面板与主区之间的竖分界线：和 a.split 那条横线同一个 editor_border，悬停/拖动时
  // 提亮一格（Blender 的 region 边界只在拖动时把光标换成 X_MOVE，颜色不变，这里加一格
  // 亮度是为了让「这条边能抓」在屏幕上看得见）。
  const bool hot = a.props_div_hot || gesture_as<DividerV>(a);
  if (a.props.w > 0.0f) {
    const float x = props_edge(a);
    r.rect({x, a.props.y, btheme::pixelsize, a.props.h},
           hot ? btheme::shade(btheme::editor_border, 0x30) : btheme::editor_border);
  } else {
    // 面板收起：Blender 那里隐藏的区域在窗口边缘留一条可以抓的带（region_scale 在区
    // 已经 RGN_FLAG_HIDDEN 时照样接着算尺寸，拖过 collapse_size 就把它放回来，
    // screen_ops.cc:3524-3525）。这条带必须看得见：窗口底和面板底同为 0x1d1d1d，
    // 照原色画就等于只有一条比背景还暗的 1px 线，用户找不到它，
    // 所以底色提亮一档、内侧边再亮一档，悬停/拖动时各再加一格。
    const float band = props_band();
    const float x0 = a.props_side ? float(r.width()) - band : 0.0f;
    const Color fill = btheme::shade(btheme::window_back, hot ? 0x28 : 0x12);
    r.rect({x0, a.props.y, band, a.props.h}, fill);
    const float x = a.props_side ? x0 : x0 + band;
    r.rect({x, a.props.y, btheme::pixelsize, a.props.h},
           btheme::shade(btheme::window_back, hot ? 0x50 : 0x3a));
  }
  draw_status(a);
  a.pop.draw(r);

  // 自动化验证：FLIP_DISCARD 下后备缓冲只有在本帧 Present 之前内容确定，故在此读回。
  if (!a.shot_path.empty()) {
    bool ready = a.preview.has_frame();
    double now = now_seconds();
    bool expired = now > a.shot_deadline;
    bool held = now < a.shot_hold;
    if ((ready && !held) || expired) {
      std::wstring out = std::move(a.shot_path);
      a.shot_path.clear();
      wchar_t buf[192];
      swprintf(buf, 192, L"shot -> %ls", out.c_str());
      trace(buf);
      bool ok = r.capture_png(out.c_str());
      swprintf(buf, 192, L"shot done ok=%d", int(ok));
      trace(buf);
      if (a.shot_quit_after) PostMessageW(a.hwnd, WM_CLOSE, 0, 0);
      return;
    }
  }
  r.end_frame(a.playing);
  // FLIP_DISCARD 后备缓冲在 Present 后内容不确定，所以每次重绘仍画全部区域；
  // 脏位的意义是「没变化就完全不重绘」，以及只有画面真换了才重传纹理。
  a.dirty = 0;
}


}  // namespace app
