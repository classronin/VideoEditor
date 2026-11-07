#include "preview.h"
#include "publish.h"
#include "synctable.h"
#include <algorithm>
#include <chrono>
#include <cmath>

// ops.cpp 里的 VSE_TRACE 落盘函数（在 namespace app 内，和 export.cpp 同一取法）。
namespace app {
void trace(const std::wstring &s);
}
using app::trace;

namespace {
double qpc_ms() {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) * 1000.0 / f.QuadPart;
}

// 流是否已经没用了：进程/管道没了，或管道空且 ffmpeg 已退出。
// 只在看不到数据时才认 exited()，避免把解码暂时跟不上误判成素材到头。
bool stream_dead(FrameReader &rd) {
  return !rd.live() || (!rd.data_ready() && rd.ended());
}
}  // namespace

// ---------------------------------------------------------------- FrameReader

void FrameReader::stop() {
  if (proc_) {
    proc_->kill();
    proc_.reset();
  }
  mf_.reset();
  t_mf_ = -1.0;
  accum_.clear();
  index_ = -1;
  eof_ = false;
}

bool FrameReader::start(const VideoInfo &vi, double seconds, int out_w, int out_h) {
  stop();
  vi_ = &vi;
  path_ = vi.path;
  // MF 那一路的时钟原点归 MfStream 自己管（开流探原点、定位加、读回减），交回的 t 已经是
  // 「从首帧起算」的相对秒，和池里 req.target 同一把尺。这里不再叠 video_pts/start_time：
  // 那是 ffprobe 的容器基，和 MF 自己的原点是两笔账，叠在一起就是同一个值两处写。
  if (MfStream::enabled() && !MfStream::banned(vi.path)) {
    auto s = std::make_unique<MfStream>();
    if (s->open(vi.path, seconds, out_w, out_h)) {
      mf_ = std::move(s);
      t0_ = seconds;
      fps_ = vi.src_fps();
      w_ = out_w;
      h_ = out_h;
      return true;
    }
  }
  proc_.reset(Ffmpeg::video_stream(vi, seconds, out_w, out_h));
  if (!proc_) return false;
  t0_ = seconds;
  fps_ = vi.src_fps();
  w_ = out_w;
  h_ = out_h;
  return true;
}

bool FrameReader::seek(double seconds) {
  // 同 start：交进 MF 的是相对秒，MfStream 内部加它自己的原点去定位；t0_ 记相对基供池比较。
  if (!mf_ || !mf_->seek(seconds)) return false;  // 子进程没有这条路
  t0_ = seconds;
  index_ = -1;
  t_mf_ = -1.0;
  eof_ = false;
  accum_.clear();
  return true;
}

bool FrameReader::data_ready() {
  if (mf_) return !eof_;  // MF 是同步取帧，没有「管道里还没数据」这回事
  return proc_ && proc_->wait_ready(0);
}

bool FrameReader::next() {
  if (mf_) {
    if (!eof_ && mf_->read()) {
      bgra_ = mf_->take_bgra();
      w_ = mf_->w();
      h_ = mf_->h();
      // MfStream 交回的已经是「从首帧起算」的相对秒（它内部减过自己探到的原点），
      // 与子进程那一路的 t0_+index/fps 同一把尺，池里 position()/accept 窗直接可比。
      t_mf_ = mf_->t();
      ++index_;
      return true;
    }
    // 一帧都没出、而且失败的原因是「流进了错误态」：这路编码系统解码器解不了，但 MF 的开流是
    // 成功的（容器它认，编码不认；实测 mpeg4 part2 到 ReadSample 才报错）。留着这条流画面就是
    // 全黑，而且池每轮都会再开一只这样的流。所以就地换成子进程、从同一个位置重开，并把这只素材
    // 记进黑名单 —— 判它行不行只能解完第一帧，之后就别再付这笔钱。
    if (index_ < 0 && mf_->rejected()) {
      MfStream::ban(path_);
      const VideoInfo *vi = vi_;
      const double t = t0_;
      const int w = w_, h = h_;
      stop();
      mf_gave_up_ = true;
      if (vi && start(*vi, t, w, h)) return next();
    }
    eof_ = true;
    return false;
  }
  if (!proc_ || eof_) return false;
  // 子进程交的是裸 BGRA（-f rawvideo -pix_fmt bgra）：尺寸开流时就定死了（scale=out_w:out_h），
  // 所以不必再靠 PPM 头把宽高带回来，也不用逐像素换序 —— 纹理要的就是这几个字节。
  const size_t bytes = size_t(w_) * size_t(h_) * 4;
  for (;;) {
    if (accum_.size() >= bytes) {
      bgra_.resize(bytes);
      memcpy(bgra_.data(), accum_.data(), bytes);
      accum_.erase(accum_.begin(), accum_.begin() + long(bytes));
      ++index_;
      return true;
    }
    uint8_t tmp[1 << 18];
    size_t got = proc_->read_some(tmp, sizeof tmp);
    if (!got) {
      eof_ = true;
      return false;
    }
    accum_.insert(accum_.end(), tmp, tmp + got);
  }
}

// ---------------------------------------------------------------- Preview

