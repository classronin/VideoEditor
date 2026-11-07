#pragma once
// Blender ui::Block 的等价层：即时模式登记控件、按 wcol_* 主题绘制、复现数值按钮的拖动映射，
// 并提供自绘的弹出菜单（popover）。
//
// 度量与配色的出处：
//   release/datafiles/userdef/userdef_default_theme.c     -> theme.h 的 wcol_*
//   editors/interface/interface_widgets.cc                -> 控件外形（圆角、滑块、文本对齐）
//   editors/interface/interface_handlers.cc:6126-6240     -> 数值按钮拖动映射
//   editors/interface/interface_region_menu_popup.cc      -> 弹出菜单度量
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <string>
#include <vector>

#include "render2d.h"
#include "theme.h"

namespace ui {

// 控件类型，对应 ui::ButtonType。
enum Type : uint8_t {
  Push,      // 普通按钮
  Toggle,    // 开关（复选框 / 图标按钮）
  Num,       // 数值按钮（wcol_num / wcol_numslider）
  Text,      // 文本输入（wcol_text）
  Pulldown,  // 头部菜单标题（wcol_pulldown）
  Menu,      // 头部下拉（wcol_menu，有底色）
  Tab,       // 属性区左侧的竖排 tab
  Label,     // 只读文本
};

struct Item {
  Rect rect;
  int id = 0;
  Type type = Push;
  // 数值映射：Button::softmin/softmax、ButNum::precision、RNA 的 is_float。
  double softmin = 0.0;
  double softmax = 0.0;
  double value = 0.0;
  double step = 1.0;  // ButtonNumber::step_size = RNA ui_range 的 step（拖动灵敏度用它算）
  int precision = 0;
  bool is_float = false;
  bool slider = false;  // 画 wcol_numslider 的进度条
  bool active = false;  // 正在拖动/编辑：用 inner_sel
  bool hot = false;     // 光标在下（只有 pulldown/menu/tab/push 有悬停底色）
  int state = 0;        // Toggle 的开关值；Tab 的选中序号
  // 画出来的那串字在按钮里的左右边界：点进框里放光标、拖选数字要按它换算字符下标。
  float text_x = 0.0f, text_w = 0.0f;
  // 登记时落在 block 的裁剪框外 ⇒ 这一格看不见，也不收事件（见 Block::set_clip）。
  bool off_clip = false;
};

// 一帧的控件表。绘制顺序即层级：后登记的在上面（弹出菜单最后画）。
class Block {
 public:
  void clear() { items_.clear(); }
  Item *add(const Item &it) {
    items_.push_back(it);
    Item &n = items_.back();
    // 整块在裁剪框外 = 看不见 = 也不许收事件：面板滚出 region 的那几行如果还能点，
    // 伸到时间线那一块里的像素就会替面板答这一击。判据用「整块在框内」而不是「有重叠」，
    // 因为 Item::rect 同时是文字定位的基准（text_x/text_w 按它换算），不能偷偷改成裁剪后
    // 那一截；差几像素没露全的行露全了自然能点。
    if (has_clip_ && !(n.rect.x >= clip_.x && n.rect.y >= clip_.y &&
                       n.rect.x + n.rect.w <= clip_.x + clip_.w &&
                       n.rect.y + n.rect.h <= clip_.y + clip_.h))
      n.off_clip = true;
    return &n;
  }
  Item *find(int id);
  const Item *find(int id) const;
  // 命中测试：从后往前（同 Blender：上层 block 先收到事件）。
  Item *hit(float x, float y);
  const std::vector<Item> &items() const { return items_; }
  // 给这一段登记设裁剪框（属性面板滚出 region 时用），clear_clip 之后照旧全收。
  void set_clip(const Rect &r) {
    clip_ = r;
    has_clip_ = true;
  }
  void clear_clip() { has_clip_ = false; }

