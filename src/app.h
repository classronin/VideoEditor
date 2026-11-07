#pragma once
// VSE 剪辑器的应用外壳：区域布局、控件表、命令与属性表。
//
// 结构对齐 Blender 的编辑器分层：
//   space_sequencer.cc      —— area 的 region 划分（HEADER/CHANNELS/SCRUBBING/FOOTER/WINDOW/PREVIEW）
//   bl_ui/space_sequencer.py —— 两个 space header 与 footer 里的按钮、菜单内容
//   bl_ui/properties_output.py / properties_strip.py —— 属性区面板与行序
//   makesrna/intern/*.cc    —— 每个数值按钮的取值范围（见 prop_spec）
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "audio.h"
#include "export.h"
#include "preview.h"
#include "publish.h"
#include "render2d.h"
#include "retiming.h"
#include "theme.h"
#include "types.h"
#include "ui.h"

namespace app {

// 一次「按下 → 拖动 → 松开」。外壳只留这一格槽，谁被点中谁交出对象，松开即销毁；
// 按下之后的那些中间量（起点、按下时刻的快照、换算系数、抓取范围）全部住在手势里，
// 不在这里摊成一排字段。定义与全部分支见 gesture.h / gesture.cpp。
struct Gesture;

// ---------------------------------------------------------------- 区域标脏位
// Blender 的重绘粒度是 region（ED_region_tag_redraw）；没有 notifier 命中就不重绘。
enum Region : uint32_t {
  kRViewport = 1u << 0,
  kRLanes = 1u << 1,
  kRScrub = 1u << 2,
  kRProps = 1u << 3,
  kRTopbar = 1u << 4,
  kRStatus = 1u << 5,
  kRChan = 1u << 6,
  kRSplit = 1u << 7,  // 预览区与时间线区之间那条可拖的分界线自己那一格
};
inline constexpr uint32_t kRegionAll = 0xffu;

// 后台线程 → UI 的发布格（PublishSrc / ui_publish）在 publish.h：worker 侧的
// preview/audio/synctable 不引这一份 UI 状态表，只能各自 include 那一小份。

// ---------------------------------------------------------------- view2d 的滚轮/缩放步长
// 滚轮一格 = deltax = 40 * UI_SCALE_FAC 像素（view2d_ops.cc:469, :512, :514）。
// 缩放一格 = view_zoomfac_default = 0.0375（view2d_ops.cc:884）：view_zoomstep_apply_ex
// （:770-778）在 fac>=0 时取 dx = size*fac、两端各收缩 dx，cur 宽变成 size*(1-2*fac)，
// 于是像素/时间 = 1/(1-2*fac)；fac<0 时先除以 (1+2*fac) 再乘 fac，正好是同一个比例的倒数
// ——「进一格再退一格」回到原值。
inline constexpr double kScrollStepPx = 40.0;
// 通道总数上限：SEQ_sequencer.hh:48 `MAX_CHANNELS = 128`（channels.cc:30-40 一次性建满
// 129 项，序号 0 只占位不画，可用的是 1..128）。
inline constexpr int kMaxChannels = 128;
inline constexpr double kViewZoomFac = 0.0375;
inline constexpr double kZoomIn = 1.0 / (1.0 - 2.0 * kViewZoomFac);
inline constexpr double kZoomOut = 1.0 - 2.0 * kViewZoomFac;

// 时间线对「缩放」这一格的反应策略 —— 它不是数值上下限，是对象收到按键以后怎么动。
// Blender 给的也是策略：算子只把 cur 乘一个比例，之后 cur 的帧宽由对象自己收
// （view2d.cc:497 的 CLAMP(width, min[0], max[0])，钳的是**帧数**）。顺带把我先前的误读
// 记在这里：minzoom/maxzoom 那对数字（0.01/100，space_sequencer.cc:187-188）在本 region
// 全程不参与 —— keepzoom 只有 V2D_KEEPZOOM、没有 V2D_LIMITZOOM，所以 view2d.cc:471 的分支进
// 不去，view2d_ops.cc:804、:840 还显式跳过它；拿 minzoom 断言「Blender 也装不下这部片」
// 是读错了地方。
// 墙只有这一条，照抄 Blender 的两个数：视野不少于 min[0] = 10 帧（:181），不多于
// max[0] = MAXFRAMEF 帧（:184）。缩小这一头没有「装不下」：3 小时的片子 728,261 帧、
// 50 条排着队也都在 max[0] 以内，对象是多少算多少，UI 不许为「怕缩丢」给它立界
// （上一版的 avail/span 就是把视图后果当界，删了）。装得下全部内容以后视图停在原地不动：
// 本 region 的 keeptot = V2D_KEEPTOT_FREE（space_sequencer.cc:194），view2d.cc:627-719
// 那一整块「不让 cur 出 tot 的界 + 贴边/居中」外面套着 `if (v2d->keeptot)`，全程跳过，
// 既不屈中也不贴边（见 ops.cpp 的 timeline_view_react 第 ③ 条）。
// 形状要求（用户提的）：界不在按键里用。按键（滚轮 / Numpad± / HOME / 视图全部）只改
// 对象属性（pps、scroll_t 和一次「指针在哪儿、想钉住哪一刻」的请求），判断全在对象被用到
// 时做 —— ops.cpp 的 timeline_view_react，对应 Blender 的 view2d_get →
// view2d_curRect_validate（view2d.cc:388，绘制和取坐标都要先过这一关）。同一格按键落在
// 不同内容长度、不同窗口宽、指针在不在车道里，对象的反应可以不一样，也可以是没有。
inline constexpr double kViewMinFrames = 10.0;      // space_sequencer.cc:181
inline constexpr double kViewMaxFrames = 1048574.0;  // MAXFRAMEF，space_sequencer.cc:184

// 初始纵向视图 = 一屏 8.5 个通道：tot.ymax = 8.5、cur = tot（space_sequencer.cc:177-179）。
// 所以「一行多少像素」不是先验数字，而是条带区高 ÷ 8.5。这个数一旦定下，之后区高变了
// 行高不变、变的是可见行数（keepzoom = V2D_KEEPZOOM，view2d.cc:447-467 把 cur 的 y 尺寸
// 按新区高缩放，「像素/通道」正好保持原值）。
inline constexpr double kViewDefaultChannels = 8.5;

// ---------------------------------------------------------------- 弹出菜单编号
// 顶栏和 space header 的编辑器菜单、吸附/叠加 popover 按需求删掉后，只剩这几个入口。
enum MenuId : int {
  kMFramerate = 0,  // RENDER_MT_framerate_presets（属性区帧率行的下拉）
  kMQuality,        // ffmpeg.constant_rate_factor 的枚举下拉（需求留的三档）
  kMSpeed,          // ffmpeg.ffmpeg_preset「编码速度」的枚举下拉（RNA 的三项）
  kMAudio,          // ffmpeg.audio_codec 的枚举下拉（需求留的两项：AAC / No Audio）
  kMContext,        // SEQUENCER_MT_context_menu（时间线右键）
  kMPropKey,        // 属性行右键的关键帧条目
  kMRetiming,       // SEQUENCER_MT_retiming（I 键）
  kMDefault,        // 顶栏「默认」：保存启动默认 / 恢复启动默认
  kMPanelSide,      // 顶栏「面板」：面板左侧 / 面板右侧
  kMDownload,       // 顶栏「下载」：检测 FFmpeg / 下载 FFmpeg
  kMGrid,           // 顶栏「网络」：预览网格开 / 关
  kMPanel,          // 分界线右键：面板放左 / 放右 / 收起
  kMFont,           // 文本组的「字体」：DirectWrite 枚举出来的家族，逐行用它自己的字体画
  kMenuCount
};

// 自绘控件的命中 id 编码：绘制（draw.cpp）与输入分发（main.cpp）共用。
// 数值按钮用 Field id（0x1000 起），命令按钮/菜单/通道图标用下面的高位段，
// 保证互不重叠。这里是纯整数段（不是 enum），因为它们要和 Cmd / MenuId / ChanKind
// 这些 enum 做按位或。
constexpr int kHitCmd = 0x0010000,     // | Cmd
            kHitMenu = 0x0020000,      // | MenuId
            kHitChan = 0x0030000,      // | (track << 3) | ChanKind
            kHitPanel = 0x0040000,     // | 面板下标（折叠/展开，对应 Blender 点面板标题）
            kHitDeco = 0x0080000,      // | Field —— 属性行右侧那颗关键帧装饰点
            kHitLock = 0x0090000;      // | Field —— 位置XY/缩放XY 行上的成对锁定按钮（值 = 这一行的 Field）

// 上面那张码表的解码：谁造这个 id（draw.cpp 的控件）、谁拆它（main.cpp 的左键、ops.cpp 的右键），
// 用的都是同一对读法 —— 所以它跟着码表放在这里，不属于某一个 .cpp。
inline int hit_base(int id) { return id & ~0xffff; }
inline int hit_val(int id) { return id & 0xffff; }

// 通道头里的三个可点区域（sequencer_channels_draw.cc:264-286 的 draw_channel_headers）：
// 锁定图标、静音图标、轨道名（点名字不动作，双击/Ctrl+点 = SEQ_OT_rename_channel）。
enum ChanKind : int {
  kChanLock = 0, kChanMute = 1, kChanName = 2
};

// 滚动条的命中分区 = view2d_ops.cc:1834-1841 的 eV2DScrollerHandle_Zone。
// 源码用 y 朝上的「min=下端」说法，翻到屏幕坐标：min 把手 = 滑块的下沿，
// max 把手 = 滑块的上沿（只有时间线没锁纵向缩放，两个把手都能改行高）。
enum ScrollZone : int {
  kSQNone = -99, kSQMinOut = -2, kSQMin = -1, kSQBar = 0, kSQMax = 1, kSQMaxOut = 2
};

// 左侧单页里的分组（kHitPanel | 这个值）。自上而下就是需求列的顺序：
// 格式（分辨率 X/Y/% + 宽高比 X/Y + 帧率）→ 文本 → 变换 → 裁切 → 速度 → 帧范围 → 输出。
// 组内行序取自 properties_output.py:109-132（RENDER_PT_format 的四个 column）与
// properties_strip.py（变换、裁切）。裁切沿用 Blender 的 DEFAULT_CLOSED。
// 文本排在枚举末尾而不是插在中间：panel_open[] 在设置文件里按**下标**存，插进来会把老
// 用户的折叠状态错位。画在哪一组之后由 draw_props 的调用顺序决定，与这里的号无关。
enum PanelId : int {
  kPFormat = 0, kPXform, kPCrop, kPSpeed, kPRange, kPOutput, kPText, kPanelCount
};

// ---------------------------------------------------------------- 命令 id
// 对应 km_sequencer / km_sequencer_preview / km_frames / km_time_scrub 里的算子。
enum Cmd : int {
  kCmdNone = 0x100,
  // 文件（TOPBAR_MT_file 里只剩需求要的两个：打开、导出）
  kCmdImport, kCmdExport, kCmdQuit,
  // 顶栏「默认」下拉：只剩恢复启动默认；保存默认在关闭 exe 时自动做（settings_save）
  kCmdSettingsReset,
  // 顶栏「下载」：先查 PATH / 主程序同目录，再决定报一句还是开跑（ffmpegget.cpp）
  kCmdFFmpegCheck, kCmdFFmpegDownload,
  // 编辑
  kCmdUndo, kCmdRedo, kCmdCopy, kCmdPaste, kCmdSelectAll, kCmdSelectNone, kCmdSelectInvert,
  kCmdSelectLeftOfFrame, kCmdSelectRightOfFrame,
  // 时间线算子（sequencer.*）
  kCmdSplit, kCmdHardSplit, kCmdDelete, kCmdRippleDelete, kCmdDuplicate,
  // 新建文本条带（sequencer.text_strip_add，sequencer_add.cc:2114-2145）
  kCmdAddText,
  // 连接/断开片段（sequencer.connect / sequencer.disconnect，strip_connect.cc）
  kCmdConnect, kCmdDisconnect,
  kCmdMute, kCmdMuteUnselected, kCmdUnmute, kCmdLock, kCmdUnlock,
  kCmdGapRemove, kCmdGapRemoveAll, kCmdGapInsert, kCmdSnap, kCmdSlide, kCmdSlip, kCmdExtend,
  kCmdRippleTrimStart, kCmdRippleTrimEnd,
  kCmdSelectMore, kCmdSelectLess, kCmdSwapLeft, kCmdSwapRight,
  kCmdStripCenterPrev, kCmdStripCenterNext,
  // 变速（sequencer.retiming_segment_speed_set，km_sequencer 里就是纯 R）
  kCmdSpeedSet,
  // 变速菜单 SEQUENCER_MT_retiming（I 键，blender_default.py:3293）与它的条目、
  // 以及 sequencer.retiming_show（Ctrl+R，:3295）
  kCmdRetimingMenu, kCmdRetimingKeyAdd, kCmdRetimingShow,
  // 播放（km_frames：screen.frame_offset / frame_jump / animation_play）
  kCmdPlay, kCmdPlayCancel, kCmdFramePrev, kCmdFrameNext, kCmdJumpStart, kCmdJumpEnd,
  kCmdTimeBack, kCmdTimeFwd, kCmdKeyPrev, kCmdKeyNext, kCmdStripPrev, kCmdStripNext,
  // 帧范围：设入点/出点。Blender 的同类算子是 anim.previewrange_set（Ctrl+Alt+P，
  // blender_default.py:2096），它只改范围、不动当前帧；键位按需求换成 Ctrl+Home/End。
  kCmdRangeSetIn, kCmdRangeSetOut,
  // sequencer.set_range_to_strips（条带右键菜单，把帧范围收成选中条带的并集）。
  kCmdRangeToStrips,
  // 视图（sequencer.view_* / view2d.zoom_*）
  kCmdViewAll, kCmdViewSelected, kCmdViewFrame, kCmdZoomIn, kCmdZoomOut, kCmdZoomBorder,
  // VIEW_MT_preview_zoom 的比例档（view2d.zoom_set，12.5% … 800%）
  kCmdZoom125, kCmdZoom25, kCmdZoom50, kCmdZoom100, kCmdZoom200, kCmdZoom400, kCmdZoom800,
  kCmdBoxSelect,
  kCmdTlZoomIn, kCmdTlZoomOut, kCmdTlZoomAll,
  // 开关（wm.context_toggle）
  kCmdToggleSeconds, kCmdToggleScrub, kCmdToggleSidebar, kCmdRefresh,
  kCmdPanelLeft, kCmdPanelRight,  // 面板换边（顶栏「面板」与分界线右键两个入口共用）
  kCmdToggleGrid,  // 顶栏「网络」：预览画幅上的等分网格 + 中心十字（做跟踪时找画面中心）
  // 变换清零（sequencer.strip_transform_clear）
  kCmdClearPos, kCmdClearScale, kCmdClearRot, kCmdClearAll,
  // 属性行的关键帧菜单：ANIM_OT_keyframe_insert_button / _delete_button /
  // _clear_button（右键按钮呼出，interface_context_menu.cc:640-719）。
  kCmdKeyInsert, kCmdKeyReplace, kCmdKeyDelete, kCmdKeyClear,
  // 画幅预设
  kCmdAspect169, kCmdAspect916, kCmdAspect43, kCmdAspect11,
  // 帧率预设（scripts/presets/framerate/*.py，菜单项文字见 properties_output.py:76-88）
  kCmdFps6, kCmdFps8, kCmdFps12, kCmdFps2398, kCmdFps24, kCmdFps25, kCmdFps2997, kCmdFps30,
  kCmdFps50, kCmdFps5994, kCmdFps60, kCmdFps120, kCmdFps240, kCmdFpsCustom,
  // 导出质量档（scene.render.ffmpeg.constant_rate_factor 的枚举项，值 = eFFMpegCrf）
  kCmdCrfNone, kCmdCrfLossless, kCmdCrfPercLossless,
  // 编码速度（scene.render.ffmpeg.ffmpeg_preset，值 = eFFMpegPreset）
  kCmdSpeedBest, kCmdSpeedGood, kCmdSpeedRealtime,
  // scene.render.ffmpeg.audio_codec 的两档：AAC 与 NONE（RNA 第一项 "No Audio"）
  kCmdAudioAac, kCmdAudioNone,
  // 窗口（TOPBAR_MT_window 里本工程真能做的三项：wm.window_fullscreen_toggle /
  // screen.show_statusbar / screen.screenshot）
  kCmdToggleFullscreen, kCmdToggleStatusbar, kCmdScreenshot,
  // 面板顶部那两颗种类按钮：亮的那颗读 a.panel_kind（这里存的，不是从选中条带算的）。
  // 点下去 = 面板换到那一种，时间线里有这一种的带就顺手选中最早的那条；没有也照换，
  // 因为文本组的落点会退到 a.text_def（新建文本带继承的那份），不靠拒绝点击来骗人。
  kCmdPickVideoStrip, kCmdPickTextStrip,
  // 文本三层的色块：点下去开系统的选色框（comdlg32 的 ChooseColorW，OS 自带）。
  // 下标就是 text_layer() 的层号：0 字形 / 1 轮廓 / 2 方框。
  kCmdColorGlyph, kCmdColorOutline, kCmdColorBox,
  // 「字体」下拉：家族是枚举出来的，条目数不写死，所以 id 从这个基数往上排。
  kCmdFontFirst = 0x800,
};

// ---------------------------------------------------------------- 数值属性 id
// 每个 id 对应 Blender 的一个 RNA 属性，取值范围照抄 rna_*.cc 的 ui_range。
enum Field : int {
  kFNone = 0x1000,
  // 项目级：scene.render（rna_scene.cc:7351-7441）+ scene.r.sfra/efra
  // （rna_scene.cc:7345 起，PROP_INT range(1, INT_MAX)；本工程时间从 0 帧起算）。
  kFResX, kFResY, kFResPct, kFAspX, kFAspY, kFFps, kFFpsBase,
  kFFrameStart, kFFrameEnd, kFFrameStep,
  // scene.render.ffmpeg.video_bitrate（rna_scene.cc:7022-7026，单位 kbit/s）
  kFBitrate,
  // 项目级那一段的末尾：prop_apply 用它分辨「改的是工程还是条带」。再加工程级属性要加在
  // 这一行之前，加在后面就会被当成条带属性。
  kFProjectLast = kFBitrate,
  // strip.transform（rna_sequencer.cc:2276-2317）
  kFOfsX, kFOfsY, kFScaleX, kFScaleY, kFRotation, kFOriginX, kFOriginY,
  // strip.crop（rna_sequencer.cc:2209-2231）
  kFCropMinX, kFCropMaxX, kFCropMinY, kFCropMaxY,
  // 条带速度（百分数）：SEQUENCER_OT_retiming_segment_speed_set 的 speed 属性
  // （sequencer_retiming.cc:701-709，默认 100、min 0.001、softmin 0.1、max FLT_MAX）。
  kFSpeed,
  // 布尔开关（不是数值按钮，走 prop_toggle）：
  //   strip.use_flip_x / use_flip_y = Strip::flag 的 SEQ_FLIPX/SEQ_FLIPY（:3187-3195），
  //   在 Transform 面板的 Mirror 一栏里成对出现（properties_strip.py:1036-1038）。
  //   strip.mute / strip.lock = SEQ_MUTE / SEQ_LOCK（:2674-2686）。
  kFFlipX, kFFlipY, kFMute, kFStripLock,
  // 文本条带的样式（TextVars，见 types.h）。三段各自连成一片，判断都用区间：
  //   粗体/斜体接在布尔那段后面，和镜像走同一个 prop_toggle；
  //   内容是唯一的字符串行（回车换行、Ctrl+回车提交，点到框外也提交），不拖也不按数值解析；
  //   再往后是数值档。字号..圆度不能插进 kFOfsX..kFRotation，那一段的 id 减一下标
  //   就是动画曲线的通道号。
  kFTextBold, kFTextItalic,
  kFTextContent,
  kFTextSize, kFTextLineSpacing, kFTextOutlineWidth, kFTextBoxMargin, kFTextBoxRoundness,
  // 三层的颜色各四格：字形 / 轮廓 / 方框。RGB 按 0..255 显示、不透明度按百分数，
  // 落进对象都折回 TextVars 那份 float[4] 的 0..1。轮廓和方框的 alpha 就是它们的开关
  // （排版器读 alpha 判关，见 text.cpp），所以 0% = 这一层不出。
  kFGlyphR, kFGlyphG, kFGlyphB, kFGlyphA,
  kFOutlineR, kFOutlineG, kFOutlineB, kFOutlineA,
  kFBoxR, kFBoxG, kFBoxB, kFBoxA,
};

// Field 里属于「点一下就翻转」的布尔项（数值拖动/文本输入那条路对它们没意义）。
inline bool is_bool_field(int id) { return id >= kFFlipX && id <= kFTextItalic; }
// 文本带才答的行（内容 + 数值 + 颜色）。别的条带类型的对象里没有这份数据，
// prop_apply 遇到它们就原样退回：一个字都不改，状态条说为什么。
inline bool is_text_field(int id) { return id >= kFTextContent && id <= kFBoxA; }
// 字符串行：缓冲区是内容本身，不是待 wcstod 的数字。
inline bool is_string_field(int id) { return id == kFTextContent; }
// 十二格颜色（三层 × 四通道）：id 连着排，行列由下面两个函数算，读写和画色块共用同一份换算。
inline bool is_color_field(int id) { return id >= kFGlyphR && id <= kFBoxA; }
// 文本那三层颜色的数组：0 字形 / 1 轮廓 / 2 方框。下标在 Cmd（kCmdColorGlyph…）和
// Field（kFGlyphR…）两头都是这个顺序，所以它只在这里定义一次。
inline float *text_layer(TextVars &t, int layer) {
  return layer == 0 ? t.color : (layer == 1 ? t.outline_color : t.box_color);
}
inline const float *text_layer(const TextVars &t, int layer) {
  return text_layer(const_cast<TextVars &>(t), layer);
}
// 一格颜色 = 哪一层、哪个通道、界面上按几档显示（alpha 是百分数，其余 0..255；对象里都是 0..1）。
inline void text_color_at(int id, int *layer, int *ch, double *scale) {
  const int d = id - kFGlyphR;
  *layer = d / 4;
  *ch = d % 4;
  *scale = *ch == 3 ? 100.0 : 255.0;
}
// 颜色行的行号（0 字形 / 1 轮廓 / 2 方框）：面板画那一行、选色框改那一层，都问它。
inline int text_color_row(int id) { return (id - kFGlyphR) / 4; }

struct PropSpec {
  double soft_min, soft_max;  // Button::softmin/softmax
  double hard_min, hard_max;  // RNA 的真实区间
  double step;                // ui_range 的 step
  int precision;              // ui_range 的 precision；整数属性按 %d 显示
  bool is_float;
};

struct TrackRow {
  int track = 0;  // project.tracks 的下标
  float y = 0, h = 0;
};

// 时间线的视图数据 = Blender 的 SpaceSeq + 它的 view2d 那几样（不是数据内容，不进快照）。
// 单独立成一个对象而不是摊在窗口外壳上：这几样永远一起变、一起被读（缩放改 pps、平移改
// scroll_t/view_ch、行高改 ch_px），摊开就会和别的零散字段互相借用，上一个手势没清干净的
// 数会被下一个手势读到。
// 它只是数据：鼠标/键盘不许绑在这里，也不许由它决定动作。谁被点中、怎么反应，归被点中的
// 那个对象自己回答（见 main.cpp 的手势分发）。时间线占多高（tl_px）不在这里：那是排版，归窗口。
struct TimelineView {
  double pps = 60.0;      // view2d 缩放：像素/秒
  double scroll_t = 0.0;  // 视口左端时刻
  // 「缩放」这一格写下的请求：指针在 (zoom_req_x, zoom_req_y)，改属性之前它底下是
  // zoom_req_t 这一刻。请求不等于决定 —— 对象在 timeline_view_react 里看指针在不在
  // 自己（车道）里面，才决定要不要把它钉回去；请求用完即清。
  bool zoom_req = false;
  float zoom_req_x = 0.0f, zoom_req_y = 0.0f;
  double zoom_req_t = 0.0;
  // 纵向视图 = 时间线 window 区的 view2d 之 y 轴。单位是「通道」，0 基：
  // 通道下标 t 占 <t, t+1>（Blender 是 1 基的 <c, c+1>，见 strip_time.cc:321-340）。
  double view_ch = 0.0;  // cur.ymin（区底边那条通道的位置）
  float ch_px = 0.0;     // 一个通道占多少像素 = 1/zoomy；0 = 还没初始化
};

// 撤消栈里的一步。Blender 的撤消是「整份 main 数据库的快照链」（blenkernel/intern/
// undo.cc：算子 exec 之后 ED_undo_push 写一份 memfile，步数 = user.undosteps，
// DNA_userdef_types.h:1144 的默认值 32，RNA 上限 256），粒度就是一个算子。
// 这里等价：一次用户动作结束时把工程数据 + 选择整套抄下来。播放头住在 Project 里，
// 所以它跟着快照走：Blender 的 cfra 在 RenderData（DNA_scene_types.h:866，和 sfra/efra
// 同一行），整份 Scene 进 memfile ⇒ 撤一步就回到那一步当时的当前帧。「拖播放头不产生
// 撤消步」说的是它不 push 新步，不是它不进快照。缩放、滚动是视图，不是数据，不在这里。
struct Snapshot {
  Project project;
  std::vector<int64_t> sel;
  // 变速键的下标是「那一份曲线里的第几把」，所以曲线和选择必须成对存回，
  // 否则撤消以后下标会指到别的键上。
  std::vector<std::pair<int64_t, size_t>> retime_sel;
  int64_t selected = -1;
  std::wstring name;  // 产生这一步的算子名（状态栏里「已撤消：xxx」用）
};

// view2d 边缘平移的数据，逐字段照抄 UI_view2d.hh:530-566 的 View2DEdgePanData，
// 参数来自 transform_convert_sequencer.cc:42-47（seq_slide 专用）。
struct EdgePan {
  double start_x = 0.0;  // 进入左/右滚动区的时刻，0 = 不在区内（延时计时器的起点）
  double last = 0.0;     // 上一次应用增量的时刻
  bool enabled = false;  // 光标必须先进过区内一次，才允许把视图推出边界
  double scroll0 = 0.0;  // 进入模态时的视图（view2d_edge_pan_cancel 要还原它）
};

struct ModalState {
  enum Kind : uint8_t { None, Slide, Slip, Extend, RetimeKey } kind = None;
  double t0 = 0.0;             // 起始鼠标时刻
  float y0 = 0.0f;             // 起始鼠标 y（seq_slide 的通道偏移原点）
  std::vector<Clip> orig;      // 起始数据快照
  EdgePan edge;                // 只有 Slide（seq_slide）会用到
  // 只有 RetimeKey 用到。一次拖拽里所有被选中的键共用同一个位移，所以「不许越过相邻键」
  // 这条限位在进入模态时按全部被拖的键算一次、取最紧的那个（create_trans_seq_clamp_data，
  // transform_convert_sequencer_retiming.cc:110-175：xmax/xmin 对所有键取 min/max）。
  double kmin = -1e18, kmax = 1e18;
  int64_t grab_id = -1;        // 手指按住的那把键属于哪条带（吸附以它的位置为源）
  size_t grab_key = 0;
};

struct App {
  HWND hwnd = nullptr;
  Render2D r2d;
  Preview preview;
  AudioPlayer audio;
  Exporter exporter;
  Project project;
  std::vector<Clip> clipboard;
  int64_t next_id = 1;
  int64_t selected = -1;         // 活动条带（active_strip）
  std::vector<int64_t> sel;      // 选中集合
  // 选中的变速键 = Blender 的 Editing::retiming_selection（全局一份，跨条带）。
  // Blender 存的是 SeqRetimingKey 指针，本工程 retiming 是 vector（一改就重分配），
  // 所以存 (条带 id, 键下标)；任何改动键数组的操作都要顺手清掉它。
  std::vector<std::pair<int64_t, size_t>> retime_sel;

