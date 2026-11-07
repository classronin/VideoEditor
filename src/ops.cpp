// 应用外壳：区域几何、数据操作、RNA 属性表、命令与菜单。
// 每个数字/名字都标了它在参考树里的出处（Blender 5.4）。
#include "app.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ffmpegget.h"
#include "gesture.h"
#include "settings.h"
#include "text.h"

namespace app {

App g_app;

// ---------------------------------------------------------------- 基础设施

FILE *trace_fp = nullptr;

double now_seconds() {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) / f.QuadPart;
}

void trace(const std::wstring &s) {
  if (!trace_fp) return;
  fwprintf(trace_fp, L"%.3f %ls\n", now_seconds(), s.c_str());
  fflush(trace_fp);
}

// 动画状态一行日志。验证「拖条带=关键帧一起走」「面板改值有没有写进曲线」这类问题，
// 读这一行比量截图像素准（曲线在 VSE 时间线上本来就没有可见标记）。
static void trace_keys(App &a, const Clip &c, const wchar_t *tag) {
  if (!trace_fp) return;
  wchar_t b[64];
  // tag 必须是 %ls：MinGW 的 swprintf 把 %s 当窄字符串，宽字符标签只会剩第一个字符。
  swprintf(b, 64, L"anim %ls id=%d t=%.3f f=%lld", tag, c.id, c.timeline_start,
           (long long)a.cur_frame());
  std::wstring s(b);
  for (int i = 0; i < kAnimCount; i++) {
    const FCurve &f = c.anim[i];
    if (f.empty())
      swprintf(b, 64, L" c%d[n=0]", i);
    else
      swprintf(b, 64, L" c%d[n=%d x=%.2f..%.2f v=%.3f]", i, int(f.keys.size()),
               f.keys.front().x, f.keys.back().x, double(f.eval(double(a.cur_frame()))));
    s += b;
  }
  // DNA 里的常量也要打一行：曲线清空后渲染与面板读的就是这些数。
  swprintf(b, 64, L" dna=(%.3f,%.3f,%.3f,%.3f,%.3f)", c.ofs_x, c.ofs_y, c.scale_x, c.scale_y,
           c.rotation);
  s += b;
  trace(s);
}

// 变速曲线状态一行日志，和 trace_keys 同一用途：曲线在时间线上没有可见标记，
// 读这一行比量像素准。
void trace_retime(App &a, const Clip &c, const wchar_t *tag) {
  if (!trace_fp) return;
  wchar_t b[96];
  // tag 必须是 %ls：MinGW 的 swprintf 把宽字符 %s 当窄字符串用。
  swprintf(b, 96, L"retime %ls id=%d t=%.3f dur=%.3f in=%.3f spd=%.3f f=%lld", tag, c.id,
           c.timeline_start, c.duration, c.source_in, retiming_speed_at(c, c.duration),
           (long long)a.cur_frame());
  std::wstring s(b);
  s += L" keys=[";
  for (const RetimeKey &k : c.retiming) {
    swprintf(b, 96, L"%.3f:%.3f ", k.strip_frame, k.src);
    s += b;
  }
  s += L"]";
  trace(s);
}

// 连接组状态一行日志。连接本身不改变任何几何，时间线上只有那颗 LINKED 角标看得见，
// 而「点一条是不是选中了全组」这类问题必须看名单才判得准，所以留一条比量像素可靠。
void trace_links(App &a, const wchar_t *tag) {
  if (!trace_fp) return;
  wchar_t b[64];
  swprintf(b, 64, L"links %ls", tag);
  std::wstring s(b);
  for (const Clip &c : a.project.clips) {
    swprintf(b, 64, L" %lld:[", (long long)c.id);
    s += b;
    for (size_t i = 0; i < c.connections.size(); i++) {
      swprintf(b, 64, L"%s%lld", i ? L"," : L"", (long long)c.connections[i]);
      s += b;
    }
    s += L"]";
  }
  swprintf(b, 64, L" sel=%d", int(a.sel.size()));
  s += b;
  for (int64_t id : a.sel) {
    swprintf(b, 64, L" %lld", (long long)id);
    s += b;
  }
  trace(s);
}

void set_status(App &a, const std::wstring &s) {
  a.status_text = s;
  a.dirty |= kRStatus;
}

// ---------------------------------------------------------------- 区域布局
//
// space_sequencer.cc:95-134 的 region 划分：
//   HEADER      : u(1.3) = 26px（HEADERY）
//   CHANNELS    : Blender 是 UI_COMPACT_PANEL_WIDTH = 160（UI_interface_c.hh:435），
//                 按需求收到只装下「数字 + 打勾 + 锁定」（见 metrics 里的 chan_w）。
//   SCRUBBING   : UI_TIME_SCRUB_MARGIN_Y = 23*UI_SCALE_FAC（UI_view2d.hh:493），
//                 与条带区上下相接，播放头的竖线（stalk）从它的下缘往下穿过条带区
//                 （time_scrub_ui.cc:43-44, 128-129）。
//   FOOTER      : 播放控制条，同样是 HEADERY。
// 属性区：Blender 的 prefsizex = UI_SIDEBAR_PANEL_WIDTH * 1.3 = 364（space_sequencer.cc:1331，
// UI_interface_c.hh:436）按需求改成「贴着参数本身的宽度」（props_w_default）——
// 364 那一档是给英文长标签留的，这里的标签只剩两三个汉字，左边就空出一大片。
// 底部状态条 = HEADERY。
struct Metrics {
  float header, scrub, chan_w, props_w, foot;
};
static Metrics metrics() {
  Metrics m;
  m.header = std::lround(btheme::u(1.3f));   // HEADERY
  m.scrub = std::lround(23.0f * btheme::scale);
  m.chan_w = std::lround(72.0f * btheme::scale);  // 通道列 = 左边距 + 数字 + 两个图标 + 右缘
  m.props_w = props_w_default();
  m.foot = std::lround(22.0f * btheme::scale);
  return m;
}

// 面板默认宽度 = 内边距×2 + 标签列 + 数值列 + 滚动条那一槽。标签列是所有参数里最长那条
// 标签的字宽（props_label_w，画到那一行时量出来的）；数值列按 10 个控件单位：放得下最长
// 一串数值加上关键帧装饰点、成对锁那两格；槽恒在那一列不计进数值列里，宽是照着「参数
// 全展开、条画出来」那一档给的 —— 槽不画条的时候也留着，行不会为它挪位（见 props_gutter）。
float props_w_default() {
  const float pad = 0.25f * btheme::u(1.0f);
  return std::lround(2.0f * pad + props_label_w() + 10.0f * btheme::u(1.0f) +
                     props_scroll_slot());
}

// 面板宽度的上下限。下限是 Blender 的 collapse_size（screen_ops.cc:3514-3522 的非
// tab 分支 = UI_UNIT_X），比它还窄就整条收起（= region_scale_toggle_hidden）。
// 上限：时间线现在满窗口宽，只有预览让给面板，所以这条界看的是预览 ——
// 预览得还放得下一格画面（同 layout 里 vp_min 那一档），不然面板能把画面挤没。
float props_w_min() { return btheme::u(1.0f); }
float props_w_max(float win_w) {
  return std::max(props_w_min(), win_w - 80.0f * btheme::scale);
}

// 面板收起后那条能抓的边有多宽。开着的时候线本身在面板内侧，指针两侧各有 3px 可点；
// 收起后线退到窗口最外一格，只有一侧有像素，再按 3px 判就等于要求指针正好压在边上 ——
// 所以这一头放宽到 6px（也就是画出来那个手柄的宽度，看得见就点得着）。
float props_band() { return 6.0f * btheme::scale; }

// 面板在哪一边。Blender 的 UI 区（show_region_ui 那一块）原位贴在 area 的 RIGHT
// （space_sequencer.cc:196 那行 region type 就是 'UI' + RIGHT）；按需求默认放左，
// 这一条是把「放左」当可换的默认而不是写死的布局，所以右 = Blender 原样。
float props_edge(const App &a) {
  return a.props_side ? float(a.r2d.width()) - a.props.w : a.props.x + a.props.w;
}

bool props_divider_at(const App &a, float fx, float fy) {
  if (!(a.props.h > 0.0f) || fy < a.props.y || fy > a.props.y + a.props.h) return false;
  if (a.props.w > 0.0f)  // 面板开着：命中线本身那几像素（和 a.split 那条横线同一个判据）
    return std::fabs(fx - props_edge(a)) <= 3.0f * btheme::scale;
  // 面板收起：Blender 的 region_resize 在区已经隐藏时照样接着算尺寸，拖过
  // collapse_size 就把区放回来（screen_ops.cc:3522-3525 那两支 toggle_hidden）。
  // 所以这条边必须还在，只是位置退到窗口最外格；判据放宽到整条手柄带，
  // 否则「拖到最左把面板收掉」这个动作会把唯一能把它拉回来的东西一起收掉。
  const float band = props_band();
  return a.props_side ? fx >= float(a.r2d.width()) - band : fx <= band;
}

// 属性面板这一块 region 的纵向滚动条。
// Blender 把「槽」和「条」分开算：槽宽 = V2D_SCROLL_WIDTH（UI_view2d.hh:79 =
// 0.45*widget_unit + 2*pixelsize），内容永远按 tot 减掉这一槽来铺
// （view2d.cc:164-198），槽只有悬停淡出时收窄到 V2D_SCROLL_MIN_WIDTH、从不为 0；
// 画出来的条再从 region 外缘让开 edge_pad = 0.1*widget_unit（view2d.cc:1386-1399）。
// 所以行宽从不为滚动条挪位，挪的只有「这一槽里画不画条」。
// 本工程没有悬停淡出（那要一条常醒的时钟），就取它的极限形态：槽恒为全宽，条显示/隐藏。
float props_scroll_slot() {
  return 0.45f * btheme::u(1.0f) + 2.0f * btheme::pixelsize;
}

// 能画、能抓的那一截轨道 = 槽再让开外缘的 edge_pad，让的数与 Blender 那一处相同
// （view2d.cc:1386-1399 的 edge_pad = 0.1*widget_unit）。竖分界线的命中带贴在 region 外缘
// 那一格，两者只在最外半像素重叠；按下时先问分界线（gesture.cpp 的 press_left），
// 所以重叠的那半格归分界线。
Rect props_scroll_trough(const App &a) {
  if (a.props.w <= 0.0f) return {};
  const float slot = props_scroll_slot();
  const float pad = 0.1f * btheme::u(1.0f);
  const float inset = 0.25f * btheme::u(1.0f);
  const float x = a.props_side ? a.props.x + pad : a.props.x + a.props.w - slot;
  return {x, a.props.y + inset, slot - pad, std::max(0.0f, a.props.h - 2.0f * inset)};
}

// 滑块 = 「看得见的那一段」在整份内容里的位置和比例。内容总高存在 a.props_content_h 里，
// 每帧画面板之前由 draw.cpp 的面板规格表算好；这里只读那一个数，不另算一份。
Rect props_scroll_thumb(const App &a) {
  const Rect t = props_scroll_trough(a);
  const float over = a.props_content_h - a.props.h;
  if (t.h <= 0.0f || over <= 0.0f) return {};
  const float h = std::clamp(t.h * a.props.h / a.props_content_h, 30.0f * btheme::scale, t.h);
  const float y = t.y + (t.h - h) * std::clamp(a.props_scroll / over, 0.0f, 1.0f);
  return {t.x, y, t.w, h};
}

// 预览与时间线之间那条横线能不能抓住。命中带只贴线本身上下各 3px，和按下开手势时
// 用的是同一个判据（gesture.cpp 的 divider_press）—— 悬停点亮的位置与真能拖动的位置
// 不许有两套算术。
bool split_divider_at(const App &a, float fx, float fy) {
  return fx >= a.split.x && fx <= a.split.x + a.split.w &&
         std::fabs(fy - a.split.y) <= 3.0f * btheme::scale;
}

void layout(App &a) {
  a.dirty = kRegionAll;
  const Metrics m = metrics();
  const float hdr = m.header;
  float w = float(a.r2d.width()), h = float(a.r2d.height());
  // screen.show_statusbar 关掉时状态条区域整个消失，上面的 area 往下顶满
  // （wm_window.cc 的 screenareas 重算，这里是等价的矩形重排）。
  float foot_h = a.show_statusbar ? m.foot : 0.0f;
  a.status = {0, h - foot_h, w, foot_h};
  const float bottom = h - foot_h;
  // 面板宽度：拖过竖分界线就是用户定的 props_px；没拖过就一直跟着内容宽度（m.props_w）。
  // 和 tl_px 一样，窗口变宽长的是主区，面板宽度不跟着偷偷变。
  float props_w = 0.0f;
  if (a.sidebar_on && w > 760 * btheme::scale) {
    // 内容宽度第一帧还没量出来（标签宽要画到那一行才知道），第二帧起才是准的，
    // 所以这个默认值不能写死进 props_px，只能每帧现取。
    const float px = a.props_px > 0.0f ? a.props_px : m.props_w;
    // 取整到设备像素：Blender 的 region 矩形本来就是整数（ARegion.sw 是 rcti），
    // 所以它的 1px 边框、分隔线永远正好压在一个像素上。这里不取整的话，拖动面板
    // 宽度留下的 .x 小数会让整条边界被 LINEAR 采样摊成两根半亮像素（= 界面发糊）。
    props_w = float(std::lround(std::clamp(px, props_w_min(), props_w_max(w))));
  }
  float main_x = a.props_side ? 0.0f : props_w;  // 预览从面板的邻边开始
  float main_w = w - props_w;
  // region_rect_recursive 是按 space_sequencer.cc:95-190 的建表顺序走表的：
  // HEADER(TOP) -> TOOL_HEADER -> FOOTER(BOTTOM) -> SCRUBBING -> UI(RIGHT) -> TOOLS ->
  // CHANNELS -> PREVIEW -> WINDOW。所以 HEADER 与 FOOTER 先切走整宽的一条，UI 区（N 面板）
  // 之后才从剩下的矩形里切出来——它夹在 header 下缘与 footer 上缘之间，而 header/footer
  // 是横跨整个 area 宽度的（含属性区上方那一段）。
  // 与源码不同的一点（需求）：面板让给预览、时间线满窗口宽。所以面板放左时只有预览
  // 右移 props_w，放右时（= Blender 的 UI 区原位）预览从 0 开始；时间线不受面板影响。
  // area 的 HEADER 不铺：它在本工程里一条控件都没有（编辑器菜单、视图/叠加下拉、吸附开关、
  // 场景名框按「没提到就删除」全去掉），留着就是顶栏和主区之间一道谁也不管的空白横条。
  // 所以主区从顶栏下缘直接开始。预览与时间线之间仍然只有那一条可拖动的区域分界线。
  a.topbar = {0, 0, w, hdr};
  const float body_top = hdr;
  // 时间线 area 底下不再有 FOOTER（播放控制条按需求删掉了），主区一直铺到状态条上缘。
  const float body_bot = bottom;
  // 预览在上、时间线整块在下（用户要求的窗口排布；Blender 默认时间线在上，只换顺序）。
  // 时间线区的高度由分界线的拖动决定（tl_px，像素）：拖到哪就停在哪，窗口变高长的是预览，
  // 可见通道行数不会跟着偷偷变。两条带的尺寸先算：SCRUBBING 顶带 + 通道行（外加分界线
  // 自己那 1px）。
  const float scrub_h = a.show_scrub ? float(m.scrub) : 0.0f;
  // 底部留白 = sequencer_view.cc:79-92 的 SEQ_get_timeline_region_padding 的下边距。
  // 没有标记时它就是 V2D_SCROLL_HANDLE_HEIGHT = 0.6*widget_unit（UI_view2d.hh:74）；
  // 本工程按需求不做标记，所以恒取这一档（有标记时的 34*UI_SCALE_FAC 那支不再存在）。
  const float pad_bot = std::lround(0.6f * btheme::u(1.0f));
  // 时间线 area 的默认高度：这是本工程的 UI 分栏默认，不是行高反推出来的。Blender 出厂
  // 工作区里时间线 area 占多大写在出厂 .blend（读不到源码），这里按窗口 body 高的比例给，
  // 和「行高 = 区高 ÷ 8.5」那条对象规则各管一层，互不借用。
  if (a.tl_px <= 0.0f) a.tl_px = std::lround(0.30f * (body_bot - body_top));
  // 至少塞得下一行：行高在第一次布局时还是 0（那一格在下面现算），所以这一条第一帧不起
  // 作用，从第二帧起就是活的行高。
  const float tl_min = scrub_h + channel_h(a) + pad_bot + btheme::pixelsize;
  const float vp_min = 80.0f * btheme::scale;
  float tl_h = std::clamp(a.tl_px, tl_min, std::max(tl_min, body_bot - body_top - vp_min));
  // 同 props_w：分界线落在整数像素上，预览区/时间线区的 1px 边界才不会摊成两根半亮线。
  float split_y = std::lround(std::max(body_top, body_bot - tl_h));
  a.viewport = {main_x, body_top, main_w, split_y - body_top};
  a.split = {0, split_y, w, btheme::pixelsize};
  // 时间线 area：CHANNELS(左) + SCRUBBING(顶带) + 条带，底边到 area FOOTER 上缘。
  // 按需求整块满窗口宽：它不再让给左侧面板那一竖条，面板只到预览下缘。
  float body_y = a.split.y + a.split.h;
  float body_h = body_bot - body_y;
  float rows_h = std::max(btheme::u(1.0f), body_h - scrub_h - pad_bot);
  a.scrub = {0, body_y, w, scrub_h};
  // CHANNELS 区域的底边一直到留白下方（通道头左边的那一块底色与留白同为 0x1d1d1d）。
  a.chan = {0, body_y + scrub_h, m.chan_w, rows_h + pad_bot};
  a.lanes = {m.chan_w, body_y + scrub_h, w - m.chan_w, rows_h};
  a.tl_area = {0, a.split.y, w, body_bot - a.split.y};
  a.props = {a.props_side ? w - props_w : 0.0f, body_top, props_w, split_y - body_top};
  // 行高（= 1/zoomy）第一次布局时按对象的初始答案定下：一屏 8.5 个通道
  // （tot.ymax = 8.5、cur = tot，space_sequencer.cc:177-179）⇒ 条带区高 ÷ 8.5。
  // 定下之后就交给 V2D_KEEPZOOM：resize 与拖分界线保持「像素/通道」不变，
  // 变的是看得见的通道数（view2d.cc:363-371 的 do_init 之后，:447-467）。
  // 这一趟只算视图的数：project 的数据一个都不改，改数据的是按键与拖动那一边。
  if (a.tl.ch_px <= 0.0f && a.lanes.h > 0.0f)
    a.tl.ch_px = float(a.lanes.h / kViewDefaultChannels);
  clamp_view_ch(a);
}

// 行高不是「轨道数除尽区域高」，而是纵向 view2d 的 1/zoomy，恒定不随行数变：
// 初始那一格由 layout 写进 ch_px（= 区高 ÷ 8.5，见上面），之后 keepzoom = V2D_KEEPZOOM
// （space_sequencer.cc:192）让区域改高度时「像素/通道」保持不变
// （view2d.cc:447-467：新 cur 高 = 旧 cur 高 * 新区高/旧区高），变的是可见行数。
// 通道数超过可见行时不压缩，改成滚动（view2d.cc:737-759 把 cur 平移着夹进 tot）。
float channel_h(const App &a) {
  float h = a.tl.ch_px;
  // cur 的尺寸夹在 v2d->min[1]..max[1] = 1..128 个通道（space_sequencer.cc:182,185
  // + view2d.cc:496-499），等价于行高不能小于区高/128、不能大于整个区高。
  // ch_px 还是 0（第一次布局的那一帧，区高刚有值）时收到下限，下一帧就被 layout 定下来。
  if (a.lanes.h > 0.0f) h = std::clamp(h, a.lanes.h / float(kMaxChannels), a.lanes.h);
  return h;
}

// 纵向 tot 的上边界（0 基通道单位）：strip_time.cc:321-340 的 timeline_boundbox 的 y 方向
// 对应物。Blender 那里 ymin=1、ymax=max(8, 条带最高通道+1)，且通道恒建满 128 个；
// 本工程的「通道数」是用户在 +/- 里声明的格数，所以这三样取最大：
// 声明的通道数、条带占到的最高一格、区域当前塞得下的行数。
// 第三项就是「拖分隔条看到通道变多」：可见行数本来就属于视图，行数不该由谁去回写
// project.chan_count —— 一旦显示层改数据，撤消恢复出来的快照立刻又被改掉，撤消链就定不住。
double timeline_tot_hi(const App &a) {
  double hi = std::max({1.0, double(a.project.chan_count), cur_channels(a)});
  for (const Clip &c : a.project.clips)
    hi = std::max(hi, double(c.track + 1));
  return hi;
}

void clamp_view_ch(App &a) {
  const float h = channel_h(a);
  if (h <= 0.0f) {
    a.tl.view_ch = 0.0;
    return;
  }
  const double cur = double(a.lanes.h) / double(h);  // 可见通道数
  a.tl.view_ch = std::max(0.0, a.tl.view_ch);              // V2D_ALIGN_NO_NEG_Y（:797-802）
  const double hi = timeline_tot_hi(a);  // 已经把「可见行数」算进去了，不需要再抬一次
  a.tl.view_ch = std::min(a.tl.view_ch, hi - cur);         // cur.ymax 不出 tot.ymax
  a.tl.view_ch = std::max(0.0, a.tl.view_ch);
}

// 右侧滚动条的轨道：vert = mask 右缘往里 V2D_SCROLL_HANDLE_WIDTH（v2d->scroll 有
// V2D_SCROLL_RIGHT|VERTICAL_HANDLES，space_sequencer.cc:191 + view2d.cc:175-180,1874），
// 上端不让出 scrub 带（ED_time_scrub_clamp_scroller_mask：mask.ymax -= 23*scale，
// time_scrub_ui.cc:370-375），下端让出 UI_AZONESPOTH = 0.6*widget_unit（view2d.cc:201-206，
// UI_interface_c.hh:2486），右端收 edge_pad = 0.1*widget_unit（view2d.cc:1387-1395）。
Rect scroll_trough(const App &a) {
  const float w = 0.6f * btheme::u(1.0f);
  const float pad = float(int(0.1f * btheme::u(1.0f)));
  const float top = a.tl_area.y + (a.show_scrub ? float(std::lround(23.0f * btheme::scale)) : 0.0f);
  const float bot = a.lanes.y + a.lanes.h;  // 原标记带的 0.6*unit 留白正好抵掉 AZONESPOTH
  // vert.xmax = mask.xmax + 1（区右缘）；vert.xmin = vert.xmax - 条宽；再 vert.xmax -= edge_pad。
  const float right = a.tl_area.x + a.tl_area.w;
  return {right - w, top, w - pad, std::max(0.0f, bot - top)};
}

// 滑块像素区间：view2d.cc:1450-1483。Blender 用的是 y 朝上的轨道坐标：
// bot = vert.ymin + fac1*scrollsize（fac1<=0 时直接取 vert.ymin）、
// top = fac2>=1 时取 vert.ymax，否则 vert.ymin + fac2*scrollsize；
// 然后「不足 V2D_SCROLL_THUMB_SIZE_MIN 就加长并两端夹住」。这里换成客户区的 y 朝下，
// 把 bot/top 记成「距轨道底边的像素」再翻到顶部坐标，逻辑与源码一一对应。
Rect scroll_thumb(const App &a) {
  const Rect t = scroll_trough(a);
  const double tot_h = std::max(1.0, timeline_tot_hi(a));  // BLI_rctf_size_y(&tot)
  const float h = channel_h(a);
  const double cur = h > 0.0f ? double(a.lanes.h) / double(h) : 8.5;
  const double fac1 = a.tl.view_ch / tot_h;
  const double fac2 = (a.tl.view_ch + cur) / tot_h;
  const double size = double(t.h);
  double bot = fac1 <= 0.0 ? 0.0 : fac1 * size;
  double top = fac2 >= 1.0 ? size : fac2 * size;
  const double min = 30.0 * double(btheme::scale);  // V2D_SCROLL_THUMB_SIZE_MIN（UI_view2d.hh:99）
  if (top - bot < min) {
    top = bot + min;
    top = std::clamp(top, min, size);
    bot = std::clamp(bot, 0.0, size - min);
  }
  return {t.x, float(t.y + t.h - top), t.w, float(top - bot)};
}

double cur_channels(const App &a) {
  const float h = channel_h(a);
  return h > 0.0f ? double(a.lanes.h) / double(h) : 8.5;
}

// 命中分区：view2d_mouse_in_scrollers 只认 v2d->vert 这块矩形（UI_view2d.hh:119），
// 里面再按 scrollbar_zone_get（view2d_ops.cc:1848-1878）的顺序判 bar → max → min → 外侧。
// sh_min/sh_max 是 y 朝上的滑块两端，翻到屏幕坐标就是滑块下沿/上沿。
int scroll_zone_at(const App &a, float fx, float fy) {
  const Rect t = scroll_trough(a);
  if (fx < t.x || fx > t.x + t.w || fy < t.y || fy > t.y + t.h) return kSQNone;
  const Rect th = scroll_thumb(a);
  const float hot = 0.6f * btheme::u(1.0f);  // V2D_SCROLL_HANDLE_SIZE_HOTSPOT
  if (fy > th.y + hot && fy < th.y + th.h - hot) return kSQBar;
  if (std::fabs(fy - th.y) <= hot) return kSQMax;
  if (std::fabs(fy - (th.y + th.h)) <= hot) return kSQMin;
  if (fy > th.y + th.h) return kSQMinOut;
  if (fy < th.y) return kSQMaxOut;
  return kSQBar;
}

// 按下之后怎么换算（fac / fac_round / vsm->lasty）是「这一次按住」的事，
// 写在 gesture.h 的 ScrollBar 自己身上。

// 通道行序：Blender 的通道自下而上叠（通道号越大越靠上、盖得越高，seq_slide 往上拖
// 就是加通道号），行按「通道 n 占 <n, n+1>、第一条在最下面一行」平铺，
// 只画视图里看得见的这几行。
//
// 空行也是要画出来的：Blender 的 128 个通道是一次性建满的（channels.cc:30-40），
// 通道头按视图可见范围逐行画（sequencer_channels_draw.cc:70-82 的
// displayed_channel_range_get：max(1,floor(cur.ymin))..ceil(cur.ymax)，上端再夹到
// MAX_CHANNELS），空通道同样有「Channel n」名字和静音/锁定图标。所以超出实际
// 轨道数的行只是还没被条带或图标用到，点下去才把这条通道补进数据。
const std::vector<TrackRow> &track_rows(const App &a) {
  static std::vector<TrackRow> rows;
  static double stamp = -1.0;
  double now_stamp = double(a.lanes.h) * 4096.0 + double(a.project.tracks.size()) +
                     double(a.lanes.y) * 1024.0 + a.tl.view_ch * 8192.0 +
                     double(channel_h(a)) * 16.0 + timeline_tot_hi(a) * 2.0;
  if (now_stamp == stamp) return rows;
  stamp = now_stamp;
  rows.clear();
  const float h = channel_h(a);
  if (h <= 0.0f) return rows;
  const float bottom = a.lanes.y + a.lanes.h;
  const double cur = double(a.lanes.h) / double(h);  // cur 的 y 尺寸 = 可见通道数
  int first = std::max(0, int(std::floor(a.tl.view_ch)));
  // 上限：Blender 是 MAX_CHANNELS，本工程再夹一道「纵向 tot 的上边界」——
  // 超出范围的行既没被声明、也没被条带占到、区域也塞不下，就不画。
  int last = std::min({kMaxChannels - 1, int(timeline_tot_hi(a)) - 1,
                       int(std::ceil(a.tl.view_ch + cur)) - 1});
  for (int t = first; t <= last; ++t)
    rows.push_back({t, bottom - float(t + 1 - a.tl.view_ch) * h, h});
  return rows;
}

int track_at_y(const App &a, float y) {
  if (y < a.lanes.y || y >= a.lanes.y + a.lanes.h) return -1;
  for (const TrackRow &r : track_rows(a))
    if (y >= r.y && y < r.y + r.h) return r.track;
  return -1;
}

float time_x(const App &a, double t) { return a.lanes.x + float(t - a.tl.scroll_t) * float(a.tl.pps); }
double x_time(const App &a, float x) {
  if (a.tl.pps <= 0) return a.tl.scroll_t;
  return a.tl.scroll_t + (x - a.lanes.x) / a.tl.pps;
}

// ---------------------------------------------------------------- 预览 view2d

Rect fit_box(const App &a) {
  Rect v = a.viewport.inset(8, 8, 8, 8);
  if (v.w <= 0 || v.h <= 0) return v;
  double out_ar = display_ar(a.project.fmt);
  double box_ar = double(v.w) / v.h;
  if (box_ar > out_ar) {
    float w2 = float(v.h * out_ar);
    return {v.x + (v.w - w2) * 0.5f, v.y, w2, v.h};
  }
  float h2 = float(v.w / out_ar);
  return {v.x, v.y + (v.h - h2) * 0.5f, v.w, h2};
}

// 预览区域默认 SEQ_ZOOM_TO_FIT（space_sequencer.cc:82），缩放上下限取 view2d 的
// minzoom/maxzoom（space_sequencer.cc:153-154 = 0.001 与 1000）。
Rect view_rect(const App &a) {
  Rect f = fit_box(a);
  float w = f.w * a.view_zoom, h = f.h * a.view_zoom;
  return {f.x + (f.w - w) * 0.5f + a.view_pan_x, f.y + (f.h - h) * 0.5f + a.view_pan_y, w, h};
}

// 条带在画布上的原始矩形（用户缩放/旋转/位移都作用在它之上）。
// Blender 没有任何「适配」：calc_strip_transform_matrix（render.cc:419-456）只有各向同性的
// image_scale_factor（= 分辨率%，画布同比例缩放，所以在画布里的相对大小不变）和用户的
// transform，全程不比较源与画布的长宽比；center_in_canvas（:451-452）把画面中心对到画布中心。
// 于是画面按源像素原尺寸摆着，比画布大就被画布裁掉、比画布小就露出画布底色。
Rect clip_base_rect(const App &a, const Clip &c, const Rect &box) {
  const OutputFormat &f = a.project.fmt;
  int sw = 0, sh = 0;
  clip_source_size(a, c, &sw, &sh);
  if (!sw || !sh || f.width < 1 || f.height < 1) return box;
  const float w = box.w * float(sw) / float(f.width);
  const float h = box.h * float(sh) / float(f.height);
  return {box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h};
}

void clip_source_size(const App &a, const Clip &c, int *w, int *h) {
  // 文本的「源」就是排版器现排出来的那张位图，它的像素就是输出像素
  // （Blender 的 text_size 同样以渲染缓冲像素计，text_effect_line_size_get :797-807）。
  // 画布宽只因为方框边距是它的比例才参与，所以传格式面板的宽。
  if (c.kind == kStripText) {
    const txt::Bitmap *bm = txt::bitmap(c.text, a.project.fmt.width);
    *w = bm ? bm->w : 0;
    *h = bm ? bm->h : 0;
    return;
  }
  *w = c.info ? c.info->width : 0;
  *h = c.info ? c.info->height : 0;
}

