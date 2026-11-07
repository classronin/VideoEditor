#include "audio.h"
#include "publish.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

// ops.cpp 里的 VSE_TRACE 落盘函数（audio.cpp 不引 app.h，避免循环包含）。
namespace app {
void trace(const std::wstring &s);
}
using app::trace;

namespace {

// 自行定义 GUID，避免依赖 __uuidof 与导入库符号。
const CLSID kEnumClsid = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const IID kAudioClientIid = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const IID kRenderClientIid = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
const IID kDeviceEnumIid = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const GUID kSubPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr double kResync = 0.2;  // 秒：小于此误差认为音画同步

std::wstring hr_suffix(HRESULT hr) {
  wchar_t b[40];
  swprintf(b, 40, L"（0x%08lX）", DWORD(hr));
  return b;
}

// 日志里的时刻统一三位小数，够用又不至于撑爆行宽。
std::wstring fnum4(double v) {
  wchar_t b[32];
  swprintf(b, 32, L"%.3f", v);
  return std::wstring(b);
}

}  // namespace

AudioPlayer::~AudioPlayer() {
  HANDLE t = nullptr;
  {
    std::unique_lock<std::mutex> lk(mu_);
    quit_ = true;
    t = thread_;
  }
  if (wake_) SetEvent(wake_);
  if (t) {
    WaitForSingleObject(t, 2000);
    CloseHandle(t);
  }
  if (wake_) {
    CloseHandle(wake_);
    wake_ = nullptr;
  }
}

// 排队中的那一段只看内容：位置漂了就让它漂，播放头会被新的发声位置拽回来。反复替换排队
// 会让工作线程一遍遍重开 ffmpeg，实测一次条带边界被拖成 0.68 秒死寂。
bool AudioPlayer::same_inputs(const Req &ref, const Req &want) {
  if (ref.inputs.size() != want.inputs.size()) return false;
  for (size_t i = 0; i < want.inputs.size(); ++i) {
    const MixInput &a = ref.inputs[i], &b = want.inputs[i];
    if (a.vi != b.vi) return false;
    if (std::fabs(b.until - a.until) >= 1e-4) return false;
    if (std::fabs(b.speed - a.speed) >= 1e-6) return false;
  }
  return true;
}

// 正在推的那一段还要看「现在听到的位置」是不是仍在想要的那一段窗口里。
bool AudioPlayer::same_set(const Req &ref, double live, const Req &want) {
  return same_inputs(ref, want) && std::fabs(want.from - live) < kResync;
}

void AudioPlayer::play(const std::vector<MixInput> &inputs, double tl_from, double tl_until) {
  std::unique_lock<std::mutex> lk(mu_);
  if (!thread_) {
    if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) {
      err_ = L"无法创建音频事件";
      return;
    }
    quit_ = false;
    thread_ = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
    if (!thread_) {
      err_ = L"无法启动音频线程";
      return;
    }
  }
  if (inputs.empty() || tl_until - tl_from < 1e-4) {
    // 没人发声就是停：这里已经握着 mu_，不能转而调用 stop()。
    if (pending_.empty() && !streaming_ && !stop_req_) return;
    pending_ = Req{};
    stop_req_ = true;
    streaming_ = false;
    cur_pos_ = -1.0;
    SetEvent(wake_);
    return;
  }
  Req want{inputs, tl_from, tl_until};
  // 正在出声才拿发声位置比大小；还在建立/还在排队时替换请求，只会让工作线程白重开一次
  // ffmpeg（每次重开都是几十到几百毫秒的死寂，而且永远追不上重建的速度）。
  if (!inflight_.empty() &&
      (streaming_ && cur_pos_ >= 0.0 ? same_set(inflight_, cur_pos_, want)
                                     : same_inputs(inflight_, want)))
    return;
  if (!pending_.empty() && same_inputs(pending_, want)) return;
  // 每次换段留一行：跨过条带边界就该出现一段新的，出现得比对账多就是在反复冷启动（每次
  // 冷启动都要重开 ffmpeg，中间是死寂）；发声位置和请求起点对不上就是漂出容差的那种。
  {
    wchar_t b[160];
    swprintf(b, 160, L"audio rebuild: 请求 [%.3f→%.3f]，声部 %llu 路，发声在 %.3f", tl_from,
             tl_until, static_cast<unsigned long long>(inputs.size()), cur_pos_);
    trace(b);
  }
  pending_ = std::move(want);
  // 排队即作废：旧那一段的位置不能再拿来定播放头，否则拖动播放头/循环回跳的那一 tick，
  // 播放头会被上一段拽回去。重建期间播放头按界面时钟自己走，新段一出声就接上。
  cur_pos_ = -1.0;
  stop_req_ = false;
  SetEvent(wake_);
}