  // 撤消链。undo[undo_cur] 恒等于当前数据（= Blender 的「memfile 链 + 现在这份 main」），
  // 所以第 0 份是启动基线，撤到底就是回到它。
  std::vector<Snapshot> undo;
  size_t undo_cur = 0;

  TimelineView tl;               // 时间线这一块「怎么看」：缩放、横向/纵向滚动、行高
  // 时间线区的高度（像素，含标尺带与底部留白；0 = 还没拖过分界线，由 layout 按窗口 body
  // 高的比例给默认）。
  // 拖预览与时间线之间那条分界线改的就是它（wm_dragarea 的等价物）。
  float tl_px = 0.0f;
  // 属性面板的宽度（像素；0 = 还没拖过分界线，由 layout 每帧现取内容宽度）。
  // 拖面板与主区之间那条竖分界线改的就是它（screen_ops.cc 的 region_scale 模态，
  // 横向边 ⇒ WM_CURSOR_X_MOVE，:3599）。
  float props_px = 0.0f;
  // 属性面板这一块 region 的纵向滚动量（像素，>=0）。滚轮和拖动滑块只写这一个数；
  // 「内容比 region 高多少、这一格到底滚不滚」由画的那一趟自己判断（draw.cpp 的 draw_props）。
  float props_scroll = 0.0f;
  // 整页内容的总高。它由 draw.cpp 的面板规格表（哪一组开着、每组几行）算出，每帧画面板
  // 之前先更新，所以「有没有滚动条、行宽让不让位、滑块多长、能不能滚到底」都读同一个当帧
  // 的数 —— 点折叠那一帧就对齐，不用等下一趟。
  float props_content_h = 0.0f;
  // 设置文件里的窗口状态：还原态矩形 + 当时是否最大化。win_saved = 文件里有这么一项，
  // wWinMain 建窗口时用它代替「屏幕 80% 居中」；没读到就一切照出厂默认。
  bool win_saved = false;
  bool win_max = false;
  int win_x = 0, win_y = 0, win_w = 0, win_h = 0;
  // PATH 与 exe 同级目录里有没有 ffmpeg.exe（启动时定，下载完成后刷新）。
  // 没有的话「设置」里多出三条下载源。
  bool has_ffmpeg = true;
  // 播放头不住这儿：它是时间线上的位置，归 project（见 types.h 的 Project::playhead）。
  bool playing = false;
  double last_tick = 0.0;
  // 15ms 的 tick 此刻是否存在。只有 clock_sync（main.cpp）会写它：那里算一遍「有没有东西
  // 在按时间推进」，和这一格比，只在跨界时调 SetTimer/KillTimer。空闲时没有时钟，
  // 消息循环就阻塞在 GetMessage 上，整机 0 消耗。
  bool tick_armed = false;
  // 已经「烘完一笔」的波形条数，用来看有没有新数据要重画（WaveCache::done()）。
  int wave_done = 0;