void Preview::request(const std::vector<Target> &targets) {
  const double now = qpc_ms();
  std::lock_guard<std::mutex> lk(mu_);
  if (!thread_.joinable() && !quit_) thread_ = std::thread([this] { worker_main(); });
  std::vector<Request> next;
  next.reserve(targets.size());
  for (const Target &t : targets) {
    if (!t.vi || t.w < 16 || t.h < 16) continue;
    Request r;
    r.lane = t.lane;
    r.vi = t.vi;
    // 目标量化到整帧：Blender 的守卫是「帧号没变就不通知重渲染」（anim_ops.cc:601-605），
    // 一格像素以内的鼠标抖动不该把解码池叫醒。哪一只帧由 VideoInfo::src_frame 说了算 ——
    // 导出的逐 tick 摆放读同一个所有者，两处不许各有答案。
    const double fps = t.vi->src_fps();
    const double s = t.source_seconds < 0 ? 0.0 : t.source_seconds;
    r.target = double(t.vi->src_frame(s)) / fps;
    r.w = t.w;
    r.h = t.h;
    r.active = true;
    next.push_back(r);
  }
  // 目标没变就不叫醒工作线程：空闲时它应当完全不醒来。
  if (next.size() == reqs_.size()) {
    size_t i = 0;
    for (; i < next.size(); ++i) {
      const Request &n = next[i], &o = reqs_[i];
      if (n.lane != o.lane || n.vi != o.vi || n.target != o.target || n.w != o.w || n.h != o.h)
        break;
    }
    if (i == next.size()) return;
  }
  // 跳位记录：一次变化跨过 kJumpFrames 帧以上记一笔，连续（间隔在 kMoveGapMs 以内）多笔
  // 就是「正在拖播放头」；播放每 tick 只走约半帧，落不进这个条件，半帧以内的变化会把计数清掉。
  std::map<int64_t, double> prev_t;
  for (const Request &o : reqs_) prev_t[o.lane] = o.target;
  std::map<int64_t, LaneJump> next_jump;
  for (const Request &n : next) {
    auto it = prev_t.find(n.lane);
    if (it == prev_t.end() || n.target == it->second) continue;
    const double fps = n.vi->src_fps();
    if (std::fabs(n.target - it->second) * fps < kJumpFrames) continue;
    LaneJump &j = next_jump[n.lane];
    const LaneJump *old = jump_.count(n.lane) ? &jump_[n.lane] : nullptr;
    j.at = now;
    j.run = old && now - old->at < kMoveGapMs ? old->run + 1 : 1;
  }
  jump_ = std::move(next_jump);
  reqs_ = std::move(next);
  dirty_ = true;
  cv_.notify_one();
}

bool Preview::pump() {
  double t0 = qpc_ms();
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto &[lane, s] : pub_) {
      auto it = shown_.find(lane);
      if (it == shown_.end() || it->second.gen != s.gen) {
        shown_[lane] = s;  // 只在真换了画面时更新；字节是 shared_ptr，UI 线程不搬整幅
        fresh = true;
      }
    }
    have_frame_ = !shown_.empty();
  }
  double ms = qpc_ms() - t0;  // 含锁等待：这正是要盯的 UI 成本
  ++stats_.pumps;
  stats_.pump_ms += ms;
  if (ms > stats_.max_pump_ms) stats_.max_pump_ms = ms;
  return fresh;
}

const uint8_t *Preview::frame(int64_t lane, int *w, int *h) const {
  auto it = shown_.find(lane);
  if (it == shown_.end() || !it->second.frame.bgra || it->second.frame.bgra->empty())
    return nullptr;
  *w = it->second.frame.w;
  *h = it->second.frame.h;
  return it->second.frame.bgra->data();
}

uint64_t Preview::frame_token(int64_t lane) const {
  auto it = shown_.find(lane);
  return it == shown_.end() ? 0 : it->second.gen;
}

void Preview::reset() {
  std::lock_guard<std::mutex> lk(mu_);
  if (!thread_.joinable()) return;
  reqs_.clear();
  jump_.clear();
  dirty_ = true;
  cv_.notify_one();
}

void Preview::shutdown() {
  std::unique_lock<std::mutex> lk(mu_);
  if (!thread_.joinable()) return;
  quit_ = true;
  cv_.notify_all();
  lk.unlock();
  thread_.join();
  thread_ = std::thread();
}

Preview::Stats Preview::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

void Preview::show_frame(int64_t lane, const Frame &f, int64_t want) {
  bool published = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    Shown &s = pub_[lane];
    if (s.frame.bgra && s.frame.vi == f.vi && s.frame.t == f.t && s.frame.w == f.w &&
        s.frame.h == f.h)
      return;
    s.frame = f;
    s.gen = ++pub_gen_;
    published = true;
    if (worker_spawn_at_ >= 0.0) {
      double ms = qpc_ms() - worker_spawn_at_;
      stats_.first_ms += ms;
      if (ms > stats_.max_first_ms) stats_.max_first_ms = ms;
      worker_spawn_at_ = -1.0;
    }
  }
  // 交出去的那一帧和规则要的那一帧不是同一只 ⇒ 落一行。规则 = VideoInfo::src_frame，
  // 导出那一路读的是同一个所有者，所以这一行也就是「预览和成片会不会对不上」的直接读数。
  // want<0 = 调用方没给规则答案（不该发生，给了就是为了这一行不必到处写）。
  if (want >= 0 && f.vi) {
    const int64_t got = f.vi->src_frame(f.t);
    if (got != want) {
      wchar_t b[192];
      swprintf(b, 192, L"预览落帧: lane=%lld 规则要第%lld只 交第%lld只 t=%.6f fps=%.6f",
               (long long)lane, (long long)want, (long long)got, f.t, f.vi->src_fps());
      trace(b);
    }
  }
  // 出了锁再说这一声：UI 收到就调 pump()，那一步要拿同一把 mu_。
  // 有了这一声，worker 交帧不再需要 15ms 时钟去发现；回放时仍然由时钟按帧率推进，
  // 那一格本来就在，所以这里不会把画面推进得比素材帧率还快。
  if (published) app::ui_publish(app::kPubPreview);
}

