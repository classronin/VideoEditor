#pragma once
// ISO-BMFF（MP4/MOV）容器索引读取器：读 stss/stsc/stco/cts 得出同步点表 + 相邻段参数集一致性。
//
// 现在的唯一买家是 synctable.cpp（拖动取帧的同步点表）。「码流直通」按覆盖面已经关掉，
// 所以这里不再有诊断命令、不再有工作线程外壳，也不再有 trace 报告文案；判据①（每个同步点一次
// 设备读）仍然留在本文件里，由 `skip_rule1` 那一格决定建表那一趟跑不跑。
//
// 对账基线（当年和 JS 探子逐位数一样跑过的那套）已经没了，现在以 synctable 的
// 行为为准；改这里的算术前先拿 1.mp4 的 kf=/sp= 计数复测。
#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

namespace app {

// 回退原因枚举里**探针能给出的那几条**。编号与「回退原因 19 条」的约定一一对齐，
// 剩下那些（1 非关键帧对齐、3~12 条带状态、14 音频边界、17 跨源、18 字幕）不是容器层能判的，
// 等导出侧落地时在同一个 enum 里补，别在别处另起一张编号会漂移的表。
enum PtReason : int {
  kPtOk = 0,
  kPtOpenGop = 2,              // stss/KEY 说是同步点，载荷或时间序说不是 ⇒ open GOP，v1 不假装能验它的恢复帧
  kPtCodecMismatch = 15,       // 容器或 codec 不在 v1 范围（只支持 MP4/MOV + H.264 avc1/avc3）
  kPtSpsPpsInconsistent = 16,  // **相邻段（拼接边界）的带内 SPS/PPS 不是同一套** ⇒ 该拼接点回退。
                               // 粒度钉在「相邻段」，不是「单文件」：一只文件内部
                               // 参数集多变**只是报告级**（每个 IDR 自带一份 ⇒ 逐段自洽），单文件多变
                               // 不许占这个号，否则 1.mp4 会被误判成不可直通。
  kPtIndexNotImplemented = 19  // 容器索引未实现：非 MP4/MOV、解析失败、截断、表异常都落这一条
};

struct IndexReport {
  // ---- 判定 ----
  bool ok = false;      // 容器索引成立 ⇒ 下面这些数才可信
  int reason = kPtIndexNotImplemented;  // **文件级**回退原因；kPtOk = 没有回退。
  // 边界级回退原因单独一列（别和文件级混成一个号）：枚举 16 只出现在这一列，
  // 它挂的是「哪条拼接边界不安全」，不是「这只文件不能直通」⇒ 整片当一段直通时这一列不参与判定。
  int boundary_reason = kPtOk;
  // 直通依据是否充分。reason==0 **不等于**可以直通：判据没跑全（无 stss、ctts 异常、
  // 参数集被窗口截断、窗口里找不到条片）时 reason 也是 0，靠这一列区分。
  bool ready = false;
  std::wstring verdict;  // 一行人话，进 trace 最后一行和状态栏

  // ---- 输入 ----
  std::wstring path;
  uint64_t file_size = 0;
  bool unbuffered = false;   // FILE_FLAG_NO_BUFFERING：绕系统缓存 = 冷态**代理**，盘自身缓存仍在
  uint32_t sector = 0;       // 生效的对齐（卷的 bytes-per-sector，量不到时按 4096）
  uint64_t io_bytes = 0;     // 真的向设备要了多少字节（对齐补头补尾也计入）
  std::wstring top_boxes;    // 前若干个顶层 box，够定位「moov 在头还是在尾」

  // ---- 容器与表 ----
  std::wstring brand;        // ftyp 的 major brand（没有 ftyp 时为空，老 MOV 就是这样）
  std::wstring codec;        // stsd 第一个样本 entry 的四字符码，如 avc1
  std::wstring framing;      // "AVCC 4" / "Annex-B"：分帧动词，见 A1 的第一处修正
  int avcc_length_size = 0;  // 分帧动词的整数形式（0 = 起始码）：给懒验复用，判定不读它
  uint32_t timescale = 0;
  uint64_t samples = 0;      // stsz 的样本数
  uint64_t sync_count = 0;   // stss 条目数；无 stss ⇒ 每个样本都是同步点
  bool has_stss = false;
  double gop = 0.0;
  uint64_t stsc_entries = 0, chunk_entries = 0;
  bool co64 = false;
  uint64_t moov_pos = 0, moov_size = 0;
  bool moov_tail = false;    // moov 落在文件后半 ⇒ 非 faststart

  // ---- 成本（A2 第 3 条：CPU 必须 user + sys 两列都记）----
  double moov_read_ms = 0.0, index_ms = 0.0, rule1_ms = 0.0, rule2_ms = 0.0, total_ms = 0.0;
  // 逐段的 user+sys 合并值，用来和 JS 探子的分段价签对账；
  // 总账只认下面这一对（它包含上面所有段，逐段再加一次就是把同一笔算两遍）。
  double cpu_moov_ms = 0.0, cpu_index_ms = 0.0, cpu_rule1_ms = 0.0, cpu_rule2_ms = 0.0;
  double cpu_user_ms = 0.0, cpu_sys_ms = 0.0;

  // ---- 判据①：同步样本的第一个条片是不是 IDR ----
  size_t window_bytes = 0;
  uint64_t idr = 0, non_idr = 0, no_slice = 0, short_read = 0;
  uint64_t max_bytes_to_slice = 0, max_nalu_walk = 0;
  uint64_t nalu_sps = 0, nalu_pps = 0, nalu_sei = 0, nalu_aud = 0;

