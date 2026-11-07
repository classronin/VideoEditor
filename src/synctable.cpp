// 会话级同步点表。公开的形状、边界、谓词都在 synctable.h 里写死了，这一份只做那几句话。
#include "synctable.h"

#include "publish.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#define ULL(v) (unsigned long long)(v)

namespace app {
namespace {

// 开门判据的四根线。**四条注**：
// ① 5 s / 1 % / 50 % 都是 30fps×GOP150 这一族素材上量出来的初值：
//    换一族素材要重评审，不许在实现里顺手调参 —— 调参要单独评审，不是编码时顺手的事。
// ② 50 % 与 lag≤dur/2 等价，和 5 s 那根线在 dur=10 s 处交叉 ⇒ 10~60 s 这一段的 50 % 是空转的，
//    真正生效的是 5 s；短片档的 50 % 只管 dur<10 s 那一小截（它存在的理由是「别把 4 秒素材的
//    一只 GOP 判成半片不可信」，不是另一套精度）。
// ③ 50 % 的精度是假的：share 的分母是样本数，30fps×GOP150 一族里 [49.667 %, 55.215 %) 这一整段
//    逐只判定相同 —— 别把「50」当成一位有效数字。
// ④ worst 取的是**最坏间隔**（max_gap 与尾段 tail_gap 的较大者），不是平均间隔：拖动落点撞上的
//    就是最坏那一格，平均值没有买家。
constexpr double kDurTierSec = 60.0;     // 片长档界
constexpr double kLagCapSec = 5.0;       // 两档共用的滞后上限
constexpr double kShareCapShort = 0.50;  // dur < 60 s
constexpr double kShareCapLong = 0.01;   // dur ≥ 60 s

// 建表那一趟和懒验那一格用的都是 4096 一窗：和判据① 全量扫描的默认值同一个数。
// 换掉它会改变「第一条条片出不出窗口」，懒验和全量判据就不再是同一件事了。
constexpr size_t kWindowBytes = 4096;

struct Entry {
  SyncTable t;
  bool asked = false;  // 登记过，等 worker 来取
  bool done = false;   // 已经交给过 worker（成没成都算一次）⇒ 同会话每源只算一次
};

// 一把锁管全部三样（表、起不起线程、待落的 trace 行）。上一版设计想分成两把，分完就出现
// 「登记和退出各自持一把，中间必须嵌套」的死结；一把锁时 worker 只在 sync_compute（读盘、算表）
// 期间不持锁，UI 线程的 peek/lookup 最坏被一次「拷一行」挡住 —— 那是可以付的，两把锁省不下它。
std::mutex g_mu;
std::map<std::wstring, Entry> g_map;
bool g_running = false;
std::deque<std::wstring> g_trace;

bool any_pending() {  // 调用方持 g_mu
  for (const auto &kv : g_map)
    if (kv.second.asked) return true;
  return false;
}

void push_trace(std::wstring line) {
  {
    std::lock_guard<std::mutex> lk(g_mu);
    // 排水口在 UI 线程（main.cpp 收到 kPubSync 那一格调 sync_take_trace）；算得快时这里
    // 最多少落几行，上限是为了「UI 一直不取」也不会把内存吃掉，取走顺序不变。
    if (g_trace.size() < 256) g_trace.push_back(std::move(line));
  }
  // 账排好了再说一声：以前是 UI 每 15ms 来问一次，现在空闲没有那个时钟，
  // 不喊就真的没人知道。出了锁再喊 —— 收账的那一步要拿同一把 g_mu。
  ui_publish(kPubSync);
}

std::wstring sync_wstr(const wchar_t *fmt, ...) {
  va_list ap;
  wchar_t buf[1024];
  va_start(ap, fmt);
  const int n = vswprintf(buf, 1024, fmt, ap);
  va_end(ap);
  return n < 0 ? std::wstring() : std::wstring(buf);
}

std::wstring sync_trace_line(const std::wstring &path, const SyncTable &t) {
  const wchar_t *ident = t.trusted ? (t.presumed ? L"推定绿" : L"可信") : L"不可信";
  const wchar_t *gate = t.kf_open ? L"开" : L"关";
  const wchar_t *tier = t.short_tier ? L"短片" : L"长片";
  std::wstring line = sync_wstr(
      L"同步表: %ls 表=%ls reason=%d 档=%ls dur=%.1fs 样=%llu 同=%llu worst=%llu lag=%.3fs share=%.2f%% "
      L"门=%ls 原点=%ls 分帧=%ls cpu_pay=%.1fms",
      path.c_str(), ident, t.reason, tier, t.dur_s, ULL(t.samples), ULL(t.sync_count), ULL(t.worst_gap),
      t.lag_s, t.share * 100.0, gate,
      t.origin_shifted ? L"已减" : L"未减",
      t.length_size ? sync_wstr(L"AVCC%d", t.length_size).c_str() : std::wstring(L"Annex-B").c_str(),
      t.cpu_pay_s * 1000.0);
  if (!t.why.empty()) line += L" 因=" + t.why;
  return line;
}

void worker_loop() {
  for (;;) {
    std::wstring path;
    double fps = 0.0;
    if (!sync_next_request(&path, &fps)) {
      // 没有登记的活：在锁内再确认一次，然后交回「我没在跑」这个牌子。确认与清牌同一次持锁 ⇒
      // 登记的线程要么看见 running=true（那这笔登记一定会被下一轮循环取到），要么自己起一条。
      std::lock_guard<std::mutex> lk(g_mu);
      if (!any_pending()) g_running = false;
      return;
    }
    SyncTable t;
    sync_compute(path, fps, &t);
    std::wstring line = sync_trace_line(path, t);
    {
      std::lock_guard<std::mutex> lk(g_mu);
      const auto it = g_map.find(path);
      if (it != g_map.end()) it->second.t = std::move(t);
    }
    push_trace(std::move(line));
  }
}

}  // namespace

bool sync_compute(const std::wstring &path, double fps, SyncTable *out) {
  *out = SyncTable{};
  out->computed = true;  // 成没成都算一次（每源一次的断言看这个，不看可信）
  out->fps = fps;
  // 建表只要「容器索引 + 判据②」，判据① 那一圈每格一次设备读改由懒验按格付（synctable.h 头注）。
  const IndexReport r = probe_container_index(path, false, kWindowBytes, nullptr,
                                              /*skip_rule1=*/true);
  out->reason = r.reason;
  out->samples = r.samples;
  out->sync_count = r.sync_count;
  out->length_size = r.avcc_length_size;
  // 这一趟真付的 CPU 秒：全账（user+sys）减判据①。skip 时那一段是 0，减它是把两档的
  // 口径写成一个式子，而不是两处各写一半。
  out->cpu_pay_s = (r.cpu_user_ms + r.cpu_sys_ms - r.cpu_rule1_ms) / 1000.0;

  // ---- 间隔账（max_gap 初值 1、尾段单独算、取最坏）----
  const uint64_t first_sync = r.sync_samples.empty() ? 0 : r.sync_samples.front();
  const uint64_t last_sync = r.sync_samples.empty() ? 0 : r.sync_samples.back();
  uint64_t max_gap = r.sync_samples.empty() ? 0 : 1;
  for (size_t i = 1; i < r.sync_samples.size(); i++) {
    const uint64_t d = r.sync_samples[i] - r.sync_samples[i - 1];
    if (d > max_gap) max_gap = d;
  }
  const uint64_t tail_gap = (r.samples && last_sync) ? (r.samples - last_sync + 1) : 0;
  out->worst_gap = max_gap > tail_gap ? max_gap : tail_gap;

  // ---- 谓词（全工程只这一处；不读 ready）----
  // 无 stss ⇒ ISO 说每个样本都是同步点，探针把 1..samples 全填进 sync（mp4index.cpp 的 stss 分支）
  // ⇒ 这一支拿到的是「整片全是关键帧」的表：可信但**推定**（没人证过第一片是 IDR，那一半由懒验补），
  // 身份和计数都独立，不并进上面的可信绿。代价也如实：行数=样本数（全内素材 16.4 万样本 ≈ 4.2 MB）。
  out->trusted = r.ok && r.reason == kPtOk && r.rule2_comparable &&
                 (r.has_stss ? (r.sync_count >= 1 && first_sync == 1) : true);
  out->presumed = out->trusted && !r.has_stss;
  if (!out->trusted) {
    if (!r.ok)
      out->why = r.verdict.empty() ? L"容器索引不可用" : r.verdict;
    else if (!r.rule2_comparable)
      out->why = L"stts/ctts 覆盖不足 ⇒ 判据② 没跑成，逐同步点 pts 这一列没有";
    else if (r.reason == kPtOpenGop)
      out->why = L"判据② 有前导帧违例 ⇒ open-GOP，表给的秒数会指到解不出来的地方";
    else if (r.has_stss && first_sync != 1)
      out->why = L"第一只同步点不是样本 1 ⇒ 片头那段没有恢复点，减完原点仍然偏";
    else
      out->why = sync_wstr(L"reason=%d", r.reason);
  }

  // ---- 开门判据（每源一次，算完就存进表）----
  if (fps > 0.0 && r.samples > 0) {
    out->lag_s = double(out->worst_gap > 0 ? out->worst_gap - 1 : 0) / fps;
    out->share = double(out->worst_gap > 0 ? out->worst_gap - 1 : 0) / double(r.samples);
    out->dur_s = double(r.samples) / fps;
  } else {
    out->why = out->why.empty() ? L"无帧率或无样本 ⇒ lag/share 算不出，门不开" : out->why;
  }
  out->short_tier = out->dur_s < kDurTierSec;
  out->kf_open = out->trusted && fps > 0.0 && out->worst_gap > 0 &&
                 out->lag_s <= kLagCapSec &&
                 out->share <= (out->short_tier ? kShareCapShort : kShareCapLong);

  // ---- 行：只在可信时建（不可信的表永远不会被查到，留着那些行就是白占内存）----
  if (out->trusted && r.rule2_comparable && !r.sync_pts.empty() &&
      r.sync_pts.size() == r.sync_count && r.sync_offsets.size() == r.sync_count) {
    std::vector<uint32_t> idx(r.sync_count);
    for (size_t i = 0; i < idx.size(); i++) idx[i] = uint32_t(i);
    // pts 升序只是容器里的观察，不是规范给的证词 ⇒ 这里按 pts 稳定排序，
    // upper_bound 的合法性由构造给出，不由观察给出。同 pts 的多行保持解码序。
    std::stable_sort(idx.begin(), idx.end(),
                     [&](uint32_t a, uint32_t b) { return r.sync_pts[a] < r.sync_pts[b]; });
    out->pts.reserve(r.sync_count);
    out->frame.reserve(r.sync_count);
    out->sample.reserve(r.sync_count);
    out->offset.reserve(r.sync_count);
    for (const uint32_t i : idx) {
      out->pts.push_back(r.sync_pts[i]);
      out->sample.push_back(r.sync_samples[i]);
      out->offset.push_back(r.sync_offsets[i]);
    }
    // **只对绿谓词成立的源减原点**。这一列的原点是「解码序第一个样本 = 0」，
    // 带 B 帧的源因此比容器 pts 晚一个常数（实测 0.08 s = 2 帧）；谓词含 first_sync==1 且
    // reason==kPtOk ⇒ pts_violations==0 ⇒ 全样本最小值必落在同步点子集里 ⇒ min(pts) 就是第一显示帧。
    // 不绿的源不减（也无所谓 —— 它的表根本不开门），也不许假装减了：why 里如实记一笔。
    const double origin = out->pts.front();
    for (double &p : out->pts) p -= origin;
    out->origin_shifted = true;
    out->frame.resize(out->pts.size());
    for (size_t i = 0; i < out->pts.size(); i++)
      out->frame[i] = fps > 0.0 ? int64_t(std::llround(out->pts[i] * fps)) : 0;
    out->idr.assign(out->pts.size(), uint8_t(kSyncIdrUnknown));
    if (origin != 0.0) out->why = sync_wstr(L"原点已减 %.4fs", origin);
  } else if (out->trusted) {
    // 谓词成立但这一列没填上（规则② 可比 ⇒ 不该走到这里）：宁可不建表，也不建半截表。
    out->trusted = false;
    out->kf_open = false;
    out->why = L"逐同步点 pts 的行数对不上同步点数 ⇒ 表不建";
  } else {
    out->why = out->why.empty() ? L"原点未减：源不绿" : L"原点未减：源不绿；" + out->why;
  }
  return out->kf_open;
}

bool sync_peek(const std::wstring &path, SyncTable *out) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_map.find(path);
  if (it == g_map.end() || !it->second.t.computed) return false;
  if (out) *out = it->second.t;
  return true;
}

