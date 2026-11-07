#pragma once
// 会话级同步点表：一只源一份，记下每个同步点（IDR）落在哪一秒，供拖动播放头时查「最近的一个」。
//
// 这一层买的东西：拖动播放头时，落点解不出来（现有流走不到）今天只能「画面保持上一帧」
// （preview.cpp 的 deferred 那一格）。表给出**落点之前最近的那个同步点的秒数** ⇒ 定位到那里，解一帧
// 而不是四十多帧，画面跟着手走。买的是画面，不是速度。
//
// 三条边界（一条不加一条不减）：**只在 worker 建**、**每源一次**、**不碰盘上产物**
// （只读容器 moov，不写任何文件）、**不预扫全工程**（读路径登记，拖到哪只才算哪只）。
//
// 线程规矩只有四件：算只在工作线程；读路径只 peek、绝不补算、绝不阻塞。
// 建一次表 = 一次 moov 读 + 判据② 那一圈（实测 `1.mp4` cpu_pay ≈33 ms，常规素材 ≤1.6 ms），
// 压在界面线程上就是假死 ⇒ `sync_request` 只登记，算由本模块自持的一条一次性 detached worker 来做
// （不借别的层的锁、不借别的层的线程启动条件）。
//
// 谓词只有一条（实现里不许出现第二个版本）：
//   `ok && reason==kPtOk && rule2_comparable && (has_stss ? sync_count≥1 && first_sync==1 : 推定绿)`
// 建表这一路**永不读 `ready`** —— 它每条置 false 的分支都读判据① 的产出，而它不跑判据① ⇒ 恒真 = 因没查而真。
// 同一原因，`reason==kPtOk` 在这一层只覆盖判据② 那半（IDR 那一半没查）⇒ 缺的那格由
// **懒验**补：级 3 真要定位某个同步点之前，只对那一个样本读一次 4 KB 窗数首片类型。
#include <cstdint>
#include <string>
#include <vector>

#include "mp4index.h"

