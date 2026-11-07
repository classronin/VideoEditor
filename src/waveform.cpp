#include "waveform.h"

#include "app.h"
#include "media.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

// 后台线程用的匿名实现细节。
namespace {

constexpr int kPerBucket = kWaveSampleRate / kWaveBucketsPerSecond;  // = 192

// 一块 = 10 秒 = 2,500 个桶：这是「已烘/未烘」记账和增量发布的粒度。批不是这块 ——
// 一批 = 视野区间里一段没烘过的连续块（见 pick），所以拟合整只电影时只起一只进程。
constexpr double kChunkSec = 10.0;
constexpr int kChunkBuckets = kWaveBucketsPerSecond * int(kChunkSec);  // 2500
// 每批多要一点的余量。分块靠 -ss 前置定位，实测（同一 build 一组分块/整段对照）
// 分块解出的样本与整段解出的逐样本相同，只是起点比标称早 0.7~1.0ms（早 = 多包含，不丢样本），
// 所以块缝只有轻微重叠、不会空洞。这 0.25 秒给「写满标称桶数」兜底：写满就停手，多的丢掉。
constexpr double kSeamPadSec = 0.25;
// 排队等解的视野区间上限。区间之间会并（见 view 里的合并），所以一次慢扫通常只剩一两条；
// 这个上限只是给「跳到很远的地方再跳回来」这类操作封个顶，超了就丢最老的。
constexpr size_t kMaxPendingIntervals = 8;

struct Interval {
  double lo = 0.0, hi = 0.0;  // 源秒
};

struct Entry {
  std::wstring path;
  std::atomic<bool> running{false};
  std::atomic<bool> stop{false};
  std::mutex mu;                      // 护 data 的内容 / 队列 / baked / cv
  std::condition_variable cv;
  std::shared_ptr<WaveData> data;
  std::vector<uint8_t> baked;         // 每块一字节：1 = 标称桶数已写满（或音轨在那儿就结束了）
  std::deque<Interval> queue;         // 待解的视野区间，最新申报的在前（会互相合并）
  bool busy = false;                  // 这一条 worker 正在解一批（解完才回到 cv.wait）
  bool failed = false;
  float overallmax = 0.0f;            // 跨批累计的样本峰值（Blender 归一用的就是这个数）
  FrameProc proc;
};

std::mutex g_map_mu;
// 每烘完一笔（成功或失败）+1，绘制端用它判断「是不是该再来一帧」。
std::atomic<int> g_wave_done{0};
// 记一笔并说一声。顺序是硬要求的：先加计数、后投递 —— UI 那边先清「在路上的消息」这个旗、
// 再读计数，两头反过来说不清「投递被合并掉的那一笔有没有人读到」。
// 有了这一声，空闲时不必留 15ms 时钟来发现「烘好了」；有活时它也只是把重绘提前一格。
void wave_published() {
  g_wave_done.fetch_add(1);
  app::ui_publish(app::kPubWave);
}
// 故意不析构：烘焙线程是 detached 的，进程收尾时它可能正阻塞在这个 Entry 的管道读上。
// 留一份 new 出来的表，退出时就没有任何析构函数去关它正在用的句柄（FrameProc::~FrameProc
// 会 TerminateProcess + CloseHandle，和另一个线程的 ReadFile 抢同一句柄是未定义行为）。
// 子进程由 media.cpp 的 kill-on-close job 兜底，不靠这里收。
std::map<std::wstring, std::unique_ptr<Entry>> &g_map =
    *new std::map<std::wstring, std::unique_ptr<Entry>>();

// 一桶 = 该时段内所有样本的最小值、最大值、均方根（Blender 每 3 个 float 一组，
// sound.cc:1326-1346）。
void bucket_stats(const float *s, int n, float *mn, float *mx, float *rms) {
  float lo = 0.0f, hi = 0.0f, power = 0.0f;
  for (int i = 0; i < n; i++) {
    const float v = s[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    power += v * v;
  }
  *mn = lo;
  *mx = hi;
  // sqrt(power/len)：Blender 在 sound.cc:1346 用的就是这个，不是加权版本。
  *rms = std::sqrt(power / float(n));
}

// 折叠层有几层、每层几个桶：只由 base 的桶数决定，所以创建时算一次、之后每次烘完照着
// 原地重写值就够了 —— 长度永不变化是绘制线程不持锁读的前提。
size_t level_buckets(size_t n, size_t depth) {
  for (size_t i = 0; i < depth; i++) n /= size_t(WaveData::kFold);
  return n;
}

void alloc_levels(WaveData *w) {
  const size_t n = w->base.size() / 3;
  for (size_t d = 1;; d++) {
    const size_t m = level_buckets(n, d);
    if (m < 1) return;
    w->levels.push_back(std::vector<float>(m * 3, 0.0f));
  }
}

// 只重折受影响的那几个折叠桶。第 d 层的桶 k 盖住 base 的 [k·8^d, (k+1)·8^d) 段，
// 所以 base 的一段 [lo,hi) 在这一层只牵动 [lo/8^d, ceil(hi/8^d)) 那几个桶。
// 为什么不整层重折：3 小时素材整层折一次实测 3.8ms，而这里是「每烘一块折一次」，
// 一千多块就是 4.6 CPU 秒 —— 比解码本身还贵的那种开销不能这么花。增量折每块的量是
// 块大小的一点几倍，跟这块真正写进去的桶数成正比。
// 折的运算（min 取最小 / max 取最大 / rms 取最大）和绘制端跨桶合并用的是同一个，
// 所以「先折再画」和「逐桶扫过来再画」读到的形状一致；还没烘到的桶是 0（= 静音），
// 折进粗层只是那一小段摊平，不会画出假峰。
void fold_range(WaveData *w, size_t lo, size_t hi) {
  size_t a = lo, b = hi;  // 这一层要重算的桶号区间 [a,b)
  const size_t fold = WaveData::kFold;
  for (size_t d = 0; d < w->levels.size(); d++) {
    const std::vector<float> &in = (d == 0) ? w->base : w->levels[d - 1];
    std::vector<float> &out = w->levels[d];
    const size_t m = out.size() / 3;
    const size_t x = a / fold;
    size_t y = (b + fold - 1) / fold;
    if (y > m) y = m;
    for (size_t k = x; k < y; k++) {
      const float *p = in.data() + k * fold * 3;
      float mn = p[0], mx = p[1], rm = p[2];
      for (int j = 1; j < int(fold); j++) {
        mn = std::min(mn, p[j * 3]);
        mx = std::max(mx, p[j * 3 + 1]);
        rm = std::max(rm, p[j * 3 + 2]);
      }
      out[k * 3] = mn;
      out[k * 3 + 1] = mx;
      out[k * 3 + 2] = rm;
    }
    a = x;
    b = y;
    if (b == 0) return;
  }
}

// 解 [lo,hi) 这些桶：起进程 → 每 192 样本一桶 → 每满一块就发布一次（加锁写进 base、
// 增量折层、更新峰值）→ 写满标称桶数就收进程。
// 返回假 = 一个桶也没拿到（起进程失败 / 音轨在那儿就没有），调用方据此判失败。
bool bake_run(Entry *e, const std::wstring &ffexe, size_t lo, size_t hi) {
  const double t0 = double(lo) / kWaveBucketsPerSecond;
  const double dur = double(hi - lo) / kWaveBucketsPerSecond;
  wchar_t s0[32], s1[32];
  swprintf(s0, 32, L"%.6f", t0);
  swprintf(s1, 32, L"%.6f", dur + kSeamPadSec);
  // -vn 明确不解视频（默认的流选择本来也只会挑音轨，写出来才不靠猜）；
  // -ac 1 折成单声道（Blender 用 ChannelMapper，sound.cc:1294 的 specs.channels=MONO），
  // -ar 48000 让桶边界落在整数样本上，-f f32le 直接给出和预览同一种样本格式。
  const std::vector<std::wstring> args = {L"-v",   L"error",     L"-nostdin", L"-ss",  s0,
                                          L"-t",   s1,           L"-i",       e->path, L"-vn",
                                          L"-f",   L"f32le",     L"-ac",      L"1",    L"-ar",
                                          L"48000", L"-"};
  std::wstring err;
  if (!e->proc.start(ffexe, args, &err)) {
    app::trace(L"wave batch fail 起进程: " + err);
    return false;
  }
  const size_t want = hi - lo;
  std::vector<uint8_t> buf(64 * 1024);
  std::vector<float> carry;   // 不足一桶的尾巴
  std::vector<float> pend;    // 攒着待发布的桶（满一块就写进 base）
  float runmax = 0.0f;
  size_t written = 0;    // 这一批算出了多少桶（相对 lo）
  size_t published = 0;  // 其中已经写进 base 的
  bool eof = false;
  while (!e->stop.load() && written < want) {
    // 用 wait_ready 分片等待，好让 stop 能在几百毫秒内被看到；读句柄只由这个线程碰，
    // 所以 kill() 也只在这里调（cancel_all 不许跨线程关句柄）。
    if (!e->proc.wait_ready(100)) {
      if (e->proc.exited()) break;
      continue;
    }
    const size_t got = e->proc.read_some(buf.data(), buf.size());
    if (got == 0) {
      eof = true;  // 管道关闭 = 音轨到头了
      break;
    }
    const int n = int(got / 4);
    carry.insert(carry.end(), reinterpret_cast<const float *>(buf.data()),
                 reinterpret_cast<const float *>(buf.data()) + n);
    size_t used = 0;
    while (carry.size() - used >= size_t(kPerBucket) && written < want) {
      float mn, mx, rms;
      bucket_stats(carry.data() + used, kPerBucket, &mn, &mx, &rms);
      used += size_t(kPerBucket);
      written++;
      runmax = std::max(runmax, std::max(std::abs(mn), std::abs(mx)));
      pend.push_back(mn);
      pend.push_back(mx);
      pend.push_back(rms);
      // 每满一块发布一次：整只电影铺在一屏上看波形时，用户能看着它从左往长出来，
      // 而不是等十几秒见不到东西。发布 = 写进 base + 标 baked + 增量折 + 更新峰值。
      if (pend.size() >= size_t(kChunkBuckets) * 3 || written == want) {
        const size_t nb = e->data->base.size() / 3;
        const size_t from = lo + published;
        const size_t to = std::min(from + pend.size() / 3, nb);
        std::lock_guard<std::mutex> lk(e->mu);
        std::copy(pend.begin(), pend.begin() + long((to - from) * 3),
                  e->data->base.data() + from * 3);
        for (size_t ci = from / size_t(kChunkBuckets); ci < e->baked.size(); ci++) {
          const size_t c_hi = std::min((ci + 1) * size_t(kChunkBuckets), nb);
          if (ci * size_t(kChunkBuckets) >= to) break;
          if (to >= c_hi) e->baked[ci] = 1;  // 只认整块写满的
        }
        e->overallmax = std::max(e->overallmax, runmax);
        // 只在峰值超过 1.0 时才整体除以峰值（sound.cc:1348-1352 的 if (overallmax > 1.0f)）。
        // 这条门槛是「看得出声音大还是小」的全部依据：安静素材（峰值 0.2）若也按峰值归一，
        // 每条声带都会涨满整行，波形就退成了装饰。存的是原始值，分母由绘制端除掉。
        e->data->norm.store(e->overallmax > 1.0f ? e->overallmax : 1.0f);
        fold_range(e->data.get(), from, to);
        published = to - lo;
        pend.clear();
        wave_published();
      }
    }
    carry.erase(carry.begin(), carry.begin() + long(used));
  }
  // 标称桶数写满了就收进程：剩下的 kSeamPadSec 是缝隙保险，本来就要丢掉，
  // 让它继续往管道里灌只会白烧 CPU（管道满了它会一直阻塞到我们 kill）。
  e->proc.kill();
  if (eof && !e->stop.load()) {
    // 音轨到头：这一段后面本来就没声音，整批认下（标成已烘）。不认下的话绘制端
    // 每帧都会对同一块重起一个进程 —— 那是另一种烧 CPU 的死循环。
    std::lock_guard<std::mutex> lk(e->mu);
    for (size_t ci = lo / size_t(kChunkBuckets); ci < e->baked.size(); ci++) {
      if (ci * size_t(kChunkBuckets) >= hi) break;
      e->baked[ci] = 1;
    }
    wave_published();
  }
  return written > 0;
}

void bake_thread(Entry *e) {
  std::wstring ffexe, ffprobe;
  std::wstring locate_err;
  if (!Ffmpeg::locate(&ffexe, &ffprobe, &locate_err)) {
    std::lock_guard<std::mutex> lk(e->mu);
    e->failed = true;
    e->running = false;
    wave_published();
    app::trace(L"wave bake fail 找不到 ffmpeg（" + locate_err + L"）");
    return;
  }
  size_t batches = 0;
  for (;;) {
    size_t lo = 0, hi = 0;
    Interval iv;
    {
      std::unique_lock<std::mutex> lk(e->mu);
      e->cv.wait(lk, [&] { return e->stop.load() || (!e->busy && !e->queue.empty()); });
      if (e->stop.load()) break;
      iv = e->queue.front();
      e->queue.pop_front();
      e->busy = true;
      // 这个区间里第一段还没烘的连续块。已烘的跳过 ⇒ 慢扫时视野和队列会合并成
      // 一大段，但真正解码的只有中间没解过的那几块，扫过的地方不会重解。
      const size_t nb = e->data->base.size() / 3;
      size_t first = size_t(std::max(0.0, iv.lo) / kChunkSec);
      size_t last = size_t(iv.hi / kChunkSec);
      if (first >= e->baked.size()) first = e->baked.size() ? e->baked.size() - 1 : 0;
      if (last >= e->baked.size()) last = e->baked.size() ? e->baked.size() - 1 : 0;
      size_t c = first;
      while (c <= last && e->baked[c]) c++;
      if (c > last) {
        e->busy = false;
        continue;  // 整段都烘过了（新队列为空时再睡）
      }
      size_t r = c;
      while (r + 1 <= last && !e->baked[r + 1]) r++;
      // 剩下的部分放回队头：下次循环接着解（这样视野里没解过的都能轮到，且始终
      // 一段一只进程，不用为「窗口比一批长」再拆进程）。
      if (r + 1 <= last) {
        Interval rest;
        rest.lo = double(r + 1) * kChunkSec;
        rest.hi = iv.hi;
        e->queue.push_front(rest);
      }
      lo = c * size_t(kChunkBuckets);
      hi = std::min((r + 1) * size_t(kChunkBuckets), nb);
    }
    const bool ok = bake_run(e, ffexe, lo, hi);
    size_t done_chunks = 0, q = 0;
    bool still = true;
    {
      std::lock_guard<std::mutex> lk(e->mu);
      for (uint8_t b : e->baked) done_chunks += b;
      q = e->queue.size();
      if (!ok) {
        e->failed = true;
        e->busy = false;
        still = false;
      } else {
        e->busy = false;
      }
    }
    wchar_t msg[256];
    swprintf(msg, 256,
             L"wave batch 桶 [%llu,%llu) 秒 [%.1f,%.1f) 已烘 %llu/%llu 块 队列 %llu 峰值 %.3f",
             static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi), double(lo) / kWaveBucketsPerSecond,
             double(hi) / kWaveBucketsPerSecond, static_cast<unsigned long long>(done_chunks),
             static_cast<unsigned long long>(e->baked.size()), static_cast<unsigned long long>(q), e->overallmax);
    app::trace(msg);
    batches++;
    if (!still) break;
  }
  e->proc.kill();
  std::lock_guard<std::mutex> lk(e->mu);
  e->running = false;
  e->busy = false;
  if (batches) app::trace(L"wave thread exit 批数 " + std::to_wstring(batches));
}

// 把 [lo,hi] 并进等待队列：和已有区间挨得近（缝隙不到一块）的就合成一条。
// 合并是安全的，因为解码会跳过已烘的块 —— 并起来只表示「这些都算用户看过的」，
// 不等于把它们重解一遍。
void push_interval(Entry *e, double lo, double hi) {
  Interval nv{lo, hi};
  for (size_t i = 0; i < e->queue.size(); i++) {
    Interval &q = e->queue[i];
    if (q.lo - kChunkSec <= nv.hi && q.hi + kChunkSec >= nv.lo) {
      q.lo = std::min(q.lo, nv.lo);
      q.hi = std::max(q.hi, nv.hi);
      // 合成后挪到队头：最新视野优先，远处的旧区间排在后面。
      Interval merged = q;
      e->queue.erase(e->queue.begin() + long(i));
      e->queue.push_front(merged);
      return;
    }
  }
  e->queue.push_front(nv);
  while (e->queue.size() > kMaxPendingIntervals) e->queue.pop_back();
}

}  // namespace

