/*
 * test/main.c - 控制台回归测试入口 (make test)
 *
 * 直接复用 src/cagent_core.h 中的全部 static 实现, 不依赖 GUI。
 * 退出码 = run_all_tests() 的失败数, 供 make 判定通过/失败。
 *
 * 包含顺序不能换: test.h 里的用例直接调用核心的 static 函数, 必须先有 cagent_core.h。
 */

#include "cagent_core.h"   /* 由 -Isrc 提供 (见 Makefile) */
#include "test.h"          /* 同目录: 测试套件本体 */

int main(void) {
    return run_all_tests();
}
