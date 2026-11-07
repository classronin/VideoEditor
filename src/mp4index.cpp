// 索引探针 v0：ISO-BMFF（MP4/MOV）容器自读 → 关键帧索引 + A1 两条恢复点判据 + SPS/PPS 一致性。
// 范围、不做的事、对账义务都写在 mp4index.h 顶部；这里只补实现级的两处坑（都是 A1 的修正）：
//
// 一、**MP4/MOV 的载荷是 AVCC，不是 Annex-B**。NALU 靠 `avcC` 的 lengthSizeMinusOne+1 个长度
//     字节分帧；本机这几只 MP4（1.mp4 / big1g / gopcheck 那对 / att_crf0 / sq）一个起始码都没有，
//     JS 探子第一趟按起始码扫 200KB 全空。所以分帧有两个动词：avcC 在 ⇒ 走长度；avcC 不在 ⇒ 才走
//     起始码（TS 和直通输出侧的裸流才是起始码那一侧）。判据本身不变。
//
// 二、**`stss` 只说「sync sample」，不说「恢复点」**。open-GOP 的 MP4 会把非恢复点也标进 stss
//     （gopcheck/open_gop.mp4 实测：stss 4 条里只有 1 条第一片真是 IDR）。所以 v0 两条并列：
//       ①载荷 = 同步样本里跳过 SPS/PPS/SEI/AUD 后第一个条片是不是 H.264 type 5；
//       ②时间序 = 该同步样本到下个同步样本之间，任何样本的 pts 不许比它自己早（早了=前导帧）。
//     ②只用 stts+ctts 算，不解码、不额外读盘。实测在 open_gop 这只上 ① 抓 3/4、② 抓 2/4，
//     且 ② 抓到的 ① 都抓到了 ⇒ ② 在这只夹具上**没有增量**，留它是为了「同步点标成真 IDR、
//     后面挂前导帧」那一类（本机造不出，正向用例仍缺）。
//
// 性能约束一条（踩过）：**stsc 必须先建前缀表**。逐样本重扫 stsc 是 O(样本数×条目数)，
// 1.mp4 的 stsc 有 317,475 条目，JS 探子第一版因此在它上面花掉 10.75 CPU 秒 —— 那是探子的错，
// 不是容器的错；改成一次前缀和 + 二分后是 0.032 CPU 秒。别再把它写回去。
#include "mp4index.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <new>
#include <thread>
#include <utility>

