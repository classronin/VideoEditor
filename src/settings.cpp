// 「设置」的实现：一个纯文本文件，键值一行一条。
// 为什么是文件而不是注册表：需求要的是「保存启动默认 / 恢复启动默认」这一对操作，
// 删文件 = 恢复默认，语义最直接，也最容易在自动化验证里核对（读回来一行行比对即可）。
#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app.h"

namespace app {
namespace {
constexpr int kMinWinW = 400;   // 小于这个宽度的窗口矩形不当它是真的（多半是读坏了）
constexpr int kMinWinH = 300;

// 「panel」那一行：左侧每一组标题左边的小三角各一位，1=展开，顺序就是面板自上而下的组序。
std::string panel_line(const App &a) {
  std::string s = "panel";
  for (int i = 0; i < kPanelCount; i++) s += ' ' + std::to_string(int(a.panel_open[i]));
  return s;
}

std::wstring local_app_data() {
  wchar_t buf[MAX_PATH];
  if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) return std::wstring();
  return std::wstring(buf);
}
}  // namespace

std::wstring settings_path() {
  const std::wstring base = local_app_data();
  return base.empty() ? std::wstring() : base + L"\\VSE\\settings.txt";
}

// 一行一个键，读不懂的行直接跳过：老版本的设置文件不该让新程序起不来。
void settings_load(App &a) {
  const std::wstring p = settings_path();
  FILE *f = p.empty() ? nullptr : _wfopen(p.c_str(), L"rb");
  if (!f) {
    trace(L"settings load: 无文件 ⇒ 全部走出厂默认");
    return;
  }
  OutputFormat &m = a.project.fmt;
  char line[256];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    int i0 = 0, i1 = 0, i2 = 0, i3 = 0, i4 = 0;
    double d0 = 0.0, d1 = 0.0;
    if (sscanf(line, "window %d %d %d %d %d", &i0, &i1, &i2, &i3, &i4) == 5) {
      if (i3 >= kMinWinW && i4 >= kMinWinH) {
        // 存过的那块屏幕拔掉以后，矩形可能整个落在桌面外（看得见日志看不见窗口）。
        // 不丢弃，只把它拉回虚拟桌面里：宽高照存，位置夹到还能抓到标题栏。
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        a.win_saved = true;
        a.win_max = i0 != 0;
        a.win_w = i3;
        a.win_h = i4;
        a.win_x = std::min(std::max(i1, vx - i3 + 100), std::max(vx, vx + vw - 100));
        a.win_y = std::min(std::max(i2, vy), std::max(vy, vy + vh - 40));
      }
    } else if (sscanf(line, "resolution %d %d", &i0, &i1) == 2) {
      if (i0 > 0 && i1 > 0) {
        m.width = i0;
        m.height = i1;
      }
    } else if (sscanf(line, "aspect %lf %lf", &d0, &d1) == 2) {
      // xasp/yasp 的 RNA 范围是 1..200（rna_scene.cc:7452 起），越界当读坏了丢掉。
      if (d0 >= 1.0 && d0 <= 200.0 && d1 >= 1.0 && d1 <= 200.0) {
        m.par_x = d0;
        m.par_y = d1;
      }
    } else if (sscanf(line, "fps %d %lf", &i0, &d0) == 2) {
      if (i0 > 0 && d0 > 0.0) {
        m.fps = i0;
        m.fps_base = d0;
      }
    } else if (sscanf(line, "crf %d", &i0) == 1) {
      // -1 = 恒定码率（FFM_CRF_NONE）是合法档位，CRF 本身只收 0..51。
      if (i0 == -1 || (i0 >= 0 && i0 <= 51)) m.crf = i0;
    } else if (sscanf(line, "speed %d", &i0) == 1) {
      // eFFMpegPreset 只有三个数：GOOD 10 / BEST 11 / REALTIME 12，读坏就当没有这一行。
      if (i0 == 10 || i0 == 11 || i0 == 12) m.speed = i0;
    } else if (sscanf(line, "bitrate %d", &i0) == 1) {
      // kbit/s，0 = 不发码率（Blender 的新场景默认）；负数读坏，当没有这一行。
      if (i0 >= 0) m.bitrate = i0;
    } else if (sscanf(line, "no_audio %d", &i0) == 1) {
      m.no_audio = i0 != 0;
    } else if (sscanf(line, "lock %d %d", &i0, &i1) == 2) {
      // XY 成对锁：位置、缩放各一位。出厂默认都是锁着，所以只有显式写了 0 才解锁。
      a.lock_pos = i0 != 0;
      a.lock_scale = i1 != 0;
    } else if (sscanf(line, "layout %d %d %d %d", &i0, &i1, &i2, &i3) >= 3) {  // 左侧面板折叠 + 两条分隔条 + 面板在哪一边。
      // 存的是像素，读回来仍由 layout() 夹到合法区间，所以这里只挡「明显读坏了」（<=0）。
      // 第 4 个字段是后加的：老清单只有 3 个数，i3 保持 0 = 面板放左（出厂默认）。
      a.sidebar_on = i0 != 0;
      if (i1 > 0) a.props_px = float(i1);
      if (i2 > 0) a.tl_px = float(i2);
      a.props_side = i3 != 0;
    } else if (strncmp(line, "panel ", 6) == 0) {
      // 六组标题左边的小三角（格式/变换/裁切/速度/帧范围/输出），一位一组、1=展开。
      // 出厂全展开，所以行里缺的格子保持展开，多余的格子忽略。
      const char *q = line + 6;
      for (int i = 0; i < kPanelCount; i++) {
        char *end = nullptr;
        const long v = strtol(q, &end, 10);
        if (end == q) break;
        a.panel_open[i] = v != 0;
        q = end;
      }
    } else if (sscanf(line, "grid %d", &i0) == 1) {
      // 出厂默认是开（做跟踪要靠它找画面中心），所以只有显式写了 0 才关。
      a.preview_grid = i0 != 0;
    } else {
      continue;
    }
    ++n;
  }
  fclose(f);
  wchar_t b[320];
  swprintf(b, 320,
           L"settings load: %d 项生效 窗口=%d %d,%d %dx%d 最大化=%d 分辨率=%dx%d 宽高比=%.3f/%.3f "
           L"帧率=%d/%.3f CRF=%d 速度=%d 码率=%d 无音频=%d 面板=%d 宽=%d 时间线高=%d 面板边=%d 位置锁=%d 缩放锁=%d "
           L"网格=%d 小三角=%d%d%d%d%d%d",
           n, int(a.win_saved), a.win_x, a.win_y, a.win_w, a.win_h, int(a.win_max), m.width,
           m.height, m.par_x, m.par_y, m.fps, m.fps_base, m.crf, m.speed, m.bitrate,
           int(m.no_audio),
           int(a.sidebar_on), int(a.props_px), int(a.tl_px), int(a.props_side), int(a.lock_pos),
           int(a.lock_scale), int(a.preview_grid), int(a.panel_open[0]), int(a.panel_open[1]),
           int(a.panel_open[2]), int(a.panel_open[3]), int(a.panel_open[4]), int(a.panel_open[5]));
  trace(b);
}