  // 预览区域的 view2d
  float view_zoom = 1.0f;
  float view_pan_x = 0.0f, view_pan_y = 0.0f;

  // region 矩形（layout 计算）
  Rect topbar, viewport, split, chan, scrub, lanes, tl_area;
  Rect props, status;

  // ui 层
  ui::Block blk;
  ui::Popover pop;
  ui::NumDrag ndrag;
  ui::MenuTable menus;
  int hover = 0;
  // 竖分界线本身不是控件（不编码进 blk），它的高亮与 X_MOVE 光标各自记在这里。
  bool props_div_hot = false;
  // 预览/时间线之间那条横线同样不是控件：悬停时点亮 + S_MOVE 光标，否则那 3px 命中带
  // 在屏幕上找不到（线的原色比上下两块底还暗）。
  bool split_div_hot = false;
  // 右键属性行时记下那一行的 Field id，菜单项就作用在它上面。
  // Blender 用「鼠标悬停的按钮」找属性（keyframing.cc:1326 的 context_active_but_prop_get），
  // 不能用 hover：呼出菜单后 hover 会被菜单条目抢走。
  int key_field = 0;
  int edit_id = 0;               // 正在输入的数值按钮
  std::wstring edit_text;
  // 输入框里的光标与选区（Blender TextField 的 cursor / select_x 那一对）。
  // 进入输入态 = 全选（caret 在串首、sel 在串尾），第一个字符先吃掉选区——
  // 这就是原来「edit_fresh 整体替换」的同一件事，但现在选区能用鼠标点断、拖出来，
  // 也能用方向键移动，所以不再需要一个专门的「还没动过」标志。
  // 数值框和通道重命名框共用这一份：同一时刻只有一个框在输入。
  ui::TextSel edit_ts;
  // >= 0：左键按在输入框里没松开，正从 anchor 这个字符边界往外拖选区。
  int edit_anchor = -1;
  int rename_track = -1;         // 正在重命名的轨道下标（SEQ_OT_rename_channel）
  std::wstring rename_text;
  // 面板每一组标题左边那颗小三角：true=展开、false=只留标题。Blender 的裁切面板写的是
  // bl_options = {'DEFAULT_CLOSED'}（properties_strip.py:118），但需求把「裁切：左/右/顶/底」
  // 列成左侧必须有的一块内容，折叠起来等于没有，所以这里默认展开。
  // 关掉 exe 时这份状态会进设置文件的 panel 行（见 settings_save）。
  bool panel_open[kPanelCount] = {true, true, true, true, true, true, true};
  // 面板此刻演的是哪一种（kStripText 或别的）。亮的那颗按钮、哪几组出现、文本行落到哪份
  // TextVars 三处都只读这一格。写它的是两笔：种类按钮按下去无条件换，选中一条带时选中也
  // 把它写成那条带的种类 —— 两个来源写的是同一个意思，不是同一个值存两处。
  int panel_kind = kStripMovie;
  // 文本参数的模板：面板演文本这一种、而选中的又不是文本带时，文本那几行写的就是这一份；
  // 新建的文本带从它抄起（见 ops.cpp 的 add_text）。它是默认值而不是工程数据，所以不进
  // 撤消快照，但关 exe 时进设置文件的 text 行（见 settings.cpp），活得过重启。
  TextVars text_def;