void AudioPlayer::stop() {
  std::unique_lock<std::mutex> lk(mu_);
  if (pending_.empty() && !streaming_ && !stop_req_) return;
  pending_ = Req{};
  stop_req_ = true;
  streaming_ = false;
  cur_pos_ = -1.0;
  if (wake_) SetEvent(wake_);
}

double AudioPlayer::position() const {
  std::unique_lock<std::mutex> lk(mu_);
  // 排队中的那一段最new：它就是界面刚刚要的位置，发声位置还没追上它之前，以它为准。
  // 反过来先交回旧那一段的耳朵位置，界面这一格会被旧段往前拽，下一格才回到受理点 —— 倒退。
  if (!pending_.empty()) return pending_.from;
  if (cur_pos_ >= 0.0) return cur_pos_;
  // 还没出声不等于没有位置：这一段已经受理，它就落在受理时的刻度上。这里交回受理点，音频时钟
  // 才从受理那一格起连续；交回 -1 就是「此刻没话说」，界面时钟会先往前走几步，等流一通再把
  // 播放头拽回受理点 —— 播放头来回跳，量化到整帧就是画面在两帧之间来回跳。
  return inflight_.empty() ? -1.0 : inflight_.from;
}

std::wstring AudioPlayer::error() const {
  std::unique_lock<std::mutex> lk(mu_);
  return err_;
}

// ---------------------------------------------------------------- 工作线程

DWORD WINAPI AudioPlayer::thread_main(LPVOID p) {
  reinterpret_cast<AudioPlayer *>(p)->run();
  return 0;
}

void AudioPlayer::run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  for (;;) {
    Req req;
    bool want_stop = false;
    {
      std::unique_lock<std::mutex> lk(mu_);
      if (quit_) break;
      if (!pending_.empty() || stop_req_) {
        req = std::move(pending_);
        pending_ = Req{};
        want_stop = stop_req_;
        stop_req_ = false;
        // 受理就在这一格记账，不留空窗。旧版这里先清空 pending_、等杀完旧流才登记 inflight_，
        // 中间那几十毫秒（重开 ffmpeg）position() 两头都没有 ⇒ 交回 -1 ⇒ 界面时钟自由前进，
        // 等流一通又被拽回受理点：播放头倒退，量化到整帧就是画面在两帧之间跳。
        // 停止请求走的也是这一格，req 为空即清空，此时确实无话可说。
        inflight_ = req;
      }
    }
    if (req.empty() && !want_stop) {
      WaitForSingleObject(wake_, 100);
      continue;
    }
    kill_stream();
    mark_idle();
    if (req.empty()) continue;
    if (!client_ && !open_device()) continue;
    if (start_stream(req)) play_loop();
  }
  close_device();
  CoUninitialize();
}

void AudioPlayer::mark_idle() {
  std::lock_guard<std::mutex> lk(mu_);
  streaming_ = false;
  cur_pos_ = -1.0;
}

bool AudioPlayer::open_device() {
  IMMDeviceEnumerator *en = nullptr;
  HRESULT hr = CoCreateInstance(kEnumClsid, nullptr, CLSCTX_ALL, kDeviceEnumIid, reinterpret_cast<void **>(&en));
  if (FAILED(hr) || !en) {
    fail(L"[enum] 无法访问音频子系统", hr);
    return false;
  }
  IMMDevice *dev = nullptr;
  hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  en->Release();
  if (FAILED(hr) || !dev) {
    fail(L"[endpoint] 没有默认播放设备", hr);
    return false;
  }
    // 端点支持用接口 IID 直接激活（文档保证可用），免去手写 CLSID_AudioRender 的值。
  hr = dev->Activate(kAudioClientIid, CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void **>(&client_));
  dev->Release();
  if (FAILED(hr) || !client_) {
    client_ = nullptr;
    fail(L"[activate] 无法打开播放端点", hr);
    return false;
  }
  hr = client_->GetMixFormat(&mix_);
  if (FAILED(hr) || !mix_) {
    fail(L"[mixfmt] 读取混音格式失败", hr);
    close_device();
    return false;
  }
  bool is_float = mix_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
  bool is_pcm = mix_->wFormatTag == WAVE_FORMAT_PCM && mix_->wBitsPerSample == 16;
  if (mix_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
      mix_->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    auto *t = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix_);
    is_float = memcmp(&t->SubFormat, &kSubFloat, sizeof(GUID)) == 0;
    is_pcm = memcmp(&t->SubFormat, &kSubPcm, sizeof(GUID)) == 0 && mix_->wBitsPerSample == 16;
  }
  if (!is_float && !is_pcm) {
    fail(L"[format] 播放设备格式不受支持", hr);
    close_device();
    return false;
  }
  pcm16_ = !is_float;
  rate_ = int(mix_->nSamplesPerSec) > 0 ? int(mix_->nSamplesPerSec) : 48000;
  ch_ = mix_->nChannels > 0 ? int(mix_->nChannels) : 2;

  // 缓冲 200ms 而不是 1 秒：推流模式下设备把整块缓冲吃完之前，耳朵还没听到声音，而播放头跟的
  // 就是「听到的位置」——1 秒的预卷在每个段起点都是画面停住不动的 0.16 秒（实测循环回跳后
  // 连续 10 tick ph 不动）。ffmpeg 比设备快几十倍，200ms 照样喂得满，Underrun 的余量还有 20 倍。
  hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0 /* 推流模式 */, 2000000 /* 200ms */, 0, mix_,
                           nullptr);
  if (FAILED(hr)) {
    fail(L"[init] 初始化音频流失败", hr);
    close_device();
    return false;
  }
  client_->GetBufferSize(&buf_frames_);
  client_->GetDevicePeriod(&period_hns_, nullptr);
  hr = client_->GetService(kRenderClientIid, reinterpret_cast<void **>(&render_));
  if (FAILED(hr) || !render_) {
    fail(L"[render] 无法取得渲染缓冲区", hr);
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    err_.clear();
  }
  return true;
}

