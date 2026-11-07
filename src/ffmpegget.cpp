// ffmpeg 自助安装：WinHTTP 下载 + 系统自带 bsdtar（C:\Windows\System32\tar.exe）解包。
// 为什么用 tar 而不是自己解 zip：Windows 10 1803 起自带 bsdtar，它认 zip，
// 而且能只解我们要的两个成员（full_build 压缩包里有几百 MB 别的东西）。
#include "ffmpegget.h"

#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "app.h"
#include "media.h"
#include "waveform.h"

namespace app {
namespace {

// gyan.dev 官方建议的先查版本号再拼下载地址（项目目标.md:18-25）。
constexpr const wchar_t *kVersionUrl = L"https://www.gyan.dev/ffmpeg/builds/release-version";
constexpr const wchar_t *kGithubBase = L"https://github.com/GyanD/codexffmpeg/releases/download/";
// 版本号问不到时的兜底：本工程所有实测数字都出自这一版。
constexpr const wchar_t *kFallbackVersion = L"9.0.2";

struct Source {
  const wchar_t *name;
  const wchar_t *prefix;  // 加速代理把完整 https 地址拼在自己后面
};
const Source kSources[] = {
    {L"ghfast.top 镜像", L"https://ghfast.top/"},
    {L"gh-proxy.org 镜像", L"https://v6.gh-proxy.org/"},
    {L"github.com 直连", L""},
};

std::wstring exe_dir() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf, n);
  const size_t s = p.find_last_of(L"\\/");
  std::wstring d = s == std::wstring::npos ? std::wstring(L".") : p.substr(0, s);
  for (auto &c : d) if (c == L'/') c = L'\\';
  return d;
}

// --ffmpeg-url 的存放处：主线程写（解析命令行），工作线程读。
std::mutex &url_mu() {
  static std::mutex m;
  return m;
}
std::wstring &url_override() {
  static std::wstring s;
  return s;
}
std::wstring url_override_copy() {
  std::lock_guard<std::mutex> lk(url_mu());
  return url_override();
}

HINTERNET session() {
  static HINTERNET h = WinHttpOpen(L"vse/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  return h;
}

// 一次 GET。dest 非空 = 直接落盘；body 非空 = 收进内存。done/total 回写下载进度。
bool http_get(const std::wstring &url, const wchar_t *dest, std::string *body,
              std::atomic<long long> *done, std::atomic<long long> *total, std::wstring *err) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof uc;
  wchar_t host[256]{};
  wchar_t path[1024]{};
  wchar_t extra[512]{};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 1023;
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 511;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
    if (err) *err = L"地址解析失败：" + url;
    return false;
  }
  const std::wstring target = std::wstring(path) + extra;
  const HINTERNET conn = WinHttpConnect(session(), host, uc.nPort, 0);
  if (!conn) {
    if (err) *err = L"连不上 " + std::wstring(host);
    return false;
  }
  const HINTERNET req =
      WinHttpOpenRequest(conn, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    if (err) *err = L"发不出请求";
    WinHttpCloseHandle(conn);
    return false;
  }
  // 镜像挂了就快速失败，好让人换下一个：连接 15s、收数据 30s 空档。
  WinHttpSetTimeouts(req, 15000, 15000, 30000, 30000);
  bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                               0, 0) &&
            WinHttpReceiveResponse(req, nullptr);
  if (!ok && err) {
    // 这两步失败时上面那个 `if (ok)` 整块都不会走，err 一直是空的 ——
    // 状态条就只能显示「下载失败：」，什么信息都没有。把 Win32 错误码带上。
    *err = std::wstring(L"连不上 ") + host + L"（Win32 错误 " + std::to_wstring(GetLastError()) +
           L"）";
  }
  long long want_total = -1;
  if (ok) {
    DWORD status = 0, cb = sizeof status;
    ok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX) &&
         status >= 200 && status < 300;
    if (!ok && err) {
      wchar_t b[96];
      swprintf(b, 96, L"HTTP %u：%ls%ls", status, host, target.c_str());
      *err = b;
    }
  }
  if (ok) {
    DWORD n = 0, cb = sizeof n;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &n, &cb, WINHTTP_NO_HEADER_INDEX)) {
      want_total = n;
      if (total) total->store(n);
    }
  }
  HANDLE file = INVALID_HANDLE_VALUE;
  if (ok && dest) {
    file = CreateFileW(dest, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      ok = false;
      if (err) *err = std::wstring(L"写不了 ") + dest;
    }
  }
  char buf[1 << 18];
  long long got = 0;
  while (ok) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail)) {
      ok = false;
      if (err)
        *err = L"问还有多少数据失败（Win32 错误 " + std::to_wstring(GetLastError()) + L"）";
      break;
    }
    if (!avail) break;  // 服务器发完了
    const DWORD want = avail > sizeof buf ? DWORD(sizeof buf) : avail;
    DWORD rd = 0;
    if (!WinHttpReadData(req, buf, want, &rd) || !rd) {
      ok = false;
      if (err) *err = L"连接中途断了";
      break;
    }
    if (body) body->append(buf, rd);
    // 判据必须是 != INVALID_HANDLE_VALUE 而不是「非零」：INVALID_HANDLE_VALUE 就是 (void*)-1，
    // 当 bool 用是真。写进内存的那次调用（dest=nullptr 查版本号）会因此拿着 -1 去 WriteFile，
    // 失败后拼错误信息时又给 std::wstring + dest(nullptr)，wcslen(0) 直接把进程带走。
    if (file != INVALID_HANDLE_VALUE) {
      DWORD wr = 0;
      if (!WriteFile(file, buf, rd, &wr, nullptr) || wr != rd) {
        ok = false;
        if (err) *err = L"磁盘写入失败：" + std::wstring(dest);  // 走到这里 dest 必非空
        break;
      }
    }
    got += rd;
    if (done) done->store(got);
  }
  // 半截 zip 交给 tar 只会报一句看不懂的「不是有效的 zip」，所以按 Content-Length 收口。
  if (ok && want_total >= 0 && got != want_total) {
    ok = false;
    if (err) *err = L"只收到 " + std::to_wstring(got) + L" / " + std::to_wstring(want_total) +
                          L" 字节";
  }
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  if (!ok && dest) DeleteFileW(dest);
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);
  return ok;
}

