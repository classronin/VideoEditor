#pragma once
// 变速曲线：Blender 的 Strip::retiming_keys 在本工程里的等价实现。
//
// 数据形状照 SeqRetimingKey（makesdna/DNA_sequence_types.h:344-352），两处按本工程的
// 单位换算，行为不变：
//  - 全程用「秒」而不是「帧」。Blender 的 strip_frame_index 是源帧号，取用时要除以
//    media_playback_rate_factor（= 场景 fps / 素材 fps，见 strip_retiming.cc:196-201 的
//    retiming_key_frame_get）；本工程的时间轴本来就是秒，这个因子恒等于 1，所以
//    strip_frame 直接就是「距条带左缘的时间线秒」。
//  - y 轴存「源内绝对秒」而不是 retiming_factor(0..1)。factor 只是把内容长度归一，
//    retiming_key_speed_get（:204-219）算的 Δ(factor×len)/Δx 与 Δsrc/Δx 是同一个数，
//    而绝对秒在裁左把手（source_in 变化）时不会让整条曲线跟着重新缩放。
//
// 空曲线 = 1× 恒速，对应 Blender 里 retiming_keys_num==0 的条带：give_frame_index 的
// `if (retiming_has_keys(strip))`（strip_time.cc:66-70）跳过求值，源时刻就是线性推进。
// retiming_data_ensure（:351-366）建的两把默认键 {0, 0.0} 与 {content_length, 1.0}
// 求值出来同样是恒速 1×，所以这里不必为「没有变速」的条带真的存键。
#include <algorithm>
#include <cmath>
#include <vector>

#include "types.h"

// 一把变速键的定义在 types.h（Clip 的字段类型，和 FCurve 一样属于共享数据）。

// 从左缘的入点算起，源素材还剩多少秒。Blender 的 strip->content_length()。
double retiming_content(const Clip &c);
// 这条带能不能参与变速：retiming_is_allowed（strip_retiming.cc:154-169）先否掉
// content_length() ≤ 1 帧的素材（一帧没有可拉伸的余量），再按类型白名单放行。
// 本工程导入的都是影片/图像/声音条带，全在名单里，所以只剩帧数这一项。
bool retiming_is_allowed(const Clip &c, double frame_dur);

// 求值：x（距左缘秒）→ 源内秒。空曲线 ⇒ source_in + x。
double retiming_src_at(const Clip &c, double x);
// x 所在段的速度 = 源秒 / 时间线秒（retiming_key_speed_get，:204）。
double retiming_speed_at(const Clip &c, double x);
// 以第 i 把键「收尾」的那一段的速度 = retiming_key_speed_get（strip_retiming.cc:204-219）。
// 注意别和上面那个混：retiming_speed_at(键自己的位置) 返回的是从这把键开始的那一段。
inline double retiming_key_speed(const Clip &c, size_t i) {
  if (i == 0 || i >= c.retiming.size()) return 1.0;
  return retiming_speed_at(c, 0.5 * (c.retiming[i - 1].strip_frame + c.retiming[i].strip_frame));
}
// x 所在段的两端（时间线秒）与速度。空曲线 ⇒ [0, duration] 一段、速度 1。
// 音频按段喂给播放器，对应 retiming_sound_animation_data_set 里逐段
// BKE_sound_set_scene_sound_pitch_constant_range 的那个循环（:1246-1274）。
void retiming_range_at(const Clip &c, double x, double *t0, double *t1, double *speed);

// 条带长度还能往长拉多少秒才吃到素材末尾。max_duration() 的实现。
double retiming_max_duration(const Clip &c);

