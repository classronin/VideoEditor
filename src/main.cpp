// 输入分发、窗口过程、进程入口。
// 分工：ops.cpp = 几何/数据/RNA/命令/菜单，draw.cpp = 全部区域绘制，本文件 = 事件。
//
// 每条手势与每个按键都对照参考树：
//   scripts/presets/keyconfig/keymap_data/blender_default.py
//     km_sequencer(:3153) / km_sequencer_generic(:3120) / km_sequencer_preview(:3337) /
//     km_sequencer_channels(:3464) / km_frames(:3818) / km_view2d(:952) / km_time_scrub(:1237)
//   windowmanager/intern/wm_event_system.cc:3730-3860 —— PRESS / CLICK / CLICK_DRAG 的分级
//   editors/interface/view2d/view2d_ops.cc —— 滚轮一步的滚动量与缩放系数
#include "app.h"

#include <shellapi.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <vector>

#include "ffmpegget.h"
#include "gesture.h"
#include "media.h"
#include "settings.h"
#include "synctable.h"
#include "waveform.h"

namespace app {

constexpr wchar_t kClass[] = L"VseEditorWindow";
constexpr UINT kTick = 1008;
constexpr UINT kMsgPublish = WM_APP + 1;

// ---------------------------------------------------------------- 时钟表
//
// tick 只干「按时间推进」的活。一行 = 一个这样的来源：needs 说它此刻在不在推进，
// advance 说推进一格要做的那点事。「有没有时钟」= 「这张表里有没有一行活着」，
// 于是这条判断只写在这张表里一次 —— 以前它在开关那格、WM_TIMER 的每一处 if、
// 状态行那格各抄了一份，抄漏一条就是界面不再刷新或多一处轮询。
// 后台线程的产出一律走 ui_publish，不挂在这张表上。全空就 KillTimer，消息循环从此
// 阻塞在 GetMessage 上。以前它无条件存在，空闲时每秒 57~63 次唤醒、每次 0.06ms，
// 那是整机上最大的一笔空转。
struct ClockRow {
  const wchar_t *name;
  bool (*needs)(App &);
  void (*advance)(App &, HWND hwnd, double now, double dt);
};

static bool need_play(App &a) { return a.playing; }
static void step_play(App &a, HWND, double, double dt) {
  // 先让音频按上一格的落点受理这一段，再取时钟：受理点就是这一格播放头该落在的刻度。
  // 反过来（先推进再受理）会让这一格读到旧那一段的发声位置，把刚受理的新位置往前拽。
  sync_audio(a);
  playhead_advance(a, dt);
  // 播放头是这一格按时间写的，预览要的源时刻跟着它变；别处（拖动、算子）自己写
  // 播放头时自己调 sync_preview，不靠这一格兜。
  sync_preview(a);
}

static bool need_pan(App &a) {
  // 手还按着 / 还在模子里：边缘平移要接着滚，状态行也要继续记。
  return a.ldown || modal_active(a);
}
static void step_pan(App &a, HWND, double now, double) {
  // 光标停在边界不动时也要继续滚：Blender 靠 edge_pan_apply_delta 里的
  // WM_event_add_mousemove 自激，这里用同一个 tick 顶替。
  edge_pan_step(a, now);
}

static bool need_export(App &a) {
  return a.export_at > 0.0 || a.exporter.running();
}
static void step_export(App &a, HWND hwnd, double now, double) {
  if (a.export_at > 0.0 && now > a.export_at) {  // 「0.3 秒后再启动」的那一延
    a.export_at = -1.0;
    start_export(a, a.export_path);
  }
  // 导出是外部进程往管道里写字节，我们这侧没有线程可通知，读管道的一直是这一格。
  // 它跑着 ⇒ 时钟在 ⇒ 这不是空闲轮询；跑完也在这格看见（pump 里同时写好 message），
  // 下一格时钟就不存在了。
  a.exporter.pump();
  if (a.exporter.running()) {
    wchar_t b[96];
    swprintf(b, 96, L"导出中 %.0f%%", a.exporter.progress() * 100.0);
    set_status(a, b);
  } else if (!a.export_reported && !a.exporter.message().empty()) {
    a.export_reported = true;
    set_status(a, a.exporter.message());
    trace(a.exporter.message());
  }
  if (a.export_quit && a.export_reported && !a.exporter.running()) {
    a.export_quit = false;
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
  }
}

static bool need_fetch(App &a) {
  // 用 active 不用 busy：跑完那一格 state 停在 kFFDone/kFFFailed，结果还没交出去，
  // 只按 busy 问的话这一格就不存在了，下载完的那句状态条永远不出现。
  return ffmpeg_fetch_active();
}
static void step_fetch(App &a, HWND, double, double) { ffmpeg_fetch_tick(a); }

static bool need_shot(App &a) { return !a.shot_path.empty(); }
static void step_shot(App &a, HWND, double now, double) {
  // --shot 的判定在 draw() 里；空闲不重绘就会一直不截图，所以到期后强制标脏一次。
  if (now > a.shot_hold || now > a.shot_deadline) a.dirty = kRegionAll;
}

constexpr ClockRow kClock[] = {
    {L"播放", need_play, step_play},   {L"拖动", need_pan, step_pan},
    {L"导出", need_export, step_export}, {L"下载", need_fetch, step_fetch},
    {L"截图", need_shot, step_shot},
};

static bool clock_needed(App &a) {
  for (const ClockRow &r : kClock)
    if (r.needs(a)) return true;
  return false;
}

// 唯一改时钟的地方。放在每条消息处理完之后问一遍，而不是塞进播放键/导出键/下载键里：
// 「该不该有时钟」的判断只能有一处，分三处写就漏一条 = 界面不再刷新。
static void clock_sync(App &a, HWND hwnd) {
  if (!hwnd) return;
  const bool want = clock_needed(a);
  if (want == a.tick_armed) return;
  a.tick_armed = want;
  if (want) {
    // 重新上钟要把「上一跳多久」的基准挪到现在：停了几分钟再按空格，dt 不能是几分钟，
    // 否则播放头一上来就跳一大截。
    a.last_tick = now_seconds();
    SetTimer(hwnd, kTick, 15, nullptr);
  } else {
    KillTimer(hwnd, kTick);
  }
}

// ---------------------------------------------------------------- worker → UI 的发布
//
// 每个来源一格，同一来源只留一条在路上的消息：pending 从「没投递」翻到「已投递」的那次才
// PostMessage，其余的直接返回。处理消息时先清这一格再读，所以「清完账、还没读完」之间
// 产出的那一次会重新投递，不会丢。
HWND g_publish_hwnd = nullptr;
std::atomic<bool> g_publish_pending[kPubCount] = {};

void ui_publish(int src) {
  HWND h = g_publish_hwnd;
  if (!h || src < 0 || src >= int(kPubCount)) return;
  if (g_publish_pending[src].exchange(true)) return;
  PostMessageW(h, kMsgPublish, WPARAM(src), 0);
}

// 滚轮步长与缩放步长（kScrollStepPx / kZoomIn / kZoomOut）在 app.h，
// 绘制层与算子层共用同一组数字。

// ---------------------------------------------------------------- 计时探针

// VSE_TIME=1：每秒把 UI 线程耗时与预览重启次数写进 trace，用来复测「卡」有没有回来。
bool g_time_probe = false;
struct Probe {
  int ticks = 0, late = 0, paints = 0;
  double sum_cost = 0.0, max_cost = 0.0, max_gap = 0.0, prev = 0.0, next = 0.0;
  // UI 帧间隔的分布：max_gap 会被一次抖动带跑，p95 才是「这段时间界面有没有停手」的读数。
  // 分档 = ≤20 / ≤30 / ≤40 / ≤50 / ≤75 / >75 ms（每 tick 一次比较链，纳秒级，不成新假死源）。
  int gapb[6] = {0, 0, 0, 0, 0, 0};
  int gaps = 0;
};
Probe g_probe;
double g_draw_avg = 0.0, g_draw_max = 0.0;

// ---------------------------------------------------------------- 快捷键表
//
// 绑定和菜单提示同源：这张表由 cmd_keys()（km_* 的逐条翻译）解析出来，
// 所以菜单右侧显示的组合键和真正生效的组合键不可能对不上。

struct Key {
  WORD mods;
  BYTE vk;
  int cmd;
};
std::vector<Key> g_keys;

WORD mod_state() {
  WORD m = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) m |= kShift;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= kCtrl;
  if (GetKeyState(VK_MENU) & 0x8000) m |= kAlt;
  return m;
}

bool ctrl_held() { return (GetKeyState(VK_CONTROL) & 0x8000) != 0; }
bool shift_held() { return (GetKeyState(VK_SHIFT) & 0x8000) != 0; }

bool key_token(const std::wstring &tok, BYTE *vk) {
  static const std::pair<const wchar_t *, BYTE> named[] = {
      {L"Space", VK_SPACE},     {L"Esc", VK_ESCAPE},     {L"Tab", VK_TAB},
      {L"Enter", VK_RETURN},    {L"Left", VK_LEFT},      {L"Right", VK_RIGHT},
      {L"Up", VK_UP},           {L"Down", VK_DOWN},      {L"Home", VK_HOME},
      {L"End", VK_END},         {L"Del", VK_DELETE},     {L"Backspace", VK_BACK},
      {L"PgUp", VK_PRIOR},      {L"PgDn", VK_NEXT},      {L"=", VK_OEM_PLUS},
      {L"[", VK_OEM_4},         {L"]", VK_OEM_6},        {L"Numpad.", VK_DECIMAL},
      {L"F11", VK_F11},         {L"F12", VK_F12},
      {L"Numpad0", VK_NUMPAD0}, {L"Numpad1", VK_NUMPAD1}, {L"Numpad2", VK_NUMPAD2},
      {L"Numpad4", VK_NUMPAD4}, {L"Numpad8", VK_NUMPAD8}, {L"Numpad+", VK_ADD},
      {L"Numpad-", VK_SUBTRACT},
  };
  for (const auto &p : named)
    if (tok == p.first) {
      *vk = p.second;
      return true;
    }
  if (tok.size() == 1) {
    wchar_t c = wchar_t(std::towupper(tok[0]));
    if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) {
      *vk = BYTE(c);
      return true;
    }
  }
  return false;
}