void Preview::spawn(FrameReader &rd, const Request &req) {
  double t0 = qpc_ms();
  const bool tried_mf = MfStream::enabled() && !MfStream::banned(req.vi->path);
  rd.start(*req.vi, req.target, req.w, req.h);
  double ms = qpc_ms() - t0;
  std::lock_guard<std::mutex> lk(mu_);
  ++stats_.restarts;
  if (tried_mf) {
    if (rd.uses_mf()) ++stats_.mf_open;
    else ++stats_.mf_refused;
  }
  stats_.spawn_ms += ms;
  if (ms > stats_.max_spawn_ms) stats_.max_spawn_ms = ms;
  worker_spawn_at_ = t0;  // 下一帧发布时记下“重建到出画”的耗时
}

const Preview::Frame *Preview::decode_step(const VideoInfo *vi, FrameReader &rd, SourceCache &sc) {
  if (!rd.live() || !rd.data_ready()) return nullptr;
  if (!rd.next()) return nullptr;
  if (rd.take_mf_gave_up()) {
    // 开流时记成了 MF，解不出第一帧才知它不行：计数跟着改口，否则 mf=75/0 配 mfdec=0 骗人。
    std::lock_guard<std::mutex> lk(mu_);
    if (stats_.mf_open > 0) --stats_.mf_open;
    ++stats_.mf_refused;
  }
  Frame f;
  f.vi = vi;
  f.t = rd.frame_time();
  f.w = rd.w();
  f.h = rd.h();
  f.bgra = std::make_shared<const std::vector<uint8_t>>(rd.take_bgra());
  sc.bytes += f.bgra->size();
  sc.frames.push_back(std::move(f));
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.decoded;
    if (rd.uses_mf()) ++stats_.mf_decoded;
  }
  return &sc.frames.back();
}

const Preview::Frame *Preview::cache_lookup(const SourceCache &sc, double target,
                                            double frame_dur) {
  const Frame *best = nullptr;
  double best_dt = 0.0;
  for (const auto &f : sc.frames) {
    double dt = std::fabs(f.t - target);
    if (!best || dt < best_dt) {
      best = &f;
      best_dt = dt;
    }
  }
  return best_dt <= frame_dur * 0.75 ? best : nullptr;
}

void Preview::evict(std::map<const VideoInfo *, SourceCache> &caches,
                    const std::vector<Request> &reqs) {
  // 保留窗口 = 该源这一轮所有 active 请求的 [最小目标-身后留量, 最大目标+预取]。同源多条带
  // 相隔几秒时，落后几条要的正是领先那条早先解出、落在窗口里的历史帧；旧版只以第一条请求
  // 为参照，把落后几条的帧当远帧扔掉 ⇒ 各开各的流解 N 遍。窗口外（这一轮没人要）先扔，
  // 内存就等于「跨度+留量」；单条带时 min==max，窗口回到旧的小窗，不会囤一大段历史。
  struct Win { double lo = 0, hi = 0; bool any = false; };
  auto win_of = [&](const VideoInfo *vi) {
    Win w;
    const double ahead = kAheadFrames / vi->src_fps();
    for (const Request &r : reqs) {
      if (!r.active || r.vi != vi) continue;
      if (!w.any) { w.lo = w.hi = r.target; w.any = true; }
      else { w.lo = std::min(w.lo, r.target); w.hi = std::max(w.hi, r.target); }
    }
    if (w.any) { w.lo -= kBehindSec; w.hi += ahead; }
    return w;
  };
  // 一帧离窗口的距离：窗内 0；窗外按超出量，身后罚 2 倍（保留来回拖时刚看过那段的手感）。
  auto dist_of = [](const Frame &f, const Win &w) {
    if (f.t >= w.lo && f.t <= w.hi) return 0.0;
    return f.t < w.lo ? (w.lo - f.t) * 2.0 : (f.t - w.hi);
  };
  auto drop_worst = [&](SourceCache &sc, const Win &w) -> size_t {
    if (sc.frames.empty()) return 0;
    auto worst = sc.frames.begin();
    double wd = -1.0;
    for (auto it = sc.frames.begin(); it != sc.frames.end(); ++it) {
      double d = dist_of(*it, w);
      if (d > wd) { wd = d; worst = it; }
    }
    size_t bytes = worst->bgra->size();
    sc.bytes -= bytes;
    sc.frames.erase(worst);
    return bytes;
  };
  size_t total = 0;
  for (auto &[vi, sc] : caches) {
    const Win w = win_of(vi);
    if (!w.any) { total += sc.bytes; continue; }  // 没人请求的素材不动它
    // 窗口外先扔（哪怕还没到预算）：留着就是占着跨度之外的内存。
    while (!sc.frames.empty()) {
      double wd = -1.0;
      for (const Frame &f : sc.frames) wd = std::max(wd, dist_of(f, w));
      if (wd <= 0.0) break;
      if (!drop_worst(sc, w)) break;
    }
    while (sc.frames.size() > kMaxFramesPerSource) {
      if (!drop_worst(sc, w)) break;
    }
    total += sc.bytes;
  }
  // 外层安全上限：跨度本身超预算（条太多/隔太远/分辨率太高）才从离窗口最远的开始让位。
  while (total > kCacheBytes) {
    SourceCache *host = nullptr;
    Win host_w;
    double worst = -1.0;
    for (auto &[vi, sc] : caches) {
      const Win w = win_of(vi);
      if (!w.any || sc.frames.empty()) continue;
      for (const Frame &f : sc.frames) {
        double d = dist_of(f, w);
        if (d > worst) { worst = d; host = &sc; host_w = w; }
      }
    }
    if (!host) break;  // 剩下的都是没人请求的素材：留着，不为它们腾地方
    total -= drop_worst(*host, host_w);
  }
}

