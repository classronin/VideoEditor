// 手势层：一次「按下 → 拖动 → 松开」的全部分支。
//
// 分工（逻辑.md）：触发的那一头绝对中立 —— main.cpp 的窗口过程只把事件交给槽里的那个对象，
// 不判断「这一点落在哪一块地方」；答应不答应、答应了之后每一格怎么算，全写在被点中的对象
// 自己交出的手势里。执行的那一头绝对自治 —— 播放头写 project.playhead（工程数据），
// 平移缩放写 TimelineView / 预览 view2d（视图数据），取景框写那一条 Clip 自己的属性，
// 分界线写排版（窗口）。
#include "gesture.h"

#include <cmath>
#include <cstdio>

#include "retiming.h"

namespace app {

// ---------------------------------------------------------------- 基类与槽

void Gesture::move(App &a, int x, int y, bool down) {
  // 一个键都没按着 = 这一次按住已经结束了。松开那格本来会收尾，但鼠标跑出去时 Windows
  // 也可能只发 MOVE 不发 UP，所以这里补一次丢弃（不跑收尾，数据不该被一次丢失的 UP 改写）。
  if (!down) {
    a.gesture.reset();
    return;
  }
  if (filter_event(a, x, y)) return;  // 闭环光标把这一格事件搬走了，不发下去
  const float fx = float(x), fy = float(y);
  if (!moved && !always_follow()) {
    // PRESS → CLICK / CLICK_DRAG 的分级（wm_event_system.cc:3730-3860）：
    // 从按下那一点累计的 |dx|+|dy| 越过阈值才认拖拽，没越过还只是「这一次点击已经选好了」。
    if (std::fabs(fx - grab_x) + std::fabs(fy - grab_y) <= drag_threshold()) return;
  }
  moved = true;
  drag(a, fx, fy);
}

void release_gesture(App &a) {
  // 先把槽腾出来再跑收尾：收尾里可能要 push 撤消步，那时这一次按下已经不属于任何人了。
  std::shared_ptr<Gesture> g = a.gesture;
  a.gesture.reset();
  if (g) g->up(a);
}

// 松开时把视图的那几个数写进追踪日志：拖动本身一路只改属性，收尾读一次才看得出
// 「这一推到底推进去了没有」。中键的三支（平移/缩放）和滚动条共用同一组读数。
static void grab_report(App &a) {
  if (!trace_fp) return;
  wchar_t t[128];
  swprintf(t, 128, L"mid-up scroll %.2f ch %.2f pps %g zoom %g", a.tl.scroll_t, a.tl.view_ch,
           a.tl.pps, a.view_zoom);
  trace(t);
}

// 按下答应的对象统一在这里补上「按下那一点」，然后交给槽。
static std::shared_ptr<Gesture> claim(Gesture *raw, float fx, float fy) {
  if (!raw) return nullptr;
  raw->grab_x = fx;
  raw->grab_y = fy;
  return std::shared_ptr<Gesture>(raw);
}

// ---------------------------------------------------------------- 分界线

static Gesture *divider_press(App &a, float fx, float fy) {
  // 预览与时间线之间那条线：命中带只贴线本身几个像素，控件（含 [+/-]）在外壳那一层
  // 已经先答过了，不会被抢走。命中判据与悬停点亮共用 split_divider_at 那一条算术。
  if (split_divider_at(a, fx, fy)) return new DividerH();
  if (props_divider_at(a, fx, fy)) return new DividerV();
  return nullptr;
}

void DividerH::drag(App &a, float fx, float fy) {
  // 跟着鼠标改两条 area 的高度（分界线上下拖）。
  (void)fx;
  const float body_top = a.viewport.y;
  const float body_bot = a.tl_area.y + a.tl_area.h;
  const float avail = body_bot - body_top;
  if (avail <= 1.0f) return;
  // 底部留白（原标记带的位置）永远占着，所以最小高度也要算上它。
  const float pad_bot = std::lround(0.6f * btheme::u(1.0f));
  const float min_tl = channel_h(a) + (a.scrub.h + pad_bot + btheme::pixelsize);
  const float min_vp = 80.0f * btheme::scale;
  const float px = std::clamp(body_bot - fy, min_tl, std::max(min_tl, avail - min_vp));
  if (px != a.tl_px) {
    a.tl_px = px;
    layout(a);
    a.dirty = kRegionAll;
  }
}

void DividerH::up(App &a) {
  if (!trace_fp) return;
  wchar_t t[48];
  swprintf(t, 48, L"split-drag tl=%.0f", double(a.tl_px));
  trace(t);
}

void DividerV::drag(App &a, float fx, float fy) {
  // 跟着鼠标改属性面板的宽度（竖分界线左右拖）。
  (void)fy;
  const float win_w = float(a.r2d.width());
  // 宽度 = 指针到「面板所在那一侧的窗口边」的距离：面板放左时就是 fx，放右时是
  // win_w - fx（同一条 region_scale，只是边在另一端）。
  const float px = std::clamp(a.props_side ? win_w - fx : fx, 0.0f, props_w_max(win_w));
  // 拖到比 collapse_size 还窄 ⇒ 整条面板收起（region_scale_toggle_hidden），
  // 宽度回默认，下次按 N 开回来还是原来那块面板，不是一条缝。
  if (px < props_w_min()) {
    a.sidebar_on = false;
    a.props_px = 0.0f;
  } else if (px != a.props_px) {
    a.sidebar_on = true;
    a.props_px = px;
  } else {
    return;  // 还在原来的宽度上：不重排也不重绘
  }
  layout(a);
}

void DividerV::up(App &a) {
  // 只改视图尺寸，不进撤消栈。松开时留一行，宽度落在哪就从像素上猜不准。
  if (!trace_fp) return;
  wchar_t t[64];
  swprintf(t, 64, L"props-drag w=%.0f on=%d", double(a.props_px), int(a.sidebar_on));
  trace(t);
}

// ---------------------------------------------------------------- 滚动条

static Gesture *scrollbar_press(App &a, float fx, float fy) {
  const int zone = scroll_zone_at(a, fx, fy);
  if (zone == kSQNone) return nullptr;
  // scroller_activate_init（view2d_ops.cc:1952-1968）：fac = 「tot ∪ cur」的 y 尺寸 / 轨道
  // 像素高。这里取「tot ∪ cur」而不是 tot 本身：视图跑到 tot 外面时，用 tot 会让滑块几乎不动。
  // fac_round = cur 的 y 尺寸 / (区高 + 1)，用来把位移取整到一格像素。
  const Rect t = scroll_trough(a);
  const double cur = cur_channels(a);
  const double uni = std::max(std::max(timeline_tot_hi(a), a.tl.view_ch), a.tl.view_ch + cur);
  ScrollBar *g = new ScrollBar();
  g->zone = zone;
  g->fac = t.h > 0.0f ? uni / double(t.h) : 0.0;
  g->fac_round = cur / (double(a.lanes.h) + 1.0);
  g->last_y = fy;  // vsm->lasty
  a.dirty |= kRChan | kRLanes | kRScrub;
  return g;
}

void ScrollBar::step(App &a, double delta_up) const {
  // scroller_activate_apply：temp = round(fac*delta / fac_round)*fac_round，然后
  // BAR / 两端空白 → cur.ymin、cur.ymax 一起走（= 只平移）；
  // MIN 把手 → 只有 cur.ymin 走（上沿钉住，行数变多/少 = 纵向缩放）；
  // MAX 把手 → 只有 cur.ymax 走（下沿钉住）。
  // 时间线没有 V2D_LOCKZOOM_Y（space_sequencer.cc:192 的 keepzoom = V2D_KEEPZOOM），
  // 所以两个把手都能真的改变行高。
  if (fac_round <= 0.0 || fac <= 0.0) return;
  const double temp = std::round(delta_up * fac / fac_round) * fac_round;
  if (temp == 0.0) return;
  const double cur = cur_channels(a);
  if (zone == kSQMin) {
    const double ymax = a.tl.view_ch + cur;
    a.tl.view_ch += temp;
    a.tl.ch_px = float(a.lanes.h / std::max(1.0, ymax - a.tl.view_ch));
  } else if (zone == kSQMax) {
    a.tl.ch_px = float(a.lanes.h / std::max(1.0, cur + temp));
  } else {
    a.tl.view_ch += temp;
  }
  clamp_view_ch(a);
  a.dirty |= kRChan | kRLanes | kRScrub;
}

void ScrollBar::drag(App &a, float fx, float fy) {
  // 逐次事件的位移（模态算子没有阈值那一步，每一格直接换算）。
  (void)fx;
  const float dy = fy - float(last_y);
  if (dy == 0.0f) return;
  last_y = fy;
  step(a, -double(dy));  // dy 朝下 → Blender 的 y 朝上取反
}

void ScrollBar::up(App &a) {
  // 按在滑块外的轨道上、一点没挪 = 翻 0.8 页（view2d 的 page step）。
  if (!moved && (zone == kSQMinOut || zone == kSQMaxOut)) {
    const double page = 0.8 * double(scroll_thumb(a).h);
    step(a, zone == kSQMaxOut ? page : -page);
  }
  grab_report(a);
}

// ---------------------------------------------------------------- 属性面板的滚动条

// 只答滑块这一格。轨道空白不翻页：翻页量要按「还能滚多少」算，而那个数是内容高减面板高，
// 属于绘制那一趟量出来的账；滑块自己不需要 —— 拖到哪就滚到哪，界由 draw_props 收。
static Gesture *props_scrollbar_press(App &a, float fx, float fy) {
  if (!props_scroll_thumb(a).contains(fx, fy)) return nullptr;
  PropsScroll *g = new PropsScroll();
  g->last_y = fy;  // vsm->lasty
  return g;
}

void PropsScroll::drag(App &a, float fx, float fy) {
  (void)fx;
  const float dy = fy - last_y;
  if (dy == 0.0f) return;
  last_y = fy;
  // 屏幕 y 朝下：滑块往下 = 内容往上走 = 看下面被截断的那一截。
  a.props_scroll += dy;
  a.dirty |= kRProps;
}

// ---------------------------------------------------------------- 播放头

void PlayheadDrag::drag(App &a, float fx, float fy) {
  (void)fy;
  // 拖动播放头：默认只做整数帧对齐、不吸附；按住 Ctrl 才启用磁性吸附（需求）。
  // 写的是工程数据 project.playhead —— 标尺带只是搬它的那个对象。
  double t = x_time(a, fx);
  if (ctrl_held()) t = snap_time(a, fx, t, nullptr, kSnapFrame);
  a.project.playhead = std::max(0.0, a.frame_to_t(a.t_to_frame(t)));
  a.dirty |= kRScrub | kRLanes | kRViewport;
  sync_preview(a);
}

static Gesture *playhead_press(App &a, float fx, float fy) {
  // 标尺/scrubbing 带整条都能拖播放头（km_time_scrub:1237 = anim.change_frame）。
  if (!a.scrub.contains(fx, fy)) return nullptr;
  PlayheadDrag *g = new PlayheadDrag();
  g->drag(a, fx, fy);  // 按下先跳一次：和拖动的每一格同一个算法，不另写一份
  if (a.playing) toggle_play(a);  // 拖播放头 = 停下来看这一帧
  return g;
}

// ---------------------------------------------------------------- 条带

namespace {

// 条带身上的哪一段：把手宽与 draw_lanes 用同一个 strip_geom，保证「看到的 = 点到的」。
enum StripPart { kPartNone = 0, kPartBody, kPartLeft, kPartRight };

StripPart strip_part(App &a, float fx, float fy, Clip **out) {
  for (size_t i = a.project.clips.size(); i-- > 0;) {
    Clip &c = a.project.clips[i];
    StripGeom g = strip_geom(a, c);
    if (!g.visible || !g.body.contains(fx, fy)) continue;
    *out = &c;
    if (g.handle > 0.0f) {
      if (fx - g.body.x < g.handle) return kPartLeft;
      if (g.body.x + g.body.w - fx < g.handle) return kPartRight;
    }
    return kPartBody;
  }
  *out = nullptr;
  return kPartNone;
}

// 同轨道相邻条带给出的可放区间（seq_slide 不允许条带互相重叠）。
std::pair<double, double> free_span(const App &a, const Clip &c, int track) {
  // 左端不设 0 下限：帧轴是有符号的（sfra/efra 可为负），条带能拖到 0 帧以左，往回裁同理。
  double lo = -1e18, hi = 1e18;
  for (const Clip &o : a.project.clips) {
    if (o.id == c.id || o.track != track) continue;
    if (o.timeline_start + o.duration <= c.timeline_start)
      lo = std::max(lo, o.timeline_start + o.duration);
    if (o.timeline_start >= c.timeline_start + c.duration) hi = std::min(hi, o.timeline_start);
  }
  return {lo, hi};
}

}  // namespace

// 光标悬停这一格：指针落在任一条带的左/右把手上就该换左右箭头（WM_CURSOR_X_MOVE）。
// 命中判定复用按下同一处的 strip_part，绝不在按键/光标那一层另起一套边界。
bool strip_handle_hot(App &a, float fx, float fy) {
  Clip *c = nullptr;
  const StripPart p = strip_part(a, fx, fy, &c);
  return p == kPartLeft || p == kPartRight;
}

// 拖一条边：把这条带的这个边推到光标这一刻，素材长度与邻居让位这些限位各自算。
// 左还是右是调用方（TrimDrag）带进来的参数，不是从外壳的某个状态里猜的。
static void trim_edge(App &a, Clip &c, bool left, double t) {
  const double fd = a.frame_dur();
  // Blender 的把手拖动先由像素算出「帧号」（SEQ_frame_from_timeline_x 是整数），
  // 再换算回时刻，所以裁切永远落在整帧上；这里同构。
  t = a.frame_to_t(a.t_to_frame(t));
  // 把手拖的是条带的一条边：吸附对象就是这条边，按住 Ctrl 临时禁用磁性吸附（需求）。
  const bool snap_off = ctrl_held();
  if (left) {
    const auto [lo, hi] = free_span(a, c, c.track);
    (void)hi;
    // 左把手：右缘不动，曲线在把手处重切；不能把入点拖成负数，所以最左退到
    // 「曲线源值为 0 的那个位置」（空曲线 = 退 source_in 秒，按首段斜率换算）。
    const double lower = std::max(lo, c.timeline_start + retiming_x_at_src(c, 0.0));
    const double ns = std::clamp(snap_off ? t : snap_time(a, float(time_x(a, t)), t, &c), lower,
                                 c.timeline_start + c.duration - fd);
    const double delta = ns - c.timeline_start;
    c.timeline_start = ns;
    c.source_in = retiming_move_left_edge(c, delta);
    c.duration -= delta;
    // 裁掉的这一段里如果有键，数组就短了一截，后面几把的下标全部前移 ⇒ 清掉失效的选择。
    retiming_prune_sel(a);
  } else {
    const double max_dur = max_duration(c);
    const auto [lo, hi] = free_span(a, c, c.track);
    (void)lo;
    const double dur =
        std::clamp(snap_off ? t : snap_time(a, float(time_x(a, t)), t, &c) - c.timeline_start, fd,
                   std::min(max_dur, hi - c.timeline_start));
    c.duration = dur;
  }
}

void TrimDrag::drag(App &a, float fx, float fy) {
  (void)fy;
  // 跟着一起裁的就是「带这颗把手标志的全部条带」：点中的那条 + 它的连接成员
  // （标志在按下时打在它们身上，名单不在这里）。
  const int flag = left ? kHandleLeft : kHandleRight;
  const double t = x_time(a, fx);
  for (Clip &c : a.project.clips)
    if (c.handle_sel == flag) trim_edge(a, c, left, t);
  recompute_duration(a);
  a.dirty |= kRLanes | kRScrub | kRViewport | kRProps;
  sync_preview(a);
}

void TrimDrag::up(App &a) {
  // 没拖过阈值就松开 = 只是给这一组打上把手标志（选中），数据一个字节没动，不占撤消步。
  if (!moved) return;
  // 拖动过程中实时改数据，松开才算一步撤消（sequencer.trim）。
  undo_push(a, L"修剪");
}

static Gesture *handle_press(App &a, Clip &c, bool left) {
  // 按下把手 = sequencer.select_handle：给被点的条带打上这颗把手标志，再由
  // sequencer_select_connected_strips（sequencer_select.cc:1355-1371）把同一颗标志抄给
  // 连接成员；transform.seq_clip_handles 裁的就是带这个标志的全部条带。
  // 锁定的成员不打（create_trans_data_strip 那一路的 transform_is_locked 过滤）。
  const int flag = left ? kHandleLeft : kHandleRight;
  for (Clip &o : a.project.clips) o.handle_sel = kHandleNone;  // 上一击的标志不清就会连裁
  c.handle_sel = flag;
  for (Clip *o : connected_clips(a, c))
    if (!o->locked) o->handle_sel = flag;
  if (trace_fp) {
    wchar_t b[32];
    std::wstring s = L"trim-press group=";
    for (const Clip &o : a.project.clips)
      if (o.handle_sel == flag) {
        swprintf(b, 32, L" %lld", (long long)o.id);
        s += b;
      }
    trace(s);
  }
  TrimDrag *g = new TrimDrag();
  g->left = left;
  return g;  // 按下只打标志：真拖过阈值才裁（和条带本体的 CLICK_DRAG 分级同一套）
}

// ---------------------------------------------------------------- 框选

Rect SelectBox::box() const {
  const float l = std::min(start.x, end.x), t = std::min(start.y, end.y);
  const float r = std::max(start.x, end.x), b = std::max(start.y, end.y);
  return Rect(l, t, r - l, b - t);
}

void SelectBox::drag(App &a, float fx, float fy) {
  end = Rect(fx, fy, 0.0f, 0.0f);
  a.dirty |= kRLanes;
}

void SelectBox::up(App &a) {
  if (moved)
    select_box(a, box(), shift_held());  // Shift = 并入已有选择（select_box 的 extend）
  else
    clear_selection(a);  // 没拖动的点击落在空白 = sequencer.select(deselect_all)
  a.dirty |= kRLanes | kRProps;
}

// ---------------------------------------------------------------- 条带本体 / 变速键

void StripClick::up(App &a) {
  select_only(a, clip_id);
}

void StripSlideArm::drag(App &a, float fx, float fy) {
  // select_mouse = CLICK_DRAG：越过阈值才 invoke transform.seq_slide。
  // 交回这一格 —— 此后归模态（数据、吸附、Esc 还原都在 ModalState 自带的那份快照里），
  // 鼠标拖和 G 键因此走的是同一条路。调用方手里有抄本，所以对象活到这一格外。
  a.gesture.reset();
  modal_begin(a, ModalState::Slide);
  if (!modal_active(a)) return;
  a.modal.t0 = t0;    // 原点回到按下那一刻，而不是刚越过阈值的这一帧
  a.modal.y0 = grab_y;
  modal_update(a, x_time(a, fx));
}

void KeySlideArm::drag(App &a, float fx, float fy) {
  // 键同理：sequencer_retiming_key_select_exec（sequencer_retiming.cc:917-925）选中键之后
  // 直接 return OPERATOR_RUNNING_MODAL，真正干活的还是 transform.seq_slide —— 它看见
  // 「有键被选中」就把变换数据换成移动键（transform_convert.cc:958-965），所以分流在
  // modal_begin 里面，鼠标和 G 键共用。
  const int64_t kid = clip_id;
  const size_t kidx = key_idx;
  a.gesture.reset();
  modal_begin(a, ModalState::Slide);
  if (!modal_active(a)) return;
  a.modal.t0 = t0;
  if (Clip *gc = find_clip(a, kid); gc && !gc->locked && kidx < gc->retiming.size()) {
    a.modal.grab_id = kid;  // 吸附源 = 手指按住的那把键
    a.modal.grab_key = kidx;
  }
  modal_update(a, x_time(a, fx));
}

static Gesture *lane_press(App &a, float fx, float fy) {
  if (!a.lanes.contains(fx, fy)) return nullptr;
  // 变速键这一层排在条带选择之前（sequencer_select.cc:1053-1097）。框选是另一个算子
  // （select_box），Blender 里它不走这段前置，所以 box_armed 时不插手。
  if (!a.box_armed) {
    int64_t key_clip = -1;
    size_t key_idx = 0;
    if (retiming_key_pick(a, fx, fy, &key_clip, &key_idx)) {
      a.dirty = kRegionAll;
      if (key_clip < 0) return nullptr;  // Shift 那一支只做增删，不 invoke 拖动
      KeySlideArm *g = new KeySlideArm();
      g->clip_id = key_clip;
      g->key_idx = key_idx;
      g->t0 = x_time(a, fx);  // 抓取点：位移按「相对按下这一刻」算，不按播放头
      return g;
    }
  }
  Clip *c = nullptr;
  const StripPart part = strip_part(a, fx, fy, &c);
  if (part == kPartNone || a.box_armed) {
    // 空白处 = 框选（tweak：真拖了才生效，否则退化成一次点击 = 取消选择）。
    SelectBox *sb = new SelectBox();
    sb->start = Rect(fx, fy, 0.0f, 0.0f);
    sb->end = sb->start;
    return sb;
  }
  // 条带身上的选择：Shift 增删、Ctrl 加入、否则单选（km_sequencer 的 LEFTMOUSE select）。
  const bool modifier = shift_held() || ctrl_held();
  if (shift_held())
    toggle_selection(a, c->id);
  else if (ctrl_held())
    add_to_selection(a, c->id);
  else if (!clip_selected(a, c->id))
    select_only(a, c->id);
  if (part == kPartLeft) return handle_press(a, *c, true);
  if (part == kPartRight) return handle_press(a, *c, false);
  // 本体：按下这一刻只做选择，真拖过阈值才 invoke（见 StripClick / StripSlideArm）。
  // 带修饰键的这一次不收起选择，所以不给它挂收尾对象。
  if (modifier) return nullptr;
  const bool slidable = !c->locked && c->track >= 0 &&
                        c->track < int(a.project.tracks.size()) &&
                        !a.project.tracks[c->track].locked;
  if (!slidable) {
    StripClick *click = new StripClick();
    click->clip_id = c->id;
    return click;
  }
  StripSlideArm *g = new StripSlideArm();
  g->clip_id = c->id;
  g->t0 = x_time(a, fx);
  return g;
}

// ---------------------------------------------------------------- 中键的平移与缩放

bool CursorGrab::filter_event(App &a, int &x, int &y) {
  // 抄 GHOST 的 warp 分支（GHOST_SystemWin32.cc:1191-1238）+ GHOST_Rect::wrapPoint
  // （GHOST_Rect.hh:225-251）。margin=2 是 kGrabWrap 的取值（kGrabHide 才用 width/10）；
  // X 和 Y 都裹，因为 view2d.pan 带 OPTYPE_GRAB_CURSOR_XY（view2d_ops.cc:375）。
  const Rect &b = bounds;
  const int ofs = 2;
  const int w = int(b.w), h = int(b.h);
  if (w - ofs * 2 <= 0 || h - ofs * 2 <= 0) return false;  // wrapPoint 的防死循环
  int nx = x, ny = y;
  while (nx - ofs < int(b.x)) nx += w - ofs * 2;
  while (nx + ofs > int(b.x) + w) nx -= w - ofs * 2;
  while (ny - ofs < int(b.y)) ny += h - ofs * 2;
  while (ny + ofs > int(b.y) + h) ny -= h - ofs * 2;
  if (nx == x && ny == y) {  // 还在界内：记账结束，坐标并进事件
    warp_x = false;
    warp_y = false;
    return false;
  }
  POINT p{nx, ny};
  ClientToScreen(a.hwnd, &p);
  // 搬的是系统指针，只有物理中键真按着的时候才轮到我们动它（注入/脚本发的事件不做，
  // 否则测试会把用户的指针抢走；真实拖动时这一条永远成立）。
  if ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0) SetCursorPos(p.x, p.y); /* wrap */
  if (!warp_x && nx != x) {
    warp_x = true;
    accum_x += x - nx;
  }
  if (!warp_y && ny != y) {
    warp_y = true;
    accum_y += y - ny;
  }
  if (trace_fp) {
    wchar_t t[128];
    swprintf(t, 128, L"grab-wrap %d,%d -> %d,%d accum %d,%d", x, y, nx, ny, accum_x, accum_y);
    trace(t);
  }
  return true;
}

