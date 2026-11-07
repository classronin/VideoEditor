#include "export.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <cstdio>

// ops.cpp 里的 VSE_TRACE 落盘函数（export.cpp 不引 app.h，避免循环包含）。
namespace app {
void trace(const std::wstring &s);
double now_seconds();
}
using app::now_seconds;
using app::trace;

namespace {

// 环境开关的读法与 VSE_MF / VSE_PROBE_NOCACHE 一致：只看第一个字符，'1' 才算开。
bool env_flag(const wchar_t *name) {
  wchar_t buf[8] = {0};
  GetEnvironmentVariableW(name, buf, 8);
  return buf[0] == L'1';
}

// 本进程累计 CPU（user+sys 毫秒）。两次读数相减就是「这一段窗口里我们自己花了多少」，
// 用来把界面线程陪跑（读进度管道、刷状态栏）的账和 ffmpeg 的账分开。
// kill switch: 不过门即删，不优化
double self_cpu_ms() {
  FILETIME creation, exit_, user, kernel;
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_, &kernel, &user)) return 0.0;
  ULARGE_INTEGER u, k;
  u.LowPart = user.dwLowDateTime;
  u.HighPart = user.dwHighDateTime;
  k.LowPart = kernel.dwLowDateTime;
  k.HighPart = kernel.dwHighDateTime;
  return double(u.QuadPart + k.QuadPart) / 1e4;
}

std::wstring fnum(double v) {
  wchar_t b[64];
  swprintf(b, 64, L"%.6f", v);
  return b;
}

// 滤镜表达式里的时间常数：%.6f 够写像素和角度，不够写「每只 tick 走几只源帧」这种比率
// —— 差 1e-6 就够把某一 tick 的落位挪一只帧。定点 17 位再去掉尾巴的 0（%g 会写成
// 1e-09，表达式的解析器不认这种）。
std::wstring fnumx(double v) {
  wchar_t b[64];
  swprintf(b, 64, L"%.17f", v);
  std::wstring s = b;
  if (s.find(L'.') == std::wstring::npos) return s;
  size_t e = s.find_last_not_of(L'0');
  if (s[e] == L'.') --e;
  s.erase(e + 1);
  return s;
}

// yuv420p 要求偶数边长
long long even_size(double v) {
  long long n = llround(v);
  if (n & 1) --n;
  return n < 2 ? 2 : n;
}

long long clampi(long long v, long long lo, long long hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// 一段恒速切片再按分段边界切开（from/to 是时间线绝对秒，落在切片内部）。
// 段速是常数，所以源时刻线性推；retiming 曲线跟着换成两端的新键，下游一律当普通条带读。
Clip sub_slice(const Clip &c, double from, double to) {
  Clip w = c;
  const double spd = retiming_speed_at(c, c.duration);
  const double a = std::max(0.0, from - c.timeline_start);
  const double b = std::min(c.duration, to - c.timeline_start);
  w.timeline_start = c.timeline_start + a;
  w.duration = b - a;
  w.source_in = c.source_in + a * spd;
  w.retiming = {{0.0, w.source_in}, {w.duration, w.source_in + (b - a) * spd}};
  return w;
}

// 逐帧求值的表达式：调用方（C++）每帧算一次值，这里把这条折线编成一棵平衡的 if 树。
// 为什么可以只按「帧」采样：ffmpeg 每收到一个输入帧才对 pos_* 求值一次，帧号就是 n ⇒
// 叶子写成「对 n 的线性式」在每个整数 n 上恰好等于我们算的那个值，非整数时刻根本不会被读到。
// 平衡（而不是右深链）是为了把解析递归压到 log2(帧数)：250 帧 = 深度 8，实测可解析。
std::wstring frame_expr(const std::vector<double> &v) {
  if (v.empty()) return L"0";
  std::function<std::wstring(int, int)> tree = [&](int a, int b) -> std::wstring {
    if (b - a == 1) {
      if (a + 1 >= int(v.size()) || std::fabs(v[a + 1] - v[a]) < 1e-12) return fnum(v[a]);
      return fnum(v[a]) + L"+" + fnum(v[a + 1] - v[a]) + L"*(n-" + std::to_wstring(a) + L")";
    }
    const int m = (a + b) / 2;
    return L"if(lt(n," + std::to_wstring(m) + L")," + tree(a, m) + L"," + tree(m, b) + L")";
  };
  return tree(0, int(v.size()));
}

// 每一 tick 要哪一只源帧，写在图里，不由 fps= 拿时间戳去猜。
// 记号：trim 之后第 n 只 = 源内第 f0+n 只；B = 段速×源帧率÷工程帧率（一只 tick 走几只源帧）；
// d = 切片左缘离整数帧格差的那点（∈[-0.5,0.5)）。预览在那一 tick 交的是 round(d + k·B)
// （VideoInfo::src_frame，同一个所有者），反解回来就是「第 n 只服务第 K(n)=ceil((n−0.5−d)/B)
// 只 tick，且 K(n) < (n+0.5−d)/B 才算真有一 tick 归它」。setpts 把留下的每一只钉在
// K(n)/工程帧率 那一秒上，之后的 fps= 只剩「空 tick 补前一只」（慢放要重复帧）可做 ——
// 选择权收回这一侧。settb=1e-6 是给这次钉位用的：源帧率那种粗时间基（1/59.94）装不下
// 1/25 秒的 tick，差半格就够把一只帧推到下一 tick 去。setpts 认 N、select 认 n，
// 是同一个「收到第几只」在两个滤镜里的两种拼法。
// 括号把除法关在 ceil 里面：写成 ceil(n−0.5)/B 是「先取整再除」，那就是另一个函数了
// （而且它几乎恒真 —— 图照样能跑通、帧一只不掉，只有逐帧量像素才看得出）。
std::wstring tick_place(double d, double b, double fr, long long ntick) {
  const std::wstring B = L"(" + fnumx(b) + L")", lo = fnumx(0.5 + d), hi = fnumx(0.5 - d);
  auto k = [&](wchar_t v) {
    return L"max(0,ceil(((" + std::wstring(1, v) + L")-(" + lo + L"))/" + B + L"))";
  };
  const std::wstring kn = k(L'n');
  return L",settb=1/1000000,setpts='" + k(L'N') + L"/" + fnumx(fr) + L"/TB'" +
         L",select='lte(" + kn + L"," + std::to_wstring(ntick - 1) + L")*lt(" + kn + L",((n)+(" +
         hi + L"))/" + B + L")'";
}

// 段尾空着几格 tick。tick_place 把每只帧钉在它「服务的第一只 tick」，之后的重复交给 fps=；
// 但 fps= 只在后面还有帧可看时才补得动 —— 最后一只钉早了，它到末 tick 之间那几格没人补，
// 段尾就露出底色。而那一截预览交的正是这最后一只：一只帧顶到的最后一格是
// ceil((n+0.5−d)/B)−1，末 tick 要的帧也正是算 f1 用的那只。差几格补几格 ——
// 只有慢放（一只源帧摊到多只 tick）会空，快进恒为 0。
long long tick_tail(double d, double b, long long n_out, long long n_last) {
  if (n_last < 0) return 0;
  const long long first = std::max(0.0, std::ceil((double(n_last) - 0.5 - d) / b));
  const long long serves = std::ceil((double(n_last) + 0.5 - d) / b) - 1.0;
  return std::min(n_out - 1, serves) - first;
}

// 命令行的上限是 32767 个字符（含所有参数），一张逐帧的图轻松破万，所以留一半给别的参数。
constexpr size_t kInlineGraphLimit = 14000;

std::wstring temp_text_path(const wchar_t *stem) {
  wchar_t dir[MAX_PATH] = {};
  DWORD n = GetTempPathW(MAX_PATH, dir);
  // 不用 swprintf 拼：这里的 %s 会按窄字符串解析 wchar_t*，路径在第一个 0 字节处截断，
  // 只剩一个「C」。
  if (!n || n > MAX_PATH - 1) return std::wstring(stem) + L".txt";
  return std::wstring(dir) + stem + L"_" + std::to_wstring(GetCurrentProcessId()) + L".txt";
}

bool write_text_file(const std::wstring &path, const std::wstring &text) {
  const int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0,
                                       nullptr, nullptr);
  std::vector<char> buf(size_t(need > 0 ? need : 0));
  if (need > 0)
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), buf.data(), need, nullptr,
                        nullptr);
  FILE *f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  const bool ok = buf.empty() || fwrite(buf.data(), 1, buf.size(), f) == buf.size();
  fclose(f);
  return ok;
}