// 池内挑选：键为「素材 + 解码尺寸」，候选必须仍能顺序前进到目标，而且本轮没被别的 lane 占用
// ——一条顺序流同时只能朝一个位置走，同素材的两条带各要一条（Blender 那边读取器本来就直接
// 挂在 Strip 上）。同 owner 的优先：跟随这条 lane 上一次的流，位置最贴，也免得来回抢。
// 续期记的是「这一道还在不在为用户服务」，不是「这只流现在顺不解得到」——后者只是挑选条件。
// 两件事原来混在一个筛里，实测的代价：预取把流甩到目标前 8 帧，随后连续 8 轮都由帧缓存出画、
// 一轮也续不到 ⇒ 隔满 8 轮被陈旧杀掉 ⇒ 循环回跳那一格池空、100ms 内重开 5 条 ffmpeg 进程。
// 只保鲜、不复活：隔满 kReaderStaleRounds-1 的不再续，停手或离开这一道之后的回收照旧发生。
Preview::ReaderEntry *Preview::acquire_reader(
    std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req, double frame_dur,
    uint64_t stamp, const std::vector<ReaderEntry *> &claimed, bool count_reuse) {
  ReaderEntry *best = nullptr, *owned = nullptr;
  double best_score = 0.0;
  for (auto &e : pool) {
    if (e->vi != req.vi || e->w != req.w || e->h != req.h) continue;
    if (stream_dead(e->rd)) continue;
    if (std::find(claimed.begin(), claimed.end(), e.get()) != claimed.end()) continue;
    const double lag = req.target - e->rd.position();
    if (e->owner == req.lane) {
      if (stamp - e->stamp < kReaderStaleRounds - 1) e->stamp = stamp;
      if (lag >= -frame_dur * 0.5 && !owned) owned = e.get();
      continue;
    }
    if (lag < -frame_dur * 0.5) continue;
    const double score = lag / frame_dur;
    if (!best || score < best_score) {
      best = e.get();
      best_score = score;
    }
  }
  ReaderEntry *res = owned ? owned : best;
  if (res) {
    res->stamp = stamp;
    res->owner = req.lane;
    if (count_reuse) {
      std::lock_guard<std::mutex> lk(mu_);
      ++stats_.reader_reuse;
    }
  }
  return res;
}

Preview::ReaderEntry *Preview::open_reader(
    std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req, uint64_t stamp) {
  // 超容量先清掉已经死的，再按"隔了多少次请求没用过"淘汰最陈的（对齐 stale 判定）。
  if (pool.size() >= kMaxReaders) {
    std::erase_if(pool, [](const auto &e) { return stream_dead(e->rd); });
  }
  if (pool.size() >= kMaxReaders) {
    auto oldest = std::min_element(pool.begin(), pool.end(),
                                   [](const auto &a, const auto &b) { return a->stamp < b->stamp; });
    // Blender 允许超出 max_entries（它的 entry 只是内存里的 avformat 句柄）；我这里每条是一个
    // 子进程，必须硬上限，所以按请求序号淘汰最久未用的一条。
    if (oldest != pool.end()) {
      (*oldest)->rd.stop();
      pool.erase(oldest);
    }
  }
  auto e = std::make_unique<ReaderEntry>();
  e->vi = req.vi;
  e->w = req.w;
  e->h = req.h;
  e->owner = req.lane;
  e->stamp = stamp;
  ReaderEntry *res = e.get();
  pool.push_back(std::move(e));
  spawn(res->rd, req);
  return res;
}