void CursorGrab::up(App &a) {
  // 闭环抓取的账（accum / warp 标志 / bounds）随对象一起销毁，这里只留一次读数。
  grab_report(a);
}

// Ctrl+中键拖动每一格事件该乘的缩放系数（view2d.zoom）。
// zoomfac = clamp(0.001×maxzoom, 0.001, 0.01) = 0.01（时间线 maxzoom=100，:1289）；
// facx = 0.01×Δpx（:1323），dx = facx×视野宽（:1350），:1084-1085 把视野收成 宽−2dx
// ⇒ 新视野 = 宽×(1 − 0.02Δpx)，也就是 pps ×= 1/(1 − 0.02Δpx)。Δ 钳在 ±45 px：
// 分母到 0 是这条公式自己的奇点（Blender 靠每事件位移小躲过去，闭环光标搬运会造出大跳）。
// 与 Blender 的差别：CONTINUE 那一支还乘了 time_step×5（:1063-1068）这个速度项，
// 我们按位移算 —— 同样的像素拖快拖慢缩放一样多。
static float zoom_drag_fac(float dpx) {
  const float d = std::clamp(dpx, -45.0f, 45.0f);
  return 1.0f / std::max(0.1f, 1.0f - 0.02f * d);
}

void TimelinePan::drag(App &a, float fx, float fy) {
  // view2d.pan：像素位移换算成时刻。不钳制左端：view2d 的平移只受 zoom 边界影响，
  // 时间线本身前后无限（需求），所以 scroll_t 可以为负，画面上就是「0 帧左边还有空白」。
  a.tl.scroll_t += (last_x - fx) / a.tl.pps;
  // 纵向同一个算子（keepofs 没锁 Y）：cur.ymin += (lasty - xy)，翻成屏幕坐标
  // 正好是「鼠标往下拖 = 看到更小编号的通道」，换算系数 = cur/(区高+1)。
  a.tl.view_ch += (fy - last_y) * (cur_channels(a) / (double(a.lanes.h) + 1.0));
  last_x = fx;
  last_y = fy;
  clamp_view_ch(a);
  a.dirty |= kRLanes | kRScrub | kRChan;
}

