#pragma once
// ffprobe/ffmpeg 子进程封装：探测、关键帧索引、帧流与音频流读取。
// 预览走裸 BGRA（-f rawvideo -pix_fmt bgra）无损管道，应用侧不含任何解码器代码。
#include "types.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 一个 ffmpeg 子进程的 stdout，带缓冲读取。stderr 由后台线程收集。
class FrameProc {
 public:
  FrameProc() = default;
  ~FrameProc();
  FrameProc(const FrameProc &) = delete;
  FrameProc &operator=(const FrameProc &) = delete;

  bool start(const std::wstring &exe, const std::vector<std::wstring> &args, std::wstring *err);
  // 读取至多 n 字节，返回实际字节数；0 表示对端关闭或出错。
  size_t read_some(void *dst, size_t n);
  bool read_exact(void *dst, size_t n) {
    auto *p = reinterpret_cast<uint8_t *>(dst);
    size_t done = 0;
    while (done < n) {
      size_t got = read_some(p + done, n - done);
      if (!got) return false;
      done += got;
    }
    return true;
  }
  // 管道里是否已有数据（不阻塞）。timeout_ms=0 只做一次探测。
  bool wait_ready(DWORD timeout_ms);
  void kill();
  bool running() const { return pi_.hProcess != nullptr; }
  // 进程是否已退出（未启动/已 kill 也算退出）。
  bool exited(DWORD *code = nullptr);
  std::wstring stderr_text();
  // 这一个子进程的 CPU（user+sys 毫秒），内核按句柄记账，进程退出后仍可读（句柄关掉之前）。
  // 用它把「这一趟导出」单独算出来：Ffmpeg::children_cpu_ms() 是 job 全家的累计，
  // 预览/波形/音频的进程都混在里面，量不出导出本身值多少。
  // kill switch: 不过门即删，不优化
  double cpu_ms() const;

 private:
  static DWORD WINAPI stderr_thread(LPVOID);
  STARTUPINFOW si_{};
  PROCESS_INFORMATION pi_{};
  HANDLE out_read_ = nullptr;
  HANDLE err_read_ = nullptr;
  HANDLE err_thread_ = nullptr;
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool eof_ = false;
  std::mutex err_mu_;
  std::string err_;
};

// 预览混音的一路声带：条带在当前这段时间线区间里的源窗口和速度。段是按「发声集合不变的
// 最大区间」切的，所以每路的起音都对齐段起点，不需要额外延时。
// speed 是条带速度：Blender 那边 sound_pitch_get 直接把 speed_factor 当音高返回
// （sequencer/intern/sound.cc:162-169），音高和节奏一起变，所以是 asetrate 不是 atempo。
struct MixInput {
  const VideoInfo *vi = nullptr;
  double from = 0.0, until = 0.0;
  double speed = 1.0;
};

class Ffmpeg {
 public:
  // PATH 优先，其次与主程序同目录（两处都要 ffmpeg.exe + ffprobe.exe 成对才算找到）。
  // 找不到返回 false。from 回写命中的来源，出问题时日志里能一眼看到用的是哪一份。
  static bool locate(std::wstring *ffmpeg, std::wstring *ffprobe, std::wstring *from = nullptr);
  static bool probe(const std::wstring &path, VideoInfo *out, std::wstring *err = nullptr);

  // 从 seconds 起的帧流：转码时 -ss 前置即为“快速且精确”，输出帧 0 落在 seconds。
  static FrameProc *video_stream(const VideoInfo &vi, double seconds, int out_w, int out_h);
  // 把这段时间线区间里的若干声带混成一路立体声 float32 流（预览用）。每一路一个输入：
  // -ss 定位到源起点（快速且精确），atrim 截出这一段的源窗口，speed≠1 走 asetrate，
  // 最后 amix=...:duration=longest:normalize=0 逐样本直加、不按路数衰减，先结束的那路
  // 立刻退场（和导出链路同一套，见 export.cpp 的 amix）。实测重叠处两音都在
  //（-24dB + -24dB = 直加），一路先结束后另一路照常放到自己的段尾。
  static FrameProc *audio_mix(const std::vector<MixInput> &inputs, int sample_rate);

  // 把一个刚起来的子进程挂进「主程序一死就全家清空」的 job（见 media.cpp 的 job_handle）。
  // 凡是能起进程的地方都要调一次：FrameProc::start、以及 ffmpegget 的 tar。
  static void attach_to_job(HANDLE process);

  // 进过这个 job 的全部子进程累计 CPU（毫秒），失败返回 -1。内核自己记账，所以短命的解码
  // 进程也不会被按秒轮询漏掉 —— 拖动的卡顿到底花在预览、烘焙还是波形上，看这个数就能分开。
  static double children_cpu_ms();
};

class MediaCache {
 public:
  static MediaCache &instance();
  VideoInfo *open(const std::wstring &path, std::wstring *err = nullptr);
  VideoInfo *find(const std::wstring &path);

 private:
  std::mutex mu_;
  std::unordered_map<std::wstring, std::unique_ptr<VideoInfo>> map_;
};