// 1 输出像素在屏幕上占多少像素，横竖各一个：横 = 画幅屏幕宽 ÷ 格式面板的宽，
// 竖 = 画幅屏幕高 ÷ 格式面板的高。分母只有格式面板那对数，所以「缩放 %」缩的是渲染缓冲，
// 不改变画面在画布上的相对位置（导出走的是同一条口径）；宽高比不为 1 时横竖的换算本来就
// 不同，这跟 clip_base_rect 给画面定尺寸用的是同一对分母。
// 画画面、算取景框四角、把拖动换成位移三处都读这里：各算各的就会出现「框跟着手走、画不跟着走」。
ScreenPx output_to_screen(const App &a, const Rect &box) {
  const OutputFormat &f = a.project.fmt;
  return {box.w / float(std::max(1, f.width)), box.h / float(std::max(1, f.height))};
}

// ---- 预览取景框（每片段一只）------------------------------------------------------
// 四角必须和真正画上去的那幅画逐像素重合，所以这里的算式就是 Render2D::image_affine
// 的算式，一个符号都不改：裁切按源 uv 收边、缩放旋转绕
// transform.origin 那个支点、镜像沿画框中线翻。
// 角序照 Blender 的 image_transform_quad_get：
// 0=右上 1=右下 2=左下 3=左上（y 在这里是屏幕向下，所以「上」是数值小的那个）。
void clip_screen_quad(const App &a, const Clip &c, const Rect &box, float *pts, float *pivot) {
  const ScreenPx sp = output_to_screen(a, box);
  const Rect r = clip_base_rect(a, c, box);
  const float bx = r.x + r.w * 0.5f, by = r.y + r.h * 0.5f;
  const float hw = r.w * 0.5f, hh = r.h * 0.5f;
  int sw = 0, sh = 0;
  clip_source_size(a, c, &sw, &sh);
  const double w = sw > 0 ? double(sw) : 1.0;
  const double h = sh > 0 ? double(sh) : 1.0;
  const float u0 = float(std::clamp(c.crop_l / w, 0.0, 1.0)),
              u1 = float(std::clamp(1.0 - c.crop_r / w, 0.0, 1.0));
  const float v0 = float(std::clamp(c.crop_t / h, 0.0, 1.0)),
              v1 = float(std::clamp(1.0 - c.crop_b / h, 0.0, 1.0));
  const float corner[8] = {hw * (u0 * 2 - 1), hh * (v0 * 2 - 1), hw * (u1 * 2 - 1),
                           hh * (v0 * 2 - 1), hw * (u1 * 2 - 1), hh * (v1 * 2 - 1),
                           hw * (u0 * 2 - 1), hh * (v1 * 2 - 1)};
  const XformAt x = xform_at(c, a.project.fmt, a.project.playhead);
  const float omx = r.w * (float(c.origin_x) - 0.5f), omy = r.h * (0.5f - float(c.origin_y));
  const float rad = float(x.rotation) * 3.14159265f / 180.0f;
  const float co = cosf(rad), si = sinf(rad);
  const float dx = float(x.ofs_x) * sp.x, dy = float(-x.ofs_y) * sp.y;
  const float sx_ = omx * float(x.scale_x), sy_ = omy * float(x.scale_y);
  const float cx = bx + dx + omx - (sx_ * co - sy_ * si);
  const float cy = by + dy + omy - (sx_ * si + sy_ * co);
  for (int i = 0; i < 4; ++i) {
    const float px = corner[i * 2] * float(x.scale_x), py = corner[i * 2 + 1] * float(x.scale_y);
    float qx = cx + px * co - py * si, qy = cy + px * si + py * co;
    if (c.flip_x) qx = 2 * bx - qx;
    if (c.flip_y) qy = 2 * by - qy;
    pts[i * 2] = qx;
    pts[i * 2 + 1] = qy;
  }
  // 支点：raw 空间里离 origin 那点最近的图像位置，缩放/旋转前后都落在同一处屏幕像素
  // （= render.cc:429-443 的 T(om) … T(-om) 里那个不动点）。
  pivot[0] = bx + dx + omx;
  pivot[1] = by + dy + omy;
}

// 凸四边形内点测试：四角按顺序绕一圈，叉积同号就在里面。
// 叉积的减数必须钉在同一条边的同一个端点上（这里统一用 i）：x 用 i、y 用 j 那种写法，
// 每条边会被多出一个 -ax*ay 的偏置，只有轴对齐（ax 或 ay 为 0）时才恰好为 0。
static bool quad_contains(const float *pts, float fx, float fy) {
  int sign = 0;
  for (int i = 0; i < 4; ++i) {
    const int j = (i + 1) & 3;
    const float ax = pts[j * 2] - pts[i * 2], ay = pts[j * 2 + 1] - pts[i * 2 + 1];
    const float cr = ax * (fy - pts[i * 2 + 1]) - ay * (fx - pts[i * 2]);
    if (cr > 0.0001f) {
      if (sign < 0) return false;
      sign = 1;
    } else if (cr < -0.0001f) {
      if (sign > 0) return false;
      sign = -1;
    }
  }
  return true;
}

// 从最上面那条往下问自己的四角和框内（rendered_clips 已按通道升序 = 叠画顺序），
// 第一个答应的交出一个 FramingDrag —— 那一次按下的快照全部写在对象自己身上，
// 外壳不再摊一排 box_* 字段。先角后框内：角把手压在框线上，同一个点既是角也在框里，
// 把手优先，否则永远抓不到缩放。
FramingDrag *framing_press(App &a, float fx, float fy) {
  if (!a.viewport.contains(fx, fy)) return nullptr;  // 取景框只活在预览画面里
  const Rect box = view_rect(a);
  std::vector<Clip *> stack = rendered_clips(a, a.project.playhead);
  for (int i = int(stack.size()) - 1; i >= 0; --i) {
    Clip *c = stack[i];
    if (c->locked) continue;  // SEQ_LOCK：条带锁了就不给拖（transform 的 is_locked 过滤）
    float pts[8], pv[2];
    clip_screen_quad(a, *c, box, pts, pv);
    int corner = -1;
    const float hr = framing_handle_r();
    for (int k = 0; k < 4; ++k)
      if (std::fabs(fx - pts[k * 2]) <= hr && std::fabs(fy - pts[k * 2 + 1]) <= hr) {
        corner = k;
        break;
      }
    if (corner < 0 && !quad_contains(pts, fx, fy)) continue;
    FramingDrag *g = new FramingDrag();
    g->clip_id = c->id;
    // 快照全部取当前帧的求值值，和四角、和画上去的那幅画读的是同一个数：挂了曲线的条带，
    // 标量 ofs 是「没动画时的那个值」，可能早就和曲线上这一帧不一样了。按标量起算的话，
    // 第一次一动就把画面甩到「标量 + 增量」，也就是甩到光标别的地点去。
    const XformAt x = xform_at(*c, a.project.fmt, a.project.playhead);
    g->ofs0x = x.ofs_x;
    g->ofs0y = x.ofs_y;
    g->scale0x = x.scale_x;
    g->scale0y = x.scale_y;
    g->pivx = pv[0];
    g->pivy = pv[1];
    g->by_corner = false;
    if (corner >= 0) {
      const float ux = pts[corner * 2] - pv[0], uy = pts[corner * 2 + 1] - pv[1];
      const float len = std::sqrt(ux * ux + uy * uy);
      if (len >= 1.0f) {  // 支点跑到角上了就没得缩放，按移动处理
        g->ux = ux / len;
        g->uy = uy / len;
        g->d0 = len;
        g->by_corner = true;
      }
    }
    select_only(a, c->id);  // 拖谁 = 选谁，面板跟着显示它
    if (trace_fp) {
      wchar_t t[160];
      swprintf(t, 160, L"box-press kind=%d id=%lld ofs=%.0f,%.0f scale=%.3f,%.3f pivot=%.0f,%.0f",
               int(g->by_corner), (long long)g->clip_id, g->ofs0x, g->ofs0y, g->scale0x, g->scale0y,
               g->pivx, g->pivy);
      trace(t);
    }
    return g;
  }
  return nullptr;
}

// 每一次拖动都从按下时的快照重算（transform_convert_sequencer_image.cc:274-279 也是
// 先把 transform->xofs/scale 复位成 orig_* 再写结果），所以中途改播放头、改曲线都不会累积误差。
void FramingDrag::drag(App &a, float fx, float fy) {
  Clip *c = find_clip(a, clip_id);
  if (!c) {
    a.gesture.reset();
    return;
  }
  const Rect box = view_rect(a);
  const ScreenPx sp = output_to_screen(a, box);
  if (sp.x <= 0.0f || sp.y <= 0.0f) return;  // 画幅被压成零：没有可换算的比例
  if (!by_corner) {
    // 位移在 image_affine 里是「旋转之前先加到中心」的纯平移（cx = bx + dx …），
    // 所以屏幕上的增量直接换算成输出像素即可，不必按旋转反解。y 轴：屏幕向下 = ofs_y 向下。
    prop_apply(a, kFOfsX, c, ofs0x + (fx - grab_x) / sp.x, false);
    prop_apply(a, kFOfsY, c, ofs0y - (fy - grab_y) / sp.y, false);
  } else {
    // 缩放绕支点（transform.origin 那一点），和 Blender 的 resize 把手同一个锚：
    // 倍率 = 光标在「支点→被抓的那个角」这条线上的投影距离 / 按下时那个角的同距离。
    // 两轴乘同一个倍率 ⇒ 宽高比保持不变（需求里的「保持比例」），比例本身按按下时的实际值。
    const float d = (fx - pivx) * ux + (fy - pivy) * uy;
    if (d0 <= 0.0f || d <= 0.0f) return;  // 拖过支点就不给翻了：缩放因子必须为正
    const float f = std::clamp(d / d0, 1e-4f, 1e4f);
    prop_apply(a, kFScaleX, c, scale0x * f, false);
    prop_apply(a, kFScaleY, c, scale0y * f, false);
  }
  a.dirty |= kRViewport | kRProps;
}

// 改的是真数据（位置/缩放），拖一趟 = 一步撤消，和拖数值按钮同一个规矩。
void FramingDrag::up(App &a) {
  if (!moved) return;
  undo_push(a, by_corner ? L"缩放取景框" : L"移动取景框");
  if (!trace_fp) return;
  const Clip *c = find_clip(a, clip_id);
  wchar_t t[160];
  swprintf(t, 160, L"box-drag id=%lld ofs=%.0f,%.0f scale=%.3f,%.3f keys=%d,%d",
           (long long)clip_id, c ? c->ofs_x : 0.0, c ? c->ofs_y : 0.0, c ? c->scale_x : 0.0,
           c ? c->scale_y : 0.0, c ? int(c->anim[kChOfsX].keys.size()) : -1,
           c ? int(c->anim[kChScaleX].keys.size()) : -1);
  trace(t);
}

void view_zoom_at(App &a, float px, float py, float factor) {
  Rect f = fit_box(a);
  float z1 = a.view_zoom;
  float z2 = std::clamp(z1 * factor, 0.001f, 1000.0f);
  if (z1 <= 0 || z2 == z1) return;
  float r = z2 / z1;
  float cx = f.x + f.w * 0.5f + a.view_pan_x, cy = f.y + f.h * 0.5f + a.view_pan_y;
  a.view_pan_x = px - (px - cx) * r - (f.x + f.w * 0.5f);
  a.view_pan_y = py - (py - cy) * r - (f.y + f.h * 0.5f);
  a.view_zoom = z2;
  a.dirty |= kRViewport;
}

// view2d.zoom_set（VIEW_MT_preview_zoom 的百分比档）：直接设定缩放，围绕视口中心。
void view_zoom_set(App &a, float percent) {
  if (a.view_zoom <= 0.0f) return;
  float target = std::clamp(percent / 100.0f, 0.001f, 1000.0f);
  view_zoom_at(a, a.viewport.x + a.viewport.w * 0.5f, a.viewport.y + a.viewport.h * 0.5f,
               target / a.view_zoom);
}

void view_reset(App &a) {  a.view_zoom = 1.0f;
  a.view_pan_x = a.view_pan_y = 0.0f;
  a.dirty |= kRViewport;
}

// 滚轮 / Numpad± 的「缩放」这一格：按键只写属性 —— pps 按系数动一格，再把「指针在哪、
// 动之前它底下是哪一刻」记成一次请求（请求 ≠ 决定）。要不要把那一刻钉在指针底下、视图
// 最终摆在哪里，是对象在 timeline_view_react 里按自己的情况（内容多长、窗口多宽、指针
// 在不在车道里）决定的。
void zoom_at(App &a, float px, float py, double factor) {
  a.tl.zoom_req = true;
  a.tl.zoom_req_x = px;
  a.tl.zoom_req_y = py;
  a.tl.zoom_req_t = x_time(a, px);  // 用改之前的映射取，才是「指针底下这一刻」
  a.tl.pps *= factor;
  a.dirty |= kRLanes | kRScrub;
  timeline_view_react(a);        // 对象立刻反应一次；绘制时还会再过同一道关
}

// ---------------------------------------------------------------- 视图回答「指针这一格事件」
//
// 滚轮、右键、几条围绕指针的键，都是同一个形状：事件带着指针那一点往下问，认出这一点在
// 自己身上那块视图才动，谁都不认 = 这一格不动作。窗口过程和快捷键表从此不再判断「这一点
// 落在哪一块地方」（逻辑.md：触发者绝对中立、执行者绝对自治），判断住在每块视图自己的回答
// 里 —— 和 gesture.cpp 的 press_left / press_middle 同构，只是这些事件没有「拖」的后半段。

// 预览那一块 view2d：Ctrl/Shift = 平移，普通滚轮 = 围绕指针缩放。
// 滚轮的三种分工是需求定的：普通缩放、Ctrl 左右移、Shift 上下移，两块视图同一套。
static bool preview_view_wheel(App &a, float fx, float fy, int dir, WORD mods) {
  if (!a.viewport.contains(fx, fy)) return false;
  const double step = kScrollStepPx * double(btheme::scale);
  if (mods & kCtrl)
    a.view_pan_x += float(dir * step);
  else if (mods & kShift)
    a.view_pan_y += float(dir * step);
  else {
    view_zoom_at(a, fx, fy, float(dir > 0 ? kZoomIn : kZoomOut));
    return true;
  }
  a.dirty |= kRViewport;
  return true;
}

// 时间线那一块 view2d。pps<=0 时它连「一格像素等于多少」都答不出来，让给下一个对象。
static bool timeline_view_wheel(App &a, float fx, float fy, int dir, WORD mods) {
  if (!a.tl_area.contains(fx, fy) || a.tl.pps <= 0) return false;
  const double step = kScrollStepPx * double(btheme::scale);
  if (mods & kCtrl) {  // 横向：时刻平移。时间线前后无限：没有左端可撞，负时刻照常显示
    a.tl.scroll_t += -double(dir) * step / a.tl.pps;
    a.dirty |= kRLanes | kRScrub;
    return true;
  }
  if (mods & kShift) {  // 纵向：翻通道。view2d_ops.cc:558 的 deltay = ∓40*UI_SCALE_FAC，
                        // 再乘 facy = cur/(区高+1)。
    const double facy = cur_channels(a) / (double(a.lanes.h) + 1.0);
    a.tl.view_ch += double(dir) * step * facy;
    clamp_view_ch(a);
    a.dirty |= kRChan | kRLanes | kRScrub;
    return true;
  }
  // 指针原样交给对象：在不在车道里、视图最终摆在哪里，是 zoom_at 那头的反应决定的。
  zoom_at(a, fx, fy, dir > 0 ? kZoomIn : kZoomOut);
  a.dirty |= kRLanes | kRScrub;
  return true;
}

// 属性面板这一块 region 对滚轮的答复：只写 props_scroll 这一个数。滚不滚得动、能滚多少
// 是它自己的事，由画的那一趟收（draw_props 用实测内容高钳制）—— 内容放得下时这里加的数
// 立刻被收回 0，视图一动不动，同 timeline_view_react 那条「拒绝就真不动」。
static bool props_view_wheel(App &a, float fx, float fy, int dir) {
  if (!a.props.contains(fx, fy)) return false;
  const float step = float(kScrollStepPx * double(btheme::scale));
  a.props_scroll -= dir > 0 ? step : -step;  // 上滚 = 内容往下走
  a.dirty |= kRProps;
  return true;
}

// 一格滚轮。菜单是上一层的 block，它开着就只归它；Alt 那一支改的是工程数据（播放头），
// 跟地方无关；剩下的三块地方挨个问。
void wheel_step(App &a, int x, int y, short delta, WORD mods) {
  const float fx = float(x), fy = float(y);
  if (a.pop.is_open()) return;
  const int dir = delta > 0 ? 1 : -1;
  // km_frames:3847-3850 —— Alt+滚轮 = screen.frame_offset ±1 帧。
  if (mods & kAlt) {
    nudge(a, -double(dir) * a.frame_dur());
    if (a.playing) toggle_play(a);
    a.dirty |= kRViewport | kRLanes | kRScrub;
    return;
  }
  if (props_view_wheel(a, fx, fy, dir)) return;
  if (preview_view_wheel(a, fx, fy, dir, mods)) return;
  if (timeline_view_wheel(a, fx, fy, dir, mods)) return;
  // 谁都不答应（顶栏、状态条、两块视图之间那一条缝）= 这一格不动作。
}

// HOME（view2d.view_all / sequencer.view_all）：先问预览，预览不认这一点就归时间线。
void view_all_at(App &a, float fx, float fy) {
  if (a.viewport.contains(fx, fy)) {
    view_reset(a);  // 预览那一块 = 画幅居中适配（缩放 100%、平移归零）
    return;
  }
  timeline_view_all(a);  // 其余 = 适配时间线自己的 tot（帧范围 ∪ 条带把手）
}

// Numpad±（km_view2d:976-979）：同一道问句，答的那一块围绕指针缩放一格。
void view_zoom_step(App &a, float fx, float fy, double factor) {
  if (a.viewport.contains(fx, fy)) view_zoom_at(a, fx, fy, float(factor));
  else zoom_at(a, fx, fy, factor);
}

// 时间线视图的「内容界」= 对象自己排出来的队：条带把手，对应 strip_time.cc:329-341 的
// timeline_expand_boundbox（右端 +1 帧，因为一帧长的条带从 1 画到 2），
// 而且每帧重算（sequencer_timeline_draw.cc:2095）⇒ 条带拖多远它跟多远。
// 帧范围（开始帧..结束帧）不在这里：它负责播放与导出（playback_bounds、export.cpp），
// 不是视图的对齐 —— 用户 2026-11-02 定的层次：「HOME 对齐的是对象所有，无论多少对象排队」
// 「初始帧和结束帧是独立的，负责 UI 播放和导出」。一个对象都没有时对象给不出界，
// 才退回 Blender region 初始化那一格视野（cur = [0, efra]，space_sequencer.cc:174-179）。
double timeline_tot_end(const App &a) {
  if (a.project.clips.empty()) return a.frame_to_t(int64_t(a.project.fmt.frame_end) + 1);
  return project_end(a) + a.frame_dur();
}

// tot 的左端：有内容时最左把手（条带拖到 0 帧以左它必须跟着变负，否则对齐不住），
// 没有内容时是原点 0。
double timeline_tot_start(const App &a) {
  double lo = 0.0;
  for (const Clip &c : a.project.clips) lo = std::min(lo, c.timeline_start);
  return lo;
}

// 对象收到「缩放」这一格以后的反应策略，一条：视野里的帧数收在 min[0]..max[0]
// （space_sequencer.cc:181 的 10 帧、:184 的 MAXFRAMEF=1048574 帧），钳的是**帧数**，
// 不是像素也不是秒 —— view2d.cc:497 的 CLAMP(width, min[0], max[0])，这条分支走得到是因为
// 该 region 的 keepzoom 只有 V2D_KEEPZOOM、没有 V2D_LIMITZOOM（:192）。
// 缩小这一头没有「装不下」：3 小时的片子 728,261 帧、50 条排队也还在 max[0] 以内，
// 想缩多小缩多小；上一版那条 `avail/span`（不许缩得比整条内容还小）是把 UI 的后果
// （「怕缩丢了」）当成对象的界，删掉。装得下全部内容以后视图停在原地不动 —— Blender
// 也没有「缩到装得下就把内容钉回中间」这一步，见下面 ③。
static double clamp_timeline_pps(const App &a, double pps) {
  // 宽度用 lanes.w，不是 lanes.w 减一格：像素↔时刻的换算（x_time/time_x）用的就是 lanes.w，
  // 这道墙必须量同一段宽度，否则「一屏多少帧」在两个地方各读各的（Blender 只有一份：
  // cur 和 mask 是同一个 rect，view2d.cc:498 钳的就是画出来那一格）。
  const double view_w = double(a.lanes.w);
  if (!(view_w > 0.0) || !(a.fps() > 0.0)) return pps;
  const double per_frame = view_w * a.fps();                // 视野只剩一帧时的 pps
  const double span_frames =
      std::max(1.0, (timeline_tot_end(a) - timeline_tot_start(a)) * a.fps());
  // 队伍比 1,048,574 帧还长时以队伍为准：HOME「对齐对象所有」不许被这道墙顶回来。
  const double zoom_out_max_frames = std::max(kViewMaxFrames, span_frames);
  const double hi = per_frame / kViewMinFrames;             // 放大墙：一屏 10 帧
  const double lo = per_frame / zoom_out_max_frames;        // 缩小墙：见上一行
  return std::clamp(pps, lo, hi);
}

// 对象自己被「用到」时的反应 —— 对应 Blender 的 view2d_get →
// view2d_curRect_validate_resize（view2d.cc:388 起，绘制和取坐标都要先过这一关）。
// 按键那一头只写属性（pps / scroll_t / 一次指针请求），判断全部在这里：
//   ① 属性越过对象的策略 → 自己收回来（上面那条帧数墙）；
//   ② 请求被认下来才搬视图（越界=拒绝，视图一动不动，同 view2d_ops.cc:803-809）；
//      怎么摆也由对象定：指针在车道里钉指针、不在绕中心；
//   ③ 没有第 ③ 条。上一版这里写着「视野容得下整条内容 → 居中」，并把它记在
//      view2d.cc:684-691 名下 —— 那是误读：那一整块（:627-719「不让 cur 出 tot 的界」）
//      外面套着 `if (v2d->keeptot)`（:628），而时间线主区设的是
//      keeptot = V2D_KEEPTOT_FREE = 0（space_sequencer.cc:194，枚举 DNA_view2d_types.h:52）
//      ⇒ 整块跳过，既不屈中也不贴边。Blender 时间线因此在任何缩放档位都能中键平移，
//      包括缩到 1,048,574 帧那一档；我们那条居中把中键平移吃掉了（用户报「中键无反应」）。
//      「缩到装得下会把内容缩丢」本来就不需要这条来救：视图的界只有 ①，摆位只有 ②。
// 什么都没越界时只清请求、不动视图 —— 这就是「对象自己判断要不要反应」：同一次按键
// 落在不同的内容长度、不同窗口宽度、指针在不在车道里，反应可以是不一样的、也可以没有。
void timeline_view_react(App &a) {
  const bool asked = a.tl.zoom_req;
  a.tl.zoom_req = false;
  const double want = a.tl.pps;
  const double pps = clamp_timeline_pps(a, want);
  double scroll = a.tl.scroll_t;
  if (asked && pps == want) {
    // 请求被接受 → 由对象选支点：指针落在车道里就把请求的那一时刻钉回指针底下；
    // 落在尺子带 / scrub 带（车道以外）就绕车道中心摆。这个判断原先写在按键里。
    const double pivot = a.lanes.contains(a.tl.zoom_req_x, a.tl.zoom_req_y)
                             ? double(a.tl.zoom_req_x)
                             : double(a.lanes.x + a.lanes.w * 0.5f);
    scroll = a.tl.zoom_req_t - (pivot - a.lanes.x) / pps;
  }
  if (pps == a.tl.pps && scroll == a.tl.scroll_t) return;
  a.tl.pps = pps;
  a.tl.scroll_t = scroll;
}

// sequencer.view_all：对齐对象排出来的队（同 Blender：init_boundbox 之后还要
// expand_boundbox 到每条带，sequencer_view.cc:116-123）。pps / scroll_t 都只是写属性；
// 会不会被对象的帧数墙收回来由 timeline_view_react 判，这里不预测它。
// 两端各留一秒：Blender 的 xmargin = frames_per_second（sequencer_view.cc:422，:433-434
// 左减右加各一次）。这一档同时管住开局和 HOME —— 只给开局留、HOME 收回到 0，
// 同一个「装下对象全部」就有两套答案。
void timeline_view_all(App &a) {
  const double margin = 1.0;
  const double end = timeline_tot_end(a) + margin, begin = timeline_tot_start(a) - margin;
  const double span = std::max(a.frame_dur(), end - begin);
  float avail = a.lanes.w - btheme::u(1.0f);
  if (avail <= 0 || span <= 0) return;
  a.tl.pps = double(avail) / span;
  a.tl.scroll_t = begin;
  a.dirty |= kRLanes | kRScrub;
  timeline_view_react(a);
}

void zoom_to_selected(App &a) {
  Clip *c = props_clip(a);
  if (!c) { timeline_view_all(a); return; }
  float avail = a.lanes.w - btheme::u(2.0f);
  if (avail <= 0 || c->duration <= 0) return;
  a.tl.pps = double(avail) / c->duration;
  a.tl.scroll_t = c->timeline_start - c->duration * 0.1;
  a.project.playhead = c->timeline_start;
  a.dirty |= kRLanes | kRScrub | kRViewport;
  timeline_view_react(a);
}

// ---------------------------------------------------------------- 数据查询

Clip *find_clip(App &a, int64_t id) {
  for (auto &c : a.project.clips)
    if (c.id == id) return &c;
  return nullptr;
}

bool clip_selected(const App &a, int64_t id) {
  return std::find(a.sel.begin(), a.sel.end(), id) != a.sel.end();
}

std::vector<Clip *> selected_clips(App &a) {
  std::vector<Clip *> out;
  for (int64_t id : a.sel)
    if (Clip *c = find_clip(a, id)) out.push_back(c);
  return out;
}

// 面板作用的那条带。整个「这一组画不给谁看」的判断都从它出发，所以它必须能在只读的
// 那一趟（量高度）里被问出来 —— 写指针的来源和读指针的来源是同一处，不是两套账。
Clip *props_clip(const App &a) {
  if (a.selected < 0) return nullptr;
  for (const Clip &c : a.project.clips)
    if (c.id == a.selected) return const_cast<Clip *>(&c);
  return nullptr;
}

Clip *clip_at(const App &a, double t) {
  Clip *best = nullptr;
  for (const auto &c : a.project.clips) {
    // render_is_muted（render.cc:2240-2243）：条带自身 SEQ_MUTE 或所在通道静音都不参与
    // 合成（iterator.cc:217-224 把它们从渲染集合里去掉），下层条带因此露出。
    if (c.muted) continue;
    if (c.timeline_start > t || c.timeline_start + c.duration <= t) continue;
    if (c.track < 0) continue;
    if (c.track < int(a.project.tracks.size()) && a.project.tracks[c.track].muted) continue;
    if (!best || c.track >= best->track) best = const_cast<Clip *>(&c);
  }
  return best;
}

// 参与合成的条带集合。逐条对照 query_rendered_strips_sorted（iterator.cc:244-258）：
// 先取 intersects_frame（:167-178），再用 collection_filter_rendered_strips（:224-235）
// 滤掉声音条带与静音条带，最后按 channel 升序排；渲染就按这个顺序自下而上往画布上叠。
// 同通道的先后按起点，图面上永远不重叠。
std::vector<Clip *> rendered_clips(const App &a, double t) {
  std::vector<Clip *> out;
  for (auto &c : a.project.clips) {
    if (c.muted) continue;
    if (c.timeline_start > t || c.timeline_start + c.duration <= t) continue;
    if (c.track < 0) continue;
    if (c.track < int(a.project.tracks.size()) && a.project.tracks[c.track].muted) continue;
    // 声音条带不参与画面（iterator.cc:224 的 collection_filter_rendered_strips：
    // `strip->type == STRIP_TYPE_SOUND || render_is_muted(...)` 为真就跳过）。
    if (c.kind == kStripSound) continue;
    out.push_back(const_cast<Clip *>(&c));
  }
  std::sort(out.begin(), out.end(), [](const Clip *x, const Clip *y) {
    return x->track != y->track ? x->track < y->track : x->timeline_start < y->timeline_start;
  });
  return out;
}

// 参与发声的条带集合。Blender 是给每条**声音条带**都在 sound_scene 里挂一个 handle，
// 播放时所有 handle 一起发声（sequencer/intern/sequencer.cc:1157-1172
// strip_update_mix_sounds）；条带自身 SEQ_MUTE 或所在通道静音的把 handle 摘掉
// （:1148-1154 seq_mute_sound_strips_cb，静音位由 strip_edit.cc:102-119 递归算出）。
// 所以重叠的几条声音一起混，不是「只播最上面那条」。
// 影片条带不在这一份集合里：BKE_sound_add_scene_sound 见 strip->sound == nullptr 就返回
// （blenkernel/intern/sound.cc:945-951），而 strip->sound 只有 add_sound_strip 会赋值
// （sequencer/intern/strip_add.cc:323）⇒ 一部影片的声音归那条 SOUND 条带管，
// 裁它、挪它、删它都只影响声音。
static bool sound_eligible(const App &a, const Clip &c) {
  if (c.kind != kStripSound) return false;
  if (c.muted) return false;
  if (c.track < 0) return false;
  if (c.track < int(a.project.tracks.size()) && a.project.tracks[c.track].muted) return false;
  if (!c.info || !c.info->has_audio) return false;
  return true;
}

std::vector<Clip *> sound_clips(const App &a, double t) {
  std::vector<Clip *> out;
  for (auto &c : a.project.clips) {
    if (c.timeline_start > t || c.timeline_start + c.duration <= t) continue;
    if (!sound_eligible(a, c)) continue;
    out.push_back(const_cast<Clip *>(&c));
  }
  std::sort(out.begin(), out.end(), [](const Clip *x, const Clip *y) {
    return x->track != y->track ? x->track < y->track : x->timeline_start < y->timeline_start;
  });
  return out;
}

double project_end(const App &a) {
  double end = 0;
  for (const auto &c : a.project.clips) end = std::max(end, c.timeline_start + c.duration);
  return end;
}

// 播放范围 = BKE_scene_get_playback_range（scene.cc:2607-2613）：没开预览范围时就是
// sfra..efra。动画计时器（anim_ops.cc:593 → BKE_scene_frame_clamp_for_playback
// :2615-2632）在 cfra 越过 efra 时回到 sfra —— 也就是循环播放，一圈正好
// efra-sfra+1 帧，和导出的长度一致。
void playback_bounds(const App &a, double *t0, double *t1) {
  const OutputFormat &f = a.project.fmt;
  *t0 = a.frame_to_t(int64_t(f.frame_start));
  *t1 = a.frame_to_t(int64_t(f.frame_end) + 1);
}