std::wstring fps_expr(int fps, double base) {
  // ffmpeg 的 -r 要有理数。Blender 的帧率本来就是「整数 / 基数」，把基数放大成整数即可：
  // 30 / 1.001 → 30000/1001。
  long long num = std::llround(double(fps) * 1000.0);
  long long den = std::llround(std::max(0.001, base) * 1000.0);
  auto gcd = [](long long a, long long b) {
    while (b) {
      long long t = a % b;
      a = b;
      b = t;
    }
    return a;
  };
  long long g = gcd(num > 0 ? num : 1, den > 0 ? den : 1);
  if (g > 1) {
    num /= g;
    den /= g;
  }
  if (den <= 1) return std::to_wstring(num);
  return std::to_wstring(num) + L"/" + std::to_wstring(den);
}

}  // namespace

bool build_export(const Project &p, const std::wstring &out_path, ExportCommand *cmd,
                  std::wstring *err) {
  auto fail = [&](const std::wstring &m) {
    if (err) *err = m;
    return false;
  };
  // 输出尺寸走格式面板的“缩放 %”（render_size 与预览用的是同一个函数）。
  int rw = 0, rh = 0;
  render_size(p.fmt, &rw, &rh);
  const long long W = rw, H = rh;
  if (W < 16 || H < 16) return fail(L"输出分辨率无效：先导入一段素材");
  const double fps = p.fmt.fps_value();
  if (fps <= 0) return fail(L"输出帧率无效");
  const std::wstring fr = fps_expr(p.fmt.fps, p.fmt.fps_base);

  std::vector<const Clip *> sorted;
  for (const auto &c : p.clips) {
    if (c.track < 0 || c.track >= int(p.tracks.size())) continue;
    if (p.tracks[c.track].muted) continue;  // 通道静音（render.cc:2240-2243）
    if (c.muted) continue;  // SEQ_MUTE（sequencer_edit.cc:1366）：条带不参与渲染
    if (c.is_sound) {
      // 声带不进画面：collection_filter_rendered_strips 第一件事就是否掉 STRIP_TYPE_SOUND
      // （sequencer/intern/iterator.cc:223-225）⇒ 它只贡献音频，画面里那一格是黑的。
      if (!c.info || !c.info->has_audio) continue;  // 没有 bSound 的 SOUND 条带不发声（sound.cc:945-951）
    } else {
      if (!c.info || c.info->width <= 0 || c.info->height <= 0) return fail(L"片段缺少媒体信息");
    }
    sorted.push_back(&c);
  }
  if (sorted.empty()) return fail(L"时间线是空的");
  // 「无音频」= Audio Codec 选 NONE：整条音频图不用搭，容器里也不写音轨。
  const bool want_audio = !p.fmt.no_audio;
  std::sort(sorted.begin(), sorted.end(),
            [](const Clip *a, const Clip *b) { return a->timeline_start < b->timeline_start; });
  // 重叠的条带不拒绝：Blender 就是让它们叠着画（iterator.cc:253-255 按 channel 升序），
  // 下面按帧分段时同一个时刻有几条活跃就叠几层。

  // 渲染区间 = 帧范围：BKE_scene_get_playback_range（scene.cc:2607-2633）在没开预览范围时
  // 就是 sfra..efra，闭区间、共 efra-sfra+1 帧（timeline_init_boundbox 的 xmax = efra + 1
  // 是同一个道理，见 strip_time.cc:321-327）。区间外的片段丢掉，跨界的那段按源时间平移。
  const double fd = 1.0 / fps;
  const double t0 = p.fmt.frame_start * fd;
  const double t1 = (p.fmt.frame_end + 1) * fd;
  if (t1 <= t0) return fail(L"帧范围是空的");
  std::vector<Clip> work;
  for (const Clip *c : sorted) {
    const double s = std::max(t0, c->timeline_start);
    const double e = std::min(t1, c->timeline_start + c->duration);
    if (e - s <= 1e-6) continue;
    // 变速曲线在导出前摊平：每一段单独成一个「恒速」片段（Blender 也是一段一个常速，
    // retiming_sound_animation_data_set:1246-1274 给每段挂一个恒定音程），
    // 所以图里不需要逐帧变帧率，setpts 那一套照旧。
    for (Clip &w : retiming_slices(*c, s - c->timeline_start, e - c->timeline_start))
      work.push_back(std::move(w));
  }
  if (work.empty()) return fail(L"帧范围内没有任何条带：先调大「帧范围-结束」");

  std::vector<std::wstring> chains, vlab, alab;
  auto add_black = [&](double dur, const std::wstring &tag) {
    chains.push_back(L"color=c=black:s=" + std::to_wstring(W) + L"x" + std::to_wstring(H) +
                     L":r=" + fr + L":d=" + fnum(dur) + L"[v" + tag + L"]");
    if (want_audio)
      chains.push_back(L"anullsrc=r=48000:cl=stereo:d=" + fnum(dur) + L"[a" + tag + L"]");
    vlab.push_back(L"[v" + tag + L"]");
    if (want_audio) alab.push_back(L"[a" + tag + L"]");
  };

  // 叠画顺序：Blender 逐帧把「这一刻活跃的条带」按 channel 升序排好再往下画，通道号大的
  // 盖在上面（query_rendered_strips_sorted，iterator.cc:253-255 的排序键只有 channel）。
  // 同一条通道上重叠在 Blender 里是没有定义的（排序只看通道），这里补「起点、id」两把
  // 钥匙只为让每次导出的结果一致。
  std::sort(work.begin(), work.end(), [](const Clip &a, const Clip &b) {
    if (a.track != b.track) return a.track < b.track;
    if (a.timeline_start != b.timeline_start) return a.timeline_start < b.timeline_start;
    return a.id < b.id;
  });

  // 分段：条带可以在任何一帧开始或结束，所以先把帧范围按帧网格切成「活跃集合不变」的若干
  // 段。切点必须在帧边界上：Blender 就是一帧一帧地决定这一帧画谁（iterator.cc:231-242 每
  // 帧重新查一次活跃条带）。段内叠几层就在 ffmpeg 里叠几层，段与段之间照旧 concat。
  const long long nf_total = std::max<long long>(0, p.fmt.frame_end - p.fmt.frame_start + 1);
  const size_t nframes = size_t(nf_total);
  std::vector<std::vector<int>> active_at(nframes);
  for (int i = 0; i < int(work.size()); i++) {
    const Clip &c = work[size_t(i)];
    const long long a =
        clampi(std::llround(c.timeline_start * fps) - p.fmt.frame_start, 0, nf_total);
    const long long b = clampi(
        std::llround((c.timeline_start + c.duration) * fps) - p.fmt.frame_start, 0, nf_total);
    for (long long f = a; f < b; f++) active_at[size_t(f)].push_back(i);
  }

  // 每段的层先算成子切片（段边界把长条带切开），顺手记下它用哪一路输入。
  struct Layer {
    Clip c;
    int src = 0;     // 源文件在 -i 列表里的序号
    int nth = 0;     // 同一个源文件切出来的第几份（画面）
    int nth_a = 0;   // 同上（声音）
    bool uses_audio = false;
    std::wstring vpad, apad;
  };
  std::vector<std::wstring> seg_tag;
  std::vector<std::pair<double, double>> seg_span;
  std::vector<std::vector<Layer>> seg_layers;
  for (long long f = 0; f < nf_total;) {
    long long g = f + 1;
    while (g < nf_total && active_at[size_t(g)] == active_at[size_t(f)]) ++g;
    const double s0 = (double(p.fmt.frame_start) + double(f)) * fd;
    const double s1 = (double(p.fmt.frame_start) + double(g)) * fd;
    const std::vector<int> &ids = active_at[size_t(f)];
    seg_tag.push_back(std::to_wstring(int(seg_tag.size())));
    seg_span.push_back({s0, s1});
    std::vector<Layer> ls;
    for (int i : ids) {
      Layer ly;
      ly.c = sub_slice(work[size_t(i)], s0, s1);
      ls.push_back(std::move(ly));
    }
    seg_layers.push_back(std::move(ls));
    f = g;
  }

  // 同一个源文件只开一路解码：被切成几份就 split/asplit 出几份。不这么做的话每个 trim 都是
  // 从文件头重新解码一遍，底带被上面几条短带切碎时导出会慢得离谱。
  std::vector<std::wstring> srcs;
  std::vector<int> vuse, ause;
  std::map<std::wstring, int> src_of;
  for (auto &ls : seg_layers)
    for (Layer &ly : ls) {
      auto it = src_of.find(ly.c.source_path);
      if (it == src_of.end()) {
        it = src_of.emplace(ly.c.source_path, int(srcs.size())).first;
        srcs.push_back(ly.c.source_path);
        vuse.push_back(0);
        ause.push_back(0);
      }
      ly.src = it->second;
      // 画面切片按源分组，声带切片另起一套计数：一条影片的图像和它的音轨是两条不同的
      // 条带，各自占一路 split/asplit，但共用同一个 -i 输入。
      if (!ly.c.is_sound) ly.nth = vuse[ly.src]++;
      if (want_audio && ly.c.is_sound) {
        ly.uses_audio = true;
        ly.nth_a = ause[ly.src]++;
      }
    }
  for (int s = 0; s < int(srcs.size()); s++) {
    auto pads = [&](int n, const wchar_t *kind) {
      std::wstring t;
      for (int i = 0; i < n; i++)
        t += L"[p" + std::to_wstring(s) + kind + std::to_wstring(i) + L"]";
      return t;
    };
    if (vuse[s] > 1)
      chains.push_back(L"[" + std::to_wstring(s) + L":v]split=" + std::to_wstring(vuse[s]) +
                       pads(vuse[s], L"v"));
    if (want_audio && ause[s] > 1)
      chains.push_back(L"[" + std::to_wstring(s) + L":a]asplit=" + std::to_wstring(ause[s]) +
                       pads(ause[s], L"a"));
  }
  for (auto &ls : seg_layers)
    for (Layer &ly : ls) {
      const std::wstring s = std::to_wstring(ly.src);
      ly.vpad = vuse[ly.src] > 1 ? L"[p" + s + L"v" + std::to_wstring(ly.nth) + L"]"
                                 : L"[" + s + L":v]";
      if (ly.uses_audio)
        ly.apad = ause[ly.src] > 1 ? L"[p" + s + L"a" + std::to_wstring(ly.nth_a) + L"]"
                                   : L"[" + s + L":a]";
    }

  // 一路切片 → 一路画面。
  // layered=false：输出的就是成品帧（黑底 + 条带），与「时间线上没有重叠」时的写法逐字相同。
  // layered=true ：输出 W×H 的透明画布（条带以外 alpha=0），交给段内的 overlay 往上叠。
  auto build_layer = [&](const Layer &ly, const std::wstring &tag, bool layered,
                         std::wstring *v_out) {
    const Clip &c = ly.c;
    const double dur = c.duration;
    // 条带走 dur 秒，源要吃 dur×spd 秒（spd = 这一段的段速，见 retiming.h）。
    const double spd = retiming_speed_at(c, c.duration);
    const double sfps = c.info->src_fps();

    long long sw = c.info->width, sh = c.info->height;
    long long cl = clampi(c.crop_l, 0, std::max<long long>(0, sw - 16));
    long long cr = clampi(c.crop_r, 0, std::max<long long>(0, sw - 16 - cl));
    long long ct = clampi(c.crop_t, 0, std::max<long long>(0, sh - 16));
    long long cb = clampi(c.crop_b, 0, std::max<long long>(0, sh - 16 - ct));
    long long cw = sw - cl - cr, ch = sh - ct - cb;

    // 帧号一律出自 VideoInfo::src_frame（预览落位读的同一个所有者），刀口按帧号、不按秒：
    // trim 的判据是 pts>=start，把边界写在某只样本的 pts 上，谁进谁出就由 µs 级舍入决定
    // （实测：原点打印成六位小数一旦向上舍入，导出的第 0 帧就是源的第 1 帧）。
    // 帧号是整数，且解码流的第 0 帧就是源内第 0 帧 ⇒ 容器原点在这道门不必出现（它属于 seek）。
    const double a = c.source_in * sfps;                          // 切片左缘的源帧坐标
    const long long n_out = std::max<long long>(1, std::llround(dur * fps));  // 这一段几只 tick
    const int64_t f0 = c.info->src_frame(c.source_in);
    // 末 tick 要的那只 = 预览在同一 tick 上给的同一只；end_frame 排他 ⇒ +1。
    // 至少留一帧：段长不足一只源帧时（源帧率低于工程帧率）也不许切空。
    const int64_t f1 =
        std::max(f0 + 1, c.info->src_frame(c.source_in + double(n_out - 1) * spd / fps) + 1);
    // 左缘没踩在整数帧格上时的那点余量，和一 tick 走几只源帧 —— tick_place 的两个自变量。
    const double off = a - double(f0), per_tick = spd * sfps / fps;
    // 导出这一路对「第 k 格交第几只源帧」的全部答案就是这几个整数/比率，落一行让预览那一路
    // 对账（预览侧的对账在 Preview::show_frame）。两边同键：条带 id + 左缘秒 + 两个帧率。
    wchar_t eb[256];
    swprintf(eb, 256,
             L"导出落帧: 条带=%lld 左缘=%.9f 段速=%.6f 源率=%.9f 程率=%.9f 起=%lld 止=%lld "
             L"一格=%.9f只 差=%.9f tick=%lld",
             (long long)c.id, c.source_in, spd, sfps, fps, (long long)f0, (long long)f1,
             per_tick, off, (long long)n_out);
    trace(eb);
    std::wstring v = ly.vpad + L"trim=start_frame=" + std::to_wstring(f0) + L":end_frame=" +
                     std::to_wstring(f1) + L",setpts=(PTS-STARTPTS)" +
                     (spd == 1.0 ? std::wstring() : L"/" + fnum(spd));
    if (cl || cr || ct || cb) {
      // Blender 的裁切是「源像素空间的遮罩」（render.cc:458 给出 (left, w-right, bottom,
      // h-top)，imbuf/intern/transform.cc:95 的 should_discard 按源 uv 丢弃）：裁掉的像素
      // 消失，剩下的既不放大也不重新居中。ffmpeg 的 crop 会把留下的部分搬到左上角，所以裁完
      // 立刻按原位置 pad 回整幅尺寸、四周填透明，后面的缩放与居中一律照未裁切来算。
      v += L",format=yuva420p,crop=" + std::to_wstring(cw) + L":" + std::to_wstring(ch) + L":" +
           std::to_wstring(cl) + L":" + std::to_wstring(ct) + L",pad=" + std::to_wstring(sw) +
           L":" + std::to_wstring(sh) + L":" + std::to_wstring(cl) + L":" +
           std::to_wstring(ct) + L":0x00000000";
    }
    const double sx = c.scale_x > 0 ? c.scale_x : 1.0;
    const double sy = c.scale_y > 0 ? c.scale_y : 1.0;
    // Blender 不做任何「适配/填满/拉伸」：calc_strip_transform_matrix（render.cc:419-456）里
    // 唯一的自动缩放是各向同性的 image_scale_factor（= 分辨率%，画布也同比例缩，所以画面在
    // 画布里的相对大小不变），其余全是用户 transform。画面按源像素尺寸摆在画布正中，
    // 比画布大的由画布裁掉、比画布小的露出画布底色。
    // 支点原点在「用户缩放之前」的显示框上（render.cc:436 的 origin_mapped =
    // resize*center_image 作用到 in*origin），所以这里记的是缩放前的框。
    const double u = double(W) / double(std::max(1, p.fmt.width));
    double fitW = sw * u, fitH = sh * u;

    // 逐帧求值：变换的五个通道里任何一条挂了关键帧 ⇒ 把这一段的「缩放 + 摆放」整个交给
    // libplacebo。它的 crop_*/pos_* 带 AV_OPT_FLAG_RUNTIME_PARAM（-h filter=libplacebo 里
    // 那一位 T），每收到一个输入帧重新求值一次，而且是浮点 —— 实测 pos_x='n*0.5' 的半个
    // 像素逐帧挪动，而 overlay/crop 会把位置截成整数像素，慢速平移就变成台阶。
    // 旋转走 rotate 的 angle：它同样带 T、每帧重求（实测 angle='n*0.04' 逐帧转到 45.8°）。
    // 但 rotate 的输出尺寸只在 init 算一次，所以逐帧旋转把画布钉成「所有帧里最大的那个外接
    // 框」——内容在里面永远居中，任何角度都不会裁边，多出来的透明边跟着 pos_* 一起缩。
    const bool per_frame = !c.anim[kChOfsX].empty() || !c.anim[kChOfsY].empty() ||
                           !c.anim[kChScaleX].empty() || !c.anim[kChScaleY].empty() ||
                           !c.anim[kChRotation].empty();
    const bool rot_anim = per_frame && !c.anim[kChRotation].empty();
    // transform.xofs/yofs（DNA_sequence_types.h:267-268）以渲染缓冲像素为单位，
    // y 轴向上为正；ffmpeg 的 y 向下，所以取反。缓冲被“缩放 %”缩小时偏移同比例缩小。
    // 翻转把整张画布镜像，偏移方向自然也跟着镜像。
    const double px_scale = double(W) / double(std::max(1, p.fmt.width));
    // transform.origin（render.cc:429-443）：支点从框中心移到 om = 框*(origin−0.5) 之后，
    // 结果整体多走 E = om − R·S·om。ffmpeg 的 y 向下，而 RNA 的 origin y=0 是图片下沿，
    // 所以 om 的 y 取 (0.5 − origin_y)。镜像在最后（IMB_flipx），E 也跟着镜像。
    const double omx = fitW * (c.origin_x - 0.5), omy = fitH * (0.5 - c.origin_y);
    const double rad = c.rotation * 3.14159265358979 / 180.0;
    const double co = std::cos(rad), sn = std::sin(rad);
    // 支点位移 E = om − R·S·om 里的 R 是当前这一帧的旋转，所以余弦/正弦由调用方给：
    // 静止路径传标量的 co/sn，逐帧路径传每帧采出来的。
    auto offsets = [&](double ofsx, double ofsy, double kx, double ky, double co, double sn,
                       double *dx, double *dy) {
      *dx = ofsx * px_scale * (c.flip_x ? -1.0 : 1.0);
      *dy = -ofsy * px_scale * (c.flip_y ? -1.0 : 1.0);
      const double vx = omx * kx, vy = omy * ky;
      *dx += (omx - (vx * co - vy * sn)) * (c.flip_x ? -1.0 : 1.0);
      *dy += (omy - (vx * sn + vy * co)) * (c.flip_y ? -1.0 : 1.0);
    };
    // 静态路径把缩放并进 scale；逐帧路径只缩到「未缩放的显示尺寸」，缩放留给 pos_w/pos_h。
    const double layW = per_frame ? fitW : fitW * sx;
    const double layH = per_frame ? fitH : fitH * sy;
    v += L",scale=" + std::to_wstring(even_size(layW)) + L":" + std::to_wstring(even_size(layH));
    std::wstring stage = L"vc" + tag;
    // rotate 和 libplacebo 的 n 都是「它自己收到的第几帧」，而它们收到的是 trim 之后的**源**帧
    // （摆放的 select 排在它们之后，见下），从 0 数 ⇒ 逐帧采样的条数就是这一刀留下的源帧数。
    // 第 i 只是源内第 f0+i 只；它落进时间线的时刻 = 「源内秒减左缘、除以段速」，曲线的自变量
    // 用它 —— 否则 2 倍速时动画会跑得比画面快一倍。所以先把每帧的变换全采出来，再决定画布。
    const size_t nn = size_t(f1 - f0);
    std::vector<double> ew, eh, ex, ey, eang;
    // 进 libplacebo 前那一格的画布尺寸：静止旋转按浮点算自然外接框（和以前逐字相同），
    // 逐帧旋转钉成所有角度里最大的外接框并向上凑偶数 —— 内容永远居中，任何角度都不裁边。
    double boxW = 0, boxH = 0;
    if (per_frame) {
      std::vector<double> kxv(nn), kyv(nn), dxv(nn), dyv(nn);
      ew.assign(nn, 0.0);
      eh.assign(nn, 0.0);
      ex.assign(nn, 0.0);
      ey.assign(nn, 0.0);
      if (rot_anim) eang.assign(nn, 0.0);
      for (size_t k = 0; k < nn; k++) {
        const XformAt x = xform_at(c, p.fmt, c.timeline_start +
                                             ((double(f0) + double(k)) / sfps - c.source_in) / spd);
        kxv[k] = x.scale_x > 0 ? x.scale_x : 1.0;
        kyv[k] = x.scale_y > 0 ? x.scale_y : 1.0;
        const double r = x.rotation * 3.14159265358979 / 180.0;
        const double ci = std::cos(r), si = std::sin(r);
        if (rot_anim) eang[k] = r;
        offsets(x.ofs_x, x.ofs_y, kxv[k], kyv[k], ci, si, &dxv[k], &dyv[k]);
      }
      if (rot_anim) {
        const double inW = double(even_size(layW)), inH = double(even_size(layH));
        double mw = 0, mh = 0;
        for (size_t k = 0; k < nn; k++) {
          const double ci = std::cos(eang[k]), si = std::sin(eang[k]);
          mw = std::max(mw, std::fabs(ci) * inW + std::fabs(si) * inH);
          mh = std::max(mh, std::fabs(si) * inW + std::fabs(ci) * inH);
        }
        // 向上凑到偶数：420 采样和 libx264 都不收奇数宽高，而向下取会把转到的那一帧裁掉。
        boxW = 2.0 * std::ceil(mw / 2.0);
        boxH = 2.0 * std::ceil(mh / 2.0);
      } else {
        boxW = std::fabs(co) * fitW + std::fabs(sn) * fitH;
        boxH = std::fabs(sn) * fitW + std::fabs(co) * fitH;
      }
      for (size_t k = 0; k < nn; k++) {
        ew[k] = boxW * kxv[k];
        eh[k] = boxH * kyv[k];
        // overlay 的 (main_w-overlay_w)/2 在 libplacebo 里就是「左上角」⇒ pos = 居中 + 偏移。
        ex[k] = (double(W) - ew[k]) / 2.0 + dxv[k];
        ey[k] = (double(H) - eh[k]) / 2.0 + dyv[k];
      }
    }
    if (rot_anim) {
      chains.push_back(v + (layered ? std::wstring(L",format=yuva420p") : std::wstring()) +
                       L"[vc" + tag + L"]");
      v = L"[vc" + tag + L"]rotate='" + frame_expr(eang) + L"':fillcolor=" +
          (layered || cl || cr || ct || cb ? L"0x00000000" : L"black") + L":ow=" +
          std::to_wstring(std::llround(boxW)) + L":oh=" + std::to_wstring(std::llround(boxH));
      stage += L"r";
    } else if (std::fabs(c.rotation) > 1e-3) {
      // 旋转在缩放之后、翻转之前：Blender 先算变换矩阵，最后才对整张结果图做
      // IMB_flipx（render.cc:770-780），所以翻转的是「已经转好并摆好位置的画布」。
      // 要叠画时旋转的外接框得带透明角，否则那一圈黑会把底下的层盖掉。
      const std::wstring ang = fnum(c.rotation * 3.14159265358979 / 180.0);
      chains.push_back(v + (layered ? std::wstring(L",format=yuva420p") : std::wstring()) +
                       L"[vc" + tag + L"]");
      v = L"[vc" + tag + L"]rotate=" + ang +
          L":fillcolor=" + (layered || cl || cr || ct || cb ? L"0x00000000" : L"black") +
          // rotate 只有 angle/ow/oh/fillcolor/bilinear 五个选项（vf_rotate.c:99-108），没有
      // force_divisible_by；外接框要凑成偶数就直接在 ow/oh 表达式里取整 —— 420 采样和
      // libx264 都不收奇数宽高。
      L":ow=trunc(rotw(" + ang + L")/2)*2:oh=trunc(roth(" + ang + L")/2)*2";
      stage += L"r";
    }
    if (c.flip_x) v += L",hflip";
    if (c.flip_y) v += L",vflip";
    // 摆放排在「这一段最后一个读 n 的滤镜」之后：那时 n 还是 trim 之后的源帧编号，
    // tick_place 的反解正是在这个编号上做的。逐帧路径最后的读者是 libplacebo（它的 pos_*
    // 按 trim 编号采样，见上面的 frame_expr），静止路径的 scale/rotate/hflip 一个都不读。
    const std::wstring place = tick_place(off, per_tick, fps, n_out);
    // 窗里超出文件的那几只并不存在：补帧最多补到文件的最后一只，再往后的格是底色
    // （预览在那儿同样解不出帧）。
    const long long n_cap =
        std::min<long long>(f1, c.info->frame_count > 0 ? c.info->frame_count : f1) - 1 - f0;
    const long long tail = tick_tail(off, per_tick, n_out, n_cap);
    const std::wstring pad =
        tail > 0 ? L",tpad=stop=" + std::to_wstring(tail) + L":stop_mode=clone" : L"";
    if (!per_frame) {
      v += place;
      // fps= 挪进这一段：tpad 的补帧按「这一路的帧率」推时间戳，只有先把这一段铺成
      // 工程帧率的等间隔流，补出来的那几格才正好落在空 tick 上。叠画之后那道 fps= 原地
      // 不动 —— 它管的是叠完的成品流，跟这一段补没补帧无关，多走一遍是空过。
      if (tail > 0) v += L",fps=" + fr + pad;
    }
    chains.push_back(v + L"[" + stage + L"]");
    const std::wstring fmt_out = layered ? L"yuva420p" : L"yuv420p";
    if (per_frame) {
      // 采样点写进日志：核对「曲线 → 表达式 → 像素」这条链时，这三个数就是 C++ 侧的
      // 真值（表达式由 node 独立求值、像素由导出的帧量出来，两边各自对一次）。
      wchar_t sp[160];
      swprintf(sp, 160, L"export: 条带 %lld 逐帧 %lld 帧 pos_x[0]=%.3f[%lld]=%.3f[%lld]=%.3f",
               (long long)c.id, (long long)nn, ex.front(), (long long)(nn / 2), ex[nn / 2],
               (long long)(nn - 1), ex[nn - 1]);
      trace(sp);
      // 叠画时画布底色要透明（libplacebo 的 fillcolor 默认就是 black@0）、输出要带 alpha，
      // 底下那层才透得上来。
      chains.push_back(L"[" + stage + L"]libplacebo=inputs=1:w=" + std::to_wstring(W) + L":h=" +
                       std::to_wstring(H) + L":pos_w='" + frame_expr(ew) + L"':pos_h='" +
                       frame_expr(eh) + L"':pos_x='" + frame_expr(ex) + L"':pos_y='" +
                       frame_expr(ey) + L"'" +
                       (layered ? std::wstring(L":format=yuva420p:alpha_mode=straight")
                                : std::wstring(L":fillcolor=black")) +
                       place + L",fps=" + fr + pad + L",format=" + fmt_out + L",setsar=1[v" + tag +
                       L"]");
    } else {
      chains.push_back(L"color=c=" + (layered ? std::wstring(L"0x00000000") : std::wstring(L"black")) +
                       L":s=" + std::to_wstring(W) + L"x" + std::to_wstring(H) + L":r=" + fr +
                       L":d=" + fnum(dur) +
                       (layered ? std::wstring(L",format=yuva420p") : std::wstring()) + L"[bg" +
                       tag + L"]");
      double dx = 0, dy = 0;
      offsets(c.ofs_x, c.ofs_y, sx, sy, co, sn, &dx, &dy);
      // eof_action=pass：叠进来的那条带先结束就只留底图。ffmpeg 的 overlay 默认是
      // repeatlast（把最后一路 overlay 帧一直重放到主输入尽头），而段与段之间的帧流由
      // concat 按段取用、两路输入的收尾时机并不对齐；实测同一条链默认出 12800 帧、
      // 加 pass 出 25 帧，整段画面会被上一段的尾帧填满。
      chains.push_back(L"[bg" + tag + L"][" + stage + L"]overlay=x='(main_w-overlay_w)/2+" +
                       fnum(dx) + L"':y='(main_h-overlay_h)/2+" + fnum(dy) +
                       L"':eval=init:eof_action=pass,fps=" + fr + L",format=" + fmt_out +
                       L",setsar=1[v" + tag + L"]");
    }
    *v_out = L"[v" + tag + L"]";
  };

  // 一路声带 → 一路音频。声音只由 SOUND 条带贡献（BKE_sound_add_scene_sound 先判
  // strip->sound 是否为空，blenkernel/intern/sound.cc:945-951），所以影片条带这一侧
  // 什么都不产；这里和 build_layer 分开，重叠的几条声带交给调用方 amix。
  auto build_sound = [&](const Layer &ly, const std::wstring &tag, std::wstring *a_out) {
    const Clip &c = ly.c;
    const double spd = retiming_speed_at(c, c.duration);
    // 声音跟 Blender 一样整体重采样：sound_pitch_get 直接返回 speed_factor
    // （sound.cc:162-169），没有音高修正（那是 SEQ_AUDIO_PITCH_CORRECTION + RubberBand
    // 那条可选的路，strip_retiming.cc:1215-1223），所以音高和节奏一起变。
    std::wstring af;
    if (spd != 1.0) {
      const int src_rate = c.info->audio_rate > 0 ? c.info->audio_rate : 48000;
      af = L",asetrate=" + std::to_wstring(int64_t(std::lround(double(src_rate) * spd))) +
           L",aresample=48000";
    }
    // 和画面同一件事的音频版：按样本号切，不按秒。atrim 数的是解码流交来的第几只样本，
    // 第 0 只就是源内第 0 秒 ⇒ 音频流自己的原点在这道门不必出现（它属于预览那一路的 `-ss`）。
    const int64_t s0 = c.info->src_sample(c.source_in);
    const int64_t s1 = std::max(s0 + 1, c.info->src_sample(c.source_in + c.duration * spd));
    chains.push_back(ly.apad + L"atrim=start_sample=" + std::to_wstring(s0) + L":end_sample=" +
                     std::to_wstring(s1) + L",asetpts=PTS-STARTPTS" + af +
                     L",aformat=sample_rates=48000:channel_layouts=stereo[a" + tag + L"]");
    *a_out = L"[a" + tag + L"]";
  };

  for (size_t s = 0; s < seg_span.size(); s++) {
    const double seg0 = seg_span[s].first, seg1 = seg_span[s].second;
    const std::wstring &tag = seg_tag[s];
    std::vector<Layer> &ls = seg_layers[s];
    // 画面与声音在这一段各自成一组：Blender 逐帧把活跃条带分流（画面侧
    // iterator.cc:223-225 先否掉 STRIP_TYPE_SOUND，声音侧 sound.cc:945-951 只收带
    // bSound 的那几条）。所以「一段里只有声带」是黑画面 + 有声音，而不是空段。
    std::vector<size_t> vids, snds;
    for (size_t j = 0; j < ls.size(); j++) (ls[j].c.is_sound ? snds : vids).push_back(j);
    const double seg_dur = seg1 - seg0;
    if (vids.empty() && snds.empty()) {  // 这一段画面上没人 ⇒ 黑帧（帧范围头部、条带之间的空隙都落在这里）
      add_black(seg_dur, L"gap" + tag);
      continue;
    }
    if (vids.empty()) {
      // 只有声带：铺一整段黑（和 add_black 的写法相同，只是不配静音）。
      chains.push_back(L"color=c=black:s=" + std::to_wstring(W) + L"x" + std::to_wstring(H) +
                       L":r=" + fr + L":d=" + fnum(seg_dur) + L"[v" + tag + L"]");
      vlab.push_back(L"[v" + tag + L"]");
    } else if (vids.size() == 1) {  // 单层：直接就是一段成品，和不重叠时的写法逐字相同
      std::wstring v1;
      build_layer(ls[vids[0]], tag, false, &v1);
      vlab.push_back(v1);
    } else {
      // 多层：每一层先摊成一张透明的 W×H 画布，再从下往上 overlay。底色是不透明黑 ——
      // Blender 的合成缓冲也是从背景色起画的（render.cc 的画布先填底色，再按通道叠条带）。
      std::vector<std::wstring> lv;
      for (size_t k = 0; k < vids.size(); k++) {
        std::wstring vv;
        build_layer(ls[vids[k]], tag + L"_" + std::to_wstring(int(k)), true, &vv);
        lv.push_back(vv);
      }
      const std::wstring base = L"[b" + tag + L"]";
      chains.push_back(L"color=c=black:s=" + std::to_wstring(W) + L"x" + std::to_wstring(H) +
                       L":r=" + fr + L":d=" + fnum(seg_dur) + L",format=yuva420p" + base);
      std::wstring prev = base;
      for (size_t j = 0; j < lv.size(); j++) {
        const bool last = j + 1 == lv.size();
        const std::wstring out =
            last ? L"[v" + tag + L"]" : L"[t" + tag + L"_" + std::to_wstring(int(j)) + L"]";
        chains.push_back(prev + lv[j] + L"overlay=x=0:y=0:eval=init:eof_action=pass" +
                         (last ? std::wstring(L",format=yuv420p,setsar=1") : std::wstring()) + out);
        prev = out;
      }
      vlab.push_back(L"[v" + tag + L"]");
    }

    // ---- 声音：这一段的活跃声带混成一轨 ----
    if (!want_audio) continue;
    if (snds.empty()) {
      // 没有声带：用静音填满这段时间，拼接器的每一路都要有音频。
      chains.push_back(L"anullsrc=r=48000:cl=stereo:d=" + fnum(seg_dur) + L"[a" + tag + L"]");
      alab.push_back(L"[a" + tag + L"]");
    } else if (snds.size() == 1) {
      std::wstring a1;
      build_sound(ls[snds[0]], tag, &a1);
      alab.push_back(a1);
    } else {
      // 重叠的声音一起混（手册 video_editing/edit/montage/strips/sound.rst:24-29：
      // 「as many Sound strips as you wish and the result will be the mixing of all of
      // them. Overlapping strips are automatically mixed down during the rendering」）。
      // amix 的 normalize=0 就是逐样本求和、不按路数衰减 —— Blender 同样不衰减，过冲
      // 交给编码器自己钳。
      std::wstring c;
      for (size_t k = 0; k < snds.size(); k++) {
        std::wstring aa;
        // 声带的标签加 "s" 后缀：同一层的画面已经用过 tag_k 这个编号了。
        build_sound(ls[snds[k]], tag + L"_" + std::to_wstring(int(k)) + L"s", &aa);
        c += aa;
      }
      c += L"amix=inputs=" + std::to_wstring(int(snds.size())) +
           L":duration=longest:dropout_transition=0:normalize=0[a" + tag + L"]";
      chains.push_back(c);
      alab.push_back(L"[a" + tag + L"]");
    }
  }

  std::wstring concat;
  for (size_t i = 0; i < vlab.size(); ++i)
    concat += vlab[i] + (want_audio ? alab[i] : std::wstring());
  // 步长 > 1 时拼接器先落在中间标签上，出图口留给下面那一道取样。
  const long long fstep = std::max<long long>(1, p.fmt.frame_step);
  const bool stepped = fstep > 1;
  const long long nf_kept = stepped ? (nf_total + fstep - 1) / fstep : nf_total;
  // 帧率在每个片段自己的链路末端用 fps= 统一（摆放/合成之后，所以 libplacebo 的 n 仍然
  // 数的是源帧）。原来统一交给输出端 -r 是错的，实测两头的偏差都出自它：一段 90 帧的流
  // 压成 200% 后 -r 30 给 47 帧（应是 45），拉成 50% 后给 179 帧（应是 180，最后一帧被
  // 当成「不完整的区间」丢掉）；改成图内 fps=30，两种倍率都正好 45 / 180。
  // fps= 在这里只余下「空 tick 重复上一只」这一件事（慢放），「第 k 只要哪一帧」已经交给
  // 上面的 tick_place 按帧号写死 —— 那一轮 -r 的偏差，根子是它拿时间戳去猜帧号。
  // 拼接后再插 fps 会让缓冲无限堆积、导出永不结束，所以只在片段内插。
  concat += L"concat=n=" + std::to_wstring(vlab.size()) + L":v=1:a=" +
            std::wstring(want_audio ? L"1" : L"0") +
            (stepped ? L"[cv]" : L"[outv]") +
            (want_audio ? (stepped ? L"[ca]" : L"[outa]") : L"");
  chains.push_back(concat);

  if (stepped) {
    // 步长 = scene.frame_step（rna_scene.cc:9392-9400「每帧渲染/播放时向前跳过的帧数」）。
    // Blender 的渲染循环逐帧求值、只写第 k×step 帧（pipeline.cc:2510-2547：cfra 每次 +1，
    // 只有 nfra == cfra 才往下走，写完 nfra += tfra），而写进容器的每一帧都占 1/fps 秒 ——
    // 所以成片是「跳过中间帧的延时」：帧数 ceil(区间/step)、时长也短 step 倍，不是把时长拉长。
    // select 按输出帧号 n 取模丢帧，setpts 把留下的帧重排成连续的 1/fps（不重排的话时间戳
    // 还带着跳帧的间隔，等于只把帧率降下来、时长不变 —— 那是 fps 滤镜的行为，不是这里的）。
    chains.push_back(L"[cv]select='not(mod(n," + std::to_wstring(fstep) + L"))'" +
                     L",setpts=N/(" + fr + L"*TB)[outv]");
    // 声音这边是本工程超出 Blender 的一处：Blender 的容器写入按「场景帧号」补音频时长
    // （movie_write.cc:1666-1668 的 (frame − start + 1) / fps），跳帧写视频时音频照写满
    // 整段，实测就是音轨比画面长 step 倍、对不上。剪出来的片子要的是同一个时间轴：
    // 第 k 帧画面来自场景第 k×step 帧，声音也得走到场景第 k×step 帧那一刻 ⇒ 整条压 step 倍。
    // atempo 保音调、单格只吃 0.5..2.0，所以拆成链（5 → 2,2,1.25）。
    if (want_audio) {
      std::wstring at;
      double r = double(fstep);
      while (r > 2.0 + 1e-9) {
        at += L"atempo=2.0,";
        r /= 2.0;
      }
      chains.push_back(L"[ca]" + at + L"atempo=" + fnum(r) + L"[outa]");
    }
    wchar_t tb[160];
    swprintf(tb, 160, L"export: 步长=%lld 区间帧=%lld → 取帧=%lld（画面 select+setpts，声音 atempo 链）",
             fstep, nf_total, nf_kept);
    trace(tb);
  }

  std::wstring graph;
  for (size_t i = 0; i < chains.size(); ++i) {
    if (i) graph += L";";
    graph += chains[i];
  }

  // 逐帧表达式的图可以长到几十万字符，命令行（32767）装不下 ⇒ 写进临时文件，
  // 用 ffmpeg 9 的 -/filter_complex（从文件读取该参数的值）。老写法
  // -filter_complex_script 在 9.0.2 已不存在。
  cmd->graph = graph;
  cmd->graph_file.clear();
  std::wstring graph_arg;
  if (graph.size() > kInlineGraphLimit) {
    cmd->graph_file = temp_text_path(L"vse_ffgraph");
    if (!write_text_file(cmd->graph_file, graph))
      return fail(L"无法写入滤镜图临时文件：" + cmd->graph_file);
    graph_arg = L"-/filter_complex";
  } else {
    graph_arg = L"-filter_complex";
  }

  cmd->args.clear();
  cmd->args = {L"-v", L"error", L"-nostdin", L"-y", L"-hide_banner"};
  for (const std::wstring &path : srcs) {
    cmd->args.push_back(L"-i");
    cmd->args.push_back(path);
  }
  cmd->args.push_back(graph_arg);
  cmd->args.push_back(cmd->graph_file.empty() ? graph : cmd->graph_file);
  // 图进了文件就再看不到了（导出后删掉），所以把原文也落一行日志。
  if (!cmd->graph_file.empty()) {
    trace(L"export: 滤镜图 " + std::to_wstring(graph.size()) + L" 字符 → " + cmd->graph_file);
    trace(L"export: graph " + graph);
  }
  cmd->args.push_back(L"-map");
  cmd->args.push_back(L"[outv]");
  if (want_audio) {
    cmd->args.push_back(L"-map");
    cmd->args.push_back(L"[outa]");
  } else {
    // Blender 的 Audio Codec = NONE：容器里不写音轨，也不是「静音的音轨」。
    cmd->args.push_back(L"-an");
  }
  // 帧率不再由输出端 -r 统一（上面每个片段自己 fps= 过了，-r 再采一次会把最后那一帧
  // 当成不完整区间丢掉）。
  // 像素宽高比非 1:1 时，把显示宽高比写进容器（Blender 渲染输出同样带 DAR）。
  // xasp/yasp 是浮点，先按千分之一通分再约分，凑成 ffmpeg 要的整数比。
  const long long dar_n =
      std::llround(double(p.fmt.width) * (p.fmt.par_x > 0 ? p.fmt.par_x : 1) * 1000.0);
  const long long dar_d =
      std::llround(double(std::max(1, p.fmt.height)) * (p.fmt.par_y > 0 ? p.fmt.par_y : 1) * 1000.0);
  if (dar_n > 0 && dar_d > 0 &&
      std::fabs(double(dar_n) / double(dar_d) - double(W) / double(H)) > 1e-3) {
    const long long g = std::gcd(dar_n, dar_d);
    cmd->args.insert(cmd->args.end(),
                     {L"-aspect", std::to_wstring(dar_n / g) + L":" + std::to_wstring(dar_d / g)});
  }
  // 质量 = Blender FFmpeg 的常量质量因子（eFFMpegCrf：恒定码率 -1 / 无损 0 / 感知无损 17，
  // DNA_scene_types.h:104-106）。两条路互斥，和 Blender 一样：movie_write.cc:752-758 见
  // crf<0 就收起 -crf、改按码率出（:753 `bit_rate = video_bitrate * 1000`，所以面板那一格
  // 是 kbit/s，命令行写成 2000k 就是 2,000,000 bit/s）；码率填 0 什么都不发，
  // 交给 libx264 自己的默认码率控制（Blender 没改过 video_bitrate 时正是这个状态）。
  // 速度 = ffmpeg_preset 那张表的第三列。
  cmd->args.insert(cmd->args.end(), {L"-c:v", L"libx264", L"-preset", p.fmt.speed_preset().x264});
  if (p.fmt.crf >= 0)
    cmd->args.insert(cmd->args.end(), {L"-crf", std::to_wstring(p.fmt.crf)});
  else if (p.fmt.bitrate > 0)
    cmd->args.insert(cmd->args.end(), {L"-b:v", std::to_wstring(p.fmt.bitrate) + L"k"});
  cmd->args.insert(cmd->args.end(), {L"-pix_fmt", L"yuv420p", L"-movflags", L"+faststart",
                                     L"-progress", L"pipe:1"});
  if (want_audio)
    cmd->args.insert(cmd->args.end(),
                     {L"-c:a", L"aac", L"-b:a", L"192k", L"-ar", L"48000", L"-ac", L"2"});
  // 帧数按 sfra..efra 闭区间锁死（Blender 渲染就是 efra−sfra+1 张图），步长 >1 时还要再除：
  // 写进容器的只有第 0、step、2step… 帧（pipeline.cc:2510-2547）。图内 fps= 之后各段本该
  // 正好，这一条是兜底：任何一段多吐一帧都不会把整条时间线往后挤。
  cmd->args.insert(cmd->args.end(), {L"-frames:v", std::to_wstring(nf_kept)});
  cmd->args.push_back(out_path);
  std::wstring line;
  for (const std::wstring &arg : cmd->args) {
    line += arg;
    line += L' ';
  }
  // 变换挂了关键帧 ⇒ 上面已经把每帧的值编进表达式：平移/缩放交给 libplacebo 的 pos_*，
  // 旋转交给 rotate 的 angle。这一行报的是「几条通道在逐帧」，核对「曲线 → 表达式 → 像素」
  // 时它就是 C++ 侧的真值清单。
  for (const Clip &c : p.clips) {
    int animated = 0;
    for (int i = 0; i < kAnimCount; i++)
      if (!c.anim[i].empty()) animated++;
    if (!animated) continue;
    wchar_t wb[128];
    if (!c.anim[kChRotation].empty())
      swprintf(wb, 128, L"export: 条带 %lld 逐帧求值 %d 通道（含 rotate 角度表达式）",
               (long long)c.id, animated);
    else
      swprintf(wb, 128, L"export: 条带 %lld 逐帧求值 %d 通道（libplacebo）", (long long)c.id,
               animated);
    trace(wb);
  }
  // 质量档、是否带音轨都只体现在这条命令行上，界面里看不出差别 —— 开 VSE_TRACE
  // 时把它整条落盘，改完设置能直接核对到 ffmpeg 真的收到了什么。
  trace(L"export: " + line);
  // 进度按成片的时长算：ffmpeg 报的是它自己写出的时间，步长 >1 时那就是 nf_kept/fps。
  cmd->total_seconds = stepped ? double(nf_kept) / fps : t1 - t0;
  return true;
}

