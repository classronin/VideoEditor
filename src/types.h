#pragma once
// VSE 独立视频剪辑器：共享类型。时间单位统一为“源帧编号”(int64) 与 “秒”(double)。
#include <windows.h>
#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "fcurve.h"

struct Frac {  // 有理数，用于帧率与宽高比显示
  int32_t num = 0;
  int32_t den = 1;
  double value() const { return den ? double(num) / den : 0.0; }
};

struct VideoInfo {
  std::wstring path;
  std::wstring codec;
  int width = 0, height = 0;
  int sar_num = 1, sar_den = 1;
  Frac frame_rate;
  bool cfr = true;
  int64_t frame_count = 0;
  double duration = 0.0;
  // 容器里第一只视频样本的 pts（秒）。只有「按秒定位」的门才比这条绝对时间基
  // （ffmpeg 输入侧 `-ss`、MF 的 SetCurrentPosition）；工程内部流转的源内秒一律从首帧起算
  // ⇒ 送定位前要把这一个常数加回去。选帧的门不许用它，见 src_frame。
  double start_time = 0.0;
  bool has_audio = false;
  int audio_rate = 48000, audio_channels = 2;
  // 音频流自己的原点：和上面那条不是一个数（AAC 的 priming、音画各自的 elst 都能把它们
  // 错开），所以音频那一路只许加这一条。
  double audio_start_time = 0.0;

  // 「源内秒 → 容器秒」这一加法只许写在这两个函数里，而且只给 seek 那两道门用：
  // 新添一个把秒交给定位的门口，调这里，别自己再写一次 + start_time。
  double video_pts(double rel_sec) const { return rel_sec + start_time; }
  double audio_pts(double rel_sec) const { return rel_sec + audio_start_time; }

  // 「源内秒 → 第几只帧/样本」是另一种货币，选帧的门（预览落位、图里的 trim 与逐 tick 摆放）用它。
  // 导出那一路不在输入侧 -ss，解码流交给滤镜的第一帧就是源内第 0 帧 ⇒ 这一路不需要容器原点；
  // 交出去的又是整数 ⇒ 刀口不会压在两只样本的 pts 上、由 µs 级舍入决定谁进谁出。
  // 实测反例：原点 2/12288 秒被 ffprobe 打成 0.000163（向上舍入），拿它当 trim 的 start
  // 就把源的第 0 帧切掉了 —— 导出的成片开头少一帧；预览按 keyframe 落位，不受这 1µs 影响。
  //
  // 帧号 = 最近的一只。这是 Blender 的那一条：round_fl_to_int(give_frame_index(...))，
  // 预览与最终渲染共用同一个函数（render.cc:1111）。浮点残渣由「取最近」本身吸收
  // （61.000000000000014 → 61），所以这里不再写容差；容差一旦写成「差一点就往后取一只」，
  // 就和预览不是同一条规则了 —— 实测源 59.94 进 25 的工程，旧写法 25 只 tick 里有 20 只
  // 比预览早一帧，导出的画面与看到的画面不是同一串帧。
  double src_fps() const {
    return frame_rate.value() > 0 ? frame_rate.value() : 25.0;  // 回退值也只有这一处
  }
  int64_t src_frame(double rel_sec) const { return std::llround(rel_sec * src_fps()); }
  // 音频没有「帧格对齐」这回事（一秒几万只样本，差一只 = 21µs），取最近的一只即可。
  int64_t src_sample(double rel_sec) const {
    return std::llround(rel_sec * double(audio_rate > 0 ? audio_rate : 48000));
  }
};

// strip.transform 的 5 个可动画浮点通道。rna_def_strip_transform
// （rna_sequencer.cc:2267-2317）没有对任何属性 clear_flag(PROP_ANIMATABLE)，
// 所以缩放/位移/旋转在 Blender 里本来就能打 keyframe（裁切 :2200-2266 同理，
// 本工程的跟踪+缩放需求暂时只用到这 5 个）。
// 顺序与 app.h 的 Field 枚举 kFOfsX..kFRotation 完全一致 ⇒ 下标 = id - kFOfsX，
// 面板不必再维护一张映射表。
enum AnimChan : int { kChOfsX = 0, kChOfsY, kChScaleX, kChScaleY, kChRotation, kAnimCount };