// 把提示串「Shift Ctrl Numpad+」拆成修饰键 + 键名。一个算子可以有多条绑定，
// 用 ';' 分隔（screen.frame_jump 就是 Shift+Left 和 Ctrl+Home 同值，:3832-3839）。
void build_keymap() {
  g_keys.clear();
  for (int cmd = kCmdNone + 1; cmd <= kCmdToggleStatusbar; ++cmd) {
    const wchar_t *hint = cmd_keys(cmd);
    if (!hint || !*hint) continue;
    std::wstring all = hint;
    size_t at = 0;
    while (at <= all.size()) {
      size_t sc = all.find(L';', at);
      std::wstring s;
      {
        std::wstring h = all.substr(at, sc == std::wstring::npos ? std::wstring::npos : sc - at);
        // 「Numpad .」「Numpad 0」的提示里带空格，先折成一个词，免得被当成两个词解析。
        for (size_t p = h.find(L"Numpad "); p != std::wstring::npos;) {
          s += h.substr(0, p);
          s += L"Numpad";
          h = h.substr(p + 7);
          p = h.find(L"Numpad ");
        }
        s += h;
      }
      at = sc == std::wstring::npos ? all.size() + 1 : sc + 1;
      WORD mods = 0;
      bool ok = true;
      std::vector<BYTE> vks;  // 「X Del」这类提示是同一个算子的两条绑定，不是组合键
      size_t pos = 0;
      while (pos <= s.size()) {
        size_t sp = s.find(L' ', pos);
        std::wstring w = s.substr(pos, sp == std::wstring::npos ? std::wstring::npos : sp - pos);
        pos = sp == std::wstring::npos ? s.size() + 1 : sp + 1;
        if (w.empty()) continue;
        if (w == L"Shift") mods |= kShift;
        else if (w == L"Ctrl") mods |= kCtrl;
        else if (w == L"Alt") mods |= kAlt;
        else {
          BYTE vk = 0;
          if (!key_token(w, &vk)) {
            ok = false;
            break;
          }
          vks.push_back(vk);
        }
      }
      if (!ok || vks.empty()) continue;
      for (BYTE vk : vks) {
        g_keys.push_back({mods, vk, cmd});
        // 小键盘的 +/- 在没数字键区的笔记本上走主键盘那一对；两者不会和别的绑定撞
        // （"=" 本身绑的是 Shift+= 即间隙插入，纯 VK_OEM_PLUS / MINUS 都空着）。
        if (vk == VK_ADD) g_keys.push_back({mods, BYTE(VK_OEM_PLUS), cmd});
        else if (vk == VK_SUBTRACT) g_keys.push_back({mods, BYTE(VK_OEM_MINUS), cmd});
      }
    }
  }
}

const Key *key_lookup(WORD mods, BYTE vk) {
  for (const Key &k : g_keys)
    if (k.vk == vk && k.mods == mods) return &k;
  return nullptr;
}

// ---------------------------------------------------------------- 文本输入
//
// Blender 的数值按钮单击进入输入态：原值全选、输入即替换、Enter 提交、Esc 放弃
// （interface_handlers.cc 的 uiNumberField）。全选和「拖出一段选区」用同一套光标/选区
// 状态表示：有选区时敲第一个字符先吃掉选区，所以不再需要一个「还没动过」的标志。

// 当前正在输入的缓冲区：通道重命名走 rename_text，其余（数值框）走 edit_text。
std::wstring &edit_buf(App &a) { return a.rename_track >= 0 ? a.rename_text : a.edit_text; }

// 正在输入的那个控件在 blk 里的 id：放光标、拖选区都要按它的矩形换算字符下标。
int edit_item_id(const App &a) {
  if (a.rename_track >= 0) return kHitChan | (a.rename_track << 3) | kChanName;
  return a.edit_id;
}

// 选区两端（空选区时两者相同）。
void edit_bounds(const App &a, int &lo, int &hi) {
  lo = std::min(a.edit_ts.caret, a.edit_ts.sel);
  hi = std::max(a.edit_ts.caret, a.edit_ts.sel);
}

// 把光标放到第 pos 个字符边界（0 = 串首，len = 串尾）。select=false 时同时收掉选区。
void edit_set_caret(App &a, int pos, bool select) {
  const int len = int(edit_buf(a).size());
  pos = std::clamp(pos, 0, len);
  a.edit_ts.caret = pos;
  if (!select) a.edit_ts.sel = pos;
  if (trace_fp) {
    wchar_t b[64];
    swprintf(b, 64, L"txt-sel id=%d caret=%d sel=%d len=%d", edit_item_id(a), a.edit_ts.caret,
             a.edit_ts.sel, len);
    trace(b);
  }
  a.dirty = kRegionAll;
}

// 指针落在框内哪两个字符之间：取最近的那个边界，框外的自然夹到串首/串尾。
// 多行框（内容那一行）先看 y 落在第几行，再在那一行里看 x；单行框只问 x。
void edit_caret_at(App &a, const ui::Item &it, float x, float y, bool select) {
  const int pos =
      it.line_h > 0.0f
          ? ui::text_index_at(a.r2d, edit_buf(a), it.rect, it.text_x, it.line_h, x, y)
          : ui::text_index_at(a.r2d, edit_buf(a), it.text_x, x);
  edit_set_caret(a, pos, select);
}

// 上下键换行：只有多行的内容框管上下（Blender 也一样 —— 数值框的上下是步进，textbox 才是换行，
// interface_handlers.cc:4335 → textbox_jump_line）。保住的是水平像素而不是字符列：一行里两个
// 汉字和两个字母宽度不同，按列走会斜。第一行按上 = 串首，最后一行按下 = 串尾（同 Blender）。
void edit_move_line(App &a, int dir, bool select) {
  ui::Item *it = a.blk.find(edit_item_id(a));
  if (!it) return;
  const std::wstring &t = edit_buf(a);
  const std::vector<ui::TextLine> lines = ui::text_lines(t);
  int li = int(lines.size()) - 1;
  for (int i = 0; i < int(lines.size()); i++)
    if (a.edit_ts.caret <= lines[i].end) {
      li = i;
      break;
    }
  const int target = li + dir;
  if (target < 0) return edit_set_caret(a, 0, select);
  if (target >= int(lines.size())) return edit_set_caret(a, INT_MAX, select);
  const int caret = a.edit_ts.caret;
  const float cx =
      it->text_x + a.r2d.text_width(t.substr(lines[li].begin, caret - lines[li].begin),
                                    btheme::font_px);
  const std::wstring dest = t.substr(lines[target].begin, lines[target].end - lines[target].begin);
  edit_set_caret(a, lines[target].begin + ui::text_index_at(a.r2d, dest, it->text_x, cx), select);
}

bool edit_char(App &a, wchar_t c) {
  // 控制字符（Enter / Esc / 退格）从 WM_KEYDOWN 那一头进来，WM_CHAR 不产它们。
  // 唯一的例外：内容框的 Ctrl+Enter 要真的落一个换行。
  if (c < 0x20 && !(c == L'\n' && is_string_field(a.edit_id))) return false;
  std::wstring &t = edit_buf(a);
  int lo, hi;
  edit_bounds(a, lo, hi);
  const bool had_sel = hi > lo;
  // 校验看「吃掉选区之后」的那串：被拒的字符不能顺手把选区删掉。
  std::wstring rest = had_sel ? std::wstring(t).erase(size_t(lo), size_t(hi - lo)) : t;
  // 数值按钮的 id 从 0x1000 起，内容那一行也在其中，所以「是不是数字」问的是 Field 表，
  // 不是问「有没有在改轨道名」。
  const bool numeric = a.rename_track < 0 && !is_string_field(a.edit_id);
  if (numeric) {
    if (c == L'.' || c == L',') c = L'.';
    const bool digit = c >= L'0' && c <= L'9';
    // 光标能落在串中间，所以符号自己也要有界：小数点只许一个，负号只许在串首。
    if (c == L'.' && rest.find(L'.') != std::wstring::npos) return false;
    if (c == L'-' && (lo != 0 || rest.find(L'-') != std::wstring::npos)) return false;
    if (!(digit || c == L'.' || c == L'-') || rest.size() >= 16) return false;
  } else if (a.rename_track >= 0 && rest.size() >= 64) {
    return false;  // 通道名那一格；内容行没有上限，字条带想写多长写多长
  }
  if (had_sel) {  // 全选/拖选都在此生效：先删掉选区，再从光标处插入
    t.erase(size_t(lo), size_t(hi - lo));
    a.edit_ts.caret = lo;
  }
  t.insert(size_t(a.edit_ts.caret), 1, c);
  ++a.edit_ts.caret;
  a.edit_ts.sel = a.edit_ts.caret;
  if (trace_fp) {
    wchar_t b[96];
    swprintf(b, 96, L"txt-type id=%d caret=%d text=%ls", edit_item_id(a), a.edit_ts.caret,
             t.c_str());
    trace(b);
  }
  a.dirty = kRegionAll;
  return true;
}

void edit_backspace(App &a) {
  std::wstring &t = edit_buf(a);
  int lo, hi;
  edit_bounds(a, lo, hi);
  if (hi > lo) {  // 有选区：删整段
    t.erase(size_t(lo), size_t(hi - lo));
  } else if (lo > 0) {  // 没选区：删光标左边一个字符
    t.erase(size_t(lo - 1), 1);
    lo -= 1;
  } else
    return;
  a.edit_ts.caret = a.edit_ts.sel = lo;
  a.dirty = kRegionAll;
}

void edit_del(App &a) {  // Delete 键：删光标右边一个字符（退格删左边，两个键都要有）
  std::wstring &t = edit_buf(a);
  int lo, hi;
  edit_bounds(a, lo, hi);
  if (hi > lo)
    t.erase(size_t(lo), size_t(hi - lo));
  else if (hi < int(t.size()))
    t.erase(size_t(hi), 1);
  else
    return;
  a.edit_ts.caret = a.edit_ts.sel = lo;
  a.dirty = kRegionAll;
}

// 进入输入态：当前值全选（光标在串首、选区尾巴在串尾），下一个字符把它整体换掉。
void begin_edit(App &a, int id, const std::wstring &current) {
  a.edit_id = id;
  a.edit_text = current;
  a.edit_ts = {0, int(current.size())};
  a.edit_anchor = -1;
  a.rename_track = -1;
  a.rename_text.clear();
  if (trace_fp) {
    wchar_t b[96];
    swprintf(b, 96, L"edit-begin id=%d caret=0 sel=%d text=%ls", id, int(current.size()),
             current.c_str());
    trace(b);
  }
  a.dirty = kRegionAll;
}

void begin_rename(App &a, int track) {
  if (track < 0 || track >= int(a.project.tracks.size())) return;
  commit_edit(a, false);
  a.rename_track = track;  // SEQ_OT_rename_channel：Ctrl+左键或双击轨道名
  a.rename_text = a.project.tracks[track].name;
  a.edit_ts = {0, int(a.rename_text.size())};
  a.edit_anchor = -1;
  a.dirty = kRegionAll;
}

// 左键按在已经打开的输入框里：先记住按下时的字符边界，再放光标；
// 之后拖动就从这个边界往外拖出选区（和 Windows/Blender 的文本框一致）。
void begin_text_select(App &a, const ui::Item &it, float x, float y) {
  a.edit_anchor = a.edit_ts.caret;
  edit_caret_at(a, it, x, y, false);
}

// ---------------------------------------------------------------- 命中辅助

// 点落在哪个 region —— 对应 Blender「事件先按 region 分配、标脏也只标这一块」。
uint32_t region_bit_at(const App &a, float fx, float fy) {
  if (a.topbar.contains(fx, fy)) return kRTopbar;
  if (a.split.contains(fx, fy)) return kRSplit;
  if (a.viewport.contains(fx, fy)) return kRViewport;
  if (a.scrub.contains(fx, fy)) return kRScrub;
  if (a.chan.contains(fx, fy)) return kRChan;
  if (a.lanes.contains(fx, fy) || a.tl_area.contains(fx, fy)) return kRLanes;
  if (a.props.contains(fx, fy)) return kRProps;
  if (a.status.contains(fx, fy)) return kRStatus;
  return kRegionAll;
}