// 目标落在这条 lane 自己的流已经走过的地方。Blender 也是在同一只句柄里 seek 的
// （movie_reader_cache.cc:187-215 给后退乘 32 罚分，但用的是同一个句柄）；子进程做不到，
// MF 做得到，所以这里先退回去，退不了才另开一条。
// 只动 owner 就是这条 lane 的流：别的 lane 的流有它自己的位置要守。
Preview::ReaderEntry *Preview::rewind_reader(
    std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req, double frame_dur,
    uint64_t stamp, const std::vector<ReaderEntry *> &claimed) {
  ReaderEntry *best = nullptr;
  double best_d = 0.0;
  for (auto &e : pool) {
    if (e->owner != req.lane || e->vi != req.vi || e->w != req.w || e->h != req.h) continue;
    if (!e->rd.uses_mf() || stream_dead(e->rd)) continue;
    if (std::find(claimed.begin(), claimed.end(), e.get()) != claimed.end()) continue;
    // 落在目标后面不叫「退回来」：那一段顺解只要 1.4ms/帧，比任何定位都便宜，交给下面的顺解循环。
    if (e->rd.position() < req.target - frame_dur * 0.5) continue;
    double d = std::fabs(e->rd.position() - req.target);
    if (!best || d < best_d) {
      best = e.get();
      best_d = d;
    }
  }
  if (!best) return nullptr;
  const double t0 = qpc_ms();
  if (!best->rd.seek(req.target)) return nullptr;
  best->stamp = stamp;
  const double ms = qpc_ms() - t0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.in_place_seeks;
    if (ms > stats_.max_seek_ms) stats_.max_seek_ms = ms;
  }
  return best;
}

// 把「借不借得到流」从「顺解走不走得到」里分出来。候选 = 本 lane 自己的活 MF 流
// （同素材、同解码尺寸、本轮没被认领）—— 别的 lane 的流有它自己的位置要守，抢过来会把那条
// lane 的定位方向掰走（Blender 把读取器按 Strip 独占也是这件事）。
// 这里**不看位置**：位置在落点之后正是「要往回借」的定义，不是排除条件。
// 只借 MF 的流：子进程做不到就地定位（FrameReader::seek 对它直接回 false），借过来只能记一次假失败。
Preview::ReaderEntry *Preview::borrow_reader(
    std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req,
    const std::vector<ReaderEntry *> &claimed) {
  for (auto &e : pool) {
    if (e->owner != req.lane || e->vi != req.vi || e->w != req.w || e->h != req.h) continue;
    if (!e->rd.uses_mf() || stream_dead(e->rd)) continue;
    if (std::find(claimed.begin(), claimed.end(), e.get()) != claimed.end()) continue;
    return e.get();
  }
  return nullptr;
}