void AudioPlayer::fail(const wchar_t *what, HRESULT hr) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    err_ = what;
    if (FAILED(hr)) err_ += hr_suffix(hr);
  }
  // 记完错再说这一声：UI 不再每 tick 问「音频有没有出错」。没有它，
  // 设备打不开这件事就只有等下一次鼠标动静才看得见。
  app::ui_publish(app::kPubAudio);
}

void AudioPlayer::close_device() {
  if (client_) client_->Stop();
  if (render_) {
    render_->Release();
    render_ = nullptr;
  }
  if (client_) {
    client_->Release();
    client_ = nullptr;
  }
  if (mix_) {
    CoTaskMemFree(mix_);
    mix_ = nullptr;
  }
  buf_frames_ = 0;
}

// 停流后清空设备缓冲，确保新位置立即出声。
void AudioPlayer::halt_device() {
  if (!client_) return;
  client_->Stop();
  client_->Reset();  // 丢弃已提交的缓冲，避免停止后仍有残音
}

bool AudioPlayer::start_stream(const Req &req) {
  from_ = req.from;
  until_ = req.until;
  left_ = int64_t((until_ - from_) * double(rate_));
  bytes_.clear();
  bpos_ = 0;
  src_done_ = false;
  real_frames_ = 0;
  if (left_ <= 0) return false;
  proc_ = Ffmpeg::audio_mix(req.inputs, rate_);
  if (!proc_) {
    fail(L"[decode] 音频解码启动失败", S_OK);
    return false;
  }
  halt_device();
  HRESULT hr = client_->Start();
  if (FAILED(hr)) {
    fail(L"[start] 启动音频流失败", hr);
    kill_stream();
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    cur_pos_ = from_;
    streaming_ = true;
  }
  // 每次真正起一段留一行：段边界跨过去就该出现一段新的，出现得比对账多说明在反复冷启动
  // （每次冷启动都要重开 ffmpeg，中间是死寂）。
  std::wstring line = L"audio mix: " + std::to_wstring(req.inputs.size()) + L" 路 段 [" +
                      fnum4(from_) + L"→" + fnum4(until_) + L"]";
  wchar_t b[96];
  for (const MixInput &m : req.inputs) {
    swprintf(b, 96, L" [%p %.3f→%.3f ×%.3f]", (void *)m.vi, m.from, m.until, m.speed);
    line += b;
  }
  trace(line);
  return true;
}

void AudioPlayer::kill_stream() {
  halt_device();
  if (!proc_) return;
  FrameProc *p = proc_;
  proc_ = nullptr;
  // 收尸丢给别的线程：实测等一个 amix 进程真退出并回收它的 stderr 线程要 918 毫秒，
  // 留在音频线程里就是条带边界上 0.9 秒的死寂。p 交出后这里再也不碰它。
  std::thread([p] {
    p->kill();
    delete p;
  }).detach();
}