// 变速键命中层：整段照 sequencer_select.cc:1053-1097 + sequencer_retiming.cc:718-832。
// 先找「键带 containing 这一点」的条带（strip_key_owner），在它里面取最近的键；阈值
// 16*UI_SCALE_FAC，正好压在条带左/右把手上的那颗翻倍（:737-742 的注释
// 「Make first and last key easier to select」）。那一层没命中就试假键（:784-803，阈值再
// 翻倍），点中即把假键落成真键（realize_fake_keys）。命中键时若原本没有键被选中，按 :1079
// 先清掉条带选择；整层落空时按 :1100 清掉键选择，把这一击交回条带选择。
// 命中之后「这一次按住」要怎么拖，是 KeySlideArm 的事（gesture.cpp），这里只负责认出键、
// 选中它，并把 (条带 id, 下标) 交出去；out_id 留 -1 = 这一击只做增删（Shift 那一支），不拖。
bool retiming_key_pick(App &a, float fx, float fy, int64_t *out_id, size_t *out_idx) {
  *out_id = -1;
  *out_idx = 0;
  const float thresh = 16.0f * btheme::scale;  // retiming_key_mouseover_threshold（:718-722）
  const bool was_retiming = retiming_keys_are_selected(a);

  for (Clip &c : a.project.clips) {  // Blender 按序列清单顺序遍历，第一条命中即返回
    if (!c.show_retiming) continue;  // retiming_show_keys（strip_retiming.cc:124-127）
    StripGeom g = strip_geom(a, c);
    if (!g.visible) continue;
    const RetKeyBand band = retiming_band(g);
    if (!band.contains(fx, fy)) continue;

    int best = -1;
    float best_d = 1e18f;
    for (size_t i = 0; i < c.retiming.size(); i++) {
      const double x = c.retiming[i].strip_frame;
      const float d = std::fabs(time_x(a, c.timeline_start + x) - fx);
      // 命中用未收位的原始位置（Blender 也只 CLAMP 绘制，:734 的 view_to_region_x 不夹），
      // 贴把手的判定按 Blender 的左右把手（我们的键位就是距左缘的秒）。
      float th = thresh;
      if (x <= 0.0 || x >= c.duration) th *= 2.0f;
      if (d < th && d < best_d) {
        best_d = d;
        best = int(i);
      }
    }
    if (best >= 0) {
      // :1071-1076：真键命中也要先补两端的假键。键数组本来就有两端，这里是空操作。
      retiming_realize_keys(c);
      if (!was_retiming) clear_selection(a);
      const WORD mods = mod_state();
      const bool toggle = (mods & kShift) != 0;
      // key_select_exec（sequencer_retiming.cc:917-925）分三种：点没选中的键 = 单选它并进入
      // 模态待拖；点已经选中的键 = 什么都不改（多键一起拖就靠这条）并按 Shift 时只做增删、
      // 不进模态。
      const bool already = retiming_key_is_selected(a, c.id, size_t(best));
      if (toggle) retiming_key_select(a, c.id, size_t(best), true, false);
      else if (!already) retiming_key_select(a, c.id, size_t(best), false, true);
      if (!toggle) {
        *out_id = c.id;
        *out_idx = size_t(best);
      }
      trace_retime(a, c, L"key-sel");
      a.dirty |= kRLanes;
      return true;
    }

    // 没有真键 ⇒ 试试这条带的两把假键（还在画、但数组里没有）。
    if (c.retiming.size() < 2) {
      const float dl = std::fabs(time_x(a, c.timeline_start) - fx);
      const float dr = std::fabs(time_x(a, c.timeline_start + c.duration) - fx);
      if (std::min(dl, dr) < thresh * 2.0f) {
        retiming_realize_keys(c);
        const size_t idx = dl < dr ? 0 : 1;  // retiming_key_get_by_frame 取的就是这一端
        if (!was_retiming) clear_selection(a);
        const WORD mods = mod_state();
        const bool toggle = (mods & kShift) != 0;
        const bool already = retiming_key_is_selected(a, c.id, idx);
        if (toggle) retiming_key_select(a, c.id, idx, true, false);
        else if (!already) retiming_key_select(a, c.id, idx, false, true);
        if (!toggle) {
          *out_id = c.id;
          *out_idx = idx;
        }
        trace_retime(a, c, L"fake-realize");
        a.dirty |= kRLanes;
        return true;
      }
    }
    // :774-776：这条带里没有键可点，接着看下一条。
  }

  // :1099-1103：整层落空 ⇒ 退出键选择模式，交回条带选择。
  if (was_retiming) retiming_clear_key_sel(a);
  return false;
}

// ---------------------------------------------------------------- 左键
//
// km_sequencer（:3162-3300）里左键的几条：
//   LEFTMOUSE PRESS        = sequencer.select（空白处 = deselect_all）
//   LEFTMOUSE CLICK_DRAG   = transform.seq_slide（条带上）/ sequencer.select_box（空白处，tweak）
//   条带两端的把手         = 改入点/时长
//   SCRUBBING 区 LEFTMOUSE = anim.change_frame（km_time_scrub:1237）
// 上面这几条落在哪一块、答应了以后怎么算，都不在这一层判断：这一层只把这一击交出去，
// 由被点中的对象各自决定（gesture.cpp 的 press_left）。
void on_lbutton_down(App &a, int x, int y) {
  a.mouse_x = x;
  a.mouse_y = y;
  a.ldown = true;
  const float fx = float(x), fy = float(y);
  if (trace_fp) {  // 交互核对：按下落在哪个区（截图脚本按像素选点，选错就什么都测不到）
    wchar_t b[128];
    swprintf(b, 128, L"press %d,%d scrub=%.0f..%.0f lanes=%.0f..%.0f pps=%g", x, y, a.scrub.y,
             a.scrub.y + a.scrub.h, a.lanes.y, a.lanes.y + a.lanes.h, a.tl.pps);
    trace(b);
  }

  // 模态变换进行中：左键 = 确认（transform 的模态处理把 LMB 当 commit）。
  if (modal_active(a)) {
    modal_end(a, true);
    return;
  }
  // 弹出菜单是上一层 block：这一击只归菜单。
  if (a.pop.is_open()) {
    int id = a.pop.on_click(fx, fy);
    if (trace_fp) {
      wchar_t b[64];
      swprintf(b, 64, L"pop click -> %d, still open=%d", id, int(a.pop.is_open()));
      trace(b);
    }
    if (id > 0) run_cmd(a, id);
    a.dirty = kRegionAll;
    return;
  }

  ui::Item *it = a.blk.hit(fx, fy);
  if (trace_fp) {
    wchar_t hb[48];
    swprintf(hb, 48, L"hit id=%d", it ? it->id : 0);
    trace(hb);
  }
  // 点到别处 = 提交正在输入的文本框。
  if (a.edit_id && !(it && it->id == a.edit_id)) commit_edit(a, true);
  if (a.rename_track >= 0 &&
      !(it && it->id == (kHitChan | (a.rename_track << 3) | kChanName)))
    commit_rename(a, true);

  if (it && it->id) {
    const int id = it->id, base = hit_base(id);
    if (base == kHitCmd) {
      run_cmd(a, hit_val(id));
      a.dirty |= region_bit_at(a, fx, fy);
      return;
    }
    if (base == kHitMenu) {
      open_menu(a, hit_val(id), it->rect.x, it->rect.y + it->rect.h);
      return;
    }
    if (base == kHitPanel) {
      int i = hit_val(id);
      if (i < kPanelCount) a.panel_open[i] = !a.panel_open[i];
      a.dirty |= kRProps;
      return;
    }
    if (base == kHitLock) {  // XY 成对锁
      xform_lock_toggle(a, hit_val(id));
      return;
    }
    if (base == kHitDeco) {  // 装饰点：单击 = 在当前帧插入/删除关键帧
      keyframe_dot_click(a, hit_val(id));
      a.dirty |= kRProps | kRViewport | kRLanes;
      return;
    }
    if (base == kHitChan) {
      int p = hit_val(id), track = p >> 3, kind = p & 7;
      if (track < 0) return;
      // 空通道的图标照常可点：Blender 的通道本来就恒存在，图标作用在那条通道上；
      // 这里点下去才把这条 Track 补进数据。
      Track &tr = channel_slot(a, track);
      if (kind == kChanName) {
        if (a.rename_track == track) {  // 重命名框已开：点里面放光标、按住拖出选区
          begin_text_select(a, *it, fx, fy);
          return;
        }
        // 普通左键不动作；Ctrl+左键 / 双击 = 重命名（km_sequencer_channels:3464）。
        if (mod_state() & kCtrl) begin_rename(a, track);
        return;
      }
      if (kind == kChanLock) {
        tr.locked = !tr.locked;
        set_status(a, L"通道 " + tr.name + (tr.locked ? L" 已锁定" : L" 已解锁"));
      } else {
        tr.muted = !tr.muted;
        set_status(a, L"通道 " + tr.name + (tr.muted ? L" 已静音" : L" 已取消静音"));
      }
      a.dirty |= kRChan | kRLanes | kRViewport;
      undo_push(a, kind == kChanLock ? L"锁定通道" : L"静音通道");
      return;
    }
    if (base == 0 && id >= kFNone) {  // Field：数值按钮、布尔勾选、文本框
      if (is_bool_field(id)) {
        prop_toggle(a, id, props_clip(a));
        return;
      }
      // 文本框（内容那一行）：按下就进输入态，光标落在点的那里，拖动只拖选区。
      // 数值按钮那套横向拖动对一串字没有意义，所以这条路根本不进 ndrag。
      if (it->type == ui::Type::Text) {
        if (a.edit_id != id) begin_edit(a, id, prop_text(a, id, props_clip(a)));
        begin_text_select(a, *it, fx, fy);
        return;
      }
      // 这一格已经在输入：左键改成放光标、按住拖出选区，不再当作数值拖动的起点。
      // 否则「按在数字上往左拖」会被读成改值，选不动字。
      if (a.edit_id == id) {
        begin_text_select(a, *it, fx, fy);
        return;
      }
      ui::num_drag_begin(a.blk, a.ndrag, id, x);
      a.dirty |= kRProps;
      return;
    }
    return;  // 未识别的 id：既不动作，也不落到时间线手势
  }

  // 往下的每一条分支都不再由这里判断「这一点落在哪一块地方」：把这一击交出去，
  // 谁答应（分界线、滚动条、标尺、条带/把手/空白、取景框）谁交出这一次的手势。
  // 一个都不答应 = 空槽，这一击不动作（Blender 的 HEADER/WINDOW 之外没有裸拖）。
  a.gesture = press_left(a, fx, fy);
}

void on_lbutton_dblclk(App &a, int x, int y) {
  if (a.pop.is_open()) {
    a.pop.close();
    a.dirty = kRegionAll;
    return;
  }
  ui::Item *it = a.blk.hit(float(x), float(y));
  if (it && hit_base(it->id) == kHitChan && (hit_val(it->id) & 7) == kChanName)
    begin_rename(a, hit_val(it->id) >> 3);
  // Windows 把第二次按下发成 DBLCLK 而不是 LBUTTONDOWN，所以正在输入的框里
  // 双击也必须能落到光标上，否则连点第二下什么都不做。
  else if (it && it->id && it->id == edit_item_id(a))
    begin_text_select(a, *it, float(x), float(y));
}

// 闭环平移的光标（GHOST_SystemWin32.cc:1191-1238 的 warp 分支 + GHOST_Rect::wrapPoint）
// 和 view2d.zoom 的每事件系数都在 gesture.cpp：那一份账只属于正在进行的这一次按住。

