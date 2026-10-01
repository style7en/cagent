/*
 * cagent_core.h - cagent 平台无关核心 (零依赖, Windows 平台)
 *
 * 本文件只做聚合: 按依赖顺序包含 core/ 下的各子模块。
 * 全部实现为 static, 仍是单编译单元, 无需改动构建方式。
 *
 * 子模块 (按包含顺序, 即依赖顺序):
 *   version   版本号单一事实来源 (RC / --version / 标题栏引用)
 *   base      平台头 / 角色常量 / 宿主钩子 / 输出封装
 *   utf8      UTF-8 解码与字符边界 (全项目唯一实现)
 *   state     缓冲常量 / 全局状态 / 工具声明
 *   log       运行日志 / 失败请求留档 / 发送前 UTF-8 预检
 *   crypto    DPAPI: API Key 加密存储
 *   json      轻量 JSON 解析器 / 字符串转义 / 自测
 *   exec      命令执行: CreateProcess + 匿名管道 + 超时
 *   net       WinHTTP 与 SSE 流式响应解析
 *   encoding  OEM(GBK) -> UTF-8 转码
 *   workspace 工作目录解析、越界限制、UTF-8 文件/路径
 *   tools     结构化工具: execute_bash / read_file / write_file / edit_file
 *   agent     Agent 循环: LLM <-> 工具调用、取消与回滚
 *   session   会话持久化: 存取 / 回放 / 多会话命名
 *   config    ini 配置读写 (含 DPAPI Key)
 *   (回归测试套件不在此聚合, 独立在 test/, 见文件末尾说明)
 *
 * 与界面解耦: 通过一组宿主钩子 (cagent_emit / cagent_on_done / cagent_read_config_ui)
 * 与前端交互, 因此 Win32 GUI 复用同一段 Agent 循环。
 *
 * 设计取向 (对齐 pi.dev 的极简理念): 仅 4 个工具 (bash/read/write/edit_file), 无权限弹窗,
 * 无轮次上限, 由模型自行决定何时停止, 用户随时可用停止按钮取消。
 *
 * 使用方式: 前端 .c 文件 #include 本头文件, 并在调用 agent_thread / agent_turn
 * 之前设置钩子; 钩子未设置时采用安全默认 (不读 UI 配置)。
 */

#ifndef CAGENT_CORE_H
#define CAGENT_CORE_H

#include "core/version.h"   /* 版本号单一事实来源, 供 RC/--version/标题栏引用 */
#include "core/base.h"
#include "core/utf8.h"     /* UTF-8 判据的唯一实现: log/encoding/workspace/tools 共用 */
#include "core/state.h"
#include "core/log.h"      /* 运行日志: 错误复现取证 (log\ 目录), 开关由 config 控制 */
#include "core/crypto.h"
#include "core/json.h"
#include "core/exec.h"
#include "core/net.h"
#include "core/encoding.h"
#include "core/workspace.h"
#include "core/tools.h"
#include "core/agent.h"
#include "core/session.h"
#include "core/config.h"

/* 注: 回归测试套件在仓库根的 test/, 刻意**不**在这里聚合 —— 核心不该背测试代码,
 * 产品二进制也不该。只有测试入口 (test/main.c) 会 include 它。 */

#endif /* CAGENT_CORE_H */