void recompute_duration(App &a) { a.project.duration = project_end(a); }

static void propagate_selection(App &a, int64_t id);  // 定义在 clear_selection 后面

// 选中换了人，面板就演他那一种。和种类按钮写的是同一格 a.panel_kind（同一个意思的两个
// 来路：按按钮 = 我要看这一种，点条带 = 我看的就是这一种），读的人只有 panel_shown 和
// 那两颗按钮。id 找不到条带（清空选中）时不动它：那是「没有新答案」，不是「换成视频」。
static void panel_follow(App &a, int64_t id) {
  const Clip *c = find_clip(a, id);
  if (c) a.panel_kind = c->kind;
}

void select_only(App &a, int64_t id) {
  a.sel.clear();
  if (id >= 0) a.sel.push_back(id);
  a.selected = id;
  panel_follow(a, id);
  propagate_selection(a, id);
  trace_links(a, L"click");
  a.dirty |= kRLanes | kRProps | kRViewport;
}

void add_to_selection(App &a, int64_t id) {
  if (id < 0) return;
  if (!clip_selected(a, id)) a.sel.push_back(id);
  a.selected = id;
  panel_follow(a, id);
  propagate_selection(a, id);
  trace_links(a, L"shift-click");
  a.dirty |= kRLanes | kRProps;
}

void toggle_selection(App &a, int64_t id) {
  if (id < 0) return;
  auto it = std::find(a.sel.begin(), a.sel.end(), id);
  if (it == a.sel.end()) a.sel.push_back(id);
  else a.sel.erase(it);
  propagate_selection(a, id);
  // 活动条带永远落在被点的那条上（sequencer_select_set_active(scene, selection.strip1)，
  // :1224 无条件执行，取消选中也不换）—— 和上面 clear_selection 注释里那条约定一致。
  a.selected = id;
  panel_follow(a, id);
  trace_links(a, L"ctrl-click");
  a.dirty |= kRLanes | kRProps;
}

void clear_selection(App &a) {
  // deselect_all_strips（sequencer_select.cc:78-94）只清 STRIP_ALLSEL 标志，
  // 活动条带 ed->act_strip 由 select_active_set 单独管，取消选中不会把它抹掉。
  // 所以取消选中后左侧面板仍然落在这条带上：键选择模式（点条带下沿的菱形）就是靠
  // 这一点才能在选完键后继续拖「速度」。
  a.sel.clear();
  a.dirty |= kRLanes | kRProps;
}

// ---------------------------------------------------------------- 连接组
//
// strip_connect.cc 那套「连接」的全貌。它不改任何条带数据，唯一的机制就是选择传播：
// sequencer_select_connected_strips（sequencer_select.cc:702-718）把被点中那条的
// STRIP_ALLSEL 逐字抄给它的每个连接成员（抄标志，不是再展开一层）。因为 connect()
// （:103-118）每次都先把成员从旧组里整个摘掉、再让它指向组内所有其它成员，组永远是
// 两两相连的团 ⇒ 从任何一员出发抄一遍就覆盖全组。
// 于是「拖动/裁切/滑移跟着整组走」全是选中集合的副产品，不是各自的规则；
// 反过来，删除不传播（sequencer.delete 只删选中的，这里连选中都已经是整组了 ⇒
// 与 Blender 同：整组一起删）。

bool clip_connected(const Clip &c) {
  return !c.connections.empty();  // is_strip_connected（:131-137）
}

std::vector<Clip *> connected_clips(App &a, const Clip &c) {
  std::vector<Clip *> out;
  for (int64_t id : c.connections)
    if (Clip *o = find_clip(a, id)) out.push_back(o);
  return out;
}

// disconnect(strip)（:38-57）：先把自己从每个成员的名单里摘掉，再清空自己的名单。
// 返回「有没有真的动过」，对应算子里 CANCELLED / FINISHED 的分岔。
static bool disconnect_clip(App &a, Clip *c) {
  if (c->connections.empty()) return false;
  for (Clip *o : connected_clips(a, *c)) {
    o->connections.erase(std::remove(o->connections.begin(), o->connections.end(), c->id),
                         o->connections.end());
  }
  c->connections.clear();
  return true;
}

// cut_one_way_connections(strip)（:69-89）：只留「对方也指着我」的那些。
// 复制条带之后必须走一遍：副本的名单是从原型抄来的，原型指向老成员，副本指向新成员，
// 老成员手里的还是原型 ⇒ 不剪就留下悬空引用（sequencer.cc:630-705 的注释原话
// "One-way connections must be cut after all connections are remapped above"）。
static void cut_one_way(App &a, Clip *c) {
  c->connections.erase(
      std::remove_if(c->connections.begin(), c->connections.end(), [&](int64_t other) {
        Clip *o = find_clip(a, other);
        return !o || std::find(o->connections.begin(), o->connections.end(), c->id) ==
                         o->connections.end();
      }),
      c->connections.end());
}

// 复制出一批新条带之后，副本的连接名单还是从原型整份抄来的、指着老条带：
// strip_duplicate（sequencer.cc:620-645）= connections_duplicate 抄表 →
// seq_new_fix_links_recursive 按 strip_map 换引用（:362-366）→ 每条新带跑一遍
// cut_one_way_connections（:641 附近）。换不动的那些（原型没被一起复制，比如它没选中）
// 就成了「我指着老带、老带不指我」的单向引用，正好由这一步剪掉。不剪的话，点副本会
// 顺着单向引用把原型也选进来，移动/切分全都连带 —— 两份数据看着像一份。
// 两个循环必须分开：注释原话是 "One-way connections must be cut after all connections
// are remapped above" —— 逐条「换完就剪」会让先处理的那半看见同伴手里还指着老带，
// 于是把成对的双向引用剪成一空一实（实测 3:[] 4:[3]）。
static void remap_copied_links(App &a, const std::vector<std::pair<int64_t, int64_t>> &id_map) {
  for (const auto &pr : id_map) {
    Clip *n = find_clip(a, pr.second);
    if (!n) continue;
    for (int64_t &ref : n->connections) {
      auto m = std::find_if(id_map.begin(), id_map.end(),
                            [&](const auto &p) { return p.first == ref; });
      if (m != id_map.end()) ref = m->second;
    }
  }
  for (const auto &pr : id_map)
    if (Clip *n = find_clip(a, pr.second)) cut_one_way(a, n);
}

// connect(strip_list)（:103-118）：整组重连 —— 每个成员先彻底断开，再指向组内所有其它成员。
// 注意这是「覆盖」而不是「追加」：把 C 加进已经是团的 {A,B} 会先拆掉 A-B 再重建 A-B-C，
// 结果还是全连接的团；而选中 {A,C}（A 连着 B）点连接，B 会被踢出组。
static void connect_clips(App &a, const std::vector<Clip *> &group) {
  for (Clip *c : group) disconnect_clip(a, c);
  for (Clip *c : group) {
    for (Clip *o : group)
      if (o != c) c->connections.push_back(o->id);
    // 名单按 id 排好：同一组的两种点选顺序（先点 A 再点 B / 框选）会给出不同的
    // group 顺序，不排序的话连上之后名单内容一样但次序不同，撤消比较会误判成「改了数据」。
    std::sort(c->connections.begin(), c->connections.end());
  }
}

// are_strips_connected_together（:139-155）：每个成员的名单正好是组内其它全部 ⇒ 已经是团。
// 这就是「连接」菜单项（toggle=True，sequencer_edit.cc:1508-1514）第二次点时改为断开的前提。
// 只选一条时 Blender 也算「已经是团」（expected = size-1 = 0，空名单正好等于 0），于是走
// 断开分支而 disconnect 又返回 false ⇒ 报「没有可断开的」。这里不加 size 特判就是为了
// 停在同一条状态文字上：单人连接不写任何数据，也不说「已连接 1 条条带」。
static bool are_connected_together(const std::vector<Clip *> &group) {
  for (const Clip *c : group) {
    if (c->connections.size() != group.size() - 1) return false;
    for (int64_t id : c->connections) {
      bool in_group = false;
      for (const Clip *o : group) in_group |= (o != c && o->id == id);
      if (!in_group) return false;
    }
  }
  return true;
}

// 选中一条 = 连带选中它的整个组（见上面那段）。deselect 方向同样传播：Blender 抄的是标志，
// 所以 Shift 点掉组里的一条会同时点掉全组。
static void propagate_selection(App &a, int64_t id) {
  Clip *c = find_clip(a, id);
  if (!c) return;
  const bool on = clip_selected(a, id);
  for (int64_t other : c->connections) {
    if (other == id) continue;
    if (on) {
      if (!clip_selected(a, other)) a.sel.push_back(other);
    } else {
      a.sel.erase(std::remove(a.sel.begin(), a.sel.end(), other), a.sel.end());
    }
  }
}

// ---------------------------------------------------------------- 撤消 / 重做
//
// 快照 = 工程数据 + 标记 + 选择，一次用户动作一份；next_id 不回退（新条带的编号只会
// 往前走，撤消回来的那些 id 早就用过了，不会撞）。

// user.undosteps 的默认值（DNA_userdef_types.h:1144）。
static const size_t kUndoSteps = 32;

// 关键帧曲线的逐字段比较。撤消链靠 snap_eq 判断「这一步到底改没改东西」，
// 少了这一段，只动 keyframe 的编辑会被当成无操作而丢掉一步。
static bool curves_eq(const FCurve &x, const FCurve &y) {
  if (x.keys.size() != y.keys.size() || x.hold_endpoints != y.hold_endpoints ||
      x.accel_smooth != y.accel_smooth)
    return false;
  for (size_t i = 0; i < x.keys.size(); i++) {
    const Key &a = x.keys[i], &b = y.keys[i];
    if (a.x != b.x || a.y != b.y || a.h1x != b.h1x || a.h1y != b.h1y || a.h2x != b.h2x ||
        a.h2y != b.h2y || a.interp != b.interp || a.h1 != b.h1 || a.h2 != b.h2 ||
        a.locked_final != b.locked_final)
      return false;
  }
  return true;
}

static bool clip_eq(const Clip &x, const Clip &y) {
  for (int i = 0; i < kAnimCount; i++)
    if (!curves_eq(x.anim[i], y.anim[i])) return false;
  if (x.retiming.size() != y.retiming.size()) return false;
  for (size_t i = 0; i < x.retiming.size(); i++)
    if (x.retiming[i].strip_frame != y.retiming[i].strip_frame ||
        x.retiming[i].src != y.retiming[i].src)
      return false;
  // 连接名单也在撤消口径内：connect/disconnect 改的就是它，漏掉这一步这两步会被
  // snap_eq 判成「什么都没改」而不出栈。
  if (x.connections != y.connections) return false;
  return x.source_path == y.source_path && x.id == y.id && x.track == y.track &&
         x.timeline_start == y.timeline_start && x.source_in == y.source_in &&
         x.duration == y.duration &&
         x.muted == y.muted && x.locked == y.locked && x.show_retiming == y.show_retiming &&
         x.ofs_x == y.ofs_x && x.ofs_y == y.ofs_y && x.scale_x == y.scale_x &&
         x.scale_y == y.scale_y && x.rotation == y.rotation && x.origin_x == y.origin_x &&
         x.origin_y == y.origin_y && x.flip_x == y.flip_x && x.flip_y == y.flip_y &&
         x.crop_l == y.crop_l && x.crop_r == y.crop_r &&
         x.crop_t == y.crop_t && x.crop_b == y.crop_b && x.label == y.label;
}

static bool fmt_eq(const OutputFormat &x, const OutputFormat &y) {
  return x.width == y.width && x.height == y.height && x.fps == y.fps &&
         x.fps_base == y.fps_base && x.par_x == y.par_x && x.par_y == y.par_y &&
         x.scale_percent == y.scale_percent && x.frame_start == y.frame_start &&
         x.frame_end == y.frame_end && x.frame_step == y.frame_step;
}

static bool snap_eq(const Snapshot &x, const Snapshot &y) {
  if (x.project.chan_count != y.project.chan_count || !fmt_eq(x.project.fmt, y.project.fmt) ||
      x.project.tracks.size() != y.project.tracks.size() ||
      x.project.clips.size() != y.project.clips.size())
    return false;
  for (size_t i = 0; i < x.project.tracks.size(); ++i) {
    const Track &p = x.project.tracks[i], &q = y.project.tracks[i];
    if (p.index != q.index || p.name != q.name || p.muted != q.muted || p.locked != q.locked)
      return false;
  }
  for (size_t i = 0; i < x.project.clips.size(); ++i)
    if (!clip_eq(x.project.clips[i], y.project.clips[i])) return false;
  return x.sel == y.sel && x.selected == y.selected;
}

static Snapshot snap_of(const App &a) {
  Snapshot s;
  s.project = a.project;
  s.sel = a.sel;
  s.retime_sel = a.retime_sel;
  s.selected = a.selected;
  return s;
}

static void snap_apply(App &a, const Snapshot &s) {
  a.project = s.project;
  a.sel = s.sel;
  a.retime_sel = s.retime_sel;
  a.selected = s.selected;
  recompute_duration(a);
  layout(a);
  sync_preview(a);
  sync_audio(a);
  a.dirty = kRegionAll;
}

void undo_reset(App &a) {
  a.undo.clear();
  a.undo.push_back(snap_of(a));
  a.undo_cur = 0;
}

void undo_push(App &a, const wchar_t *name) {
  if (a.undo.empty()) {
    // 链还空着 = 这一步之前没有任何基线，先按当前状态建基线（命令行导入就是这种：
    // 它带来的那条是出厂画面，不该被算成一次可撤消的动作）。
    undo_reset(a);
    return;
  }
  Snapshot s = snap_of(a);
  if (snap_eq(s, a.undo[a.undo_cur]))
    return;  // 空动作不占一步，否则撤消要按两次才看得见
  s.name = name ? name : L"";
  // 新动作丢掉 redo 尾巴（Blender：ED_undo_push 之后 redo 链就断了）。
  a.undo.erase(a.undo.begin() + long(a.undo_cur + 1), a.undo.end());
  a.undo.push_back(std::move(s));
  a.undo_cur = a.undo.size() - 1;
  if (a.undo.size() > kUndoSteps + 1) {
    a.undo.erase(a.undo.begin());
    --a.undo_cur;
  }
}

bool undo_step(App &a) {
  if (a.undo.empty() || a.undo_cur == 0) return false;
  const std::wstring name = a.undo[a.undo_cur].name;
  snap_apply(a, a.undo[--a.undo_cur]);
  set_status(a, name.empty() ? L"已撤消" : L"已撤消：" + name);
  return true;
}

bool redo_step(App &a) {
  if (a.undo.empty() || a.undo_cur + 1 >= a.undo.size()) return false;
  ++a.undo_cur;
  const std::wstring name = a.undo[a.undo_cur].name;
  snap_apply(a, a.undo[a.undo_cur]);
  set_status(a, name.empty() ? L"已重做" : L"已重做：" + name);
  return true;
}

// ---------------------------------------------------------------- 时间格式

static std::wstring num_str(double v, int precision) {
  wchar_t buf[64];
  if (precision <= 0) swprintf(buf, 64, L"%d", int(std::lround(v)));
  else swprintf(buf, 64, L"%.*f", precision, v);
  return buf;
}

// 时间字符串（time_str）在 draw.cpp：那里才有 BLI_timecode_string_from_time 的逐行移植，
// 与标尺刻度、播放头框共用同一套格式化规则。

// ---------------------------------------------------------------- RNA 属性表
//
// 区间全部照抄 makesrna：
//   rna_scene.cc:7351/7362  resolution_x/y  PROP_INT  range(4, 65536)
//   rna_scene.cc:7369       resolution_percentage  ui_range(1, 100, 10, 1)
//   rna_scene.cc:7383/7393  pixel_aspect_x/y  PROP_FLOAT range(1, 200)
//   rna_scene.cc:7428       fps  PROP_INT range(1, SHRT_MAX)  ui_range(1, 240, 1, -1)
//   rna_scene.cc:7436       fps_base  PROP_FLOAT range(1e-5, 1e6)  ui_range(0.1, 120, 2, 3)
//     —— 注释写明预设会把它设成 1.001，所以 23.976 = 24 / 1.001，而不是 24000/1001。
//   rna_sequencer.cc:2209+  crop：min_x=Left、max_x=Right、max_y=Top、min_y=Bottom，
//                 PROP_INT/Pixel，ui_range(0, 4096, 1, -1)
//   rna_sequencer.cc:2276+  scale_x/y ui_range(0, FLT_MAX, 3, 3) 默认 1.0
//   rna_sequencer.cc:2290+  offset_x/y PROP_PIXEL ui_range(-FLT_MAX, FLT_MAX, 100, 3)
//   rna_sequencer.cc:2310+  origin ui_range(0, 1, 1, 3)
//   rna_scene.cc:9376-9390 frame_start/frame_end PROP_INT/PARAM_TIME，
//                 range(MINFRAME, MAXFRAME)、无 ui_range
//   rna_scene.cc:9392-9400 frame_step PROP_INT/PARAM_TIME，range(0, MAXFRAME)、
//                 ui_range(1, 100, 1, -1)
const PropSpec &prop_spec(int id) {
  const double big = 1e30;
  static const PropSpec res{4, 65536, 4, 65536, 1, 0, false};
  static const PropSpec pct{1, 100, 1, SHRT_MAX, 10, 0, false};
  // 宽高比：pixel_aspect_x/y 是 PROP_FLOAT、range(1,200)（rna_scene.cc:7383/7393），
  // 没声明 precision/step ⇒ 用浮点默认 precision=3、step=10（rna_define.cc:1272-1273），
  // 所以 Blender 界面显示 1.000。
  static const PropSpec asp{1, 200, 1, 200, 10, 3, true};
  static const PropSpec fps{1, 240, 1, SHRT_MAX, 1, 0, false};
  static const PropSpec fps_base{0.1, 120, 1e-5, 1e6, 2, 3, true};
  static const PropSpec frame{0, big, 0, big, 1, 0, false};
  // 步长：RNA_def_property_range(prop, 0, MAXFRAME) + ui_range(1, 100, 1, -1)
  // （rna_scene.cc:9395-9396），MAXFRAME = 1048574（DNA_scene_types.h:3050）。
  // 软下限 1 只约束滑块与拖拽，键入 0 走的是硬区间 ⇒ 下面写入处再按版本迁移折回 1。
  static const PropSpec step{1, 100, 0, 1048574, 1, 0, false};
  // 码率：RNA 只给了单位（"Video bitrate (kbit/s)"），没声明 range，所以整数默认区间就是
  // 全幅；这里只把负数挡在 0（负 bit_rate 到 avcodec 那边没有意义），上限不设。
  static const PropSpec bitrate{0, big, 0, big, 1, 0, false};
  static const PropSpec ofs{-big, big, -big, big, 100, 3, true};
  static const PropSpec scale{0, big, 0, big, 3, 3, true};
  static const PropSpec rot{-big, big, -big, big, 5, 2, true};
  static const PropSpec origin{0, 1, 0, 1, 1, 3, true};
  static const PropSpec crop{0, 4096, 0, 4096, 1, 0, false};
  // 速度：RNA_def_float("speed", 100.0f, 0.001f, FLT_MAX, …, 0.1f, FLT_MAX)
  // （sequencer_retiming.cc:701-709）—— 百分数，硬下限 0.001，软下限 0.1，无上限。
  // 未声明 precision ⇒ Blender 的浮点默认 2 位小数。
  static const PropSpec speed{0.1, big, 0.001, big, 10, 2, true};
  // 文本样式（TextVars）。字号在 Blender 里就是输出像素（text_effect_line_size_get 只按
  // 预览渲染比例折算一次），所以这里给整数、下限 1：填到 0 以下排版器直接不出图（那是它
  // 自己的门，不是界面替它设的界）。
  // 这四行的上限是裁决定的（字号 300、行距 3、轮廓宽 0.2、边距 0.1），不是 RNA 给的：
  // Blender 那边这四个属性都是到 FLT_MAX。上限同时就是那一格进度条的两端 —— 滑块没有
  // 上限可画，所以定上限和加进度条是同一笔。键入超界的数被夹回上限，不报错也不越界。
  static const PropSpec t_size{1, 300, 1, 300, 10, 0, false};
  // 行距 = 自然行高的倍数（DWrite 的 PROPORTIONAL）。
  static const PropSpec t_line{0.1, 3, 0.1, 3, 10, 2, true};
  // 轮廓宽、方框边距、方框圆度都是相对量：轮廓按行高、边距按画布宽、圆度按盒高的一半
  // （三处算式都在 text.cpp，逐条抄 Blender）。界面上就按原值给，不换成像素 —— 换了
  // 就是同一个数两处各有意思。
  static const PropSpec t_outline{0, 0.2, 0, 0.2, 10, 3, true};
  static const PropSpec t_margin{0, 0.1, 0, 0.1, 10, 3, true};
  static const PropSpec t_round{0, 1, 0, 1, 10, 3, true};
  // 颜色：RGB 按 0..255 给（对象里存的是 0..1 的浮点，读写各折算一次），不透明度按百分数。
  static const PropSpec t_rgb{0, 255, 0, 255, 10, 0, false};
  static const PropSpec t_alpha{0, 100, 0, 100, 10, 0, false};
  switch (id) {
    case kFResX: case kFResY: return res;
    case kFResPct: return pct;
    case kFAspX: case kFAspY: return asp;
    case kFFps: return fps;
    case kFFpsBase: return fps_base;
    // 帧范围：RNA_def_property_range(MINFRAME, MAXFRAME)（rna_scene.cc:9380/:9388），
    // 没有 ui_range ⇒ 整数、无小数位。本工程时间从 0 帧起算，下限取 0。
    case kFFrameStart: case kFFrameEnd: return frame;
    case kFFrameStep: return step;
    case kFBitrate: return bitrate;
    case kFOfsX: case kFOfsY: return ofs;
    case kFScaleX: case kFScaleY: return scale;
    case kFRotation: return rot;
    case kFOriginX: case kFOriginY: return origin;
    case kFCropMinX: case kFCropMaxX: case kFCropMinY: case kFCropMaxY: return crop;
    case kFSpeed: return speed;
    case kFTextSize: return t_size;
    case kFTextLineSpacing: return t_line;
    case kFTextOutlineWidth: return t_outline;
    case kFTextBoxMargin: return t_margin;
    case kFTextBoxRoundness: return t_round;
    case kFGlyphR: case kFGlyphG: case kFGlyphB:
    case kFOutlineR: case kFOutlineG: case kFOutlineB:
    case kFBoxR: case kFBoxG: case kFBoxB: return t_rgb;
    case kFGlyphA: case kFOutlineA: case kFBoxA: return t_alpha;
    default: return frame;
  }
}

// 面板/快捷键的速度读数 = strip_speed_get（sequencer_retiming.cc:581-603）。两种模式：
//  - 条带模式（没有键被选中）：读活动条带「右把手那把键」收尾的那一段，即最后一段；
//    条带没有键 ⇒ 1.0（:586 的 retiming_has_keys 判断）。
//  - 键选中模式：只读选中的那把键收尾的那一段；选中的不是一把 ⇒ 1.0（:596-600）。
double strip_speed_get(App &a, const Clip *c) {
  if (retiming_keys_are_selected(a)) {
    if (a.retime_sel.size() != 1) return 1.0;
    const Clip *owner = find_clip(a, a.retime_sel[0].first);
    if (!owner || a.retime_sel[0].second >= owner->retiming.size()) return 1.0;
    return retiming_key_speed(*owner, a.retime_sel[0].second);
  }
  if (!c || c->retiming.empty()) return 1.0;
  return retiming_key_speed(*c, c->retiming.size() - 1);
}

// 文本那十二格颜色 = 三层（字形 / 轮廓 / 方框）× 四通道（R G B A）：换算在 app.h 的
// text_layer / text_color_at，读写和画色块都问同一处。

// 文本行落到哪一份 TextVars：见 app.h 的声明。这一句是那个问题唯一的答案。
TextVars &text_target(App &a, Clip *c) {
  return c && c->kind == kStripText ? c->text : a.text_def;
}

double prop_value(App &a, int id, Clip *c) {
  const OutputFormat &f = a.project.fmt;
  switch (id) {
    case kFResX: return f.width;
    case kFResY: return f.height;
    case kFResPct: return f.scale_percent;
    case kFAspX: return f.par_x * 1.0;
    case kFAspY: return f.par_y * 1.0;
    case kFFps: return f.fps;
    case kFFpsBase: return f.fps_base;
    case kFFrameStart: return f.frame_start;
    case kFFrameEnd: return f.frame_end;
    case kFFrameStep: return f.frame_step;
    case kFBitrate: return f.bitrate;
    default: break;
  }
  // 文本那几格（数值 + 颜色）不挑条带：面板演文本这一种时读的是 text_target，没有文本带
  // 就是 a.text_def。这一问排在「没有条带就返回 0」之前：排在其后就等于两处各自回答一次
  // 「没有」，面板会把默认值显示成 0。
  if (is_text_field(id) && id != kFTextContent) {
    TextVars &t = text_target(a, c);
    if (is_color_field(id)) {  // 行列由 id 算出（见 text_color_at）
      int layer = 0, ch = 0;
      double sc = 1.0;
      text_color_at(id, &layer, &ch, &sc);
      return double(text_layer(t, layer)[ch]) * sc;
    }
    switch (id) {
      case kFTextSize: return t.size;
      case kFTextLineSpacing: return t.line_spacing;
      case kFTextOutlineWidth: return t.outline_width;
      case kFTextBoxMargin: return t.box_margin;
      case kFTextBoxRoundness: return t.box_roundness;
      default: break;
    }
    return 0.0;
  }
  if (!c) {
    // 键选中模式的速度读的是选中的键，跟面板落到哪条带无关（strip_speed_get 的
    // retiming 分支：:594-600），所以这里可以没有条带。
    if (id == kFSpeed && retiming_keys_are_selected(a)) return strip_speed_get(a, nullptr) * 100.0;
    return 0.0;
  }
  // 变换 5 个通道：挂了曲线就读当前帧的求值值。Blender 的 AnimSys 每帧把求值结果
  // 写回 Strip 本身（anim_sys.cc:574-597），面板的 getter 拿到的就是动画之后的数，
  // 所以这里不是「额外加的一层」，而是同一个读取口。
  if (id >= kFOfsX && id <= kFRotation) {
    const FCurve &fcu = c->anim[id - kFOfsX];
    if (!fcu.empty()) return fcu.eval(double(a.cur_frame()));
  }
  switch (id) {
    case kFOfsX: return c->ofs_x;
    case kFOfsY: return c->ofs_y;
    case kFScaleX: return c->scale_x;
    case kFScaleY: return c->scale_y;
    case kFRotation: return c->rotation;
    case kFOriginX: return c->origin_x;
    case kFOriginY: return c->origin_y;
    case kFCropMinX: return c->crop_l;   // crop.min_x = “Left”（rna_sequencer.cc:2221）
    case kFCropMaxX: return c->crop_r;   // crop.max_x = “Right”
    case kFCropMaxY: return c->crop_t;   // crop.max_y = “Top”（:2211）
    case kFCropMinY: return c->crop_b;   // crop.min_y = “Bottom”（:2217）
    // 面板按 Blender 的 speed 属性显示百分数，读数走 strip_speed_get（:581-603）：
    // 条带模式 = 右把手那把键收尾的那一段（最后一段），键选中模式 = 选中那把键收尾的段。
    case kFSpeed: return strip_speed_get(a, c) * 100.0;
    default: break;
  }
  return 0.0;
}

std::wstring prop_text(App &a, int id, Clip *c) {
  // 内容那一格显示的就是串本身：没有单位、不做数值换算。
  if (is_string_field(id)) return prop_string(a, c);
  const PropSpec &s = prop_spec(id);
  double v = prop_value(a, id, c);
  std::wstring t = num_str(v, s.precision);
  switch (id) {
    case kFResPct: t += L"%"; break;
    case kFSpeed: t += L"%"; break;
    case kFRotation: t += L"°"; break;
    case kFFps: case kFFpsBase: t += L" fps"; break;
    case kFBitrate: t += L" kbit/s"; break;  // RNA 的单位就写在这句话里（"Video bitrate (kbit/s)"）
    // 不透明度给百分数（对象里存的是 0..1 的 alpha，读写各折算一次）。
    case kFGlyphA: case kFOutlineA: case kFBoxA: t += L"%"; break;
    default: break;
  }
  return t;
}

std::wstring prop_string(App &a, Clip *c) { return text_target(a, c).content; }

// 内容那一行的写入。空串是合法值：排版器读它判「这一帧没有画面」，和 Blender 的
// early_out_text 同一道门，所以清空 = 字真的没了，不是「什么都没填所以不算」。
void text_content_set(App &a, const std::wstring &s) {
  text_target(a, props_clip(a)).content = s;
  a.dirty |= kRViewport | kRProps;
  sync_preview(a);
}

// 变换的一个通道怎么写：挂了曲线就在当前帧落键，没曲线就写标量。
// XY 成对锁定时要用它把同一行写两次，所以把「曲线还是标量」这段判断收成一个函数。
static void xform_write(App &a, Clip *c, int id, double v) {
  FCurve &fcu = c->anim[id - kFOfsX];
  if (!fcu.empty()) {
    const bool wrote = fcu.set_at_if_needed(double(a.cur_frame()), float(v));
    trace_keys(a, *c, wrote ? L"panel->key" : L"panel->skip");
    return;
  }
  switch (id) {
    case kFOfsX: c->ofs_x = v; break;
    case kFOfsY: c->ofs_y = v; break;
    case kFScaleX: c->scale_x = v; break;
    case kFScaleY: c->scale_y = v; break;
    default: c->rotation = v; break;
  }
}

// 这一行的成对伙伴，只有锁着才算伙伴：位置锁管位置X/Y，缩放锁管缩放X/Y；旋转、速度没有伙伴。
// 「伙伴是谁、什么时候才算伙伴」只有这一处答案，改值和打关键帧都照它办，否则同一个承诺会有
// 两套说法（数值行两轴一起写、菱形却只给一轴挂键，锁就只管一半）。
static int locked_partner(const App &a, int field) {
  switch (field) {
    case kFOfsX:
      return a.lock_pos ? kFOfsY : 0;
    case kFOfsY:
      return a.lock_pos ? kFOfsX : 0;
    case kFScaleX:
      return a.lock_scale ? kFScaleY : 0;
    case kFScaleY:
      return a.lock_scale ? kFScaleX : 0;
    default:
      return 0;
  }
}