void sync_request(const std::wstring &path, double fps) {
  if (path.empty()) return;
  bool launch = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    Entry &e = g_map[path];
    if (!e.asked && !e.done) {
      e.asked = true;
      e.t.fps = fps;  // 登记时就把帧率放进去：worker 取的是这一格
      if (!g_running) {
        g_running = true;
        launch = true;
      }
    }
  }
  // 起线程这一步不持锁：std::thread 在锁里失败会抛，抛在 UI 线程的读路径上是产品事故。
  if (launch) {
    try {
      std::thread(worker_loop).detach();
    } catch (...) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_running = false;  // 起不来就把牌子收回，下一次登记再试；登记的活不会丢
    }
  }
}

bool sync_next_request(std::wstring *out_path, double *out_fps) {
  std::lock_guard<std::mutex> lk(g_mu);
  for (auto &kv : g_map) {
    Entry &e = kv.second;
    if (!e.asked || e.done) continue;
    e.asked = false;
    e.done = true;  // 交出去就不再重发：算的人要么落表，要么这一源真的读不了
    if (out_path) *out_path = kv.first;
    if (out_fps) *out_fps = e.t.fps;
    return true;
  }
  return false;
}

bool sync_lookup(const std::wstring &path, double target, SyncPoint *out) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_map.find(path);
  if (it == g_map.end() || !it->second.t.computed) return false;
  const SyncTable &t = it->second.t;
  if (!t.trusted || !t.kf_open || t.pts.empty()) return false;
  const auto p = std::upper_bound(t.pts.begin(), t.pts.end(), target);
  if (p == t.pts.begin()) return false;  // 落点比第一只同步点还早：没有「之前」可指
  const uint32_t row = uint32_t(p - t.pts.begin() - 1);
  if (out) {
    out->pts = t.pts[row];
    out->frame = t.frame[row];
    out->sample = t.sample[row];
    out->offset = t.offset[row];
    out->row = row;
  }
  return true;
}

