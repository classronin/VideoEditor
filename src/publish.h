#pragma once
// 后台线程 → UI 的发布通道。
//
// 线程产出了 UI 要读的东西，就自己说一声；UI 不再靠每个 tick 去「发现」有没有新东西。
// 一个来源一格，消息的 wParam 就是来源。同一来源只允许一条消息在路上（见 main.cpp 的
// ui_publish 定义处），所以高频产出（每帧一贴）不会把队列堆起来；处理消息前先把这一格的
// 账清掉，读期间的新产出就会重新投递 —— 少清一次就是丢一帧画面。
//
// 单独一份头文件的原因：预览核、音频、同步点表都不引 app.h（那会把 UI 的整张状态表
// 拖进 worker 侧，还形成循环包含）。这一格必须只有一处定义 —— 编号写两遍就是
// 「同一数值两处读」那一类违反。
namespace app {

enum PublishSrc {
  kPubPreview = 0,  // 预览 worker 交了一帧
  kPubWave,         // 波形烘焙线程完成一笔（成功或失败都算）
  kPubAudio,        // 音频线程记下了错误
  kPubSync,         // 同步点表 worker 攒下了 trace 行
  kPubCount,
};

// 任何线程可调（含预览、烘焙、下载 worker）。窗口还没建好时丢掉这一格 —— 建好以后
// WM_CREATE 会把每一格主动读一遍，所以丢掉的那一次不会变成「画面永远不刷新」。
void ui_publish(int src);

}  // namespace app