bool prop_apply(App &a, int id, Clip *c, double v, bool pair) {
  const PropSpec &s = prop_spec(id);
  if (!s.is_float) v = std::round(v);
  // 键入的取值只受 RNA 硬区间约束（softmin/softmax 仅限制滑块与拖拽映射，
  // 见 interface_handlers.cc 的 ui_numedit_apply_softminmax）；fps_base 需要能填 1001。
  v = std::clamp(v, s.hard_min, s.hard_max);
  OutputFormat &f = a.project.fmt;
  switch (id) {
    case kFResX: f.width = int(v); break;
    case kFResY: f.height = int(v); break;
    case kFResPct: f.scale_percent = int(v); break;
    case kFAspX: f.par_x = v; break;
    case kFAspY: f.par_y = v; break;
    case kFFps: f.fps = int(v); break;                 // frs_sec（PROP_INT）
    case kFFpsBase: f.fps_base = v > 0 ? v : 1.0; break;  // frs_sec_base（PROP_FLOAT）
    // 帧范围：rna_Scene_start_frame_set（rna_scene.cc:1080-1090）先夹进
    // [MINFRAME, MAXFRAME]，再在超过 efra 时把 efra 顶上去；end_frame_set（:1092-1100）
    // 对称。两者都会顺带刷新整个工程。
    case kFFrameStart:
      f.frame_start = int(v);
      if (f.frame_end < f.frame_start) f.frame_end = f.frame_start;
      break;
    case kFFrameEnd:
      f.frame_end = int(v);
      if (f.frame_start > f.frame_end) f.frame_start = f.frame_end;
      break;
    // 步长：硬区间容 0（rna_scene.cc:9395），但 0 在 Blender 只出现在老文件里，
    // 加载时就被折回 1（versioning_250.cc:1014-1016）——渲染循环靠它前进
    // （pipeline.cc:2547 的 nfra += tfra），0 等于永不推进。
    case kFFrameStep:
      f.frame_step = std::max(1, int(v));
      break;
    case kFBitrate: f.bitrate = int(v); break;
    default: break;
  }
  if (id <= kFProjectLast) {  // 格式类属性：画布形状、导出参数、时间线全长全变
    if (trace_fp && (id == kFFrameStart || id == kFFrameEnd || id == kFFrameStep)) {
      wchar_t tb[96];
      swprintf(tb, 96, L"frame-range 开始=%d 结束=%d 步长=%d", f.frame_start, f.frame_end,
               f.frame_step);
      trace(tb);
    }
    a.dirty = kRegionAll;
    sync_preview(a);
    return true;
  }
  // 键选中模式（Blender 的 retiming mode）：走 segment_speed_set
  // （sequencer_retiming.cc:622-638），对每把选中的键只改「它收尾的那一段」。关键差别是
  // 这里不清空曲线 —— :609 那句注释说的「只有显式按段变速时才保留已有键」就是这个分支。
  if (id == kFSpeed && retiming_keys_are_selected(a)) {
    const double ns = std::max(1e-5, v / 100.0);
    for (const auto &s : a.retime_sel) {
      Clip *k = find_clip(a, s.first);
      if (!k || s.second >= k->retiming.size()) continue;
      retiming_set_key_speed(*k, s.second, ns, a.frame_dur());
      trace_retime(a, *k, L"seg-speed");
    }
    // 改完可能有多出来的键已经不存在（collapse_if_identity 会折回空曲线），把失效下标清掉，
    // 别让后面的 R / X 指向一颗不存在的键。
    retiming_prune_sel(a);
    recompute_duration(a);
    a.dirty |= kRViewport | kRProps | kRLanes;
    sync_preview(a);
    return true;
  }
  // 文本样式行：落点由 text_target 回答 —— 选中的是文本带就写它自己，否则写 a.text_def
  // （新建文本带继承的那一份）。种类按钮放开以后「面板演文本、手上没有文本带」是常态，
  // 所以这一行不再有「先选中一条文本」那种拒绝；它排在 !c 之前，否则空手时写不进去、
  // 读数却读得到，就成了同一个值两处各有答案。
  if (is_text_field(id)) {
    TextVars &t = text_target(a, c);
    if (is_color_field(id)) {
      int layer = 0, ch = 0;
      double sc = 1.0;
      text_color_at(id, &layer, &ch, &sc);
      text_layer(t, layer)[ch] = float(v / sc);  // 界面上的 0..255 / 0..100 折回 0..1
    } else {
      switch (id) {
        case kFTextSize: t.size = v; break;
        case kFTextLineSpacing: t.line_spacing = v; break;
        case kFTextOutlineWidth: t.outline_width = v; break;
        case kFTextBoxMargin: t.box_margin = v; break;
        case kFTextBoxRoundness: t.box_roundness = v; break;
        default: return false;
      }
    }
    a.dirty |= kRViewport | kRProps;
    sync_preview(a);
    return true;
  }
  if (!c) return false;
  // 变换的五个可动画通道。曲线非空 ⇒ 这个通道已经是动画的：改值 = 在当前帧插/替换一个键，
  // 走 Blender 的 INSERTKEY_NEEDED 判定（见 fcurve.cpp 的 set_at_if_needed）。标量之后再也没人读，
  // 因为 AnimSys 每帧用求值值覆盖 Strip 里的常量（anim_sys.cc:574-597），面板和渲染看到的是
  // 同一个数。曲线为空 ⇒ 照旧写标量，等同没挂 FCurve 的属性。
  //
  // XY 成对锁定是本工程自加的。partner 的取值必须在写本行之前算出来：位置锁的是「同样的
  // 增量」，增量的基准是旧值，写完再读就读到改过的数了。
  // pair = 这一击只动了一根轴（拖数值按钮、敲数值、方向键挪一格都是）。预览里拖取景框和
  // 方向键本来就是一次一轴/两轴齐动，所以它们不要求补另一轴，传 false。
  if (id >= kFOfsX && id <= kFRotation) {
    const int partner = pair ? locked_partner(a, id) : 0;
    double pv = 0.0;
    if (partner == kFScaleX || partner == kFScaleY) pv = v;  // 缩放锁 = 两轴同一个值
    else if (partner) pv = prop_value(a, partner, c) + (v - prop_value(a, id, c));
    xform_write(a, c, id, v);
    if (partner) xform_write(a, c, partner, pv);
    if (trace_fp) {
      wchar_t tb[160];
      swprintf(tb, 160,
               L"xform set id=%d v=%.3f pair=%d lock=%d/%d -> ofs=%.2f,%.2f scale=%.4f,%.4f", id,
               v, int(pair), int(a.lock_pos), int(a.lock_scale), c->ofs_x, c->ofs_y, c->scale_x,
               c->scale_y);
      trace(tb);
    }
    a.dirty |= kRViewport | kRProps | kRLanes;
    sync_preview(a);
    return true;
  }
  switch (id) {
    // transform.origin：RNA 是 ui_range(0, 1, 1, 3)（rna_sequencer.cc:2310 起），
    // 归一化到图片尺寸，(0,0)=左下角、(0.5,0.5)=中心。
    case kFOriginX: c->origin_x = v; break;
    case kFOriginY: c->origin_y = v; break;
    case kFCropMinX: c->crop_l = int(v); break;
    case kFCropMaxX: c->crop_r = int(v); break;
    case kFCropMaxY: c->crop_t = int(v); break;
    case kFCropMinY: c->crop_b = int(v); break;
    // 速度（百分数）：Blender 的 strip 模式走 strip_speed_set（sequencer_retiming.cc:605-620）
    // —— 先 retiming_reset 把已有键全清掉（那里的注释原文：「Overwrite existing keys, since
    // these hidden/visible keys just add noise when the user wants to retime the whole strip」），
    // 再 ensure 出两端键、把右端那把的速度设成请求值（retiming_key_speed_set:244）。
    // 段吃掉的源内容不变，条带长度 = 内容/speed 并 round 到整帧
    // （retiming_key_new_frame_from_speed_get:221-242）。
    // 作用范围 = 点中的这条 + 它的连接成员（非锁定的），不是单挑 active。音视频是同一条带
    // 拆出来的两半、导入即成组；只改 active 就留下面不动、声音拉长的裂缝。Blender 的 strip 模式
    // 本就对 selected_strips_from_context 全体调 strip_speed_set（sequencer_retiming.cc:647-651），
    // 连接让整组同时入选 ⇒ 整组一起变；这里用同一处的连接名单把那一格补回来。
    case kFSpeed: {
      std::vector<Clip *> group = {c};
      for (Clip *o : connected_clips(a, *c))
        if (!o->locked) group.push_back(o);
      const double ns = std::max(1e-5, v / 100.0);
      bool did = false;
      for (Clip *m : group) {
        if (!retiming_is_allowed(*m, a.frame_dur())) continue;  // :648 的 remove_if
        if (std::fabs(retiming_speed_at(*m, m->duration) - ns) <= 1e-9) continue;
        retiming_reset(*m);
        retiming_set_speed(*m, m->duration, ns, a.frame_dur());
        trace_retime(a, *m, L"panel-speed");
        did = true;
      }
      if (!did) break;
      // reset 把整条曲线清了 ⇒ 之前按下的键（如果有）都不存在了。
      retiming_prune_sel(a);
      recompute_duration(a);
      break;
    }
    default: return false;
  }
  a.dirty |= kRViewport | kRProps | kRLanes;
  sync_preview(a);
  return true;
}

// 属性行上那颗锁按钮。X 和 Y 两行共用一把锁：点哪一行都切同一对
// （成对锁定的意义就是「这两行别再各走各的」，分两个按钮反而要多点一次）。
void xform_lock_toggle(App &a, int field) {
  if (field == kFOfsX || field == kFOfsY) {
    a.lock_pos = !a.lock_pos;
    set_status(a, a.lock_pos ? L"位置 X/Y 已锁定：改一轴，另一轴跟同样的增量"
                             : L"位置 X/Y 已解锁：两轴各改各的");
  } else if (field == kFScaleX || field == kFScaleY) {
    a.lock_scale = !a.lock_scale;
    set_status(a, a.lock_scale ? L"缩放 X/Y 已锁定：两轴同一个值（不变形）"
                              : L"缩放 X/Y 已解锁：可以只拉一轴");
  } else {
    return;
  }
  a.dirty |= kRProps;
}

// 属性行的关键帧状态位（kKeyAnimated / kKeyAtFrame，声明在 app.h）。
int key_field_state(App &a, int field) {
  Clip *c = props_clip(a);
  if (!c) return 0;
  if (field == kFSpeed) {
    // 速度行背后那条「曲线」就是变速键数组：有键 = 挂了曲线（粗空心），
    // 当前帧正好落在某把键上 = 实心。空数组是恒速 1×，画细空心。
    if (c->retiming.empty()) return 0;
    int st = kKeyAnimated;
    const double x = a.project.playhead - c->timeline_start;
    for (const RetimeKey &k : c->retiming)
      if (std::fabs(k.strip_frame - x) <= 0.0001) {
        st |= kKeyAtFrame;
        break;
      }
    return st;
  }
  if (field < kFOfsX || field > kFRotation) return 0;
  const FCurve &fcu = c->anim[field - kFOfsX];
  if (fcu.empty()) return 0;
  int st = kKeyAnimated;
  const double fr = double(a.cur_frame());
  for (const Key &k : fcu.keys)
    if (std::fabs(k.x - fr) <= 0.0001) {
      st |= kKeyAtFrame;
      break;
    }
  return st;
}

// 一轴的一条关键帧命令：往这一行的曲线上写/删/清。伙伴在不在它管辖里由调用那一层决定。
static void keyframe_cmd_one(App &a, Clip *c, int field, int cmd) {
  const int ch = field - kFOfsX;
  FCurve &fcu = c->anim[ch];
  const double fr = double(a.cur_frame());
  switch (cmd) {
    // 手动 keying 的默认 flag 里不带 INSERTKEY_NEEDED：get_keyframing_flags
    // （keyframing.cc:296）查的是 MANUALKEY_FLAG_INSERTNEEDED，而 UserDef 默认只给了
    // AUTOKEY_FLAG_INSERTNEEDED（DNA_userdef_types.h:1245）。所以这两项是无条件写键。
    case kCmdKeyInsert:
    case kCmdKeyReplace:
      fcu.set_at(fr, float(prop_value(a, field, c)));
      break;
    case kCmdKeyDelete:
      fcu.remove_at(fr);
      break;
    case kCmdKeyClear: {
      // 清除关键帧 = 整条曲线没了，属性停在最后求值到的那个数上（Blender 的 DNA 值
      // 就是 AnimSys 上一帧写进去的值）。曲线空掉之后面板回到读标量。
      double *scalar[kAnimCount] = {&c->ofs_x, &c->ofs_y, &c->scale_x, &c->scale_y, &c->rotation};
      *scalar[ch] = fcu.empty() ? *scalar[ch] : double(fcu.eval(fr));
      fcu = FCurve{};
      break;
    }
    default: return;
  }
}

// 右键属性行菜单的四项，对应 ANIM_OT_keyframe_insert_button / _delete_button /
// _clear_button（interface_context_menu.cc:639-719 决定哪几项出现）。
void keyframe_menu_cmd(App &a, int cmd) {
  Clip *c = props_clip(a);
  if (!c || a.key_field < kFOfsX || a.key_field > kFRotation) {
    set_status(a, L"这一行没有可打关键帧的属性");
    return;
  }
  keyframe_cmd_one(a, c, a.key_field, cmd);
  // XY 锁着 ⇒ 这一行的菱形和菜单管的是「这对轴」，两轴一起挂键、一起删、一起清。只给一轴挂
  // 的话另一轴还停在标量上：曲线一动它就纹丝不动，锁管住了写数值、管不住动画。
  const int partner = locked_partner(a, a.key_field);
  if (partner) keyframe_cmd_one(a, c, partner, cmd);
  trace_keys(a, *c, cmd == kCmdKeyDelete   ? L"menu-del"
                   : cmd == kCmdKeyClear   ? L"menu-clear"
                   : cmd == kCmdKeyReplace ? L"menu-repl"
                                           : L"menu-ins");
  set_status(a, cmd == kCmdKeyDelete ? L"已删除当前帧的关键帧"
                     : cmd == kCmdKeyClear ? L"已清除该属性的关键帧"
                                           : L"已在当前帧写入关键帧");
  a.dirty |= kRViewport | kRProps | kRLanes;
  sync_preview(a);
}

// 速度行右边那颗装饰点：它管的「曲线」是这条带的变速键，所以状态和变换那五行同构
// （没键 = 细空心、有键 = 粗空心、当前帧正落在某把键上 = 实心），点一下也是同一条规则：
// 没键就插、有键就删。插走的是 I 菜单同一个 retiming_add_key（键值取该处求值，插下去
// 画面不跳），删不了首尾那两把（retiming_remove_multiple_keys:532-536 也跳过它们）。
void speed_dot_click(App &a, Clip *c) {
  const double x = a.project.playhead - c->timeline_start;
  for (size_t i = 0; i < c->retiming.size(); ++i) {
    if (std::fabs(c->retiming[i].strip_frame - x) > 0.0001) continue;
    if (i == 0 || i + 1 == c->retiming.size()) {
      set_status(a, L"条带首尾那两把变速键删不掉");
      return;
    }
    retiming_remove_key_index(*c, i);
    retiming_prune_sel(a);  // 下标跟着漂了
    trace_retime(a, *c, L"key-del");
    undo_push(a, L"删除变速关键帧");
    a.dirty |= kRLanes | kRProps | kRViewport;
    sync_preview(a);
    set_status(a, L"已删除当前帧的变速关键帧");
    return;
  }
  if (!retiming_is_allowed(*c, a.frame_dur())) {
    set_status(a, c->kind == kStripText ? L"文本条带没有变速：它不是一段素材"
                                        : L"这条带没得变速：素材太短");
    return;
  }
  if (!retiming_add_key(*c, x)) {
    set_status(a, L"这个帧上没法加键：播放头要在条带内部，且该处还没有键");
    return;
  }
  c->show_retiming = true;  // retiming_key_add_from_selection :185 也顺手打开显示
  retiming_clear_key_sel(a);
  trace_retime(a, *c, L"key-add");
  undo_push(a, L"插入变速关键帧");
  a.dirty |= kRLanes | kRProps;
  set_status(a, L"已在当前帧插入变速关键帧：点条带下沿的菱形选中它，按 R 改这一段的速度");
}

// 装饰点的单击：Blender 把插入和删除合在同一个可点元素上（button_anim_decorate_cb 里
// 就一个 if (but_anim->flag & BUT_ANIMATED_KEY) 分流），走的是和右键菜单完全同一个算子，
// 所以这里只需要按当前状态选 cmd。
void keyframe_dot_click(App &a, int field) {
  Clip *c = props_clip(a);
  if (!c || c->locked) return;
  if (field == kFSpeed) {
    speed_dot_click(a, c);
    return;
  }
  if (field < kFOfsX || field > kFRotation) return;
  const bool keyed = (key_field_state(a, field) & kKeyAtFrame) != 0;
  a.key_field = field;
  keyframe_menu_cmd(a, keyed ? kCmdKeyDelete : kCmdKeyInsert);
  undo_push(a, keyed ? L"删除关键帧" : L"插入关键帧");
}

// 面板里的布尔行：翻转 Strip::flag 的对应位。
//   use_flip_x / use_flip_y = SEQ_FLIPX / SEQ_FLIPY（rna_sequencer.cc:3187-3195）
//   mute / lock = SEQ_MUTE / SEQ_LOCK（:2674-2686）
bool prop_toggle(App &a, int id, Clip *c) {
  // 粗体/斜体写在 TextVars 的 flag 位上，落点和文本那几行同一处（text_target）：没有文本带
  // 时也翻得动，翻的是新建文本带继承的那一份。
  if (id == kFTextBold || id == kFTextItalic) {
    TextVars &t = text_target(a, c);
    bool &b = id == kFTextBold ? t.bold : t.italic;
    b = !b;
    a.dirty |= kRViewport | kRProps;
    sync_preview(a);
    undo_push(a, L"文本字形");
    return true;
  }
  if (!c) return false;
  switch (id) {
    case kFFlipX: c->flip_x = !c->flip_x; break;
    case kFFlipY: c->flip_y = !c->flip_y; break;
    case kFMute: c->muted = !c->muted; break;
    case kFStripLock: c->locked = !c->locked; break;
    default: return false;
  }
  a.dirty |= kRViewport | kRProps | kRLanes;
  sync_preview(a);
  undo_push(a, id == kFFlipX || id == kFFlipY ? L"镜像" : L"条带开关");
  return true;
}

// ---------------------------------------------------------------- 预览 / 音频同步
void sync_preview(App &a) {
  std::vector<Clip *> stack = rendered_clips(a, a.project.playhead);
  if (stack.empty()) {
    a.preview.reset();
    return;
  }
  Rect box = fit_box(a);
  const OutputFormat &f = a.project.fmt;
  // 解码尺寸量化：窗口逐像素变化会让解码流不断重建，取 64px 一档。
  std::vector<Preview::Target> targets;
  targets.reserve(stack.size());
  for (Clip *c : stack) {
    // 解码这条路只有影片能走：声带不产画面，文本的画面是排版器现排的（clip_source_size），
    // 两者都没有源文件可解。
    if (c->kind != kStripMovie) continue;
    // 解码尺寸按「条带在屏幕上的大小」要：画面不再缩放适配（见 clip_base_rect），源比画布大时
    // 屏幕上占的像素比画布还多，按画布解码等于把小图放大糊掉。上限取源本身——超源解码没有信息。
    const int sw = c->info->width, sh = c->info->height;
    double dw = std::min(box.w * double(sw) / double(std::max(1, f.width)), double(sw));
    double dh = std::min(box.h * double(sh) / double(std::max(1, f.height)), double(sh));
    int pw = int(dw / 64.0) * 64;
    int ph = int(dh / 64.0) * 64;
    if (pw < 64) pw = 64;
    if (ph < 64) ph = 64;
    Preview::Target t;
    t.lane = c->id;  // 按条带建键：同一条素材在两条轨道上，源位置各是各的
    t.vi = c->info;
    t.source_seconds = src_time(*c, a.project.playhead);
    t.w = pw;
    t.h = ph;
    targets.push_back(t);
  }
  a.preview.request(targets);
}

void sync_audio(App &a) {
  if (!a.playing) {
    a.audio.stop();
    return;
  }
  std::vector<Clip *> snd = sound_clips(a, a.project.playhead);
  if (snd.empty()) {
    a.audio.stop();
    return;
  }
  // 这一次推的段尾：下一个「发声集合」会变样的时刻，也就是手里这几条里最早结束的，加上
  // 现在还没开始、但会在途中冒头的那些条带的开始。跨过它就得换一组声部，所以段推到那里为止。
  double seg_end = 1e18;
  for (auto &c : a.project.clips) {
    if (!sound_eligible(a, c)) continue;
    const double s0 = c.timeline_start, s1 = c.timeline_start + c.duration;
    if (s0 > a.project.playhead) seg_end = std::min(seg_end, s0);
    if (s1 > a.project.playhead) seg_end = std::min(seg_end, s1);
  }
  // 音频端点都是「源秒」。变速曲线是分段的，所以每条只喂当前这一段：
  // Blender 那边同样逐段给 Audaspace 挂常量音高
  // （retiming_sound_animation_data_set，strip_retiming.cc:1246-1274 的
  //  BKE_sound_set_scene_sound_pitch_constant_range 循环），播放头跨过键自然换下一段。
  std::vector<MixInput> mix;
  for (Clip *c : snd) {
    double r0 = 0.0, r1 = 0.0, spd = 1.0;
    const double x0 = a.project.playhead - c->timeline_start;  // 条带内的时间线时刻
    retiming_range_at(*c, x0, &r0, &r1, &spd);
    // 这一条在当前这段里出声到 x1 为止，换到源秒就是 from→until。
    const double x1 =
        std::min({seg_end, c->timeline_start + std::min(r1, c->duration)}) - c->timeline_start;
    if (x1 <= x0) continue;
    const double from = retiming_src_at(*c, x0);
    const double until = retiming_src_at(*c, x1);
    if (until - from < a.frame_dur() * spd) continue;  // 这一段已经没有音频了
    mix.push_back(MixInput{c->info, from, until, spd});
  }
  if (mix.empty()) {
    a.audio.stop();
    return;
  }
  const double tl_from = a.project.playhead;
  a.audio.play(mix, tl_from, seg_end);
  // 音频为主时钟，但这里只负责「受理」：发声位置交给 playhead_advance 去取，本函数不写播放头。
  // 两处写同一个数就是这一跳跳前一下跳后一下 —— 画面在两帧之间来回跳。
  // 主从关系的读数：每秒一行，请求起点和发声位置差多少、段尾在哪，量出来才知道有没有漂。
  {
    static double last = 0.0;
    if (now_seconds() - last > 1.0) {
      last = now_seconds();
      wchar_t b[96];
      swprintf(b, 96, L"audio clock: req=%.3f out=%.3f 段尾=%.3f 声部=%llu", tl_from,
               a.audio.position(), seg_end, static_cast<unsigned long long>(mix.size()));
      trace(b);
    }
  }
}

// ---------------------------------------------------------------- 导入

// 新条带落在已有条带上时，让位的是新来的这一组：整组一起升到上面第一条空通道，
// 已有条带一格时间都不动。单位是「这一次新增的一组」（影片 + 它的声带），所以两者永远
// 停在同一时刻、还是上下相邻；逐个各算一次才会把它们拆到两个通道上。
// 「空通道」= 这条通道上没有别的条带和它在时间上相交（相接不算相交），且通道没静音、没锁定。
// 一开始就不重叠 ⇒ 一格都不动，这是常态。
void shuffle_vertical(App &a, const std::vector<Clip *> &fresh) {
  if (fresh.empty()) return;
  auto f0 = [&](const Clip &x) { return a.t_to_frame(x.timeline_start); };
  auto f1 = [&](const Clip &x) { return a.t_to_frame(x.timeline_start + x.duration); };
  // 同通道 + 时间相交，相接不算相交。只把自己排除在外，组里其它成员照测：
  // 它们和自己是同一个 delta 一起抬起来的，相对位置不变，测不出假重叠。
  auto overlaps = [&](const Clip *x) {
    for (const Clip &o : a.project.clips)
      if (&o != x && o.track == x->track && f0(*x) < f1(o) && f1(*x) > f0(o)) return true;
    return false;
  };

  std::vector<int> orig;
  orig.reserve(fresh.size());
  int max_track = 0;
  bool overlapped = false;
  for (Clip *c : fresh) {
    orig.push_back(c->track);
    max_track = std::max(max_track, c->track);
    if (overlaps(c)) overlapped = true;
  }
  if (!overlapped) return;

  // 试位就是把通道号真的写上去再测，不行就还原：整组同一个 delta 平移，所以影片和声带
  // 抬上去之后还是上下相邻、还停在同一时刻。
  for (int delta = 1; max_track + delta < kMaxChannels; delta++) {
    for (size_t i = 0; i < fresh.size(); i++) fresh[i]->track = orig[i] + delta;
    bool free_lane = true;
    for (Clip *c : fresh) {
      const bool open = c->track >= int(a.project.tracks.size()) ||
                        (!a.project.tracks[c->track].muted && !a.project.tracks[c->track].locked);
      if (!open || overlaps(c)) {
        free_lane = false;
        break;
      }
    }
    if (free_lane) {
      // 升到还没有的通道 = 先把这条通道建出来，否则时间线上没有那一行，条带画不出来。
      for (Clip *c : fresh) channel_slot(a, c->track);
      a.dirty |= kRLanes;
      return;
    }
    for (size_t i = 0; i < fresh.size(); i++) fresh[i]->track = orig[i];
  }
  // 一百二十八格通道全占满才会走到这里：就叠在原位。横向推开已有条带是另一种让位语义，
  // 不在这里借道。
}

void add_source(App &a, const std::wstring &path, double at, int track) {
  std::wstring err;
  VideoInfo *vi = MediaCache::instance().open(path, &err);
  if (!vi) {
    set_status(a, L"导入失败：" + err);
    return;
  }
  // 帧率与像素宽高比跟源 = Blender sequencer.movie_add 的 use_framerate（出厂默认 true，
  // sequencer_add.cc:1558-1562「Set frame rate of the current scene to the frame rate of the
  // movie」→ strip_add.cc:457-463 写 scene->r.frs_sec/frs_sec_base）。
  // 它的开关只看条带：sequencer_add.cc:766-775 时间线里已有条带就把 use_framerate 设回 false，
  // :1342-1343 同一次批量里只有第一条视频流能改。所以这里的门是「工程里还没有条带」，
  // 不能再加上「还没有通道」—— 通道建出来就再也拆不掉，加上去会让「清空时间线重导另一只
  // 不同帧率的素材」把工程帧率悄悄停在旧值。
  if (a.project.clips.empty()) {
    // 分辨率不跟素材：Blender 全树只有显式算子 sequencer.rendersize（sequencer_edit.cc:3813-3817）
    // 会写 scene->r.xsch/ysch，导入影片从不改工程尺寸。
    double rate = vi->frame_rate.value();
    // Blender 的帧率是「整数 / 基数」：29.97 存成 fps=30、fps_base=1.001
    // （scripts/presets/framerate/29.97.py），所以这里把非整数帧率折成同一形式。
    a.project.fmt.fps = int(std::lround(rate));
    a.project.fmt.fps_base = a.project.fmt.fps > 0 ? double(a.project.fmt.fps) / rate : 1.0;
    if (!(a.project.fmt.fps_base > 0)) a.project.fmt.fps_base = 1.0;
    a.project.fmt.par_x = vi->sar_num;
    a.project.fmt.par_y = vi->sar_den;
  }
  if (a.project.tracks.empty()) {
    Track v;
    v.index = 0;
    v.name = std::to_wstring(v.index + 1);  // 通道列只显数字（Blender 的默认名是 DATA_("Channel %d")）
    a.project.tracks.push_back(v);
  }
  Clip c;
  c.id = a.next_id++;
  c.source_path = vi->path;
  c.info = vi;
  // Blender 的通道分配：声音在 base、影片在 base + sound_count（sequencer_add.cc:1334 与
  // :1350，base = max(1, 请求通道)，1 基）。本工程 0 基，所以带音轨时影片往上让一格，
  // 声带落在请求的那一格 = 时间线上影片在下、声部在更低的通道行。
  const bool has_snd = vi->has_audio;
  const int base = track >= 0 ? track : 0;  // -1 = 用默认（最底）通道
  c.track = base + (has_snd ? 1 : 0);
  // 落在还没有的通道上 = 先把这条通道建出来（Blender 的通道本来就恒存在）。
  if (c.track >= int(a.project.tracks.size())) channel_slot(a, c.track);
  c.timeline_start = a.frame_to_t(a.t_to_frame(at >= 0 ? at : a.project.playhead));
  c.source_in = 0;
  c.duration = vi->duration > 0 ? vi->duration : 1.0;
  // 导入时把「适配」烘进条带自己的缩放：set_scale_to_fit(strip, ibuf->x, ibuf->y,
  // scene->r.xsch, scene->r.ysch, fit_method)（strip_add.cc:285 → utils.cc:380-391），
  // fit_method 出厂默认 SEQ_SCALE_TO_FIT = min(宽比, 高比) ⇒ 等比铺满、必要时留边。
  // 变换语义本身仍是「源像素原尺寸摆在画布中心」（strip_source_size_get，
  // strip_transform.cc:759），所以这里写的是一条真实的缩放数据（用户能看见、能改、能打键），
  // 而不是绘制时偷偷适配 —— 分辨率解绑之后，画面大小就靠这一行接住。
  if (vi->width > 0 && vi->height > 0) {
    const double fit = std::min(double(a.project.fmt.width) / vi->width,
                                double(a.project.fmt.height) / vi->height);
    if (fit > 0) c.scale_x = c.scale_y = fit;
  }
  if (trace_fp) {
    wchar_t t[192];
    swprintf(t, 192, L"add %dx%d@%.3f → fmt %dx%d scale=%.3f frame_end=%d", vi->width, vi->height,
             vi->frame_rate.value(), a.project.fmt.width, a.project.fmt.height, c.scale_x,
             a.project.fmt.frame_end);
    trace(t);
  }
  size_t slash = path.find_last_of(L"\\/");
  c.label = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
  a.project.clips.push_back(std::move(c));
  // 声带：把手与影片逐帧对齐后连成一组。对齐 = sequencer_add_movie_sync_strip 的
  // right_handle_set(anchor->right_handle) / left_handle_set(anchor->left_handle)
  // （sequencer_add.cc:1280-1283）；连接 = 同文件的所有条带自动成组（:1394-1400，
  // 开关 connect_strips_by_default 出厂就是开的，DNA_userdef_types.h:1305）。
  Clip *movie = &a.project.clips.back();
  const int64_t movie_id = movie->id;
  std::vector<Clip *> group;
  group.push_back(movie);
  if (has_snd) {
    Clip s;
    s.id = a.next_id++;
    s.source_path = vi->path;
    s.info = vi;
    s.kind = kStripSound;
    s.track = base;
    if (s.track >= int(a.project.tracks.size())) channel_slot(a, s.track);
    s.timeline_start = movie->timeline_start;
    s.duration = movie->duration;
    s.label = movie->label;
    a.project.clips.push_back(std::move(s));
    // push_back 可能整体搬家，所以影片指针按 id 重新取，只有尾元素可以直接取。
    group.clear();
    group.push_back(find_clip(a, movie_id));
    group.push_back(&a.project.clips.back());
  }
  // 落在已有条带上 = 整组往上升通道让位，不叠上去，也不把已有条带往后推。
  shuffle_vertical(a, group);
  if (has_snd) connect_clips(a, group);
  select_only(a, group.front()->id);
  recompute_duration(a);
  // 导入到此为止：帧范围（开始帧..结束帧）不被对象写。Blender 全树没有任何导入/条带算子
  // 碰 sfra/efra（只有 rna_Scene 的 start/end_frame_set 和 sequencer.set_range_to_strips
  // 这两个显式入口），它负责的是播放与导出，对象负责的是视图对齐（用户 2026-11-02）。
  // 于是新场景导入 3 小时的片子，范围仍是 0..250：空格只放这 250 帧，HOME 却把整条片子
  // 摆满屏幕。要让播放与导出盖住内容，用条带右键菜单的「设为条带范围」，或者直接在
  // 属性区改开始帧/结束帧 —— 这两条都是显式操作，不是对象替用户决定。
  wchar_t buf[200];
  swprintf(buf, 200, L"%dx%d  %.3ffps  %.2fs  %ls", vi->width, vi->height,
           vi->frame_rate.value(), vi->duration, vi->codec.c_str());
  set_status(a,
             has_snd ? L"已添加条带 + 声带  " + a.project.clips.back().label + L"  " + buf
                     : L"已添加条带  " + a.project.clips.back().label + L"  " + buf);
}