void TimelineZoom::drag(App &a, float fx, float fy) {
  // 时间线：没有 keepaspect ⇒ 横向那一支生效（view2d_ops.cc:1323、:1350）。
  (void)fy;
  const float dx = fx - last_x;
  last_x = fx;
  const double pps0 = a.tl.pps, scroll0 = a.tl.scroll_t;
  zoom_at(a, pivot_x, pivot_y, zoom_drag_fac(dx));
  if (trace_fp) {
    wchar_t t[160];
    swprintf(t, 160, L"mid-zoom tl dx %.0f pivot_x %d pivot_t %.3f pps %g->%g scroll %.2f->%.2f",
             dx, int(pivot_x), a.tl.zoom_req_t, pps0, a.tl.pps, scroll0, a.tl.scroll_t);
    trace(t);
  }
}

void ViewPan::drag(App &a, float fx, float fy) {
  a.view_pan_x += fx - last_x;
  a.view_pan_y += fy - last_y;
  last_x = fx;
  last_y = fy;
  a.dirty |= kRViewport;
}

void ViewZoom::drag(App &a, float fx, float fy) {
  // 预览区：keepaspect 那一支把横向清零，只看纵向（view2d_ops.cc:1334-1347）。
  const float dy = last_y - fy;  // Blender 的 y 向上，客户区向下
  last_y = fy;
  const float z0 = a.view_zoom, px0 = a.view_pan_x, py0 = a.view_pan_y;
  view_zoom_at(a, pivot_x, pivot_y, zoom_drag_fac(dy));
  if (trace_fp) {
    wchar_t t[160];
    swprintf(t, 160, L"mid-zoom view dy %.0f zoom %g->%g pan %.0f,%.0f->%.0f,%.0f", dy, z0,
             double(a.view_zoom), px0, py0, a.view_pan_x, a.view_pan_y);
    trace(t);
  }
}