// 一轮服务一条 lane。后退的处理照 Blender 的思路但代价不同：它可以在同一个 avformat 句柄里
// seek 并给后退乘 32 罚分（movie_reader_cache.cc:187-215），ffmpeg 子进程做不到，后退就另开
// 一条流、把旧的留在池里等淘汰 —— 这正是池的价值：来回拖播放头不必反复付启动进程的钱。
void Preview::service_lane(const Request &req, std::vector<std::unique_ptr<ReaderEntry>> &pool,
                           std::map<const VideoInfo *, SourceCache> &caches, uint64_t stamp,
                           std::vector<ReaderEntry *> &claimed) {
  double fps = req.vi->src_fps();
  double frame_dur = 1.0 / fps;
  // 规则在这一格要的第几只源帧（问的就是 VideoInfo::src_frame 这一个所有者，导出那一路也是它）。
  const int64_t want_idx = req.vi->src_frame(req.target);
  SourceCache &sc = caches[req.vi];

  // 这一路是不是「正在被拖着走」：连续两次以上的跳位、且最后一次还没停够 kSettleMs。
  const double now = qpc_ms();
  double last_jump = -1.0;
  int jump_run = 0;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = jump_.find(req.lane);
    if (it != jump_.end()) {
      last_jump = it->second.at;
      jump_run = it->second.run;
    }
  }
  const bool scrubbing = jump_run >= 2 && last_jump >= 0.0 && now - last_jump < kSettleMs;

  // 先查帧缓存，命中就根本不必碰解码器（Blender 也把缓存放在读取器前面）。拖动中这一路照常
  // 出画：看过的段还在缓存里，画面跟手，但一分钱进程启动都不花。
  if (const Frame *hit = cache_lookup(sc, req.target, frame_dur)) {
    // 命中也要给这条 lane 的流续期并认领它：否则纯靠缓存的这段时间里它会被判陈旧关掉，
    // 播放头一跑出缓存范围反倒要多付一次进程启动。
    ReaderEntry *warm = acquire_reader(pool, req, frame_dur, stamp, claimed, false);
    if (warm) claimed.push_back(warm);
    {
      std::lock_guard<std::mutex> lk(mu_);
      ++stats_.cache_hits;
    }
    show_frame(req.lane, *hit, want_idx);
    return;
  }

  // 先把「现有源流顺手走不走得到」问清楚（这一步不 seek、不开进程，只挑流并续期）。
  ReaderEntry *e = acquire_reader(pool, req, frame_dur, stamp, claimed, false);
  if (e) claimed.push_back(e);
  const bool walkable =
      e && e->rd.position() <= req.target + frame_dur * 0.5 &&
      req.target - e->rd.position() <= frame_dur * kAheadFrames;

  // 这一轮真正交付的位置：默认就是目标；查表接住时换成「落点之前最近的那只同步点」。
  // 后面的落后判定、就地定位、解码到哪儿算到位、预取上界都必须跟着它 —— 各写各的话，
  // 刚付完一次定位的那一格会立刻被 lag>2s 再判成落后，同一格付第二次。
  double serve_at = req.target;
  bool served = false;  // 级2/级3 接住了这一跳（级1 自己 return，不进这两个变量）
  if (scrubbing) {
    // 播放头还在跳：只解「现有流顺手就能走到」的位置（前进 kAheadFrames 帧以内）。要另开
    // 子进程的按住不做，等它停下来再定位——一次拖动付一次启动钱，而不是每个鼠标位一次。
    // Blender 挂起的是预取线程（prefetch.cc:561-564 的 is_scrubbing），我挂起的是最贵的开进程。
    if (!walkable) {
      // 查表走不到 ⇒ 原来的做法只有「画面保持上一帧」。
      // 会话同步点表能给的是**落点之前最近的那只同步点的秒数**：到那儿只解一帧，画面跟着手走。
      // 三级按价钱排 —— 级1 帧缓存（内存查找，免费）、级2 池里现有的流顺解得到（几乎免费）、
      // 级3 同一只流就地定位（付一次 seek，实测 70~844ms 挂钟）。级3 只对「这一格」读一窗懒验
      // （建表时没跑判据①，缺的那半在这里按格补）。付过一次之后同一只 GOP 里的落点全部
      // 由级1/级2 接住 ⇒ 一次拖动同一只同步点只付一次，不需要另外记「已付过」的表。
      // 表由本模块自持的 worker 建：这里只 lookup / peek / 登记，绝不补算、绝不阻塞。
      // 三级都没接住 ⇒ 照旧 deferred，行为与写这一层之前一字不差（回退门第 0 层）。
      app::sync_request(req.vi->path, fps);  // 拖到哪只才算哪只：不预扫全工程，每源一次
      app::SyncPoint sp;
      if (app::sync_lookup(req.vi->path, req.target, &sp)) {
        // 续期不在这里写：这一格没解源码，但这一道还在为用户服务，那条规矩整个长在
        // acquire_reader 里（一处记账），下面的 borrow_reader 只问「借不借得到去就地定位的流」。
        if (const Frame *hit = cache_lookup(sc, sp.pts, frame_dur)) {
          ReaderEntry *warm = acquire_reader(pool, req, frame_dur, stamp, claimed, false);
          if (warm) claimed.push_back(warm);
          {
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.kf_free;
          }
          show_frame(req.lane, *hit, want_idx);
          return;
        }
        // 级2/级3 前面那次问问的是「顺解走不走得到」，它的位置筛把「已经在落点之后」
        // 的流一律丢掉，所以向后拖时 `e` 恒为空、级2/级3 结构性不可达。这里再问一遍「有没有活流
        // 可借去就地定位」；借不到照旧记 kf_nostream（不为拖动中的跳位开流，这条边界不松）。
        ReaderEntry *use = (e && !stream_dead(e->rd)) ? e : borrow_reader(pool, req, claimed);
        if (use && use != e) {
          claimed.push_back(use);
          // 借到 = 用过 —— 续期走的是和顺解命中**同一把尺**（同一个 `stamp`、同一个
          // `kReaderStaleRounds`，不新增字段/计时器/淘汰队列）。写在调用点而不是 borrow_reader 里：
          // 那个函数必须是纯查询，它一写就把被量的东西改了。
          // 不续期的代价实测过：一路都借到、`隔` 却只涨不跌，8 轮一到被陈旧杀掉 ⇒ 池空 ⇒
          // 停手那一格按 why=1 重开一条流（`restart` 的 `src` 1→2）。
          use->stamp = stamp;
        }
        if (use) {
          if (use->rd.position() <= sp.pts + frame_dur * 0.5 &&
              sp.pts - use->rd.position() <= frame_dur * kAheadFrames) {
            serve_at = sp.pts;
            served = true;
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.kf_walk;
          } else if (app::sync_verify_idr(req.vi->path, sp)) {
            const double t0 = qpc_ms();
            if (use->rd.seek(sp.pts)) {
              serve_at = sp.pts;
              served = true;
              const double ms = qpc_ms() - t0;
              std::lock_guard<std::mutex> lk(mu_);
              ++stats_.kf_paid;
              ++stats_.in_place_seeks;  // 这一跳省的正是「另开一条流」那 1.5 秒
              if (ms > stats_.max_seek_ms) stats_.max_seek_ms = ms;
            } else {
              std::lock_guard<std::mutex> lk(mu_);
              ++stats_.kf_seekfail;  // 表说可以付、这一格也证过是 IDR，但流定位不过去
            }
          } else {
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.kf_gated;  // 懒验不给这一格付钱（非 IDR / 本源懒验已挂），逐格原因在 trace
          }
          if (served) e = use;  // 借来的流交付了这一格：后面那圈解码/落后判定都以它为基准，
                                // 不再走 rewind_reader（它会定位到已作废的目标）
        } else {
          // 表说这一格可以付，但池里没有拿得出来的流（从没为这个尺寸开过，或已经死了）。
          // 第一段的边界是「不为拖动中的跳位开流」，所以这一格只能照旧保持上一帧 —— 但它既不是
          // 「门关」（源不配）也不是「流拒了定位」，混进那两个数会把两种不同的修法读成一种。
          std::lock_guard<std::mutex> lk(mu_);
          ++stats_.kf_nostream;
        }
      } else if (app::SyncTable st; app::sync_peek(req.vi->path, &st)) {
        // 表在但这一跳它不给付：门关、不可信，或落点在第一只同步点之前。和「还没建好」分开数
        // （回退门：读不到表与门关不许并成一个数）。
        //
        // **门关 ∧ 本道有可用流 ⇒ 不 defer**，把这一格交给既有下游那一步 —— 落后 >2s
        // 就在同一只流里定位，否则顺解追过去。这一支只删掉「门关就整格挂起」这一条界，不新增任何定位手段：
        // 老路本来就是这么做的，只是它从来没机会在拖动中走到那一步。为什么值得删：实测就地向后定位只要
        // 1.98~3.55 ms、整机与 defer 同价，而原来「门关 ⇒ 保持上一帧」把便宜的那件也一起捆走了。
        // 四条边界（逐条写死，一条不加一条不减）：
        // · 只对 `trusted`（表算得出来）而门已关的源生效；**不可信源（含 reason=19）照旧 kf_gated + defer**。
        // · 门开时 `sync_lookup` 在上面那一支就给付了（这里一字未动），进不到这一支；
        //   「门开但落点在第一只同步点之前」这个子情形也照旧 kf_gated + defer，**不放行**。
        // · **不放开「为拖动中的跳位开流」**：拿不出可用流就记 kf_nostream 并照旧 defer，绝不走到下面的 open_reader。
        // · 整段在 `scrubbing` 里 ⇒ 停手后仍走既有精确落点。
        if (!st.trusted || st.kf_open) {
          std::lock_guard<std::mutex> lk(mu_);
          ++stats_.kf_gated;
        } else {
          // 「有可用流」= 本道流已挑中 ∧ 未死 ∧ 池里在 ∧ 做得到就地定位。**不含位置条件** —— 流在落点
          // 之前还是之后一律不问，那一问是下游 `lag > kLagRestart` 的职责。
          // uses_mf 不是位置条件，是能力条件：子进程读者的 seek() 恒 false，把落后 >2s 的格交给它只会落到
          // 「另开一条流」，那就是上面那条边界明令禁止的事（同 borrow_reader / rewind_reader 的既有理由）。
          ReaderEntry *live = (e && !stream_dead(e->rd) && e->rd.uses_mf())
                                  ? e
                                  : rewind_reader(pool, req, frame_dur, stamp, claimed);
          if (live) {
            e = live;  // 认领交给下面那一步（`if (e) claimed.push_back(e)`），这里再推一次就是重复
            served = true;  // serve_at 保持 req.target：交给下游就是照鼠标那一格定位/顺解
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.gc_serve;
          } else {
            std::lock_guard<std::mutex> lk(mu_);
            ++stats_.kf_nostream;  // 门关 ∧ 池空：这一格照旧保持上一帧，等停手
          }
        }
      } else {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.kf_notready;
      }
      if (!served) {
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.deferred;
        const double ready = last_jump + kSettleMs;
        if (ready > wake_at_) wake_at_ = ready;
        return;  // 画面保持上一帧：不空屏，也不为已经作废的位置解码
      }
    }
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.reader_reuse;  // 这一次是真的沿用现有流解到目标，补记
  } else if (e) {
    std::lock_guard<std::mutex> lk(mu_);
    ++stats_.reader_reuse;  // 静止帧沿用池里的流：原来这一步由 acquire_reader 记，现在流在前面就挑好了
  }
  if (!e) e = rewind_reader(pool, req, frame_dur, stamp, claimed);
  if (e) {
    claimed.push_back(e);
  } else {
    int why = 0;  // 与 Stats 的 why_* 一一对应
    bool any_vi = false, any_size = false, any_dead = false, any_ahead = false;
    for (const auto &en : pool) {
      if (en->vi != req.vi) continue;
      any_vi = true;
      if (en->w != req.w || en->h != req.h) continue;
      any_size = true;
      if (stream_dead(en->rd)) {
        any_dead = true;
        continue;
      }
      if (en->rd.position() < req.target - frame_dur * 0.5) {
        any_ahead = true;
      }
    }
    why = !any_vi ? 1 : !any_size ? 2 : any_ahead ? 3 : any_dead ? 5 : 4;
    {
      std::lock_guard<std::mutex> lk(mu_);
      switch (why) {
        case 1: ++stats_.why_src; break;
        case 2: ++stats_.why_size; break;
        case 3: ++stats_.why_back; break;
        case 4: ++stats_.why_lag; break;
        case 5: ++stats_.why_eof; break;
      }
    }
    e = open_reader(pool, req, stamp);
    claimed.push_back(e);
  }

  // 刚按目标开出来的流 index<0，游标就落在目标上，不必再判落后。
  // 这一格往后一律以 serve_at 为基准：级2/级3 交付的是同步点而不是作废的目标，拿目标来量lag
  // 会在刚付完一次定位之后立刻判成「落后 > 2s」，同一格再付第二次。
  if (e && e->rd.frame_time() >= 0.0) {
    double lag = serve_at - e->rd.frame_time();
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (lag > stats_.max_lag) stats_.max_lag = lag;
    }
    if (lag > kLagRestart) {
      // 落后这么多，顺解追不划算（1.4ms/帧 × 领先秒数×60 ≈ 两秒的账）。能在同一只流里
      // 定位就先定位：实测 seek 到手 70~844ms，重建整条流到出画 1.5 秒。
      // 子进程做不到，那条路照旧另开一条并记 why_lag。
      const double t0 = qpc_ms();
      if (!e->rd.seek(serve_at)) {
        {
          std::lock_guard<std::mutex> lk(mu_);
          ++stats_.why_lag;
        }
        // 落后太多：另开一条定位到目标的流，旧的留在池里等淘汰，不强行拆掉。
        e = open_reader(pool, req, stamp);
        claimed.push_back(e);
      } else {
        const double ms = qpc_ms() - t0;
        std::lock_guard<std::mutex> lk(mu_);
        ++stats_.in_place_seeks;
        if (ms > stats_.max_seek_ms) stats_.max_seek_ms = ms;
      }
    }
  }

  if (e && !stream_dead(e->rd)) {
    // 顺序解码追到落点，但只交付落点那一帧。逐帧发布的话追赶过程本身就是把画面往回放：
    // 循环回跳那一格要从片尾解回片头，用户看到的是画面在两帧之间跳，而不是「跳回片头」。
    // 追不上（超这一轮的 0.5 秒预算 / 流断了）就交付解到的最后一帧，不空屏。
    //
    // 「到位」问的是规则点名的那一只源帧（VideoInfo::src_frame，和导出 trim=start_frame 同一个
    // 所有者）。MF 那一路的钟在 MfStream 里已减掉它自己的原点（带 elst 的素材 MF 把第 0 只
    // stamp 在 2×media_time，画面却是完整的第 0 只），所以这里的 f->t 与导出同一把尺，
    // 不再需要为头部错位换引擎。
    const double wait_until = qpc_ms() + 500.0;
    bool have = false;
    Frame landed;  // 拷一份：缓存可能在这中间淘汰掉指针指向的那一帧
    for (int guard = 0; guard < 600; ++guard) {
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return;
      }
      const Frame *f = decode_step(req.vi, e->rd, sc);
      if (!f) {
        if (stream_dead(e->rd)) break;
        if (qpc_ms() > wait_until) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      landed = *f;
      have = true;
      if (f->t >= serve_at - frame_dur * 0.5) break;
    }
    if (have) show_frame(req.lane, landed, want_idx);
  }

  // 预取：交付位置之后再解几帧进缓存，让随后的前进与小幅后退都落在缓存上。
  // 上界必须以 serve_at 为基准（不是已解位置，也不是作废的目标）：以已解位置为基准会让流越跑越前，
  // 播放头反倒“后退”而频繁重建；以目标为基准则级3 那一跳要顺解整只 GOP —— 买跟手买成解全片。
  if (e && !stream_dead(e->rd)) {
    double ahead = serve_at + frame_dur * kAheadFrames;
    for (int guard = 0; guard < 12 && sc.bytes < kCacheBytes; ++guard) {
      if (e->rd.frame_time() >= ahead) break;
      if (!decode_step(req.vi, e->rd, sc)) break;
    }
  }
}