void import_paths(App &a, const std::vector<std::wstring> &paths, double at, int track) {
  // 本次会话第一次打开文件 ⇒ 记住那个目录，导出对话框以后就从它起步（只认第一次，
  // 关掉窗口就没了，所以也不写进设置文件）。
  if (a.open_dir.empty() && !paths.empty()) {
    std::wstring p0 = paths.front();
    // 命令行传进来的路径可以是正斜杠，而 shell 只认反斜杠的那个（SHCreateItemFromParsingName
    // 对 'E:/a/b' 直接给 E_INVALIDARG）⇒ 存之前统一成反斜杠。
    for (auto &ch : p0) if (ch == L'/') ch = L'\\';
    const size_t s = p0.find_last_of(L"\\/");
    if (s != std::wstring::npos && s > 0) a.open_dir = p0.substr(0, s);
  }
  // 多文件时逐个接在上一个后面（Blender 的 add 也是从当前帧起依次排开）；
  // 但当前帧本身不动：加完只是 select_active_set 改活动条带，不写 scene->r.cfra
  // （sequencer_add.cc:730-733）。
  const double playhead0 = a.project.playhead;
  for (const auto &p : paths) {
    add_source(a, p, at, track);
    at = -1.0;
    a.project.playhead = project_end(a);
  }
  a.project.playhead = playhead0;
  a.dirty = kRegionAll;
  sync_preview(a);
  // 一次导入 = 一步撤消（多个文件也只是一次 sequencer.add_movies 的结果）。
  undo_push(a, L"导入条带");
}

// 新建文本条带，对应 Blender 的 sequencer.text_strip_add（sequencer_add.cc:2114-2145）。
// 落位照它的 invoke：起点 = 当前帧（SEQPROP_STARTFRAME），通道 = 从猜中的那一格往上找
// 第一条没被占用的（:363-393 sequencer_generic_invoke_xy_guess_channel +
// find_unlocked_unmuted_channel）。长度走「没有内在时长的条带」那个默认值（:501 →
// strip_add.cc:64 default_strip_length，它读 U.sequencer_default_strip_length，出厂 1.0 秒
// —— 需求要 3 秒，所以这里写 3 秒，这是明知故犯的偏离，不是漏抄）。
// 文本没有源文件：info / source_path 一律留空，画面由排版器现给（clip_source_size），
// 时长由条带自己说。也正因为没有源，导入时烘「适配」缩放那一步对它不存在 —— 字号本来就是
// 输出像素，再乘一次适配系数等于凭空放大。
void add_text(App &a) {
  Clip c;
  c.id = a.next_id++;
  c.kind = kStripText;
  // 样式从 a.text_def 抄起：那一格就是「还没有文本带时面板改的地方」，两者必须是同一个
  // 来源，否则用户在里面调半天、新建的带一个字都不认。
  c.text = a.text_def;
  c.label = L"文本";
  int top = -1;
  for (const Clip &o : a.project.clips) top = std::max(top, o.track);
  c.track = top + 1;
  // 起点对准播放头，并且落在帧格上：帧才是时间线上的刻度，导入走的是同一句换算。
  c.timeline_start = a.frame_to_t(a.t_to_frame(a.project.playhead));
  c.duration = a.frame_to_t(a.t_to_frame(3.0));
  if (c.track >= int(a.project.tracks.size())) channel_slot(a, c.track);
  const int64_t id = c.id;
  a.project.clips.push_back(std::move(c));
  if (trace_fp) {
    wchar_t t[192];
    swprintf(t, 192, L"add_text id=%lld track=%d start=%.6f dur=%.6f", (long long)id, top + 1,
             a.project.clips.back().timeline_start, a.project.clips.back().duration);
    trace(t);
  }
  select_only(a, id);
  recompute_duration(a);
  a.dirty |= kRLanes | kRViewport;
  set_status(a, L"已添加文本条带：内容、字体、颜色在左侧「文本」组里改");
}

// ---------------------------------------------------------------- 编辑算子

// sequencer.split。切分帧 = scene->r.cfra，条件 = edit_frame_splits_strip：
// left_handle < frame < right_handle（strip_edit.cc:307-310）。本工程不建模
// hold/left_handle（HARD 与 SOFT 的差别只在能否向左多拖出帧），所以两者在这里的
// 数据结果相同，仅状态文字不同。
//
// 连接的处理取 Blade/整组切分那条路（ignore_connections=false），不是 K 键
// only_selected=True 那条断路：Blade 默认 only_selected=false、ignore_connections=false
// （blender_default.py:8977-8981），edit_strip_split 于是沿连接 expand_strips
// （include=ConnectedEffectChain，strip_edit.cc:442-444）把整组一起切，且复制前不
// disconnect（:460-462 只在 ignore_connections 时才断）。右半是整组复制出来的，连接名单
// 按 strip_map 换引用（sequencer.cc:362-366）再剪单向（:639-644），被删半段的引用由
// 删除时的 disconnect 清掉 ⇒ 净效果 = 左半幸存者互联成一团、右半幸存者互联成一团。
// 用户实测 Blender 切分后连接未断，正是这一条。组在本工程里本来就是全连接团
// （connect_clips），所以这里直接按两侧幸存者各重建一团，仍走 connect_clips 这一个所有者。
void split_selected(App &a, bool hard) {
  const double t = a.frame_to_t(a.cur_frame());
  // 种子 = 选中、未锁、播放头落在内部；沿连接扩成整组（切分是对整组动手，不是对单条）。
  std::vector<int64_t> visited;
  std::vector<std::vector<int64_t>> comps;
  for (const Clip &c : a.project.clips) {
    if (!clip_selected(a, c.id) || c.locked) continue;
    if (!(t > c.timeline_start && t < c.timeline_start + c.duration)) continue;
    if (std::find(visited.begin(), visited.end(), c.id) != visited.end()) continue;
    std::vector<int64_t> comp, stack = {c.id};
    while (!stack.empty()) {
      const int64_t id = stack.back();
      stack.pop_back();
      if (std::find(visited.begin(), visited.end(), id) != visited.end()) continue;
      visited.push_back(id);
      comp.push_back(id);
      if (const Clip *m = find_clip(a, id))
        for (int64_t o : m->connections) stack.push_back(o);
    }
    comps.push_back(std::move(comp));
  }
  if (comps.empty()) {
    set_status(a, L"播放头没有落在选中条带内部");
    return;
  }

  std::vector<Clip> added;
  std::vector<std::vector<int64_t>> cliques;  // 每团 = 切完后仍互联的一组（左半一团/右半一团）
  bool any = false, locked_block = false;
  for (const auto &comp : comps) {
    // Blender：组里只要有锁定的就不切（"Strip is locked, cannot split"，strip_edit.cc:398-402）。
    bool blocked = false;
    for (int64_t id : comp)
      if (const Clip *m = find_clip(a, id))
        if (m->locked) blocked = true;
    if (blocked) {
      locked_block = true;
      continue;
    }
    std::vector<int64_t> left_ids, right_ids;
    for (int64_t id : comp) {
      Clip *m = find_clip(a, id);
      if (!m) continue;
      const double start = m->timeline_start, end = start + m->duration;
      if (t > start && t < end) {
        Clip right = *m;
        const double off = t - start;
        m->duration = off;
        right.timeline_start = t;
        // 右半段把曲线在它自己的新左缘处重切：切点之前的键丢掉、之后的键平移（Blender 的
        // split 是整条复制连同 retiming_keys 一起带走，SEQ_split.cc 里改的就是 start/len）。
        right.source_in = retiming_move_left_edge(right, off);
        right.duration -= off;
        const int64_t rid = a.next_id++;
        right.id = rid;
        right.connections.clear();  // 名单稍后按右半幸存者重建
        added.push_back(std::move(right));
        left_ids.push_back(m->id);
        right_ids.push_back(rid);
        any = true;
      } else if (end <= t) {
        left_ids.push_back(id);  // 整条在切点左侧：原样留在左组
      } else {
        right_ids.push_back(id);  // 整条在切点右侧：原样留在右组
      }
    }
    if (!left_ids.empty()) cliques.push_back(std::move(left_ids));
    if (!right_ids.empty()) cliques.push_back(std::move(right_ids));
  }
  if (!any) {
    set_status(a, locked_block ? L"连接组里有锁定条带，未切分" : L"播放头没有落在选中条带内部");
    return;
  }
  for (auto &c : added) a.project.clips.push_back(std::move(c));
  // 重连：左半们一团、右半们一团。push_back 之后指针才稳定，按 id 重新取。
  for (const auto &clique : cliques) {
    std::vector<Clip *> group;
    for (int64_t id : clique)
      if (Clip *c = find_clip(a, id)) group.push_back(c);
    if (group.size() >= 2) connect_clips(a, group);
    else if (group.size() == 1) group[0]->connections.clear();
  }
  recompute_duration(a);
  a.dirty |= kRLanes | kRScrub | kRProps;
  trace_links(a, hard ? L"hard_split" : L"split");
  set_status(a, hard ? L"已硬切" : L"已切分");
}

// 整条平移一个条带。transform_translate_strip（strip_transform.cc:106-113）在
// 改 strip->start 之后无条件调用 offset_animdata()：关键帧（含把手）跟着条带走
// 同样的帧数。所有「整条搬走」的路径都必须走这里，直接写 timeline_start 会把
// 动画留在原地。
static void move_clip_to(App &a, Clip *c, double ns) {
  const double delta = double(a.t_to_frame(ns) - a.t_to_frame(c->timeline_start));
  c->timeline_start = ns;
  if (delta != 0.0)
    for (FCurve &fcu : c->anim) fcu.offset_keys(delta);
  trace_keys(a, *c, L"move");
}

// sequencer.delete / ripple_delete
void delete_selected(App &a, bool ripple) {
  std::vector<Clip *> victims = selected_clips(a);
  if (victims.empty()) {
    set_status(a, L"未选中条带");
    return;
  }
  // erase 会让 vector 里的元素整体前移，指针失效，所以先把几何信息抄下来。
  struct Victim { int track; double start, duration; };
  std::vector<Victim> vd;
  for (Clip *v : victims) vd.push_back({v->track, v->timeline_start, v->duration});
  std::vector<int64_t> ids;
  for (Clip *v : victims) ids.push_back(v->id);
  a.project.clips.erase(
      std::remove_if(a.project.clips.begin(), a.project.clips.end(),
                     [&](const Clip &c) {
                       return std::find(ids.begin(), ids.end(), c.id) != ids.end();
                     }),
      a.project.clips.end());
  // 名单里指向已删条带的引用要一起清掉。Blender 在 strip_free 里做同样的事
  // （sequencer.cc:232-234 的 `if (is_strip_connected(strip)) disconnect(strip)`，
  // 断开时顺手把别人名单里的自己摘干净）。删除本身不传播：连接组成员不会因为
  // 同伴被删而跟着删（它们只是会在下一次被点中时才成为同组）。
  for (Clip &c : a.project.clips)
    c.connections.erase(std::remove_if(c.connections.begin(), c.connections.end(),
                                       [&](int64_t ref) { return !find_clip(a, ref); }),
                        c.connections.end());
  if (ripple) {
    // 同轨道上、左边缘落在被删区间右侧的条带整体左移（sequencer.ripple_delete）。
    for (Clip &c : a.project.clips) {
      double shift = 0.0;
      for (const Victim &v : vd) {
        if (v.track != c.track) continue;
        if (c.timeline_start >= v.start + v.duration - 1e-9) shift += v.duration;
      }
      if (shift > 0) move_clip_to(a, &c, std::max(0.0, c.timeline_start - shift));
    }
  }
  a.sel.clear();
  a.selected = -1;
  // 条带没了 ⇒ 它身上的键选择也一起作废（Shift X 的涟漪删除不走键删除那条绑定，
  // 所以真会带着选中的键跑到这里）。
  retiming_prune_sel(a);
  recompute_duration(a);
  a.dirty |= kRLanes | kRScrub | kRProps | kRViewport;
  sync_preview(a);
  trace_links(a, L"delete");
  set_status(a, ripple ? L"已删除并闭合间隙" : L"已删除条带");
}

void duplicate_selected(App &a) {
  std::vector<Clip *> src = selected_clips(a);
  if (src.empty()) {
    set_status(a, L"未选中条带");
    return;
  }
  // sequencer.duplicate：新集合成为选中项，随后拖动它即可。落点不叠在原型上：和导入、
  // 粘贴走同一条让位（整组升通道），所以「复制」不会在同一通道上盖出双层。
  std::vector<Clip> copies;
  std::vector<std::pair<int64_t, int64_t>> id_map;  // 原型 id → 副本 id
  for (Clip *c : src) {
    Clip n = *c;
    id_map.push_back({c->id, a.next_id});
    n.id = a.next_id++;
    copies.push_back(std::move(n));
  }
  a.sel.clear();
  for (auto &c : copies) {
    a.sel.push_back(c.id);
    a.project.clips.push_back(std::move(c));
  }
  // 副本的连接名单是从原型整份抄来的，里面还指着老条带：换引用 + 剪单向，
  // 与粘贴走同一处（remap_copied_links）。
  remap_copied_links(a, id_map);
  // 让位的单位是「这一次新增的一组」：整组同一个 delta 上移，影片和它的声带才不会被
  // 拆到两个通道上，也不会盖在原型那一格里。
  std::vector<Clip *> fresh;
  fresh.reserve(id_map.size());
  for (const auto &pr : id_map)
    if (Clip *n = find_clip(a, pr.second)) fresh.push_back(n);
  shuffle_vertical(a, fresh);
  a.selected = a.sel.back();
  recompute_duration(a);
  a.dirty |= kRLanes | kRProps;
  trace_links(a, L"duplicate");
  set_status(a, L"已复制条带（升到空通道，可直接拖动）");
}

// sequencer.connect（sequencer_edit.cc:1496-1532）。菜单项把 toggle 设成 True，所以
// 「选中的已经是互相连接的一群」时，这一击变成断开：这就是同一个菜单项往返切换的用法。
// 连接不改变任何条带数据，只是把这 N 条编成一个「选中一个 = 选中全部」的组。
void connect_selected(App &a) {
  std::vector<Clip *> group = selected_clips(a);
  if (group.empty()) {
    // 算子里 selected.is_empty() ⇒ OPERATOR_CANCELLED（:1504-1506）。
    set_status(a, L"未选中条带");
    return;
  }
  if (are_connected_together(group)) {
    bool changed = false;
    for (Clip *c : group) changed |= disconnect_clip(a, c);
    a.dirty |= kRLanes | kRProps;
    trace_links(a, L"connect->disconnect");
    set_status(a, changed ? L"已断开条带连接" : L"选中条带没有连接");
    return;
  }
  connect_clips(a, group);
  trace_links(a, L"connect");
  wchar_t buf[64];
  swprintf(buf, 64, L"已连接 %d 条条带（点一条就选中全部）", int(group.size()));
  set_status(a, buf);
  a.dirty |= kRLanes | kRProps;
}

// sequencer.disconnect（:1540-1553）：对所选每条断开，一条都没断开过 ⇒ CANCELLED。
void disconnect_selected(App &a) {
  std::vector<Clip *> group = selected_clips(a);
  bool changed = false;
  for (Clip *c : group) changed |= disconnect_clip(a, c);
  a.dirty |= kRLanes | kRProps;
  trace_links(a, L"disconnect");
  set_status(a, changed ? L"已断开条带连接" : L"选中条带没有连接");
}

// sequencer.mute / unmute：条带级 SEQ_MUTE（sequencer_edit.cc:1359-1380）。
// H = 静音选中，Shift+H = 静音未选中，Alt+H = 全部取消静音。
// SEQUENCER_OT_mute / SEQUENCER_OT_unmute（sequencer_edit.cc:1307-1333 与 :1362-1388）：
// 只对所选条带置位/清位 SEQ_MUTE，不碰其它条带（unselected=true 的那半边在
// mute_unselected 里）。
void mute_selected(App &a, bool mute) {
  if (a.sel.empty()) {
    set_status(a, L"未选中条带");
    return;
  }
  for (Clip *c : selected_clips(a)) c->muted = mute;
  a.dirty |= kRLanes | kRProps | kRViewport;
  sync_preview(a);
  set_status(a, mute ? L"已静音选中条带" : L"已取消静音");
}

void mute_unselected(App &a) {
  if (a.project.clips.empty()) {
    set_status(a, L"没有条带");
    return;
  }
  // mute(unselected=True)：只给未选中的置位。
  for (Clip &c : a.project.clips)
    if (!clip_selected(a, c.id)) c.muted = true;
  a.dirty |= kRLanes | kRProps | kRViewport;
  sync_preview(a);
  set_status(a, L"已静音未选中的条带");
}

// sequencer.lock / unlock：SEQ_LOCK，禁止被变换（sequencer_edit.cc:1422-1431）。
void lock_selected(App &a, bool lock) {
  if (a.sel.empty()) {
    set_status(a, L"未选中条带");
    return;
  }
  for (Clip *c : selected_clips(a)) c->locked = lock;
  a.dirty |= kRLanes;
  set_status(a, lock ? L"已锁定条带" : L"已解锁条带");
}

// sequencer.gap_remove（Backspace）—— 逐段对应 strip_edit.cc:513-535 + strip_time.cc:361-397：
// 间隙 = 整条时间线上「任何通道都没有条带覆盖」的连续帧段。先从当前帧找出这段间隙的起点
// （初始帧被覆盖就向后找，否则向前找），再向后找间隙结束，然后把该帧之后的所有条带左移
// （transform_strips_after_frame）。all = 循环直到右侧再无间隙。
static bool gap_info_at(const App &a, int64_t cfra, int64_t *start, int64_t *length) {
  auto covered = [&](int64_t f) {
    for (const Clip &c : a.project.clips)
      if (c.timeline_start <= a.frame_to_t(f) &&
          a.frame_to_t(f) < c.timeline_start + c.duration)
        return true;
    return false;
  };
  int64_t sfra = 0, efra = std::max<int64_t>(1, a.t_to_frame(project_end(a)));
  int64_t frame = cfra, gs = cfra;
  if (!covered(cfra)) {  // 向前找到第一个有条带的帧，间隙从它的下一帧开始
    for (; frame >= sfra; --frame)
      if (covered(frame)) break;
    gs = frame + 1;
    frame = cfra;
  } else {  // 向后找到第一个空白帧
    for (; frame <= efra; ++frame)
      if (!covered(frame)) { gs = frame; break; }
  }
  for (; frame <= efra; ++frame) {
    if (covered(frame)) {
      *start = gs;
      *length = frame - gs;
      return true;
    }
  }
  return false;
}

static void transform_strips_after_frame(App &a, int64_t timeline_frame, int delta_frames) {
  double from = a.frame_to_t(timeline_frame), delta = delta_frames * a.frame_dur();
  for (Clip &c : a.project.clips)
    // transform_shuffle_horizontal（strip_transform.cc:160-178）逐条调用
    // transform_translate_strip，所以被推走的条带关键帧一起平移。
    if (c.timeline_start >= from - 1e-9) move_clip_to(a, &c, std::max(0.0, c.timeline_start + delta));
}

void gap_remove(App &a, bool all) {
  int64_t gs;
  int64_t len;
  if (!gap_info_at(a, a.cur_frame(), &gs, &len)) {
    set_status(a, L"播放头右侧没有间隙");
    return;
  }
  transform_strips_after_frame(a, gs, -int(len));
  if (all) {
    int64_t guard = 0;
    while (guard++ < 4096 && gap_info_at(a, a.cur_frame(), &gs, &len))
      transform_strips_after_frame(a, gs, -int(len));
  }
  recompute_duration(a);
  a.dirty = kRegionAll;
  sync_preview(a);
  set_status(a, all ? L"已移除所有间隙" : L"已移除间隙");
}

// sequencer.gap_insert（Shift+=）：sequencer_edit.cc:569-579 —— 与选中无关，
// 把当前帧之后的所有条带（left_handle >= cfra）右移 frames，默认 10 帧。
void gap_insert(App &a, int frames) {
  transform_strips_after_frame(a, a.cur_frame(), frames);
  recompute_duration(a);
  a.dirty = kRegionAll;
  wchar_t buf[64];
  swprintf(buf, 64, L"已在当前帧插入 %d 帧间隙", frames);
  set_status(a, buf);
}

void slide_selected(App &a, double delta_t) {
  for (Clip *c : selected_clips(a)) {
    if (c->locked) continue;
    double ns = std::max(0.0, c->timeline_start + delta_t);
    move_clip_to(a, c, a.frame_to_t(a.t_to_frame(ns)));
  }
  recompute_duration(a);
  a.dirty |= kRLanes | kRScrub | kRViewport;
}

void slip_selected(App &a, double delta_t) {
  for (Clip *c : selected_clips(a)) {
    // 滑移 = 条带不动、采样点沿曲线滑过 delta_t 秒（Blender 只改 start/startofs，
    // 键与入点字段都不碰）；限位到素材两端由 retiming_slip 自己算。
    retiming_slip(*c, delta_t);
  }
  a.dirty |= kRLanes | kRViewport;
  sync_preview(a);
}

// ---------------------------------------------------------------- 模态变换
//
// km_sequencer（blender_default.py:3276-3290）的三个模态算子：
//   G  transform.seq_slide   {view2d_edge_pan=True}
//   S  sequencer.slip        {use_cursor_position=False}
//   E  transform.transform   {mode='TIME_EXTEND'}
// 三者都是「按下即开始、鼠标驱动、Enter/LMB 提交、Esc 还原」。这里用快照还原实现取消，
// 与 Blender 的 op->customdata 回滚等价。
bool modal_active(const App &a) {
  return a.modal.kind != ModalState::None;
}

void modal_begin(App &a, ModalState::Kind kind) {
  // km_sequencer 只把 G 和左键 CLICK_DRAG 绑到 transform.seq_slide
  // （blender_default.py:3279-3286），但 transform_convert.cc:958-965 在 SPACE_SEQ 里先看
  // 「有没有选中变速键」，有就把变换数据换成 TransConvertType_SequencerRetiming：同一个算子，
  // 移动的是键而不是条带。分流放在这里而不是调用点，鼠标拖和 G 键才共用一套语义。
  if (retiming_keys_are_selected(a)) kind = ModalState::RetimeKey;
  std::vector<Clip *> cs;
  if (kind == ModalState::RetimeKey) {
    // createTransSeqRetimingData（:177-212）拖的就是「选中键所属的那些条带」，
    // 锁定的先整条摘掉（:186 selection.remove_if(transform_is_locked)）。
    retiming_prune_sel(a);
    for (const auto &s : a.retime_sel) {
      Clip *c = find_clip(a, s.first);
      if (!c || c->locked || s.second >= c->retiming.size()) continue;
      if (std::find(cs.begin(), cs.end(), c) == cs.end()) cs.push_back(c);
    }
  } else {
    cs = selected_clips(a);
  }
  if (cs.empty()) {
    set_status(a, kind == ModalState::RetimeKey ? L"没有可移动的变速关键帧" : L"未选中条带");
    return;
  }
  a.modal = {};
  a.modal.kind = kind;
  a.modal.edge.last = now_seconds();
  a.modal.edge.scroll0 = a.tl.scroll_t;
  for (Clip *c : cs) a.modal.orig.push_back(*c);
  // use_cursor_position=False 的 slip 以当前帧为基准；slide/extend 以播放头为基准。
  a.modal.t0 = a.project.playhead;
  a.modal.y0 = float(a.mouse_y);  // G 键进入时，原点就是当前光标位置
  if (kind == ModalState::RetimeKey) {
    // T_POINTS：键只能沿时间线走，位移是「光标相对进入模态那一刻」的帧差
    // （create_trans_seq_clamp_data:121 还专门 T_NO_CONSTRAINT 关掉轴向约束）。
    a.modal.t0 = x_time(a, float(a.mouse_x));
    a.modal.grab_id = cs.front()->id;
    a.modal.grab_key = 0;
    for (const auto &s : a.retime_sel) {
      if (find_clip(a, s.first) == cs.front()) {
        a.modal.grab_key = s.second;
        break;
      }
    }
    // 整批键的公共限位（:156-173）：往右最多到「离下一把还差一帧」，往左最多到「离上一把
    // 还差一帧」；末键右边没有 next ⇒ 不限，首键左边没有 prev ⇒ 不限。一批里取最紧的那条。
    const double fd = a.frame_dur();
    for (Clip *c : cs)
      for (const auto &s : a.retime_sel) {
        if (s.first != c->id || s.second >= c->retiming.size()) continue;
        const auto &k = c->retiming;
        if (s.second + 1 < k.size())
          a.modal.kmax =
              std::min(a.modal.kmax, k[s.second + 1].strip_frame - k[s.second].strip_frame - fd);
        if (s.second > 0)
          a.modal.kmin =
              std::max(a.modal.kmin, -(k[s.second].strip_frame - k[s.second - 1].strip_frame - fd));
      }
  }
  const wchar_t *name = kind == ModalState::Slide ? L"移动（Enter 提交 / Esc 取消）"
                        : kind == ModalState::Slip ? L"滑动内容（Enter 提交 / Esc 取消）"
                        : kind == ModalState::RetimeKey
                            ? L"移动变速关键帧（Enter 提交 / Esc 取消）"
                            : L"从当前帧移动或延长（Enter 提交 / Esc 取消）";
  set_status(a, name);
  a.dirty = kRegionAll;
}

void modal_update(App &a, double t) {
  if (a.modal.kind == ModalState::None) return;
  const double fd = a.frame_dur();
  // 变速键：一次拖拽里所有选中的键共用同一个整帧位移（recalcData_sequencer_retiming 就是
  // 拿「键的原始帧 + 同一个 offset_clamped」逐个算目标），所以位移在循环外算一次。
  double dkey = 0.0;
  if (a.modal.kind == ModalState::RetimeKey) {
    const Clip *g0 = nullptr;
    for (const Clip &r : a.modal.orig)
      if (r.id == a.modal.grab_id) g0 = &r;
    if (g0 && g0->retiming.size() > a.modal.grab_key) {
      const int64_t f0 =
          a.t_to_frame(g0->timeline_start + g0->retiming[a.modal.grab_key].strip_frame);
      // 鼠标位移无条件量化到整帧（transform_convert_sequencer_clamp:915-935 先 round 再夹）。
      int64_t df = a.t_to_frame(t) - a.t_to_frame(a.modal.t0);
      // 吸附：被拖的键自己就是 snap source（build_sources_timeline_retiming:219-231），
      // 目标是别的条带边与播放头；按住 Ctrl 临时反过来（需求里那条）。
      if (!ctrl_held()) {
        double cand = a.frame_to_t(f0 + df);
        cand = snap_time(a, float(time_x(a, cand)), cand, g0, kSnapStrip);
        df = a.t_to_frame(cand) - f0;
      }
      dkey = std::clamp(double(df) * fd, a.modal.kmin, a.modal.kmax);
    }
  }
  // 纵向位移整组共用一个夹过的值：限位对组里每条带取最紧的那一条
  // （transform_convert_sequencer.cc:529-530 的 offset_clamp 夹的是位移，不是落点）。
  // 逐条夹落点会让连接组贴底时影片和声带叠到同一通道，看起来像声带没了。
  int dch_clamped = 0;
  if (a.modal.kind == ModalState::Slide) {
    int min_tr = kMaxChannels, max_tr = 0;
    for (const Clip &r : a.modal.orig) {
      min_tr = std::min(min_tr, r.track);
      max_tr = std::max(max_tr, r.track);
    }
    const int dch =
        int(std::lround((double(a.modal.y0) - double(a.mouse_y)) / double(channel_h(a))));
    // 往下最多到组里最低那条贴底，往上最多到最高那条贴顶（0 基通道）。
    dch_clamped = std::clamp(dch, -min_tr, kMaxChannels - 1 - max_tr);
  }
  for (size_t i = 0; i < a.modal.orig.size(); ++i) {
    Clip &ref = a.modal.orig[i];
    Clip *c = find_clip(a, ref.id);
    if (!c) continue;
    double q = a.frame_to_t(a.t_to_frame(t));
    switch (a.modal.kind) {
      case ModalState::Slide: {
        const bool row_locked = ref.track >= 0 && ref.track < int(a.project.tracks.size()) &&
                                a.project.tracks[ref.track].locked;
        // 时间轴是 MINFRAME..MAXFRAME 的有符号整数轴（sfra/efra 本身可为负），所以
        // 条带可以拖到 0 帧以左 —— 这里不钳左端。
        double ns = ref.timeline_start + (q - a.modal.t0);
        // 需求：拖片段默认开磁性吸附，按住 Ctrl 临时关掉。
        // 按下那一刻的鼠标时刻是连续量（drag_t0），不减成整数帧的话片段会停在
        // 半帧上；整数帧对齐始终生效，所以先取整、再在取整后的位置上做吸附。
        ns = a.frame_to_t(a.t_to_frame(ns));
        if (!ctrl_held()) ns = snap_strip_move(a, *c, ns);
        if (row_locked) ns = ref.timeline_start;  // 锁定通道：seq_slide 整条都不动
        c->timeline_start = ns;
        // transform_translate_strip（strip_transform.cc:113）平移条带时无条件调用
        // offset_animdata()：关键帧跟着条带走同一个帧数。这里是「从快照重放」的写法，
        // 所以先还原快照里的曲线，再按总位移平移一次，位移不会被逐帧累加。
        const double kdelta = double(a.t_to_frame(ns) - a.t_to_frame(ref.timeline_start));
        for (int ch = 0; ch < kAnimCount; ch++) {
          c->anim[ch] = ref.anim[ch];
          if (kdelta != 0.0) c->anim[ch].offset_keys(kdelta);
        }
        if (!row_locked) {
          // 纵向 = 换通道。Blender 把鼠标纵向像素差按「1 通道 = channel_h 像素」换算
          // （convertViewVec2D transform.cc:156-164），round 成整数通道
          // （applySeqSlide transform_mode_edge_seq_slide.cc:133-134）。夹在整组那一笔
          // （dch_clamped）已经做过，这里只落点：组内相对位置一格都不变。
          // 通道是扁平的一列（channels.cc:30-40），视频/声音条带都能落任何一条，
          // 落在还没建出来的通道上就把这条通道补出来。
          const int nt = ref.track + dch_clamped;
          if (nt >= int(a.project.tracks.size())) channel_slot(a, nt);
          c->track = nt;
        }
        break;
      }
      case ModalState::RetimeKey: {
        // 从快照重放：每一帧先还原这条带的曲线与位置，再施加同一个位移。位移因此不会被
        // 逐帧累加，Esc 也只是丢掉这次重放（= Blender 的 op->customdata 回滚）。
        Clip w = ref;
        for (const auto &s : a.retime_sel)
          if (s.first == ref.id) retiming_move_key(w, s.second, dkey, fd);
        *c = w;
        break;
      }
      case ModalState::Slip: {
        retiming_slip(*c, q - a.modal.t0);
        break;
      }
      case ModalState::Extend: {
        // 光标落在条带右缘之外 = 延长；落在左缘之前 = 左缘跟随光标（TIME_EXTEND 的两个方向）。
        // 长度是时间线秒数，源时刻沿曲线换算，所以上限是 max_duration()。
        if (q >= ref.timeline_start) {
          c->duration = std::max(fd, std::min(q - ref.timeline_start, max_duration(*c)));
        } else {
          double d = ref.timeline_start - q;
          *c = ref;
          c->timeline_start = q;
          // 向左延长 = 曲线的左缘往回退 d 秒，首段斜率外推；退到源 0 为止。
          const double back = std::min(d, -retiming_x_at_src(ref, 0.0));
          c->source_in = retiming_move_left_edge(*c, -back);
          c->duration = std::max(fd, ref.duration + back);
        }
        break;
      }
      default: break;
    }
  }
  recompute_duration(a);
  a.dirty |= kRLanes | kRScrub | kRViewport | kRProps;
  sync_preview(a);
}