bool sync_idr_state(const std::wstring &path, uint32_t row, uint8_t *state, bool *stopped) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_map.find(path);
  if (it == g_map.end() || !it->second.t.computed) return false;
  const SyncTable &t = it->second.t;
  if (stopped) *stopped = t.lazy_stopped;
  if (state) *state = row < t.idr.size() ? t.idr[row] : uint8_t(kSyncIdrUnknown);
  return row < t.idr.size();
}

void sync_set_idr(const std::wstring &path, uint32_t row, uint8_t state) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_map.find(path);
  if (it == g_map.end()) return;
  SyncTable &t = it->second.t;
  if (row < t.idr.size()) t.idr[row] = state;
}

void sync_stop_lazy(const std::wstring &path) {
  std::lock_guard<std::mutex> lk(g_mu);
  const auto it = g_map.find(path);
  if (it != g_map.end()) it->second.t.lazy_stopped = true;
}

bool sync_verify_idr(const std::wstring &path, const SyncPoint &sp) {
  uint8_t st = uint8_t(kSyncIdrUnknown);
  bool stopped = false;
  int length_size = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_map.find(path);
    if (it == g_map.end() || !it->second.t.computed) return false;
    SyncTable &t = it->second.t;
    if (sp.row >= t.idr.size()) return false;  // 表被降级/换了：这一格没有身份
    st = t.idr[sp.row];
    stopped = t.lazy_stopped;
    length_size = t.length_size;
  }
  // 证过的格不再重读盘：一次证是 IDR 的格，后面的每一跳走到它都白拿（级 3 付过一次钱之后，
  // 同一 GOP 的落点全部落进级 1 缓存或级 2 位置），一次证不是的格也永远不会变成是。
  if (st == uint8_t(kSyncIdrOk)) return true;
  if (st == uint8_t(kSyncIdrBad)) return false;
  if (stopped) return false;  // 挂过 ⇒ 从没证过的格一律不付（不逐格重试）

  const IdrProbe p = probe_sync_idr_at(path, sp.offset, length_size, kWindowBytes);
  if (!p.read_ok || p.slice == 0) {
    // 「读不到」和「这一窗里数不出条片」都不是「这一格不是 IDR」的证据，不能记成坏。
    // 它们只说明这一源的懒验买不起：挂起来，本会话不再逐格重试（也不许每格重试）。
    sync_stop_lazy(path);
    push_trace(sync_wstr(L"同步表懒验: %ls 样=%llu 判不成(read=%d 窗口=%lluB) ⇒ 本源停懒验", path.c_str(),
                         ULL(sp.sample), p.read_ok ? 1 : 0, ULL(p.got)));
    return false;
  }
  const uint8_t next = (p.slice == 5) ? uint8_t(kSyncIdrOk) : uint8_t(kSyncIdrBad);
  sync_set_idr(path, sp.row, next);
  push_trace(sync_wstr(L"同步表懒验: %ls 样=%llu ⇒ %ls", path.c_str(), ULL(sp.sample),
                       next == uint8_t(kSyncIdrOk) ? L"IDR" : L"非IDR(stss 骗人)"));
  return next == uint8_t(kSyncIdrOk);
}

