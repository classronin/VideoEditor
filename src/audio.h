#pragma once
// 预览音频：WASAPI 共享模式推流（push）。MinGW 的 IAudioClient 没有 GetNextPacketSize，
// 事件驱动模式不可用，故按设备周期轮询剩余空间并补写。
// 重叠的多条声带由同一个 ffmpeg 进程 amix 成一路（见 Ffmpeg::audio_mix），本模块永远只推
// 一路混好的流。Blender 那边也是给每条有声条带在 sound_scene 里挂一个 handle 一起混
// （sequencer/intern/sequencer.cc:1157-1172 strip_update_mix_sounds），静音的摘掉 handle
// （:1148-1154 seq_mute_sound_strips_cb）；这里把「一起混」交给 amix 做掉。
#include "media.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class AudioPlayer {
 public:
  AudioPlayer() = default;
  ~AudioPlayer();
  AudioPlayer(const AudioPlayer &) = delete;
  AudioPlayer &operator=(const AudioPlayer &) = delete;

  // 混着播 inputs：时间线上 [tl_from, tl_until) 这一段的音频。
  // 位置都是时间线秒：position() 直接给播放头该跟的刻度，不再按某条素材反算。
  // sync_audio 每个 tick 都会调，所以内容相同且发声位置还在这一段窗口里就什么都不做。
  void play(const std::vector<MixInput> &inputs, double tl_from, double tl_until);
  void stop();
  // 播放头跟的时钟：正在发声就交回声声位置，还在建立这一段就交回受理点，两头都没有（没播、
  // 落在空隙、已停）才交回 -1，让界面时钟自己走。绝不交回旧那一段的位置去盖新受理点。
  double position() const;
  std::wstring error() const;

 private:
  struct Req {
    std::vector<MixInput> inputs;
    double from = 0.0, until = 0.0;  // 时间线秒
    bool empty() const { return inputs.empty(); }
  };

  static DWORD WINAPI thread_main(LPVOID);
  // 逐条对得上、且「现在听到的位置」还落在想要的那一段窗口里就不重建：sync_audio 每个
  // tick 都调，起点会随播放头一帧帧往前挪。
  static bool same_inputs(const Req &ref, const Req &want);
  static bool same_set(const Req &ref, double live, const Req &want);
  void run();
  bool open_device();
  void close_device();
  void fail(const wchar_t *what, HRESULT hr);
  void halt_device();
  bool start_stream(const Req &req);
  void kill_stream();
  void play_loop();
  void mark_idle();
  void refill(size_t need_frames, int wait_ms);
  void write_device(BYTE *dst, const float *src, uint32_t frames);

  mutable std::mutex mu_;
  // UI -> worker
  Req pending_;
  bool stop_req_ = false, quit_ = false;
  // worker -> UI
  double cur_pos_ = -1.0;
  bool streaming_ = false;
  Req inflight_;  // 工作线程正在建立/正在推的那一段
  std::wstring err_;

  HANDLE thread_ = nullptr;
  HANDLE wake_ = nullptr;  // 自动复位：有新请求

  IAudioClient *client_ = nullptr;
  IAudioRenderClient *render_ = nullptr;
  WAVEFORMATEX *mix_ = nullptr;
  uint32_t buf_frames_ = 0;
  int64_t period_hns_ = 100000;  // 设备默认周期（100ns 单位）
  int rate_ = 48000, ch_ = 2;
  bool pcm16_ = false;

  // 以下仅工作线程访问
  FrameProc *proc_ = nullptr;
  std::vector<uint8_t> bytes_;  // 管道取回的交错立体声 float32 字节
  std::vector<uint8_t> stage_;
  size_t bpos_ = 0;
  int64_t left_ = 0;      // 还需提交的时间线帧数
  bool src_done_ = false;  // 解码管道已读尽
  double from_ = 0.0, until_ = 0.0;  // 时间线秒
  int64_t real_frames_ = 0;  // 已提交的音频帧（不含静音）
};
