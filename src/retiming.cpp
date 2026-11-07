// 变速曲线的求值与编辑。单位换算与逐条出处见 retiming.h 的注释。
#include "retiming.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kxEps = 1e-6;  // 键位匹配的容差（秒）：半帧都远大于它

double seg_speed(const RetimeKey &a, const RetimeKey &b) {
  const double dx = b.strip_frame - a.strip_frame;
  return dx > kxEps ? (b.src - a.src) / dx : 0.0;
}

// x 落在哪一段（返回段起点下标）；空曲线返回 -1。
// 对应 retiming_find_segment_start_key（strip_retiming.cc:102-117）：跳过最后一把键，
// 取 strip_frame_index 不超过 x 的最后一把。
int seg_index_at(const Clip &c, double x) {
  const auto &k = c.retiming;
  int idx = -1;
  for (size_t i = 0; i + 1 < k.size(); i++) {
    if (k[i].strip_frame > x) break;
    idx = int(i);
  }
  return idx;
}

// 键挪过位置之后把入点字段对齐回曲线左缘：source_in 恒等于「条带左缘的源时刻」，
// retiming_content()（素材长 − 入点）与工程存盘都按这个约定算。
void sync_in(Clip &c) {
  if (c.retiming.size() >= 2) c.source_in = retiming_src_at(c, 0.0);
}

// 曲线退化成恒速 1×、且左缘就是入点 ⇒ 清成空曲线。「空」是恒速的唯一表示，
// 否则同一个状态有两种存法，撤消比较和面板灰显都会跟着摇摆。
void collapse_if_identity(Clip &c) {
  const auto &k = c.retiming;
  if (k.size() != 2) return;
  if (std::fabs(seg_speed(k[0], k[1]) - 1.0) > kxEps) return;
  if (std::fabs(retiming_src_at(c, 0.0) - c.source_in) > kxEps) return;
  c.retiming.clear();
}

}  // namespace

double retiming_content(const Clip &c) {
  const double total = c.info && c.info->duration > 0.0 ? c.info->duration : 0.0;
  return std::max(0.0, total - c.source_in);
}

bool retiming_is_allowed(const Clip &c, double frame_dur) {
  // Blender 的白名单里没有 TEXT（strip_retiming.cc:161-168）：文本带不是一段素材，
  // 没有「源里还剩多少秒」可以压。这一格不能只靠 info 为空蒙对 —— 哪天文本带上挂了
  // 什么时长，变速就会自己活过来。
  if (c.kind == kStripText) return false;
  return frame_dur > 0.0 && retiming_content(c) / frame_dur > 1.0;
}

double retiming_src_at(const Clip &c, double x) {
  const auto &k = c.retiming;
  if (k.size() < 2) return c.source_in + x;
  const int i = seg_index_at(c, x);
  if (i < 0) return k[0].src + (x - k[0].strip_frame) * seg_speed(k[0], k[1]);
  return k[i].src + (x - k[i].strip_frame) * seg_speed(k[i], k[i + 1]);
}

double retiming_speed_at(const Clip &c, double x) {
  const auto &k = c.retiming;
  if (k.size() < 2) return 1.0;
  const int i = seg_index_at(c, x);
  if (i < 0) return seg_speed(k[0], k[1]);
  return seg_speed(k[i], k[i + 1]);
}

double retiming_x_at_src(const Clip &c, double src) {
  const auto &k = c.retiming;
  if (k.size() < 2) return src - c.source_in;
  const double s0 = seg_speed(k[0], k[1]);
  if (src <= k[0].src) return s0 > kxEps ? k[0].strip_frame + (src - k[0].src) / s0 : k[0].strip_frame;
  for (size_t i = 0; i + 1 < k.size(); i++) {
    if (src <= k[i + 1].src) {
      const double s = seg_speed(k[i], k[i + 1]);
      return s > kxEps ? k[i].strip_frame + (src - k[i].src) / s : k[i + 1].strip_frame;
    }
  }
  const double sn = seg_speed(k[k.size() - 2], k.back());
  return sn > kxEps ? k.back().strip_frame + (src - k.back().src) / sn : k.back().strip_frame;
}

void retiming_range_at(const Clip &c, double x, double *t0, double *t1, double *speed) {
  const auto &k = c.retiming;
  if (k.size() < 2) {
    *t0 = 0.0;
    *t1 = c.duration;
    *speed = 1.0;
    return;
  }
  int i = seg_index_at(c, x);
  if (i < 0) i = 0;
  *t0 = k[i].strip_frame;
  *t1 = k[i + 1].strip_frame;
  *speed = seg_speed(k[i], k[i + 1]);
}

