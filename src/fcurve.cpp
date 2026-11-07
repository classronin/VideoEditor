// 关键帧曲线实现。逐段对应 Blender 5.4 的
// source/blender/blenkernel/intern/fcurve.cc（求值）与 intern/curve.cc（自动把手、
// 连续加速度平滑）。只实现本工程用得到的分支：非循环曲线、把手类型 AUTO_ANIM、
// 外插默认 CONSTANT_HOLD。
#include "fcurve.h"

#include <algorithm>
#include <cmath>
#include <float.h>
#include <limits.h>

namespace {

constexpr double kIndexEps = 0.0001;  // 二分查找的“算不算正好落在点上”阈值
constexpr float kRootSmall = -1.0e-10f;  // SMALL，fcurve.cc:57（根的有效区间下界）
constexpr float kHandleMinGap = 0.001f;  // fcurve.cc:1215 的把手水平钳制余量

// ------------------------------------------------------ 浮点“够不够近”比较
// blenlib/intern/math_base_inline.cc:421-461 的 ulp_diff_ff / compare_ff_relative。
// 按位模式数两个 float 之间隔了几个可表示值，所以零附近的绝对误差和 1e6 附近的
// 相对误差用的是同一个尺度。

uint32_t ulp_diff_ff(float a, float b) {
  union {
    float f;
    uint32_t i;
  } ua, ub;
  ua.f = a;
  ub.f = b;
  const uint32_t sign_bit = 0x80000000u, infinity = 0x7f800000u;
  const uint32_t a_sign = ua.i & sign_bit, b_sign = ub.i & sign_bit;
  const uint32_t a_abs = ua.i & ~sign_bit, b_abs = ub.i & ~sign_bit;
  if (a_abs > infinity || b_abs > infinity) return 0xffffffffu;  // NaN 视为最远
  if (a_sign == b_sign)
    return (a_abs > b_abs ? a_abs : b_abs) - (a_abs < b_abs ? a_abs : b_abs);
  return a_abs + b_abs;
}

int compare_ff_relative(float a, float b, float max_diff, int max_ulps) {
  if (std::fabs(a - b) <= max_diff) return 1;
  return ulp_diff_ff(a, b) <= uint32_t(max_ulps) ? 1 : 0;
}

// ------------------------------------------------------------ 三次贝塞尔求值
// 以下四个函数是 fcurve.cc:1424-1606 的直译。

// 把手不能长过相邻点的时间位置，否则曲线会打环。
void correct_bezpart(float v1x, float v1y, float &v2x, float &v2y, float &v3x, float &v3y,
                     float v4x, float v4y) {
  const float h1x = v1x - v2x, h1y = v1y - v2y;
  const float h2x = v4x - v3x, h2y = v4y - v3y;
  const float len = v4x - v1x;
  const float len1 = std::fabs(h1x), len2 = std::fabs(h2x);
  if (len1 + len2 == 0.0f) return;
  if (len1 > len) {
    const float fac = len / len1;
    v2x = v1x - fac * h1x;
    v2y = v1y - fac * h1y;
  }
  if (len2 > len) {
    const float fac = len / len2;
    v3x = v4x - fac * h2x;
    v3y = v4y - fac * h2y;
  }
}

double cubert(double d) {  // BLI_sqrt3d（math_base_inline.cc:62）
  if (d == 0.0) return 0.0;
  if (d < 0.0) return -std::exp(std::log(-d) / 3.0);
  return std::exp(std::log(d) / 3.0);
}

// 解 c0 + c1·x + c2·x² + c3·x³ = 0，只保留落在 [SMALL, 1.000001] 的根。
int solve_cubic(double c0, double c1, double c2, double c3, float *o) {
  int nr = 0;
  auto keep = [&](double r) { return r >= double(kRootSmall) && r <= 1.000001; };
  if (c3 != 0.0) {
    double a = c2 / c3, b = c1 / c3, c = c0 / c3;
    a /= 3.0;
    const double p = b / 3 - a * a;
    const double q = (2 * a * a * a - a * b + c) / 2;
    const double d = q * q + p * p * p;
    if (d > 0.0) {
      const double t = std::sqrt(d);
      o[0] = float(cubert(-q + t) + cubert(-q - t) - a);
      return keep(o[0]) ? 1 : 0;
    }
    if (d == 0.0) {
      const double t = cubert(-q);
      o[0] = float(2 * t - a);
      if (keep(o[0])) nr++;
      o[nr] = float(-t - a);
      if (keep(o[nr])) return nr + 1;
      return nr;
    }
    const double phi = std::acos(-q / std::sqrt(-(p * p * p)));
    double t = std::sqrt(-p);
    const double cp = std::cos(phi / 3);
    const double sq = std::sqrt(3 - 3 * cp * cp);
    o[0] = float(2 * t * cp - a);
    if (keep(o[0])) nr++;
    o[nr] = float(-t * (cp + sq) - a);
    if (keep(o[nr])) nr++;
    o[nr] = float(-t * (cp - sq) - a);
    if (keep(o[nr])) return nr + 1;
    return nr;
  }
  double a = c2, b = c1, c = c0;
  if (a != 0.0) {
    const double disc = b * b - 4 * a * c;
    if (disc > 0) {
      const double p = std::sqrt(disc);
      o[0] = float((-b - p) / (2 * a));
      if (keep(o[0])) nr++;
      o[nr] = float((-b + p) / (2 * a));
      if (keep(o[nr])) return nr + 1;
      return nr;
    }
    if (disc == 0) {
      o[0] = float(-b / (2 * a));
      if (keep(o[0])) return 1;
    }
    return 0;
  }
  if (b != 0.0) {
    o[0] = float(-c / b);
    return keep(o[0]) ? 1 : 0;
  }
  if (c == 0.0) {
    o[0] = 0.0f;
    return 1;
  }
  return 0;
}

int findzero(float x, float q0, float q1, float q2, float q3, float *o) {
  return solve_cubic(double(q0) - x, 3.0 * (q1 - q0), 3.0 * (double(q0) - 2 * q1 + q2),
                     double(q3) - q0 + 3.0 * (q1 - q2), o);
}

float berekeny(float f1, float f2, float f3, float f4, float t) {
  const double c0 = f1, c1 = 3.0 * (f2 - f1), c2 = 3.0 * (double(f1) - 2 * f2 + f3),
               c3 = double(f4) - f1 + 3.0 * (f2 - f3);
  return float(c0 + t * c1 + double(t) * t * c2 + double(t) * t * t * c3);
}

// ------------------------------------------------------------ 把手计算
// curve.cc:3091-3260 的 calchandleNurb_intern，只走 is_fcurve + AUTO_ANIM 分支。
void calc_auto_handle(Key &k, const Key *prev, const Key *next, bool smoothing) {
  k.locked_final = false;
  if (prev == nullptr && next == nullptr) return;

  double p1x, p1y, p3x, p3y;
  if (prev == nullptr) {
    p3x = next->x; p3y = next->y;
    p1x = 2.0 * k.x - p3x; p1y = 2.0 * double(k.y) - p3y;
  } else {
    p1x = prev->x; p1y = prev->y;
  }
  if (next == nullptr) {
    p1x = prev->x; p1y = prev->y;
    p3x = 2.0 * k.x - p1x; p3y = 2.0 * double(k.y) - p1y;
  } else {
    p3x = next->x; p3y = next->y;
  }

  const double da_x = double(k.x) - p1x, da_y = double(k.y) - p1y;
  const double db_x = p3x - double(k.x), db_y = p3y - double(k.y);
  double len_a = da_x, len_b = db_x;  // is_fcurve ⇒ 只取 x 分量
  if (len_a == 0.0) len_a = 1.0;
  if (len_b == 0.0) len_b = 1.0;

  if (k.h1 != Key::AutoAnim && k.h2 != Key::AutoAnim) return;

  const double tvec_x = db_x / len_b + da_x / len_a;
  const double tvec_y = db_y / len_b + da_y / len_a;
  // 平滑开启时强制「把手水平长度 = 区间/3」，让时间的参数化变成线性（curve.cc:3170-3183）。
  double len = smoothing ? 6.0 / 2.5614 : tvec_x;
  len *= 2.5614;
  if (len == 0.0) return;
  if (!smoothing) {
    len_a = std::min(len_a, 5.0 * len_b);
    len_b = std::min(len_b, 5.0 * len_a);
  }

  bool left_violate = false, right_violate = false;
  if (k.h1 == Key::AutoAnim) {
    const double s = len_a / len;
    k.h1x = float(k.x - tvec_x * s);
    k.h1y = float(double(k.y) - tvec_y * s);
    if (prev && next) {  // 极值点钳成水平；否则把手不得越过邻点
      const double y1 = double(prev->y) - k.y, y2 = double(next->y) - k.y;
      if ((y1 <= 0 && y2 <= 0) || (y1 >= 0 && y2 >= 0)) {
        k.h1y = k.y;
        k.locked_final = true;
      } else if (y1 <= 0) {
        if (prev->y > k.h1y) { k.h1y = prev->y; left_violate = true; }
      } else {
        if (prev->y < k.h1y) { k.h1y = prev->y; left_violate = true; }
      }
    }
  }
  if (k.h2 == Key::AutoAnim) {
    const double s = len_b / len;
    k.h2x = float(k.x + tvec_x * s);
    k.h2y = float(double(k.y) + tvec_y * s);
    if (prev && next) {
      const double y1 = double(prev->y) - k.y, y2 = double(next->y) - k.y;
      if ((y1 <= 0 && y2 <= 0) || (y1 >= 0 && y2 >= 0)) {
        k.h2y = k.y;
        k.locked_final = true;
      } else if (y1 <= 0) {
        if (next->y < k.h2y) { k.h2y = next->y; right_violate = true; }
      } else {
        if (next->y > k.h2y) { k.h2y = next->y; right_violate = true; }
      }
    }
  }
  // 一侧被钳过 ⇒ 另一侧按同一斜率对齐，保持把手共线（curve.cc:3248-3258）。
  if (left_violate || right_violate) {
    const float h1x = k.h1x - float(k.x), h2x = float(k.x) - k.h2x;
    if (left_violate) {
      if (h1x != 0.0f) k.h2y = float(double(k.y) + (double(k.y) - k.h1y) / h1x * h2x);
    } else {
      if (h2x != 0.0f) k.h1y = float(double(k.y) + (double(k.y) - k.h2y) / h2x * h1x);
    }
  }
}

// ------------------------------------------------------------ 连续加速度平滑
// curve.cc:3400-4015。解三对角方程组让二阶导连续，把手越界就锁住重解。

void eq_continuous(float *a, float *b, float *c, float *d, const float *dy, const float *l, int i) {
  a[i] = l[i] * l[i];
  b[i] = 2.0f * (l[i] + 1);
  c[i] = 1.0f / l[i + 1];
  d[i] = dy[i] * l[i] * l[i] + dy[i + 1];
}
void eq_noaccel_right(float *a, float *b, float *c, float *d, const float *dy, const float *l, int i) {
  a[i] = 0.0f; b[i] = 2.0f; c[i] = 1.0f / l[i + 1]; d[i] = dy[i + 1];
}
void eq_noaccel_left(float *a, float *b, float *c, float *d, const float *dy, const float *l, int i) {
  a[i] = l[i] * l[i]; b[i] = 2.0f * l[i]; c[i] = 0.0f; d[i] = dy[i] * l[i] * l[i];
}
void lock_unknown(float *a, float *b, float *c, float *d, int i, float value) {
  a[i] = 0.0f; b[i] = 1.0f; c[i] = 0.0f; d[i] = value;
}
// auto clamp：本点不许反向，邻点不许过冲（curve.cc:3648-3676）。
void clamp_range(float *hmax, float *hmin, int i, float dy, bool no_reverse, bool no_overshoot) {
  if (dy > 0) {
    if (no_overshoot) hmax[i] = std::min(hmax[i], dy);
    if (no_reverse) hmin[i] = 0.0f;
  } else if (dy < 0) {
    if (no_reverse) hmax[i] = 0.0f;
    if (no_overshoot) hmin[i] = std::max(hmin[i], dy);
  } else if (no_reverse || no_overshoot) {
    hmax[i] = hmin[i] = 0.0f;
  }
}
float handle_adj(float hx, float dx) {  // bezier_calc_handle_adj
  const float fac = dx / (hx + dx / 3.0f);
  if (fac < 1.0f) hx *= fac;
  return 1.0f - 3.0f * hx / dx;
}

// 标准 Thomas 解；本工程的曲线恒非循环，a[0] 与 c[n-1] 必为 0。
bool tridiagonal(const float *a, const float *b, const float *c, const float *d, float *x, int n) {
  std::vector<double> tmp(n), sol(n);
  tmp[0] = b[0];
  sol[0] = d[0];
  for (int i = 1; i < n; i++) {
    const double m = tmp[i - 1] == 0.0 ? 0.0 : 1.0 / tmp[i - 1];
    if (m == 0.0) return false;
    tmp[i] = b[i] - m * a[i] * c[i - 1];
    sol[i] = d[i] - m * a[i] * sol[i - 1];
    if (tmp[i] == 0.0) return false;
  }
  x[n - 1] = float(sol[n - 1] / tmp[n - 1]);
  for (int i = n - 2; i >= 0; i--)
    x[i] = float(sol[i] / tmp[i] - c[i] * x[i + 1] / tmp[i]);
  return true;
}

bool solve_with_limits(float *a, float *b, float *c, float *d, float *h, const float *hmin,
                       const float *hmax, int n) {
  std::vector<float> a0(a, a + n), b0(b, b + n), c0(c, c + n), d0(d, d + n);
  std::vector<char> locked(n, 0), unlocks(n, 0);
  bool overshoot = false, unlocked = false;
  do {
    if (!tridiagonal(a, b, c, d, h, n)) return false;
    bool all = false, was_locked = false;
    do {
      overshoot = false;
      for (int i = 0; i < n; i++) {
        if (h[i] >= hmin[i] && h[i] <= hmax[i]) continue;
        overshoot = true;
        const float target = h[i] > hmax[i] ? hmax[i] : hmin[i];
        if (target != 0.0f || all) {
          locked[i] = 1;
          lock_unknown(a, b, c, d, i, target);
          was_locked = true;
        }
      }
      all = true;
    } while (overshoot && !was_locked);

    if (!was_locked) {
      for (int i = 0; i < n; i++) {
        if (!locked[i] || unlocks[i] >= 2) continue;
        const double state = double(a0[i]) * h[(i + n - 1) % n] + double(b0[i]) * h[i] +
                             double(c0[i]) * h[(i + 1) % n] - double(d0[i]);
        const double relax = -state * b0[i];
        if ((relax > 0 && h[i] < hmax[i]) || (relax < 0 && h[i] > hmin[i])) {
          a[i] = a0[i]; b[i] = b0[i]; c[i] = c0[i]; d[i] = d0[i];
          locked[i] = 0;
          unlocks[i]++;
          unlocked = true;
        }
      }
    }
  } while (overshoot || unlocked);
  return true;
}

bool is_clamped(const Key &k) { return k.h1 == Key::AutoAnim || k.h2 == Key::AutoAnim; }

// 对 keys[start..start+count) 这一段做平滑；段外的点把手固定。
void smooth_run(std::vector<Key> &keys, int start, int count) {
  if (count < 2) return;
  Key &first = keys[start];
  Key &last = keys[start + count - 1];
  const bool solve_first = first.free_auto() && start == 0;
  const bool solve_last = last.free_auto() && start + count == int(keys.size());
  if (count == 2 && solve_first == solve_last) return;

  std::vector<float> dx(count, 0), dy(count, 0), l(count, 1), a(count, 0), b(count, 0), c(count, 0),
      d(count, 0), h(count, 0), hmax(count, FLT_MAX), hmin(count, -FLT_MAX);
  for (int i = 1; i < count; i++) {
    dx[i] = float(keys[start + i].x - keys[start + i - 1].x);
    dy[i] = keys[start + i].y - keys[start + i - 1].y;
  }
  l[0] = l[count - 1] = 1.0f;
  for (int i = 1; i < count - 1; i++) l[i] = dx[i + 1] / dx[i];

  bool clamped_prev = false, clamped_cur = is_clamped(first);
  for (int i = 1; i < count; i++) {
    clamped_prev = clamped_cur;
    clamped_cur = is_clamped(keys[start + i]);
    clamp_range(hmax.data(), hmin.data(), i - 1, dy[i], clamped_prev, clamped_prev);
    clamp_range(hmax.data(), hmin.data(), i, dy[i] * l[i], clamped_cur, clamped_cur);
  }

  float first_adj = 0.0f, last_adj = 0.0f;
  if (!solve_first) {
    first_adj = handle_adj(first.h2x - float(first.x), dx[1]);
    lock_unknown(a.data(), b.data(), c.data(), d.data(), 0, first.h2y - first.y);
  } else {
    eq_noaccel_right(a.data(), b.data(), c.data(), d.data(), dy.data(), l.data(), 0);
  }
  if (!solve_last) {
    last_adj = handle_adj(float(last.x) - last.h1x, dx[count - 1]);
    lock_unknown(a.data(), b.data(), c.data(), d.data(), count - 1, last.y - last.h1y);
  } else {
    eq_noaccel_left(a.data(), b.data(), c.data(), d.data(), dy.data(), l.data(), count - 1);
  }
  for (int i = 1; i < count - 1; i++)
    eq_continuous(a.data(), b.data(), c.data(), d.data(), dy.data(), l.data(), i);
  if (count > 2 || solve_last) b[1] += l[1] * first_adj;
  if (count > 2 || solve_first) b[count - 2] += last_adj;

  if (!solve_with_limits(a.data(), b.data(), c.data(), d.data(), h.data(), hmin.data(), hmax.data(),
                         count))
    return;
  for (int i = 1; i < count - 1; i++) {
    Key &k = keys[start + i];
    if (k.h1 == Key::AutoAnim) k.h1y = float(double(k.y) - h[i] / l[i]);
    if (k.h2 == Key::AutoAnim) k.h2y = float(double(k.y) + h[i]);
  }
  if (solve_first && first.h2 == Key::AutoAnim) first.h2y = float(double(first.y) + h[0]);
  if (solve_last && last.h1 == Key::AutoAnim) last.h1y = float(double(last.y) - h[count - 1] / l[count - 1]);
}

// 段内插值（fcurve_eval_keyframes_interpolate，fcurve.cc:2076-2200）。
float interp_segment(const Key &a, const Key &b, double x) {
  const float duration = float(b.x - a.x);
  if (a.interp == Key::Constant || duration == 0.0f) return a.y;
  if (a.interp == Key::Linear)
    return a.y + (b.y - a.y) * (float(x - a.x) / duration);

  const float v1x = float(a.x), v1y = a.y;
  float v2x = a.h2x, v2y = a.h2y, v3x = b.h1x, v3y = b.h1y;
  const float v4x = float(b.x), v4y = b.y;
  if (std::fabs(v1y - v4y) < FLT_EPSILON && std::fabs(v2y - v3y) < FLT_EPSILON &&
      std::fabs(v3y - v4y) < FLT_EPSILON)
    return v1y;  // 四个把手的 y 全相等 ⇒ 曲线是平的，值就是这个常数
  float u1x = v1x, u1y = v1y;
  correct_bezpart(u1x, u1y, v2x, v2y, v3x, v3y, v4x, v4y);
  float roots[32];
  if (findzero(float(x), u1x, v2x, v3x, v4x, roots) == 0) return 0.0f;
  return berekeny(v1y, v2y, v3y, v4y, roots[0]);
}

}  // namespace