void on_mouse_move(App &a, int x, int y, bool down) {
  a.mouse_x = x;
  a.mouse_y = y;
  if (a.pop.is_open()) {
    const bool ch = a.pop.on_move(float(x), float(y));
    if (trace_fp) {
      wchar_t mb[64];
      swprintf(mb, 64, L"pop-move %g,%g depth=%d ch=%d", float(x), float(y), a.pop.depth(), int(ch));
      trace(mb);
    }
    if (ch) a.dirty = kRegionAll;
    return;
  }
  if (a.edit_anchor >= 0) {  // 按住输入框里的字拖动：选区从按下那点拖到当前指针
    ui::Item *ed = a.blk.find(edit_item_id(a));
    if (ed) edit_caret_at(a, *ed, float(x), float(y), true);
    return;
  }
  if (a.ndrag.dragging) {  // 数值按钮：横向拖动改值（interface_handlers.cc:6126-6240）
    if (ui::num_drag_update(a.blk, a.ndrag, x, (mod_state() & kShift) != 0)) {
      prop_apply(a, a.ndrag.id, props_clip(a), a.ndrag.value);
      a.dirty = kRegionAll;
    }
    return;
  }
  if (modal_active(a)) {  // G / 拖动条带本体：光标时刻就是变换输入
    edge_pan_step(a, now_seconds());
    modal_update(a, x_time(a, float(x)));
    return;
  }
  // 此刻有按下正在进行 = 这一格事件交给它：CLICK / CLICK_DRAG 的阈值分级、闭环光标的搬运、
  // 每一格怎么换算，全在那个手势对象自己身上（gesture.cpp）。
  // 先抄一份槽再调 —— 子类的 drag() 有权把这一格交回去（条带本体越过阈值就 invoke 模态）。
  std::shared_ptr<Gesture> g = a.gesture;
  if (g) {
    g->move(a, x, y, down);
    return;
  }
  // 槽是空的：这一击谁都没答应（或已经交回给模态），只剩悬停。
  ui::Item *it = a.blk.hit(float(x), float(y));
  int hov = it ? it->id : 0;
  if (hov != a.hover) {  // 悬停只影响控件的 hot 底色
    a.hover = hov;
    a.dirty |= kRTopbar | kRProps | kRChan;
  }
  const bool div_hot = props_divider_at(a, float(x), float(y));
  if (div_hot != a.props_div_hot) {
    a.props_div_hot = div_hot;
    a.dirty |= kRProps;
  }
  const bool split_hot = split_divider_at(a, float(x), float(y));
  if (split_hot != a.split_div_hot) {
    a.split_div_hot = split_hot;
    a.dirty |= kRSplit;
  }
}

void on_lbutton_up(App &a) {
  a.ldown = false;
  a.edit_anchor = -1;  // 拖选结束：光标停在最后指到的那个字符边界
  if (a.ndrag.dragging) {
    const int id = a.ndrag.id;
    const bool moved = a.ndrag.moved;
    a.ndrag = {};
    a.dirty |= kRProps;
    // 一次拖动 = 一步撤消：transform.widget_num 在 Blender 也是松开才 ED_undo_push，
    // 否则拖一趟滑块会留下几十步。
    if (moved) undo_push(a, L"修改属性");
    // 单击而没拖动 = 进入文本输入（数值按钮改值的另一条路）。
    if (!moved && id) begin_edit(a, id, prop_text(a, id, props_clip(a)));
    return;
  }
  if (modal_active(a)) {  // 拖出来的 seq_slide：松开即提交
    modal_end(a, true);
    return;
  }
  // 收尾归这一次手势自己：框选落地、把手的撤消步、滚动条的翻页、取景框的撤消步、
  // 分界线的读数，全在自己的 up() 里（gesture.cpp / ops.cpp）。
  release_gesture(a);
  // B  armed 的框选只用一次（和 Blender 的 select_box 模态结束一样）。
  a.box_armed = false;
}

// ---------------------------------------------------------------- 中键 / 右键 / 滚轮

void on_mbutton_down(App &a, int x, int y) {
  a.mouse_x = x;
  a.mouse_y = y;
  // 中键这一击也一样：外壳只把事件交出去，答不答应（滚动条 / view2d.pan / view2d.zoom）
  // 由被点中的视图自己说（gesture.cpp 的 press_middle）。
  a.gesture = press_middle(a, float(x), float(y));
}

void on_mbutton_up(App &a) { release_gesture(a); }

// 滚轮的一格怎么分给两块视图，在 ops.cpp 的 wheel_step：窗口过程不判断指针落在哪一块地方。

// on_wheel = wheel_step + 一行结果 trace。滚轮的四种分工（缩放/Ctrl 横移/Shift 纵移/
// Alt 逐帧）光看画面分不出来，把改动前后的视图参数打进日志才好核对。
void on_wheel(App &a, int x, int y, short delta) {
  if (a.pop.is_open()) {  // 菜单开着：滚轮翻菜单，不碰时间线和面板
    if (a.pop.on_wheel(delta)) a.dirty = kRegionAll;
    return;
  }
  const WORD mods = mod_state();
  const double pps0 = a.tl.pps, st0 = a.tl.scroll_t, vc0 = a.tl.view_ch;
  const float z0 = a.view_zoom, px0 = a.view_pan_x, py0 = a.view_pan_y;
  const float ps0 = a.props_scroll;
  wheel_step(a, x, y, delta, mods);
  if (!trace_fp) return;
  wchar_t b[224];
  swprintf(b, 192,
           L"wheel %d,%d d=%d mods=%u pps %g->%g scroll %.2f->%.2f ch %.2f->%.2f "
           L"zoom %.2f->%.2f pan %.0f,%.0f->%.0f,%.0f pscr %.0f->%.0f",
           x, y, int(delta), mods, pps0, a.tl.pps, st0, a.tl.scroll_t, vc0, a.tl.view_ch, double(z0),
           double(a.view_zoom), px0, py0, a.view_pan_x, a.view_pan_y, double(ps0),
           double(a.props_scroll));
  trace(b);
}

// ---------------------------------------------------------------- 键盘

void on_key(App &a, WORD vk) {
  const WORD mods = mod_state();
  if (a.pop.is_open()) {  // 菜单打开时按键先归菜单（Esc 收一层 / Enter 执行悬停项）
    int r = a.pop.on_key(WORD(vk));
    if (r > 0) run_cmd(a, r);
    if (r != 0) a.dirty = kRegionAll;
    return;
  }
  // 输入期间不吃编辑快捷键（同 Blender 的 uiNumberField），但光标能动：
  // 左右/Home/End 移动，按住 Shift 从光标处拖出选区，Backspace 删左边、Delete 删右边。
  if (a.edit_id || a.rename_track >= 0) {
    const bool shift = (mods & kShift) != 0;
    const bool ren = a.rename_track >= 0;
    // 不带 Shift 的左右键先把选区收到靠移动方向的那一端，再走一格（所有文本框一致）。
    int c = a.edit_ts.caret;
    if (vk == VK_ESCAPE)
      ren ? commit_rename(a, false) : commit_edit(a, false);
    else if (vk == VK_RETURN && !ren && is_string_field(a.edit_id)) {
      // 内容框：回车换行，Ctrl+回车提交（裁决改口：Ctrl+回车不再换行）。点到框外也提交。
      if (mods & kCtrl)
        commit_edit(a, true);
      else
        edit_char(a, L'\n');
    } else if (vk == VK_RETURN)
      ren ? commit_rename(a, true) : commit_edit(a, true);
    else if (vk == VK_BACK)
      edit_backspace(a);
    else if (vk == VK_DELETE)
      edit_del(a);
    else if (vk == VK_LEFT)
      edit_set_caret(a, (shift ? c : std::min(c, a.edit_ts.sel)) - 1, shift);
    else if (vk == VK_RIGHT)
      edit_set_caret(a, (shift ? c : std::max(c, a.edit_ts.sel)) + 1, shift);
    else if (vk == VK_HOME)
      edit_set_caret(a, 0, shift);
    else if (vk == VK_END)
      edit_set_caret(a, INT_MAX, shift);
    else if ((vk == VK_UP || vk == VK_DOWN) && is_string_field(a.edit_id))
      edit_move_line(a, vk == VK_DOWN ? 1 : -1, shift);
    a.dirty = kRegionAll;
    return;
  }
  if (modal_active(a)) {
    if (vk == VK_RETURN || vk == VK_SPACE) modal_end(a, true);
    else if (vk == VK_ESCAPE || vk == VK_RBUTTON) modal_end(a, false);
    return;  // 其余按键留给模态算子（轴向锁定等未实现，等于忽略）
  }
  if (a.box_armed && vk == VK_ESCAPE) {
    a.box_armed = false;
    set_status(a, L"已取消框选");
    a.dirty = kRegionAll;
    return;
  }
  if (a.gesture) return;  // 手势期间不吃快捷键
  const Key *k = key_lookup(mods, BYTE(vk));
  if (trace_fp) {
    wchar_t b[80];
    swprintf(b, 80, L"key vk=%u mods=%u cmd=%d", WORD(vk), mods, k ? k->cmd : -1);
    trace(b);
  }
  if (!k) return;
  run_cmd(a, k->cmd);
  if (trace_fp) {
    wchar_t b[128];
    swprintf(b, 128, L"  -> f=%lld ph=%.3f pps=%g zoom=%.2f", (long long)a.cur_frame(),
             a.project.playhead, a.tl.pps, double(a.view_zoom));
    trace(b);
  }
  a.dirty = kRegionAll;  // 头部按钮/菜单的勾选态可能变了
}