double retiming_max_duration(const Clip &c) {
  const double total = c.info && c.info->duration > 0.0 ? c.info->duration : 0.0;
  const auto &k = c.retiming;
  if (k.size() < 2) return total - c.source_in;
  // 曲线内部先到达素材末尾 ⇒ 上限就是那个交点；否则按末段斜率往外推。
  for (size_t i = 0; i + 1 < k.size(); i++) {
    if (k[i + 1].src >= total) {
      const double s = seg_speed(k[i], k[i + 1]);
      return s > 0.0 ? k[i].strip_frame + (total - k[i].src) / s : k[i].strip_frame;
    }
  }
  const double s = seg_speed(k[k.size() - 2], k.back());
  return s > 0.0 ? k.back().strip_frame + (total - k.back().src) / s : 1e18;
}

bool retiming_add_key(Clip &c, double x) {
  if (x <= kxEps || x >= c.duration - kxEps) return false;  // 两端已经有键
  // 键必须挂在一条完整曲线上：先补出条带两端的键（ensure_left_and_right_keys，:368-373）。
  if (c.retiming.size() < 2)
    c.retiming = {{0.0, c.source_in}, {c.duration, c.source_in + c.duration}};
  for (const RetimeKey &k : c.retiming)
    if (std::fabs(k.strip_frame - x) <= kxEps) return false;  // 已有键 ⇒ 原样返回
  const double v = retiming_src_at(c, x);
  auto it = std::upper_bound(c.retiming.begin(), c.retiming.end(), x,
                             [](double xx, const RetimeKey &k) { return xx < k.strip_frame; });
  c.retiming.insert(it, {x, v});
  return true;
}

// 拖一把键 = Blender 的 retiming_key_frame_set（strip_retiming.cc:947-982）：先按
// strip_retiming_clamp_offset（:918-945）把位移钳进「相邻键之间、至少隔一帧」，再分三种
// 情况落地。这里不做 collapse_if_identity —— 拖动中途把曲线清成「没有键」会让快照重放
// 找不到键，Blender 也没有这一步。
double retiming_move_key(Clip &c, size_t i, double delta, double frame_dur) {
  auto &k = c.retiming;
  if (frame_dur <= 0.0 || k.size() < 2 || i >= k.size()) return 0.0;
  const bool is_last = (i + 1 == k.size());
  // 相邻键限位：末键右边没有 next ⇒ 不限（条带可以一直往长拖）；首键 x==0 没有 prev ⇒
  // 不限左边。Blender 的 :918-945 就是这个形状。
  double dmin = -1e18, dmax = 1e18;
  if (!is_last) dmax = k[i + 1].strip_frame - k[i].strip_frame - frame_dur;
  if (i > 0) dmin = -(k[i].strip_frame - k[i - 1].strip_frame - frame_dur);
  delta = std::clamp(delta, dmin, dmax);
  if (std::fabs(delta) < kxEps) return 0.0;

  if (i == 0) {
    // 首键（= 左把手）：条带整体平移，后面的键反向补偿，右缘因此钉住不动。
    // Blender 那边是 content_start += d、length（= 末键的 strip_frame_index）随之 −d，
    // 可见右缘 = content_end − end_offset 恰好不动（strip_time.cc:532-569）。
    c.timeline_start += delta;
    for (size_t j = 1; j < k.size(); j++) k[j].strip_frame -= delta;
    c.duration -= delta;
  } else {
    k[i].strip_frame += delta;
    // 末键挪动就是改长度；中间键只改自己那两段的速度，总长不动。
    if (is_last) c.duration += delta;
  }
  sync_in(c);
  return delta;
}

// 把「假键」变成真键：Blender 在没有键的条带两端各画一把 fake key，点上去才
// realize_fake_keys（strip_retiming.cc:375-380 = retiming_data_ensure + 在两个把手处
// 各加一把键）。这里刻意不走 collapse_if_identity ⇒ 恒速的两把键会留着，和 Blender 一样。
bool retiming_realize_keys(Clip &c) {
  if (c.retiming.size() >= 2) return false;
  c.retiming = {{0.0, c.source_in}, {c.duration, c.source_in + c.duration}};
  return true;
}

void retiming_reset(Clip &c) {
  // 清键 = 回到恒速 1×，但「这条带吃掉了多少源」不能跟着一起清掉：恒速下条带长度就等于
  // 吃掉的源长，所以先把曲线末端的源时刻折回长度，再清数组。Blender 不需要这一步，
  // 它那边条带长度是从曲线推出来的（strip_time.cc:532-543 的 length()），清完键自然回到
  // 1× 的长度；本工程把 duration 存成字段，不折就把上一次的变速长度当成新的源跨度，
  // 于是每次改整条带速度都在放大：6 秒素材 50% → 12 → 60% → 20 → 70% → 28.6。
  const double eaten = src_span(c);
  c.retiming.clear();
  if (eaten > kxEps) c.duration = eaten;
}