// view2d_edge_pan_apply（view2d_edge_pan.cc:221-277）的横向分支。
// 时间线只有 X 能滚（v2d.keepofs 有 V2D_LOCKOFS_Y），outside_pad=0 表示光标可以出界继续滚。
void edge_pan_step(App &a, double now) {
  // 键拖动走的还是同一个 transform.seq_slide（只是数据换成 SequencerRetiming），
  // 所以 view2d_edge_pan=True 对它同样成立。
  if (a.modal.kind != ModalState::Slide && a.modal.kind != ModalState::RetimeKey) return;
  EdgePan &e = a.modal.edge;
  const float pad = btheme::u(3.5f);  // STRIP_EDGE_PAN_INSIDE_PAD
  const float min_x = a.lanes.x + pad, max_x = a.lanes.x + a.lanes.w - pad;
  const float mx = a.mouse_x;
  if (mx >= min_x && mx <= max_x) e.enabled = true;
  const int dir = !e.enabled ? 0 : (mx > max_x ? 1 : (mx < min_x ? -1 : 0));
  if (dir == 0) {
    e.start_x = 0.0;  // edge_pan_manage_delay_timers：离开滚动区就清零延时
    e.last = now;
    return;
  }
  if (e.start_x == 0.0) e.start_x = now;
  const double distance = dir > 0 ? mx - max_x : min_x - mx;
  // view2d_edge_pan_speed：距离因子按 speed_ramp(=1.0 UI 单位) 收敛到 0..1，
  // 延时因子用 smootherstep(delay=0.5s) 淡入，zoom_influence=0 所以不看缩放。
  const double dist_f = std::clamp(distance / btheme::u(1.0f), 0.0, 1.0);
  auto smootherstep = [](double dmax, double x) {
    x = std::clamp(x / dmax, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
  };
  const double delay_f = smootherstep(0.5, now - e.start_x);
  const double speed = dist_f * delay_f * 26.0 * btheme::widget_unit * btheme::scale;
  const double dx_px = (now - e.last) * speed * dir;
  e.last = now;
  if (dx_px == 0.0) return;
  a.tl.scroll_t += dx_px / a.tl.pps;  // 边缘 pan 不钳左端：时间线前后无限
  a.dirty |= kRLanes | kRScrub;
  modal_update(a, x_time(a, mx));  // 视图变了 = 光标时刻变了，条带继续跟着抓取点
}

// 拖动提交时的让位 = Blender 出厂的 Overlap: Shuffle（strip_transform.cc:701-729）：
// 被拖的这一组横着推到最近的一侧，已有条带一格时间都不动；推完还叠着才竖着升通道。
// 导入/粘贴/复制不横推：Blender 那三处直接调 transform_shuffle_vertical
// （sequencer_add.cc:703-707、sequencer_clipboard.cc:548-550），所以它们只升通道。
static void slide_shuffle(App &a, const std::vector<Clip *> &moved) {
  if (moved.empty()) return;
  auto f0 = [&](const Clip &x) { return a.t_to_frame(x.timeline_start); };
  auto f1 = [&](const Clip &x) { return a.t_to_frame(x.timeline_start + x.duration); };
  auto in_group = [&](const Clip &x) {
    for (const Clip *m : moved)
      if (m->id == x.id) return true;
    return false;
  };
  // shuffle_frame_offset_get（:130-158）对 Left、Right 各跑一遍不动点：推量只往一个方向
  // 累积（min / max），这一轮没撞人就保持原值 ⇒ 循环必停。两侧合在一个循环里每轮清零重算
  // 会「推到位→不重叠→归零→又重叠」来回振荡，停不下来。
  // 相接不算相交（strips_overlap_at_offset:119-128）⇒ 推到位正好贴边，不留缝也不叠。
  auto frame_offset = [&](int side) {
    int64_t off = 0;
    for (;;) {
      const int64_t prev = off;
      for (const Clip *s : moved) {
        for (const Clip &o : a.project.clips) {
          if (in_group(o) || o.track != s->track) continue;
          if (f0(*s) + off >= f1(o) || f1(*s) + off <= f0(o)) continue;
          off = side < 0 ? std::min(off, f0(o) - f1(*s)) : std::max(off, f1(o) - f0(*s));
        }
      }
      if (off == prev) return off;
    }
  };
  const int64_t off_l = frame_offset(-1);
  const int64_t off_r = frame_offset(1);
  const int64_t offset = (-off_l < off_r) ? off_l : off_r;  // 取近的那一侧，一样近取左（:168）
  if (offset != 0) {
    const double dt = double(offset) * a.frame_dur();
    for (Clip *c : moved) move_clip_to(a, c, c->timeline_start + dt);
  }
  shuffle_vertical(a, moved);
}

void modal_end(App &a, bool commit) {
  if (a.modal.kind == ModalState::None) return;
  if (!commit) {  // 还原快照（Esc）。
    for (const Clip &ref : a.modal.orig)
      if (Clip *c = find_clip(a, ref.id)) {
        if (a.modal.kind == ModalState::RetimeKey) {
          // 键拖动的落点同时改曲线、条带位置和长度（首键平移整条、末键改长度），
          // 整份还原才干净；下标在拖动中不会变，所以 retime_sel 保持有效。
          *c = ref;
          continue;
        }
        c->timeline_start = ref.timeline_start;
        c->duration = ref.duration;
        c->source_in = ref.source_in;
        c->track = ref.track;  // seq_slide 的纵向分量也是这次模态改的，一起还原
        for (int ch = 0; ch < kAnimCount; ch++) c->anim[ch] = ref.anim[ch];
      }
    recompute_duration(a);
    // view2d_edge_pan_cancel（view2d_edge_pan.cc:289）：取消时连视图一起还原到进入模态时。
    a.tl.scroll_t = a.modal.edge.scroll0;
    set_status(a, L"已取消");
  } else {
    // 让位发生在提交这一刻，拖动中允许暂时叠着：Blender 也是确认后才让位
    // （special_aftertrans_update__sequencer，transform_convert_sequencer.cc:903）。
    if (a.modal.kind == ModalState::Slide) {
      std::vector<Clip *> moved;
      for (const Clip &ref : a.modal.orig) {
        // 锁定通道上的成员这一笔根本没动（modal_update 的 row_locked），它是挡路者不是
        // 让位的一方：Blender 把锁定条带整个排除在变换数据之外（:88、:480）。
        if (ref.track >= 0 && ref.track < int(a.project.tracks.size()) &&
            a.project.tracks[ref.track].locked)
          continue;
        if (Clip *c = find_clip(a, ref.id)) moved.push_back(c);
      }
      slide_shuffle(a, moved);
      recompute_duration(a);
    }
    set_status(a, L"已提交");
    undo_push(a, a.modal.kind == ModalState::Slide
                     ? L"移动"
                     : (a.modal.kind == ModalState::Slip
                            ? L"滑移"
                            : (a.modal.kind == ModalState::RetimeKey ? L"移动变速关键帧"
                                                                     : L"延长")));
  }
  for (const Clip &ref : a.modal.orig)
    if (Clip *c = find_clip(a, ref.id)) {
      trace_keys(a, *c, commit ? L"drag" : L"drag-esc");
      if (a.modal.kind == ModalState::RetimeKey)
        trace_retime(a, *c, commit ? L"key-drag" : L"key-drag-esc");
    }
  a.modal = {};
  a.dirty |= kRLanes | kRScrub | kRViewport | kRProps;
  sync_preview(a);
}

// sequencer.snap（Shift+S，keep_offset=True）：把选中条带的左缘贴到当前帧，
// 保持与相邻条带的间隙不变 —— Blender 的实现是就近吸附到当前帧。
void snap_selected_to_frame(App &a) {
  for (Clip *c : selected_clips(a)) {
    if (c->locked) continue;
    double d = a.project.playhead - c->timeline_start;
    if (std::fabs(d) > 2.0 * a.frame_dur() * 10) continue;  // 太远不动（就近原则）
    // 两个把手都没选中 ⇒ transform_translate_strip（:710-713），关键帧一起平移。
    move_clip_to(a, c, a.project.playhead);
  }
  recompute_duration(a);
  a.dirty |= kRLanes | kRScrub;
  set_status(a, L"已吸附到当前帧");
}

// sequencer.select_all 的 exec 在键选择模式下整条改道（sequencer_select.cc:360-362 →
// sequencer_retiming_select_all_exec，sequencer_retiming.cc:1037-1086）：动作语义不变，
// 只是对象从条带换成键，而且只碰「正在显示键」的条带（:1064 的 retiming_show_keys）。
enum { kSelKeysSelect, kSelKeysDeselect, kSelKeysInvert };

static void retiming_select_keys_all(App &a, int action) {
  if (action == kSelKeysDeselect) retiming_clear_key_sel(a);  // :1059-1061
  for (const Clip &c : a.project.clips) {
    if (!c.show_retiming) continue;
    for (size_t i = 0; i < c.retiming.size(); i++) {
      const bool on = retiming_key_is_selected(a, c.id, i);
      if (action == kSelKeysSelect && !on) retiming_key_select(a, c.id, i, false, false);
      if (action == kSelKeysInvert)
        retiming_key_select(a, c.id, i, on, false);  // on ⇒ toggle 摘掉，!on ⇒ 加上
    }
  }
  a.dirty |= kRLanes | kRProps;
}

// A 是 TOGGLE：:1044-1057 先扫一遍「显示中的条带上有没有已选中的键」，有 ⇒ DESELECT。
static bool any_key_shown_and_selected(const App &a) {
  for (const Clip &c : a.project.clips) {
    if (!c.show_retiming) continue;
    for (size_t i = 0; i < c.retiming.size(); i++)
      if (retiming_key_is_selected(a, c.id, i)) return true;
  }
  return false;
}

void select_all(App &a) {
  if (retiming_keys_are_selected(a)) {
    retiming_select_keys_all(a, any_key_shown_and_selected(a) ? kSelKeysDeselect
                                                              : kSelKeysSelect);
    return;
  }
  a.sel.clear();
  for (const auto &c : a.project.clips) a.sel.push_back(c.id);
  a.selected = a.sel.empty() ? -1 : a.sel.back();
  a.dirty |= kRLanes | kRProps;
}

void select_none(App &a) {
  if (retiming_keys_are_selected(a)) {
    retiming_select_keys_all(a, kSelKeysDeselect);
    return;
  }
  // 同一个 deselect_all_strips（:383 那条分支）：只清标志，活动条带留着。
  a.sel.clear();
  a.dirty |= kRLanes | kRProps;
}

void invert_selection(App &a) {
  if (retiming_keys_are_selected(a)) {
    retiming_select_keys_all(a, kSelKeysInvert);
    return;
  }
  std::vector<int64_t> out;
  for (const auto &c : a.project.clips)
    if (!clip_selected(a, c.id)) out.push_back(c.id);
  a.sel = out;
  a.selected = a.sel.empty() ? -1 : a.sel.back();
  a.dirty |= kRLanes | kRProps;
}

// sequencer.select_side_of_frame
void select_side_of_frame(App &a, bool right_side) {
  clear_selection(a);
  for (const auto &c : a.project.clips) {
    bool hit = right_side ? c.timeline_start >= a.project.playhead : c.timeline_start + c.duration <= a.project.playhead;
    if (hit) a.sel.push_back(c.id);
  }
  a.selected = a.sel.empty() ? -1 : a.sel.front();
  a.dirty |= kRLanes | kRProps;
}

// sequencer.select_more / select_less（Ctrl+Numpad+ / Ctrl+Numpad-）。
// 逐段对应 sequencer_select.cc:1460-1498：只看「同一通道、正好接在左右两侧」的邻居
// （find_neighboring_strip :293-327 用 channel 相等 + left_handle==右邻的 right_handle 判定）。
// more = 从已选条带出发，把未选的邻居加入选中；less = 从未选条带出发，把已选的邻居剔除。
void select_more_less(App &a, bool more) {
  const double fd = a.frame_dur();
  auto right_edge = [&](const Clip &c) { return c.timeline_start + c.duration; };
  auto neighbor_of = [&](const Clip &c, bool left_side) -> Clip * {
    for (auto &o : a.project.clips) {
      if (&o == &c || o.track != c.track) continue;
      bool selected = clip_selected(a, o.id);
      if (more ? selected : !selected) continue;  // more 只要未选的，less 只要已选的
      if (left_side ? std::fabs(c.timeline_start - right_edge(o)) <= fd * 0.5
                    : std::fabs(right_edge(c) - o.timeline_start) <= fd * 0.5)
        return &o;
    }
    return nullptr;
  };
  std::vector<int64_t> add;
  for (const Clip &c : a.project.clips) {
    if (clip_selected(a, c.id) != more) continue;  // more 从已选出发，less 从未选出发
    for (bool left : {true, false}) {
      Clip *n = neighbor_of(c, left);
      if (n && std::find(add.begin(), add.end(), n->id) == add.end()) add.push_back(n->id);
    }
  }
  if (add.empty()) {
    set_status(a, more ? L"没有可多选的相邻条带" : L"没有可少选的相邻条带");
    return;
  }
  for (int64_t id : add) {
    if (more) add_to_selection(a, id);
    else {
      a.sel.erase(std::remove(a.sel.begin(), a.sel.end(), id), a.sel.end());
      if (a.selected == id) a.selected = a.sel.empty() ? -1 : a.sel.back();
    }
  }
  a.dirty |= kRLanes | kRProps;
}

// sequencer.swap（Alt+Left / Alt+Right）：与同通道上最近的左/右邻居交换位置
// （sequencer_edit.cc:3633-3646 的 swap_strips —— 两条带各取对方的起始帧，长度保持不变）。
void swap_with_neighbor(App &a, bool right_side) {
  Clip *act = props_clip(a);
  if (!act) {
    set_status(a, L"没有活动条带");
    return;
  }
  if (act->locked) {
    set_status(a, L"通道或条带已锁定");
    return;
  }
  Clip *best = nullptr;
  double best_dist = 1e30;
  for (auto &c : a.project.clips) {
    if (&c == act || c.track != act->track || c.locked) continue;
    double dist = right_side ? c.timeline_start - (act->timeline_start + act->duration)
                             : act->timeline_start - (c.timeline_start + c.duration);
    if (right_side ? dist < -1e-6 : dist > 1e-6) continue;  // 只在指定一侧找
    if (std::fabs(dist) < best_dist) {
      best_dist = std::fabs(dist);
      best = &c;
    }
  }
  if (!best) {
    set_status(a, right_side ? L"右侧没有可交换的条带" : L"左侧没有可交换的条带");
    return;
  }
  std::swap(act->timeline_start, best->timeline_start);
  recompute_duration(a);
  a.dirty |= kRLanes | kRScrub | kRViewport;
  sync_preview(a);
  set_status(a, L"已与相邻条带交换位置");
}

// sequencer.select_box（B）：矩形与条带相交即选中。
// Blender 的实现同样是「求交」（sequencer_select.cc 的 select_box，use_include_handles
// 只影响把手区域是否算进去，本工程不建模把手）。
void select_box(App &a, Rect box, bool extend) {
  if (!extend) clear_selection(a);
  double t0 = x_time(a, std::min(box.x, box.x + box.w));
  double t1 = x_time(a, std::max(box.x, box.x + box.w));
  float y0 = std::min(box.y, box.y + box.h), y1 = std::max(box.y, box.y + box.h);
  for (const auto &c : a.project.clips) {
    if (c.timeline_start > t1 || c.timeline_start + c.duration < t0) continue;
    bool hit_row = false;
    for (const TrackRow &r : track_rows(a)) {
      if (r.track != c.track) continue;
      if (r.y + r.h >= y0 && r.y <= y1) hit_row = true;
    }
    if (!hit_row) continue;
    if (extend) {
      if (!clip_selected(a, c.id)) a.sel.push_back(c.id);
    } else if (std::find(a.sel.begin(), a.sel.end(), c.id) == a.sel.end()) {
      a.sel.push_back(c.id);
    }
    // 框到的每一条都要把选择抄给它所在的连接组（sequencer_select.cc:2072-2078）。
    // 键位表里只有 Alt+拖框那一条绑定带了 ignore_connections=True（blender_default.py:3188），
    // 默认的 B 框选照抄连接。
    propagate_selection(a, c.id);
  }
  a.selected = a.sel.empty() ? -1 : a.sel.back();
  trace_links(a, L"box");
  a.dirty |= kRLanes | kRProps;
}

void copy_selected(App &a) {
  a.clipboard.clear();
  for (Clip *c : selected_clips(a)) a.clipboard.push_back(*c);
  set_status(a, a.clipboard.empty() ? L"未选中条带" : L"已复制");
}

void paste_clips(App &a) {
  if (a.clipboard.empty()) {
    set_status(a, L"剪贴板为空");
    return;
  }
  a.sel.clear();
  const double base = a.clipboard.front().timeline_start;
  // 先全部复制，再一起摆到播放头，最后整组做一次让位：粘贴落在已有条带上时动的是副本，
  // 原来那批一格时间都不动。
  std::vector<int64_t> ids;
  std::vector<std::pair<int64_t, int64_t>> id_map;  // 剪贴板原型 id → 副本 id
  for (const Clip &src : a.clipboard) {
    Clip c = src;
    id_map.push_back({src.id, a.next_id});
    c.id = a.next_id++;
    a.project.clips.push_back(c);
    ids.push_back(c.id);
  }
  // 副本的连接名单还原样指着老条带：换引用 + 剪单向，与「复制条带」同一处。
  // 少了这一步，点副本会顺着单向引用把原型也选进来，移动/切分两份一起动。
  remap_copied_links(a, id_map);
  std::vector<Clip *> fresh;
  for (size_t i = 0; i < ids.size(); i++) {
    // push_back 可能整体搬家，所以指针在全部复制完之后按 id 重新取。
    Clip *np = find_clip(a, ids[i]);
    // 平移这一步走 move_clip_to，关键帧跟着条带一起走。
    move_clip_to(a, np, a.project.playhead + (a.clipboard[i].timeline_start - base));
    fresh.push_back(np);
    a.sel.push_back(np->id);
  }
  shuffle_vertical(a, fresh);
  a.selected = a.sel.back();
  recompute_duration(a);
  a.dirty |= kRLanes | kRProps | kRViewport;
  sync_preview(a);
  trace_links(a, L"paste");
  set_status(a, L"已粘贴到播放头");
}

// 通道在 Blender 里恒存在（channels.cc:30-40 一次建满 128 条，默认名 DATA_("Channel %d")），
// 本工程的 Track 是按需补出来的：点空通道的图标或改名时才真正建这一条。
Track &channel_slot(App &a, int idx) {
  while (int(a.project.tracks.size()) <= idx) {
    Track t;
    t.index = int(a.project.tracks.size());
    t.name = std::to_wstring(t.index + 1);
    a.project.tracks.push_back(t);
  }
  a.dirty = kRegionAll;
  return a.project.tracks[idx];
}

// sequencer.strip_jump（PgUp / PgDn，Alt 版 center=True）：
// 逐段对应 sequencer_edit.cc:3545-3561 → strip_time.cc:244-310 的 time_find_next_prev_edit。
// center=False 时候选 = 每条条带的左右两个把手；center=True 时候选 = 每条的中心帧；
// 取「时间距离最近」的那一个（不是通道最高），静音条带跳过（do_skip_mute 恒为 true）。
void strip_jump(App &a, bool next, bool center) {
  const int64_t cfra = a.cur_frame();
  int64_t best = cfra, best_dist = INT64_MAX;
  for (const auto &c : a.project.clips) {
    if (c.muted) continue;
    int64_t cand[2];
    int n = 0;
    int64_t lh = a.t_to_frame(c.timeline_start);
    int64_t rh = a.t_to_frame(c.timeline_start + c.duration);
    if (center) {
      cand[n++] = (lh + rh) / 2;
    } else {
      cand[n++] = lh;
      cand[n++] = rh;
    }
    for (int i = 0; i < n; ++i) {
      int64_t dist = next ? cand[i] - cfra : cfra - cand[i];
      if (dist <= 0) continue;
      if (dist < best_dist) {
        best_dist = dist;
        best = cand[i];
      }
    }
  }
  if (best == cfra) return;
  a.set_frame(best);
  if (a.playing) toggle_play(a);
  a.dirty |= kRLanes | kRScrub | kRViewport;
  sync_preview(a);
}

// sequencer.ripple_trim：把条带右侧（或左侧）全部内容删掉。
void ripple_trim(App &a, bool left) {
  std::vector<Clip *> cs = selected_clips(a);
  if (cs.empty()) {
    set_status(a, L"未选中条带");
    return;
  }
  for (Clip *c : cs) {
    if (left) {
      if (a.project.playhead > c->timeline_start && a.project.playhead < c->timeline_start + c->duration) {
        double d = a.project.playhead - c->timeline_start;
        c->timeline_start = a.project.playhead;
        c->source_in = retiming_move_left_edge(*c, d);
        c->duration -= d;
      }
    } else if (a.project.playhead > c->timeline_start && a.project.playhead < c->timeline_start + c->duration) {
      c->duration = a.project.playhead - c->timeline_start;
    }
  }
  recompute_duration(a);
  a.dirty |= kRLanes | kRViewport;
  set_status(a, left ? L"已向右涟漪裁切" : L"已向左涟漪裁切");
}

// sequencer.set_range_to_strips（sequencer_edit.cc:4534-4577）：范围 = 选中条带的并集，
// efra 要减一，因为每帧占一格宽：1..1 会画成 1..2。
// 没选中 ⇒ CANCELLED（Blender 报 "Select one or more strips"）；本工程不建模 handle 外延，
// 也不做 preview 分支（SCER_PRV_RANGE），所以只写 sfra/efra 两个值。
void range_to_strips(App &a) {
  const std::vector<Clip *> cs = selected_clips(a);
  if (cs.empty()) {
    set_status(a, L"先选中一条或多条带");
    return;
  }
  int sfra = INT32_MAX, efra = INT32_MIN;
  for (const Clip *c : cs) {
    sfra = std::min(sfra, int(a.t_to_frame(c->timeline_start)));
    efra = std::max(efra, int(a.t_to_frame(c->timeline_start + c->duration)) - 1);
  }
  if (efra < 0) {
    set_status(a, L"无法设为负范围");
    return;
  }
  a.project.fmt.frame_start = std::max(0, sfra);
  a.project.fmt.frame_end = efra;
  if (trace_fp) {
    wchar_t t[96];
    swprintf(t, 96, L"range strips sfra=%d efra=%d（%d 条）", a.project.fmt.frame_start,
             a.project.fmt.frame_end, int(cs.size()));
    trace(t);
  }
  undo_push(a, L"设为条带范围");
  a.dirty = kRegionAll;
  sync_preview(a);
  wchar_t buf[96];
  swprintf(buf, 96, L"帧范围 → %d…%d", a.project.fmt.frame_start, a.project.fmt.frame_end);
  set_status(a, buf);
}

void nudge(App &a, double delta_seconds) {  // screen_ops.cc:3913 的 FRAMENUMBER_MIN_CLAMP：只挡下界，上不封顶。
  const double next = std::max(0.0, a.project.playhead + delta_seconds);
  if (next == a.project.playhead) return;
  a.project.playhead = next;
  a.dirty |= kRLanes | kRScrub;
}

// 播放时每一 tick 写播放头的只有这一格。时钟取哪一个由状态先决定，再让画面和声音去读：
// 音频受理过一段（正在发声、或正在建立）就以发声位置/受理点为准，画面绝不跑在声音前面；
// 音频无话（播到空隙、还没出声、刚停）才按界面时间自己走。回卷和取时钟在同一格里做完，
// 免得回卷之后又被上一段的发声位置拽回去。
void playhead_advance(App &a, double dt) {
  const double pos = a.audio.position();
  nudge(a, pos >= 0.0 ? pos - a.project.playhead : dt);
  // BKE_scene_frame_clamp_for_playback（scene.cc:2615-2632）：cfra 越过 efra 就回到 sfra，
  // 一圈是 efra-sfra+1 帧，正好等于导出长度。
  double t0 = 0.0, t1 = 0.0;
  playback_bounds(a, &t0, &t1);
  if (a.project.playhead >= t1) a.project.playhead = t0;
}

void jump_frame(App &a, int64_t frame) {
  a.set_frame(std::max<int64_t>(0, frame));
  if (a.playing) toggle_play(a);
  follow_playhead(a);
  a.dirty |= kRLanes | kRScrub | kRViewport;
  sync_preview(a);
}

// ED_areas_do_frame_follow（screen_ops.cc:3986）：跳帧之后让播放头回到视野里。
// 竖线已经在可见范围内时不动视野，跳出左/右边缘时把它分别贴到左右边缘。
void follow_playhead(App &a) {
  const double x = time_x(a, a.project.playhead);
  if (x < a.lanes.x) {
    a.tl.scroll_t = a.project.playhead - btheme::u(0.5f) / a.tl.pps;
  } else if (x > a.lanes.x + a.lanes.w) {
    a.tl.scroll_t = a.project.playhead - (double(a.lanes.w) - btheme::u(0.5f)) / a.tl.pps;
  }
}

// screen.keyframe_jump：跳到相邻有画面的条带起点/终点（这里等价于条带边界）。
void keyframe_jump(App &a, bool next) {
  strip_jump(a, next);
}

// 预览区里的方向键 = 把选中条带的位置挪一格。条件都长在对象身上：指针落在预览区里、
// 有一条带正在编辑、而且它有画面（声音条带没有画面，也就没有可挪的东西）。都满足就写
// 位置 X/Y（1 输出像素，和拖取景框同一条路、同一个单位），否则返回 false，把这一格按键
// 交回原来的走帧 / 走条带边界。
// 一次只走一轴，所以位置锁（「改一轴，另一轴跟同样的增量」）不参与：方向是用户按出来的，
// 让另一轴跟着走会把「往上」变成「斜着往上」。
static bool strip_nudge(App &a, int dx, int dy) {
  if (!a.viewport.contains(float(a.mouse_x), float(a.mouse_y))) return false;
  Clip *c = props_clip(a);
  if (!c || c->kind == kStripSound) return false;  // 声音条带没有画面，和它那只不存在的取景框一致
  const bool ok_x = dx == 0 || prop_apply(a, kFOfsX, c, prop_value(a, kFOfsX, c) + dx, false);
  const bool ok_y = dy == 0 || prop_apply(a, kFOfsY, c, prop_value(a, kFOfsY, c) + dy, false);
  const bool moved = (dx != 0 || dy != 0) && (ok_x || ok_y);
  if (moved) undo_push(a, L"移动位置");
  return moved;
}

void toggle_play(App &a) {
  a.playing = !a.playing;
  a.last_tick = now_seconds();
  a.dirty = kRegionAll;
  if (!a.playing) {
    a.audio.stop();
    return;
  }
  // 起播时先把播放头夹进播放范围（screen_edit.cc:1983 的同一个调用）。
  double t0 = 0.0, t1 = 0.0;
  playback_bounds(a, &t0, &t1);
  const double before = a.project.playhead;
  if (a.project.playhead < t0 || a.project.playhead >= t1) a.project.playhead = t0;
  if (trace_fp) {
    wchar_t b[160];
    swprintf(b, 160, L"play on ph=%.3f->%.3f bounds=%.3f..%.3f f=%lld..%lld", double(before),
             double(a.project.playhead), t0, t1, (long long)a.project.fmt.frame_start,
             (long long)a.project.fmt.frame_end);
    trace(b);
  }
}

void set_aspect(App &a, int num, int den) {
  int long_side = std::max(1, std::max(a.project.fmt.width, a.project.fmt.height));
  if (num >= den) {
    a.project.fmt.width = long_side;
    a.project.fmt.height = int(std::llround(double(long_side) * den / num));
  } else {
    a.project.fmt.height = long_side;
    a.project.fmt.width = int(std::llround(double(long_side) * num / den));
  }
  a.project.fmt.par_x = 1;
  a.project.fmt.par_y = 1;
  wchar_t buf[80];
  swprintf(buf, 80, L"画幅 %d:%d  →  %d×%d", num, den, a.project.fmt.width, a.project.fmt.height);
  set_status(a, buf);
  a.dirty = kRegionAll;
  sync_preview(a);
}

// scripts/presets/framerate/*.py：预设写的就是 fps（整数）与 fps_base 两个值，
// 23.98/29.97/59.94 用 base=1.001，其余 base=1。
void set_fps_preset(App &a, int fps, double base) {
  a.project.fmt.fps = fps;
  a.project.fmt.fps_base = base > 0 ? base : 1.0;
  wchar_t buf[64];
  swprintf(buf, 64, L"帧率 %d / %.3f = %.4g fps", fps, base, a.project.fmt.fps_value());
  set_status(a, buf);
  a.dirty = kRegionAll;
  sync_preview(a);
}

// 导出质量档的名字 = ffmpeg.constant_rate_factor 的枚举项（rna_scene.cc:6950-6952），
// 数值就是 eFFMpegCrf 里的 CRF（DNA_scene_types.h:104-106）。菜单项和按钮文字共用这一份，
// 因为 Blender 里它们本来就是同一个 enum 的同一个显示名。需求只留这三档。
const wchar_t *crf_name(int crf) {
  switch (crf) {
    case -1: return L"恒定码率";   // FFM_CRF_NONE：不用常量质量，交给码率控制
    case 0:  return L"无损（CRF 0）";
    case 17: return L"感知无损（CRF 17）";
    default: return nullptr;  // 不在表里 = RNA 的 CUSTOM 档
  }
}

// 不在表里的数字 = RNA 的 CUSTOM 档，界面上显示成「自定义（CRF n）」。行上的按钮文字和
// 状态条这一句都从这里出，所以这串格式全工程只写一次。
std::wstring crf_label(int crf) {
  const wchar_t *n = crf_name(crf);
  if (n) return n;
  wchar_t buf[40];
  swprintf(buf, 40, L"自定义（CRF %d）", crf);
  return buf;
}

// 导出质量：只改这一档的枚举值，导出时按它决定发不发 -crf。
void set_crf(App &a, int crf) {
  a.project.fmt.crf = crf;
  set_status(a, std::wstring(L"输出质量 ") + crf_label(crf));
  a.dirty = kRProps;
}

// 编码速度：名字和 x264 的 -preset 都从 OutputFormat 那张表读，这里只写数值。
void set_speed(App &a, int preset) {
  a.project.fmt.speed = preset;
  auto sp = OutputFormat::speed_of(preset);
  wchar_t buf[64];
  swprintf(buf, 64, L"编码速度 %s（-preset %s）", sp.label, sp.x264);
  set_status(a, buf);
  a.dirty = kRProps;
}

// 音频：AAC ↔ NONE，写的是同一个布尔，两档各有一个命令（菜单里的两项都点得动，
// 不需要「切换」这种按下才知道结果的动作）。状态条报的就是行上那一格显示的名字。
void set_audio(App &a, bool none) {
  a.project.fmt.no_audio = none;
  set_status(a, std::wstring(L"音频 ") + OutputFormat::audio_of(none));
  a.dirty = kRProps;
}

// ---------------------------------------------------------------- 对话框 / 导出

void open_dialog(App &a) {
  IFileOpenDialog *dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
  COMDLG_FILTERSPEC filters[] = {{L"视频", L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.mts;*.wmv;*.flv"},
                                 {L"所有文件", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
  if (SUCCEEDED(dlg->Show(a.hwnd))) {
    IShellItemArray *items = nullptr;
    std::vector<std::wstring> paths;
    if (SUCCEEDED(dlg->GetResults(&items))) {
      DWORD count = 0;
      items->GetCount(&count);
      for (DWORD i = 0; i < count; ++i) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item))) {
          PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
          }
          item->Release();
        }
      }
      items->Release();
    }
    import_paths(a, paths);
  }
  dlg->Release();
}