void settings_save(App &a) {
  const std::wstring base = local_app_data();
  if (base.empty()) {
    set_status(a, L"保存失败：找不到本地应用数据目录");
    return;
  }
  CreateDirectoryW((base + L"\\VSE").c_str(), nullptr);  // 已存在的失败不用管
  const std::wstring p = settings_path();
  FILE *f = _wfopen(p.c_str(), L"wb");
  if (!f) {
    set_status(a, L"保存失败：写不了 " + p);
    trace(L"settings save: 打不开文件 " + p);
    return;
  }
  fprintf(f, "vse.settings 1\n");
  // 存的是「还原态」矩形 + 当前是不是最大化：最大化状态下 GetWindowRect 拿到的是
  // 盖住任务栏的那块，退出后没法还原，所以走 WINDOWPLACEMENT::rcNormalPosition。
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  if (a.hwnd && GetWindowPlacement(a.hwnd, &wp) && wp.showCmd != SW_HIDE &&
      wp.rcNormalPosition.right > wp.rcNormalPosition.left) {
    const RECT &r = wp.rcNormalPosition;
    fprintf(f, "window %d %d %d %d %d\n", wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0, int(r.left),
            int(r.top), int(r.right - r.left), int(r.bottom - r.top));
  }
  const OutputFormat &m = a.project.fmt;
  fprintf(f, "resolution %d %d\n", m.width, m.height);
  fprintf(f, "aspect %.6f %.6f\n", m.par_x, m.par_y);
  fprintf(f, "fps %d %.6f\n", m.fps, m.fps_base);
  fprintf(f, "crf %d\n", m.crf);
  fprintf(f, "speed %d\n", m.speed);
  fprintf(f, "bitrate %d\n", m.bitrate);
  fprintf(f, "no_audio %d\n", m.no_audio ? 1 : 0);
  // 磁盘代理池的三位参数不再进设置文件：界面里没有它的开关和档位（那四行已删），
  // 一条只有代码能读的持久项等于一个悄悄生效的后门。旧文件里的 disk_proxy 行会被忽略。
  // XY 成对锁（位置、缩放各一位）：这是「怎么编辑」的偏好，跟条带数据无关，所以进默认清单。
  fprintf(f, "lock %d %d\n", int(a.lock_pos), int(a.lock_scale));
  fprintf(f, "grid %d\n", int(a.preview_grid));  // 顶栏「网络」：预览画幅上的等分网格
  // 面板折叠状态 + 两条分隔条拖出来的像素宽度（竖的是属性面板宽，横的是时间线区高）+
  // 面板在哪一边（0=左，1=右）。
  fprintf(f, "layout %d %d %d %d\n", int(a.sidebar_on), int(a.props_px), int(a.tl_px),
          int(a.props_side));
  fprintf(f, "%s\n", panel_line(a).c_str());  // 每一组标题左边的小三角
  fclose(f);
  wchar_t b[320];
  swprintf(b, 320,
           L"settings save -> %ls（分辨率=%dx%d 帧率=%d/%.3f CRF=%d 速度=%d 码率=%d 无音频=%d 面板=%d 宽=%d "
           L"时间线高=%d 面板边=%d 位置锁=%d 缩放锁=%d 网格=%d 小三角=%d%d%d%d%d%d）",
           p.c_str(), m.width, m.height, m.fps, m.fps_base, m.crf, m.speed, m.bitrate,
           int(m.no_audio),
           int(a.sidebar_on), int(a.props_px), int(a.tl_px), int(a.props_side), int(a.lock_pos),
           int(a.lock_scale), int(a.preview_grid), int(a.panel_open[0]), int(a.panel_open[1]),
           int(a.panel_open[2]), int(a.panel_open[3]), int(a.panel_open[4]), int(a.panel_open[5]));
  trace(b);
  set_status(a, L"已保存启动默认");
}