// 等一个子进程跑完（tar 解压）。窗口模式下看不到控制台，所以 CREATE_NO_WINDOW + 隐藏。
bool run_wait(const std::wstring &cmd, std::wstring *err) {
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring line = cmd;
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    if (err) *err = L"启动 tar.exe 失败";
    return false;
  }
  Ffmpeg::attach_to_job(pi.hProcess);  // 主程序被结束时也别把 tar.exe 留在系统里
  WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0 && err) {
    wchar_t b[96];
    swprintf(b, 96, L"解压失败（tar 退出码 %u）", unsigned(code));
    *err = b;
  }
  return code == 0;
}

// zip 里的成员是 ffmpeg-<版本>-full_build/bin/ffmpeg.exe，版本号会变，所以要往下钻。
// 不能用 dir + L"\\*\\bin\\" + name 一把梭：FindFirstFileW 的通配符只在最后一段有效，
// 中间那层目录名写 `*` 直接查不到（实测解出来的文件明明在，返回却是没有）。
bool walk_find(const std::wstring &dir, const wchar_t *name, int depth, std::wstring *out) {
  WIN32_FIND_DATAW fd{};
  const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool found = false;
  do {
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
    const std::wstring p = dir + L"\\" + fd.cFileName;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (depth > 0 && walk_find(p, name, depth - 1, out)) {
        found = true;
        break;
      }
    } else if (!wcscmp(fd.cFileName, name)) {
      *out = p;
      found = true;
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

std::wstring find_extracted(const std::wstring &dir, const wchar_t *name) {
  std::wstring out;
  return walk_find(dir, name, 4, &out) ? out : std::wstring();
}

// 递归删掉我们自己在临时目录里解出来的那棵树。root 只可能是 kUnpackDir 拼出来的路径，
// 传进来的别的任何东西都不该走到这里。
void remove_tree(const std::wstring &root) {
  WIN32_FIND_DATAW fd{};
  const HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
      const std::wstring p = root + L"\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
      else DeleteFileW(p.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(root.c_str());
}

std::wstring temp_dir() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetTempPathW(MAX_PATH, buf);
  std::wstring d(buf, n);
  if (!d.empty() && d.back() != L'\\') d += L'\\';
  return d;
}

void worker() {
  FFmpegFetch &f = ffmpeg_fetch();
  const std::wstring dir = exe_dir();
  std::wstring err;
  std::string body;

  std::wstring version = kFallbackVersion;
  const std::wstring over = url_override_copy();
  if (over.empty()) {
    f.state = kFFVersion;
    if (http_get(kVersionUrl, nullptr, &body, nullptr, nullptr, &err)) {
      std::wstring v;
      for (unsigned char c : body) if (c > 0x20) v += wchar_t(c);
      if (!v.empty()) version = v;
    }
    err.clear();
  } else {
    version = L"（覆盖地址）";
  }
  const std::wstring file = L"ffmpeg-" + version + L"-full_build.zip";

  std::wstring url = over;
  bool ok = false;
  // 菜单上只有一颗「下载FFmpeg」，镜像就不让用户挑了：按项目目标.md 的顺序
  // （两个加速代理 → github 直连）一家一家试，哪家下下来就停。
  constexpr int kNumSources = int(sizeof(kSources) / sizeof(kSources[0]));
  for (int attempt = 0; attempt < (over.empty() ? kNumSources : 1) && !ok; ++attempt) {
    if (over.empty())
      url = std::wstring(kSources[attempt].prefix) + kGithubBase + version + L"/" + file;
    {
      wchar_t b[320];
      swprintf(b, 320, L"ffmpeg 下载（版本=%ls）：从 %ls，地址=%ls", version.c_str(),
               over.empty() ? kSources[attempt].name : L"覆盖地址", url.c_str());
      trace(b);
    }
    std::wstring zip = temp_dir() + url.substr(url.find_last_of(L"/\\") + 1);
    f.done = 0;
    f.total = 0;
    f.state = kFFDownload;
    ok = http_get(url, zip.c_str(), nullptr, &f.done, &f.total, &err);
    if (!ok) continue;  // 下一家镜像
    f.state = kFFExtract;
    // 解到临时目录里的一个专属文件夹，再把两个 exe 搬到主程序旁边：
    // 直接解进 exe 目录的话，事后没法分清哪些目录是这次解压出来的。
    const std::wstring unpack = temp_dir() + L"vse-ffmpeg-unpack";
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    remove_tree(unpack);
    std::wstring cmd = std::wstring(sysdir) + L"\\tar.exe -xf \"" + zip + L"\" -C \"" + unpack +
                       L"\" \"*/bin/ffmpeg.exe\" \"*/bin/ffprobe.exe\"";
    ok = run_wait(cmd, &err);
    if (!ok) {
      // tar 不会替你建目标目录。
      CreateDirectoryW(unpack.c_str(), nullptr);
      err.clear();
      ok = run_wait(cmd, &err);
    }
    if (ok) {
      const std::wstring got_ff = find_extracted(unpack, L"ffmpeg.exe");
      const std::wstring got_pr = find_extracted(unpack, L"ffprobe.exe");
      if (got_ff.empty() || got_pr.empty()) {
        ok = false;
        err = L"解压完没找到 ffmpeg.exe / ffprobe.exe";
      } else {
        for (const auto &pair : {std::make_pair(got_ff, L"ffmpeg.exe"),
                                 std::make_pair(got_pr, L"ffprobe.exe")}) {
          const std::wstring dst = dir + L"\\" + pair.second;
          DeleteFileW(dst.c_str());
          ok = MoveFileW(pair.first.c_str(), dst.c_str()) != FALSE;
          if (!ok) err = L"搬不动 " + dst;
        }
      }
    }
    remove_tree(unpack);
    DeleteFileW(zip.c_str());
    // 解压/搬运失败不是某一家镜像的问题（压缩包已经在本地盘上了），换镜像也没用：停。
  }
  if (ok) {
    f.note = version;
    f.state = kFFDone;
  } else {
    f.note = err;
    f.state = kFFFailed;
  }
}

}  // namespace

FFmpegFetch &ffmpeg_fetch() {
  static FFmpegFetch f;
  return f;
}

bool ffmpeg_fetch_busy() {
  const int st = ffmpeg_fetch().state;
  return st == kFFVersion || st == kFFDownload || st == kFFExtract;
}

bool ffmpeg_fetch_active() {
  return ffmpeg_fetch().state != kFFIdle;
}

void ffmpeg_fetch_start() {
  if (ffmpeg_fetch_busy()) return;
  FFmpegFetch &f = ffmpeg_fetch();
  f.note.clear();
  f.done = 0;
  f.total = 0;
  f.state = kFFVersion;
  std::thread(worker).detach();
}

void ffmpeg_fetch_tick(App &a) {
  FFmpegFetch &f = ffmpeg_fetch();
  const int st = f.state;
  if (st == kFFIdle) return;
  wchar_t b[160];
  switch (st) {
    case kFFVersion:
      set_status(a, L"正在问 gyan.dev 当前 ffmpeg 版本…");
      break;
    case kFFDownload: {
      const double mb = double(f.done) / (1024.0 * 1024.0);
      const long long total = f.total;
      if (total > 0) {
        swprintf(b, 160, L"下载 ffmpeg：%.1f / %.1f MB（%.0f%%）", mb,
                 double(total) / (1024.0 * 1024.0), double(f.done) * 100.0 / double(total));
      } else {
        swprintf(b, 160, L"下载 ffmpeg：%.1f MB", mb);
      }
      set_status(a, b);
      break;
    }
    case kFFExtract:
      set_status(a, L"解压 ffmpeg.exe / ffprobe.exe…");
      break;
    case kFFDone: {
      f.state = kFFIdle;
      std::wstring from;
      a.has_ffmpeg = Ffmpeg::locate(nullptr, nullptr, &from);
      // 之前因为「找不到 ffmpeg」而判失败的声波烘焙要重新放行，否则老素材永远没有波形。
      WaveCache::instance().reset();
      set_status(a, L"ffmpeg " + f.note + (a.has_ffmpeg ? L" 已装好，可以直接用了"
                                                        : L" 已下载，但还是找不到可执行文件"));
      trace(L"ffmpeg 安装完成：" + f.note + L"（定位 " + from + L"）");
      a.dirty = kRegionAll;  // 「设置」里那几条下载源该消失了
      break;
    }
    default: {  // kFFFailed
      f.state = kFFIdle;
      set_status(a, L"ffmpeg 下载失败：" + f.note);
      trace(L"ffmpeg 下载失败：" + f.note);
      break;
    }
  }
}

void ffmpeg_set_url_override(const std::wstring &url) {
  std::lock_guard<std::mutex> lk(url_mu());
  url_override() = url;
}

}  // namespace app
