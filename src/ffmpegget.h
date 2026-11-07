#pragma once
// ffmpeg 两处来源（PATH、主程序同目录）都落空时的自助安装（项目目标.md 的「PATH、目录、
// 下载」这一优先级）：从镜像下载 GyanD 的 full_build 压缩包，只解出 ffmpeg.exe /
// ffprobe.exe 放到主程序同目录。下载在一条工作线程里跑，UI 线程只读进度；
// 镜像按表里的顺序一家一家试，不让用户挑。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <string>

namespace app {
struct App;

// 进度（工作线程写 done/total/note，最后写 state；UI 线程先读 state 再读其余）。
enum FFmpegFetchState : int {
  kFFIdle = 0,     // 没在跑
  kFFVersion = 1,  // 正在问 gyan.dev 当前版本号
  kFFDownload = 2,
  kFFExtract = 3,  // 解压 + 搬文件
  kFFDone = 4,
  kFFFailed = 5,
};

struct FFmpegFetch {
  std::atomic<int> state{kFFIdle};
  std::atomic<long long> done{0};
  std::atomic<long long> total{0};
  std::wstring note;  // 失败原因 / 成功后的版本号；写完后才翻 state
};

FFmpegFetch &ffmpeg_fetch();
bool ffmpeg_fetch_busy();
// 这一条线还没走完：正在跑，或者已经跑完但结果还没交出去（state 停在
// kFFDone / kFFFailed，等下一格把状态条写完才回 kFFIdle）。时钟问的是这个。
bool ffmpeg_fetch_active();
// 起一条工作线程做「查版本 → 下载 → 解压 → 搬到 exe 同目录」。忙时忽略。
void ffmpeg_fetch_start();
// 每次 tick 调用：状态或进度变了就刷状态条（并标脏重绘）。
void ffmpeg_fetch_tick(App &a);

// 只给自动化验证用：覆盖完整下载地址（跳过问版本号），好让整条下载→解压→搬运链
// 能在本机的一个小 http 服务上跑通，不必去拉真镜像那一两百 MB。
void ffmpeg_set_url_override(const std::wstring &url);

}  // namespace app