// 一把变速键。数据形状照 Blender 的 SeqRetimingKey
// （makesdna/DNA_sequence_types.h:344-352），单位换算见 retiming.h 的注释。
struct RetimeKey {
  double strip_frame = 0.0;  // 距条带左缘的时间线秒（Blender: strip_frame_index）
  double src = 0.0;          // 该处对应的源内秒（Blender: retiming_factor × content_length）
};

// 拖把手时「跟着一起裁」的那一颗把手，对应 Strip::flag 里的 SEQ_LEFTSEL / SEQ_RIGHTSEL
// （makesdna/DNA_sequence_types.h:45-46）。它是逐条带的标志，不是一份全局清单：
// sequencer.select_handle 给被点的条带打上它，再由 sequencer_select_connected_strips
// （sequencer_select.cc:1355-1371）把同一颗标志抄给连接成员，随后的
// transform.seq_clip_handles 裁的就是「带这颗标志的全部条带」。
enum StripHandle : int { kHandleNone = 0, kHandleLeft = 1, kHandleRight = 2 };

// Strip::type 的三档。Blender 的 DNA_sequence_types.h 只有 STRIP_TYPE_MOVIE / STRIP_TYPE_SOUND，
// 文本在它那里是「影片条带 + effectdata 里的 TextVars」（SEQ_TEXT effect）；本工程没有 effect 栈，
// 文本自己成一档，好处是所有「这条路只对影片成立」的判断都写成 kind 而不是「非声音」。
enum StripKind : int { kStripMovie = 0, kStripSound = 1, kStripText = 2 };

// 文本条带的内容与样式，逐项对齐 TextVars（makesdna/DNA_sequence_types.h）与
// sequencer/intern/effects/effect_text.cc 的 init_text_effect（:187-222）。
// Blender 另有 SEQ_TEXT_OUTLINE / SEQ_TEXT_BORDER 两颗开关标志；这里不存它们，
// 因为算子里的判据本来就含 alpha（effect_text.cc 的门是
// `outline_width < 1 || outline_color[3] <= 0`），alpha 为 0 就是关。所以出厂默认的
// 轮廓/方框 alpha 写 0 = 关（Blender 的默认是「颜色存着但标志没开」，同一个观感）。
struct TextVars {
  std::wstring content = L"你好，世界";  // Blender 默认 "Text"；换行用 '\n'（手动回车，不做自动换行）
  std::wstring family = L"Microsoft YaHei";  // DirectWrite 的家族名，出厂默认（本机字体表里确有此项）
  bool bold = false, italic = false;         // 对应 TextVars::flag 的 SEQ_TEXT_BOLD / _ITALIC
  // text_size 在 Blender 里就是「输出像素」，只按预览渲染比例折算一次
  // （text_effect_line_size_get，:797-807：size_scale * text_size）⇒ 这里存原值。
  double size = 60.0;
  // 行距倍数：Blender 的 space_line 默认 1.0，行高 = 字体自己的自然行高 * space_line；
  // 排版器按 PROPORTIONAL 把倍数落在自然行高上（见 text.cpp），所以这里只存倍数。
  double line_spacing = 1.0;
  float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // copy_v4_fl(data->color, 1.0f)
  float outline_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  // 宽度是相对量：Blender 换算成像素用 outline_width * line_height * 0.5（见 text.cpp）。
  double outline_width = 0.05;
  float box_color[4] = {0.2f, 0.2f, 0.2f, 0.0f};
  double box_margin = 0.01;      // 相对画布宽（Blender：box_margin * image->x）
  double box_roundness = 0.0;    // 0..1，圆角半径 = roundness * 盒高 / 2
};