namespace app {
namespace {

// ---------------------------------------------------------------- 小工具

double qpc_now() {
  LARGE_INTEGER f, t;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) / double(f.QuadPart);
}

// 进程 CPU 时间（100ns 计）。user 和 sys 分开留：A2 第 3 条要求两列都记，
// 杀软那部分落在 sys，只报 user 等于把成本藏进对手看不见的地方。
struct CpuMark {
  uint64_t user = 0, sys = 0;
  static CpuMark sample() {
    CpuMark m;
    FILETIME c, e, u, k;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &u, &k)) return m;
    m.user = (uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime;
    m.sys = (uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime;
    return m;
  }
};
double cpu_ms(const CpuMark &a, const CpuMark &b) {
  return double(b.user >= a.user ? b.user - a.user : 0) / 10000.0;
}
double cpu_sys_ms(const CpuMark &a, const CpuMark &b) {
  return double(b.sys >= a.sys ? b.sys - a.sys : 0) / 10000.0;
}
double cpu_both_ms(const CpuMark &a, const CpuMark &b) {
  return cpu_ms(a, b) + cpu_sys_ms(a, b);
}

inline uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

uint64_t fnv1a64(const uint8_t *p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

// 四字符码转可读串：不可打印字节换成 '?'，免得坏文件把控制字符写进 trace。
std::wstring fourcc(const uint8_t *p) {
  wchar_t s[5];
  for (int i = 0; i < 4; i++) s[i] = (p[i] < 0x20 || p[i] > 0x7e) ? L'?' : wchar_t(p[i]);
  s[4] = 0;
  return std::wstring(s);
}

// MinGW 的宽格式串里宽字符串必须写 %ls（写 %s 只剩第一个字符，ops.cpp 里同一条注释）；
// 整数一律升到 unsigned long long 配 %llu：本机实测 %zu 也数得对（仿真层开/关两臂各数过一遍），
// 但那份「也数得对」只在这台机器的 CRT 上证过 ⇒ 关键读数不赌第二台。
std::wstring wf(const wchar_t *fmt, ...) {
  wchar_t buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vswprintf(buf, 1024, fmt, ap);
  va_end(ap);
  if (n < 0) return std::wstring(L"（这一行的数太多，格式缓冲溢出）");
  return std::wstring(buf, size_t(n));
}
#define ULL(v) (unsigned long long)(v)

// ---------------------------------------------------------------- 只读句柄

// unbuffered=true ⇒ FILE_FLAG_NO_BUFFERING：读绕开系统缓存，每次都真的下设备。
// 它是**冷态代理**，不是真空态 —— 盘自己的缓存还在，报数时必须带上这句话。
// 用它而不是写一份没读过的副本：为了量 0.16 秒的账去擦 11GB，SSD 那笔是要付的。
class Reader {
 public:
  ~Reader() { close(); }
  void close() {
    if (scratch_) VirtualFree(scratch_, 0, MEM_RELEASE);
    scratch_ = nullptr;
    scratch_cap_ = 0;
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }
  bool open(const std::wstring &path, bool unbuffered) {
    unbuffered_ = unbuffered;
    const DWORD flags = unbuffered ? FILE_FLAG_NO_BUFFERING : FILE_ATTRIBUTE_NORMAL;
    h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                     OPEN_EXISTING, flags, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) {
      last_err_ = GetLastError();
      return false;
    }
    LARGE_INTEGER sz;
    sz.QuadPart = 0;
    if (!GetFileSizeEx(h_, &sz) || sz.QuadPart < 8) {
      last_err_ = GetLastError();
      close();
      return false;
    }
    size_ = uint64_t(sz.QuadPart);
    align_ = 4096;  // 量不到卷信息时的保底对齐：一定是合法对齐，代价是最多多读一点（io_bytes 记真实的）
    if (unbuffered) {
      wchar_t vol[MAX_PATH] = L"";
      // 出参顺序：**第 1 个是每簇扇区数，第 2 个才是每扇区字节数**。取错的那版把簇
      // 形状当扇区大小（这台机器 = 8）⇒ 被下面 >=512 挡回 4096 保底，不炸，但每次窗读按 4096
      // 而不是真实的 512 补齐 ⇒ 冷态多读。512 是 4096 的约数，改完只会少读，不会引入新失败。
      DWORD sectors_per_cluster = 0, bytes_per_sector = 0, free_clusters = 0, total_clusters = 0;
      if (GetVolumePathNameW(path.c_str(), vol, MAX_PATH) &&
          GetDiskFreeSpaceW(vol, &sectors_per_cluster, &bytes_per_sector, &free_clusters,
                            &total_clusters) &&
          bytes_per_sector >= 512 && bytes_per_sector <= 65536 &&
          (bytes_per_sector & (bytes_per_sector - 1)) == 0)
        align_ = bytes_per_sector;
    }
    return true;
  }
  uint64_t size() const { return size_; }
  uint32_t align() const { return align_; }
  bool unbuffered() const { return unbuffered_; }
  DWORD last_err() const { return last_err_; }
  uint64_t io_bytes() const { return io_bytes_; }

  // 返回一个指向内部暂存的指针，长度写进 *got；下一次调用使上一次的指针失效。
  // 对齐模式的补头补尾都在这里处理，*got 已经剪回请求的范围。
  const uint8_t *window(uint64_t pos, size_t len, size_t *got) {
    *got = 0;
    if (pos >= size_ || len == 0) return nullptr;
    const uint64_t a = unbuffered_ ? (pos & ~uint64_t(align_ - 1)) : pos;
    const size_t head = size_t(pos - a);
    size_t want = head + len;
    if (unbuffered_) want = (want + align_ - 1) & ~size_t(align_ - 1);
    if (!ensure(want)) return nullptr;
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.Offset = DWORD(a & 0xffffffffu);
    ov.OffsetHigh = DWORD(a >> 32);
    DWORD nr = 0;
    if (!ReadFile(h_, scratch_, DWORD(want), &nr, &ov)) {
      // 越过文件尾的对齐读在 NO_BUFFERING 下可能整笔失败 ⇒ 缩到「一个整扇区都不越」再试一次。
      const uint64_t fits = size_ > a ? ((size_ - a) & ~uint64_t(align_ - 1)) : 0;
      nr = 0;
      if (fits == 0 || !ensure(size_t(fits)) || !ReadFile(h_, scratch_, DWORD(fits), &nr, &ov))
        return nullptr;
    }
    io_bytes_ += nr;
    const size_t avail = nr > head ? size_t(nr - head) : 0;
    *got = avail < len ? avail : len;
    return scratch_ + head;
  }

  // 整块读进 dst（dst 至少 len 字节）。返回实际读到的字节数。
  size_t read(uint64_t pos, uint8_t *dst, size_t len) {
    size_t got = 0;
    const uint8_t *p = window(pos, len, &got);
    if (p && got) memcpy(dst, p, got);
    return got;
  }

 private:
  bool ensure(size_t cap) {
    if (cap <= scratch_cap_) return true;
    const size_t want = (cap + (1u << 20) - 1) & ~size_t((1u << 20) - 1);
    if (scratch_) VirtualFree(scratch_, 0, MEM_RELEASE);
    scratch_ = nullptr;
    scratch_cap_ = 0;
    scratch_ = (uint8_t *)VirtualAlloc(nullptr, want, MEM_COMMIT, PAGE_READWRITE);
    if (scratch_) scratch_cap_ = want;
    return scratch_ != nullptr;
  }
  HANDLE h_ = INVALID_HANDLE_VALUE;
  uint8_t *scratch_ = nullptr;
  size_t scratch_cap_ = 0;
  uint64_t size_ = 0, io_bytes_ = 0;
  uint32_t align_ = 4096;
  bool unbuffered_ = false;
  DWORD last_err_ = 0;
};

// ---------------------------------------------------------------- box 读取

struct Range {
  uint64_t off = 0, end = 0;  // 内容区间（不含 box 头）
};

// ISO-BMFF 头：size(4) + type(4)；size==1 ⇒ 后面跟 8 字节 largesize（头共 16）。
// 越界、非正、size 小于头 一律在这一层停住，不猜下一个 box（坏文件不许崩）。
bool box_at(const uint8_t *b, uint64_t p, uint64_t end, uint64_t *size, uint64_t *hdr,
            const uint8_t **type) {
  if (p + 8 > end) return false;
  uint64_t sz = be32(b + p);
  uint64_t h = 8;
  if (sz == 1) {
    if (p + 16 > end) return false;
    sz = be64(b + p + 8);
    h = 16;
  }
  if (sz < 8 || p + sz > end) return false;
  *size = sz;
  *hdr = h;
  *type = b + p + 4;
  return true;
}

bool box_find(const uint8_t *b, Range scope, const char *type, Range *out) {
  uint64_t p = scope.off;
  for (;;) {
    uint64_t sz, hdr;
    const uint8_t *t;
    if (!box_at(b, p, scope.end, &sz, &hdr, &t)) return false;
    if (memcmp(t, type, 4) == 0) {
      out->off = p + hdr;
      out->end = p + sz;
      return true;
    }
    p += sz;
  }
}

void box_find_all(const uint8_t *b, Range scope, const char *type, std::vector<Range> *out) {
  uint64_t p = scope.off;
  for (;;) {
    uint64_t sz, hdr;
    const uint8_t *t;
    if (!box_at(b, p, scope.end, &sz, &hdr, &t)) return;
    if (memcmp(t, type, 4) == 0) out->push_back(Range{p + hdr, p + sz});
    p += sz;
  }
}

// 沿着一串嵌套 box 走下去（trak→mdia→hdlr 这种）。任一层缺就返回 false。
bool box_descend(const uint8_t *b, Range root, std::initializer_list<const char *> chain,
                 Range *out) {
  Range cur = root;
  for (const char *t : chain) {
    Range nx;
    if (!box_find(b, cur, t, &nx)) return false;
    cur = nx;
  }
  *out = cur;
  return true;
}

// stsz 的样本大小（uniform 时不查表）。越界返回 0：调用方的表一致性已经先验过，这里只兜底。
struct Sizes {
  const uint8_t *b = nullptr;
  Range box{};
  uint32_t uniform = 0;
  uint64_t count = 0;
  uint64_t at(uint64_t i) const {
    if (uniform) return uniform;
    if (i >= count || box.off + 16 + i * 4 > box.end) return 0;
    return be32(b + box.off + 12 + i * 4);
  }
};

// 一个同步样本的载荷走查结果。
struct SliceWalk {
  int slice = 0;        // 0 = 窗口里没有条片 NALU；1 = 非 IDR；5 = IDR
  size_t bytes_to = 0;  // 到第一个条片头的字节数（含长度前缀/起始码）
  size_t walked = 0;    // 一共走了几条 NALU
};

struct NaluTally {
  uint64_t sps = 0, pps = 0, sei = 0, aud = 0;
  bool truncated = false;  // 有参数集没落在窗口里 ⇒ SPS/PPS 一致性不可判
  std::vector<uint64_t> sps_hash, pps_hash;
};

// 一只样本的带内参数集**折成一个键**（排序去重后 FNV）。
// 相邻段比较用的就是这个键：两只相邻同步点的键不同 ⇒ 它们的段头参数集不同 ⇒
// 剪切落在那条边界上时该拼接回退（枚举 16）。键相等则不管全文件里有几种参数集。
uint64_t param_set_key(std::vector<uint64_t> *v) {
  std::sort(v->begin(), v->end());
  v->erase(std::unique(v->begin(), v->end()), v->end());
  uint64_t h = 1469598103934665603ull;
  for (const uint64_t x : *v) {
    h ^= x;
    h *= 1099511628211ull;
  }
  return h;
}

uint64_t sample_param_key(NaluTally *t) {
  // SPS 与 PPS 分开折，再按类型混起来：只换 PPS、不换 SPS 的那些边界也要判成不同。
  const uint64_t a = param_set_key(&t->sps_hash);
  const uint64_t b = param_set_key(&t->pps_hash);
  uint64_t h = 1469598103934665603ull;
  for (const uint64_t x : {a, b + 0x9e3779b97f4a7c15ull}) {
    h ^= x;
    h *= 1099511628211ull;
  }
  return h;
}

// 记一条 NALU 的类型。have_len=false（Annex-B 分支）时长度未知：参数集照样计数，
// 但一致性标成不可判 —— 拿一段猜长度的字节去比哈希，比出来的一致性是假的。
void note_nalu(uint8_t type, const uint8_t *payload, size_t avail, uint64_t len, bool have_len,
               SliceWalk *w, NaluTally *tally) {
  w->walked++;
  switch (type) {
    case 1:
    case 5:
      break;
    case 6:
      tally->sei++;
      break;
    case 9:
      tally->aud++;
      break;
    case 7:
    case 8: {
      const bool fits = have_len && len <= avail;
      if (!fits) tally->truncated = true;
      const uint64_t h = fits ? fnv1a64(payload, size_t(len)) : 0;
      if (type == 7) {
        tally->sps++;
        if (fits) tally->sps_hash.push_back(h);
      } else {
        tally->pps++;
        if (fits) tally->pps_hash.push_back(h);
      }
      break;
    }
    default:
      break;
  }
}

// 一只同步样本的载荷走查：从窗口开头逐条 NALU 数到第一个条片（含条片自己）。
// 判据① 的循环体原来是就地写的，现在原样提成这一份 —— 懒验（只验某一个同步点的第一片
// 类型）必须和全量判据用**同一个动词**，否则「懒验过了」和「判据① 过了」是两件事。
// 循环条件、长度读法、note_nalu 的入参、遇到条片就 break —— 一字未改。
void walk_first_slice(const uint8_t *p, size_t n, int length_size, SliceWalk *w, NaluTally *one) {
  uint64_t j = 0;
  if (length_size) {
    while (j + uint64_t(length_size) + 1 <= n) {
      uint64_t l = 0;
      if (length_size == 4)
        l = be32(p + j);
      else if (length_size == 3)
        l = (uint64_t(p[j]) << 16) | (uint64_t(p[j + 1]) << 8) | p[j + 2];
      else if (length_size == 2)
        l = be16(p + j);
      else
        l = p[j];
      const uint8_t t = uint8_t(p[j + length_size] & 0x1f);
      const size_t avail = n - size_t(j + length_size);
      note_nalu(t, p + j + length_size, avail, l, true, w, one);
      if (t == 1 || t == 5) {
        w->slice = t;
        w->bytes_to = size_t(j + length_size + 1);
        break;
      }
      if (l == 0 || j + length_size + l > n) break;  // 下一条出了这一窗
      j += length_size + l;
    }
  } else {
    while (j + 4 < n) {
      if (p[j] == 0 && p[j + 1] == 0 && (p[j + 2] == 1 || (p[j + 2] == 0 && p[j + 3] == 1))) {
        const size_t sc = p[j + 2] == 1 ? 3 : 4;
        const uint8_t t = uint8_t(p[j + sc] & 0x1f);
        const size_t avail = n - size_t(j) - sc;
        note_nalu(t, p + j + sc, avail, 0, false, w, one);  // 起始码侧没有长度
        if (t == 1 || t == 5) {
          w->slice = t;
          w->bytes_to = size_t(j + sc + 1);
          break;
        }
        j += sc + 1;
      } else {
        j++;
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------- 主入口

IndexReport probe_container_index(const std::wstring &path, bool unbuffered, size_t window_bytes,
                                  const std::atomic<bool> *cancel, bool skip_rule1) {
  IndexReport r;
  r.path = path;
  r.window_bytes = window_bytes ? window_bytes : 4096;

  const double t_all = qpc_now();
  const CpuMark cpu_all0 = CpuMark::sample();

  Reader rd;
  if (!rd.open(path, unbuffered)) {
    r.verdict = wf(L"打不开（GetLastError=%lu）⇒ 枚举 19", rd.last_err());
    return r;
  }
  // 取消标志每检一次是一次 relaxed 原子读（几纳秒），比紧随其后的一次设备读便宜三个数量级，
  // 所以只放在「真的干活」的循环入口：顶层 box 走一遍、判据①每格同步样本。
  // 检到就立刻返回，**不产出半截判定**（r.cancelled 的报告不参与任何对账口径）。
  auto wants_stop = [&]() {
    if (cancel == nullptr || !cancel->load(std::memory_order_relaxed)) return false;
    r.cancelled = true;
    return true;
  };
  // 取消出口：**只出账、不判定**。半截扫描的种数/边界/违例数拿去和 JS 对账会污染口径，
  // 所以 ok/ready 一律 false，报告里留「停在哪儿 + 已经付了多少读盘和 CPU」。
  auto stop_report = [&]() -> IndexReport {
    r.io_bytes = rd.io_bytes();
    rd.close();
    const CpuMark cpu_end = CpuMark::sample();
    r.total_ms = (qpc_now() - t_all) * 1000.0;
    r.cpu_user_ms = cpu_ms(cpu_all0, cpu_end);
    r.cpu_sys_ms = cpu_sys_ms(cpu_all0, cpu_end);
    r.ok = false;
    r.ready = false;
    r.reason = kPtIndexNotImplemented;
    r.boundary_reason = kPtOk;
    r.verdict = wf(L"已取消：停在%ls ⇒ 这一趟不做判定（ok=false，不进对账）", r.cancelled_at.c_str());
    return r;
  };

  r.unbuffered = unbuffered;
  r.sector = rd.align();
  r.file_size = rd.size();

  // ---- 顶层 box：只读每只 box 的 8/16 字节头 ----
  struct Top {
    uint64_t pos, size, hdr;
    std::wstring type;
  };
  std::vector<Top> tops;
  {
    uint64_t pos = 0;
    while (pos + 8 <= rd.size()) {
      if (wants_stop()) {
        r.cancelled_at = wf(L"顶层 box 走到第 %d 只（偏移 %llu）", int(tops.size()), ULL(pos));
        return stop_report();
      }
      size_t got = 0;
      const uint8_t *h = rd.window(pos, 8, &got);
      if (!h || got < 8) break;
      uint64_t sz = be32(h);
      uint64_t hdr = 8;
      const std::wstring type = fourcc(h + 4);
      if (sz == 1) {
        const uint8_t *h2 = rd.window(pos + 8, 8, &got);
        if (!h2 || got < 8) break;
        sz = be64(h2);
        hdr = 16;
      } else if (sz == 0) {
        sz = rd.size() - pos;
      }
      if (sz < 8) break;  // 合法文件走不到；坏文件不许在这里挂死
      tops.push_back(Top{pos, sz, hdr, type});
      if (r.top_boxes.size() < 240)
        r.top_boxes += wf(L"%ls@%llu+%llu ", type.c_str(), ULL(pos), ULL(sz));
      pos += sz;
    }
  }
  // ftyp 的 major brand（老 MOV 没有 ftyp，空着不算失败）。
  if (!tops.empty() && tops[0].type == L"ftyp" && tops[0].size >= tops[0].hdr + 4) {
    size_t got = 0;
    const uint8_t *p = rd.window(tops[0].pos + tops[0].hdr, 4, &got);
    if (p && got == 4) r.brand = fourcc(p);
  }
  size_t moov_i = SIZE_MAX;
  for (size_t i = 0; i < tops.size(); i++)
    if (tops[i].type == L"moov") {
      moov_i = i;
      break;
    }
  if (moov_i == SIZE_MAX) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = wf(L"顶层没有 moov（顶层 box %d 个：%ls）⇒ 非 MP4/MOV 或索引不可读，枚举 19", int(tops.size()),
                   r.top_boxes.c_str());
    return r;
  }
  r.moov_pos = tops[moov_i].pos;
  r.moov_size = tops[moov_i].size;
  // moov 的中心落在文件后半 ⇒ 非 faststart（1.mp4 就是：mdat@32、moov 在尾）。
  r.moov_tail = r.moov_pos + r.moov_size / 2 > r.file_size / 2;
  if (r.moov_size < 16 || r.moov_pos + r.moov_size > r.file_size ||
      tops[moov_i].hdr >= r.moov_size) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = wf(L"moov 声明越界（@%llu size=%llu 头=%llu 文件=%llu）⇒ 截断，枚举 19", ULL(r.moov_pos),
                   ULL(r.moov_size), ULL(tops[moov_i].hdr), ULL(r.file_size));
    return r;
  }

  // ---- 整只 moov 读进内存：电影尺度 27.64 MiB 一次读，实测 ~10ms ----
  std::vector<uint8_t> moov;
  {
    const double t0 = qpc_now();
    const CpuMark c0 = CpuMark::sample();
    bool bad = false;
    try {
      moov.resize(size_t(r.moov_size));
    } catch (const std::bad_alloc &) {
      bad = true;
    }
    if (!bad) {
      const size_t got = rd.read(r.moov_pos, moov.data(), size_t(r.moov_size));
      if (got < moov.size()) {
        r.verdict = wf(L"moov 只读到 %llu/%llu 字节 ⇒ 截断，枚举 19", ULL(got), ULL(r.moov_size));
        r.reason = kPtIndexNotImplemented;
        bad = true;
      }
    } else {
      r.verdict = wf(L"moov 要 %llu 字节，分配不下 ⇒ 枚举 19", ULL(r.moov_size));
      r.reason = kPtIndexNotImplemented;
    }
    r.moov_read_ms = (qpc_now() - t0) * 1000.0;
    r.cpu_moov_ms = cpu_both_ms(c0, CpuMark::sample());
    if (bad) return r;
  }
  const uint8_t *b = moov.data();
  const Range moov_body{tops[moov_i].hdr, moov.size()};  // moov 的孩子：跳过自己的头

  // ---- 第一条视频 trak（hdlr 的 handler_type 在内容 +8）----
  Range trak, stbl;
  {
    std::vector<Range> traks;
    box_find_all(b, moov_body, "trak", &traks);
    bool found = false;
    for (const Range &tk : traks) {
      Range hd;
      if (!box_descend(b, tk, {"mdia", "hdlr"}, &hd) || hd.off + 12 > hd.end) continue;
      if (memcmp(b + hd.off + 8, "vide", 4) != 0) continue;
      Range sb;
      if (!box_descend(b, tk, {"mdia", "minf", "stbl"}, &sb)) continue;
      trak = tk;
      stbl = sb;
      found = true;
      break;
    }
    if (!found) {
      r.reason = kPtCodecMismatch;
      r.verdict = wf(L"没有视频 trak（trak %d 条）⇒ 枚举 15", int(traks.size()));
      return r;
    }
  }

  Range mdhd, stsd, stsz_b, stsc_b, off_b, stss_b, stts_b, ctts_b;
  if (!box_descend(b, trak, {"mdia", "mdhd"}, &mdhd) || mdhd.off + 4 > mdhd.end) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = L"缺 mdia/mdhd ⇒ 枚举 19";
    return r;
  }
  const uint64_t ts_at = mdhd.off + (b[mdhd.off] == 1 ? 20 : 12);  // version 1 的时间基在第 20 字节
  if (ts_at + 4 > mdhd.end) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = L"mdhd 短到没有 timescale ⇒ 枚举 19";
    return r;
  }
  r.timescale = be32(b + ts_at);

  if (!box_find(b, stbl, "stsd", &stsd) || stsd.off + 16 > stsd.end) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = L"缺 stsd ⇒ 枚举 19";
    return r;
  }
  r.codec = fourcc(b + stsd.off + 12);  // 第一个样本 entry 的 format 四字符码
  // 分帧：在 stsd 内容里找 avcC，取配置记录第 5 字节的 lengthSizeMinusOne。
  // JS 探子从 stsd+8 一路搜到 moov 尾；这里限制在 stsd 之内（更严格），实测数相同。
  int length_size = 0;
  for (uint64_t p = stsd.off + 8; p + 12 <= stsd.end; p++) {
    if (b[p] == 'a' && b[p + 1] == 'v' && b[p + 2] == 'c' && b[p + 3] == 'C') {
      length_size = int(b[p + 12] & 3) + 1;
      break;
    }
  }
  r.framing = length_size ? wf(L"AVCC %d 字节长度", length_size)
                          : std::wstring(L"Annex-B 起始码（stsd 里没有 avcC）");
  // 只加出口：这一列和 framing 是同一个数的两种写法（一个是给人看的串，一个是给代码用的整数）。
  // 懒验要在某一个同步点的偏移上重走一遍**同一套分帧**，没有它就得自己再解一次 avcC ⇒ 两处读
  // 同一个数、两套算术，那是反复点名过的形状。不参与任何判定。
  r.avcc_length_size = length_size;
  if (r.codec != L"avc1" && r.codec != L"avc3") {
    r.reason = kPtCodecMismatch;
    r.verdict = wf(L"codec=%ls 不在 v1 范围（只支持 MP4/MOV 里的 avc1/avc3）⇒ 枚举 15", r.codec.c_str());
    return r;
  }

