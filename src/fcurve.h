#pragma once
// 关键帧曲线：Blender FCurve 的最小子集，只保留本工程用得到的三种插值。
// 全部语义抄 source/blender/blenkernel/intern/{fcurve.cc,curve.cc}，出处写在实现里。
#include <cstdint>
#include <vector>

// 一个关键帧 = BezTriple：vec[0] 左把手、vec[1] 点本身、vec[2] 右把手（绝对坐标）。
struct Key {
  enum Interp : uint8_t { Bezier = 0, Linear = 1, Constant = 2 };  // BEZT_IPO_*
  enum Handle : uint8_t { Free = 1, Vector = 3, AutoAnim = 4 };    // HD_AUTO / HD_VECT / HD_AUTO_ANIM

  double x = 0.0;  // 绝对帧号（动画系统按 scene->cfra 求值）
  float y = 0.0f;
  float h1x = 0.0f, h1y = 0.0f;  // 左把手
  float h2x = 0.0f, h2y = 0.0f;  // 右把手
  uint8_t interp = Bezier;
  uint8_t h1 = AutoAnim, h2 = AutoAnim;
  // HD_AUTOTYPE_LOCKED_FINAL：自动把手被钳制成水平的那个点（端点/极值点），
  // 后续平滑pass 不再动它。
  bool locked_final = false;

  bool auto_handle() const { return h1 == AutoAnim || h2 == AutoAnim; }
  bool free_auto() const { return auto_handle() && !locked_final; }
};

// 一条曲线对应一条可动画属性（Blender：一个属性一条 FCurve）。
struct FCurve {
  std::vector<Key> keys;  // 恒按 x 升序；插入时维持
  // fcu->extend 默认 FCURVE_EXTRAPOLATE_CONSTANT ⇒ 首尾之外按端点值平持。
  bool hold_endpoints = true;
  // U.auto_smoothing_new 默认 FCURVE_SMOOTH_CONT_ACCEL（DNA_userdef_types.h:1183）。
  bool accel_smooth = true;

  float eval(double x) const;
  // 在 x 处取值（无点则插点、有点则改值），随后重算把手。
  // 对应 insert_vert_fcurve()（animrig/intern/fcurve.cc:429 起）不带
  // INSERTKEY_NEEDED 的那条路。
  void set_at(double x, float y);
  // KEYING_FLAG_INSERTNEEDED：值没变就不动曲线。判定逐条抄
  // animrig/intern/fcurve.cc:362-385 的 new_key_needed()。返回是否真的写了键。
  bool set_at_if_needed(double x, float y);
  // animation.cc:56-61 offset_animdata()：键和两个把手的 x 一起平移，
  // 不重算把手（Blender 平移后把手相对点的位置保持不变）。
  void offset_keys(double frames);
  void remove_at(double x);
  bool empty() const { return keys.empty(); }
  void recalc_handles();
};