struct Clip {
  std::wstring source_path;
  const VideoInfo *info = nullptr;  // 由 MediaCache 持有
  int64_t id = 0;
  int track = 0;                // 所在通道（下标 0 = Blender 的 Channel 1）
  // 导入带音轨的影片时，音轨是一条独立的 SOUND 条带，落在影片下面的通道，和影片自动连接
  // （sequencer_add.cc:1334/1350 的通道分配、:1394-1400 的连接）。只有 SOUND 条带发声：
  // BKE_sound_add_scene_sound 先判 strip->sound == nullptr 就直接返回
  // （blenkernel/intern/sound.cc:945-951），而 strip->sound 只在 add_sound_strip 里赋值
  // （sequencer/intern/strip_add.cc:323）⇒ 影片条带本身不带声音。
  StripKind kind = kStripMovie;
  // kind == kStripText 时才有内容；其余两档忽略它（不另开 union：一份 Clip 表要能整体拷贝进
  // 撤消快照，成员的默认值就是「没设过」的样子）。
  TextVars text;
  double timeline_start = 0.0;  // 秒
  double source_in = 0.0;       // 秒
  double duration = 0.0;        // 秒（时间线上）
  // 变速曲线，对应 Blender 的 Strip::retiming_keys（DNA_sequence_types.h:383 起）与
  // 变速算子 sequencer.retiming_segment_speed_set（sequencer_retiming.cc:685-710，
  // 属性 speed 是「百分之」，默认 100、硬下限 0.001、软下限 0.1）。
  // 段速的定义照抄 retiming_key_speed_get（strip_retiming.cc:204-219）：
  //   speed = 段内源帧数 / 段长  ⇒  时间线走 1 秒，源素材走 speed 秒。
  // 空 = 没有变速（retiming_keys_num==0 的条带，give_frame_index 跳过求值，
  // strip_time.cc:66-70），源时刻线性推进。求值与编辑都在 retiming.h/.cpp。
  std::vector<RetimeKey> retiming;
  // Strip::flag 的 SEQ_SHOW_RETIMING（sequencer.retiming_show = Ctrl+R 切它，
  // 加键的算子也会顺手打开，sequencer_retiming.cc:185）：关掉时时间线上不画这些键。
  bool show_retiming = false;
  // 条带级开关，对应 Strip::flag 的 SEQ_MUTE / SEQ_LOCK / SEQ_SELECT
  // （sequencer_edit.cc:1366-1380 的 mute、:1422-1431 的 lock 都作用在条带本身，
  //  与通道头里的静音/锁定图标 = SeqTimelineChannel.mute/lock 是两个层级；
  //  两层在 render.cc:2240-2243 的 render_is_muted() 里取并集）。
  bool muted = false;
  bool locked = false;
  // 这一次按住的把手（Strip::flag 的 SEQ_LEFTSEL/SEQ_RIGHTSEL 的等价物，见上面的
  // StripHandle）。拖把手裁的就是「带着这颗标志的全部条带」，所以这一组不需要外壳
  // 另存一份名单：标志打在谁身上，谁就是这一组的成员（连接组的抄送见 gesture.cpp）。
  int handle_sel = kHandleNone;
  // 变换/裁切，字段语义对齐 RNA（makesrna/intern/rna_sequencer.cc）：
  //   transform.offset_x/offset_y：PROP_PIXEL，ui_range(-FLT_MAX, FLT_MAX, step=100, 3)
  //     —— 以输出像素为单位的位移，y 轴向上为正（渲染时取反到屏幕坐标）。
  //   transform.scale_x/scale_y：PROP_UNSIGNED，ui_range(0, FLT_MAX, step=3, 3)，默认 1.0
  //   transform.rotation：PROP_ANGLE，绕画面中心。
  //   crop.min_x/max_x/min_y/max_y：PROP_INT，ui_range(0, 4096, 1)。
  double ofs_x = 0.0, ofs_y = 0.0;
  double scale_x = 1.0, scale_y = 1.0;
  double rotation = 0.0;        // 度
  // transform.origin：StripTransform::origin[2]，默认 {0.5, 0.5}（sequencer.cc:100），
  // RNA 是 ui_range(0, 1, 1, 3) 的二维浮点（rna_sequencer.cc:2310 起，注释
  // "Origin of image for transformation"）。(0,0) = 图片左下角，(1,1) = 右上角；
  // 它是「变换围绕的那个点」在画面框里的归一化位置，见 strip_transform.cc:803-810
  // 的 convert_origin_to_image_offset（= box*origin - box/2）。
  double origin_x = 0.5, origin_y = 0.5;
  // strip.use_flip_x / use_flip_y = Strip::flag 的 SEQ_FLIPX / SEQ_FLIPY
  // （rna_sequencer.cc:3187-3195），在 Transform 面板的 “Mirror” 一栏里成对出现
  // （properties_strip.py:1036-1038）。
  bool flip_x = false, flip_y = false;
  int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;  // 像素
  // 关键帧。Blender 把这些条带的动画挂在 Scene 的 AnimData/Action 上，通道名是
  // "sequence_editor.strips_all[<条带名>].transform.xxx"（animation.cc:38-44 用
  // 同一个前缀做匹配），求值结果每帧写回 Strip 结构体本身（anim_sys.cc:574-597）。
  // 这里按条带存一条曲线，下标 = app.h 里的 Field id - kFOfsX，与面板控件一一对应。
  // 上面那 5 个标量保持「没被动画覆盖时的值」：曲线为空时读写标量，行为与今天完全一致。
  FCurve anim[kAnimCount];
  std::wstring label;
  // 连接组，对应 Strip::connections（DNA_sequence_types.h:426-427，每项是 StripConnection
  // {Strip *strip_ref}，:605-608）。Blender 存指针，
  // 本工程的 Clip 住在 std::vector 里、增删就整体搬家，所以存 id。
  // 语义照 strip_connect.cc 的 connect()：它先把每个成员从旧组里整个摘掉，再让它指向
  // 组内所有其它成员 ⇒ 组永远是「两两相连」的团，没有相邻、同通道、间隙之类的要求，
  // 连接本身不携带任何数据，只有一条规则：选中一个 = 选中全组。
  std::vector<int64_t> connections;
};