// ------------------------------------------------------------------ 求值

float FCurve::eval(double x) const {
  const int n = int(keys.size());
  if (n == 0) return 0.0f;
  if (n == 1) return keys[0].y;
  if (std::fabs(x - keys[0].x) <= kIndexEps) return keys[0].y;
  if (std::fabs(x - keys[n - 1].x) <= kIndexEps) return keys[n - 1].y;

  // 端点外插（fcurve_eval_keyframes_extrapolate，fcurve.cc:2024-2074）：默认平持端点值。
  auto extrapolate = [&](const Key &ep, const Key &nb, double t, bool forward) -> float {
    if (hold_endpoints || ep.interp == Key::Constant) return ep.y;
    if (ep.interp == Key::Linear) {
      const double span = nb.x - ep.x;
      if (span == 0.0) return ep.y;
      return float(double(ep.y) + double(t - ep.x) * double(nb.y - ep.y) / span);
    }
    // 用朝向邻点那一侧把手的斜率外推。
    const double hx = forward ? ep.h1x : ep.h2x;
    const double hy = forward ? ep.h1y : ep.h2y;
    const double fac = ep.x - hx;
    if (fac == 0.0) return ep.y;
    return float(double(ep.y) - double(ep.y - hy) / fac * double(ep.x - t));
  };
  if (x < keys[0].x) return extrapolate(keys[0], keys[1], x, false);
  if (x > keys[n - 1].x) return extrapolate(keys[n - 1], keys[n - 2], x, true);

  int lo = 0, hi = n - 1;  // 不变式：keys[lo].x <= x <= keys[hi].x
  while (hi - lo > 1) {
    const int mid = (lo + hi) / 2;
    if (keys[mid].x <= x) lo = mid; else hi = mid;
  }
  return interp_segment(keys[lo], keys[hi], x);
}

