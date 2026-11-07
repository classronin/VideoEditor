#pragma once
// 预览核。机制对齐 Blender 5.4 的 source/blender/sequencer/intern/cache：
// - 解码器池按「素材路径 + 解码尺寸」建键，同素材可同时持有多条不同位置的流；只能前进的流
//   不会拿去解后面的位置（movie_reader_cache.cc:204-205 按距离打分，后退乘 32 罚分）。
//   Blender 的一个读取器只是一次 avformat open，我这里是一条 ffmpeg 子进程，代价高得多
//   （实测一次定位 0.15~0.2 秒挂钟、0.4~1.1 秒 CPU），所以容量取小、隔若干轮没用就硬关掉。
// - 拖动播放头期间不为每个位置开子进程：对齐 Blender 的 is_scrubbing 挂起
//   （prefetch.cc:561-564、690-702，标志来自 sequencer_preview_draw.cc:143 的 screen->scrubbing）。
//   Blender 挂起的是预取线程，我这里挂起的是「起进程」这件最贵的事。
// - 帧缓存按素材分开（对齐 source_image_cache 以读取器+帧号建键）：切回上一个素材时它的帧还在。
//   超预算时扔「离播放头最远」的那一帧，身后的距离罚 2 倍（final_image_cache.cc:267-276），
//   不是按先进先出扔——来回拖播放头要的是「刚才那一段还在」。
// 解码仍在工作线程进行，UI 线程只取回最新帧；重建的空窗期 UI 继续显示缓存帧。
#include "media.h"
#include "mfreader.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

class FrameReader {
 public:
  FrameReader() = default;
  ~FrameReader() { stop(); }
  FrameReader(const FrameReader &) = delete;
  FrameReader &operator=(const FrameReader &) = delete;

  void stop();
  // 从 seconds 开始，输出尺寸为 out_w*out_h。第 0 帧精确落在 seconds。
  // MF 那一路的时钟原点在 MfStream 内部归一（定位加、读回减），交回的 t 与导出/子进程同一把尺。
  bool start(const VideoInfo &vi, double seconds, int out_w, int out_h);
  bool next();  // 解析下一帧（会阻塞到管道有数据）
  // 同一只流里改位置。只有 MF 做得到（子进程只能另开一条，那是 restarts 的来源）。
  // 成功后游标落在 seconds，第一帧到手以前 frame_time()<0，与刚开出来的流同形。
  bool seek(double seconds);
  // 管道里是否已有可读数据。false 表示暂时没有（不代表结束）。
  bool data_ready();
  // 流是否已经到头：进程退出或管道读尽。
  bool ended() { return (!proc_ && !mf_) || eof_ || (proc_ && proc_->exited(nullptr)); }
  bool eof() const { return eof_; }
  bool live() const { return (proc_ || mf_) && !eof_; }
  bool uses_mf() const { return mf_ != nullptr; }
  // 这一路是不是「MF 开流成功、第一帧解不出，已就地换成子进程」——计数该从 mf_open 挪到 mf_refused。
  bool take_mf_gave_up() {
    bool v = mf_gave_up_;
    mf_gave_up_ = false;
    return v;
  }
  // 一帧的字节（BGRA）。take_ 把所有权交出去，缓存拿到就不再搬：一帧 4.1MB，
  // 上一版每发布一帧要拷三次整幅，全花在把对的字节再放一遍上。
  std::vector<uint8_t> take_bgra() { return std::move(bgra_); }
  int w() const { return w_; }
  int h() const { return h_; }
  double t0() const { return t0_; }
  double fps() const { return fps_; }
  int64_t index() const { return index_; }  // 相对 t0 的帧序
  // MF 那一路报的是样本自己的时间（SetCurrentPosition 落在目标之后半帧以内，不按 t0 硬算）；
  // 子进程那一路的帧 0 精确落在 t0，用帧序推更稳。
  double frame_time() const {
    return index_ < 0 ? -1.0 : (mf_ ? t_mf_ : t0_ + double(index_) / fps_);
  }
  // 流的“游标”：还没解出帧时它就是 t0，否则是已解到的那一帧。池内挑选看的是这个。
  double position() const { return index_ < 0 ? t0_ : frame_time(); }