  // 开关（对齐 RNA）。吸附与叠加层的分项开关按需求不做：吸附候选集固定为设计稿那两条
  // 规则（见 snap_time），时间线网格与条带文字恒画，所以这里只剩区域/显示本身的开关。
  bool draw_frames = false;      // sseq->flag & SEQ_DRAWFRAMES（默认关 = 显示时间码）
  bool show_scrub = true;        // space_data.show_region_scrubbing
  bool show_statusbar = true;    // screen.show_statusbar（TOPBAR_MT_window 里的那一项）
  bool fullscreen = false;       // wm.window_fullscreen_toggle
  bool sidebar_on = true;        // space_data.show_region_ui
  int props_side = 0;            // 面板在哪一边：0=左（需求默认），1=右（Blender 的 UI 区原位）
  // 预览画幅上的等分网格（顶栏「网络」）：默认开。做跟踪运动时画面中心点没有别的依据，
  // 靠它定位；它不进时间线，也不进导出。
  bool preview_grid = true;
  // 变换 XY 成对锁定（需求自定：Blender 的 transform 属性之间没有任何联动，
  // rna_sequencer.cc:2267-2317 里 offset_x/y、scale_x/y 是四个独立浮点）。
  // 缩放锁定 = 两轴同一个值；位置锁定 = 两轴同一个增量（「X=Y」对位移没有意义，
  // 位移能锁的只有「一起走多远」）。默认全锁：改一个轴不会把另一个甩掉。
  // 预览里拖取景框 = 二维操作，两轴本来就要一起写，所以它绕过位置锁（见 prop_apply 的 pair）。
  bool lock_pos = true, lock_scale = true;

