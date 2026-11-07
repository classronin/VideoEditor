#include "media.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cmath>

namespace {

// 按 CommandLineToArgvW 的语义转义单个参数。
std::wstring quote_arg(const std::wstring &s) {
  if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : s) {
    if (c == L'\\') { ++backslashes; continue; }
    if (c == L'"') { out.append(backslashes * 2 + 1, L'\\'); out += L'"'; backslashes = 0; continue; }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  out += L'"';
  return out;
}

std::wstring join_cmdline(const std::wstring &exe, const std::vector<std::wstring> &args) {
  std::wstring cmd = quote_arg(exe);
  for (const auto &a : args) { cmd += L' '; cmd += quote_arg(a); }
  return cmd;
}

std::wstring wfrom_utf8(const std::string &s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
  return w;
}

bool parse_rational(const std::string &v, int32_t *num, int32_t *den) {
  size_t slash = v.find('/');
  int n = 0, d = 0;
  if (slash == std::string::npos) {
    if (v.empty() || v == "N/A") return false;
    n = atoi(v.c_str());
    d = 1;
  } else {
    n = atoi(v.substr(0, slash).c_str());
    d = atoi(v.substr(slash + 1).c_str());
  }
  *num = int32_t(n);
  *den = int32_t(d);
  return d != 0 && n > 0;
}

struct Section { std::vector<std::pair<std::string, std::string>> kv; };

// 解析 ffprobe 的 [STREAM]/[SECTION] 结构文本。
std::vector<Section> split_sections(const std::string &text, const char *tag) {
  std::vector<Section> out;
  const std::string open = std::string("[") + tag + "]";
  const std::string close = std::string("[/") + tag + "]";
  size_t pos = 0;
  while ((pos = text.find(open, pos)) != std::string::npos) {
    size_t end = text.find(close, pos);
    if (end == std::string::npos) break;
    std::string body = text.substr(pos + open.size(), end - pos - open.size());
    pos = end + close.size();
    Section sec;
    size_t p = 0;
    while (p < body.size()) {
      size_t eol = body.find('\n', p);
      if (eol == std::string::npos) eol = body.size();
      std::string line = body.substr(p, eol - p);
      p = eol + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      size_t eq = line.find('=');
      if (eq == std::string::npos || line.empty() || line[0] == '[') continue;
      sec.kv.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    out.push_back(std::move(sec));
  }
  return out;
}

std::string find_kv(const Section &sec, const std::string &key) {
  for (auto it = sec.kv.rbegin(); it != sec.kv.rend(); ++it)
    if (it->first == key) return it->second;
  return "";
}

double find_num(const Section &sec, const std::string &key, double fallback = 0.0) {
  std::string v = find_kv(sec, key);
  return v.empty() ? fallback : strtod(v.c_str(), nullptr);
}

}  // namespace

// ---------------------------------------------------------------- 进程与管道

// 子进程兜底 job：最后一个 job 句柄关闭时，内核把 job 里的所有进程一起清掉。
// 为什么光靠 FrameProc::kill() 不够：
//  1) 用 scoop/choco 之类装的 ffmpeg 时，PATH 上命中的 shims\ffmpeg.exe 只是个 136KB 的
//     壳，真正的 ffmpeg.exe 是它的子进程（实测 ppid=壳）。TerminateProcess 杀得到壳，
//     grandchild 就成了孤儿。
//  2) 任何不走正常退出的路径 —— 崩溃、被任务管理器结束、超时后放弃等待的收尾线程 ——
//     都来不及执行 kill()。
// job 是内核保证：进程只要没了（句柄全关），descendant 一个不留。Win8 起 job 可嵌套，
// 所以即使主程序自己已经在某个 job 里（被 shell/CI 包着）也能建自己的子 job。
namespace {
HANDLE job_handle() {
  static HANDLE job = [] {
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (!j) return j;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
      CloseHandle(j);
      return static_cast<HANDLE>(nullptr);
    }
    return j;
  }();
  return job;
}
}  // namespace

void Ffmpeg::attach_to_job(HANDLE process) {
  const HANDLE j = job_handle();
  if (j && process) AssignProcessToJobObject(j, process);
}