// 按下标删一把键（retiming_remove_multiple_keys:532-536 的过滤条件：
// 第一把和最后一把永不删）。
bool retiming_remove_key_index(Clip &c, size_t i) {
  if (i == 0 || i + 1 >= c.retiming.size()) return false;
  c.retiming.erase(c.retiming.begin() + i);
  collapse_if_identity(c);
  return true;
}

double retiming_set_key_speed(Clip &c, size_t end_key, double speed, double frame_dur) {
  if (c.retiming.size() < 2)
    c.retiming = {{0.0, c.source_in}, {c.duration, c.source_in + c.duration}};
  if (end_key == 0 || end_key >= c.retiming.size()) return 1.0;
  const double s = std::max(1e-5, speed);
  RetimeKey &a = c.retiming[end_key - 1], &b = c.retiming[end_key];
  const double content = b.src - a.src;
  // round 到整帧（retiming_key_new_frame_from_speed_get:241 的 std::round）。
  double want = a.strip_frame + std::round(content / s / frame_dur) * frame_dur;
  const bool is_last = (end_key + 1 == c.retiming.size());
  // 限位只按相邻键，不按素材长度（strip_retiming_clamp_bounds_get:177-193：
  // 上界取 next key、下界取 prev key，末键没有 next ⇒ 往右不限）。
  // 是否连带改条带长度，看的是 retiming_key_frame_set:962 那句
  // 「frame_old == strip->right_handle()」：这把键正好贴在可见右缘上才算右把手。
  // 裁短过的条带末键会落在右缘之外（Blender 那里是 endofs 变大），那时挪它只改这一段
  // 的速度、不把长度顶回老位置。
  const bool is_edge = is_last && std::fabs(b.strip_frame - c.duration) <= 0.5 * frame_dur;
  if (!is_last) want = std::min(want, c.retiming[end_key + 1].strip_frame - frame_dur);
  want = std::max(want, a.strip_frame + frame_dur);
  b.strip_frame = want;
  if (is_edge) c.duration = want;
  const double actual = seg_speed(a, b);
  collapse_if_identity(c);
  return actual;
}

double retiming_set_speed(Clip &c, double x, double speed, double frame_dur) {
  int i = seg_index_at(c, x);
  if (i < 0) i = 0;
  return retiming_set_key_speed(c, size_t(i + 1), speed, frame_dur);
}

double retiming_move_left_edge(Clip &c, double cut) {
  const double s0 = retiming_src_at(c, cut);
  std::vector<RetimeKey> nk;
  nk.push_back({0.0, s0});
  for (const RetimeKey &k : c.retiming)
    if (k.strip_frame > cut + kxEps) nk.push_back({k.strip_frame - cut, k.src});
  // 只剩两端那一把 ⇒ 曲线退化成恒速，回到「没有键」的形态。
  c.retiming = nk.size() >= 2 ? std::move(nk) : std::vector<RetimeKey>{};
  c.source_in = s0;
  collapse_if_identity(c);
  return s0;
}

double retiming_slip(Clip &c, double delta) {
  const double total = c.info && c.info->duration > 0.0 ? c.info->duration : 0.0;
  // 曲线沿 x 平移 −d 之后，条带 [0, duration] 采的是原来 [d, d + duration] 那一段，
  // 限位就是「这一段不许超出素材」：源 0 处对应的 x 是下界，源末尾对应的 x 减掉条带长是上界。
  const double lo = retiming_x_at_src(c, 0.0);
  const double hi = total > 0.0 ? retiming_x_at_src(c, total) - c.duration : delta;
  const double d = std::clamp(delta, lo, std::max(lo, hi));
  if (d == 0.0) return 0.0;
  for (RetimeKey &k : c.retiming) k.strip_frame -= d;
  sync_in(c);  // 空曲线没有键可挪，入点直接就是平移量。
  if (c.retiming.empty()) c.source_in += d;
  collapse_if_identity(c);
  return d;
}

void retiming_shift_src(Clip &c, double d) {
  for (RetimeKey &k : c.retiming) k.src += d;
}

std::vector<Clip> retiming_slices(const Clip &c, double x_from, double x_to) {
  std::vector<double> cut{x_from};
  for (const RetimeKey &k : c.retiming)
    if (k.strip_frame > x_from + kxEps && k.strip_frame < x_to - kxEps) cut.push_back(k.strip_frame);
  cut.push_back(x_to);
  std::vector<Clip> out;
  for (size_t i = 0; i + 1 < cut.size(); i++) {
    const double a = cut[i], b = cut[i + 1];
    if (b - a <= kxEps) continue;
    Clip w = c;
    w.timeline_start = c.timeline_start + a;
    w.duration = b - a;
    w.source_in = retiming_src_at(c, a);
    // 每一片只剩两端一把键，段内恒速 ⇒ 下游按普通条带走就行。
    w.retiming = {{0.0, w.source_in}, {w.duration, retiming_src_at(c, b)}};
    out.push_back(std::move(w));
  }
  return out;
}
