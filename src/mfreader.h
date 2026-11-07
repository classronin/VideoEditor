#pragma once
// Media Foundation 硬解的一路取帧流。契约与 FrameReader 的 ffmpeg 子进程那一路对齐：
// 交回 BGRA 字节 + 这一帧真实的样本时间，池逻辑（选流、判落后、预取）不必知道底下是谁。
//
// 为什么值得多这一条路（同一只 build、1.mp4 1920x1080@59.94、Quadro P2200，app 内 VSE_TIME=1
// 双臂实测，数字从下列 trace 摊出来）：纯前进回放两臂都跟得上实时间
// （50.7 vs 54.4 解出帧/秒），但整机消耗差一倍多 —— MF 走 NVDEC 是 0.87 CPU 秒/挂钟秒（自进程
// 0.70 + 子进程 0.17），子进程软解是 1.81（0.30 + 1.51）；省的就是 ffmpeg 那 1.5 秒。
// 真正的分水岭是跳位：子进程每跳一次就
// 另开一条流，池被六条各停在别处的流占满、重启 0.80 次/秒全记 why_lag、解出帧掉到 15.7/秒；
// MF 在同一只 reader 里定位，池里始终一条流、lag 0（重点击 21 秒：1.25 vs 2.56 CPU 秒/秒）。
// 定位本身不是收益：SetCurrentPosition 到目标帧真到手要 70~844ms，且随位置线性涨。
// 它省的是「另开一只 reader」那 1.5 秒（app 内测得的 first_max），所以只在已经握着这只
// reader 的时候用它。
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MfStream {
 public:
  MfStream();
  ~MfStream();
  MfStream(const MfStream &) = delete;
  MfStream &operator=(const MfStream &) = delete;

  // 环境开关：默认就走这条路；VSE_MF=0 关掉它，只用 ffmpeg 子进程。
  static bool enabled();
  // 这只素材的系统解码器解不出帧（第一次 read 失败时记进来），以后连试都不试：
  // 判它行不行只能在解出第一帧之后，而每轮再试一次等于每轮白付一次开流。
  static void ban(const std::wstring &path);
  static bool banned(const std::wstring &path);
  // 从 seconds 起，输出 out_w*out_h。开不出来（非系统解码器支持的编码、设备失败、
  // MF 不肯给解码后的 NV12）返回 false，调用方按原样回落子进程。
  bool open(const std::wstring &path, double seconds, int out_w, int out_h);
  // 同一只 reader 里改定位（Blender 在同一个 avformat 句柄里 seek 的那个能力）。
  // 省下的是整次重建：实测 1.mp4 上重建到出画 1.5 秒，原地定位 0.07~0.84 秒。
  bool seek(double seconds);
  bool read();  // 解下一帧；false = 到头或出错
  // 上一次 read 失败是不是「这路编码系统解码器解不了」（错误态），而不是走到素材末尾。
  bool rejected() const;
  // 这一帧的字节，BGRA（和 D3D 纹理要的格式同一个，中间不再过一遍 RGB24）。
  // take_* 把缓冲的所有权交出去，下一帧 read() 自己重要一块 —— 省的是每帧一次整幅拷贝。
  const std::vector<uint8_t> &bgra() const { return bgra_; }
  std::vector<uint8_t> take_bgra() { return std::move(bgra_); }
  int w() const { return w_; }
  int h() const { return h_; }
  double t() const { return t_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
  std::vector<uint8_t> bgra_;
  int w_ = 0, h_ = 0;
  double t_ = -1.0;
  // MF 自己的时钟原点（开流时定位到 0 读第一只样本的标号）。带 elst 的素材它非 0：
  // 定位加它、读回减它，MF 的钟才和导出/子进程同一把尺。无 elst 时为 0，行为不变。
  double origin_ = 0.0;
};