 private:
  std::unique_ptr<FrameProc> proc_;
  std::unique_ptr<MfStream> mf_;  // 非空 = 这一路走 MF 硬解（默认；VSE_MF=0 或系统解码器解不了就没有）
  const VideoInfo *vi_ = nullptr;  // 开流用的素材：MF 解不出时要按同一位置换子进程重开
  std::wstring path_;
  bool mf_gave_up_ = false;  // 这一次换路过（计数要从 mf_open 挪到 mf_refused），取一次清一次
  double t_mf_ = -1.0;
  std::vector<uint8_t> accum_;
  std::vector<uint8_t> bgra_;
  int w_ = 0, h_ = 0;
  int64_t index_ = -1;
  double t0_ = 0.0;
  double fps_ = 25.0;
  bool eof_ = false;
};

// 预览目标与当前画面。request/pump 都在 UI 线程调用，两者都不阻塞。
// 一次 request 给的是「播放头下所有参与合成的条带」，每条一路（lane，用条带 id 建键）：
// Blender 每帧也是把 query_rendered_strips 的全部条带逐条解码再叠加（render.cc 的
// seq_render_frame），不是只取最上面那一条。
class Preview {
 public:
  Preview() = default;
  ~Preview() { shutdown(); }
  Preview(const Preview &) = delete;
  Preview &operator=(const Preview &) = delete;

  struct Target {
    int64_t lane = 0;             // 条带 id：同素材的两条带位置不同，必须分开
    const VideoInfo *vi = nullptr;
    double source_seconds = 0.0;
    int w = 0, h = 0;
  };

  struct Stats {
    int restarts = 0;
    int why_size = 0, why_src = 0, why_back = 0, why_lag = 0, why_eof = 0;
    int cache_hits = 0, decoded = 0;
    int reader_reuse = 0;          // 池内命中：直接沿用已有流，没开新子进程
    int readers_live = 0;          // 最近一次请求时池里还活着的流数
    int deferred = 0;              // 拖动中按住的「另开流」：等播放头停下来再做
    int stale_kills = 0;           // 隔若干轮没用到而结束的流
    // MF 硬解这条路走到哪一步，只从 decoded/pool 分不出来：ffmpeg 兜底时这些数一模一样。
    // open = 建流走了 MF；refused = 试了 MF 但系统解码器解不了这路编码（开流被拒，或开流成功、
    // 第一帧解不出来后换回子进程）；dec = MF 解出的帧数。
    int mf_open = 0, mf_refused = 0, mf_decoded = 0;
    // 「同一只流里定位」用了几次、最坏一次多久。它替代的正是 restarts 里的整条流重建
    // （实测重建到出画 1.5 秒），所以 restarts 掉下来时要在这一行核对钱是真省了。
    int in_place_seeks = 0;
    double max_seek_ms = 0.0;
    double spawn_ms = 0.0, max_spawn_ms = 0.0;
    double first_ms = 0.0, max_first_ms = 0.0;  // 重建流后等到第一帧的时长
    int pumps = 0;
    double pump_ms = 0.0, max_pump_ms = 0.0;  // UI 线程取帧耗时
    double max_lag = 0.0;                     // 目标领先已解码位置的最大秒数
    // 拖动落点的三条级 + 四条「这一格为什么没给付/付不成」，七格一条都不合并：
    // 合并了就只能数出「用了几次关键帧」，数不出「钱花在哪一级、以及为什么这一跳没花」。
    int kf_free = 0;      // 级1 免费：那只同步点自己的画面已经在帧缓存里
    int kf_walk = 0;      // 级2 便宜：池里已有的流顺解得到那只同步点，不定位
    int kf_paid = 0;      // 级3 付钱：给已有的流定位到同步点秒数（一次拖动内同一只由级1/级2 接住）
    int kf_notready = 0;  // 表还没建好：已登记，这一跳照旧按住（不许和「门已关」并成一个数）
    int kf_gated = 0;     // 门关 / 不可信 / 懒验不给这一格付钱：原因在 sync= 与懒验那两行 trace 里
    // 表说可以付、懒验也证过是 IDR，但同一只流就是定位不过去（MF 拒 seek）。这一格必须单独数：
    // 把它记进 kf_gated 会说谎 —— 门关是「这源不配」，这是「这源配但这一次没做到」，两种修法不同。
    int kf_seekfail = 0;
    // 表给付这一格，但池里没有拿得出来的流（从没为这个请求尺寸开过 / 已经死了）。第一段不许为
    // 拖动中的跳位开流 ⇒ 这一格只能照旧保持上一帧。单独数：它既不是「门关」（源不配）也不是
    // 「流拒了定位」（那两格各有修法），把它记进 kf_gated 会把「该不该付」读成「有没有钱付」。
    // 门关放行之后这一格还多收一种：门关、但本道连一只可用的流都没有（或只有做不到的子进程流）。
    // 两处共用是故意的 —— 修法同一条（这一道当时没有可用的流），要分开的是「放行」与「池空 defer」。
    int kf_nostream = 0;
    // 门关放行：门已关（表可信但开门判据不过）这一支里
    // **本道有可用流 ⇒ 不 defer**，把这一格交给既有下游那一步（落后 >2s 就地定位，否则顺解）交付。
    // 单独一格是硬要求：它和「门关 · 池空 · 保持上一帧」（`kf_nostream`）
    // 是两种结果，合进 `kf_gated` 就读不出这条路到底接住了几格。
    // 名字写 serve 不写 seek：放行的格里多数是顺解走到鼠标那一格，只有第一拍和反向拍真付一次定位。
    int gc_serve = 0;
  };