namespace app {

// 懒验的三态（trace 两态分开计数，不许并成一个绿）：没查过 / 查过是 IDR / 查过不是 IDR。
enum SyncIdrState : uint8_t { kSyncIdrUnknown = 0, kSyncIdrOk = 1, kSyncIdrBad = 2 };

struct SyncPoint {
  double pts = 0.0;    // 该同步点的秒数（**已减过原点**时 = 第一显示帧起算）
  int64_t frame = 0;   // CFR 网格上的显示帧号 = llround(pts*fps)；片尾有空洞那一类源它不保证
  uint32_t sample = 0;  // stss 里的样本号（1-based，解码序）
  uint64_t offset = 0;  // 该样本的字节偏移：懒验那一格要读的窗口起点
  uint32_t row = 0;     // 表内下标（pts 升序），懒验状态落回表里用它当键
};

struct SyncTable {
  bool computed = false;   // 这一源算过一次（成没成都算一次 ⇒ 每源一次的断言看这个）
  bool trusted = false;    // 判据① 谓词成立
  bool presumed = false;   // 无 stss 的「推定绿」独立身份（不并进可信绿，单独一个身份）
  bool origin_shifted = false;  // 只对绿源减过 min(pts)
  int reason = kPtIndexNotImplemented;
  double fps = 0.0;
  uint64_t samples = 0, sync_count = 0;
  uint64_t worst_gap = 0;   // = max(max_gap, tail_gap)，**最坏间隔，不取平均**
  double lag_s = 0.0;       // (worst_gap−1)/fps
  double share = 0.0;       // (worst_gap−1)/samples
  double dur_s = 0.0;       // samples/fps（档界 60 s 用的是这把尺，和 lag/share 同一分母）
  bool short_tier = false;  // dur_s < 60 s ⇒ 用 50 % 那一档
  bool kf_open = false;     // 表可信 && lag_s ≤ 5 s && share ≤ 该档那根线（两档各两条 AND）
  int length_size = 0;      // avcC 的长度前缀字节数（0 = Annex-B 起始码分帧）；懒验复用
  double cpu_pay_s = 0.0;   // 建这一张表真付的 CPU 秒（= cpu_total − cpu_rule1，判据① 没跑）
  bool lazy_stopped = false;  // 懒验挂过 ⇒ 本会话这一源不再懒验（不许逐格重试）
  std::wstring why;         // 回退/未减原点的人话原因，进 trace（不许静默）
  std::vector<double> pts;  // 升序，查表按它做 upper_bound
  std::vector<int64_t> frame;
  std::vector<uint32_t> sample;
  std::vector<uint64_t> offset;
  std::vector<uint8_t> idr;  // 每行的懒验态（SyncIdrState）
};

// ① 算：读容器索引（**跳过判据①**）→ 谓词 → 开门判据 → 建表。只许本模块的 worker 调。
// 失败也返回一份带 `computed` 与 `why` 的表（落进会话表，读路径下次不再重问）。
bool sync_compute(const std::wstring &path, double fps, SyncTable *out);

// ② 只查表：绝不算、绝不补算、绝不阻塞，任何线程可调（含预览线程）。没就绪返回 false。
// 热路径要的是 `sync_lookup`（只拷一个 SyncPoint，不拷整张表）。
bool sync_peek(const std::wstring &path, SyncTable *out);

// ③ 登记「这只源要表」：本会话每源登记一次，立刻返回，不等结果。fps 供开门判据换算秒数。
void sync_request(const std::wstring &path, double fps);

// ③ 的那一半：worker 来取登记。取走即不再重发（算过＝成没成都算一次）。
bool sync_next_request(std::wstring *path, double *fps);

// 拖动里那一跳的全部活：**查表 + 读一个 bool**，判定不在热路径。
// 表不在 / 不可信 / 门已关 / 落点在所有同步点之前 ⇒ false（调用方走它本来就有的回退分支）。
bool sync_lookup(const std::wstring &path, double target, SyncPoint *out);

// 懒验：级 3 付钱之前问某一个同步点验过没有，验完把结果落回表里。
// `sync_idr_state` 同时给出「这一源是否已停懒验」（挂过一次就不再逐格重试）。
bool sync_idr_state(const std::wstring &path, uint32_t row, uint8_t *state, bool *stopped);
void sync_set_idr(const std::wstring &path, uint32_t row, uint8_t state);
void sync_stop_lazy(const std::wstring &path);

// 级 3 付钱之前的那一句问话：这一只同步点是不是真的 IDR。true = 可以付（本格证过是 IDR）。
// false = 不付，走调用方本来就有的回退：证过不是 IDR、本源懒验已挂且这一格从没证过、表没了。
// 「读不到」和「窗口里没有条片」都不算「不是 IDR」：那一格判不成，只把本源懒验挂起来，并落一行
// trace 要两态分开计数，不许把「验出非 IDR」和「验不了」并成一个数。
// 这一句会读一次盘（一窗 4 KB）⇒ 只许在预览工作线程上调，不许在 UI 线程调。
bool sync_verify_idr(const std::wstring &path, const SyncPoint &sp);

// 有登记就起那条一次性线程（没有在跑的才起）。调用点 = `sync_request` 自己，读路径不阻塞。
void sync_worker_ensure();

// worker 算完一源会把一行账（档、lag、share、开/关、原点减没减、cpu_pay）排在这里，界面线程取走落
// trace —— trace_fp 是全局 FILE*，线程直接写会撞车（和 index 探针那条口径一致）。
bool sync_take_trace(std::wstring *out);

// 计数（trace 的 `sync=` 那一格 + 「每源一次」那条断言）：已建成的表数、其中开门的表数、
// 已登记但还没算完的源数。pending 单独一格是必需的：拖动刚开始时表必然还在路上，
// 没有它，`kf_notready` 到底是「这一次真的没赶上」还是「登记根本没生效」就分不出来。
void sync_counts(int *tables, int *open, int *pending);

}  // namespace app
