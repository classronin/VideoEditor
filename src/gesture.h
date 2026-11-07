#pragma once
// ---------------------------------------------------------------- 手势
// 「一次按下」是一个对象。为什么要单独立这一层（逻辑.md 的归属）：
// 按下之后要记住的那些数 —— 起点、按下那一刻的快照、每像素的换算系数、闭环抓取的范围 ——
// 只在这一次按住期间有意义。它们不属于窗口外壳（外壳摊成一排零散字段，就是上一个手势没清
// 干净、下一个手势读到借用值的那类病），也不属于被拖的那个对象（把按下的快照写进 Clip，
// 撤消快照会把它一起带进历史；Blender 存的是 op->customdata，也不是条带）。
// 所以存这里：谁被点中，谁交出一个手势对象；松开，这整个对象连同它记的账一起销毁。
//
// 触发的那一头（main.cpp 的窗口过程）绝对中立：它只知道「槽里有一个手势」，把事件交给它，
// 不判断这是哪一块地方。答应不答应、怎么反应，全在被点中的那个对象自己身上。
#include <memory>
#include <vector>

#include "app.h"

namespace app {

// PRESS 之后要走成 CLICK_DRAG 所需的最小位移：U.drag_threshold_mouse = 3
// （versioning_userdef.cc:1251），乘 UI_SCALE_FAC（wm_event_query.cc:407-426），
// 判定用 |dx|+|dy|（interface_handlers.cc:2213-2217）。
inline float drag_threshold() { return 3.0f * btheme::scale; }

struct Gesture {
  float grab_x = 0.0f, grab_y = 0.0f;  // 按下那一点
  bool moved = false;  // 越过 3px 阈值 = 这一次按下走的是拖拽分支，不是点击

  virtual ~Gesture() = default;

  // 每个 MOUSEMOVE 都先进这里：分级（CLICK / CLICK_DRAG）由基类统一做，子类只写「拖的一格」。
  // 调用方必须先把槽抄进局部变量再调这里 —— 子类的 drag() 有权把这一格交回去（invoke 模态），
  // 直接 a.gesture->move(...) 会让对象在函数体里被销毁。
  void move(App &a, int x, int y, bool down);