// 条带的「源时刻」「吃掉的源长度」「还能延多长」都随变速曲线变化，实现放在 retiming.h。

// 通道 = Blender 的 SeqTimelineChannel（DNA_scene_types.h）：一条扁平的 128 项列表，
// 视频与声音条带都住在同一列通道里（channels.cc:30-40 一次建满，默认名 "Channel %d"，
// 序号 0 只占位不画）。所以这里没有「视频轨/声音轨」两种行，只有通道号。
struct Track {
  int index = 0;
  std::wstring name;
  bool muted = false;
  bool locked = false;
};

struct OutputFormat {
  // 对齐 Blender 的 RenderData（source/blender/makesdna/DNA_scene_types.h）：
  // xsch/ysch 是缓冲尺寸，xasp/yasp 是像素宽高比，pcent 是渲染缩放。
  // 帧率同样是「整数 + 基数」两个字段：frs_sec / frs_sec_base
  // （rna_scene.cc:7428 fps 是 PROP_INT range(1,SHRT_MAX)；:7436 fps_base 是
  //  PROP_FLOAT range(1e-5,1e6)，注释明确写着预设会把它设成 1.001 —— 即 29.97 = 30 / 1.001）。
  // 显示宽高比 = (xsch * xasp) / (ysch * yasp)，见 sequencer_view.cc:300。
  int width = 1920, height = 1080;      // xsch / ysch
  int fps = 30;                         // frs_sec
  double fps_base = 1.0;                // frs_sec_base
  // xasp / yasp：PROP_FLOAT，range(1,200)、无小数位声明 ⇒ Blender 的浮点默认
  // precision=3（rna_define.cc:1273），界面显示 1.000。这里必须是浮点：存成整数会让
  // 1.01 被截回 1，正是「输入 1.01 它变回 1.00」的原因。
  double par_x = 1, par_y = 1;          // xasp / yasp
  int scale_percent = 100;              // pcent
  // 帧范围 = RenderData::sfra / efra（DNA_scene_types.h:866 的新场景默认 1..250）。
  // 本工程的时间从 0 帧起算，所以默认写成 0..250；它就是播放范围
  // （BKE_scene_get_playback_range，scene.cc:2607-2633：没开 SCER_PRV_RANGE 时
  //  直接返回 sfra/efra），Ctrl+Home/End 改的就是这里、导出也按这里取帧。
  int frame_start = 0;                  // sfra
  int frame_end = 250;                  // efra
  // 步长 = RenderData::frame_step（DNA_scene_types.h:878 的新场景默认 1）。
  // RNA：PROP_INT、range(0, MAXFRAME)、ui_range(1, 100, 1, -1)（rna_scene.cc:9392-9400），
  // 说明文字是「Number of frames to skip forward while rendering/playing back each frame」。
  // 本工程里读它的是导出（export.cpp 的取帧），不是交互播放 —— Blender 的交互播放器用的是
  // 自己那份步长（wm_playanim.cc:269 的 ps.frame_step，:1835 起手 1、:1903 只从命令行改），
  // 从不读 r.frame_step；视图/时间线也不读它（它不进取景那一格）。填 0 时按 Blender 的
  // 版本迁移折回 1（versioning_250.cc:1014-1016），否则渲染循环 nfra += step 永不前进。
  int frame_step = 1;                   // frame_step
  // 导出质量 = Blender 的 FFmpeg 常量质量因子（eFFMpegCrf，DNA_scene_types.h:104-113）。
  // 需求留三档：恒定码率 -1（FFM_CRF_NONE）、无损 0、感知无损 17；默认走「感知无损」，
  // 不是真正的无损（0 走的是另一条编码路径）。
  // 恒定码率这一档不发 CRF：movie_write.cc:751-758 见 crf<0 就收起 -crf、改按码率走，
  // 那个码率就是下面 bitrate 这一格（新场景默认 0 = 不指定 = 交给编码器自己的码率控制）。
  int crf = 17;
  // 码率 = scene.render.ffmpeg.video_bitrate：单位 kbit/s（RNA 的说明文字
  // "Video bitrate (kbit/s)"，rna_scene.cc:7022-7026），DNA 的新场景默认 0
  // （DNA_scene_types.h:179）。导出只在「恒定码率」那一档读它（movie_write.cc:753
  // `bit_rate = video_bitrate * 1000`），填 0 就什么都不发。
  int bitrate = 0;
  // 编码速度 = scene.render.ffmpeg.ffmpeg_preset（eFFMpegPreset，DNA_scene_types.h:88-96）：
  // 良好 10（GOOD，RNA 的默认项）、最低 11（BEST）、实时 12（REALTIME）。
  int speed = 10;
  // 「无音频」= Blender 的 Audio Codec = NONE（rna_scene.cc:6967 "No Audio"，
  // “Disables audio output, for video-only renders”）：只出台风，不带音轨。
  // false = AAC（ffmpeg_audio_codec_items 的第二项，也是本工程的默认）。
  bool no_audio = false;

