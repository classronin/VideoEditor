// Blender ui::Block 层的实现。出处标注在各自段落，全部数值取自参考树
// E:\GitHub\Blender-VSE\blender（Blender 5.4）。
#include "ui.h"

#include <algorithm>
#include <cmath>

namespace ui {

Item *Block::find(int id) {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    if (it->id == id) return &*it;
  return nullptr;
}

const Item *Block::find(int id) const {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    if (it->id == id) return &*it;
  return nullptr;
}

Item *Block::hit(float x, float y) {
  for (auto it = items_.rbegin(); it != items_.rend(); ++it)
    if (it->type != Label && !it->off_clip && it->rect.contains(x, y)) return &*it;
  return nullptr;
}

// ---------------------------------------------------------------- 数值拖动

void num_drag_begin(Block &blk, NumDrag &d, int id, int mx) {
  Item *it = blk.find(id);
  d = {};
  if (!it) return;
  const double softmin0 = it->softmin, softmax0 = it->softmax;
  double softmin = softmin0, softmax = softmax0;
  double softrange = softmax - softmin;

  d.id = id;
  d.dragging = true;
  d.startvalue = d.value = it->value;
  d.dragstartx = d.draglastx = mx;

  // interface_handlers.cc:4702-4748：拖动映射的范围以当前值为中心收窄到
  // UI_DRAG_MAP_SOFT_RANGE_PIXEL_MAX(=1000) * UI_SCALE_FAC，防止大范围属性一动到顶。
  const double map_max = 1000.0 * btheme::scale;
  // value_step = ButtonNumber::step_size × UI_PRECISION_FLOAT_SCALE(0.01)（浮点）
  // 或 int(step_size)（整数）—— step_size 就是 RNA ui_range 的 step。
  // 不能用显示精度凑：位置 X 的 step 是 100（rna_sequencer.cc:2293），
  // 按 precision=3 凑出来是 0.001，拖拽映射区间从 2000 缩到 20，
  // 于是「拖 80 像素只动 0.27 像素」= 用户看到的「位置无反应」。
  const double value_step =
      it->is_float ? it->step * 0.01 : double(std::llround(it->step > 0 ? it->step : 1.0));
  const double softrange_max =
      it->is_float ? std::min(softrange, 2.0 * std::min(value_step, 0.1) * (map_max / 0.1))
                   : std::min(softrange, 2.0 * map_max);
  if (softrange > softrange_max) {
    softmin = d.value - softrange_max / 2.0;
    softmax = d.value + softrange_max / 2.0;
    if (softmin < softmin0) {
      softmin = softmin0;
      softmax = softmin + softrange_max;
    } else if (softmax > softmax0) {
      softmax = softmax0;
      softmin = softmax - softrange_max;
    }
    if (softmin == softmax) softmin = std::nextafter(softmin, -1e300);
    softrange = softmax - softmin;
  }

  d.map_min = softmin;
  d.map_max = softmax;
  // :4753-4786（PROP_SCALE_LINEAR）：dragfstart = (value - min) / range
  d.dragfstart = d.dragf = softrange == 0.0 ? 0.0f : float((d.value - softmin) / softrange);
}

bool num_drag_update(const Block &blk, NumDrag &d, int mx, bool shift) {
  const Item *it = blk.find(d.id);
  if (!it || !d.dragging) return false;

  const double softrange = d.map_max - d.map_min;
  const bool is_float = it->is_float;
  const float fac = shift ? 0.05f : 1.0f;  // mouse_scale_warp_factor（:690）

  // 以下直到映射结束都是 interface_handlers.cc:6134-6240 的逐行翻译。
  double deler = 500.0;
  double non_linear_range_limit, non_linear_pixel_map;
  if (is_float) {
    non_linear_range_limit = 11.0;
    non_linear_pixel_map = 500.0;
  } else {
    non_linear_range_limit = 129.0;
    non_linear_pixel_map = 250.0;
    if (softrange > 600) deler = std::pow(softrange, 0.75);
    else if (softrange < 25) deler = 50.0;
    else if (softrange < 100) deler = 100.0;
  }
  deler /= fac;

  double non_linear_scale = softrange > non_linear_range_limit
                                ? double(std::abs(mx - d.dragstartx)) / non_linear_pixel_map
                                : 1.0;
  if (!is_float) non_linear_scale = std::max(non_linear_scale, 0.5 * btheme::scale);

  d.dragf += float((mx - d.draglastx) / deler * non_linear_scale);
  // 触到真正的软边界时钉住 dragf，回拖不会“空转”（:6180-6185）。
  if (d.map_min == it->softmin) d.dragf = std::max(d.dragf, 0.0f);
  if (d.map_max == it->softmax) d.dragf = std::min(d.dragf, 1.0f);
  d.draglastx = mx;

  double tempf = d.map_min + double(d.dragf) * softrange;  // PROP_SCALE_LINEAR
  bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  double value;
  if (!is_float) {
    int t = int(std::lround(tempf));
    // numedit_apply_snap(SNAP_ON)（:5968-5978）：Ctrl 吸附到 10 的整倍数，边界值不动。
    if (ctrl && t != int(it->softmin) && t != int(it->softmax)) t = 10 * (t / 10);
    value = std::clamp(double(t), it->softmin, it->softmax);
  } else {
    value = std::clamp(tempf, it->softmin, it->softmax);
  }
  if (!is_float && value == std::floor(d.value)) return false;
  if (is_float && value == d.value) return false;
  d.value = value;
  d.moved = true;
  return true;
}

// ---------------------------------------------------------------- 控件绘制

// 通用外观：底 wcol.inner（活动态 inner_sel）+ 1px wcol.outline 圆角描边。
// blend 非空时把状态色按 wcol_state.blend（0.5）混进底色，就是 Blender 给动画属性
// 上绿底/黄底那一步（interface_widgets.cc:3344-3364）。
static void emboss(Render2D &r, const Rect &area, const btheme::Wcol &w, bool active,
                   const Color *blend = nullptr) {
  const float rad = btheme::radius(w.roundness);
  Color inner = active ? w.inner_sel : w.inner;
  if (blend) inner = btheme::mix(inner, *blend, btheme::state_blend);
  r.rrect(area, rad, inner);
  r.rring(area, rad, btheme::pixelsize, w.outline);
}

static float text_y(const Rect &area) { return area.y + (area.h - btheme::font_px) * 0.5f; }

// 编辑态的选区底色 + 光标。两者都按「画出来的那串字」的左边界换算，所以 but_num 的
// 居中排版和 but_text 的左对齐排版共用这一段。
static void draw_caret_sel(Render2D &r, const Rect &area, const std::wstring &text, float tx,
                           const TextSel &ts) {
  const float pad = 0.25f * area.h;
  const int n = int(text.size());
  auto edge = [&](int i) {
    return tx + r.text_width(text.substr(0, std::clamp(i, 0, n)), btheme::font_px);
  };
  const Color cur = btheme::widget_text_cursor;
  const int lo = std::min(ts.caret, ts.sel), hi = std::max(ts.caret, ts.sel);
  if (hi > lo)
    r.rect({edge(lo), area.y + pad, edge(hi) - edge(lo), area.h - pad * 2.0f},
           Color{cur.r, cur.g, cur.b, 0x55});
  r.rect({edge(ts.caret), area.y + pad, btheme::pixelsize, area.h - pad * 2.0f}, cur);
}

// 指针 x 落在哪个字符边界上：取距离最近的那一个（0 = 串首，len = 串尾）。
int text_index_at(Render2D &r, const std::wstring &s, float text_x, float x) {
  const int n = int(s.size());
  int best = 0;
  float best_d = 1e30f;
  for (int i = 0; i <= n; ++i) {
    const float bx = text_x + r.text_width(s.substr(0, i), btheme::font_px);
    const float d = std::fabs(bx - x);
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

Rect but_num(Render2D &r, Block &blk, int id, const Rect &area, const wchar_t *label,
             const std::wstring &text, double softmin, double softmax, double step, double value,
             int precision, bool is_float, bool slider, bool active, int anim,
             const TextSel *ts) {
  const btheme::Wcol &w = slider ? btheme::wcol_numslider : btheme::wcol_num;
  // anim：0=没动画，1=挂了曲线（绿底），2=当前帧上有键（黄底）。
  const Color state = anim >= 2 ? btheme::state_key : (anim == 1 ? btheme::state_anim : Color{});
  emboss(r, area, w, active, anim ? &state : nullptr);
  if (slider && softmax > softmin) {
    // 进度条 = wcol.item(0x4772b3)，宽度按值在区间内的占比。
    double f = std::clamp((value - softmin) / (softmax - softmin), 0.0, 1.0);
    Rect bar{area.x, area.y, area.w * float(f), area.h};
    if (bar.w > 1.0f) r.rrect(bar, btheme::radius(w.roundness), w.item);
  }
  const Color tc = active ? w.text_sel : w.text;
  float pad = 0.25f * area.h;  // 条目内文字边距（draw_menu_item 同款规则）
  float x = area.x + pad, wbox = area.w - pad * 2.0f;
  if (label && *label) {
    float lw = r.text_width(label, btheme::font_px);
    r.text(x, text_y(area), label, btheme::font_px, w.text);
    x += lw + pad;
    wbox -= lw + pad;
  }
  // 数值在剩余区域居中；单位后缀（°、%、fps）由调用方拼在字符串里。
  float tw = r.text_width(text, btheme::font_px);
  const float tx = x + std::max(0.0f, (wbox - tw) * 0.5f);
  if (ts) draw_caret_sel(r, area, text, tx, *ts);
  r.text(tx, text_y(area), text, btheme::font_px, tc);

  Item it;
  it.rect = area;
  it.id = id;
  it.type = Num;
  it.text_x = tx;
  it.text_w = tw;
  it.softmin = softmin;
  it.softmax = softmax;
  it.step = step;
  it.value = value;
  it.precision = precision;
  it.is_float = is_float;
  it.slider = slider;
  it.active = active;
  blk.add(it);
  return area;
}

Rect but_push(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &text,
              bool hot) {
  const btheme::Wcol &w = btheme::wcol_regular;
  emboss(r, area, w, false);
  if (hot) r.rrect(area, btheme::radius(w.roundness), btheme::wcol_tool.inner_sel);
  float tw = r.text_width(text, btheme::font_px);
  r.text(area.x + (area.w - tw) * 0.5f, text_y(area), text, btheme::font_px,
         hot ? w.text_sel : w.text);
  Item it;
  it.rect = area;
  it.id = id;
  it.type = Push;
  it.hot = hot;
  blk.add(it);
  return area;
}

Rect but_toggle(Render2D &r, Block &blk, int id, const Rect &area, const wchar_t *label, bool on,
                const wchar_t *text) {
  // 布尔开关：wcol_tool，激活态整块填充 inner_sel（蓝 0x4772b3）。
  const btheme::Wcol &w = btheme::wcol_tool;
  emboss(r, area, w, on);
  float pad = 0.25f * area.h;
  float x = area.x + pad;
  if (label && *label) {
    float lw = r.text_width(label, btheme::font_px);
    r.text(x, text_y(area), label, btheme::font_px, w.text);
    x += lw + pad;
  }
  const wchar_t *t = text && *text ? text : (on ? L"开" : L"关");
  // 没有左标签时文字居中（Blender 的按钮文字默认就是居中的：X / Y 这种开关只有文字）。
  float tx = (label && *label) ? x : area.x + (area.w - r.text_width(t, btheme::font_px)) * 0.5f;
  r.text(tx, text_y(area), t, btheme::font_px, on ? w.text_sel : w.text);
  Item it;
  it.rect = area;
  it.id = id;
  it.type = Toggle;
  it.state = on ? 1 : 0;
  blk.add(it);
  return area;
}

Rect but_pulldown(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &text,
                  bool hot) {
  // 头部菜单标题 = wcol_pulldown：常态完全透明，悬停 inner_sel（白色 10%）。
  const btheme::Wcol &w = btheme::wcol_pulldown;
  if (hot) r.rrect(area, btheme::radius(w.roundness), w.inner_sel);
  float pad = 0.5f * btheme::u(1.0f);
  float tw = r.text_width(text, btheme::font_px);
  r.text(area.x + (area.w - tw) * 0.5f, text_y(area), text, btheme::font_px,
         hot ? w.text_sel : w.text);
  Item it;
  it.rect = area;
  it.id = id;
  it.type = Pulldown;
  it.hot = hot;
  it.state = int(tw) + int(pad * 2.0f);
  blk.add(it);
  return area;
}

Rect but_text(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &text,
              bool active, const TextSel *ts) {
  // wcol_text：底 0x1d1d1d，编辑态 0x181818（userdef_default_theme.c:70-72）。
  // 文字左对齐；编辑态画选区与光标（widget_text_cursor 0x71a8ff）。
  const btheme::Wcol &w = btheme::wcol_text;
  emboss(r, area, w, active);
  const float pad = 0.25f * area.h;
  float x = area.x + pad;
  if (ts) draw_caret_sel(r, area, text, x, *ts);
  r.text(x, text_y(area), text, btheme::font_px, active ? w.text_sel : w.text);
  Item it;
  it.rect = area;
  it.id = id;
  it.type = Text;
  it.active = active;
  it.text_x = x;
  it.text_w = r.text_width(text, btheme::font_px);
  blk.add(it);
  return area;
}

Rect but_tab(Render2D &r, Block &blk, int id, const Rect &area, const std::wstring &glyph,
             bool selected, bool hot) {
  // 属性区竖排 tab：wcol_tab.inner = 0x1d1d1d；选中 = inner_sel(0x303030)，与面板相接。
  r.rect(area, selected ? btheme::wcol_tab.inner_sel
                        : (hot ? btheme::mix(btheme::wcol_tab.inner, btheme::wcol_tab.inner_sel,
                                             0.5f)
                               : btheme::wcol_tab.inner));
  float tw = r.text_width(glyph, btheme::font_px);
  r.text(area.x + (area.w - tw) * 0.5f, text_y(area), glyph, btheme::font_px,
         selected ? btheme::wcol_tab.text_sel : btheme::wcol_tab.text);
  Item it;
  it.rect = area;
  it.id = id;
  it.type = Tab;
  it.state = selected ? 1 : 0;
  blk.add(it);
  return area;
}

void draw_label(Render2D &r, const Rect &area, const std::wstring &text, Color col) {
  r.text(area.x, text_y(area), text, btheme::font_px, col);
}

void panel_box(Render2D &r, const Rect &area, bool open) {
  const float rad = btheme::radius(btheme::panel_roundness);
  if (open) {
    r.rrect(area, rad, btheme::panel_back);
    r.rring(area, rad, btheme::pixelsize, btheme::panel_outline);
  } else {
    r.rrect({area.x, area.y, area.w, btheme::u(1.25f)}, rad, btheme::panel_back);
  }
}

void panel_header(Render2D &r, const Rect &area, const std::wstring &title, bool open, bool hot) {
  // 标题高 = UI_UNIT_Y * 1.25；左侧三角（展开向下 / 折叠向右）+ 标题。
  const float rad = btheme::radius(btheme::panel_roundness);
  if (hot) r.rrect({area.x, area.y, area.w, area.h}, rad, btheme::wcol_regular.inner);
  float cy = area.y + area.h * 0.5f;
  float ax = area.x + 0.25f * btheme::u(1.0f) + btheme::icon_size() * 0.5f;
  float s = 4.0f * btheme::scale;
  if (open)
    r.tri(ax - s, cy - s * 0.6f, ax + s, cy - s * 0.6f, ax, cy + s * 0.9f, btheme::panel_title);
  else
    r.tri(ax - s * 0.6f, cy - s, ax + s * 0.9f, cy, ax - s * 0.6f, cy + s, btheme::panel_title);
  r.text(ax + s + 0.25f * btheme::u(1.0f), cy - btheme::font_px * 0.5f, title, btheme::font_px,
         btheme::panel_title);
}

// ---------------------------------------------------------------- 弹出菜单

static const Color kSepCol{0xdd, 0xdd, 0xdd, 30};  // 1px @ alpha 30

static float row_height(const Entry &e) {
  // 条目高 = UI_UNIT_Y；分隔线占 int(7*UI_SCALE_FAC)（interface_layout.cc:3605）。
  return e.sep ? std::max(2.0f, std::floor(7.0f * btheme::scale)) : btheme::u(1.0f);
}

void Popover::close() { stack_.clear(); }

void Popover::open(Render2D &r, int menu, float x, float y, const Rect &bounds) {
  r2d_ = &r;
  bounds_ = bounds;
  if (!table_ || menu < 0 || menu >= int(table_->size())) return;
  Level lv;
  lv.menu = menu;
  stack_.clear();
  stack_.push_back(lv);
  layout_level(stack_.back(), x, y);
}

// 面板宽 = max(UI_MENU_WIDTH_MIN = 9*unit, 最宽条目)；高 = 各条目之和 + 2*UI_MENU_PADDING。
// 越界时把面板拉回客户区（Blender 的 popup 会夹紧到窗口）。
void Popover::layout_level(Level &lv, float x, float y) const {
  const auto &entries = (*table_)[lv.menu];
  const float pad = float(int(0.2f * btheme::u(1.0f)));   // UI_MENU_PADDING
  const float ipad = 0.25f * btheme::u(1.0f);             // 条目内边距
  float widest = 0.0f, h = pad;
  for (const Entry &e : entries) {
    float w = ipad * 2.0f;
    if (!e.sep) {
      w += float(measure(e.name));
      if (!e.shortcut.empty()) w += float(measure(e.shortcut)) + btheme::icon_size();
      if (e.submenu >= 0) w += 6.0f * btheme::scale + 10.0f * btheme::scale;  // 箭头 + 余量
    }
    widest = std::max(widest, w);
    h += row_height(e);
  }
  float w = std::max(btheme::u(9.0f), widest);
  float rx = x, ry = y;
  if (rx + w > bounds_.x + bounds_.w) rx = std::max(bounds_.x, bounds_.x + bounds_.w - w - 1.0f);
  if (ry + h > bounds_.y + bounds_.h) ry = std::max(bounds_.y, bounds_.y + bounds_.h - h - 1.0f);
  lv.rect = {rx, ry, w, h};
  lv.rows.clear();
  float cy = ry + pad;
  for (const Entry &e : entries) {
    lv.rows.push_back({rx + pad * 0.5f, cy, w - pad, row_height(e)});
    cy += row_height(e);
  }
}

bool Popover::expand_hovered(bool highlight_first) {
  if (stack_.empty() || !table_) return false;
  bool changed = false;
  // 收起失效的下层：父项不再指向当前这层就弹掉。
  while (stack_.size() > 1) {
    const Level &prev = stack_[stack_.size() - 2];
    const auto &pe = (*table_)[prev.menu];
    bool keep = prev.hover >= 0 && pe[size_t(prev.hover)].submenu == stack_.back().menu;
    if (keep) break;
    stack_.pop_back();
    changed = true;
  }
  Level &top = stack_.back();
  const auto &entries = (*table_)[top.menu];
  if (top.hover >= 0 && entries[top.hover].submenu >= 0) {
    int sub = entries[top.hover].submenu;
    bool already = false;
    for (size_t i = stack_.size(); i-- > 1;)
      if (stack_[i].menu == sub) already = true;
    if (!already) {
      Level lv;
      lv.menu = sub;
      Rect anchor = top.rows[size_t(top.hover)];
      stack_.push_back(lv);
      // 子面板与父条目顶部对齐、向左压 2px（Blender 的 submenu 贴合父面板右缘）。
      Level &nl = stack_.back();
      layout_level(nl, anchor.x + anchor.w - 2.0f, anchor.y - 2.0f);
      if (highlight_first) {
        const auto &ne = (*table_)[nl.menu];
        for (size_t i = 0; i < ne.size(); ++i)
          if (!ne[i].sep && ne[i].enabled) {
            nl.hover = int(i);
            break;
          }
      }
      changed = true;
    }
  }
  return changed;
}

// 指针落在哪一层：后打开的子面板画在上面，所以从最上层往下找。
int Popover::level_at(float x, float y) const {
  for (size_t i = stack_.size(); i-- > 0;)
    if (stack_[i].rect.contains(x, y)) return int(i);
  return -1;
}

bool Popover::on_move(float x, float y) {
  if (stack_.empty() || !table_) return false;
  bool changed = false;
  // 只在「指针所在的那一层」改悬停态，然后砍掉它下面的所有子面板。
  // 早先只认最上层：子面板一开，父层里其它带子菜单的行再也收不到 hover，
  // 于是父层那一行的兄弟项点不动（用户报的那条）。
  const int hit = level_at(x, y);
  if (hit < 0) {  // 移出所有面板：回到只剩根层、无高亮（点外面才关，见 on_click）
    while (stack_.size() > 1) stack_.pop_back();
    changed = stack_.back().hover != -1;
    stack_.back().hover = -1;
    return changed;
  }
  while (int(stack_.size()) > hit + 1) stack_.pop_back();
  Level &top = stack_.back();
  const auto &entries = (*table_)[top.menu];
  int hover = -1;
  for (size_t i = 0; i < top.rows.size(); ++i)
    if (top.rows[i].contains(x, y) && !entries[i].sep && entries[i].enabled) hover = int(i);
  if (hover != top.hover) {
    top.hover = hover;
    changed = true;
  }
  // 悬停即展开子菜单（不需要点击）。
  return expand_hovered(false) || changed;
}

int Popover::on_click(float x, float y) {
  if (stack_.empty() || !table_) return 0;
  const int hit = level_at(x, y);
  if (hit < 0) {
    close();
    return -1;
  }
  while (int(stack_.size()) > hit + 1) stack_.pop_back();
  Level &top = stack_.back();
  const auto &entries = (*table_)[top.menu];
  for (size_t i = 0; i < top.rows.size(); ++i) {
    if (!top.rows[i].contains(x, y)) continue;
    const Entry &e = entries[i];
    // 点在分隔线 / 禁用项 / 带子菜单的行上 = 什么都不做，菜单留着。
    if (e.sep || !e.enabled || e.submenu >= 0) return 0;
    int id = e.id;
    close();
    return id ? id : -1;
  }
  return 0;  // 落在面板内的空白上：不关（关要靠点面板外）
}

int Popover::on_key(UINT vk) {
  if (stack_.empty() || !table_) return 0;
  if (vk == VK_ESCAPE) {
    if (stack_.size() > 1) stack_.pop_back();
    else close();
    return -1;
  }
  Level &top = stack_.back();
  const auto &entries = (*table_)[top.menu];
  if (vk == VK_RETURN) {
    int id = top.hover >= 0 ? entries[size_t(top.hover)].id : 0;
    close();
    return id ? id : -1;
  }
  if (vk == VK_UP || vk == VK_DOWN) {
    int dir = vk == VK_UP ? -1 : 1;
    int n = int(entries.size());
    int i = top.hover < 0 ? (dir > 0 ? -1 : 0) : top.hover;
    for (int k = 0; k < n; ++k) {
      i = (i + dir + n) % n;
      if (!entries[size_t(i)].sep && entries[size_t(i)].enabled) break;
    }
    top.hover = i;
    // 方向键移到子菜单行 = 打开它并高亮其第一项（BUTTON_ACTIVATE_OPEN）。
    expand_hovered(true);
    return -2;  // 已处理，无命令
  }
  return 0;
}

void Popover::draw(Render2D &r) {
  r2d_ = &r;
  for (size_t li = 0; li < stack_.size(); ++li) {
    Level &lv = stack_[li];
    layout_level(lv, lv.rect.x, lv.rect.y);
    const auto &entries = (*table_)[lv.menu];
    const btheme::Wcol &wb = btheme::wcol_menu_back;
    const float rad = btheme::radius(wb.roundness);
    r.rrect(lv.rect, rad, wb.inner);
    r.rring(lv.rect, rad, btheme::pixelsize, wb.outline);
    const float pad = 0.25f * btheme::u(1.0f);
    for (size_t i = 0; i < lv.rows.size(); ++i) {
      const Entry &e = entries[i];
      const Rect &row = lv.rows[i];
      if (e.sep) {
        float cy = row.y + row.h * 0.5f;
        r.rect({row.x + pad, cy, row.w - pad * 2.0f, btheme::pixelsize}, kSepCol);
        continue;
      }
      const bool hov = int(i) == lv.hover;
      Color tc = btheme::wcol_menu_item.text;
      if (hov) {
        // 悬停底 = wcol_menu_item.inner_sel（蓝），左右各超出 0.125*unit（:5048）。
        Rect hr{row.x - 0.125f * btheme::u(1.0f), row.y, row.w + 0.25f * btheme::u(1.0f), row.h};
        r.rrect(hr, btheme::radius(btheme::wcol_menu_item.roundness), btheme::wcol_menu_item.inner_sel);
        tc = btheme::wcol_menu_item.text_sel;
      }
      float ty = row.y + (row.h - btheme::font_px) * 0.5f;
      float tx = row.x + pad;
      if (!e.enabled)
        tc = btheme::mix(btheme::wcol_menu_item.text, btheme::wcol_menu_back.inner, 0.5f);
      r.text(tx, ty, e.name, btheme::font_px, tc);
      if (!e.shortcut.empty()) {
        // 快捷键右对齐、暗 50%（BUT_INACTIVE 状态）。
        float sw = float(measure(e.shortcut));
        r.text(row.x + row.w - pad - sw, ty, e.shortcut, btheme::font_px,
               btheme::mix(tc, btheme::wcol_menu_back.inner, 0.5f));
      }
      if (e.submenu >= 0) {
        float cy = row.y + row.h * 0.5f;
        float s = 3.0f * btheme::scale;
        r.tri(row.x + row.w - pad - s, cy - s * 1.4f, row.x + row.w - pad - s, cy + s * 1.4f,
              row.x + row.w - pad + s * 0.6f, cy, tc);
      }
    }
  }
}

}  // namespace ui