// 已知架构限制（别再在 concat 清单上找政策）：
// concat demuxer 的 [inpoint, outpoint) 是**连续窗口**，源含 B 帧时做不到帧数 ±0 ——
// 每拼接点带过界的是**非连续**的显示帧（实测 `-bf 3` 夹具重复第 120 与第 124 帧），窗口减不掉它。
// 无 B 帧源上 outpoint 实测是排他的（10 段 × 120 帧 ⇒ 包 1200 / 不同 pts 1200 / 零重复）。
// 有 B 帧的源在本机素材里占 0.81% ⇒ 码流直通这条路关掉，不留开关。

Exporter::~Exporter() { cancel(); }

void Exporter::drop_graph_file() {
  if (!graph_file_.empty()) {
    DeleteFileW(graph_file_.c_str());
    graph_file_.clear();
  }
}

bool Exporter::start(const Project &proj, const std::wstring &out_path, std::wstring *err) {
  cancel();
  measure_ = env_flag(L"VSE_MEASURE");
  ExportCommand cmd;
  if (!build_export(proj, out_path, &cmd, err)) return false;
  std::wstring exe, probe;
  if (!Ffmpeg::locate(&exe, &probe)) {
    drop_graph_file();
    if (err) *err = L"找不到 ffmpeg.exe";
    return false;
  }
  auto proc = std::make_unique<FrameProc>();
  if (measure_) {
    t_start_ = now_seconds();
    self_cpu0_ = self_cpu_ms();
  }
  if (!proc->start(exe, cmd.args, err)) {
    drop_graph_file();
    return false;
  }
  proc_ = std::move(proc);
  graph_file_ = cmd.graph_file;
  buf_.clear();
  total_ = cmd.total_seconds;
  done_ = 0;
  out_ = out_path;
  message_.clear();
  running_ = true;
  return true;
}