// 关窗口时的折叠状态落盘：整份读回来，只把 layout 行的第一个数、panel 行整行换成此刻的状态，
// 其余每一行原样写回。文件里没有这两行（或整个文件都还没有）才补，那时分隔条宽度与面板边就取
// 此刻的值 —— 除此之外不新增任何键，所以这一次写盘不会顶掉「保存启动默认」存过的窗口位置或
// 格式参数。
void settings_save_collapse(App &a) {
  const std::wstring p = settings_path();
  if (p.empty()) return;
  std::string text;
  bool have_layout = false, have_panel = false;
  if (FILE *f = _wfopen(p.c_str(), L"rb")) {
    char buf[256];
    while (fgets(buf, sizeof buf, f)) text += buf;
    fclose(f);
  } else {
    text = "vse.settings 1\n";
  }
  const std::string pan = panel_line(a);
  std::string out;
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    const bool last = (e == std::string::npos);
    if (last) e = text.size();
    std::string line = text.substr(i, e - i);
    if (line.rfind("layout", 0) == 0) {
      int px = int(a.props_px), tl = int(a.tl_px), side = int(a.props_side);
      sscanf(line.c_str(), "layout %*d %d %d %d", &px, &tl, &side);  // 后三位照旧，读不到就用此刻的值
      char rep[64];
      snprintf(rep, sizeof rep, "layout %d %d %d %d", int(a.sidebar_on), px, tl, side);
      line = rep;
      have_layout = true;
    } else if (line.rfind("panel", 0) == 0) {
      line = pan;
      have_panel = true;
    }
    out += line;
    if (last) break;
    out += '\n';
    i = e + 1;
  }
  const auto append = [&out](const std::string &line) {
    if (!out.empty() && out.back() != '\n') out += '\n';
    out += line;
    out += '\n';
  };
  if (!have_layout) {
    char rep[64];
    snprintf(rep, sizeof rep, "layout %d %d %d %d", int(a.sidebar_on), int(a.props_px),
             int(a.tl_px), int(a.props_side));
    append(rep);
  }
  if (!have_panel) append(pan);
  CreateDirectoryW((local_app_data() + L"\\VSE").c_str(), nullptr);  // 已存在的失败不用管
  FILE *w = _wfopen(p.c_str(), L"wb");
  if (!w) {
    trace(L"settings collapse save: 写不了 " + p);
    return;
  }
  fwrite(out.data(), 1, out.size(), w);
  fclose(w);
  wchar_t b[200];
  swprintf(b, 200, L"settings collapse save: 折叠=%d 小三角=%d%d%d%d%d%d -> %ls", int(a.sidebar_on),
           int(a.panel_open[0]), int(a.panel_open[1]), int(a.panel_open[2]), int(a.panel_open[3]),
           int(a.panel_open[4]), int(a.panel_open[5]), p.c_str());
  trace(b);
}

void settings_reset(App &a) {
  const std::wstring p = settings_path();
  const bool deleted = !p.empty() && DeleteFileW(p.c_str()) != FALSE;
  trace(std::wstring(L"settings reset: 删除=") + (deleted ? L"1" : L"0") + L" " + p);
  set_status(a, deleted ? L"已恢复启动默认（下次启动生效）" : L"没有保存过启动默认");
}

}  // namespace app