// ------------------------------------------------------------------ 编辑

void FCurve::set_at(double x, float y) {
  for (Key &k : keys) {
    if (std::fabs(k.x - x) <= kIndexEps) {
      k.y = y;
      recalc_handles();
      return;
    }
  }
  Key nk;
  nk.x = x;
  nk.y = y;
  // insert_vert_fcurve 的新键把手先放在 x±1（animrig/intern/fcurve.cc:300-313）。
  nk.h1x = float(x) - 1.0f; nk.h1y = y;
  nk.h2x = float(x) + 1.0f; nk.h2y = y;
  auto it = std::lower_bound(keys.begin(), keys.end(), x,
                             [](const Key &a, double v) { return a.x < v; });
  // 曲线已有 2 个点以上时，新键继承前一个点的插值（animrig/intern/fcurve.cc:470-490）。
  if (keys.size() >= 2 && it != keys.begin()) nk.interp = (it - 1)->interp;
  keys.insert(it, nk);
  recalc_handles();
}

bool FCurve::set_at_if_needed(double x, float y) {
  // new_key_needed() 的三条分支（animrig/intern/fcurve.cc:362-385），
  // 判定通过后才走 insert_vert_fcurve。
  if (keys.empty()) {
    set_at(x, y);
    return true;
  }
  // 该帧上已经有键：:374 用 float 精确比较，不是容差比较。
  for (const Key &k : keys) {
    if (std::fabs(k.x - x) <= kIndexEps) {
      if (k.y == y) return false;
      set_at(x, y);
      return true;
    }
  }
  // 该帧上没有键：曲线在这里本来就等于这个值，就不插。
  if (compare_ff_relative(eval(x), y, FLT_EPSILON, 32)) return false;
  set_at(x, y);
  return true;
}