  // 指针交互：外壳这一层只剩「此刻哪一次按下正在进行」这一格槽。
  // 谁被点中、这一次按下要记住哪些中间量（起点、按下时刻的快照、换算系数、抓取范围），
  // 全写在手势对象里（gesture.h），松开就整份销毁。所以：下一个手势读不到上一个手势没清
  // 干净的数，上一个手势也不可能被下一个手势改写 —— 这类互相借用是有名字的状态才能防的病。
  std::shared_ptr<Gesture> gesture;
  // 条带本体的拖动越过阈值以后交给自己，此后归模态：modal = Blender 的
  // transform.seq_slide 模态，鼠标拖和 G 键走同一条路，Esc 都能还原，快照自带。
  ModalState modal;
  // sequencer.select_box（B）：按下 B 后，下一次左键拖拽 = 框选（Blender 里 B 直接 invoke 该算子）。
  bool box_armed = false;
  int mouse_x = 0, mouse_y = 0;
  bool ldown = false;

  uint32_t dirty = kRegionAll;
  std::wstring status_text = L"Ctrl+O 导入视频，或把视频文件拖进窗口";

  // 自动化验证
  std::wstring shot_path;
  double shot_deadline = 0.0, shot_hold = 0.0;
  // --shot 截完退出；窗口 ▸ 保存截图（screen.screenshot）截完留在原地。
  bool shot_quit_after = true;
  bool export_quit = false, export_reported = false, audio_err_shown = false;
  // --export-delay 秒：等到这一刻再启动导出。手势脚本（--shot/click_shot）是窗口建好之后
  // 才注入的，而导出默认在进消息循环前就跑完了 —— 拖完键再导出的最终曲线根本来不及导出。
  double export_at = -1.0;
  std::wstring export_path;
  // 本次会话第一次「打开文件」的那个文件所在目录，导出对话框就从这里起步。
  // 只认第一次（之后再开别处的文件也不改），关掉窗口即失效 —— 不进设置文件。
  std::wstring open_dir;