void Exporter::consume() {
  size_t nl = 0;
  while (true) {
    size_t start = nl;
    size_t pos = buf_.size();
    for (size_t i = start; i < buf_.size(); ++i)
      if (buf_[i] == '\n') { pos = i; break; }
    if (pos == buf_.size()) break;
    std::string line(buf_.begin() + long(start), buf_.begin() + long(pos));
    nl = pos + 1;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), val = line.substr(eq + 1);
    if (!val.empty() && val.back() == '\r') val.pop_back();
    if (key == "out_time_us" || key == "out_time_ms") {
      double us = strtod(val.c_str(), nullptr);
      if (us > 0) done_ = us / 1e6;
    } else if (key == "progress" && val == "end") {
      done_ = total_;
    }
  }
  buf_.erase(buf_.begin(), buf_.begin() + long(nl));
}

void Exporter::pump() {
  if (!running_ || !proc_) return;
  uint8_t tmp[4096];
  while (proc_->wait_ready(0)) {
    size_t got = proc_->read_some(tmp, sizeof tmp);
    if (!got) break;
    buf_.insert(buf_.end(), tmp, tmp + got);
  }
  consume();
  DWORD code = 0;
  if (!proc_->exited(&code)) return;
  running_ = false;
  // 这一组数的口径（开关 VSE_MEASURE=1 才记）：child_cpu 是这一只 ffmpeg 自己的 user+sys
  // （多线程编码 ⇒ 可以大于墙钟），self_cpu 是主程序在同一窗口里陪跑的份额。
  if (measure_) {
    const double wall = now_seconds() - t_start_;
    const double child = proc_->cpu_ms();
    const double self = self_cpu_ms() - self_cpu0_;
    wchar_t mb[256];
    swprintf(mb, 256,
             L"export-cost: exit=%lu wall=%.3fs child_cpu=%.3fs self_cpu=%.3fs "
             L"seconds=%.3f",
             (unsigned long)code, wall, child / 1000.0,
             self / 1000.0, total_);
    trace(mb);
  }
  drop_graph_file();
  wchar_t w[512];
  if (code == 0) {
    swprintf(w, 512, L"导出完成：%ls", out_.c_str());
  } else {
    std::wstring e = proc_->stderr_text();
    swprintf(w, 512, L"导出失败（ffmpeg 退出码 %lu）：%ls", code, e.c_str());
  }
  message_ = w;
  proc_.reset();
}

void Exporter::cancel() {
  drop_graph_file();
  if (!proc_) return;
  proc_->kill();
  proc_.reset();
  running_ = false;
  message_ = L"已取消导出";
}