  double fps_value() const { return fps_base > 0 ? double(fps) / fps_base : 0.0; }
  // 名字不能叫 fps（和数据字段 frs_sec 同名会冲突）。非法帧率退回 25，
  // 与 Blender 的 scene->r.frs_sec 必须为正的前提等价。
  double fps_sec() const { double f = fps_value(); return f > 0 ? f : 25.0; }
  double frame_dur() const { return 1.0 / fps_sec(); }
  int64_t t_to_frame(double t) const { return int64_t(std::llround(t * fps_sec())); }
  // 「编码速度」这一档的两件事 —— 界面上的名字、导出命令行里的 -preset 字符串 —— 只在
  // 这一张表里各写一次。x264 那列就是 Blender 自己的映射（movie_write.cc:931-957：
  // GOOD→medium、BEST→slower、REALTIME→superfast），名字用需求里的中文。
  struct SpeedPreset {
    const wchar_t *label;
    const wchar_t *x264;
  };
  static SpeedPreset speed_of(int preset) {
    switch (preset) {
      case 11: return {L"最低", L"slower"};
      case 12: return {L"实时", L"superfast"};
      default: return {L"良好", L"medium"};
    }
  }
  SpeedPreset speed_preset() const { return speed_of(speed); }
  // 音频的名字按 eFFMpegAudioCodec 的两个在用的项来：AAC 与 "No Audio"（RNA 里后者
  // 排在第一项，说明文字是「只出台风、不带音轨」，见 rna_scene.cc:6967-6971）。
  // 行上的当前值和菜单里的两项都读这一句，所以「无音频」这个字符串全工程只有一处。
  static const wchar_t *audio_of(bool none) { return none ? L"无音频" : L"AAC"; }
  const wchar_t *audio_name() const { return audio_of(no_audio); }
};