void start_export(App &a, const std::wstring &path) {
  std::wstring err;
  a.export_reported = false;
  if (a.exporter.start(a.project, path, &err)) {
    set_status(a, L"导出中…");
    trace(L"export started -> " + path);
  } else {
    set_status(a, L"导出失败：" + err);
    a.export_reported = true;
    trace(L"export refused: " + err);
  }
}

void export_dialog(App &a) {
  IFileSaveDialog *dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
  COMDLG_FILTERSPEC filters[] = {{L"MP4 (H.264/AAC)", L"*.mp4"}, {L"所有文件", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  dlg->SetOptions(FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
  dlg->SetDefaultExtension(L"mp4");
  // 文件名只在保存对话框里写，左侧面板不再占一格；这里只是初始建议名。
  dlg->SetFileName(L"未命名");
  // 起点 = 本次会话第一次打开文件的那个目录（需求：打开文件等于导出的路径）。
  if (!a.open_dir.empty()) {
    IShellItem *folder = nullptr;
    const HRESULT hr = SHCreateItemFromParsingName(a.open_dir.c_str(), nullptr,
                                                   IID_PPV_ARGS(&folder));
    if (SUCCEEDED(hr)) {
      dlg->SetFolder(folder);
      folder->Release();
    }
    wchar_t wb[512];
    swprintf(wb, 512, L"export dialog: 起点目录=%ls（SetFolder hr=%08X）",
             a.open_dir.c_str(), unsigned(hr));
    trace(wb);
  } else {
    trace(L"export dialog: 还没打开过文件 ⇒ 用系统默认目录");
  }
  if (SUCCEEDED(dlg->Show(a.hwnd))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        start_export(a, path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
}

// ---------------------------------------------------------------- 窗口命令
//
// wm.window_fullscreen_toggle（TOPBAR_MT_window，space_topbar.py:677）：普通态的样式与
// 矩形先记住，再切成盖住整个显示器的无边框窗口；第二次按原样还原。
static void toggle_fullscreen(App &a) {
  static LONG style0 = 0;
  static RECT rect0{};
  if (!a.hwnd) return;
  if (!a.fullscreen) {
    style0 = GetWindowLong(a.hwnd, GWL_STYLE);
    GetWindowRect(a.hwnd, &rect0);
    a.fullscreen = true;
    SetWindowLong(a.hwnd, GWL_STYLE, style0 & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                                                 WS_MAXIMIZEBOX));
    MONITORINFO mi{sizeof(mi), {}, {}, 0};
    GetMonitorInfo(MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(a.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED);
  } else {
    a.fullscreen = false;
    SetWindowLong(a.hwnd, GWL_STYLE, style0);
    SetWindowPos(a.hwnd, HWND_TOP, rect0.left, rect0.top, rect0.right - rect0.left,
                 rect0.bottom - rect0.top, SWP_FRAMECHANGED);
  }
}

// screen.screenshot（TOPBAR_MT_window:690 的 "Save Screenshot…"）：先问路径，再把文件名
// 交给绘制循环，下一帧画完就地读回后备缓冲写 PNG —— 与命令行的 --shot 走同一条通道。
static void screenshot_dialog(App &a) {
  IFileSaveDialog *dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&dlg))))
    return;
  COMDLG_FILTERSPEC filters[] = {{L"PNG 图片", L"*.png"}, {L"所有文件", L"*.*"}};
  dlg->SetFileTypes(2, filters);
  dlg->SetOptions(FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
  dlg->SetDefaultExtension(L"png");
  dlg->SetFileName(L"截图");
  if (SUCCEEDED(dlg->Show(a.hwnd))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        a.shot_path = path;
        CoTaskMemFree(path);
      }
      item->Release();
      if (a.shot_path.size() < 4 || a.shot_path.compare(a.shot_path.size() - 4, 4, L".png") != 0)
        a.shot_path += L".png";
      a.shot_deadline = now_seconds();
      a.shot_quit_after = false;  // 菜单里点的截图截完继续留着窗口
      set_status(a, L"已保存截图：" + a.shot_path);
    }
  }
  dlg->Release();
}

// ---------------------------------------------------------------- 吸附
//
// 阈值按需求定死：8 屏幕像素（不是 Blender 的 snap_distance = 15，sequencer.cc:384）。
// 整数帧对齐在调用方做，这里只负责「在候选里挑一个最近的」。
// 两种拖动的候选集与优先级不同：
//   拖片段：吸附对象是片段的起止边，候选 = 播放头 + 其它片段的起止边，同距离播放头优先；
//   拖播放头：候选 = 片段起止边 + 帧范围入点/出点，同距离帧范围优先。
// 总开关与分项开关按需求都不做（头部那颗磁铁和它的 popover 删了）：候选集就是上面
// 那两条固定规则，唯一的开关是拖动时按住的 Ctrl（反过来）。
double snap_time(App &a, float x, double t, const Clip *self, SnapWhat what) {
  const float kSnapPx = 8.0f * btheme::scale;
  // 8 屏幕像素是硬上限：超出就一定不吸，不许用判等余量把它放宽。
  double best = t;
  float best_px = kSnapPx + 1e-3f;
  int best_prio = -1;
  auto consider = [&](double s, int prio) {
    const float d = std::fabs(time_x(a, s) - x);
    if (d > kSnapPx) return;
    // 差半个像素以内算「距离相同」，这时才比优先级。
    if (d > best_px - 0.5f * btheme::pixelsize && prio <= best_prio) return;
    best_px = d;
    best_prio = prio;
    best = s;
  };
  const OutputFormat &f = a.project.fmt;
  const double rng_in = a.frame_to_t(int64_t(f.frame_start));
  const double rng_out = a.frame_to_t(int64_t(f.frame_end));
  auto add_clips = [&](int prio) {
    for (const auto &o : a.project.clips) {
      if (self && o.id == self->id) continue;  // 不吸附被拖片段自身
      consider(o.timeline_start, prio);
      consider(o.timeline_start + o.duration, prio);
    }
  };
  if (what == kSnapFrame) {
    consider(rng_in, 2);
    consider(rng_out, 2);
    add_clips(1);
  } else {
    // 拖片段：需求写的候选是「播放头、其它片段起止边」，帧范围点不在其中。
    consider(a.project.playhead, 2);
    add_clips(1);
  }
  if (trace_fp && best != t) {  // 只在真的吸上了才记一笔，否则每次 MOUSEMOVE 都会刷一行
    wchar_t b[128];
    swprintf(b, 128, L"snap %ls x=%.0f %.4f->%.4f (%.1fpx)",
             what == kSnapFrame ? L"frame" : L"strip", x, t, best,
             std::fabs(time_x(a, best) - x));
    trace(b);
  }
  return best;
}

// 拖片段：需求写明「吸附对象：被拖片段的开始边缘和结束边缘」，两条边都要参与，
// 谁离候选更近就用谁的偏移。这里对两条边各求一次最优吸附再比距离。
double snap_strip_move(App &a, const Clip &c, double new_start) {
  const double end = new_start + c.duration;
  const double s = snap_time(a, float(time_x(a, new_start)), new_start, &c, kSnapStrip);
  const double e = snap_time(a, float(time_x(a, end)), end, &c, kSnapStrip);
  const double ds = std::fabs(time_x(a, s) - time_x(a, new_start));
  const double de = std::fabs(time_x(a, e) - time_x(a, end));
  // 没吸上的那条边位移是 0，不能拿它和「吸上了」的那条比大小，否则永远选到没吸的那条。
  const bool sm = s != new_start, em = e != end;
  if (sm && em) return ds <= de ? s : e - c.duration;
  if (sm) return s;
  if (em) return e - c.duration;
  return new_start;
}

// ---------------------------------------------------------------- 文本输入

// Blender 的数值按钮单击进入输入态：原值全选，输入即替换，Enter 提交、Esc 放弃
// （interface_handlers.cc 的 ui_number_field_*）。
void commit_edit(App &a, bool apply) {
  int id = a.edit_id;
  std::wstring text = a.edit_text;
  a.edit_id = 0;
  a.edit_text.clear();
  a.edit_ts = {};
  a.edit_anchor = -1;
  // 空串对数值框 = 「什么都没填」，等于放弃；对内容框它是一个合法值（清空就是让字消失）。
  if (apply && (is_string_field(id) || !text.empty())) {
    if (is_string_field(id))
      text_content_set(a, text);
    else
      prop_apply(a, id, props_clip(a), wcstod(text.c_str(), nullptr));
  }
  a.dirty = kRegionAll;
  undo_push(a, L"修改属性");
}

// SEQ_OT_rename_channel（sequencer_channels_edit.cc）：Ctrl+左键或双击轨道名进入编辑。
void commit_rename(App &a, bool apply) {
  int t = a.rename_track;
  std::wstring text = a.rename_text;
  a.rename_track = -1;
  a.rename_text.clear();
  a.edit_ts = {};
  a.edit_anchor = -1;
  if (apply && t >= 0 && !text.empty()) channel_slot(a, t).name = text;
  a.dirty |= kRChan | kRLanes;
  undo_push(a, L"重命名通道");
}

// ---------------------------------------------------------------- 变速关键帧

// SEQUENCER_OT_retiming_key_add（sequencer_retiming.cc:214-237）：帧号没有显式给定时
// 取 BKE_scene_frame_get，也就是当前帧。作用对象先看选中的条带
// （retiming_key_add_from_selection :169-187），一条都没有时退到「键被选中的那些条带」
// （retiming_key_add_to_editable_strips :189-207）—— 键选择模式下条带本身是被取消选中的，
// 少了这条退路，点完菱形再按 I 就只会报「未选中条带」。
// 键值取曲线在该处的当前求值（strip_retiming.cc:430-482 的
// `float value = strip_retiming_evaluate(strip, frame_index)`），插下去画面不跳。
// 成功后 Blender 顺手置 SEQ_SHOW_RETIMING（:185），所以加完键一定看得见。
static void retiming_key_add(App &a) {
  std::vector<Clip *> cs = selected_clips(a);
  if (cs.empty()) {
    for (const auto &s : a.retime_sel) {
      Clip *c = find_clip(a, s.first);
      if (c && std::find(cs.begin(), cs.end(), c) == cs.end()) cs.push_back(c);
    }
  }
  if (cs.empty()) {
    set_status(a, L"未选中条带");
    return;
  }
  int added = 0;
  for (Clip *c : cs) {
    if (c->locked) continue;  // SEQ_LOCK 的条带不参与编辑
    if (!retiming_is_allowed(*c, a.frame_dur())) continue;  // :180 的 retiming_is_allowed
    if (retiming_add_key(*c, a.project.playhead - c->timeline_start)) {
      c->show_retiming = true;
      added++;
      trace_retime(a, *c, L"key-add");
    }
  }
  if (added == 0) {
    set_status(a, L"这个帧上没法加键：播放头要在条带内部，且该处还没有键");
    return;
  }
  // 插键会在数组中间挪位置，按下的那几把键的下标全部跟着漂；Blender 存的是裸指针，
  // 那里同样一插就作废，所以这里直接清空选择。
  retiming_clear_key_sel(a);
  a.dirty |= kRLanes | kRProps;
  wchar_t b[96];
  // 下一步就是 Blender 的按键流程：菱形点一下选中它，R =
  // SEQUENCER_OT_retiming_segment_speed_set（blender_default.py:3294）改那一段的速度。
  swprintf(b, 96, L"已添加 %d 把变速关键帧：点条带下沿的菱形选中它，按 R 改这一段的速度", added);
  set_status(a, b);
}

// SEQUENCER_OT_retiming_show（sequencer_retiming.cc:72-115）：作用集合 = 选中的条带
// （滤掉不可变速的）∪ 键被选中且正在显示键的那些条带；集合里全在显示 ⇒ 全部隐藏，
// 否则全部显示（:93-95 的 all_of 判断）。隐藏时顺手把条带选中（:100-102 的注释：
// 可能本来只有键被选中，这样用户可以立刻切回条带模式），显示时反过来把条带的选中摘掉
// （:106-109：两者不能同时成立）。
static void retiming_show(App &a) {
  std::vector<Clip *> cs = selected_clips(a);
  std::vector<Clip *> owners;  // selected_keys（:80-87）
  for (const auto &s : a.retime_sel) {
    Clip *c = find_clip(a, s.first);
    if (!c) continue;
    if (std::find(owners.begin(), owners.end(), c) == owners.end()) owners.push_back(c);
    if (c->show_retiming && std::find(cs.begin(), cs.end(), c) == cs.end()) cs.push_back(c);
  }
  cs.erase(std::remove_if(cs.begin(), cs.end(),
                          [&](const Clip *c) { return !retiming_is_allowed(*c, a.frame_dur()); }),
           cs.end());
  if (cs.empty()) {
    set_status(a, L"未选中条带");
    return;
  }
  const bool all_show =
      std::all_of(cs.begin(), cs.end(), [](const Clip *c) { return c->show_retiming; });
  for (Clip *c : cs) {
    c->show_retiming = !all_show;
    const bool is_key_owner = std::find(owners.begin(), owners.end(), c) != owners.end();
    if (all_show) {
      if (!clip_selected(a, c->id)) a.sel.push_back(c->id);
    } else if (is_key_owner) {
      a.sel.erase(std::remove(a.sel.begin(), a.sel.end(), c->id), a.sel.end());
    }
  }
  a.dirty |= kRLanes | kRProps;
  set_status(a, all_show ? L"已隐藏变速关键帧" : L"已显示变速关键帧");
}

// ---- 变速键的选择 ----------------------------------------------------
// 存在 App 里而不是 Editing/工程里，但语义对齐 Blender 的全局 selection 表：
// 一把键 = (条带 id, 键下标)，跨条带可以有多把。

bool retiming_keys_are_selected(const App &a) {
  return !a.retime_sel.empty();
}

bool retiming_key_is_selected(const App &a, int64_t clip_id, size_t key) {
  for (const auto &s : a.retime_sel)
    if (s.first == clip_id && s.second == key) return true;
  return false;
}

void retiming_clear_key_sel(App &a) {
  if (a.retime_sel.empty()) return;
  a.retime_sel.clear();
  a.dirty |= kRLanes;
}

// 条带被删、键数组被重排之后，(条带, 下标) 里可能有几把已经不存在了。Blender 存的是键数组
// 里的裸指针（数组长度和下标都不变，只是重新分配过），它自己也会在下一次点击时把选择清掉；
// 我们按下标存，所以结构一变就把失效的那几份摘掉，别让 R / X 指向一颗不存在的键。
void retiming_prune_sel(App &a) {
  const size_t n0 = a.retime_sel.size();
  a.retime_sel.erase(std::remove_if(a.retime_sel.begin(), a.retime_sel.end(),
                                    [&a](const std::pair<int64_t, size_t> &s) {
                                      const Clip *k = find_clip(a, s.first);
                                      return !k || s.second >= k->retiming.size();
                                    }),
                     a.retime_sel.end());
  if (a.retime_sel.size() != n0) a.dirty |= kRLanes | kRProps;
}

void retiming_key_select(App &a, int64_t clip_id, size_t key, bool toggle, bool deselect_all) {
  if (deselect_all) a.retime_sel.clear();
  auto it = std::find_if(a.retime_sel.begin(), a.retime_sel.end(), [&](const auto &s) {
    return s.first == clip_id && s.second == key;
  });
  if (toggle && it != a.retime_sel.end()) a.retime_sel.erase(it);
  else if (it == a.retime_sel.end()) a.retime_sel.push_back({clip_id, key});
  a.dirty |= kRLanes | kRProps;
}

// 键的曲线一改（加、删、滑移、裁边、拆分）下标就可能失效，统一由调用处清一次。
bool retiming_delete_selected_keys(App &a) {
  if (a.retime_sel.empty()) return false;
  std::vector<std::pair<int64_t, size_t>> sel = a.retime_sel;
  a.retime_sel.clear();
  // 同一条带的键按下标从大到小删，前面的下标才不会被后面的删除动作挪位。
  std::sort(sel.begin(), sel.end(), [](const auto &x, const auto &y) {
    return x.first == y.first ? x.second > y.second : x.first < y.first;
  });
  int removed = 0;
  for (const auto &s : sel) {
    Clip *c = find_clip(a, s.first);
    if (!c) continue;
    if (retiming_remove_key_index(*c, s.second)) {
      removed++;
      trace_retime(a, *c, L"key-del");
    }
  }
  if (removed == 0) {
    set_status(a, L"选中的是条带两端的键，删不掉（Blender 同样只删中间那几把）");
    return true;
  }
  recompute_duration(a);
  a.dirty |= kRLanes | kRProps | kRViewport;
  sync_preview(a);
  undo_push(a, L"删除变速关键帧");
  return true;
}

// ---------------------------------------------------------------- 命令表

// 种类按钮点下去 = 面板换到这一种，无条件、不拒绝（裁决：放开自由使用）。时间线里有这一种
// 的带就顺手选中最先出现的那条，让面板落得到实物；一条都没有也照换 —— 文本那几行换个落点
// 照样改得动（text_target 退到 a.text_def，新建的文本带从它抄起）。
static void show_strip_kind(App &a, bool text) {
  a.panel_kind = text ? kStripText : kStripMovie;
  int64_t best = -1;
  double best_start = 0.0;
  for (const Clip &c : a.project.clips) {
    if ((c.kind == kStripText) != text) continue;
    if (best < 0 || c.timeline_start < best_start) {
      best = c.id;
      best_start = c.timeline_start;
    }
  }
  if (best >= 0) select_only(a, best);
  a.dirty = kRegionAll;
}

// 色块点下去 = 系统的选色框（comdlg32 的 ChooseColorW，OS 自带；不自己画一套色轮，
// 一套色轮就是给颜色立界）。取消 = 什么都不改；确定 = 只写 RGB，alpha 一格不动 ——
// alpha 是这一层的开关（排版器读它判关），开关归那一格百分数管，不由选色框代答。
static void pick_text_color(App &a, int layer) {
  float *dst = text_layer(text_target(a, props_clip(a)), layer);
  static COLORREF custom[16] = {};  // 选色框要求这块自定义色板活得比一次调用长
  CHOOSECOLORW cc = {};
  cc.lStructSize = sizeof(cc);
  cc.hwndOwner = a.hwnd;
  cc.rgbResult = RGB(uint8_t(dst[0] * 255.0f + 0.5f), uint8_t(dst[1] * 255.0f + 0.5f),
                     uint8_t(dst[2] * 255.0f + 0.5f));
  cc.lpCustColors = custom;
  cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
  if (!ChooseColorW(&cc)) return;
  dst[0] = GetRValue(cc.rgbResult) / 255.0f;
  dst[1] = GetGValue(cc.rgbResult) / 255.0f;
  dst[2] = GetBValue(cc.rgbResult) / 255.0f;
  a.dirty |= kRViewport | kRProps;
  sync_preview(a);
  undo_push(a, L"文本颜色");
}

// 「字体」下拉里的一条：条目是枚举出来的，所以命令号减基数就是那张表的第几行。
static void pick_text_family(App &a, int index) {
  const std::vector<std::wstring> &fams = txt::families();
  if (index < 0 || size_t(index) >= fams.size()) return;
  text_target(a, props_clip(a)).family = fams[size_t(index)];
  set_status(a, L"字体：" + fams[size_t(index)]);
  a.dirty |= kRViewport | kRProps;
  sync_preview(a);
  undo_push(a, L"文本字体");
}

void run_cmd(App &a, int cmd) {
  double fd = a.frame_dur();
  switch (cmd) {
    case kCmdImport: open_dialog(a); break;
    case kCmdExport: export_dialog(a); break;
    case kCmdSettingsReset: settings_reset(a); break;
    case kCmdFFmpegCheck: {
      std::wstring from;
      const bool ok = Ffmpeg::locate(nullptr, nullptr, &from);
      a.has_ffmpeg = ok;
      set_status(a, ok ? L"FFmpeg已存在" : L"FFmpeg不存在");
      trace(L"ffmpeg 检测：" + (ok ? from : std::wstring(L"哪都没有")));
      break;
    }
    case kCmdFFmpegDownload: {
      // 已经存在就不必再拉几百 MB：这一条和「检测」用的是同一个查法（PATH → 主程序同目录）。
      std::wstring from;
      a.has_ffmpeg = Ffmpeg::locate(nullptr, nullptr, &from);
      if (a.has_ffmpeg) {
        set_status(a, L"FFmpeg已存在");
        trace(L"ffmpeg 下载跳过（已有：" + from + L"）");
      } else {
        ffmpeg_fetch_start();
      }
      break;
    }
    case kCmdRetimingMenu:
      // km_sequencer:3293 的 I = wm.call_menu(SEQUENCER_MT_retiming)，菜单跟着光标走。
      open_menu(a, kMRetiming, float(a.mouse_x), float(a.mouse_y));
      break;
    case kCmdRetimingKeyAdd: retiming_key_add(a); break;
    case kCmdRetimingShow: retiming_show(a); break;
    case kCmdSpeedSet: {
      // km_sequencer 的纯 R = sequencer.retiming_segment_speed_set（blender_default.py:3294，
      // 算子定义 sequencer_retiming.cc:685-710）：按下弹一个「速度」输入框，取值是百分数、
      // 默认 100。这里等价为把左侧「速度」字段拉进键入态（同一个 prop_apply，同一条时长联动）。
      Clip *c = props_clip(a);
      // 键选中模式不需要活动条带：弹框默认值来自选中那把键所在段（invoke :677-679 走
      // strip_speed_get，它的 retiming 分支只看键表）。
      if (!c && !retiming_keys_are_selected(a)) {
        set_status(a, L"未选中条带");
        break;
      }
      a.panel_open[kPSpeed] = true;  // 折叠着就看不见输入，先展开
      begin_edit(a, kFSpeed, prop_text(a, kFSpeed, c));
      a.dirty |= kRProps;
      break;
    }
    case kCmdQuit: DestroyWindow(a.hwnd); break;
    case kCmdUndo:
      if (!undo_step(a)) set_status(a, L"没有可撤消的操作");
      break;
    case kCmdRedo:
      if (!redo_step(a)) set_status(a, L"没有可重做的操作");
      break;
    case kCmdCopy: copy_selected(a); break;
    case kCmdPaste: paste_clips(a); break;
    case kCmdSelectAll: select_all(a); break;
    case kCmdSelectNone: select_none(a); break;
    case kCmdSelectInvert: invert_selection(a); break;
    case kCmdSelectLeftOfFrame: select_side_of_frame(a, false); break;
    case kCmdSelectRightOfFrame: select_side_of_frame(a, true); break;
    case kCmdSplit: split_selected(a, false); break;
    case kCmdHardSplit: split_selected(a, true); break;
    // X / DEL 在 Blender 里是两条并列绑定：sequencer.retiming_key_delete 排在
    // sequencer.delete 之前（blender_default.py:3222-3225），没有键被选中时它返回
    // CANCELLED|PASS_THROUGH（sequencer_retiming.cc:508-510）⇒ 这一击落到条带删除。
    // 涟漪删除（Shift X，:3226）没有并行的键绑定，所以那条路不碰键。
    case kCmdDelete:
      if (!retiming_delete_selected_keys(a)) delete_selected(a, false);
      break;
    case kCmdRippleDelete: delete_selected(a, true); break;
    case kCmdDuplicate: duplicate_selected(a); break;
    case kCmdAddText: add_text(a); break;
    case kCmdConnect: connect_selected(a); break;
    case kCmdDisconnect: disconnect_selected(a); break;
    case kCmdMute: mute_selected(a, true); break;
    case kCmdMuteUnselected: mute_unselected(a); break;
    case kCmdUnmute: mute_selected(a, false); break;
    case kCmdLock: lock_selected(a, true); break;
    case kCmdUnlock: lock_selected(a, false); break;
    case kCmdGapRemove: gap_remove(a, false); break;
    case kCmdGapRemoveAll: gap_remove(a, true); break;
    case kCmdGapInsert: gap_insert(a); break;
    case kCmdSnap: snap_selected_to_frame(a); break;
    case kCmdKeyInsert:
    case kCmdKeyReplace:
    case kCmdKeyDelete:
    case kCmdKeyClear: keyframe_menu_cmd(a, cmd); break;
    case kCmdSlide: modal_begin(a, ModalState::Slide); break;
    case kCmdSlip: modal_begin(a, ModalState::Slip); break;
    case kCmdExtend: modal_begin(a, ModalState::Extend); break;
    case kCmdRippleTrimStart: ripple_trim(a, true); break;
    case kCmdRippleTrimEnd: ripple_trim(a, false); break;
    case kCmdPlay: toggle_play(a); break;
    case kCmdPlayCancel: if (a.playing) toggle_play(a); break;
    // 左右上下这四个键先问预览区底下那条被选中的条带（strip_nudge）：它认下这一格就到此
    // 为止，认不下才走原来的走帧 / 走条带边界 —— 和 HOME 按指针所在视图分流同一个路子。
    case kCmdFramePrev:
      if (strip_nudge(a, -1, 0)) break;
      jump_frame(a, a.cur_frame() - 1);
      break;
    case kCmdFrameNext:
      if (strip_nudge(a, 1, 0)) break;
      jump_frame(a, a.cur_frame() + 1);
      break;
    // screen.frame_jump（screen_ops.cc:3952-3996）：cfra = BKE_scene_get_playback_range
    // 的 start_frame / end_frame，subframe 归零，然后 ED_areas_do_frame_follow。
    // 播放中走的是 animtimer->nextfra 这条路（同一个端点）。
    case kCmdJumpStart: jump_frame(a, a.project.fmt.frame_start); break;
    case kCmdJumpEnd: jump_frame(a, a.project.fmt.frame_end); break;
    case kCmdTimeBack: jump_frame(a, a.cur_frame() - int(std::lround(a.fps()))); break;
    case kCmdTimeFwd: jump_frame(a, a.cur_frame() + int(std::lround(a.fps()))); break;
    case kCmdKeyPrev:
      if (strip_nudge(a, 0, 1)) break;  // 上 = 位置 Y 变大（ofs_y 正方向就是屏幕向上）
      keyframe_jump(a, false);
      break;
    case kCmdKeyNext:
      if (strip_nudge(a, 0, -1)) break;
      keyframe_jump(a, true);
      break;
    case kCmdStripPrev: strip_jump(a, false); break;
    case kCmdStripNext: strip_jump(a, true); break;
    // Ctrl+Home/End：入点/出点设到当前帧，播放头不动（需求 :108-111）。取值走面板那条路，
    // 和手输帧范围共用同一套 RNA 约束；状态条按 Blender previewrange_set 的写法
    // 报「范围 <起 - 止>」。
    case kCmdRangeToStrips: range_to_strips(a); break;
    case kCmdRangeSetIn:
    case kCmdRangeSetOut: {
      const int id = cmd == kCmdRangeSetIn ? kFFrameStart : kFFrameEnd;
      if (prop_apply(a, id, nullptr, double(a.cur_frame()))) {
        undo_push(a, cmd_name(cmd));
        wchar_t sb[64];
        swprintf(sb, 64, L"帧范围 <%d - %d>", a.project.fmt.frame_start, a.project.fmt.frame_end);
        set_status(a, sb);
      }
      break;
    }
    case kCmdBoxSelect:
      a.box_armed = true;
      set_status(a, L"框选：在条带区拖出矩形（Esc 取消）");
      break;
    case kCmdZoom125: view_zoom_set(a, 12.5f); break;
    case kCmdZoom25: view_zoom_set(a, 25.0f); break;
    case kCmdZoom50: view_zoom_set(a, 50.0f); break;
    case kCmdZoom100: view_zoom_set(a, 100.0f); break;
    case kCmdZoom200: view_zoom_set(a, 200.0f); break;
    case kCmdZoom400: view_zoom_set(a, 400.0f); break;
    case kCmdZoom800: view_zoom_set(a, 800.0f); break;
    case kCmdViewAll:
      // 需求：HOME 看鼠标指针在哪个视图。分流不在这里做 —— 这一点交给底下的视图挨个问
      // （上面的 view_all_at）。
      view_all_at(a, float(a.mouse_x), float(a.mouse_y));
      break;
    case kCmdViewSelected: zoom_to_selected(a); break;
    case kCmdViewFrame: a.dirty |= kRViewport; break;
    // 菜单里的「放大 / 缩小」= view2d.zoom_in/out，围绕预览中心；键盘那条（下面的
    // kCmdTlZoom*）围绕光标，因为 Blender 的 invoke 走 USER_ZOOM_TO_MOUSEPOS（默认开）。
    case kCmdZoomIn:
      view_zoom_at(a, a.viewport.x + a.viewport.w * 0.5f, a.viewport.y + a.viewport.h * 0.5f,
                   float(kZoomIn));
      break;
    case kCmdZoomOut:
      view_zoom_at(a, a.viewport.x + a.viewport.w * 0.5f, a.viewport.y + a.viewport.h * 0.5f,
                   float(kZoomOut));
      break;
    case kCmdZoomBorder: view_reset(a); break;
    // Numpad +/-（km_view2d:976-979）作用于光标所在的那个 view2d region：这一点交给底下
    // 的视图挨个问（上面的 view_zoom_step），谁认出这一点谁围绕它缩放一格。
    case kCmdTlZoomIn:
      view_zoom_step(a, float(a.mouse_x), float(a.mouse_y), kZoomIn);
      break;
    case kCmdTlZoomOut:
      view_zoom_step(a, float(a.mouse_x), float(a.mouse_y), kZoomOut);
      break;
    case kCmdTlZoomAll: timeline_view_all(a); break;
    case kCmdToggleSeconds:
      a.draw_frames = !a.draw_frames;
      a.dirty |= kRScrub | kRLanes;
      set_status(a, a.draw_frames ? L"显示帧号" : L"显示秒数");
      break;
    case kCmdToggleScrub: a.show_scrub = !a.show_scrub; layout(a); break;
    case kCmdToggleSidebar: a.sidebar_on = !a.sidebar_on; layout(a); break;
    // 预览网格（顶栏「网络」）：只改这一位显示开关，不碰任何条带数据，也不标时间线脏。
    case kCmdToggleGrid:
      a.preview_grid = !a.preview_grid;
      a.dirty |= kRViewport;
      set_status(a, a.preview_grid ? L"网格开" : L"网格关");
      break;
    // 面板换边：Blender 那里 region 的左右由 region type 的 alignment 写死在建表里
    // （space_sequencer.cc:196 的 UI 区就是 RIGHT），没有换边的算子；这条是需求要的
    // 「随便切换左右」。换边只改布局，宽度/收起状态跟着走。
    case kCmdPanelLeft:
      if (a.props_side == 0) break;
      a.props_side = 0;
      layout(a);
      set_status(a, L"面板放左");
      break;
    case kCmdPanelRight:
      if (a.props_side == 1) break;
      a.props_side = 1;
      layout(a);
      set_status(a, L"面板放右");
      break;
    case kCmdRefresh: a.dirty = kRegionAll; sync_preview(a); break;
    case kCmdClearPos: case kCmdClearScale: case kCmdClearRot: case kCmdClearAll: {
      std::vector<Clip *> cs = selected_clips(a);
      for (Clip *c : cs) {
        if (cmd == kCmdClearPos || cmd == kCmdClearAll) c->ofs_x = c->ofs_y = 0.0;
        if (cmd == kCmdClearScale || cmd == kCmdClearAll) c->scale_x = c->scale_y = 1.0;
        if (cmd == kCmdClearRot || cmd == kCmdClearAll) c->rotation = 0.0;
      }
      a.dirty |= kRViewport | kRProps;
      set_status(a, L"已清除变换");
      break;
    }
    case kCmdAspect169: set_aspect(a, 16, 9); break;
    case kCmdAspect916: set_aspect(a, 9, 16); break;
    case kCmdAspect43: set_aspect(a, 4, 3); break;
    case kCmdAspect11: set_aspect(a, 1, 1); break;
    case kCmdFps6: set_fps_preset(a, 6, 1.0); break;
    case kCmdFps8: set_fps_preset(a, 8, 1.0); break;
    case kCmdFps12: set_fps_preset(a, 12, 1.0); break;
    case kCmdFps2398: set_fps_preset(a, 24, 1.001); break;
    case kCmdFps24: set_fps_preset(a, 24, 1.0); break;
    case kCmdFps25: set_fps_preset(a, 25, 1.0); break;
    case kCmdFps2997: set_fps_preset(a, 30, 1.001); break;
    case kCmdFps30: set_fps_preset(a, 30, 1.0); break;
    case kCmdFps50: set_fps_preset(a, 50, 1.0); break;
    case kCmdFps5994: set_fps_preset(a, 60, 1.001); break;
    case kCmdFps60: set_fps_preset(a, 60, 1.0); break;
    case kCmdFps120: set_fps_preset(a, 120, 1.0); break;
    case kCmdFps240: set_fps_preset(a, 240, 1.0); break;
    // scripts/presets/framerate/Custom.py 的文件体是空的：选它等于不改数值，
    // 把当前值留着当自定义帧率用。
    case kCmdFpsCustom:
      set_status(a, L"帧率保持当前值（自定义）");
      break;
    // 导出质量：枚举项的数值就是 CRF（eFFMpegCrf），恒定码率那档记 -1（FFM_CRF_NONE：
    // 导出时收起 -crf，交给编码器的码率控制）。libx264 直接用同一数字（movie_write.cc:794 起，
    // H.264 不做 remap；crf 0 在 :788 那条路上就是无损）。
    case kCmdCrfNone:         set_crf(a, -1); break;
    case kCmdCrfLossless:     set_crf(a, 0); break;
    case kCmdCrfPercLossless: set_crf(a, 17); break;
    // 编码速度：ffmpeg_preset 的三项（eFFMpegPreset：GOOD 10 / BEST 11 / REALTIME 12）。
    case kCmdSpeedBest:     set_speed(a, 11); break;
    case kCmdSpeedGood:     set_speed(a, 10); break;
    case kCmdSpeedRealtime: set_speed(a, 12); break;
    // 音频：audio_codec 只有两项在本工程用得到 —— AAC 与 NONE（RNA 第一项 "No Audio"）。
    // 两项各有一条命令，点哪一项就是哪一项，不需要「切换」这种按下才知道结果的动作。
    case kCmdAudioAac:  set_audio(a, false); break;
    case kCmdAudioNone: set_audio(a, true); break;
    case kCmdSelectMore: select_more_less(a, true); break;
    case kCmdSelectLess: select_more_less(a, false); break;
    case kCmdSwapLeft: swap_with_neighbor(a, false); break;
    case kCmdSwapRight: swap_with_neighbor(a, true); break;
    case kCmdStripCenterPrev: strip_jump(a, false, true); break;
    case kCmdStripCenterNext: strip_jump(a, true, true); break;
    case kCmdToggleFullscreen: toggle_fullscreen(a); layout(a); break;
    case kCmdToggleStatusbar:
      a.show_statusbar = !a.show_statusbar;
      layout(a);
      break;
    case kCmdScreenshot: screenshot_dialog(a); break;
    case kCmdPickVideoStrip: show_strip_kind(a, false); break;
    case kCmdPickTextStrip: show_strip_kind(a, true); break;
    // 三层的色块：下标就是 text_layer 的层号，和 Field 那十二格同一个数法。
    case kCmdColorGlyph: pick_text_color(a, 0); break;
    case kCmdColorOutline: pick_text_color(a, 1); break;
    case kCmdColorBox: pick_text_color(a, 2); break;
    default:
      // 「字体」那一段：条目是枚举出来的，命令号减基数就是 txt::families() 的第几行。
      if (cmd >= kCmdFontFirst) pick_text_family(a, cmd - kCmdFontFirst);
      break;
  }
  (void)fd;
  // 一个算子 = 一步撤消（ED_undo_push 就挂在算子执行完的地方）。数据没动的命令——包括
  // 撤消/重做本身，它们只挪链上的指针——由 undo_push 里那次比对跳过；前提是恢复完之后
  // 没有别的地方回写 project，所以 layout 那一步只算矩形。
  undo_push(a, cmd_name(cmd));
}