WaveCache &WaveCache::instance() {
  static WaveCache c;
  return c;
}

const std::vector<float> &WaveData::level_for(double pixels_per_second,
                                              double *bucket_rate) const {
  double r = double(kWaveBucketsPerSecond);
  const std::vector<float> *use = &base;
  for (const std::vector<float> &lv : levels) {
    // 只在这层仍然「一像素至少一桶」时才继续变粗，所以每列的读数量落在 [1, kFold]。
    const double next = r / double(kFold);
    if (next < pixels_per_second) break;
    use = &lv;
    r = next;
  }
  *bucket_rate = r;
  return *use;
}

std::shared_ptr<WaveData> WaveCache::view(const std::wstring &path, double from_sec,
                                          double to_sec) {
  if (path.empty()) return nullptr;
  // kill switch: 不过门即删，不优化。量导出的口径里有一条「只跑导出」：
  // 波形烘焙是后台 ffmpeg，会和导出抢同一台机器的 CPU，把两臂的墙钟与账一起抬虚。
  // 设了 VSE_NOBAKE=1 就不派活、也不建条目 ⇒ 时间线画平波形，但量出来的是导出自己那份。
  static const bool no_bake = [] {
    wchar_t buf[8] = {0};
    GetEnvironmentVariableW(L"VSE_NOBAKE", buf, 8);
    return buf[0] == L'1';
  }();
  if (no_bake) return nullptr;
  std::lock_guard<std::mutex> lk(g_map_mu);
  auto it = g_map.find(path);
  if (it == g_map.end()) {
    // 桶数组按整条素材的长度定死（Blender 的 waveform 也是整条长度），音轨常比画面长，
    // 多留 1 秒。定死后再也不 resize —— 绘制线程读它时不持锁，靠的就是长度不变。
    VideoInfo *vi = MediaCache::instance().find(path);
    if (!vi || vi->duration <= 0.0) return nullptr;  // 长度还不知道：这一帧先不占位
    const size_t nb = size_t(std::ceil((vi->duration + 1.0) * kWaveBucketsPerSecond));
    auto e = std::make_unique<Entry>();
    e->path = path;
    e->data = std::make_shared<WaveData>();
    e->data->base.assign(nb * 3, 0.0f);
    alloc_levels(e->data.get());
    e->baked.assign((nb + size_t(kChunkBuckets) - 1) / size_t(kChunkBuckets), 0);
    it = g_map.emplace(path, std::move(e)).first;
    app::trace(L"wave alloc 桶 " + std::to_wstring(nb) + L" 块 " +
               std::to_wstring(it->second->baked.size()) + L" " + path);
  }
  Entry *e = it->second.get();
  {
    std::unique_lock<std::mutex> lk2(e->mu);
    if (e->failed) return nullptr;
    const double lo = std::max(0.0, from_sec);
    const double hi = std::max(lo, to_sec);
    if (hi > lo) {
      push_interval(e, lo, hi);
      // 已经睡死的线程（烘失败过又没被 reset 放行的）由这里重起来；正常情况它常驻。
      if (!e->running.load()) {
        e->running = true;
        lk2.unlock();
        std::thread(bake_thread, e).detach();
        return e->data;
      }
      e->cv.notify_one();
    }
  }
  return e->data;
}

