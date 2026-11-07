#pragma once
// 「默认」菜单 = 保存/恢复启动默认。
// 文件里存的是「怎么开机」：窗口状态（放大/还原 + 还原态矩形）、导出格式参数
// （分辨率X/Y、宽高比X/Y、帧率、质量 CRF、无音频）、XY 成对锁、界面开关（预览网格）
// 与面板布局（整块面板的折叠 + 每一组标题左边的小三角 + 两条分隔条 + 面板在哪一边）。
// 条带、时间线内容、变换与关键帧、裁切、速度、帧范围一律不进。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace app {
struct App;

// %LOCALAPPDATA%\VSE\settings.txt；拿不到本地目录时返回空串。
std::wstring settings_path();
// 启动时（建窗口之前）调用：文件不存在或某行读坏都不出声，那一项就保持出厂值。
void settings_load(App &a);
// 「保存启动默认」：把当前窗口的还原态矩形 + 是否最大化 + 格式参数写进文件。
void settings_save(App &a);
// 关窗口时调用：把 layout 行的折叠位与 panel 行的六颗小三角换成此刻的状态，文件里其余每一行
// 原样保留（所以这不是「保存启动默认」，也不会把没点过的参数偷偷顶上去）。
void settings_save_collapse(App &a);
// 「恢复启动默认」= 删掉文件，下次启动回到出厂默认（本次会话不受影响）。
void settings_reset(App &a);

}  // namespace app