  // UI 线程：更新目标列表（按合成顺序，自下而上），立刻返回。
  void request(const std::vector<Target> &targets);
  // UI 线程：取回工作线程发布的最新帧。返回 true 表示至少一路是新的（对应 region 脏标记）。
  bool pump();
  const uint8_t *frame(int64_t lane, int *w, int *h) const;
  // 已显示帧的标识：与 frame() 配套，同一 token 即同一内容，纹理无需重传。
  uint64_t frame_token(int64_t lane) const;
  bool has_frame() const { return have_frame_; }
  void reset();     // 播放头落到空隙：停流，保留缓存与线程
  void shutdown();  // 结束工作线程
  Stats stats() const;

 private:
  static constexpr double kAheadFrames = 8.0;  // 目标之后预取的帧数
  static constexpr double kBehindSec = 1.0;    // 保留窗口身后留量（来回拖时刚看过那段还在）
  static constexpr double kLagRestart = 2.0;   // 领先这么多秒就另开一条流，不逐帧硬解
  // 下面两条只是外层安全上限；实际留多少由 evict 的保留窗口（同源 active 请求的跨度）决定。
  static constexpr size_t kCacheBytes = 2048u << 20;
  static constexpr size_t kMaxFramesPerSource = 512;  // 单素材帧数安全上限（跨度超它才让位）
  static constexpr size_t kMaxReaders = 6;            // Blender 取 16：它的读取器不是子进程
  // 拖动判定：一个 tick 跳这么多帧以上算「跳位」而不是播放那样半帧半帧地前进。
  static constexpr double kJumpFrames = 2.0;
  // 连续跳位之间隔这么久以内还是同一次拖动；停这么久就认为是「用户落在这一格」，立刻定位。
  static constexpr double kMoveGapMs = 250.0;
  static constexpr double kSettleMs = 120.0;
  // 一条流隔了这么多轮没被用到就关掉。轮数照 Blender 的 stale_after_timestamps=8
  // （movie_reader_cache.cc:88），但它只把陈旧项当候选，我这里必须真的结束进程。
  static constexpr uint64_t kReaderStaleRounds = 8;

  // 一帧画面。字节用 shared_ptr：缓存、发布、UI 三方看的是同一块 8.3MB，谁都不再搬它——
  // 上一版每发布一帧要拷三次整幅（1.5GB/秒的 memcpy 全花在把对的字节再放一遍上）。
  // 淘汰/换帧只是解引用计数，被 UI 拿着的那一帧不会因为缓存扔掉而消失。
  struct Frame {
    const VideoInfo *vi = nullptr;  // 源标识：同一 t 在不同素材上是不同画面
    double t = 0.0;
    int w = 0, h = 0;
    std::shared_ptr<const std::vector<uint8_t>> bgra;
  };
  struct SourceCache {
    std::deque<Frame> frames;
    size_t bytes = 0;
  };
  struct ReaderEntry {
    FrameReader rd;
    const VideoInfo *vi = nullptr;
    int w = 0, h = 0;
    // 最近服务的那条 lane。同素材的两条带源位置不同，一条顺序流没法同时朝两个位置走，
    // 所以读取器按 lane 独占（Blender 那边每条 Strip 自带自己的 MovieReader）。
    int64_t owner = 0;
    uint64_t stamp = 0;  // 最近一次被使用的请求序号
  };
  struct Request {
    int64_t lane = 0;
    const VideoInfo *vi = nullptr;
    double target = 0.0;
    int w = 0, h = 0;
    bool active = false;
  };
  // 一条 lane 的当前画面。gen 每换一代 +1，UI 只重拷变过的那几路。
  struct Shown {
    Frame frame;
    uint64_t gen = 0;
  };

