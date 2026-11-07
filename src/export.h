#pragma once
// 导出：把时间线编译成一条 ffmpeg filter_complex 命令，异步执行并回报进度。
// 视频/音频都交给 ffmpeg，应用侧不写任何编码器。
#include "media.h"
#include "retiming.h"
#include "types.h"
#include <memory>
#include <string>
#include <vector>

struct ExportCommand {
  std::vector<std::wstring> args;  // 不含可执行文件名
  std::wstring graph;              // filter_complex 原文，便于排查
  // 导出进程活着期间要读的临时文件：逐帧表达式的图可以长到几十万字符，命令行（32767）
  // 装不下，只能写进文件用 -/filter_complex 读；文本条带的画面是排版器现排的位图，ffmpeg
  // 只认文件，所以落成 PNG。两边都由 Exporter 在进程结束时删掉。
  std::vector<std::wstring> temp_files;
  double total_seconds = 0;
};

// 几何换算与预览保持一致：裁切(源像素) -> 适配 -> 缩放% -> 偏移 -> 旋转。
bool build_export(const Project &proj, const std::wstring &out_path, ExportCommand *cmd,
                  std::wstring *err);

class Exporter {
 public:
  ~Exporter();
  bool start(const Project &proj, const std::wstring &out_path, std::wstring *err);
  void pump();  // 主线程每帧调用，非阻塞
  bool running() const { return running_; }
  double progress() const { return total_ > 0 ? done_ / total_ : 0.0; }
  const std::wstring &message() const { return message_; }
  void cancel();

 private:
  void consume();
  void drop_temp_files();
  std::unique_ptr<FrameProc> proc_;
  std::vector<uint8_t> buf_;
  std::wstring out_;
  std::vector<std::wstring> temp_files_;
  double total_ = 0, done_ = 0;
  bool running_ = false;
  std::wstring message_;
  // ---- 导出账（VSE_MEASURE=1 才记）----
  // 窗口 = 起 ffmpeg 之前 到 检测到退出 那一拍。主程序自己的启动不在窗口里（进程启动不是导出的
  // 钱）；ffmpeg 自己的 spawn 在窗口里 —— 那是用户真付的钱，不该藏。
  bool measure_ = false;
  double t_start_ = 0.0, self_cpu0_ = 0.0;
};