  // ---- 便捷访问
  double fps() const { return project.fmt.fps_sec(); }
  double frame_dur() const { return 1.0 / fps(); }
  int64_t cur_frame() const { return project.fmt.t_to_frame(project.playhead); }
  void set_frame(int64_t f) { project.playhead = double(f) / fps(); }
  int64_t t_to_frame(double t) const { return project.fmt.t_to_frame(t); }
  double frame_to_t(int64_t f) const { return double(f) / fps(); }
};

inline App &app_ref() {
  extern App g_app;
  return g_app;
}

// 修饰键的归一化码：main.cpp 的 mod_state() 把 Windows 的 GetKeyState 翻成这三个位，
// 之后「按住的是什么」就是应用自己的词汇，任何对象都能问，不必再碰 Win32。
constexpr WORD kShift = 1, kCtrl = 2, kAlt = 4;

// ---------------------------------------------------------------- 几何 / 视图
void layout(App &a);
float channel_h(const App &a);
// 属性面板与主区之间那条竖分界线（region_scale 的横向边，screen_ops.cc:3495-3600）：
// 拖它改的是面板宽度 props_px。上下限按 Blender 的思路给：本区要还画得出一列控件，
// 隔壁主区要还留得下通道头 + 一点条带区。
float props_w_min();
float props_w_max(float win_w);
// 面板里所有参数标签里最长那条的字宽（draw.cpp 画标签时逐条量出来累加的，见 props_label_w
// 的定义处）。面板默认宽度和每行的分栏位置都读这一个数。
float props_label_w();
// 没人拖过分界线时的面板宽度 = 内边距×2 + 标签列 + 数值列 + 滚动条那一槽：宽度刚好包住
// 所有参数，槽那一列本来就另算（条画不画都占着）。
float props_w_default();
// 收起后可抓的那条边的宽度（= 画出来的手柄宽，见 props_band 的定义处）。
float props_band();
// 面板与主区之间那条竖边的 x：面板开着时是它的内侧边（放左就是右缘、放右就是左缘），
// 收起时退到窗口的最外一格。
float props_edge(const App &a);
// 命中分界线本身那几像素（面板开着时只贴边 3px）；面板收起时整条手柄带都算命中。
bool props_divider_at(const App &a, float fx, float fy);
// 属性面板这一块 region 的纵向滚动条。槽（内容让开的那一列）恒在，条只在内容比 region 高
// 时才画、时才存在。轨道、滑块、命中都只从 props_scroll / props_content_h 这两个数换算，
// 别处不再另存一份。
float props_scroll_slot();
Rect props_scroll_trough(const App &a);
Rect props_scroll_thumb(const App &a);
// 预览/时间线之间那条横分界线的命中带（上下各 3px）：悬停点亮、按下开手势都走这一条。
bool split_divider_at(const App &a, float fx, float fy);
// 纵向 tot 的上边界（0 基通道单位）：声明的通道数、条带占到的最高一格、当前塞得下的行数取大者。
double timeline_tot_hi(const App &a);
// 横向 tot = 对象（条带把手）排出来的队，不含帧范围（它只管播放与导出）；
// 没有条带时才退回 region 初始化的 [0, efra]。
double timeline_tot_end(const App &a);
double timeline_tot_start(const App &a);
// 同上。对象被「用到」（绘制、读坐标）时的反应：按键那一头只改属性（下面的 pps /
// scroll_t / zoom_req_*），要不要动、怎么动在这里判断。界（clamp_timeline_pps）是
// 这个对象自己的私事，不暴露给按键。
void timeline_view_react(App &a);
// view2d.cc:737-759（cur 平移着夹进 tot）+ :797-802（V2D_ALIGN_NO_NEG_Y）。
void clamp_view_ch(App &a);
// 右侧滚动条的轨道矩形（view2d.cc:175-199 + :1385-1403 的 edge_pad/AZONESPOT 内缩）。
Rect scroll_trough(const App &a);
// 滑块在轨道里的像素区间（view2d.cc:1449-1483）。
Rect scroll_thumb(const App &a);
// 可见通道数 = cur 的 y 尺寸（一个通道 = channel_h 像素）。
double cur_channels(const App &a);
// 滚动条命中分区（view2d.cc:1988-2015 + view2d_ops.cc:1848-1878）；没落在轨道里 = kSQNone。
// 按下之后怎么换算（fac / fac_round / vsm->lasty）是「这一次按住」的事，归 gesture.h 的 ScrollBar。
int scroll_zone_at(const App &a, float fx, float fy);
const std::vector<TrackRow> &track_rows(const App &a);
int track_at_y(const App &a, float y);
float time_x(const App &a, double t);
double x_time(const App &a, float x);
Rect fit_box(const App &a);
Rect view_rect(const App &a);
Rect clip_base_rect(const App &a, const Clip &c, const Rect &box);
// 条带那一幅「源画面」有多大，单位 = 输出像素：影片问 probe，文本问排版器。
// 定画面尺寸、摆取景框四角、把裁切量换成纹素坐标三处都读这里 —— 源尺寸在别处再算一遍
// 就是两个所有者，会出现「框和画各走各的」。
void clip_source_size(const App &a, const Clip &c, int *w, int *h);
// 1 输出像素在屏幕上占多少像素，横竖各一个（画面上那两行位移的单位是「格式面板的分辨率」）。
// 预览里画画面、算取景框四角、把鼠标拖动换成位移三处都读它，所以只有这一个算法。
struct ScreenPx {
  float x, y;
};
ScreenPx output_to_screen(const App &a, const Rect &box);

// ---- 预览取景框 ---------------------------------------------------------------
// 取景框 = 每一条片段自己的画面四角（不是画幅）：播放头下有两条片段就有两只框，
// 各自独立。拖框内 = 改这一条的 位置X/Y，拖角 = 改这一条的 缩放X/Y（保持当前宽高比，
// 支点是 transform.origin，与 Blender 的 resize 把手同一个锚）。写的是真数据：
// 有曲线就在当前帧打键、进撤消栈、导出跟着变。声音条带没有画面 ⇒ 没有框
// （iterator.cc:224 的 collection_filter_rendered_strips 本来也不把它算进合成）。
// 视图本身的缩放/平移仍走滚轮与中键（view2d.zoom / view2d.pan），和框无关。
// 角把手的抓取半径：画的和点的是同一个数，能看见的小方块就是能抓的小方块。
inline float framing_handle_r() { return 8.0f * btheme::scale; }
// 条带四角 + 支点在屏幕上的位置。角序与 Blender 的 image_transform_quad_get
// （strip_transform.cc:885-901）一致：0=右上 1=右下 2=左下 3=左上。
// pivot = transform.origin 那一点，缩放与旋转都绕它，所以它在 scale 前后不动。
void clip_screen_quad(const App &a, const Clip &c, const Rect &box, float *pts, float *pivot);
// 按下命中与拖动的每一格：谁家的框被点中，谁家交出一个 FramingDrag，那一次按下的快照由它
// 自己拿着（声明见 gesture.h，实现见 ops.cpp）。一只框都没命中 = 裸左键不动作。

void view_zoom_at(App &a, float px, float py, float factor);
// view2d.zoom_set：把预览的缩放直接设成某个百分比（100% = 适配窗口）。
void view_zoom_set(App &a, float percent);
void view_reset(App &a);
void zoom_at(App &a, float px, float py, double factor);
void timeline_view_all(App &a);  // sequencer.view_all：适配对象的 tot（帧范围 ∪ 条带把手）
void zoom_to_selected(App &a);

// ---------------------------------------------------------------- 撤消 / 重做
// ed.undo / ed.redo（Ctrl+Z / Ctrl+Shift+Z，blender_default.py:839-840）。
void undo_reset(App &a);            // 重建整条链（启动、载入新工程）
void undo_push(App &a, const wchar_t *name);  // 一次动作结束；数据没变则不产生新步
bool undo_step(App &a);
bool redo_step(App &a);

// ---------------------------------------------------------------- 数据
Clip *find_clip(App &a, int64_t id);
Clip *props_clip(const App &a);      // 面板作用的条带（活动条带，退化为播放头下的条带）
bool clip_selected(const App &a, int64_t id);
std::vector<Clip *> selected_clips(App &a);
Clip *clip_at(const App &a, double t);
// 播放头下所有参与画面合成的条带，按通道自下而上排好 —— 叠画的顺序与解码目标都由它给。
std::vector<Clip *> rendered_clips(const App &a, double t);
// 播放头下所有该发声的条带，按通道自下而上排（混音与顺序无关，排只是为了日志好读）。
std::vector<Clip *> sound_clips(const App &a, double t);
double project_end(const App &a);
void playback_bounds(const App &a, double *t0, double *t1);
void recompute_duration(App &a);
void select_only(App &a, int64_t id);
void add_to_selection(App &a, int64_t id);
void toggle_selection(App &a, int64_t id);
void clear_selection(App &a);

// ---- 变速键的选择（Blender 的 Editing::retiming_selection，全局一份）----
bool retiming_keys_are_selected(const App &a);
bool retiming_key_is_selected(const App &a, int64_t clip_id, size_t key);
void retiming_clear_key_sel(App &a);
// 摘掉指向不存在的条带/不存在的下标的选择项（曲线被重排、条带被删之后调用）。
void retiming_prune_sel(App &a);
// 照 select_key（sequencer_retiming.cc:834-857）：先按 deselect_all 清空，
// 再「Shift 且已选中 ⇒ 移除，否则加入」。
void retiming_key_select(App &a, int64_t clip_id, size_t key, bool toggle, bool deselect_all);
// SEQUENCER_OT_retiming_key_delete（:501-534）：只处理选中的键。没有键被选中 ⇒ 返回
// false，X/DEL 于是继续走到条带删除（Blender 那两个绑定靠 retiming_poll 分流）。
bool retiming_delete_selected_keys(App &a);
// 键的命中层。Blender 在条带选择之前先问它（sequencer_select.cc:1053-1097）：命中 ⇒ 选好键
// （并按 :1079 清掉条带选择）返回 true；没命中 ⇒ 清掉键选择返回 false，这一击交回条带选择。
// 返回 true 时，手指按住的那把键写在 out_id / out_idx 里 —— 只有「点住它往外拖」的那一种
// 点击（不按 Shift）才给，Shift 那一支只做增删不 invoke 变换（sequencer_retiming.cc:917-925）。
bool retiming_key_pick(App &a, float fx, float fy, int64_t *out_id, size_t *out_idx);

void trace(const std::wstring &s);
// 变速曲线状态一行日志（ops.cpp），键的增删改都打一条，读它比量像素准。
void trace_retime(App &a, const Clip &c, const wchar_t *tag);
// 连接组名单一行日志（哪些条带互相指着 + 当前选中集合）。
void trace_links(App &a, const wchar_t *tag);
extern FILE *trace_fp;         // VSE_TRACE 指定的日志文件（ops.cpp 定义）
double now_seconds();
void set_status(App &a, const std::wstring &s);
std::wstring time_str(const App &a, double t);
// 吸附到标记/条带边界（tool_settings.use_snap_sequencer，Shift+Tab 切换）。
// 被吸附的东西分两种，候选集与优先级按需求各定一套：
//   kSnapStrip = 拖片段（吸附对象是片段的起止边，播放头优先）；
//   kSnapFrame = 拖播放头（吸附对象是播放头本身，帧范围端点优先）。
enum SnapWhat { kSnapStrip = 0, kSnapFrame };
double snap_time(App &a, float x, double t, const Clip *self, SnapWhat what = kSnapStrip);
// 拖片段时的吸附：起止两条边都算，取贴得更近的那一条。
double snap_strip_move(App &a, const Clip &c, double new_start);
// 修饰键的实时状态（拖动过程中每帧都要问一次，不是按键消息里才拿得到）。
bool ctrl_held();
bool shift_held();  // 同上（框选要不要并入已有选择，由框这个对象在落地那一刻自己问）
const PropSpec &prop_spec(int id);
double prop_value(App &a, int id, Clip *c);
std::wstring prop_text(App &a, int id, Clip *c);
// 文本那几行落到哪一份 TextVars：选中的是文本带就是它自己，否则是 a.text_def。
// 读、写、画色块三处都只经过这里 —— 面板演文本这一种时没有「带」可写是常态（时间线里
// 还没有文本带），落点必须由这一处回答，不许每行自己判一遍。
TextVars &text_target(App &a, Clip *c);
// 字符串行的读数（只有内容那一行）。它和 prop_text 是两个出口，不是一个值的两处写法：
// prop_text 的产物是「一个数格式化后的串」，把整段文字塞进那条路要先编一个数出来。
std::wstring prop_string(App &a, Clip *c);
// 内容那一行的写入口，和 prop_string 成对（prop_apply 收 double，装不下宽字符串）。
void text_content_set(App &a, const std::wstring &s);
// 改一个属性。pair = 是否遵守 XY 成对锁定：预览里拖取景框是二维操作，两轴由调用方
// 自己写对，所以它传 false；面板输入框/滑块拖拽传 true（默认）。
bool prop_apply(App &a, int id, Clip *c, double v, bool pair = true);
// 点属性行上的锁按钮：切这一对（位置XY 或 缩放XY）的锁定状态。
void xform_lock_toggle(App &a, int field);
// Mirror X/Y 这类布尔行：点击即翻转 Strip::flag 的对应位。
bool prop_toggle(App &a, int id, Clip *c);
// 属性行的关键帧状态位，语义照 interface_anim.cc:62-124 的 button_anim_flag：
// 有非空曲线 ⇒ kKeyAnimated（BUT_ANIMATED）；该帧上还有键 ⇒ 再并上 kKeyAtFrame
// （BUT_ANIMATED_KEY，animrig::fcurve_frame_has_keyframe）。绘制装饰点和点它时
// 走插入还是删除都要读，所以位和函数都放头里。
enum : int { kKeyAnimated = 1, kKeyAtFrame = 2 };
int key_field_state(App &a, int field);
// 点装饰点 = Blender 的 button_anim_decorate_cb（interface_anim.cc:346-381）：
// toggle_keyframe_on_click 为真时，当前帧已有键就删键，否则按当前值插一个键。
void keyframe_dot_click(App &a, int field);
void sync_preview(App &a);
void sync_audio(App &a);

// ---------------------------------------------------------------- 算子
void add_source(App &a, const std::wstring &path, double at = -1.0, int track = -1);
// 新条带落位后的让位：整组一起升到上面第一条空通道，已有条带不被时间推移。
// 单位是「这一次新增的一组」，所以影片和它的声带永远停在同一时刻。
void shuffle_vertical(App &a, const std::vector<Clip *> &fresh);
void import_paths(App &a, const std::vector<std::wstring> &paths, double at = -1.0, int track = -1);
// 新建文本条带（sequencer.text_strip_add）：起点对准播放头、落在最上面的空通道、默认 3 秒。
void add_text(App &a);
void split_selected(App &a, bool hard);
void delete_selected(App &a, bool ripple);
void duplicate_selected(App &a);
// 连接/断开：sequencer.connect（toggle=True）与 sequencer.disconnect。两者都作用在选中集合上。
void connect_selected(App &a);
void disconnect_selected(App &a);
// 这条带在不在某个连接组里（draw 画 LINKED 角标、trim 决定拖把手带不带全组）。
bool clip_connected(const Clip &c);
// c 的连接成员（= connected_strips_get，strip_connect.cc:120-129）。
std::vector<Clip *> connected_clips(App &a, const Clip &c);
void mute_selected(App &a, bool mute);
void mute_unselected(App &a);
void lock_selected(App &a, bool lock);
void gap_remove(App &a, bool all);
void gap_insert(App &a, int frames = 10);
void snap_selected_to_frame(App &a);
void slip_selected(App &a, double delta_t);
void slide_selected(App &a, double delta_t);
// 模态变换（sequencer.slide / slip / extend：G / S / E）——Enter 提交、Esc 还原。
void modal_begin(App &a, ModalState::Kind kind);
void modal_update(App &a, double t);
// seq_slide 的 view2d_edge_pan=True：光标停在时间线左右边界外时按速度滚动视图。
void edge_pan_step(App &a, double now);
void modal_end(App &a, bool commit);
bool modal_active(const App &a);
void select_all(App &a);
void select_none(App &a);
void invert_selection(App &a);
void select_side_of_frame(App &a, bool right_side);
// sequencer.select_more / select_less（sequencer_select.cc:1460-1498）。
void select_more_less(App &a, bool more);
// sequencer.swap（sequencer_edit.cc:3633-3646）。
void swap_with_neighbor(App &a, bool right_side);
// sequencer.select_box（B）：矩形与条带求交。
void select_box(App &a, Rect box, bool extend);
void copy_selected(App &a);
void paste_clips(App &a);
Track &channel_slot(App &a, int idx);
void strip_jump(App &a, bool next, bool center = false);
void ripple_trim(App &a, bool left);
// sequencer.set_range_to_strips：把帧范围收成选中条带的并集。
void range_to_strips(App &a);
void nudge(App &a, double delta_seconds);
// 播放中每一 tick 唯一的播放头写入者：取音频时钟（没有就按界面时间），再做帧范围回卷。
void playhead_advance(App &a, double dt);
void jump_frame(App &a, int64_t frame);
// ED_areas_do_frame_follow：跳帧后把播放头带回视野（已在视野内则不动）。
void follow_playhead(App &a);
void keyframe_jump(App &a, bool next);
void toggle_play(App &a);
void run_cmd(App &a, int cmd);
const wchar_t *cmd_name(int cmd);
const wchar_t *cmd_keys(int cmd);  // 菜单右侧的快捷键提示（km_* 的修饰键顺序）
void build_menus(App &a);
void open_menu(App &a, int menu, float x, float y);
// 右键的一击：窗口过程只交出坐标，「开哪一个菜单」由认出这一点的对象自己报（ops.cpp）。
void on_rbutton_down(App &a, float fx, float fy);
// 滚轮的一格：两块视图挨个问，谁认出指针这一点谁答（ops.cpp）。
void wheel_step(App &a, int x, int y, short delta, WORD mods);
// HOME / Numpad +/- 同样按指针所在的那一块视图分流（ops.cpp）。
void view_all_at(App &a, float fx, float fy);
void view_zoom_step(App &a, float fx, float fy, double factor);
void set_aspect(App &a, int num, int den);
void set_fps_preset(App &a, int fps, double base);
void set_crf(App &a, int crf);
const wchar_t *crf_name(int crf);  // 质量档的显示名（= RNA 枚举项名），不在表里返回 nullptr
std::wstring crf_label(int crf);   // 行上的当前值：有名用名，无名按 RNA 的 CUSTOM 档显示
void set_speed(App &a, int preset);   // ffmpeg_preset：最低 11 / 良好 10 / 实时 12
void set_audio(App &a, bool none);    // audio_codec：false = AAC，true = NONE
void open_dialog(App &a);
void export_dialog(App &a);
void start_export(App &a, const std::wstring &path);
void commit_edit(App &a, bool apply);
void begin_edit(App &a, int id, const std::wstring &current);  // 数值按钮进入键入态
void commit_rename(App &a, bool apply);

// ---------------------------------------------------------------- 绘制
void draw(App &a);

// 下面两处几何由 draw.cpp 按 Blender 的度量算出，main.cpp 的命中测试复用同一套数字，
// 保证「看到的」和「点到的」是同一个矩形。
// 通道头（sequencer_channels_draw.cc:53-61, 264-286）：图标宽 = widget_unit*0.8*scale，
// 锁定在右缘内 1.5*图标宽，静音在 2.5*图标宽，名字从 0.65*图标宽起。
struct ChanWidgets {
  float scale = 1.0f, icon_w = 0.0f;
  Rect lock, mute, label;
};
ChanWidgets chan_widgets(const App &a, const TrackRow &row);

// 条带（sequencer_timeline_draw.cc:215-280 + DNA_sequence_types.h:623/624）：
// 上下内缩 0.05/0.95 通道高，圆角由通道行高决定，把手宽 = 5*U.pixelsize 个视图像素。
struct StripGeom {
  Rect body;                    // 像素矩形（已按着色器外扩 1px）
  float radius = 0.0f;          // 圆角（calc_strip_round_radius）
  float handle = 0.0f;          // 裁切把手宽（strip_handle_draw_size_get）
  float header_h = 0.0f;        // 文字条高度（strip_header_size_get），0 = 不放文字
  bool text_overlay = false;    // can_draw_text_overlay（决定 strip_content_top 的位置）
  bool show_text = false;       // 上面那条 + 长度门槛：文字是否真的上屏
  bool content = false;         // can_draw_strip_content（有声带的条带上这一档门槛反过来）
  bool waveform = false;        // seq_draw_waveforms_poll && !strip_is_too_small
  bool visible = false;         // 轨道行存在且条带在视野里
};
StripGeom strip_geom(const App &a, const Clip &c);

// 变速键带的几何 = strip_retiming_keys_box_get（sequencer_retiming_draw.cc:86-95）：
// 横 = 条带矩形全宽，纵 = 键中心上下各半颗键（键尺寸 10*U.pixelsize，中心在条带下沿往上
// 4+size/2 个像素）。绘制与命中都走它，保证「看到的 = 点到的」。
struct RetKeyBand {
  float cy = 0.0f, size = 0.0f, left = 0.0f, right = 0.0f;
  bool contains(float fx, float fy) const {
    return fx >= left && fx <= right && fy >= cy - size * 0.5f && fy <= cy + size * 0.5f;
  }
};
RetKeyBand retiming_band(const StripGeom &g);

}  // namespace app