// 在 x 处加一把键，键值取当前曲线在该处的求值（strip_retiming_add_key，:430-482：
// `float value = strip_retiming_evaluate(strip, frame_index)`）。
// x 越出条带、或那个位置已经有键 ⇒ 不动并返回 false。
bool retiming_add_key(Clip &c, double x);
// 拖一把键：Blender 的 retiming_key_frame_set（strip_retiming.cc:947-982）收的是「绝对
// 时间线帧」，先按 strip_retiming_clamp_offset（:918-945）把位移钳到相邻键之间（键不许
// 碰到或越过邻居，最少隔一帧），然后按三种情况落地：
//  - 末键（= 条带右把手）⇒ 只挪它，条带长度跟着变（length() 就是从末键推出来的，
//    strip_time.cc:532-543）；
//  - 首键（strip_frame_index == 0）⇒ start += delta，其余键反向补偿 -delta，
//    于是右缘不动、左缘移动、第一段变速；
//  - 中间键 ⇒ 只挪它，前一段变速、条带总长不动。
// delta 用时间线秒（调用方已按整帧量化），frame_dur 用来算「至少隔一帧」那条限位。
// 返回真正生效的位移（被钳过就可能变小）。
double retiming_move_key(Clip &c, size_t i, double delta, double frame_dur);
// 没有键的条带在 Blender 里画两把「假键」，点中它们才真正建出两端的两把
// （realize_fake_keys，:375-380）。已经 ≥2 把 ⇒ 不动并返回 false。
bool retiming_realize_keys(Clip &c);
// 按下标删一把键。首尾两把不可删（retiming_remove_multiple_keys:532-536）。
bool retiming_remove_key_index(Clip &c, size_t i);
// 清成恒速 = retiming_data_clear（:393-400），retiming_reset 的前半段。
void retiming_reset(Clip &c);
// 设「以第 end_key 把键收尾的那一段」的速度，返回段的实际速度（受整帧取整与相邻键的
// 夹持，可能 ≠ 请求值）。这就是 retiming_key_speed_set（:244-256）：Blender 的键属于
// 它右边那段的结尾，所以选中一把键改的是它左边那段。end_key 是首把 ⇒ 没有这一段，返回 1。
double retiming_set_key_speed(Clip &c, size_t end_key, double speed, double frame_dur);
// 按位置设速：x 落在哪一段就改那一段（= retiming_set_key_speed 的段尾键是 x 之后第一把）。
double retiming_set_speed(Clip &c, double x, double speed, double frame_dur);

// 把左缘移到距原点 cut 秒处（cut>0 = 裁掉左边这一段；cut<0 = 向左延长，源时刻按首段
// 斜率外推）。所有键跟着平移，返回新的入点秒，调用方写回 c.source_in。
double retiming_move_left_edge(Clip &c, double cut);
// 曲线在 x 轴上的反函数：源秒 src 落在曲线的哪个时间线位置。曲线单调递增（段速恒正）。
// 越出两端时按首/末段的斜率往外推，不钳到端点（give_frame_index 的钳制是在源轴上，
// 不在时间线轴上）。滑移限位「不许超出素材」就靠这个外推值。
double retiming_x_at_src(const Clip &c, double src);
// 滑移：条带在时间线上不动，采样点沿曲线滑过 delta 秒（delta>0 = 往后吃内容）。
// Blender 的 slip（strip_time.cc:406-467）只改 start / startofs / end_offset，
// retiming 键与 anim_startofs 都不碰，所以这里等价于把整条曲线沿 x 平移 −delta；
// 滑到素材头/尾即停，返回实际滑动的秒数。
double retiming_slip(Clip &c, double delta);
// 整条曲线在源轴上平移 d 秒（滑移：时间线不动、内容挪）。
void retiming_shift_src(Clip &c, double d);
// 把 [x_from, x_to] 这段按曲线切成「每段恒速」的若干条带，供导出用：每份的
// timeline_start/duration/source_in 都是该段的常数，曲线只剩两端。
std::vector<Clip> retiming_slices(const Clip &c, double x_from, double x_to);

// ---- 调用方惯用的三个换算（都是上面求值的薄封装）----

// 条带的「源时刻」：时间线时刻 t（在条带内）对应的素材内秒数。
// 没有变速键时就是 source_in + (t - timeline_start)。
inline double src_time(const Clip &c, double t) {
  return retiming_src_at(c, t - c.timeline_start);
}
// 条带吃掉的源长度（秒）。TrimR / slip / 导出 trim 的上限都按它算。
inline double src_span(const Clip &c) {
  return std::max(0.0, retiming_src_at(c, c.duration) - retiming_src_at(c, 0.0));
}
// 源素材到头之前，条带在时间线上还能有多少秒。
inline double max_duration(const Clip &c) {
  if (!c.info || c.info->duration <= 0.0) return 1e18;
  return std::max(0.0, retiming_max_duration(c));
}