  // ---- SPS/PPS：**两个口径分开记**（单文件多变 = 报告级，相邻段不一致 = 回退级）----
  uint64_t sps_kinds = 0, pps_kinds = 0;  // 单文件里的「种」数 ⇒ 只报告，不占回退号
  // 0=没有带内参数集（只有 avcC 一份）1=单文件内一致 2=单文件多变（报告级）3=不可判（窗口截断）
  int sps_pps_state = 0;
  // 「直通段必须从同步点起 + 段头带 SPS/PPS」这条前置条件里「带参数集」的那一半：
  // 自带至少一条带内 SPS 或 PPS 的同步点数（对不上同步点总数 ⇒ 有的段头没参数集）。
  uint64_t sync_with_params = 0;
  // 相邻段口径（回退级）：比的是**相邻两个同步点各自的带内参数集**是不是同一套。
  // 不相等的每一处 = 一个「在这儿剪切就要回退」的拼接边界（枚举 16 归这一档，与上面的种数无关）。
  uint64_t boundary_pairs = 0, boundary_mismatch = 0, boundary_unknown = 0;
  std::vector<uint64_t> boundary_mismatch_at;  // 第 i 与 i+1 个同步点之间不一致（0-based，最多记 32 处）
  std::wstring boundary_example;               // 第一条不一致的人话样子，给 trace

  // ---- 判据②：同步点到下个同步点之间不许出现更早的 pts（前导帧 = open GOP）----
  uint64_t stts_entries = 0, ctts_entries = 0, ctts_nonzero = 0, ctts_covered = 0;
  uint64_t pts_violations = 0;
  std::wstring pts_example;
  bool rule2_comparable = false;  // stts/ctts 覆盖完整才谈得上「判过」

  // ---- 对账用：同步样编号（1-based）与它的字节偏移，下标一一对应 ----
  std::vector<uint32_t> sync_samples;
  std::vector<uint64_t> sync_offsets;
  // 同步点的 pts（秒），与 sync_samples **同下标**。只在判据② 可比（`rule2_comparable`）时填，
  // 否则留空 ⇒ 读的人用 `size()==sync_count` 判「有没有」，不另开一个标志位。
  // 这一列只加出口、不加判据：填它在判据② 那个**已经存在**的循环里顺手取，
  // 不新增遍历、不参与任何判定 ⇒ `reason`/`ready`/判据①② 的数与本列无关。
  // **原点如实写明（读的人不许猜）**：合成式是 `pts[s] = (解码序第 s 个样本的 DTS + ctts 偏移) / timescale`，
  // 其中 **DTS 从 0 起累加** ⇒ 这一列的原点是「**解码序第一个样本 = 0**」，不是「第一显示帧 = 0」，
  // 也没减容器 start_time、没读 elst。它和容器 pts 差一个常数 `−dts₁`（= 合成延迟）：
  // 带 B 帧的源实测差 0.08 s = 2 帧（五只源对 ffprobe 到 1e-6；
  // `btest.mp4` 用 bundled `ffmpeg -ss` 验：表里那只 kf 的 0.48 落到第 13 帧，容器 pts 0.40 才落到第 11 帧）。
  // ⇒ 谁拿它当「产品里那一秒」的定位目标，**必须先减掉这一列的最小值**（谓词含 `first_sync==1` 且
  // `reason==kPtOk` ⇒ `pts_violations==0` ⇒ 全样本最小值必落在同步点子集里 ⇒ `min(sync_pts)` 就是第一显示帧），
  // 原点减在哪一侧是单独一条裁，别在这一列的算术里顺手定。
  std::vector<double> sync_pts;

  // ---- 取消（worker 要能停、不崩、trace 记取消原因）----
  bool cancelled = false;
  std::wstring cancelled_at;  // 停在哪一段、第几格；连同已付的读盘/成本一起进 trace
};

// 跑一遍容器索引。unbuffered=true 走 FILE_FLAG_NO_BUFFERING（冷态代理），false 走正常缓存。
// 任何失败都返回一份带 reason 的报告，不抛、不崩、不返回半截可信数。
// cancel 非空时，每读一格同步样本检一次标志（原子读，比一次设备读便宜得多）；置了就走
// cancelled 分支返回，**不产出半截判定**。检查点只在有真活的循环里，不在热路径上加锁。
// skip_rule1=true（建表那一趟）：判据① 那个「每个同步点一次设备读」的循环整个不跑，
// 相应的那几列留初值、`ready` 自己说不成立（见函数里的注释）；容器解析、stss 换算、判据②
// 照旧 ⇒ `sync_pts`/`sync_samples`/`rule2_comparable`/`reason` 四样都还是真数。
IndexReport probe_container_index(const std::wstring &path, bool unbuffered = false,
                                  size_t window_bytes = 4096,
                                  const std::atomic<bool> *cancel = nullptr,
                                  bool skip_rule1 = false);

// 懒验的**单格**出口：只在某一个同步点的字节偏移上读一窗，走 NALU 数第一个条片。
// 走查用的是判据① 循环体原样提出来的那一份（walk_first_slice），不是重写的第二套算术。
// 一次 = 一次 CreateFile + 一窗读；调用方（预览线程）按「每只同步点验一次、挂过就整源停手」用。
// read_ok=false 表示打不开/读不到 ⇒ 懒验视为失败（调用方置 lazy_stopped），不当成「不是 IDR」。
struct IdrProbe {
  bool read_ok = false;
  int slice = 0;   // 0 = 窗口里没有条片 1 = 非 IDR 5 = IDR（与判据① 同一口径）
  size_t got = 0;  // 真读到的字节数
};
IdrProbe probe_sync_idr_at(const std::wstring &path, uint64_t offset, int length_size,
                           size_t window_bytes = 4096);

}  // namespace app