// ---------------------------------------------------------------- 分发

std::shared_ptr<Gesture> press_left(App &a, float fx, float fy) {
  // 依次问对象，第一个答应的给出这次手势。问的顺序就是叠放的优先级（菜单/控件在外壳那一层
  // 已经先答过并 return 了）：分界线只贴线那几像素；滚动条排在时间线自己的 select/box 之前
  //（km_view2d:963 —— LEFTMOUSE PRESS = view2d.scroller_activate）；标尺带管播放头
  //（km_time_scrub:1237）；车道自己管变速键、把手、条带本体与空白；预览区里能答这一击的
  // 只有各条片段自己的取景框（裸左键不动作）。
  Gesture *raw = divider_press(a, fx, fy);
  if (!raw) raw = props_scrollbar_press(a, fx, fy);
  if (!raw) raw = scrollbar_press(a, fx, fy);
  if (!raw) raw = playhead_press(a, fx, fy);
  if (!raw) raw = lane_press(a, fx, fy);
  if (!raw) raw = framing_press(a, fx, fy);
  return claim(raw, fx, fy);
}

std::shared_ptr<Gesture> press_middle(App &a, float fx, float fy) {
  // km_view2d:964 —— MIDDLEMOUSE PRESS 同样是 scroller_activate，命中滚动条就不平移。
  if (Gesture *raw = scrollbar_press(a, fx, fy)) return claim(raw, fx, fy);
  // km_view2d:965 pan / :988 同一颗键加 Ctrl = zoom（围绕按下那一点）。答这一击的是
  // 指针底下的那块视图：预览区答 ⇒ 预览的 view2d；否则时间线答。
  const bool zoom = ctrl_held();
  const bool in_view = a.viewport.contains(fx, fy);
  std::shared_ptr<Gesture> g;
  if (in_view && zoom) {
    std::shared_ptr<ViewZoom> p = std::make_shared<ViewZoom>();
    p->pivot_x = fx;
    p->pivot_y = fy;
    g = p;
  } else if (in_view) {
    g = std::make_shared<ViewPan>();
  } else if (zoom) {
    std::shared_ptr<TimelineZoom> p = std::make_shared<TimelineZoom>();
    p->pivot_x = fx;
    p->pivot_y = fy;
    g = p;
  } else {
    g = std::make_shared<TimelinePan>();
  }
  CursorGrab *cg = static_cast<CursorGrab *>(g.get());
  // GHOST 的 grab 在算子 invoke 时开始（wm_event_system.cc:1787），范围取按下时所在的
  // region，落不到 region 就退化成整块客户区（同 :1777-1781 的 region→area→client）。
  cg->bounds = in_view ? a.viewport : a.tl_area;
  if (!cg->bounds.contains(fx, fy))
    cg->bounds = Rect(0.0f, 0.0f, float(a.r2d.width()), float(a.r2d.height()));
  cg->grab_x = fx;
  cg->grab_y = fy;
  cg->last_x = fx;
  cg->last_y = fy;
  if (trace_fp) {
    wchar_t t[160];
    swprintf(t, 160, L"mid-down kind=%s at %d,%d bounds %.0f,%.0f %.0fx%.0f scroll %.2f pps %g",
             zoom ? (in_view ? "view-zoom" : "tl-zoom") : (in_view ? "view-pan" : "tl-pan"),
             int(fx), int(fy), double(cg->bounds.x), double(cg->bounds.y), double(cg->bounds.w),
             double(cg->bounds.h), a.tl.scroll_t, a.tl.pps);
    trace(t);
  }
  return g;
}

}  // namespace app