int WaveCache::done() { return g_wave_done.load(); }

void WaveCache::cancel_all() {
  {
    std::lock_guard<std::mutex> lk(g_map_mu);
    for (auto &kv : g_map) {
      kv.second->stop = true;
      kv.second->cv.notify_all();
    }
  }
  // 给烘焙线程一点时间自己收（它每 100ms 看一次 stop 标志，正常立刻退出，顺手 kill 子进程）。
  // 等不到也没关系：子进程都在 kill-on-close job 里，主程序一退出内核就全清，所以这里的
  // 上限是「关窗口的手感延迟」而不是「会不会留孤儿」——留在 250ms。
  const ULONGLONG t0 = GetTickCount64();
  while (GetTickCount64() - t0 < 250) {
    bool any = false;
    {
      std::lock_guard<std::mutex> lk(g_map_mu);
      for (auto &kv : g_map)
        if (kv.second->running.load()) any = true;
    }
    if (!any) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void WaveCache::reset() {
  std::lock_guard<std::mutex> lk(g_map_mu);
  for (auto &kv : g_map) {
    if (kv.second->running.load()) continue;
    std::lock_guard<std::mutex> lk2(kv.second->mu);
    // 已经烘到的留着（同一块不必重解），只把「因为找不到 ffmpeg 而失败」的重新放行：
    // 线程已经退出去了，下一次 view() 看到队列里还排着区间会把它重起来。
    if (kv.second->failed) {
      kv.second->failed = false;
      kv.second->queue.clear();  // 失败前攒的旧视野不算数，等绘制端重新申报
    }
  }
}
