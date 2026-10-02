/*
 * core/version.h - 版本号与项目标识的单一事实来源
 *
 * 改动版本号/对外文案只需修改本文件。其余位置 (app.rc / --version / 标题栏 /
 * "关于" 对话框) 都通过宏引用这里, 不重复硬编码。
 */

#ifndef CAGENT_VERSION_H
#define CAGENT_VERSION_H

#define CAGENT_VER_MAJOR 1
#define CAGENT_VER_MINOR 1
#define CAGENT_VER_PATCH 2
#define CAGENT_VER_BUILD 0      /* 构建序号, 非正式发布可保持 0 */

/* 人类可读版本串 (窄字符串); 标题栏用的宽串由它派生, 不要另写一份 */
#define CAGENT_VERSION_STR "1.1.2"

/* 一句话定位 (窄字符串): "关于" 对话框等需要对外介绍的地方共用 */
#define CAGENT_TAGLINE     "用 C 语言实现的极简 AI 编程 Agent, 零第三方依赖"

/* 项目主页 ("关于" 对话框里展示, 便于用户找到源码与更新)。
 * 两个远端互为镜像, 国内访问 gitee 更稳, 所以两个都列出来。 */
#define CAGENT_PROJECT_URL       "https://github.com/style7en/cagent"
#define CAGENT_PROJECT_URL_GITEE "https://gitee.com/style7en/cagent"

/* 由窄串派生宽串 (L"1.1.2"), 避免维护两份字面量 */
#define CAGENT_WIDE_(s) L##s
#define CAGENT_WIDE(s)  CAGENT_WIDE_(s)
#define CAGENT_VERSION_WSTR CAGENT_WIDE(CAGENT_VERSION_STR)

#endif /* CAGENT_VERSION_H */