void Preview::worker_main() {
  std::vector<std::unique_ptr<ReaderEntry>> pool;
  std::map<const VideoInfo *, SourceCache> caches;
  uint64_t stamp = 0;

  for (;;) {
    std::vector<Request> reqs;
    {
      std::unique_lock<std::mutex> lk(mu_);
      if (wake_at_ < 0.0) {
        cv_.wait(lk, [&] { return quit_ || dirty_; });
      } else {
        // 有 lane 在等播放头停下来：最多睡到那一刻，到点自己醒来按当前的目标再走一轮。
        // 不能只等 dirty_：拖动停下后不再有新目标，也就没人会叫醒它。
        const double ms = wake_at_ - qpc_ms();
        cv_.wait_for(lk, std::chrono::milliseconds(ms > 0.0 ? long(ms) : 0),
                     [&] { return quit_ || dirty_; });
      }
      if (quit_) return;
      dirty_ = false;
      reqs = reqs_;
      wake_at_ = -1.0;
    }
    ++stamp;
    // 自下而上服务：一轮把快照里的每条 lane 都走一遍，中途不因「又来了新目标」而中断。
    // 播放/拖动时每 tick 都有新目标，一中断就永远停在第一条 lane 上，排在后面的那条一整轮
    // 都轮不到、一条帧也不发布 ⇒ 画面上只剩一个片段。Blender 的 seq_render_frame 同步解完
    // 整帧才返回，帧缓存从不半途作废，等价于「跑完这一轮」。
    std::vector<ReaderEntry *> claimed;
    for (const Request &req : reqs) {
      if (!req.active) continue;  // 播放头落到空隙：流与缓存都留着，回到原处不必重建
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (quit_) return;
      }
      service_lane(req, pool, caches, stamp, claimed);
    }
    // 隔了 kReaderStaleRounds 轮没被用到的流真的关掉。Blender 只把陈旧项当复用候选
    // （movie_reader_cache.cc:88 的 stale_after_timestamps），它的读取器是个内存句柄；我这里
    // 每一条是一个 ffmpeg 进程，留着就是白占着解码器和几百 MB 的读缓冲。
    int kills = 0;
    for (auto it = pool.begin(); it != pool.end();) {
      if (stamp - it->get()->stamp <= kReaderStaleRounds && !stream_dead(it->get()->rd)) {
        ++it;
        continue;
      }
      if (it->get()->rd.live()) it->get()->rd.stop();
      it = pool.erase(it);
      ++kills;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      stats_.readers_live = int(pool.size());
      stats_.stale_kills += kills;
    }
    evict(caches, reqs);
  }
}
