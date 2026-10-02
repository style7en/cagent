/*
 * main_gui.c - Win32 GUI 前端 (界面 + 宿主钩子)
 *
 * 本文件只做聚合: 按依赖顺序包含 ui/ 下的各界面子模块。
 * 共用 cagent_core.h 中的 Agent 循环 / 工具 / 网络 / JSON 解析。
 *
 * 子模块 (按包含顺序, 即依赖顺序):
 *   state       控件 ID / 自定义消息 / 全局句柄与字体
 *   dpi         高 DPI: per-monitor v2 感知 + 统一缩放 dp()/dp_at() (必须排在布局模块之前)
 *   hooks       宿主钩子实现: 输出投递 / 一轮结束 / 配置同步
 *   helpers     UI 辅助: Edit 读写(UTF-8) 与 RichEdit 分角色追加
 *   task        启动一轮 Agent: 配置校验 / 工作目录 / 线程派发
 *   wndproc     布局 / 输入框子类 / 主窗口过程 / 历史框子类
 *   session_dlg 会话选择对话框: 枚举 / 自绘列表 / 载入
 *   about       关于对话框: 一句话功能 / 版本 / 两个项目地址 (文案取自 version.h)
 *   main        程序入口: 注册窗口类 / 图标 / 消息循环
 *
 * 通过钩子把核心与界面解耦:
 *   cagent_emit            -> PostMessage 追加到历史框
 *   cagent_on_done         -> 一轮结束, 恢复 UI
 *   cagent_read_config_ui  -> 退出时从 Edit 同步配置到全局
 *
 * 编译: 见 Makefile (make)
 */

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include "cagent_core.h"

#include "ui/state.h"
#include "ui/dpi.h"          /* dp() 被下面的布局模块使用, 必须排在其前 */
#include "ui/hooks.h"
#include "ui/helpers.h"
#include "ui/task.h"
#include "ui/wndproc.h"
#include "ui/session_dlg.h"
#include "ui/about.h"
#include "ui/main.h"