  // 拖动的一格。fx/fy 是换算过闭环光标搬运之后的客户区坐标。
  virtual void drag(App &a, float fx, float fy) {}
  // 松开那一步：事务收尾（撤消步、框选落地、翻页）。
  virtual void up(App &a) {}
  // 事件源头的前置：闭环抓取光标把跑出去的指针搬回来。
  // 返回 true = 这一格事件不发下去（GHOST 的 warp 分支同样是 return nullptr）。
  virtual bool filter_event(App &a, int &x, int &y) { return false; }
  // true = 不等阈值，从第一次 MOUSEMOVE 就跟手：滚动条、分界线、取景框 —— 拖动本来就要连续
  // 跟手，这里没有第二种点击语义要靠阈值去区分。（阈值只用来分 CLICK 和 CLICK_DRAG。）
  virtual bool always_follow() const { return false; }
};

// 画的那一头要知道槽里现在装的是哪种手势（框选的矩形、滚动条的高亮、分界线的光标）。
// 这是「读一个正在进行的手势的状态」，不是「按地方决定动作」。
template <class T> inline T *gesture_as(App &a) {
  return dynamic_cast<T *>(a.gesture.get());
}

// 按下的一击交给第一个答应的对象；谁都不答应 = 空槽（这一击不动作）。
std::shared_ptr<Gesture> press_left(App &a, float fx, float fy);
std::shared_ptr<Gesture> press_middle(App &a, float fx, float fy);
// 松开：跑完这次手势的收尾，再把槽交还。左键和中键的松开都走这里。
void release_gesture(App &a);

// ---- 播放头 -------------------------------------------------------------------
// 数据归工程（types.h 的 Project::playhead，和 sfra/efra 同层，跟着快照走）；
// 「拖播放头」这个动作归标尺带：它自己回答「这一点在我身上吗」，答应之后就改那一个数。
struct PlayheadDrag : Gesture {
  void drag(App &a, float fx, float fy) override;
};

// ---- 条带的把手（左/右缘 = 入点/出点）-----------------------------------------
// 悬停命中：指针落在这颗把手上就给左右箭头光标（WM_SETCURSOR 用，和按下走同一处判定）。
bool strip_handle_hot(App &a, float fx, float fy);

// 裁的是「带着这颗把手标志的那一组」，名单不在这儿：标志打在 Clip::handle_sel 上
// （= Blender 的 SEQ_LEFTSEL/SEQ_RIGHTSEL，逐条带的标志），按下时打在被点的那条和它的
// 连接成员身上（sequencer_select.cc:1355-1371 的抄送）。
struct TrimDrag : Gesture {
  bool left = true;
  void drag(App &a, float fx, float fy) override;
  void up(App &a) override;
};

// ---- 空白处框选（sequencer.select_box 的 tweak）--------------------------------
// 真拖了才生效；没拖动的这一次按下松开 = sequencer.select(deselect_all)。
struct SelectBox : Gesture {
  Rect start, end;
  void drag(App &a, float fx, float fy) override;
  void up(App &a) override;
  Rect box() const;  // 规整成左上/右下，绘制与落地共用
};

// ---- 条带本体与变速键：CLICK_DRAG 的另一半 ------------------------------------
// 条带身上的一次按下分两步走，两步都在这个对象里：
// 按下只做选择（在命中那一层已经做完）；松开才把选择收成「被点的这一条 + 它的连接组」。
// Blender 把同一步写在 sequencer.select 的松开分支里（按在已选中的条带上先返回
// RUNNING_MODAL，未越过阈值才 deselect_all + 单选）。不这么做的话，断开连接后点另一条，
// 那条早就挂在名单上的人就再也点不亮自己 —— 选择集合里还留着上一组。
// 真拖过阈值时 drag() 把槽交给 transform.seq_slide（数据、吸附、Esc 还原全在 ModalState
// 自带的那份快照里），松开那一步就不再归这里；鼠标拖和 G 键因此走同一条路。
struct StripClick : Gesture {
  int64_t clip_id = -1;
  void up(App &a) override;
};
struct StripSlideArm : StripClick {
  double t0 = 0.0;  // 按下那一刻指针底下的时刻（抓取点，不是播放头）
  void drag(App &a, float fx, float fy) override;
};
struct KeySlideArm : Gesture {
  int64_t clip_id = -1;
  size_t key_idx = 0;
  double t0 = 0.0;
  void drag(App &a, float fx, float fy) override;
};

// ---- 右侧滚动条（view2d.scroller_activate）------------------------------------
struct ScrollBar : Gesture {
  int zone = int(kSQBar);           // 按在滑块上、把手上还是外面的轨道（ScrollZone）
  double fac = 0.0, fac_round = 0.0;  // view2d_ops.cc:1952-1968 的两个换算系数
  double last_y = 0.0;                // vsm->lasty：逐次事件的位移
  bool always_follow() const override { return true; }
  void drag(App &a, float fx, float fy) override;
  void up(App &a) override;
  // 按在滑块身上（画的那一头用它决定高亮哪一格）。
  bool on_thumb() const { return zone == kSQBar; }
  void step(App &a, double delta_up) const;  // delta_up = 「y 朝上」的像素位移
};

// ---- 两条分界线 ---------------------------------------------------------------
// 改的是排版（时间线占多高 / 面板占多宽），归窗口；不走阈值。
struct DividerH : Gesture {
  bool always_follow() const override { return true; }
  void drag(App &a, float fx, float fy) override;
  void up(App &a) override;
};
struct DividerV : Gesture {
  bool always_follow() const override { return true; }
  void drag(App &a, float fx, float fy) override;
  void up(App &a) override;
};

// ---- 属性面板的滚动条 ----------------------------------------------------------
// 只有滑块这一格：拖它 = 写 props_scroll（界由 draw_props 收）。时间线那条滚动条是
// view2d 的 scroller_activate，两端把手能改行高；面板这一块没有 view2d，也没有
// 「行高」可改，所以没有把手那一层。
struct PropsScroll : Gesture {
  float last_y = 0.0f;  // vsm->lasty：逐次事件的位移
  bool always_follow() const override { return true; }
  void drag(App &a, float fx, float fy) override;
};

// ---- 中键：view2d.pan / view2d.zoom -------------------------------------------
// 闭环抓取光标（GHOST_SystemWin32.cc:1179-1244）的那一份账只属于这一次拖动：指针拖出范围
// 就搬到对侧，搬掉的那段距离记进 accum，事件坐标 + accum 才是这次拖动的「虚拟」坐标，
// 所以同一次按住不放能一直往一个方向推。
struct CursorGrab : Gesture {
  Rect bounds;  // 按下时所在的那一块视图；落不到视图就退成整块客户区
  int accum_x = 0, accum_y = 0;
  bool warp_x = false, warp_y = false;  // SetCursorPos 偶尔被系统丢掉：一次搬运不许记两次账
  float last_x = 0.0f, last_y = 0.0f;
  bool filter_event(App &a, int &x, int &y) override;
  void up(App &a) override;
};
// 缩放的那两支共用一条：支点是「按下那一点」，不是当前指针（view2d_ops.cc:1080 用的是
// invoke 时存下的 mx_2d）；拖动过程中指针跑远了，被钉住的那一时刻也不换。
struct ZoomGrab : CursorGrab {
  float pivot_x = 0.0f, pivot_y = 0.0f;
};
// 时间线：pan 改 scroll_t/view_ch，zoom 改 pps —— 都是 TimelineView 这个对象的属性，
// 由对象自己的反应函数收尾（timeline_view_react / clamp_view_ch）。
struct TimelinePan : CursorGrab {
  void drag(App &a, float fx, float fy) override;
};
struct TimelineZoom : ZoomGrab {
  void drag(App &a, float fx, float fy) override;
};
// 预览区的 view2d：同一个算子，另一块视图。
struct ViewPan : CursorGrab {
  void drag(App &a, float fx, float fy) override;
};
struct ViewZoom : ZoomGrab {
  void drag(App &a, float fx, float fy) override;
};

// ---- 预览取景框 ---------------------------------------------------------------
// 每条片段一只自己的框，按下先判它落在谁的哪只角上。下面这组就是那一次按下的快照
// （transform_convert_sequencer_image.cc:60-86 的 TransDataSeq：orig_translation、
// orig_scale、quad_orig 三个都存下来），每个 MOUSEMOVE 都从快照重算，不做累加 ——
// 所以中途改播放头、改曲线都不会累积误差。实现与命中测试都在 ops.cpp（几何在那一层）。
struct FramingDrag : Gesture {
  int64_t clip_id = -1;
  double ofs0x = 0.0, ofs0y = 0.0;        // orig_translation
  double scale0x = 1.0, scale0y = 1.0;    // orig_scale
  float pivx = 0.0f, pivy = 0.0f;         // 支点（transform.origin 那一点）的屏幕像素
  float ux = 0.0f, uy = 0.0f;             // 支点 → 被抓那个角的单位向量
  float d0 = 0.0f;                        // 按下时「角到支点」的距离
  bool by_corner = false;                 // 角上 = 等比写缩放XY，框内 = 写位置XY
  bool always_follow() const override { return true; }
  void drag(App &a, float fx, float fy) override;
  void up(App &a) override;
};
// 从最上面那条片段往下问自己的四角和框内（先角后框内：角把手压在框线上，同一个点既是角
// 也在框里，把手优先，否则永远抓不到缩放）。没命中返回 nullptr。
FramingDrag *framing_press(App &a, float fx, float fy);

}  // namespace app
