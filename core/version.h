/*
 * core/version.h - 版本号单一事实来源
 *
 * 改动版本号只需修改本文件。其余位置 (app.rc / --version / 标题栏)
 * 都通过宏引用这里, 不重复硬编码。
 */

#ifndef CAGENT_VERSION_H
#define CAGENT_VERSION_H

#define CAGENT_VER_MAJOR 1
#define CAGENT_VER_MINOR 1
#define CAGENT_VER_PATCH 1
#define CAGENT_VER_BUILD 0      /* 构建序号, 非正式发布可保持 0 */

/* 人类可读版本串 (窄字符串); 标题栏用的宽串由它派生, 不要另写一份 */
#define CAGENT_VERSION_STR "1.1.1"

/* 由窄串派生宽串: L"1.0.0", 避免维护两份字面量 */
#define CAGENT_WIDE_(s) L##s
#define CAGENT_WIDE(s)  CAGENT_WIDE_(s)
#define CAGENT_VERSION_WSTR CAGENT_WIDE(CAGENT_VERSION_STR)

#endif /* CAGENT_VERSION_H */