// 快捷键提示：修饰键顺序按 km_*（Shift Ctrl Alt）。全部对照
// scripts/presets/keyconfig/keymap_data/blender_default.py 的 km_sequencer /
// km_sequencer_generic / km_frames / km_sequencer_preview。
// 唯一的例外：Ctrl+Home / Ctrl+End —— 默认键位表里 HOME/END 只做视图类算子
// （sequencer.view_all :3240），设范围在 Blender 里是 anim.previewrange_set（Ctrl+Alt+P）；
// 需求把「设入点/出点」这两条指派给了 Ctrl+Home/End，所以这里照需求换键、
// 保留 Blender 的语义（只动帧范围，当前帧不变）。跳到范围端点仍然是 Shift+Left/Right，
// 与 km_frames:3832-3835 的 screen.frame_jump 一致。
const wchar_t *cmd_keys(int cmd) {
  switch (cmd) {
    case kCmdImport: return L"Ctrl O";
    case kCmdSpeedSet: return L"R";  // km_sequencer:3294 的纯 R
    case kCmdRetimingMenu: return L"I";  // km_sequencer:3293 的 I = SEQUENCER_MT_retiming
    case kCmdRetimingShow: return L"Ctrl R";  // km_sequencer:3295 的 Ctrl+R
    case kCmdRetimingKeyAdd: return L"";      // 只在 I 菜单里，没有独立键
    case kCmdSplit: return L"K";
    case kCmdHardSplit: return L"Shift K";
    case kCmdDelete: return L"X Del";
    case kCmdRippleDelete: return L"Shift X";
    // sequencer.connect 在 km_sequencer 里有绑定：Ctrl+Alt+C，属性 toggle=True
    // （blender_default.py:3213-3214）。sequencer.disconnect 只有菜单项，没有键。
    case kCmdConnect: return L"Ctrl Alt C";
    case kCmdDisconnect: return L"";
    case kCmdMute: return L"H";
    case kCmdMuteUnselected: return L"Shift H";
    case kCmdUnmute: return L"Alt H";
    case kCmdLock: return L"Ctrl H";
    case kCmdUnlock: return L"Ctrl Alt H";
    case kCmdGapRemove: return L"Backspace";
    case kCmdGapRemoveAll: return L"Shift Backspace";
    case kCmdGapInsert: return L"Shift =";
    case kCmdSnap: return L"Shift S";
    case kCmdSlide: return L"G";
    case kCmdSlip: return L"S";
    case kCmdExtend: return L"E";
    case kCmdRippleTrimStart: return L"Shift Q";
    case kCmdRippleTrimEnd: return L"Shift W";
    case kCmdSelectAll: return L"A";
    case kCmdSelectNone: return L"Alt A";
    case kCmdSelectInvert: return L"Ctrl I";
    case kCmdBoxSelect: return L"B";
    case kCmdSelectLeftOfFrame: return L"[";
    case kCmdSelectRightOfFrame: return L"]";
    case kCmdCopy: return L"Ctrl C";
    case kCmdPaste: return L"Ctrl V";
    case kCmdPlay: return L"Space";
    case kCmdPlayCancel: return L"Esc";
    case kCmdFramePrev: return L"Left";
    case kCmdFrameNext: return L"Right";
    case kCmdJumpStart: return L"Shift Left";
    case kCmdJumpEnd: return L"Shift Right";
    case kCmdRangeSetIn: return L"Ctrl Home";
    case kCmdRangeSetOut: return L"Ctrl End";
    case kCmdTimeBack: return L"Ctrl Left";
    case kCmdTimeFwd: return L"Ctrl Right";
    case kCmdKeyPrev: return L"Up";
    case kCmdKeyNext: return L"Down";
    case kCmdStripPrev: return L"PgUp";
    case kCmdStripNext: return L"PgDn";
    case kCmdStripCenterPrev: return L"Alt PgUp";
    case kCmdStripCenterNext: return L"Alt PgDn";
    case kCmdSelectMore: return L"Ctrl Numpad+";
    case kCmdSelectLess: return L"Ctrl Numpad-";
    case kCmdSwapLeft: return L"Alt Left";
    case kCmdSwapRight: return L"Alt Right";
    case kCmdViewAll: return L"Home";
    case kCmdViewSelected: return L"Numpad .";
    case kCmdViewFrame: return L"Numpad 0";
    // km_view2d:976-979 —— NUMPAD_PLUS / NUMPAD_MINUS（可连发）= view2d.zoom_in/out，
    // 作用于光标所在的那个 view2d 区域（预览或时间线，分流在 view_zoom_step）。
    case kCmdTlZoomIn: return L"Numpad +";
    case kCmdTlZoomOut: return L"Numpad -";
    // km_sequencer_preview:3382-3385 —— sequencer.view_zoom_ratio：
    // 小键盘 1/2/4/8 = 100%/50%/25%/12.5%，Ctrl + 2/4/8 = 200%/400%/800%。
    case kCmdZoom100: return L"Numpad 1";
    case kCmdZoom50: return L"Numpad 2";
    case kCmdZoom25: return L"Numpad 4";
    case kCmdZoom125: return L"Numpad 8";
    case kCmdZoom200: return L"Ctrl Numpad 2";
    case kCmdZoom400: return L"Ctrl Numpad 4";
    case kCmdZoom800: return L"Ctrl Numpad 8";
    case kCmdToggleSeconds: return L"Shift Alt O";
    case kCmdToggleSidebar: return L"N";
    case kCmdRefresh: return L"Ctrl E";
    case kCmdClearPos: return L"Alt G";
    case kCmdClearScale: return L"Alt S";
    case kCmdClearRot: return L"Alt R";
    // km_screen:839-845 —— Ctrl+F12 = render.render(animation=True)，也就是本工程的导出；
    // Ctrl+Z / Ctrl+Shift+Z = ed.undo / ed.redo。
    case kCmdExport: return L"Ctrl F12";
    case kCmdUndo: return L"Ctrl Z";
    case kCmdRedo: return L"Shift Ctrl Z";
    case kCmdQuit: return L"";
    default: return L"";
  }
}

const wchar_t *cmd_name(int cmd) {
  switch (cmd) {
    case kCmdImport: return L"添加影片剪辑…";
    case kCmdExport: return L"导出影片…";
    case kCmdQuit: return L"退出";
    case kCmdSpeedSet: return L"设置速度";
    case kCmdRetimingKeyAdd: return L"添加变速关键帧";      // "Add Retiming Key"
    case kCmdRetimingShow: return L"显示变速关键帧";        // "Retime Strips"
    case kCmdSplit: return L"切分";
    case kCmdHardSplit: return L"硬切";
    case kCmdDelete: return L"删除";
    case kCmdRippleDelete: return L"涟漪删除";
    case kCmdDuplicate: return L"复制条带";
    // Add Text Strip（sequencer_add.cc:2119 的 ot->name = "Add Text Strip"）。
    case kCmdAddText: return L"添加文本";
    // Copy / Paste（sequencer_edit.cc:3856/3877 的 ot->name）。Blender 只给粘贴挂
    // OPTYPE_UNDO，复制不改工程数据；这里不靠标记区分，数据没动的那一趟由
    // undo_push 的比对自己跳过。
    case kCmdCopy: return L"复制";
    case kCmdPaste: return L"粘贴";
    // Connect Strips / Disconnect Strips（sequencer_edit.cc:1522/1557 的 ot->name）。
    case kCmdConnect: return L"连接条带";
    case kCmdDisconnect: return L"断开条带";
    case kCmdMute: return L"静音条带";
    case kCmdMuteUnselected: return L"静音未选中条带";
    case kCmdUnmute: return L"取消静音";
    case kCmdLock: return L"锁定";
    case kCmdUnlock: return L"解锁";
    case kCmdGapRemove: return L"移除间隙";
    case kCmdGapRemoveAll: return L"移除所有间隙";
    case kCmdGapInsert: return L"插入间隙";
    case kCmdSnap: return L"吸附到当前帧";
    case kCmdKeyInsert: return L"插入关键帧";
    case kCmdKeyReplace: return L"替换关键帧";
    case kCmdKeyDelete: return L"删除关键帧";
    case kCmdKeyClear: return L"清除关键帧";
    case kCmdRippleTrimStart: return L"涟漪裁切起点";
    case kCmdRippleTrimEnd: return L"涟漪裁切终点";
    case kCmdPlay: return L"播放/暂停";
    case kCmdFramePrev: return L"上一帧";
    case kCmdFrameNext: return L"下一帧";
    case kCmdJumpStart: return L"跳到开头";
    case kCmdJumpEnd: return L"跳到结尾";
    case kCmdRangeSetIn: return L"设入点";
    case kCmdRangeSetOut: return L"设出点";
    case kCmdRangeToStrips: return L"设为条带范围";
    case kCmdStripPrev: return L"跳到上一个条带";
    case kCmdStripNext: return L"跳到下一个条带";
    case kCmdStripCenterPrev: return L"跳到上一个条带（居中）";
    case kCmdStripCenterNext: return L"跳到下一个条带（居中）";
    case kCmdSelectMore: return L"多选";
    case kCmdSelectLess: return L"少选";
    case kCmdSwapLeft: return L"向左交换条带";
    case kCmdSwapRight: return L"向右交换条带";
    case kCmdBoxSelect: return L"框选";
    case kCmdZoom125: return L"预览缩放 12.5%";
    case kCmdZoom25: return L"预览缩放 25%";
    case kCmdZoom50: return L"预览缩放 50%";
    case kCmdZoom100: return L"预览缩放 100%";
    case kCmdZoom200: return L"预览缩放 200%";
    case kCmdZoom400: return L"预览缩放 400%";
    case kCmdZoom800: return L"预览缩放 800%";
    case kCmdClearPos: return L"位置";
    case kCmdClearScale: return L"缩放";
    case kCmdClearRot: return L"旋转";
    case kCmdViewAll: return L"全部显示";
    case kCmdViewSelected: return L"显示选中";
    case kCmdToggleSeconds: return L"显示秒数";
    case kCmdToggleScrub: return L"Scrubbing";
    case kCmdToggleSidebar: return L"属性";
    case kCmdPanelLeft: return L"面板左侧";
    case kCmdPanelRight: return L"面板右侧";
    case kCmdToggleGrid: return L"网格";
    case kCmdRefresh: return L"全部刷新";
    case kCmdClearAll: return L"所有变换";
    case kCmdToggleFullscreen: return L"全屏";
    case kCmdToggleStatusbar: return L"显示状态栏";
    case kCmdScreenshot: return L"保存截图…";
    case kCmdPickVideoStrip: return L"面板切到影像";
    case kCmdPickTextStrip: return L"面板切到文本";
    case kCmdColorGlyph: case kCmdColorOutline: case kCmdColorBox: return L"文本颜色";
    default: return cmd >= kCmdFontFirst ? L"文本字体" : L"";
  }
}

// ---------------------------------------------------------------- 菜单表
//
// 内容按 bl_ui/space_sequencer.py 与 space_topbar.py 的 draw() 顺序；
// 本工程没有的算子不列（避免“假的样儿”）。

static ui::Entry it(const wchar_t *name, int cmd, int submenu = -1) {
  ui::Entry e;
  e.name = name;
  e.id = cmd;
  e.submenu = submenu;
  if (cmd) e.shortcut = cmd_keys(cmd);
  return e;
}
static ui::Entry disabled(ui::Entry e) {
  e.enabled = false;
  e.id = 0;
  return e;
}
static ui::Entry sep() {
  ui::Entry e;
  e.sep = true;
  return e;
}

void build_menus(App &a) {
  const bool has_strip = !a.sel.empty();
  ui::MenuTable m(kMenuCount);
  // poll 失败时 Blender 会把这些条目灰掉（ sequencer_edit.cc 的
  // sequencer_strip_editable_and_selected_poll 等），这里用 enabled=false 表达同一状态。
  auto need_strip = [&](ui::Entry e) { return has_strip ? e : disabled(e); };
  // 但 I 菜单不在这个口径上：retiming_poll（sequencer_retiming.cc:40-64）看的是
  // ed->act_strip，不是选中集 —— 键选择模式下条带全被取消选中，菜单仍然要按得动。
  Clip *act = props_clip(a);
  const bool can_retime = act && retiming_is_allowed(*act, a.frame_dur());
  auto need_retiming = [&](ui::Entry e) { return can_retime ? e : disabled(e); };

  // space header 那一排编辑器菜单、头部的吸附/叠加 popover 按需求都删了。顶栏只剩三个入口
  // （默认 / 面板 / 网络），其余下拉都挂在控件自己身上：帧率预设和导出质量挂在属性行的
  // 下拉按钮上，右键上下文菜单挂在被点中的那块区域上。

  // ---- RENDER_MT_framerate_presets（scripts/presets/framerate/*.py）--------------
  // 菜单项文字 = 预设文件名；每项设的正是该预设里的 fps / fps_base
  // （例：23.98.py 是 fps=24、fps_base=1.001）。Custom.py 内容为空，
  // 即“选了不改数值、将当前值当作自定义”，这里同样保持数值不变。
  m[kMFramerate] = {{it(L"6", kCmdFps6), it(L"8", kCmdFps8), it(L"12", kCmdFps12),
                     it(L"23.98", kCmdFps2398), it(L"24", kCmdFps24), it(L"25", kCmdFps25),
                     it(L"29.97", kCmdFps2997), it(L"30", kCmdFps30), it(L"50", kCmdFps50),
                     it(L"59.94", kCmdFps5994), it(L"60", kCmdFps60), it(L"120", kCmdFps120),
                     it(L"240", kCmdFps240), sep(), it(L"Custom", kCmdFpsCustom)}};

  // ---- ffmpeg.constant_rate_factor 的枚举下拉（rna_scene.cc:6950-6952）------------
  // 项序照 RNA：Constant Bitrate / Lossless / Perceptually Lossless，文字用需求里的中文。
  m[kMQuality] = {{it(crf_name(-1), kCmdCrfNone), it(crf_name(0), kCmdCrfLossless),
                   it(crf_name(17), kCmdCrfPercLossless)}};

  // ---- ffmpeg.ffmpeg_preset「编码速度」的枚举下拉（rna_scene.cc:6928-6934）---------
  // 项序照 RNA：Slowest / Good / Realtime；名字和 -preset 字符串都在 OutputFormat 那张表里。
  m[kMSpeed] = {{it(OutputFormat::speed_of(11).label, kCmdSpeedBest),
                 it(OutputFormat::speed_of(10).label, kCmdSpeedGood),
                 it(OutputFormat::speed_of(12).label, kCmdSpeedRealtime)}};

  // ---- ffmpeg.audio_codec 的枚举下拉（rna_scene.cc:6967-6972）---------------------
  // RNA 的九项里，本工程只用得上 AAC 与 NONE（"No Audio"）：前者是有声的默认，
  // 后者是「容器里不写音轨」。项序按需求：AAC 在前、无音频在后。
  m[kMAudio] = {{it(OutputFormat::audio_of(false), kCmdAudioAac),
                 it(OutputFormat::audio_of(true), kCmdAudioNone)}};

  // ---- 文本组的「字体」-------------------------------------------------------------
  // 家族由 DirectWrite 枚举（txt::families），所以条目数不写死；命令号 = 基数 + 表里的行号。
  // 这里不排「本机没有的字体」，也不给名单设上限：名单多长是这台机器的字体表说了算。
  {
    const std::vector<std::wstring> &fams = txt::families();
    // 当前家族只问 text_target 一次：按钮上的文字和这份名单里的勾读的是同一个值。
    const std::wstring &cur = text_target(a, act).family;
    std::vector<ui::Entry> fonts;
    fonts.reserve(fams.size());
    for (size_t i = 0; i < fams.size(); ++i) {
      ui::Entry e;
      e.name = fams[i];
      e.id = kCmdFontFirst + int(i);
      e.selected = fams[i] == cur;
      fonts.push_back(std::move(e));
    }
    m[kMFont] = std::move(fonts);
  }

  // ---- SEQUENCER_MT_retiming（I 键呼出，space_sequencer.py:1312-1323）--------------
  // Blender 这个菜单两条：添加关键帧、添加冻结帧滑动。冻结帧那条要先有「定格」这个
  // 条带形态（本工程还没有），所以只挂第一条。
  m[kMRetiming] = {{need_retiming(it(L"添加关键帧", kCmdRetimingKeyAdd))}};

  // ---- 顶栏「默认」--------------------------------------------------------------
  // 保存不用点：关闭 exe 时整份落盘（main.cpp 的 WM_DESTROY 调 settings_save）。这里只留
  // 恢复 = 删掉设置文件，下次启动才回到出厂默认，本次会话不受影响。
  m[kMDefault] = {it(L"恢复启动默认", kCmdSettingsReset)};

  // ---- 顶栏「下载」--------------------------------------------------------------
  // 检测 = 只报现状（PATH 优先，其次主程序同目录，两处都要 ffmpeg.exe 和 ffprobe.exe 齐）；
  // 下载 = 同样的查法，命中就一句「已存在」不动盘，没命中才从镜像拉压缩包、解压、搬到
  // 主程序同目录。下载在工作线程里跑，跑完由 ffmpeg_fetch_tick 交结果。
  ui::Entry dl = it(L"下载FFmpeg", kCmdFFmpegDownload);
  m[kMDownload] = {{it(L"检测FFmpeg", kCmdFFmpegCheck),
                    ffmpeg_fetch_busy() ? disabled(dl) : dl}};

  // ---- 顶栏「面板」--------------------------------------------------------------
  // 和分界线右键那个菜单同一对命令：这里是「不抓边缘也改得动」的入口（收起后又换边时，
  // 边缘手柄只有几像素，菜单不会跟着丢）。
  m[kMPanelSide] = {{it(L"面板左侧", kCmdPanelLeft), it(L"面板右侧", kCmdPanelRight)}};

  // ---- 顶栏「网络」--------------------------------------------------------------
  // 预览画幅上的等分网格 + 中心十字：默认开。做跟踪运动时画面中心点没有别的依据，
  // 这一层就是给它的；关掉 = 预览区少画一批线，不牵扯任何数据。
  m[kMGrid] = {{it(a.preview_grid ? L"关闭网格" : L"开启网格", kCmdToggleGrid)}};


  // ---- 右键上下文菜单 ---------------------------------------------------------------
  // 按需求只留这四条（其余条目已从菜单里撤下，算子本身和快捷键都还在：切分 X 之外的
  // Split、复制、连接/断开、删除、锁定/静音等仍由键盘和面板走）。
  // 「添加文本」不在那条口径里：它是新建，不是对选中条带动手，所以不受 need_strip 管，
  // 时间线空着也按得动。Blender 把它放在「添加」菜单（Add > Text Strip），需求要它出现在
  // 条带区右键，就挂在这里 —— 右键落在哪都开这一个菜单，落点不是条带时也能建新条带。
  m[kMContext] = {{it(L"添加文本", kCmdAddText), sep(),
                   need_strip(it(L"切分", kCmdSplit)),
                   need_strip(it(L"复制条带", kCmdDuplicate)), sep(),
                   need_strip(it(L"连接条带", kCmdConnect)),
                   need_strip(it(L"断开条带", kCmdDisconnect))}};

  // ---- 属性行右键的关键帧条目（interface_context_menu.cc:639-719）----------------
  // 哪几项出现完全由按钮的动画状态决定：
  //   当前帧上有键（BUT_ANIMATED_KEY）⇒「替换关键帧」「删除关键帧」+ 分隔线；
  //   否则属性可动画 ⇒「插入关键帧」；
  //   只要曲线存在（BUT_ANIMATED）再补「清除关键帧」。
  {
    const int st = key_field_state(a, a.key_field);
    std::vector<ui::Entry> rows;
    if (st & kKeyAtFrame) {
      rows.push_back(it(L"替换关键帧", kCmdKeyReplace));
      rows.push_back(it(L"删除关键帧", kCmdKeyDelete));
      rows.push_back(sep());
    } else {
      rows.push_back(it(L"插入关键帧", kCmdKeyInsert));
    }
    if (st & kKeyAnimated) rows.push_back(it(L"清除关键帧", kCmdKeyClear));
    m[kMPropKey] = std::move(rows);
  }

  // ---- 分界线右键（面板换边 / 收起）------------------------------------------------
  // Blender 的区域边缘右键没有这一套（它的区域位置写死在 region type 建表里），这是需求
  // 要的「随便切换左侧右侧」；「收起/展开」与 N 键同一个开关，只是从边缘够得着。
  m[kMPanel] = {{it(L"面板左侧", kCmdPanelLeft), it(L"面板右侧", kCmdPanelRight), sep(),
                 it(a.sidebar_on ? L"收起面板" : L"展开面板", kCmdToggleSidebar)}};
  a.menus = std::move(m);
  a.pop.set_table(&a.menus);
}

void open_menu(App &a, int menu, float x, float y) {
  build_menus(a);
  Rect bounds{0, 0, float(a.r2d.width()), float(a.r2d.height())};
  a.pop.open(a.r2d, menu, x, y, bounds);
  if (trace_fp) {
    wchar_t b[64];
    swprintf(b, 64, L"menu %d open at %.0f,%.0f", menu, x, y);
    trace(b);
  }
  a.dirty = kRegionAll;
}

// 右键的一击：窗口过程只把这一点交下来，开哪一个菜单由「谁认出这一点」决定，顺序就是叠放
// 的层次（和左键那一击同一套问法）。前三个不认地方 —— 它们是上一层正在握着事件的那个对象。
void on_rbutton_down(App &a, float fx, float fy) {
  if (a.pop.is_open()) {  // 弹出菜单是上一层的 block：右键先收起它（Blender 的 popup 一样）
    a.pop.close();
    a.dirty = kRegionAll;
    return;
  }
  if (modal_active(a)) {  // 变换模态正在进行：右键 = 取消这一趟
    modal_end(a, false);
    return;
  }
  if (a.box_armed) {  // 框选还armed着：右键先退掉这个前置状态
    a.box_armed = false;
    a.dirty = kRegionAll;
    return;
  }
  commit_edit(a, false);    // 菜单一开，正在输入的文本框先丢弃
  commit_rename(a, false);  // 通道重命名同理
  // 面板分界线（含面板收起后那条手柄带）：边缘自己的菜单 = 换边 / 收起。Blender 的区域边缘
  // 右键没有这套（它把 resize/collapse 挂在 action zone 的左键上），这条按需求补，好让收起的
  // 面板除了拖还有地方可点。
  if (props_divider_at(a, fx, fy)) {
    open_menu(a, kMPanel, fx, fy);
    return;
  }
  // 属性面板里的那些控件是 ui::Block 的东西：落在哪一行，那一行自己报要开哪个菜单。
  // 装饰点带的是「被装饰属性」的 RNA（but->decorated_rnapoin，interface_anim.cc:126-150），
  // 所以右键落在点上和落在那一行的数值按钮上是同一个菜单；锁按钮同理。
  ui::Item *hi = a.blk.hit(fx, fy);
  const int fid = hi ? hi->id : 0;
  const int base = hit_base(fid);
  const int field = (base == kHitDeco || base == kHitLock) ? hit_val(fid) : fid;
  a.key_field = (field >= kFOfsX && field <= kFRotation) ? field : 0;
  if (a.key_field) {
    open_menu(a, kMPropKey, fx, fy);
    return;
  }
  // 最后一格归时间线：SEQUENCER_MT_context_menu。
  open_menu(a, kMContext, fx, fy);
}

}  // namespace app