double Ffmpeg::children_cpu_ms() {
  const HANDLE j = job_handle();
  if (!j) return -1.0;
  JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION acc{};
  if (!QueryInformationJobObject(j, JobObjectBasicAndIoAccountingInformation, &acc, sizeof acc,
                                 nullptr))
    return -1.0;
  // 内核以 100ns 为单位记账，而且累加的是「进过这个 job 的所有进程」，已经退出的也算。
  const uint64_t t = uint64_t(acc.BasicInfo.TotalUserTime.QuadPart) +
                     uint64_t(acc.BasicInfo.TotalKernelTime.QuadPart);
  return double(t) / 10000.0;
}

// 起进程的全局锁。CreatePipe 出来的两端一开始都是「可继承」的，读端要等一等才被清掉标志；
// 这段窗口里如果别的线程（预览工作线程、波形烘焙线程、音频收尾线程、导出线程都会同时起进程）
// 正好执行 CreateProcessW(bInheritHandles=TRUE)，那个新子进程就白拿了一份别人的管道读端。
// 后果不在当次：等主程序退出、本该收到「管道已断」的那个 ffmpeg，因为读端还被另一个 ffmpeg
// 握着而永远阻塞在 WriteFile 上 —— 任务管理器里就是一批 CPU 0%、一天不动的 ffmpeg.exe。
// 把 CreatePipe → CreateProcess → 关写端整段串起来，窗口里全进程就只有这一次要用的写端是可继承的。
std::mutex g_spawn_mu;

FrameProc::~FrameProc() { kill(); }

DWORD WINAPI FrameProc::stderr_thread(LPVOID param) {
  auto *self = reinterpret_cast<FrameProc *>(param);
  uint8_t chunk[4096];
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(self->err_read_, chunk, sizeof chunk, &got, nullptr) || got == 0) break;
    std::lock_guard<std::mutex> lk(self->err_mu_);
    self->err_.append(reinterpret_cast<char *>(chunk), got);
    if (self->err_.size() > 65536) self->err_.erase(0, self->err_.size() - 65536);
  }
  return 0;
}

bool FrameProc::start(const std::wstring &exe, const std::vector<std::wstring> &args, std::wstring *err) {
  std::lock_guard<std::mutex> spawn_lk(g_spawn_mu);  // 见 g_spawn_mu 的注释
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out_write = nullptr, err_write = nullptr;
  if (!CreatePipe(&out_read_, &out_write, &sa, 1 << 18)) {
    if (err) *err = L"CreatePipe(stdout) 失败";
    return false;
  }
  if (!CreatePipe(&err_read_, &err_write, &sa, 1 << 16)) {
    CloseHandle(out_read_);
    out_read_ = nullptr;
    if (err) *err = L"CreatePipe(stderr) 失败";
    return false;
  }
  SetHandleInformation(out_read_, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(err_read_, HANDLE_FLAG_INHERIT, 0);

  memset(&si_, 0, sizeof(si_));
  si_.cb = sizeof(si_);
  si_.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si_.wShowWindow = SW_HIDE;
  si_.hStdOutput = out_write;
  si_.hStdError = err_write;

  std::wstring cmd = join_cmdline(exe, args);
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(0);
  BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si_, &pi_);
  CloseHandle(out_write);
  CloseHandle(err_write);
  if (!ok) {
    wchar_t msg[160];
    // 宽字符参数在 MinGW 的 swprintf 里必须用 %ls，用 %s 会被当成窄字符串，
    // 只剩路径的第一个字符。
    swprintf(msg, 160, L"无法启动 %ls（Win32 错误 %lu）", exe.c_str(), GetLastError());
    if (err) *err = msg;
    CloseHandle(out_read_); out_read_ = nullptr;
    CloseHandle(err_read_); err_read_ = nullptr;
    return false;
  }
  // 先进 job 再干活：万一后面的收尾没跑到，内核也会替我们把这条进程链清干净。
  Ffmpeg::attach_to_job(pi_.hProcess);
  err_thread_ = CreateThread(nullptr, 0, stderr_thread, this, 0, nullptr);
  return true;
}

size_t FrameProc::read_some(void *dst, size_t n) {
  if (eof_ || !out_read_ || n == 0) return 0;
  auto *out = reinterpret_cast<uint8_t *>(dst);

  if (pos_ < buf_.size()) {  // 先交还缓冲，避免额外拷贝
    size_t take = std::min(buf_.size() - pos_, n);
    memcpy(out, buf_.data() + pos_, take);
    pos_ += take;
    if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
    return take;
  }
  if (n == 0) return 0;

  DWORD got = 0;
  if (n > (1u << 16)) {  // 大块直读目标
    if (!ReadFile(out_read_, out, DWORD(n), &got, nullptr) || got == 0) { eof_ = true; return 0; }
    return got;
  }
  std::vector<uint8_t> tmp(1 << 18);
  if (!ReadFile(out_read_, tmp.data(), DWORD(tmp.size()), &got, nullptr) || got == 0) {
    eof_ = true;
    return 0;
  }
  size_t take = std::min<size_t>(got, n);
  memcpy(out, tmp.data(), take);
  if (take < got) { buf_.assign(tmp.begin() + take, tmp.begin() + got); pos_ = 0; }
  return take;
}