void FCurve::offset_keys(double frames) {
  // 三处都用 float 加：Blender 的 vec[][0] 是 float，double 结果再窄化会多出一次舍入。
  const float d = float(frames);
  for (Key &k : keys) {
    k.h1x += d;
    k.x += d;
    k.h2x += d;
  }
}

void FCurve::remove_at(double x) {
  for (size_t i = 0; i < keys.size(); i++) {
    if (std::fabs(keys[i].x - x) <= kIndexEps) {
      keys.erase(keys.begin() + long(i));
      break;
    }
  }
  recalc_handles();
}

void FCurve::recalc_handles() {
  const int n = int(keys.size());
  if (n < 2) return;

  for (int i = 0; i < n; i++) {
    Key &k = keys[i];
    // 把手水平位置钳在点两侧 0.001 之外（fcurve.cc:1215-1224）。
    k.h1x = std::min(k.h1x, float(std::nextafter(k.x - kHandleMinGap, -1e30)));
    k.h2x = std::max(k.h2x, float(std::nextafter(k.x + kHandleMinGap, 1e30)));
    calc_auto_handle(k, i > 0 ? &keys[i - 1] : nullptr, i + 1 < n ? &keys[i + 1] : nullptr,
                     accel_smooth);
    // 端点 + CONSTANT 外插 ⇒ 两侧把手都水平，并标记为不再参与平滑（fcurve.cc:1229-1240）。
    if (k.auto_handle() && (i == 0 || i == n - 1) && hold_endpoints) {
      k.h1y = k.h2y = k.y;
      k.locked_final = true;
    }
    // 重复点会让平滑整体崩掉，退化成锁定（fcurve.cc:1244-1247）。
    if (i > 0 && keys[i - 1].x >= k.x) keys[i - 1].locked_final = k.locked_final = true;
  }

  if (!accel_smooth) return;
  // 对每段连续的「自由自动把手」点做二阶导连续平滑，两端各带一个固定点当边界
  // （BKE_nurb_handle_smooth_fcurve，curve.cc:3926-3968）。
  for (int s = 0; s < n;) {
    if (!keys[s].free_auto()) { s++; continue; }
    int e = s;
    while (e + 1 < n && keys[e + 1].free_auto()) e++;
    const int rs = std::max(0, s - 1), re = std::min(n - 1, e + 1);
    if (re > rs) smooth_run(keys, rs, re - rs + 1);
    s = e + 1;
  }
}

