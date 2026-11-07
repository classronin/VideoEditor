#pragma once
// 声带波形的包络缓存：一个源文件一份，250 个桶/秒，每桶三个浮点 (min, max, rms)。
//
// 对应 Blender 的 bSound::runtime->waveform（Vector<float> 三元组，
// blenkernel/intern/sound.cc:1357-1401 的 BKE_sound_read_waveform）：
//  - 数据挂在「文件」上而不是「条带」上，所以同一个 mp4 的两条声带共用一份包络；
//  - 桶数 = 秒 × SOUND_WAVE_SAMPLES_PER_SECOND(=250，BKE_sound.hh:21)，
//    每桶取该时段内所有样本的最小值、最大值、均方根；
//  - 单声道：Blender 用 ChannelMapper 把声像折成一路（:1294 的 specs.channels = MONO），
//    这里交给 ffmpeg 的 -ac 1（立体声折半直加，整条曲线同幅缩放，不影响相对大小）；
//  - 只在「样本峰值 > 1.0」时才整体除以峰值（:1348-1352）。这一条对判断音量很重要：
//    峰值 0.2 的安静素材归一后仍然只占条带高度的一小截，波形才当得了「声音大还是小」的读数。
//
// 【只解看得见的窗口】数组的长度按素材定死（Blender 也是整条长度），但**解码不是整条**：
// 绘制端每帧把自己那几列落在的源秒报给 view()，后台线程只解这些块（一块 10 秒），
// 烘过就不再碰。原来的「第一次画就把整条音轨解完」在 3 小时 11GB 电影上是几十 CPU 秒
// 的预付款，而那一趟里用户真正看见的通常只有几十秒 —— 按 lossless-cut 的可见窗口口径改掉
// （它的波形本来就只出当前窗口那一张 showwavespic，见 源码/lossless-cut/src/main/ffmpeg.ts:207-250）。
// 分块靠 ffmpeg 的 -ss 前置（快速且精确）：实测同一 build 上分块解出的样本与整段解出的
// 逐样本相同，只早不到 1 毫秒（同一 build 的分块/整段对照实测，跑具已删、结论留在这里），
// 所以块缝只有轻微重叠、不会空洞。
//
// 烘好之前那一列读到的是 0 ⇒ 画出来就是那条零线，和 Blender「job 没回之前什么都不画」
// 是同一件事（本工程零线本来就一直画）。烘好的块到位后由烘焙线程给 UI 投递一格，
// UI 收到才发现 done() 变了、把条带区标脏 —— 不再有时钟去问「有没有烘完」。
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// 每秒多少个桶（Blender 的 SOUND_WAVE_SAMPLES_PER_SECOND）。
inline constexpr int kWaveBucketsPerSecond = 250;
// 解码出来的样本率固定成这个数：48000 / 250 = 每块 192 个样本，
// 桶边界落在整数上，不必再照 Blender 那样对 float 的 samplejump 取 floor。
inline constexpr int kWaveSampleRate = 48000;

// 一源一份的包络数据：base 是 Blender 原样的 250 桶/秒，levels 是在它之上折出来的
// 折叠层（每层把相邻 kFold 个桶折成 1 个，所以第 i 层的桶率 = 250 / 8^(i+1)）。
//
// 为什么要折：绘制是「每个设备像素一列」，一列要把落在它里面的所有桶折叠成一个
// min/max/rms。Blender 只有 250 桶/秒这一层，所以它在 pps=0.107（一屏放 3h22m 电影）
// 时每列扫 2,336 个桶 —— sequencer_timeline_draw.cc:537-547 的循环和我们的一样，实测
// 一帧 4.10ms（独立量具实测，只一条声带、1,300px），而缩得越小越贵、没有上限。
// 折叠层让每一列的读数量恒定在 ≤kFold 个：同一段代码换个桶率跑，一帧 0.01ms。
//
// 折叠是保守（不丢峰）的：min 取各 min 的最小、max 取各 max 的最大、rms 取各 rms 的最大
// —— 这三个正是绘制端跨桶合并时用的运算，可结合，所以折层不改变画出来的包络形状。
// 唯一的边界差：列落在折叠桶的半个桶之外时，会多读到相邻 8 个原始桶的极值（最多 32ms
// 音频，而在用得上粗层的缩放级别上一像素本身就代表 9 秒以上）。
//
// 【数组创建时定死、之后只改值】这是「按需烘」能不加锁的前提：base/levels 的长度从第一次
// 被画到就固定（按素材长度，多留 1 秒给比画面更长的音轨），后台线程只往里写值、从不 resize，
// 所以绘制线程读到的要么是 0（还没烘到）要么是一个完整的浮点 —— 最坏晚一帧，下一帧补上。
struct WaveData {
  static constexpr int kFold = 8;
  std::vector<float> base;                 // 250 桶/秒，(min,max,rms) 三元组
  std::vector<std::vector<float>> levels;  // levels[i] 桶率 = 250 / 8^(i+1)
  // 归一分母：烘到的这一块里样本峰值若超过 1.0 就是那个峰值，否则 1.0（Blender 的门槛）。
  // 存的是**原始值**、绘制端把 y_scale 除以它 —— 整条曲线同幅缩放，和烘完再除等价，
  // 但按块烘的时候「整条的峰值」本来就要一块一块才知道，只能这样存。
  std::atomic<float> norm{1.0f};
  // 当前缩放该用哪一层：取「桶率仍 ≥ 每像素秒数」的最粗一层，于是每列至少读到一桶、
  // 至多读到 kFold 桶。pps ≥ 250 时返回 base（一像素不足一桶，和 Blender 一样逐列取一桶）。
  const std::vector<float> &level_for(double pixels_per_second, double *bucket_rate) const;
};

class WaveCache {
 public:
  static WaveCache &instance();
  // 画某条声带时每帧调一次：path 的那份包络（没见过就按素材长度把数组定死并起后台线程），
  // 同时把「这一帧看得见的源秒」[from,to] 交给它去补解。还没烘到的地方是 0（画成零线）。
  // 烘失败 ⇒ nullptr。
  std::shared_ptr<WaveData> view(const std::wstring &path, double from_sec, double to_sec);
  // 每烘完一笔（成功或失败）+1，UI 收到烘焙线程那一格发布后读它：变了就重画条带区。
  // 以前还有个「还有没有没烘到的块」的查询，靠 15ms 时钟每一格问一遍 —— 现在
  // 最后一笔也会自己说一声，那一问没有买家了，别再加回来。
  int done();
  // 关窗口时收掉所有还在跑的 ffmpeg 子进程。
  void cancel_all();
  // 「刚下载完 ffmpeg」之后调一次：之前因为找不到 ffmpeg 而判失败的条目重新允许烘焙。
  void reset();

 private:
  WaveCache() = default;
};