// 某个时间线上「此刻的变换」。Blender 的 AnimSys 每帧把曲线求值结果写进
// Strip 本身（anim_sys.cc:574-597），所以后面所有读 transform 的代码看到的
// 都是覆盖后的值；本工程不改原数据，只把这个小结构交给唯一读它的绘制路径。
struct XformAt {
  double ofs_x, ofs_y, scale_x, scale_y, rotation;
};

inline XformAt xform_at(const Clip &c, const OutputFormat &f, double t) {
  XformAt x{c.ofs_x, c.ofs_y, c.scale_x, c.scale_y, c.rotation};
  if (c.anim[kChOfsX].empty() && c.anim[kChOfsY].empty() && c.anim[kChScaleX].empty() &&
      c.anim[kChScaleY].empty() && c.anim[kChRotation].empty())
    return x;  // 没有动画 ⇒ 与标量完全一致（Blender 里就是没挂 action 的条带）
  const double fr = double(f.t_to_frame(t));  // keyframe 的 x 轴 = 绝对时间线帧号
  double *dst[kAnimCount] = {&x.ofs_x, &x.ofs_y, &x.scale_x, &x.scale_y, &x.rotation};
  for (int i = 0; i < kAnimCount; i++)
    if (!c.anim[i].empty()) *dst[i] = c.anim[i].eval(fr);
  return x;
}

// Blender 的“缩放 %”真的改变输出尺寸：rectx = roundf(render_scale * scene->r.xsch)
// （editors/space_sequencer/sequencer_preview_draw.cc:135）。取偶数是为了 h264。
inline void render_size(const OutputFormat &f, int *w, int *h) {
  double p = std::clamp(f.scale_percent / 100.0, 0.01, 1.0);
  *w = std::max(2, int(std::lround(f.width * p)) & ~1);
  *h = std::max(2, int(std::lround(f.height * p)) & ~1);
}

// 画布形状 = 分辨率比 × 像素宽高比（Blender 的显示宽高比）。
inline double display_ar(const OutputFormat &f) {
  double num = double(f.width) * (f.par_x > 0 ? f.par_x : 1);
  double den = double(std::max(1, f.height)) * (f.par_y > 0 ? f.par_y : 1);
  return den > 0 ? num / den : 16.0 / 9.0;
}

struct Project {
  std::vector<Track> tracks;
  std::vector<Clip> clips;
  OutputFormat fmt;
  double duration = 0.0;  // 时间线总时长（秒）
  // 通道数。Blender 5.x 恒建满 128 个通道（channels.cc:30-40），可见行数只由区域高度决定；
  // 这里按需求默认只留 2 个，用「+/-」增减，区域变高时自动补齐到塞得下的行数。
  int chan_count = 2;
  // 播放头 = 时间线上的当前位置（秒）。归属在工程数据这一层，不在窗口外壳：
  // Blender 的 cfra 和 sfra/efra 写在同一行的同一个结构体里（RenderData，
  // makesdna/DNA_scene_types.h:866 的 `int cfra = 1, sfra = 1, efra = 250;`），
  // 而 RenderData 是 Scene 的成员 ⇒ 它和帧范围一起进 memfile、一起被撤消还原。
  // 「移动播放头不产生撤消步」说的是它不 push 新步，不等于它不进快照。
  double playhead = 0.0;
};