// ---------------------------------------------------------------- worker → UI 的读
//
// 后台线程只负责说一声「这一格有东西了」；怎么读、读完重画哪一块，全在这一格。这四段以前
// 挂在每 15ms 的 tick 里靠轮询发现，空闲时也跟着每秒跑 60 遍。
static void consume_publish(App &a, HWND hwnd, int src) {
  switch (src) {
    case kPubPreview:
      if (a.preview.pump()) a.dirty |= kRViewport;
      break;
    case kPubWave: {
      // 声带波形由后台线程烘焙（第一次画某条声带时起，对应 Blender 的 waveform preview
      // job）。烘完一笔 —— 成功或失败都算 —— 就投递一次，所以「还有在烘的」那条轮询判据
      // （WaveCache::active()）不再需要：最后一笔也会自己说一声，不会差一帧。
      const int d = WaveCache::instance().done();
      if (d != a.wave_done) {
        a.dirty |= kRLanes;
        a.wave_done = d;
      }
      break;
    }
    case kPubAudio: {
      if (a.audio_err_shown) break;
      std::wstring e = a.audio.error();
      if (e.empty()) break;
      a.audio_err_shown = true;
      set_status(a, L"音频：" + e);
      trace(L"audio: " + e);
      break;
    }
    case kPubSync: {
      // 同步点表：算表的 worker 和懒验都只排队账，落盘的一直是这一格 ——
      // trace_fp 是全局 FILE*，两边同时 fwrite 会撞车。
      std::wstring sl;
      while (app::sync_take_trace(&sl)) trace(sl);
      break;
    }
    default:
      break;
  }
  if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- 窗口过程

static LRESULT handle_msg(App &a, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      a.hwnd = hwnd;
      g_publish_hwnd = hwnd;  // 从这一行起，worker 的产出有地方投递了
      // wm_window.cc:765-780：DPI 决定 pixelsize / UI_SCALE_FAC / widget_unit / 字号。
      {
        HDC dc = GetDC(hwnd);
        int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(hwnd, dc);
        btheme::set_dpi(dpi > 0 ? dpi : 96);
      }
      if (!a.r2d.init(hwnd)) {
        trace(L"D3D11 init failed: " + a.r2d.fail_reason());
        if (!a.shot_path.empty()) {
          PostQuitMessage(1);
          return 0;
        }
        MessageBoxW(hwnd, a.r2d.fail_reason().c_str(), L"VSE 初始化失败", MB_ICONERROR);
        return -1;
      }
      layout(a);
      // Blender 的 region 初始化就是 cur = tot（space_sequencer.cc:179），
      // 所以一进来先按场景帧范围铺满时间线，而不是用一个自创的 60px/s。
      timeline_view_all(a);
      build_keymap();
      trace(L"WM_CREATE ok");
      // 命令行那只素材的导入发生在建窗口之前，预览/波形线程可能已经在这几毫秒里交过一帧，
      // 而那时还没有窗口可以投递。所以先把投递的收件人挂上，再把每一格主动读一遍 ——
      // 少了这一遍，「启动就有的那一帧」要等下一次有人敲门才会出现。
      for (int s = 0; s < int(kPubCount); s++) consume_publish(a, hwnd, s);
      DragAcceptFiles(hwnd, TRUE);  // 直接拖视频文件进窗口导入
      return 0;
    case WM_SIZE:
      a.r2d.resize(int(LOWORD(lp)), int(HIWORD(lp)));
      layout(a);
      return 0;
    case WM_ERASEBKGND: {
      // 首刷 = 露脸而 D3D 首帧还没 Present 时 DWM 显示的那块底。用类上的深色刷子填一遍
      // 就把刷子摘掉（GHOST_SystemWin32.cc:2310 同款）：之后每一帧都归 D3D，系统不再插手。
      HBRUSH bg = (HBRUSH)GetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND);
      if (bg) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, bg);
        SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)nullptr);
        DeleteObject(bg);
      }
      return 1;
    }
    case WM_SETCURSOR:
      // 分界线上的光标换成左右箭头 = Blender 在 region_scale 里设的 WM_CURSOR_X_MOVE
      // （screen_ops.cc:3599）；wm_cursors.cc:99 那一款在 Win32 就是 IDC_SIZEWE。
      // 预览/时间线那条横线同理，方向换成上下（WM_CURSOR_V_MOVE）。
      if (LOWORD(lp) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        const bool on_div = a.props_div_hot || gesture_as<DividerV>(a) ||
                            props_divider_at(a, float(pt.x), float(pt.y));
        const bool on_split = a.split_div_hot || gesture_as<DividerH>(a) ||
                              split_divider_at(a, float(pt.x), float(pt.y));
        // 条带首尾把手：命中左/右缘 → 左右箭头，和按下用同一处命中层（strip_part）。
        const bool on_handle = !on_div && !on_split &&
                               strip_handle_hot(a, float(pt.x), float(pt.y));
        SetCursor(LoadCursor(nullptr, (on_div || on_handle) ? IDC_SIZEWE
                                  : on_split ? IDC_SIZENS
                                             : IDC_ARROW));
        return TRUE;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_DPICHANGED: {
      RECT *nr = reinterpret_cast<RECT *>(lp);
      SetWindowPos(hwnd, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      btheme::set_dpi(int(LOWORD(wp)));
      layout(a);
      return 0;
    }
    case WM_SETFOCUS:
      a.last_tick = now_seconds();  // 失焦期间的 tick 不计入播放推进，回来不能跳帧
      return 0;
    case WM_PAINT: {
      const double t0 = g_time_probe ? now_seconds() : 0.0;
      draw(a);
      if (g_time_probe) {
        ++g_probe.paints;
        const double ms = (now_seconds() - t0) * 1000.0;
        static double s_sum = 0.0;
        static int s_n = 0;
        ++s_n;
        s_sum += ms;
        g_draw_avg = s_sum / s_n;
        if (ms > g_draw_max) g_draw_max = ms;
      }
      ValidateRect(hwnd, nullptr);
      return 0;
    }
    case WM_TIMER: {
      const double enter = now_seconds();
      const double now = enter;
      const double dt = now - a.last_tick;
      a.last_tick = now;
      // 表里活着的行才推进；「此刻谁活着」顺手记下来，状态行里报出这一格醒着的原因。
      const ClockRow *live[sizeof(kClock) / sizeof(kClock[0])] = {};
      int n_live = 0;
      for (const ClockRow &r : kClock)
        if (r.needs(a)) {
          live[n_live++] = &r;
          r.advance(a, hwnd, now, dt);
        }
      if (g_time_probe) {
        const double cost = (now_seconds() - enter) * 1000.0;
        ++g_probe.ticks;
        g_probe.sum_cost += cost;
        if (cost > g_probe.max_cost) g_probe.max_cost = cost;
        const double gap = (now - g_probe.prev) * 1000.0;
        if (g_probe.prev > 0.0) {
          if (gap > g_probe.max_gap) g_probe.max_gap = gap;
          if (gap > 22.0) ++g_probe.late;
          ++g_probe.gaps;
          if (gap <= 20.0) ++g_probe.gapb[0];
          else if (gap <= 30.0) ++g_probe.gapb[1];
          else if (gap <= 40.0) ++g_probe.gapb[2];
          else if (gap <= 50.0) ++g_probe.gapb[3];
          else if (gap <= 75.0) ++g_probe.gapb[4];
          else ++g_probe.gapb[5];
        }
        g_probe.prev = now;
        if (now >= g_probe.next) {
          g_probe.next = now + 1.0;
          const auto st = a.preview.stats();
          // 同步点表的三格账：表数 / 其中开门的 / 还在路上的。P 单独一格是必需的 ——
          // 拖动刚开始时表必然还在路上，没有它，kf_out 里的「没赶上」和「登记没生效」分不出来。
          int sync_tables = 0, sync_open = 0, sync_pending = 0;
          app::sync_counts(&sync_tables, &sync_open, &sync_pending);
          // 这一秒烧掉的 CPU，分「本进程（含解码/烘焙/波形线程）」和「全部 ffmpeg 子进程」两本账。
          // 子进程那本用 job 的内核累计计数：它们来得快去得也快，按 100ms 轮询采样会整段漏掉。
          double self_ms = 0.0, child_ms = 0.0;
          {
            FILETIME fc, fe, fk, fu;
            if (GetProcessTimes(GetCurrentProcess(), &fc, &fe, &fk, &fu)) {
              const uint64_t k = (uint64_t(fk.dwHighDateTime) << 32) | fk.dwLowDateTime;
              const uint64_t u = (uint64_t(fu.dwHighDateTime) << 32) | fu.dwLowDateTime;
              self_ms = double(k + u) / 10000.0;
            }
            child_ms = std::max(0.0, Ffmpeg::children_cpu_ms());
          }
          static double prev_self = 0.0, prev_child = 0.0;
          const double dself = self_ms - prev_self, dchild = child_ms - prev_child;
          prev_self = self_ms;
          prev_child = child_ms;
          // p95 从直方图取「累计首次达到 95% 的那一档上边界」：报的是不超过值，不是精确分位。
          // 最后一档（>75ms）没有上界，直接写 >75 —— 假死的形状不许被一个数字盖住。
          int cum = 0, p95 = -1;
          for (int b = 0; b < 6; b++) {
            cum += g_probe.gapb[b];
            if (g_probe.gaps == 0 || double(cum) >= 0.95 * double(g_probe.gaps)) {
              const int edge[6] = {20, 30, 40, 50, 75, -1};
              p95 = edge[b];
              break;
            }
          }
          std::wstring l = L"tick=" + std::to_wstring(g_probe.ticks) + L"/s" +
                           L" paint=" + std::to_wstring(g_probe.paints) + L"/s" +
                           L" cpu=" + std::to_wstring(dself) + L"+" + std::to_wstring(dchild) +
                           L"ms" +
                           L" cost_avg=" + std::to_wstring(g_probe.sum_cost / g_probe.ticks) +
                           L"ms cost_max=" + std::to_wstring(g_probe.max_cost) +
                           L"ms gap_max=" + std::to_wstring(g_probe.max_gap) +
                           L"ms late=" + std::to_wstring(g_probe.late) +
                           // 帧间隔分布：先给 p95，再给 ≤20/≤30/≤40/≤50/≤75/>75 六档计数。
                           L" gap_p95=" +
                           (p95 < 0 ? std::wstring(L">75") : std::to_wstring(p95)) + L"ms" +
                           L" gap_h=" + std::to_wstring(g_probe.gapb[0]) + L"/" +
                           std::to_wstring(g_probe.gapb[1]) + L"/" + std::to_wstring(g_probe.gapb[2]) +
                           L"/" + std::to_wstring(g_probe.gapb[3]) + L"/" +
                           std::to_wstring(g_probe.gapb[4]) + L"/" + std::to_wstring(g_probe.gapb[5]) +
                           L" draw_avg=" + std::to_wstring(g_draw_avg) +
                           L"ms draw_max=" + std::to_wstring(g_draw_max) +
                           L"ms restart=" + std::to_wstring(st.restarts) +
                           L"(size=" + std::to_wstring(st.why_size) +
                           L",src=" + std::to_wstring(st.why_src) +
                           L",back=" + std::to_wstring(st.why_back) +
                           L",lag=" + std::to_wstring(st.why_lag) +
                           L",eof=" + std::to_wstring(st.why_eof) + L")" +
                           L" hit=" + std::to_wstring(st.cache_hits) +
                           L" reuse=" + std::to_wstring(st.reader_reuse) +
                           L" defer=" + std::to_wstring(st.deferred) +
                           // 拖动落点：三级按价钱排（缓存 / 池内顺解 / 就地定位），四条「表没接住」
                           // 按原因排（还没建好 / 门关或懒验不给付 / 没有拿得出来的流 / 定位失败）。
                           // 合并任何一格就只剩「用了几次关键帧」，数不出钱花在哪一级、也数不出为什么
                           // 这一跳没花。
                           L" kf=" + std::to_wstring(st.kf_free) + L"/" +
                           std::to_wstring(st.kf_walk) + L"/" + std::to_wstring(st.kf_paid) +
                           L" kf_out=" + std::to_wstring(st.kf_notready) + L"/" +
                           std::to_wstring(st.kf_gated) + L"/" + std::to_wstring(st.kf_nostream) +
                           L"/" + std::to_wstring(st.kf_seekfail) +
                           // 门关这一支里「本道有可用流 ⇒ 交给既有下游交付」真的接住了一格。
                           // 单独一格是设计的硬要求：合进 kf_gated 就读不出
                           // 这条路接住了几格、也读不出还有多少格停在「门关 ∧ 池空」那一类。
                           L" gc_serve=" + std::to_wstring(st.gc_serve) +
                           L" sync=" + std::to_wstring(sync_tables) + L"/" +
                           std::to_wstring(sync_open) + L"/" + std::to_wstring(sync_pending) +
                           L" kill=" + std::to_wstring(st.stale_kills) +
                           L" pool=" + std::to_wstring(st.readers_live) +
                           L" decoded=" + std::to_wstring(st.decoded) +
                           L" mf=" + std::to_wstring(st.mf_open) + L"/" +
                           std::to_wstring(st.mf_refused) + L" mfdec=" +
                           std::to_wstring(st.mf_decoded) +
                           L" seek=" + std::to_wstring(st.in_place_seeks) + L"@" +
                           std::to_wstring(st.max_seek_ms) + L"ms" +
                           L" first_max=" + std::to_wstring(st.max_first_ms) +
                           L"ms pump_max=" + std::to_wstring(st.max_pump_ms) + L"ms";
          trace(l);
          g_probe.ticks = 0;
          g_probe.paints = 0;
          g_probe.sum_cost = 0.0;
          g_probe.max_cost = 0.0;
          g_probe.max_gap = 0.0;
          g_probe.late = 0;
          g_probe.gaps = 0;
          for (int b = 0; b < 6; b++) g_probe.gapb[b] = 0;
        }
      }
      // 交互期间的状态行（VSE_TRACE）：截图脚本核对播放头/视图/条带位置用，不靠猜。
      // 「这一格该不该记」直接读上面那张表，不再抄一遍条件；顺带报出它是被哪几行叫醒的。
      if (n_live && trace_fp) {
        static double s_due = 0.0;
        if (now >= s_due) {
          s_due = now + 0.25;
          std::wstring live_names;
          for (int i = 0; i < n_live; i++) {
            if (i) live_names += L"+";
            live_names += live[i]->name;
          }
          Clip *mc = a.project.clips.empty() ? nullptr : &a.project.clips.front();
          wchar_t sb[256];
          swprintf(sb, 256,
                   L"state live=%ls ph=%.3f f=%lld scroll=%.3f pps=%g modal=%d strip=%.3f x=%d y=%d",
                   live_names.c_str(), a.project.playhead, (long long)a.cur_frame(), a.tl.scroll_t,
                   a.tl.pps, int(a.modal.kind), mc ? mc->timeline_start : -1.0, a.mouse_x, a.mouse_y);
          trace(sb);
        }
      }
      // 只在真有变化时标脏：空闲不重绘、不 Present（Blender 没收到 notifier 就不重绘）。
      if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      on_lbutton_down(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDBLCLK:
      if (trace_fp) trace(L"dblclk");
      on_lbutton_dblclk(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      on_lbutton_up(a);
      if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_MBUTTONDOWN:
      SetCapture(hwnd);
      on_mbutton_down(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MBUTTONUP:
      ReleaseCapture();
      on_mbutton_up(a);
      return 0;
    case WM_RBUTTONDOWN:
      // 右键这一击同样只是交出去：开哪个菜单由「谁认出指针这一点」决定（ops.cpp 的
      // on_rbutton_down）。
      on_rbutton_down(a, float(GET_X_LPARAM(lp)), float(GET_Y_LPARAM(lp)));
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_MOUSEMOVE:
      on_mouse_move(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & (MK_LBUTTON | MK_MBUTTON)) != 0);
      if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_MOUSEWHEEL: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // 滚轮给的是屏幕坐标，要换成客户区
      ScreenToClient(hwnd, &p);
      on_wheel(a, p.x, p.y, GET_WHEEL_DELTA_WPARAM(wp));
      if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_CHAR:
      if (a.edit_id || a.rename_track >= 0) {
        edit_char(a, wchar_t(wp));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      return DefWindowProc(hwnd, msg, wp, lp);
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:  // Alt+键在 Win32 里是 SYSKEYDOWN，两类都要收
      on_key(a, WORD(wp));
      if (a.dirty) InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wp);
      wchar_t buf[MAX_PATH * 4];
      std::vector<std::wstring> paths;
      for (UINT i = 0, n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0); i < n; ++i) {
        UINT c = DragQueryFileW(drop, i, buf, DWORD(std::size(buf)));
        if (c) paths.emplace_back(buf, c);
      }
      if (trace_fp) {
        wchar_t b[96];
        // 注意：MinGW 的 swprintf 里 %s 会按窄字符解释，宽字符串要用 %hs 之外的
        // 显式长度，这里只报长度和首字符。
        swprintf(b, 96, L"drop files=%llu first_len=%llu first=%lc",
                 static_cast<unsigned long long>(paths.size()),
                 static_cast<unsigned long long>(paths.empty() ? size_t(0) : paths.front().size()),
                 paths.empty() ? L'-' : paths.front()[0]);
        trace(b);
      }
      POINT pt{};
      DragQueryPoint(drop, &pt);  // 已经是客户区坐标
      DragFinish(drop);
      if (paths.empty()) return 0;
      // 落点决定条带的位置，对照 sequencer_drag_drop.cc:275-279 与 :370-389：
      // 落在时间线里 = 用鼠标的时刻和通道；落在别处（预览区）= 用当前帧 +
      // 该帧上最靠上的那条通道。
      double at = -1.0;
      int track = -1;
      if (a.scrub.contains(float(pt.x), float(pt.y)) || a.lanes.contains(float(pt.x), float(pt.y))) {
        at = x_time(a, float(pt.x));  // 落点可为负：帧轴是有符号的，不钳 0
        const int t = track_at_y(a, float(pt.y));
        // 通道是扁平的一列（channels.cc:30-40），落在哪条就是哪条；还没建出来的行
        // 由 channel_slot 补出来。
        if (t >= 0) track = t;
      } else {
        int top = -1;
        for (const Clip &c : a.project.clips) {
          if (c.timeline_start > a.project.playhead || c.timeline_start + c.duration < a.project.playhead) continue;
          // 该帧上最靠上的通道（sequencer_drag_drop.cc:370-389 的「当前帧上的通道」）。
          top = std::max(top, c.track);
        }
        if (top >= 0) track = top;
      }
      import_paths(a, paths, at, track);
      a.dirty = kRegionAll;
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case kMsgPublish: {
      // 先清这一格的账，再读：清完到读完之间新产出的那一次会重新投递，所以既不丢也不排队。
      const int src = int(wp);
      if (src >= 0 && src < int(kPubCount)) g_publish_pending[src].store(false);
      consume_publish(a, hwnd, src);
      return 0;
    }
    case WM_DESTROY:
      KillTimer(hwnd, kTick);
      a.tick_armed = false;
      // 关闭 exe = 保存启动默认：窗口状态、格式参数、布局、小三角、文本组整份落盘，
      // 不需要用户再点一次菜单（菜单里只留「恢复启动默认」）。
      settings_save(a);
      g_publish_hwnd = nullptr;  // 之后 worker 再说「有东西了」就丢掉：窗口已经没有了
      // 关窗口就是关窗口：正在跑的导出先掐掉（ffmpeg 只靠 -progress 管道连着主程序，
      // 输出却写进文件，主程序没了它也不会察觉，会自己闷头跑到完 —— 任务管理器里
      // 就是「窗口关了进程还在」）。再把还在解码声波的子进程收掉，最后拆渲染层。
      a.exporter.cancel();
      WaveCache::instance().cancel_all();
      a.preview.shutdown();
      a.r2d.shutdown();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

// 外壳唯一的时钟开关：每条消息处理完问一遍「此刻有没有东西在按时间推进」。空闲 ⇒ 没有
// tick ⇒ GetMessage 一直睡着，整机零消耗。
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App &a = app_ref();
  const LRESULT lr = handle_msg(a, hwnd, msg, wp, lp);
  if (msg != WM_DESTROY) clock_sync(a, hwnd);
  return lr;
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE self, HINSTANCE, PWSTR, int) {
  using namespace app;
  App &g = app_ref();
  // 系统定时粒度约 15.6ms：16ms 的 tick 会被并成约 31ms（实测 32 次/秒）。
  timeBeginPeriod(1);
  wchar_t tp[MAX_PATH];
  if (GetEnvironmentVariableW(L"VSE_TRACE", tp, MAX_PATH) > 0) {
    trace_fp = _wfopen(tp, L"wt, ccs=UTF-8");  // 否则中文在日志里是问号
    if (!trace_fp) trace_fp = _wfopen(tp, L"w");
  }
  g_time_probe = GetEnvironmentVariableW(L"VSE_TIME", tp, MAX_PATH) > 0;

  // 命令行：素材路径直接导入；--shot 截图退出；--seek 定位播放头；--cut 在播放头剪切；
  // --export 导出并退出；--play 启动即播放；--hold 让窗口多活若干秒（用于观察音频推流）。
  // --crop / --xform 只给自动化验证用：把裁切和变换直接写进第一个片段，再截图比对像素，
  // 免得每次判断「裁切是不是被当成了拉伸」都要靠手点界面。
  // --drop 同样只为验证：把文件当作拖放导入，走真实的 WM_DROPFILES 消息。
  // --pct 写「缩放 %」（r.pcent），用来核对导出尺寸真的是 分辨率 × %。
  // --speed 写第一个片段的「速度 %」（100=原速），走面板那条 prop_apply 路，
  // 好让验证脚本和手输碰到的是同一段时长联动代码。
  // --key 通道:帧:值 给第一个片段插一个关键帧（通道 0=位置X 1=位置Y 2=缩放X
  // 3=缩放Y 4=旋转），可重复；求值结果写进日志的 xform_at 行。
  // --text 调的就是右键「添加文本」那一个算子（不是另开一条造数据的路），好让截图能证明
  // 用户点到的那条链真的落下条带、排版器真的出图。
  std::vector<std::wstring> files;
  std::wstring shot, export_path, drop_file, ffmpeg_url;
  bool ffmpeg_fetch = false;
  double seek = -1.0, hold = 0.0, export_delay = -1.0;
  bool cut = false, play = false;
  int add_txt = 0;
  int crop[4] = {0, 0, 0, 0};
  double xs = 1.0, ys = 1.0, rot = 0.0, ox = 0.0, oy = 0.0;
  double org_x = 0.5, org_y = 0.5;
  // --key 每给一次就攒一个三元组（通道, 帧, 值）；可以重复用同一条命令插多个关键帧。
  std::vector<double> key_list;
  // --speed 支持 50:60:70 这样的连设：整条带改速每次都要先清键再重建曲线，只有同一个进程
  // 里连走几遍 prop_apply，才看得出「长度被一遍遍顶长」的累积。
  std::vector<double> speed_list;
  // --rkey 时刻 / --retime 时刻:速度% —— 直接驱动 retiming_add_key 与 retiming_set_speed，
  // 也就是 I 菜单和「速度」字段走的同两条路。用来在没有界面的条件下验证多段变速的预览
  // 与导出（导出把曲线摊成逐段恒速，见 export.cpp 的 retiming_slices）。
  std::vector<double> rkey_list, retime_list;
  // --at 片段序号:通道:起始秒 —— 只为自动化验证用：把已经导入的片段搬到指定通道和时刻，
  // 好在「重叠」这类手工拖不出来的布局上跑导出与截图比对。
  struct AtMove {
    int idx, track;
    double start;
  };
  std::vector<AtMove> at_list;
  // --slide 片段序号:横移帧:纵移通道 —— 没有鼠标也能走真的模态那条路（modal_begin →
  // modal_update → modal_end 提交）：落位让位、关键帧跟随、撤消步都和鼠标拖出来的一样。
  // 配 --at 先摆出重叠，再复现「拖到已有条带上松手」那一下。kill switch: 不过门即删，不优化
  struct SlideMove {
    int idx, frames, channels;
  };
  std::vector<SlideMove> slide_list;
  // --wheel 客户区x:y:格数 —— 投真的 WM_MOUSEWHEEL 给本窗口，走窗口过程那一路：滚轮落在
  // 哪一块地方就由哪一块对象回答，所以这条不改任何属性，只送事件。正数 = 上滚。
  // kill switch: 不过门即删，不优化
  int wheel_x = 0, wheel_y = 0, wheel_n = 0;
  // --cuts 120:240:… —— 在同一趟里连切多刀（--cut 只切一刀），帧号用源帧网格。
  // --fmt-source —— 把输出格式钉到素材自己（尺寸/帧率/像素比/有无音轨）。
  // 这几条只服务量测趟：搭一个「多段首尾相接」的工程去比「重编」与「直通」两臂，
  // 默认路径不读它们。kill switch: 不过门即删，不优化
  std::vector<double> cut_frames;
  bool fmt_source = false;
  // --src-in 0.016 —— 把导入条带的源窗口整体往后推 d 秒（条带同步变短 d）。
  // 1.mp4 的画面 pts 起点不是 0 而是 0.016（容器有 edit list），而 concat 的 inpoint 按
  // 文件 pts 算 ⇒ 源网格不对齐的话，切在帧 120 的 inpoint 会落在 IDR 前 0.016 秒，直通臂
  // 就偷偷多抄一整个 GOP（清单按帧网格对齐 vs 按 IDR 对齐：362 帧 vs 242 帧）。
  // 这只挪数据，不碰导出逻辑。
  // kill switch: 不过门即删，不优化
  double src_in_shift = -1.0;
  bool has_crop = false, has_xform = false, has_org = false, has_speed = false;
  auto parse_doubles = [](const std::wstring &s, double *out, int n) {
    int got = 0;
    size_t pos = 0;
    while (got < n && pos <= s.size()) {
      size_t c = s.find(L':', pos);
      out[got++] = wcstod(s.substr(pos, c == std::wstring::npos ? std::wstring::npos : c - pos)
                              .c_str(),
                          nullptr);
      if (c == std::wstring::npos) break;
      pos = c + 1;
    }
    return got == n;
  };
  // 设置文件在解析命令行之前读：命令行是「这一趟」的覆盖，设置文件是「每一趟」的默认，
  // 所以默认先落地、命令行后说话。反过来（原来在建窗口前才读）会让清单里的 crf/resolution
  // 顶掉 --quality/--pct，读日志的人以为参数生效了，其实被上一行的默认吃回去。
  // 格式参数仍旧赶在第一次导入之前（导入只在时间线还空着时改分辨率/帧率），窗口状态赶在
  // CreateWindowExW 之前 —— 这两条靠的是「解析完命令行才建窗口」这个顺序，不是靠读文件的时机。
  settings_load(g);
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) {
    std::wstring s = argv[i];
    if (s == L"--shot" && i + 1 < argc) shot = argv[++i];
    else if (s == L"--export" && i + 1 < argc) export_path = argv[++i];
    else if (s == L"--export-delay" && i + 1 < argc) export_delay = wcstod(argv[++i], nullptr);
    else if (s == L"--seek" && i + 1 < argc) seek = wcstod(argv[++i], nullptr);
    else if (s == L"--hold" && i + 1 < argc) hold = wcstod(argv[++i], nullptr);
    else if (s == L"--cut") cut = true;
    else if (s == L"--text") ++add_txt;  // 写几次就建几条，叠着的文本带正是排版器共用一张图的那种
    else if (s == L"--play") play = true;
    else if (s == L"--fmt-source") fmt_source = true;
    else if (s == L"--cuts" && i + 1 < argc) {
      // 冒号分隔的帧号列表，个数不定（parse_doubles 的 n 是固定的，这里自己切）。
      const std::wstring v = argv[++i];
      size_t pos = 0;
      while (pos < v.size()) {
        size_t c = v.find(L':', pos);
        if (c == std::wstring::npos) c = v.size();
        if (c > pos) cut_frames.push_back(wcstod(v.substr(pos, c - pos).c_str(), nullptr));
        pos = c + 1;
      }
    }
    else if (s == L"--src-in" && i + 1 < argc) src_in_shift = wcstod(argv[++i], nullptr);
    else if (s == L"--drop" && i + 1 < argc) drop_file = argv[++i];
    // --ffmpeg-url 地址 / --ffmpeg-fetch：只为自动化验证用。前者覆盖下载地址
    // （在本机一个小 http 服务上跑完整条「下载→解压→搬文件」链，不必拉真镜像那几百 MB），
    // 后者不等点顶栏菜单、启动就直接开跑。
    else if (s == L"--ffmpeg-url" && i + 1 < argc) ffmpeg_url = argv[++i];
    else if (s == L"--ffmpeg-fetch") ffmpeg_fetch = true;
    else if (s == L"--crop" && i + 1 < argc) {
      double v[4];
      if (parse_doubles(argv[++i], v, 4)) {
        for (int k = 0; k < 4; ++k) crop[k] = int(v[k]);
        has_crop = true;
      }
    } else if (s == L"--pct" && i + 1 < argc) {
      g.project.fmt.scale_percent = int(wcstod(argv[++i], nullptr));
    } else if (s == L"--speed" && i + 1 < argc) {
      // 冒号分隔的多个百分数按顺序连设（parse_doubles 的 n 是固定的，这里个数不定，自己切）。
      const std::wstring v = argv[++i];
      size_t pos = 0;
      while (pos < v.size()) {
        size_t c = v.find(L':', pos);
        if (c == std::wstring::npos) c = v.size();
        if (c > pos) speed_list.push_back(wcstod(v.substr(pos, c - pos).c_str(), nullptr));
        pos = c + 1;
      }
      has_speed = true;
    } else if (s == L"--key" && i + 1 < argc) {
      double v[3];
      if (parse_doubles(argv[++i], v, 3)) {
        key_list.push_back(v[0]);
        key_list.push_back(v[1]);
        key_list.push_back(v[2]);
      }
    } else if (s == L"--rkey" && i + 1 < argc) {
      rkey_list.push_back(wcstod(argv[++i], nullptr));
    } else if (s == L"--retime" && i + 1 < argc) {
      double v[2];
      if (parse_doubles(argv[++i], v, 2)) {
        retime_list.push_back(v[0]);
        retime_list.push_back(v[1]);
      }
    } else if (s == L"--at" && i + 1 < argc) {
      double v[3];
      if (parse_doubles(argv[++i], v, 3)) at_list.push_back({int(v[0]), int(v[1]), v[2]});
    } else if (s == L"--slide" && i + 1 < argc) {
      double v[3];
      if (parse_doubles(argv[++i], v, 3)) slide_list.push_back({int(v[0]), int(v[1]), int(v[2])});
    } else if (s == L"--wheel" && i + 1 < argc) {
      double v[3];
      if (parse_doubles(argv[++i], v, 3)) {
        wheel_x = int(v[0]);
        wheel_y = int(v[1]);
        wheel_n = int(v[2]);
      }
    } else if (s == L"--range" && i + 1 < argc) {
      double v[2];
      if (parse_doubles(argv[++i], v, 2)) {
        g.project.fmt.frame_start = int64_t(v[0]);
        g.project.fmt.frame_end = int64_t(v[1]);
      }
    } else if (s == L"--step" && i + 1 < argc) {
      // 步长（scene.frame_step）：只影响导出的取帧，命令行留一口好测。
      g.project.fmt.frame_step = std::max(1, int(wcstod(argv[++i], nullptr)));
    } else if (s == L"--quality" && i + 1 < argc) {
      // 输出质量（ffmpeg.constant_rate_factor 的三个在用档：-1 恒定码率 / 0 无损 / 17 感知无损）。
      g.project.fmt.crf = int(wcstod(argv[++i], nullptr));
    } else if (s == L"--bitrate" && i + 1 < argc) {
      // 码率（ffmpeg.video_bitrate，kbit/s）：只有 --quality -1 那一档会读它。
      g.project.fmt.bitrate = std::max(0, int(wcstod(argv[++i], nullptr)));
    } else if (s == L"--origin" && i + 1 < argc) {
      double v[2];
      if (parse_doubles(argv[++i], v, 2)) {
        org_x = v[0]; org_y = v[1];
        has_org = true;
      }
    } else if (s == L"--xform" && i + 1 < argc) {
      double v[5];
      if (parse_doubles(argv[++i], v, 5)) {
        xs = v[0]; ys = v[1]; rot = v[2]; ox = v[3]; oy = v[4];
        has_xform = true;
      }
    } else if (!s.empty() && s[0] != L'-') files.push_back(std::move(s));
  }
  if (argv) LocalFree(argv);
  g.shot_path = shot;

  SetProcessDPIAware();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASSEXW wc{};
  // hIconSm 只在 Ex 那一版里有（WNDCLASSW 没有这个成员），而标题栏左上角那颗和任务栏
  // 预览用的正是它，所以注册走 RegisterClassExW。
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = self;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // 图标 = vse.rc 里的 1 号 ICON（icon.ico，单张 48x48 32 位）。类的 hIcon 管标题栏 +
  // 任务栏，hIconSm 管小那一档；exe 文件本身的图标由资源目录给 Explorer 用。
  // 资源里只有一张 48，LoadImage 按要的尺寸重采样（高 DPI 下 SM_CXICON/SM_CXSMICON
  // 已经是缩放后的像素数）。
  wc.hIcon = (HICON)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                               LR_DEFAULTCOLOR);
  wc.hIconSm = (HICON)LoadImageW(self, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                 LR_DEFAULTCOLOR);
  // 首刷底色 = 界面背景色（GHOST_SystemWin32.cc:643 同款）：窗口露脸而 D3D 首帧还没
  // Present 时，DWM 显示的是这块底色而不是白表面。WM_ERASEBKGND 里刷一次就摘刷子。
  wc.hbrBackground = CreateSolidBrush(RGB(btheme::window_back.r, btheme::window_back.g,
                                          btheme::window_back.b));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  // ffmpeg 的优先级（需求）：PATH → 主程序同目录 → 都没有就走顶栏「下载」。
  std::wstring ff_from;
  g.has_ffmpeg = Ffmpeg::locate(nullptr, nullptr, &ff_from);
  trace(L"ffmpeg 定位（" + ff_from + L"）");
  if (!g.has_ffmpeg) {
    set_status(g, L"找不到 ffmpeg.exe / ffprobe.exe：点顶栏「下载」→「下载FFmpeg」");
  }
  if (!ffmpeg_url.empty()) ffmpeg_set_url_override(ffmpeg_url);
  if (ffmpeg_fetch) ffmpeg_fetch_start();
  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  int w = int(sw * 0.8), h = int(sh * 0.8);
  int wx = (sw - w) / 2, wy = (sh - h) / 2;
  if (g.win_saved) {
    w = g.win_w;
    h = g.win_h;
    wx = g.win_x;
    wy = g.win_y;
  }
  HWND hwnd = CreateWindowExW(0, kClass, L"VideoEditor - ClassRonin", WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS,
                              wx, wy, w, h, nullptr, nullptr, self, nullptr);
  if (!hwnd) return 1;
  // 标题栏/边框交给 DWM 的深色开关（19041 起 = 20）。不设它，非客户区按系统浅色主题画成白条，
  // 和客户区自绘的深色界面接缝刺眼 —— 那是「窗口框架」这一层的属性，跟客户区画什么无关。
  const BOOL dark_title = TRUE;
  DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark_title, sizeof(dark_title));
  // 这里不 ShowWindow：下面还要跑命令行导入、探测（几百毫秒），露脸挪到 undo_reset 之后。
  // 露脸那一刻 D3D 首帧还没 Present，DWM 显示的是类背景刷子的颜色 —— 注册时已设成
  // 界面背景色（见 hbrBackground），所以第一眼是深灰底而不是白表面；首帧紧随 UpdateWindow 画出。
  trace(L"window created");

  // --drop 只给自动化验证用：自己造一个 HDROP 发给本窗口，走真实的 WM_DROPFILES 路径
  // （DragQueryFileW/DragQueryPoint/DragFinish），而不是直接调 import_paths 抄近路。
  if (!drop_file.empty()) {
    // 这套 MinGW 的 shellapi.h 只声明了 HDROP 和 DragQueryFileW，没有 DROPFILES 本体，
    // 所以按 Windows SDK 的字段顺序本地镜像一份（无 8 字节成员，32/64 位都是 20 字节）。
    struct DropFiles {
      DWORD p_files;
      POINT pt;
      BOOL f_nc;
      BOOL f_wide;
    };
    static_assert(sizeof(DropFiles) == 20, "DROPFILES 布局变了");
    const size_t hdr = sizeof(DropFiles);
    HGLOBAL hg = GlobalAlloc(GHND, hdr + (drop_file.size() + 2) * sizeof(wchar_t));
    if (hg) {
      auto *df = static_cast<DropFiles *>(GlobalLock(hg));
      df->p_files = DWORD(hdr);
      df->pt.x = LONG(g.lanes.x + btheme::u(2.0f));  // 落在时间线上 = 按鼠标时刻插入
      df->pt.y = LONG(g.lanes.y + g.lanes.h * 0.5f);
      df->f_wide = TRUE;
      memcpy(reinterpret_cast<char *>(df) + hdr, drop_file.data(),
             (drop_file.size() + 1) * sizeof(wchar_t));
      GlobalUnlock(hg);
      PostMessageW(hwnd, WM_DROPFILES, WPARAM(hg), 0);
    }
  }

  import_paths(g, files);
  // 播放头先落，再建文本带：add_text 的起点读的就是播放头（右键那一条也是），把 seek 排在它
  // 之前，「--seek + --text」交出的落位才和用户在界面里拖过去再点右键是同一个数。
  if (seek >= 0) g.project.playhead = seek;
  // --text：走右键那一个算子（add_text），落位、默认内容、默认长度都由它自己说。排在改数据
  // 那几组旗标之前 —— 它们只认第一个片段，而文本带是追加到末尾的；先建出来，命令行才谈得上
  // 给文本带本身设变换、插关键帧（用户报的「旋转 + 移位后导出」就是这么来的）。
  for (int k = 0; k < add_txt; k++) add_text(g);
  // --at：把指定片段搬到「通道 + 起始秒」。只做数据搬运，界面在下一次重绘时自己跟上。
  // 通道不够就先补出来（本工程按需建 Track，见 channel_slot）。
  for (const AtMove &m : at_list)
    if (m.idx >= 0 && m.idx < int(g.project.clips.size()) && m.track >= 0) {
      channel_slot(g, m.track);
      Clip &c = g.project.clips[size_t(m.idx)];
      c.track = m.track;
      c.timeline_start = m.start;
    }
  if (!at_list.empty()) g.dirty = kRegionAll;
  for (const SlideMove &m : slide_list) {
    if (m.idx < 0 || m.idx >= int(g.project.clips.size())) continue;
    select_only(g, g.project.clips[size_t(m.idx)].id);
    g.mouse_x = 0;
    g.mouse_y = 0;
    modal_begin(g, ModalState::Slide);
    g.mouse_y = float(-m.channels * channel_h(g));
    modal_update(g, g.project.playhead + double(m.frames) * g.frame_dur());
    modal_end(g, true);
    const Clip &c = g.project.clips[size_t(m.idx)];
    wchar_t sb[128];
    swprintf(sb, 128, L"slide cli: id=%lld 通道=%d 起=%lld帧", (long long)c.id, c.track,
             (long long)g.t_to_frame(c.timeline_start));
    trace(sb);
  }
  if (!slide_list.empty()) g.dirty = kRegionAll;
  if (has_crop || has_xform || has_org || has_speed) {
    for (Clip &c : g.project.clips) {
      if (has_crop) {
        c.crop_l = crop[0]; c.crop_r = crop[1]; c.crop_t = crop[2]; c.crop_b = crop[3];
      }
      if (has_xform) {
        c.scale_x = xs; c.scale_y = ys; c.rotation = rot; c.ofs_x = ox; c.ofs_y = oy;
      }
      if (has_org) {
        c.origin_x = org_x; c.origin_y = org_y;
      }
      // 速度不能直接写字段：内容不变、条带长度要按 1/speed 重算，那是 prop_apply 的活。
      for (double sp : speed_list) prop_apply(g, kFSpeed, &c, sp);
      if (has_speed) trace_retime(g, c, L"speed-chain");
      break;  // 只作用在第一个片段上
    }
  }
  // --rkey / --retime —— 第一条片段的变速曲线：先按 I 菜单那条路插键，再按「速度」
  // 字段那条路改所在段的速度（retiming_set_speed 改的是 x 所在的那一段）。
  if (!rkey_list.empty() || retime_list.size() >= 2) {
    for (Clip &c : g.project.clips) {
      for (double x : rkey_list) retiming_add_key(c, x);
      for (size_t k = 0; k + 1 < retime_list.size(); k += 2)
        retiming_set_speed(c, retime_list[k], retime_list[k + 1] / 100.0, g.frame_dur());
      c.show_retiming = true;
      recompute_duration(g);
      trace_retime(g, c, L"cli");
      break;  // 只作用在第一个片段上
    }
  }
  // --key 通道:帧:值 —— 往第一个片段的对应曲线插一个关键帧，走的就是面板将来要走的
  // 同一个 FCurve::set_at。用来在没有界面的条件下验证「插点 → 求值 → 预览」这条链。
  for (size_t k = 0; k + 2 < key_list.size(); k += 3) {
    const int ch = int(key_list[k]);
    if (ch < 0 || ch >= kAnimCount || g.project.clips.empty()) continue;
    Clip &c = g.project.clips.front();
    c.anim[ch].set_at(key_list[k + 1], float(key_list[k + 2]));
  }
  // 求值结果打成一行日志：验证插值时读这个数，比量截图像素可靠。
  if (!key_list.empty() && !g.project.clips.empty()) {
    const XformAt x = xform_at(g.project.clips.front(), g.project.fmt, g.project.playhead);
    wchar_t tb[192];
    swprintf(tb, 192, L"xform_at ph=%.3f fr=%lld ofs=(%.3f,%.3f) scale=(%.3f,%.3f) rot=%.3f",
             g.project.playhead, (long long)g.project.fmt.t_to_frame(g.project.playhead), x.ofs_x, x.ofs_y,
             x.scale_x, x.scale_y, x.rotation);
    trace(tb);
  }
  // --src-in：把所有条带的源窗口整体后推 d 秒（时间线左缘不动、长度同步减 d），于是「源内
  // 秒」和文件 pts 同一把尺 —— 直通清单的 inpoint 才能正好落在 IDR 上。只挪数据。
  // kill switch: 不过门即删，不优化
  if (src_in_shift > 0) {
    int moved = 0;
    for (Clip &c : g.project.clips) {
      if (c.duration <= src_in_shift) continue;
      c.source_in += src_in_shift;
      c.duration -= src_in_shift;
      moved++;
    }
    wchar_t tb[96];
    swprintf(tb, 96, L"量具: 源窗口后推 %.3fs 条带 %d 条", src_in_shift, moved);
    trace(tb);
  }
  // --fmt-source：输出格式钉到素材自己。存盘默认可能是 24fps + 无音频，而直通臂只能按源
  // 出片 ⇒ 两臂不落在同一组参数上，D1−D2 差就不是「重编 vs 直通」，是「24fps 无音轨 vs
  // 59.94 有音轨」。kill switch: 不过门即删，不优化
  if (fmt_source) {
    for (Clip &c : g.project.clips) {
      if (c.kind != kStripMovie || !c.info) continue;
      const VideoInfo &vi = *c.info;
      OutputFormat &f = g.project.fmt;
      if (vi.width > 0 && vi.height > 0) {
        f.width = vi.width;
        f.height = vi.height;
      }
      const double rate = vi.frame_rate.value();
      if (rate > 0) {
        f.fps = int(std::lround(rate));
        f.fps_base = f.fps > 0 ? double(f.fps) / rate : 1.0;
      }
      if (vi.sar_num > 0 && vi.sar_den > 0) {
        f.par_x = double(vi.sar_num);
        f.par_y = double(vi.sar_den);
      }
      f.no_audio = !vi.has_audio;
      break;
    }
    const OutputFormat &f = g.project.fmt;
    wchar_t tb[200];
    swprintf(tb, 200,
             L"量具: 格式钉到源 %dx%d %.6g fps par=%.4g/%.4g audio=%d crf=%d 范围=%d..%d",
             f.width, f.height, f.fps_value(), f.par_x, f.par_y, f.no_audio ? 0 : 1, f.crf,
             f.frame_start, f.frame_end);
    trace(tb);
  }
  // --cuts 帧号：同一趟里连切多刀（--cut 只有播放头那一刀）。每一刀把穿过播放头的条带全
  // 选上再切 ⇒ 影片和它下面的声带一起断，和用户框选两段按 K 的结果一致，D1 的混音路数
  // 也就对得上。kill switch: 不过门即删，不优化
  if (!cut_frames.empty()) {
    std::sort(cut_frames.begin(), cut_frames.end());
    for (double fr : cut_frames) {
      g.project.playhead = fr * g.frame_dur();
      const double t = g.frame_to_t(g.cur_frame());
      clear_selection(g);
      int hit = 0;
      for (Clip &c : g.project.clips) {
        // 切分找的是素材自己那条带：--text 建的文本带不参与（它现在排在切分之前建出来）。
        if (c.kind == kStripText) continue;
        if (t > c.timeline_start && t < c.timeline_start + c.duration) {
          add_to_selection(g, c.id);
          hit++;
        }
      }
      if (!hit) continue;
      split_selected(g, false);
    }
    wchar_t tb[96];
    swprintf(tb, 96, L"量具: 切 %d 刀 → 条带 %d 条", int(cut_frames.size()),
             int(g.project.clips.size()));
    trace(tb);
  }
  if (cut) {
    Clip *c = clip_at(g, g.project.playhead);
    if (c && c->kind == kStripText) c = nullptr;  // 同 --cuts：切分只管素材带
    if (c) {
      select_only(g, c->id);
      split_selected(g, false);
    }
  }
  if (seek >= 0 || cut || !files.empty() || add_txt) sync_preview(g);
  if (!g.shot_path.empty()) g.shot_deadline = now_seconds() + 4.0 + hold;
  g.shot_hold = now_seconds() + hold;
  if (play) {
    g.playing = true;
    g.last_tick = now_seconds();
  }
  if (!export_path.empty()) {
    // --export-delay：把手势脚本跑完之后的数据导出去（拖键的落地状态只有那时才有）。
    // 这一条不「导完就退」—— 收尾交给 --shot 的到期时间，好让同一趟既能截图又能导出。
    if (export_delay >= 0.0) {
      g.export_at = now_seconds() + export_delay;
      g.export_path = export_path;
    } else {
      g.export_quit = true;
      start_export(g, export_path);
    }
  }
  // 撤消链的基线 = 窗口建好、命令行素材也导进来之后的这份数据。
  undo_reset(g);
  // ShowWindow 会同步送出 WM_SIZE，布局在这儿才成型；露脸第一眼是深色底（类刷子），
  // 紧接 UpdateWindow 画出真正的第一帧。
  ShowWindow(hwnd, g.win_max ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
  UpdateWindow(hwnd);
  // --wheel 的几格在首帧画完之后才投：面板能滚多少读的是上一趟量出来的内容高，第一帧之前
  // 那个数是 0，投早了会被绘制那一趟的界抵掉（界归绘制，不是这里的界）。
  if (wheel_n != 0) {
    POINT pw{LONG(wheel_x), LONG(wheel_y)};
    ClientToScreen(hwnd, &pw);  // 窗口过程按屏幕坐标收滚轮，这里换算回客户区是它的事
    const SHORT d = wheel_n > 0 ? WHEEL_DELTA : SHORT(-WHEEL_DELTA);
    for (int k = 0, nk = wheel_n > 0 ? wheel_n : -wheel_n; k < nk; k++)
      PostMessageW(hwnd, WM_MOUSEWHEEL, MAKELPARAM(0, d), MAKELPARAM(pw.x, pw.y));
  }
  MSG m;
  while (GetMessage(&m, nullptr, 0, 0)) {
    TranslateMessage(&m);
    DispatchMessage(&m);
  }
  CoUninitialize();
  timeEndPeriod(1);
  if (trace_fp) fclose(trace_fp);
  return 0;
}