void AudioPlayer::play_loop() {
  HRESULT hr = S_OK;
  for (;;) {
    bool want_stop = false, want_new = false;
    {
      std::unique_lock<std::mutex> lk(mu_);
      want_stop = quit_ || stop_req_;
      want_new = !pending_.empty();
    }
    if (want_stop || want_new) return;
    UINT32 pad = 0;
    hr = client_->GetCurrentPadding(&pad);
    if (FAILED(hr)) {
      fail(L"[padding] 读取播放进度失败", hr);
      return;
    }
    {
      // 已提交但仍在设备缓冲里的帧尚未发声，扣除后才是耳朵听到的位置。这一段本来就是按
      // 时间线排的，混音后的采样率和条带速度都在 ffmpeg 图里做掉了，所以位置就是时间线秒。
      std::unique_lock<std::mutex> lk(mu_);
      int64_t audible = real_frames_ - int64_t(pad);
      cur_pos_ = audible > 0 ? from_ + double(audible) / double(rate_ > 0 ? rate_ : 48000)
                            : from_;
    }
    UINT32 space = buf_frames_ > pad ? buf_frames_ - pad : 0;
    if (space > 0 && left_ > 0) {
      const size_t need = size_t(std::min<int64_t>(space, left_));
      // 第一段料要等 ffmpeg 起进程和首帧解码，之后每轮只等一下下：管道常年比设备快
      // 几十倍（实测 268x），等太久就是自己把自己卡住。
      refill(need, real_frames_ == 0 ? 500 : 20);
      size_t avail = (bytes_.size() - bpos_) / (2 * sizeof(float));
      if (avail > size_t(left_)) avail = size_t(left_);
      if (avail == 0) {
        if (src_done_) left_ = 0;  // 素材本身没有这么多音频，收尾排空即停
      } else {
        UINT32 n = UINT32(avail);
        BYTE *dst = nullptr;
        hr = render_->GetBuffer(n, &dst);
        if (FAILED(hr) || !dst) {
          fail(L"[buffer] 写入音频缓冲区失败", hr);
          return;
        }
        write_device(dst, reinterpret_cast<const float *>(bytes_.data() + bpos_), n);
        render_->ReleaseBuffer(n, 0);
        bpos_ += size_t(n) * 2 * sizeof(float);
        left_ -= int64_t(n);
        real_frames_ += int64_t(n);
        if (bpos_ > (1u << 17)) {
          bytes_.erase(bytes_.begin(), bytes_.begin() + long(bpos_));
          bpos_ = 0;
        }
      }
    }
    Sleep(period_hns_ / 10000 > 0 ? DWORD(period_hns_ / 10000) : 10);
    if (left_ <= 0) {
      // 音频已全部提交，等设备缓冲排空再收流，否则尾巴会被 Reset 截掉。
      UINT32 rest = 0;
      if (FAILED(client_->GetCurrentPadding(&rest)) || rest == 0) return;
    }
  }
}

// 从管道补充至多 need_frames 个立体声帧，等不到就带着已有的部分返回。
// 真的等一小会儿：这一轮拿不到料就只能提交 0 帧，设备缓冲被抽干，发声位置停住。20 毫秒
// 远小于缓冲里的余量，而且这是独立工作线程，不碰界面。
void AudioPlayer::refill(size_t need_frames, int wait_ms) {
  if (!proc_ || src_done_) return;
  if (stage_.empty()) stage_.resize(1 << 16);
  size_t want = need_frames * 2 * sizeof(float);
  while (bytes_.size() - bpos_ < want) {
    if (!proc_->wait_ready(DWORD(wait_ms))) {
      // 管道暂时没数据：只有进程确实退了才算素材读完，否则只是解码没跟上。
      if (proc_->exited(nullptr)) src_done_ = true;
      return;
    }
    size_t room = want - (bytes_.size() - bpos_);
    if (room > stage_.size()) room = stage_.size();
    size_t got = proc_->read_some(stage_.data(), room);
    if (!got) {
      src_done_ = true;
      return;
    }
    bytes_.insert(bytes_.end(), stage_.data(), stage_.data() + long(got));
  }
}

// 立体声混音 -> 设备格式：按需展开声道、钳位，pcm16 端点做定点转换。
void AudioPlayer::write_device(BYTE *dst, const float *src, uint32_t frames) {
  float *fout = pcm16_ ? nullptr : reinterpret_cast<float *>(dst);
  int16_t *pout = pcm16_ ? reinterpret_cast<int16_t *>(dst) : nullptr;
  for (uint32_t i = 0; i < frames; ++i) {
    float l = src[i * 2], r = src[i * 2 + 1];
    for (int c = 0; c < ch_; ++c) {
      float v = c == 0 ? l : (c == 1 ? r : 0.5f * (l + r));
      if (pcm16_) {
        float s = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        pout[i * ch_ + c] = int16_t(lroundf(s * 32767.0f));
      } else {
        // 直加会超过 1.0，float 端点不钳位就是绕回成爆音。
        fout[i * ch_ + c] = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
      }
    }
  }
}