bool FrameProc::wait_ready(DWORD timeout_ms) {
  if (!out_read_) return false;
  if (pos_ < buf_.size()) return true;
  ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    DWORD avail = 0;
    if (PeekNamedPipe(out_read_, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) return true;
    if (eof_ || GetTickCount64() >= deadline) return false;
    Sleep(1);
  }
}

bool FrameProc::exited(DWORD *code) {
  if (!pi_.hProcess) return true;
  DWORD c = STILL_ACTIVE;
  if (!GetExitCodeProcess(pi_.hProcess, &c) || c == STILL_ACTIVE) return false;
  if (code) *code = c;
  return true;
}

// kill switch: 不过门即删，不优化
double FrameProc::cpu_ms() const {
  if (!pi_.hProcess) return -1.0;
  FILETIME creation, exit_, user, kernel;
  if (!GetProcessTimes(pi_.hProcess, &creation, &exit_, &kernel, &user)) return -1.0;
  ULARGE_INTEGER u, k;
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  // FILETIME 的刻度是 100 纳秒 ⇒ 毫秒除以 1e4
  return double(u.QuadPart + k.QuadPart) / 1e4;
}

void FrameProc::kill() {
  if (pi_.hProcess) {
    TerminateProcess(pi_.hProcess, 0);
    WaitForSingleObject(pi_.hProcess, 300);
    CloseHandle(pi_.hProcess);
    CloseHandle(pi_.hThread);
    pi_ = {};
  }
  if (out_read_) { CloseHandle(out_read_); out_read_ = nullptr; }
  if (err_thread_) { WaitForSingleObject(err_thread_, 1000); CloseHandle(err_thread_); err_thread_ = nullptr; }
  if (err_read_) { CloseHandle(err_read_)  ; err_read_ = nullptr; }
}

std::wstring FrameProc::stderr_text() {
  std::lock_guard<std::mutex> lk(err_mu_);
  std::wstring w;
  w.reserve(err_.size());
  for (unsigned char c : err_) w += c < 0x80 ? wchar_t(c) : L' ';
  return w;
}