void sync_worker_ensure() {
  bool launch = false;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_running && any_pending()) {
      g_running = true;
      launch = true;
    }
  }
  if (launch) {
    try {
      std::thread(worker_loop).detach();
    } catch (...) {
      std::lock_guard<std::mutex> lk(g_mu);
      g_running = false;
    }
  }
}

bool sync_take_trace(std::wstring *out) {
  std::lock_guard<std::mutex> lk(g_mu);
  if (g_trace.empty()) return false;
  if (out) *out = std::move(g_trace.front());
  g_trace.pop_front();
  return true;
}

void sync_counts(int *tables, int *open, int *pending) {
  int n = 0, k = 0, p = 0;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto &kv : g_map) {
      // 「在路上」= 还等着被取走，或已经交出去但结果还没落表。只数前一半会漏掉算的过程中那一段，
      // 而 `kf_notready` 恰恰就是在那一段里增长的 —— 两格必须同一把尺，否则对不上账。
      if (kv.second.asked || (kv.second.done && !kv.second.t.computed)) ++p;
      if (!kv.second.t.computed) continue;
      ++n;
      if (kv.second.t.kf_open) ++k;
    }
  }
  if (tables) *tables = n;
  if (open) *open = k;
  if (pending) *pending = p;
}

}  // namespace app