  if (!box_find(b, stbl, "stsz", &stsz_b) || stsz_b.off + 12 > stsz_b.end) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = L"缺 stsz ⇒ 枚举 19";
    return r;
  }
  Sizes sizes;
  sizes.b = b;
  sizes.box = stsz_b;
  sizes.uniform = be32(b + stsz_b.off + 4);
  sizes.count = be32(b + stsz_b.off + 8);
  r.samples = sizes.count;
  // 样本数大过文件字节数 = 表在撒谎（每个样本至少占一字节）。不挡住的话后面按它开数组能要几十 GB。
  if (r.samples == 0 || r.samples > r.file_size) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = wf(L"stsz 样本数=%llu（文件=%llu 字节）不合理 ⇒ 枚举 19", ULL(r.samples), ULL(r.file_size));
    return r;
  }

  if (!box_find(b, stbl, "stsc", &stsc_b) || stsc_b.off + 8 > stsc_b.end) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = L"缺 stsc ⇒ 枚举 19";
    return r;
  }
  const bool co64 = box_find(b, stbl, "co64", &off_b);
  if (!co64 && !box_find(b, stbl, "stco", &off_b)) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = L"既没有 co64 也没有 stco ⇒ 枚举 19";
    return r;
  }
  r.co64 = co64;

  const uint64_t stsc_n = be32(b + stsc_b.off + 4);
  r.stsc_entries = stsc_n;
  r.chunk_entries = be32(b + off_b.off + 4);
  const uint64_t stride = co64 ? 8 : 4;
  // 先验表长再分配：坏文件把条目数写成 40 亿时，这里就得停下来，而不是去要 96GB。
  if (stsc_n == 0 || stsc_b.off + 8 + stsc_n * 12 > stsc_b.end ||
      off_b.off + 8 + r.chunk_entries * stride > off_b.end) {
    r.reason = kPtIndexNotImplemented;
    r.verdict = wf(L"stsc(%llu 条)/%ls(%llu 条)声明越过 box ⇒ 截断或异常，枚举 19", ULL(stsc_n),
                   co64 ? L"co64" : L"stco", ULL(r.chunk_entries));
    return r;
  }
  const auto chunk_off_at = [&](uint64_t i) -> uint64_t {
    const uint64_t p = off_b.off + 8 + i * stride;
    return co64 ? be64(b + p) : be32(b + p);
  };
  const auto stsc_first = [&](uint64_t i) { return be32(b + stsc_b.off + 8 + i * 12); };
  const auto stsc_spc = [&](uint64_t i) { return be32(b + stsc_b.off + 12 + i * 12); };

  // 前缀表：这是「0.032 CPU 秒」和「10.75 CPU 秒」的分别，别退回逐样本重扫 stsc。
  std::vector<uint64_t> e_chunk0(stsc_n), e_spc(stsc_n), e_before(stsc_n);
  {
    uint64_t cs = 0, sam = 0;
    for (uint64_t i = 0; i < stsc_n; i++) {
      const uint32_t spc = stsc_spc(i);
      const uint64_t first = stsc_first(i);
      const uint64_t next = (i + 1 < stsc_n) ? stsc_first(i + 1) : r.chunk_entries + 1;
      if (spc == 0) {
        r.reason = kPtIndexNotImplemented;
        r.verdict = wf(L"stsc 第 %llu 条 samples_per_chunk=0 ⇒ 表异常，枚举 19", ULL(i));
        return r;
      }
      if (first < 1 || first > r.chunk_entries + 1 || next <= first) {
        r.reason = kPtIndexNotImplemented;
        r.verdict = wf(L"stsc 第 %llu 条 first_chunk=%llu→%llu 非单调（chunk 总数 %llu）⇒ 枚举 19", ULL(i),
                       ULL(first), ULL(next), ULL(r.chunk_entries));
        return r;
      }
      e_chunk0[i] = cs;
      e_spc[i] = spc;
      e_before[i] = sam;
      const uint64_t here = next - first;
      cs += here;
      sam += here * uint64_t(spc);
    }
    if (cs > r.chunk_entries || sam < r.samples) {
      r.reason = kPtIndexNotImplemented;
      r.verdict = wf(L"stsc 覆盖 chunk=%llu/偏移表=%llu、样本=%llu/stsz=%llu ⇒ 表不一致，枚举 19", ULL(cs),
                     ULL(r.chunk_entries), ULL(sam), ULL(r.samples));
      return r;
    }
  }
  // arr[] 升序，取最后一个 ≤v 的下标（lo 一定命中：arr[0] 分别是 0 与 0 样本）。
  const auto entry_for = [&](const std::vector<uint64_t> &arr, uint64_t v) -> uint64_t {
    uint64_t lo = 0, hi = stsc_n - 1;
    while (lo < hi) {
      const uint64_t m = (lo + hi + 1) >> 1;
      if (arr[m] <= v)
        lo = m;
      else
        hi = m - 1;
    }
    return lo;
  };
  const auto chunk_of = [&](uint64_t sample, uint64_t *chunk, uint64_t *in_chunk) {
    const uint64_t i = entry_for(e_before, sample - 1);
    const uint64_t d = sample - 1 - e_before[i];
    *chunk = e_chunk0[i] + d / e_spc[i];
    *in_chunk = d % e_spc[i];
  };
  const auto first_sample_of_chunk = [&](uint64_t chunk) -> uint64_t {
    const uint64_t i = entry_for(e_chunk0, chunk);
    return e_before[i] + (chunk - e_chunk0[i]) * e_spc[i] + 1;
  };

  {
    const double t0 = qpc_now();
    const CpuMark c0 = CpuMark::sample();
    // ---- 同步样本表：stss 给 1-based 样本号；没有 stss ⇒ 按 ISO 每个样本都是同步点 ----
    r.has_stss = box_find(b, stbl, "stss", &stss_b);
    std::vector<uint32_t> sync;
    bool bad = false;
    if (r.has_stss) {
      const uint64_t n = be32(b + stss_b.off + 4);
      if (n == 0 || stss_b.off + 8 + n * 4 > stss_b.end || n > r.file_size) {
        r.verdict = wf(L"stss 声明 %llu 条越过 box 或样本数 ⇒ 截断，枚举 19", ULL(n));
        bad = true;
      } else {
        sync.reserve(size_t(n));
        for (uint64_t i = 0; i < n; i++) sync.push_back(be32(b + stss_b.off + 8 + i * 4));
      }
    } else {
      sync.reserve(size_t(r.samples));
      for (uint64_t s = 1; s <= r.samples; s++) sync.push_back(uint32_t(s));
    }
    // 校验 + 换成字节偏移。任何越界都是表撒谎 ⇒ 索引不可信。
    if (!bad) {
      r.sync_samples.reserve(sync.size());
      r.sync_offsets.reserve(sync.size());
      uint64_t prev = 0;
      for (const uint32_t sn : sync) {
        if (sn < 1 || uint64_t(sn) > r.samples) {
          r.verdict = wf(L"stss 有样本号 %u 不在 1..%llu ⇒ 枚举 19", sn, ULL(r.samples));
          bad = true;
          break;
        }
        uint64_t chunk = 0, in_chunk = 0;
        chunk_of(sn, &chunk, &in_chunk);
        if (chunk >= r.chunk_entries) {
          r.verdict = wf(L"样本 %u 落到 chunk %llu（偏移表只有 %llu 条）⇒ 枚举 19", sn, ULL(chunk),
                         ULL(r.chunk_entries));
          bad = true;
          break;
        }
        uint64_t off = chunk_off_at(chunk);
        const uint64_t fsc = first_sample_of_chunk(chunk);
        for (uint64_t k = 0; k < in_chunk; k++) off += sizes.at(fsc + k - 1);
        if (off >= rd.size()) {
          r.verdict = wf(L"样本 %u 的字节偏移 %llu 越过文件尾 %llu ⇒ 截断，枚举 19", sn, ULL(off),
                         ULL(rd.size()));
          bad = true;
          break;
        }
        if (sn <= prev) {
          r.verdict = wf(L"stss 样本号非升序（%llu 跟在 %llu 后）⇒ 索引不可信，枚举 19", ULL(sn), ULL(prev));
          bad = true;
          break;
        }
        prev = sn;
        r.sync_samples.push_back(sn);
        r.sync_offsets.push_back(off);
      }
    }
    r.index_ms = (qpc_now() - t0) * 1000.0;
    r.cpu_index_ms = cpu_both_ms(c0, CpuMark::sample());
    if (bad) {
      r.reason = kPtIndexNotImplemented;
      r.sync_samples.clear();
      r.sync_offsets.clear();
      return r;
    }
  }
  r.sync_count = r.sync_samples.size();
  r.gop = r.sync_count ? double(r.samples) / double(r.sync_count) : 0.0;

  // moov 已读完、判据未开跑：这是冷态里第一个「可以无损停下」的里程碑（读盘账照记）。
  if (wants_stop()) {
    r.cancelled_at = wf(L"读完 moov（%.2f MiB）与 %llu 个同步点、判据循环还没开",
                        double(r.moov_size) / (1024 * 1024), ULL(r.sync_count));
    return stop_report();
  }

  // ---- 判据②：逐样本 pts，同步点到下个同步点之间不许出现更早的 pts ----
  {
    const double t0 = qpc_now();
    const CpuMark c0 = CpuMark::sample();
    bool ok2 = box_find(b, stbl, "stts", &stts_b) && stts_b.off + 8 <= stts_b.end;
    const uint64_t stts_n = ok2 ? be32(b + stts_b.off + 4) : 0;
    r.stts_entries = stts_n;
    if (ok2 && (stts_n == 0 || stts_b.off + 8 + stts_n * 8 > stts_b.end || stts_n > r.file_size)) ok2 = false;
    std::vector<int64_t> pts;
    if (ok2) {
      try {
        pts.assign(size_t(r.samples + 1), 0);
      } catch (const std::bad_alloc &) {
        ok2 = false;
      }
    }
    if (ok2) {
      int64_t dts = 0;
      uint64_t s = 0;
      for (uint64_t i = 0; i < stts_n && s < r.samples; i++) {
        const uint64_t cnt = be32(b + stts_b.off + 8 + i * 8);
        const uint32_t dur = be32(b + stts_b.off + 12 + i * 8);
        for (uint64_t k = 0; k < cnt && s < r.samples; k++) {
          s++;
          pts[size_t(s)] = dts;
          dts += dur;
        }
      }
      r.ctts_covered = s;
      ok2 = s == r.samples;  // stts 没盖住全部样本 ⇒ 后半段的 pts 是 0，判据②不能算跑过
    }
    if (ok2 && box_find(b, stbl, "ctts", &ctts_b) && ctts_b.off + 8 <= ctts_b.end) {
      const uint64_t n = be32(b + ctts_b.off + 4);
      r.ctts_entries = n;
      if (n == 0 || n > r.file_size || ctts_b.off + 8 + n * 8 > ctts_b.end) {
        ok2 = false;  // ctts 异常 ⇒ ②不可判（坏文件不许崩那一类）
      } else {
        const uint8_t ver = b[ctts_b.off];
        uint64_t s2 = 1;
        for (uint64_t i = 0; i < n && s2 <= r.samples; i++) {
          const uint64_t cnt = be32(b + ctts_b.off + 8 + i * 8);
          // version 1 的 offset 是有符号的（负偏移 = 展示序在解码序之前），version 0 是无符号。
          const int64_t o = ver == 1 ? int64_t(int32_t(be32(b + ctts_b.off + 12 + i * 8)))
                                     : int64_t(be32(b + ctts_b.off + 12 + i * 8));
          for (uint64_t k = 0; k < cnt && s2 <= r.samples; k++, s2++) {
            if (s2 > r.ctts_covered) r.ctts_covered = s2;
            if (o == 0) continue;
            pts[size_t(s2)] += o;
            r.ctts_nonzero++;
          }
        }
      }
    }
    r.rule2_comparable = ok2 && r.timescale > 0;
    if (r.rule2_comparable) {
      r.sync_pts.reserve(r.sync_samples.size());
      for (size_t i = 0; i < r.sync_samples.size(); i++) {
        const uint64_t a = r.sync_samples[i];
        const uint64_t nx = (i + 1 < r.sync_samples.size()) ? r.sync_samples[i + 1] : r.samples + 1;
        int64_t min_pts = pts[size_t(a)];
        uint64_t min_at = a;
        r.sync_pts.push_back(double(pts[size_t(a)]) / r.timescale);
        for (uint64_t s2 = a; s2 < nx && s2 <= r.samples; s2++)
          if (pts[size_t(s2)] < min_pts) {
            min_pts = pts[size_t(s2)];
            min_at = s2;
          }
        if (min_pts < pts[size_t(a)]) {
          r.pts_violations++;
          if (r.pts_example.empty())
            r.pts_example = wf(L"同步样本 %llu 之后第 %llu 项 pts 比它早 %.4fs", ULL(a), ULL(min_at),
                               double(pts[size_t(a)] - min_pts) / r.timescale);
        }
      }
    }
    r.rule2_ms = (qpc_now() - t0) * 1000.0;
    r.cpu_rule2_ms = cpu_both_ms(c0, CpuMark::sample());
  }

  // ---- 判据①：每个同步样本的字节偏移上读一窗，走 NALU 看第一个条片 ----
  // skip_rule1 = 建表那一趟：这一圈是「每个同步点一次设备读」，一只 3 小时素材要读两千多次，
  // 而这一层买的是「落点之前最近的那只同步点的秒数」，第一片类型改由**懒验**在真要定位的那一格上读
  // 一窗（见 synctable.h 的懒验一节）。跳过之后判据① 的那几列（idr/non_idr/no_slice/short_read/
  // 相邻段参数集）全留初值 ⇒ 表的谓词一个字都不读它们，也读不到假的「0 个非 IDR = 全绿」。
  if (!skip_rule1) {
    const double t0 = qpc_now();
    const CpuMark c0 = CpuMark::sample();
    NaluTally tally;
    // 相邻段比较的状态：上一只同步样本的参数集键 + 它是否「完整可读」（截断就不许拿来比）
    // + 它段头到底有没有带内参数集（两侧都没有 ⇒ 这一笔边界不记，见下面的理由）。
    bool have_prev_key = false;
    uint64_t prev_key = 0;
    bool prev_complete = false;
    bool prev_has_params = false;
    for (size_t i = 0; i < r.sync_offsets.size(); i++) {
      // 冷态下这一圈就是那 1.7~2.4 秒的主要去处（每格一次设备读），所以每格检一次标志：
      // 点「取消」到线程真的停手实测 14~30ms = 一到两拍 timer。
      if (wants_stop()) {
        r.cancelled_at = wf(L"判据①读到第 %d/%d 个同步点", int(i + 1), int(r.sync_offsets.size()));
        return stop_report();
      }
      size_t got = 0;
      const uint8_t *p = rd.window(r.sync_offsets[i], r.window_bytes, &got);
      const size_t n = p ? got : 0;
      if (n < 4) {
        r.short_read++;
        have_prev_key = false;  // 这一格读不到 ⇒ 它与下一格之间的边界不可证
        prev_complete = false;
        continue;
      }
      // 每只样本各用一份 tally：种数看全文件（合并），**相邻段**看单只样本（不合并就没法比）。
      NaluTally one;
      SliceWalk w;
      walk_first_slice(p, n, length_size, &w, &one);
      if (w.slice == 5)
        r.idr++;
      else if (w.slice == 1)
        r.non_idr++;
      else
        r.no_slice++;
      if (w.bytes_to > r.max_bytes_to_slice) r.max_bytes_to_slice = w.bytes_to;
      if (w.walked > r.max_nalu_walk) r.max_nalu_walk = w.walked;

      // ---- 相邻段参数集比较（回退级只看这一条）----
      // 两侧段头都没有带内参数集时**不记这一笔**：空对空相等证明不了任何事，把它算成
      // 「可比且一致」会和上面那句「无从比对」自相矛盾。有一侧带参数集才比 ——
      // 一带一不带也是真不一致（那一段不自带参数集，靠的是封装/avcC，拼接点上一边换过规矩）。
      const bool complete = !one.truncated;
      const bool has_params = one.sps || one.pps;
      if (has_params) r.sync_with_params++;
      if (complete) {
        const uint64_t key = sample_param_key(&one);
        if (have_prev_key && prev_complete) {
          if (has_params || prev_has_params) {
            r.boundary_pairs++;
            if (key != prev_key) {
              r.boundary_mismatch++;
              if (r.boundary_mismatch_at.size() < 32) r.boundary_mismatch_at.push_back(uint64_t(i));
              if (r.boundary_example.empty())
                r.boundary_example =
                    wf(L"第 %llu 与第 %llu 个同步点（样本 %llu / %llu）段头参数集不同", ULL(i), ULL(i + 1),
                       ULL(r.sync_samples[i - 1]), ULL(r.sync_samples[i]));
            }
          }
        } else if (have_prev_key) {
          r.boundary_unknown++;  // 上一格截断/读不到 ⇒ 这条边界不能算证过
        }
        prev_key = key;
        prev_has_params = has_params;
        have_prev_key = true;
        prev_complete = true;
      } else {
        if (have_prev_key && prev_complete) r.boundary_unknown++;
        have_prev_key = true;  // 键留着但标记不完整：它参与的边界一律进 unknown
        prev_complete = false;
      }

      tally.sps += one.sps;
      tally.pps += one.pps;
      tally.sei += one.sei;
      tally.aud += one.aud;
      tally.truncated = tally.truncated || one.truncated;
      tally.sps_hash.insert(tally.sps_hash.end(), one.sps_hash.begin(), one.sps_hash.end());
      tally.pps_hash.insert(tally.pps_hash.end(), one.pps_hash.begin(), one.pps_hash.end());
    }
    r.nalu_sps = tally.sps;
    r.nalu_pps = tally.pps;
    r.nalu_sei = tally.sei;
    r.nalu_aud = tally.aud;
    auto kinds = [](std::vector<uint64_t> *v) {
      std::sort(v->begin(), v->end());
      return uint64_t(std::unique(v->begin(), v->end()) - v->begin());
    };
    r.sps_kinds = kinds(&tally.sps_hash);
    r.pps_kinds = kinds(&tally.pps_hash);
    // 单文件口径（**报告级**）：0=没有带内参数集（只有 avcC 里那一份）1=单文件内一致
    // 2=单文件多变（只报种数，不回退）3=不可判（有参数集出了窗口）。
    // 「不一致 ⇒ 回退」这一档不归这一行：回退只看下面的相邻段比较。
    if (r.nalu_sps == 0 && r.nalu_pps == 0)
      r.sps_pps_state = 0;
    else if (tally.truncated)
      r.sps_pps_state = 3;
    else if (r.sps_kinds > 1 || r.pps_kinds > 1)
      r.sps_pps_state = 2;
    else
      r.sps_pps_state = 1;
    r.rule1_ms = (qpc_now() - t0) * 1000.0;
    r.cpu_rule1_ms = cpu_both_ms(c0, CpuMark::sample());
  }

  r.io_bytes = rd.io_bytes();
  rd.close();
  moov.clear();

  // ---- 判定 ----
  // 两列分开判（别混成一个号）：
  //   文件级 reason 只由「整片能不能当一段直通」决定；
  //   边界级 boundary_reason 只由「哪些拼接点不安全」决定，它不改 ready —— 整片作一段时没有拼接点。
  // 所以边界不一致**不进下面这条链**，只在链尾追加一句，免得它把「判据没跑全」那几条压掉。
  r.ok = true;
  r.reason = kPtOk;
  r.ready = true;
  if (skip_rule1) {
    // 这一趟没跑判据① ⇒ 「恢复点全判」这句话没有依据，ready 必须自己说不成立。
    // reason 仍然是 kPtOk：它是**文件级**的回退原因，判据② 那一半（前导帧）跑过了、没违例，
    // 而表的谓词读的正是「容器索引可用 + 判据② 可比 + 无违例」，不读 ready、也不读判据①。
    r.ready = false;
    r.verdict = L"skip_rule1：判据① 未跑 ⇒ idr/non_idr/no_slice/相邻段参数集四栏是初值而不是「全绿」，"
                L"直通依据不足（建表只用判据② 那一半）";
  } else if (r.non_idr > 0 || r.pts_violations > 0) {
    r.reason = kPtOpenGop;
    r.ready = false;
    r.verdict = wf(L"同步点里 %llu 个第一片不是 IDR、%llu 处后面挂着更早的 pts ⇒ 判 open-GOP，枚举 2（v1 不直通）",
                   ULL(r.non_idr), ULL(r.pts_violations));
  } else if (r.no_slice > 0 || r.short_read > 0) {
    r.ready = false;
    r.verdict = wf(L"%llu 个同步点在窗口里没找到条片、%llu 个短读 ⇒ 恢复点未全判，不能算直通通过",
                   ULL(r.no_slice), ULL(r.short_read));
  } else if (!r.rule2_comparable) {
    r.ready = false;
    r.verdict = wf(L"判据①全过（IDR %llu/%llu），但 stts/ctts 覆盖不足 ⇒ 判据②没跑成，直通依据不足",
                   ULL(r.idr), ULL(r.sync_count));
  } else if (r.sps_pps_state != 0 && r.sync_with_params < r.sync_count) {
    // 「直通段必须从同步点起 + 段头带 SPS/PPS」这条前置里后半截没满足：
    // 有段头不自带的段 ⇒ 整片能直通，但那些段单独切出来不自洽，v1 不假装封装会补。
    r.ready = false;
    r.verdict = wf(L"①IDR %llu/%llu ②违例 %llu/%llu；但 %llu/%llu 个同步点的段头没有带内参数集 ⇒ "
                   L"「段头带 SPS/PPS」前置不成立，直通依据不足",
                   ULL(r.idr), ULL(r.sync_count), ULL(r.pts_violations), ULL(r.sync_count),
                   ULL(r.sync_count - r.sync_with_params), ULL(r.sync_count));
  } else if (r.boundary_unknown > 0 || r.sps_pps_state == 3) {
    r.ready = false;
    r.verdict = wf(L"①IDR %llu/%llu ②违例 0 ⇒ closed-GOP；但 %llu 处边界的参数集出了窗口 ⇒ "
                   L"相邻段一致性不可证，直通依据不足",
                   ULL(r.idr), ULL(r.sync_count), ULL(r.boundary_unknown));
  } else if (r.sps_pps_state == 0) {
    // 参数集只在 avcC 里一份 ⇒ 带内没有可比的副本；但「段头带 SPS/PPS」这条前置也就不成立：
    // 直通输出得由封装侧替每段补上参数集，否则段不自洽。
    r.verdict = wf(L"①IDR %llu/%llu ②前导帧违例 %llu/%llu ⇒ closed-GOP；带内无参数集（只在 avcC 一份）⇒ "
                   L"相邻段无从比对，直通时须由封装把 avcC 那份写进每段段头",
                   ULL(r.idr), ULL(r.sync_count), ULL(r.pts_violations), ULL(r.sync_count));
  } else {
    r.verdict = wf(L"①IDR %llu/%llu ②前导帧违例 %llu/%llu ⇒ closed-GOP；相邻段参数集可比 %llu 处边界，"
                   L"不一致 %llu 处 ⇒ 容器索引成立",
                   ULL(r.idr), ULL(r.sync_count), ULL(r.pts_violations), ULL(r.sync_count),
                   ULL(r.boundary_pairs), ULL(r.boundary_mismatch));
  }
  if (r.boundary_mismatch > 0) {
    r.boundary_reason = kPtSpsPpsInconsistent;
    r.verdict += wf(L"；另：相邻段 %llu/%llu 处边界的带内参数集不是同一套 ⇒ 剪到那些位置的段回退（边界级 16），"
                    L"文件级判定不受影响",
                    ULL(r.boundary_mismatch), ULL(r.boundary_pairs));
  }
  const CpuMark cpu_end = CpuMark::sample();
  r.total_ms = (qpc_now() - t_all) * 1000.0;
  r.cpu_user_ms = cpu_ms(cpu_all0, cpu_end);
  r.cpu_sys_ms = cpu_sys_ms(cpu_all0, cpu_end);
  return r;
}

// ---------------------------------------------------------------- 懒验的单格出口

IdrProbe probe_sync_idr_at(const std::wstring &path, uint64_t offset, int length_size,
                           size_t window_bytes) {
  IdrProbe out;
  Reader rd;
  if (!rd.open(path, false)) return out;  // 打不开：read_ok=false，让调用方去停懒验，不当成「不是 IDR」
  size_t got = 0;
  const uint8_t *p = rd.window(offset, window_bytes, &got);
  out.got = p ? got : 0;
  if (out.got < 4) return out;  // 短读同上一条：这一格没判成
  out.read_ok = true;
  SliceWalk w;
  NaluTally one;
  walk_first_slice(p, out.got, length_size, &w, &one);
  out.slice = w.slice;
  return out;
}

}  // namespace app