// ---------------------------------------------------------------- 定位 ffmpeg
//
// 优先级按需求：PATH 第一，其次主程序同目录，两处都没有就走「下载 ffmpeg」（ffmpegget.cpp）。
// 一个目录里必须 ffmpeg.exe 和 ffprobe.exe 都在才算命中：只有半套会让后面的
// 探测（ffprobe）与解码（ffmpeg）各自报一句看不懂的错。
namespace {
bool pair_in_dir(const std::wstring &dir, std::wstring *ff, std::wstring *probe) {
  const std::wstring a = dir + L"ffmpeg.exe";
  const std::wstring b = dir + L"ffprobe.exe";
  if (GetFileAttributesW(a.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  if (GetFileAttributesW(b.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  if (ff) *ff = a;
  if (probe) *probe = b;
  return true;
}
}  // namespace

bool Ffmpeg::locate(std::wstring *ffmpeg, std::wstring *ffprobe, std::wstring *from) {
  wchar_t buf[8192];
  const DWORD n = GetEnvironmentVariableW(L"PATH", buf, 8192);
  if (n > 0 && n < 8192) {
    const std::wstring path(buf, n);
    size_t start = 0;
    while (start <= path.size()) {
      const size_t semi = path.find(L';', start);
      std::wstring dir = path.substr(start, semi == std::wstring::npos ? std::wstring::npos
                                                                       : semi - start);
      for (auto &c : dir) if (c == L'/') c = L'\\';
      while (!dir.empty() && dir.back() == L' ') dir.pop_back();
      if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
      if (pair_in_dir(dir, ffmpeg, ffprobe)) {
        if (from) *from = L"PATH：" + dir;
        return true;
      }
      if (semi == std::wstring::npos) break;
      start = semi + 1;
    }
  }
  wchar_t mod[MAX_PATH];
  const DWORD m = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring base(mod, m);
  const size_t s = base.find_last_of(L"\\/");
  std::wstring bindir = s == std::wstring::npos ? std::wstring(L".\\") : base.substr(0, s + 1);
  for (auto &c : bindir) if (c == L'/') c = L'\\';
  const bool ok = pair_in_dir(bindir, ffmpeg, ffprobe);
  if (from) *from = ok ? L"主程序目录：" + bindir : L"哪都没有";
  return ok;
}

// ---------------------------------------------------------------- ffprobe

namespace {
bool run_capture(const std::wstring &exe, const std::vector<std::wstring> &args,
                 std::string *text, std::wstring *err) {
  FrameProc p;
  if (!p.start(exe, args, err)) return false;
  std::vector<uint8_t> all;
  uint8_t chunk[1 << 16];
  for (;;) {
    size_t got = p.read_some(chunk, sizeof chunk);
    if (!got) break;
    all.insert(all.end(), chunk, chunk + got);
    if (all.size() > (size_t(512) << 20)) break;  // 防御：超长输出截断
  }
  p.kill();
  text->assign(reinterpret_cast<char *>(all.data()), all.size());
  if (all.empty()) {
    std::wstring detail = p.stderr_text();
    if (err) *err = detail.empty() ? L"ffprobe 无输出" : detail;
    return false;
  }
  return true;
}
}  // namespace

// 这里试过把解码交给 GPU（-hwaccel cuda，d3d11va 兜底），结论是不要，别再来一遍：
// 命令行单点取帧确实省一半 CPU（1.03→0.55 CPU 秒/位置），但进程内实测两头都亏 ——
// 拖动：波形烘焙（一部电影要 41.5 CPU 秒）两边都跑完之后，子进程总 CPU 只从 43.75 省到
// 42.61，首帧却从 489.6ms 涨到 1742.1ms（late 那列不能当证据：软解重跑一次也从 79 跳到 437）；
// 回放：每秒烧的 CPU 没降（1.90→2.03 CPU 秒/挂钟秒），交付帧率却从 48.7fps 掉到 13.7~20.6fps。
// 原因是我们的池会把一路 reader
// 连着喂帧（reuse≈47/s），本要吃掉 119 帧 GOP 的「帧精确重定位」这件事在应用里几乎不发生，
// 省不到；而每路新起的子进程都要付一次 CUDA 上下文初始化（467ms），每帧还要 GPU→内存搬
// 3MB 给纹理上传，两头都是净支出。
//
// 2026-11-03 复核，把上面这句的适用面收窄：被推翻的是
// 「ffmpeg 子进程 + -hwaccel」这一种形态，不是「GPU 解码」。当前 build 同一只 1.mp4 连续回放的
// 软解基线重测是 2.75 CPU 秒/挂钟秒（self 0.26 + 子进程 2.49，restart=58 次/73 秒，
// paint 55.6/s，first_max 539.9ms）—— 比这里写的 1.90 更贵，因为那次是 --seek 单点口径。
// 进程内常驻 DXVA（GStreamer d3d11h264dec）在同一只文件上是 0.02~0.59 CPU 秒/挂钟秒、
// 逐跳 100.6ms/28.1ms CPU 且帧精确（pts_err +1ms）。所以「要不要 GPU 解码」要按常驻解码器
// 重开，不是按 -hwaccel 重开；跨设备零拷贝那条在 MSYS2 GStreamer 1.28.8 上不通（拿不到
// 共享句柄，也无法把我们的设备注册给元素），落点是 GPU 转 BGRx 后 download 一次。

bool Ffmpeg::probe(const std::wstring &path, VideoInfo *out, std::wstring *err) {
  std::wstring ffexe, ffprobe;
  if (!locate(&ffexe, &ffprobe)) { if (err) *err = L"找不到 ffmpeg.exe / ffprobe.exe"; return false; }
  const std::wstring entries =
      L"stream=index,codec_type,codec_name,width,height,r_frame_rate,avg_frame_rate,duration,"
      L"nb_frames,sample_rate,channels,sar,pix_fmt,start_time";
  std::vector<std::wstring> args = {L"-v", L"error", L"-show_entries", entries,
                                    L"-of", L"default=noprint_wrappers=0", path};
  std::string text;
  if (!run_capture(ffprobe, args, &text, err)) return false;

  out->path = path;
  const Section *vsec = nullptr, *asec = nullptr;
  auto sections = split_sections(text, "STREAM");
  for (const auto &sec : sections) {
    std::string type = find_kv(sec, "codec_type");
    if (type == "video" && !vsec) vsec = &sec;
    else if (type == "audio" && !asec) asec = &sec;
  }
  if (!vsec) { if (err) *err = L"未找到视频流"; return false; }

  out->codec = wfrom_utf8(find_kv(*vsec, "codec_name"));
  out->width = int(find_num(*vsec, "width"));
  out->height = int(find_num(*vsec, "height"));
  out->duration = find_num(*vsec, "duration");
  out->start_time = find_num(*vsec, "start_time");
  out->frame_count = int64_t(find_num(*vsec, "nb_frames"));
  Frac rfr{}, afr{};
  bool rfr_ok = parse_rational(find_kv(*vsec, "r_frame_rate"), &rfr.num, &rfr.den);
  bool afr_ok = parse_rational(find_kv(*vsec, "avg_frame_rate"), &afr.num, &afr.den);
  parse_rational(find_kv(*vsec, "sar"), &out->sar_num, &out->sar_den);
  if (out->sar_num <= 0 || out->sar_den <= 0) { out->sar_num = 1; out->sar_den = 1; }
  if (asec) { out->has_audio = true; out->audio_rate = int(find_num(*asec, "sample_rate", 48000));
              out->audio_channels = int(find_num(*asec, "channels", 2));
              out->audio_start_time = find_num(*asec, "start_time"); }

  // 时长缺失时回退到容器层。
  if (!out->duration && !out->frame_count) {
    std::vector<std::wstring> dur = {L"-v", L"error", L"-show_entries", L"format=duration",
                                     L"-of", L"default=noprint_wrappers=1:nokey=1", path};
    std::string dtext;
    if (run_capture(ffprobe, dur, &dtext, nullptr)) out->duration = strtod(dtext.c_str(), nullptr);
  }
  out->cfr = rfr_ok && afr_ok && rfr.num * afr.den == afr.num * rfr.den;
  Frac chosen = (out->cfr && rfr_ok) ? rfr : (afr_ok ? afr : rfr);
  if (chosen.den == 0 || chosen.num <= 0) { if (err) *err = L"帧率未知，无法建立时间线"; return false; }
  out->frame_rate = chosen;
  if (!out->frame_count && out->duration) out->frame_count = llround(out->duration * chosen.value());
  if (!out->duration && out->frame_count) out->duration = out->frame_count / chosen.value();
  if (out->width <= 0 || out->height <= 0) { if (err) *err = L"分辨率未知"; return false; }
  return true;
}

// ---------------------------------------------------------------- 帧流

FrameProc *Ffmpeg::video_stream(const VideoInfo &vi, double seconds, int out_w, int out_h) {
  std::wstring ffexe, ffprobe;
  if (!locate(&ffexe, &ffprobe) || out_w <= 0 || out_h <= 0) return nullptr;
  double dur = vi.duration > 0 ? vi.duration : 1e9;
  // `-ss` 写在 `-i` 前面，比的是容器里的绝对 pts，而工程内部交下来的秒一律「从首帧起算」：
  // 带原点的素材（实测夹具视频流 start_time=0.084001 = 2.014 帧）不加基就落到前一只 GOP。
  // 加基这件事在 VideoInfo::video_pts 里，只有 seek 这类「按秒定位」的门要加；
  // 导出那一路是按帧号选的（trim=start_frame），第 0 帧天生就是源内第 0 帧，不需要这个数。
  const double rel = seconds < 0 ? 0.0 : (seconds > dur ? dur : seconds);
  wchar_t ts[32];
  swprintf(ts, 32, L"%.6f", vi.video_pts(rel));
  wchar_t vf[64];
  swprintf(vf, 64, L"scale=%d:%d:flags=bilinear", out_w, out_h);
  std::vector<std::wstring> args = {L"-v", L"error",   L"-nostdin", L"-ss",  ts,
                                    L"-i", vi.path,   L"-an",        L"-sn",  L"-dn",
                                    L"-vf", vf,       L"-f",         L"rawvideo",
                                    L"-pix_fmt", L"bgra", L"pipe:1"};
  auto *p = new FrameProc();
  std::wstring err;
  if (!p->start(ffexe, args, &err)) { p->kill(); delete p; return nullptr; }
  return p;
}

FrameProc *Ffmpeg::audio_mix(const std::vector<MixInput> &voices, int sample_rate) {
  std::wstring ffexe, ffprobe;
  if (!locate(&ffexe, &ffprobe) || voices.empty()) return nullptr;
  const int rate = sample_rate > 0 ? sample_rate : 48000;
  auto fnum = [](double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%.6f", v);
    return std::wstring(b);
  };

  // 先把源窗口收到素材真实长度里，再扔掉空的：引用一个不存在的音频流会让整张图直接失败。
  struct Cut {
    const VideoInfo *vi;
    double from, src_dur, speed;
  };
  std::vector<Cut> cuts;
  for (const MixInput &m : voices) {
    if (!m.vi || !m.vi->has_audio) continue;
    const double dur = m.vi->duration > 0 ? m.vi->duration : 1e9;
    const double from = std::clamp(m.from, 0.0, dur);
    const double until = std::clamp(m.until, from, dur);
    if (until - from <= 0.0) continue;
    cuts.push_back(Cut{m.vi, from, until - from, m.speed > 0.0 ? m.speed : 1.0});
  }
  if (cuts.empty()) return nullptr;

  std::vector<std::wstring> args = {L"-v", L"error", L"-nostdin"};
  std::wstring graph, pads;
  for (size_t i = 0; i < cuts.size(); ++i) {
    const Cut &c = cuts[i];
    // -ss 前置：从源起点开始解码，于是 atrim 只需按「段内还要多少源秒」收尾。
    // 加的是音频流自己的基（不是视频那条）：AAC 的 priming 能把两条流错开，加错基就是
    // 听到的和看到的差几帧。后面的 atrim 不用再加 —— -ss 已经把时间基挪到这一段的 0 点。
    args.insert(args.end(), {L"-ss", fnum(c.vi->audio_pts(c.from)), L"-i", c.vi->path});
    std::wstring f = L"[" + std::to_wstring(i) + L":a]atrim=start=0:end=" + fnum(c.src_dur) +
                     L",asetpts=PTS-STARTPTS";
    // asetrate 只是改写采样率标签：以 rate 播放就是 speed 倍速、音高同步变，aresample
    // 再把标签拉回设备采样率。speed=1 时完全不插这两级。
    if (std::fabs(c.speed - 1.0) > 1e-6) {
      const int src = c.vi->audio_rate > 0 ? c.vi->audio_rate : 48000;
      f += L",asetrate=" + std::to_wstring(int64_t(std::lround(double(src) * c.speed))) +
           L",aresample=" + std::to_wstring(rate);
    }
    // 各路都先摊成同一个采样率和声道布局，amix 才肯把它们当同一格式的和。
    f += L",aformat=sample_rates=" + std::to_wstring(rate) + L":channel_layouts=stereo";
    f += L"[a" + std::to_wstring(i) + L"]";
    graph += f;
    pads += L"[a" + std::to_wstring(i) + L"]";
    if (i + 1 < cuts.size()) graph += L";";
  }
  std::wstring out;
  if (cuts.size() == 1) {
    out = pads;
  } else {
    // duration=longest + dropout_transition=0：先结束的那路立刻退场、不留残音，也不按
    // 路数衰减（normalize=0 就是逐样本直加），和 Blender 渲染时混叠音条一致
    // （手册 video_editing/edit/montage/strips/sound.rst:24-29）。
    graph += L";" + pads + L"amix=inputs=" + std::to_wstring(int(cuts.size())) +
             L":duration=longest:dropout_transition=0:normalize=0[mix]";
    out = L"[mix]";
  }
  const std::wstring rs = std::to_wstring(rate);
  args.insert(args.end(), {L"-filter_complex", graph, L"-map", out, L"-ac", L"2", L"-ar", rs,
                           L"-f", L"f32le", L"pipe:1"});
  auto *p = new FrameProc();
  std::wstring err;
  if (!p->start(ffexe, args, &err)) {
    p->kill();
    delete p;
    return nullptr;
  }
  return p;
}

// ---------------------------------------------------------------- 缓存

MediaCache &MediaCache::instance() {
  static MediaCache cache;
  return cache;
}

VideoInfo *MediaCache::find(const std::wstring &path) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = map_.find(path);
  return it == map_.end() ? nullptr : it->second.get();
}

VideoInfo *MediaCache::open(const std::wstring &path, std::wstring *err) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = map_.find(path);
    if (it != map_.end()) return it->second.get();
  }
  auto vi = std::make_unique<VideoInfo>();
  if (!Ffmpeg::probe(path, vi.get(), err)) return nullptr;
  std::lock_guard<std::mutex> lk(mu_);
  auto inserted = map_.emplace(path, std::move(vi));
  return inserted.first->second.get();
}