  void worker_main();
  // 一轮里服务一条 lane：挑/开读取器 → 解到目标 → 预取。
  void service_lane(const Request &req, std::vector<std::unique_ptr<ReaderEntry>> &pool,
                    std::map<const VideoInfo *, SourceCache> &caches, uint64_t stamp,
                    std::vector<ReaderEntry *> &claimed);
  // 池内挑选：同素材同尺寸、仍活着、本轮没被别的 lane 占用、且能顺序前进到目标的读取器里
  // 取距离最小的，同 owner 者优先；没有返回 nullptr。count_reuse=false 用于「先看看有没有」
  // 的探查（拖动中按住不做的那次），只续期不记数，免得把没干活的查找算成省下的启动。
  ReaderEntry *acquire_reader(std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req,
                              double frame_dur, uint64_t stamp,
                              const std::vector<ReaderEntry *> &claimed, bool count_reuse = true);
  // 新开了条流（含容量淘汰）。Blender 里对应新建 MovieReaderCacheEntry。
  ReaderEntry *open_reader(std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req,
                           uint64_t stamp);
  // 播放头退回这条 lane 自己的流已经走过的地方：能在同一只 reader 里定位就定位，
  // 不另开一条。只有 MF 走这条路（子进程做不到），做不到返回 nullptr 交给 open_reader。
  ReaderEntry *rewind_reader(std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req,
                             double frame_dur, uint64_t stamp,
                             const std::vector<ReaderEntry *> &claimed);
  // 级2/级3 的第二次问法。上面那两次问的是「顺解走不走得到」，
  // 位置条件写在筛选里，向后拖时永远借不到；这一句只问「这条 lane 有没有活的 MF 流可借去就地
  // 定位」，**不含任何位置条件**（借来就是为了往回走）。借不到不许开流；也不续期（续期发生在真用过它的那一格）。
  ReaderEntry *borrow_reader(std::vector<std::unique_ptr<ReaderEntry>> &pool, const Request &req,
                             const std::vector<ReaderEntry *> &claimed);
  // 解一帧进缓存，返回该缓存项；失败（结束/断开）返回 nullptr。
  const Frame *decode_step(const VideoInfo *vi, FrameReader &rd, SourceCache &sc);
  void spawn(FrameReader &rd, const Request &req);
  // 预算淘汰：参照点是这一轮各素材被请求的位置（没有请求的素材不动）。
  void evict(std::map<const VideoInfo *, SourceCache> &caches, const std::vector<Request> &reqs);
  // 发布某条 lane 的画面。同源同位置时不产生新一代（否则 UI 每 tick 都被标脏）。
  void show_frame(int64_t lane, const Frame &f, int64_t want);
  static const Frame *cache_lookup(const SourceCache &sc, double target, double frame_dur);

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  std::vector<Request> reqs_;
  // 每条 lane 最近一次「跳位」的时刻与连续次数（μs 级都用 qpc 毫秒）。连续两次以上才算拖动：
  // 单独一次是点时间线，那种该立刻定位，不该等。
  struct LaneJump {
    double at = -1.0;
    int run = 0;
  };
  std::map<int64_t, LaneJump> jump_;
  double wake_at_ = -1.0;  // 仅工作线程读写：最早能在这一刻为等待停下的 lane 开流
  bool dirty_ = false;
  bool quit_ = false;
  // worker -> UI
  std::map<int64_t, Shown> pub_;
  // 全局代号：每发布一帧 +1，跨 lane 唯一。纹理缓存拿它当「同一 token 即同一内容」的键，
  // 按 lane 各自计数会让两条带撞上同一个号，第二条就被当成第一条的画面。
  uint64_t pub_gen_ = 0;
  Stats stats_;
  double worker_spawn_at_ = -1.0;  // 仅工作线程读写
  // UI 私有：pub_ 的副本，绘制代码只读它，不必拿锁
  std::map<int64_t, Shown> shown_;
  bool have_frame_ = false;
};