 private:
  std::vector<Item> items_;
  Rect clip_;
  bool has_clip_ = false;
};

// ---------------------------------------------------------------- 数值拖动
//
// Blender 的数值按钮改值 = 在按钮上横向拖动。逐段对应 interface_handlers.cc：
//   690-693    fac = shift ? 0.05 : 1.0（mouse_scale_warp_factor）
//   155        UI_DRAG_MAP_SOFT_RANGE_PIXEL_MAX = 1000
//   4700-4786  numedit_begin：以当前值为中心收窄拖动映射范围，再算 dragfstart
//   6126-6240  numedit_but_FIELD：deler / 非线性缩放 / dragf 累积 / 线性映射
//   5962-5980  numedit_apply_snap（Ctrl 吸附到 10 的整倍数）
struct NumDrag {
  int id = 0;
  double startvalue = 0.0;  // data->startvalue
  double value = 0.0;       // data->value
  float dragf = 0.0f, dragfstart = 0.0f;
  int dragstartx = 0, draglastx = 0;
  double map_min = 0.0, map_max = 0.0;  // data->drag_map_soft_min/max
  bool moved = false;       // 动过像素就不再当成“单击”
  bool dragging = false;
};

void num_drag_begin(Block &blk, NumDrag &d, int id, int mx);
// 返回 true 表示值变了，新值在 d.value。
bool num_drag_update(const Block &blk, NumDrag &d, int mx, bool shift);

// ---------------------------------------------------------------- 控件绘制
//
// 每个函数画完控件并登记进 block，返回其矩形。

// ---------------------------------------------------------------- 文本编辑态
//
// 数值/文本按钮进入输入态后的光标与选区（Blender TextField 的 cursor / select_x 一对）。
// caret == sel 表示没有选区。点进框里 = 光标落到最近的字符边界；按住拖 = 从按下那点拖出选区；
// 有选区时敲第一个字符先吃掉选区（这就是「原值全选、输入即替换」的同一套机制）。
struct TextSel {
  int caret = 0;
  int sel = 0;
};

// 指针 x 落在串里的哪个字符边界（0..len，取最近的一个）。
int text_index_at(Render2D &r, const std::wstring &s, float text_x, float x);

Rect but_num(Render2D &r, Block &blk, int id, const Rect &area, const wchar_t *label,
             const std::wstring &text, double softmin, double softmax, double step, double value,
             int precision, bool is_float, bool slider, bool active, int anim = 0,
             const TextSel *ts = nullptr);
Rect but_toggle(Render2D &r, Block &blk, int id, const Rect &area, const wchar_t *label, bool on,
                const wchar_t *text = nullptr);
Rect but_push(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &text,
              bool hot = false);
Rect but_pulldown(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &text,
                  bool hot);
Rect but_text(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &text,
              bool active, const TextSel *ts = nullptr);
Rect but_tab(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &glyph,
             bool selected, bool hot);
void draw_label(Render2D &r, const Rect &area, const std::wstring &text, Color col);

// 面板：外框 + 标题栏（panel.back / panel_outline，圆角读 btheme::panel_roundness）。
void panel_box(Render2D &r, const Rect &area, bool open);
void panel_header(Render2D &r, const Rect &area, const std::wstring &title, bool open, bool hot);

// ---------------------------------------------------------------- 弹出菜单
//
// 照抄 Blender：面板 = wcol_menu_back（外描边 0x242424、内底 0x181818、圆角 0.2*unit），
// 条目高 = UI_UNIT_Y、文本左对齐、快捷键右对齐并暗 50%（draw_menu_item 用 BUT_INACTIVE 画
// hint），分隔线 = 1px @ alpha 30、占位 int(7*UI_SCALE_FAC)，条目内边距 0.25*行高。
struct Entry {
  std::wstring name;
  std::wstring shortcut;  // "Shift Ctrl K"（修饰键顺序见 wm_keymap.cc:1248）
  int id = 0;             // 命令 id，0 = 只读标题
  int submenu = -1;       // MenuTable 的下标
  bool sep = false;
  bool enabled = true;
};

using MenuTable = std::vector<std::vector<Entry>>;

struct Level {
  int menu = -1;
  Rect rect;                 // 面板矩形（客户区像素）
  std::vector<Rect> rows;    // 与 entries 一一对应（分隔线也占一行，高度不同）
  int hover = -1;
};

class Popover {
 public:
  void set_table(const MenuTable *t) { table_ = t; }
  // 在 (x,y)（通常是按钮左上角下方）打开菜单；bounds = 客户区，用于翻转/夹紧。
  void open(Render2D &r, int menu, float x, float y, const Rect &bounds);
  void close();
  bool is_open() const { return !stack_.empty(); }
  int depth() const { return int(stack_.size()); }

  bool on_move(float x, float y);
  // 单击：>0 = 命中的命令 id；-1 = 关闭了菜单；0 = 未处理。
  int on_click(float x, float y);
  // 键盘：Esc 收起一层，Enter 执行悬停项（返回命令 id 或 -1），上下移动悬停。
  int on_key(UINT vk);
  void draw(Render2D &r);

 private:
  const MenuTable *table_ = nullptr;
  std::vector<Level> stack_;
  Rect bounds_;
  Render2D *r2d_ = nullptr;

  void layout_level(Level &lv, float x, float y) const;
  // 指针所在的那一层（子面板盖在父面板上面，所以从最上层往下找）；-1 = 所有面板之外。
  int level_at(float x, float y) const;
  // 按当前悬停项展开/收起下层。highlight_first = 键盘导航打开时高亮下层第一项
  // （interface_handlers.cc:9757-9773 的 BUTTON_ACTIVATE_OPEN）；鼠标悬停只展开
  // （:10579-10583 的 BUTTON_STATE_MENU_OPEN）。
  bool expand_hovered(bool highlight_first);
  int measure(const std::wstring &s) const {
    return r2d_ ? int(r2d_->text_width(s, btheme::font_px)) : int(s.size()) * 8;
  }
};

}  // namespace ui
