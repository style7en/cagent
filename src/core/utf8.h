/*
 * core/utf8.h - UTF-8 解码与字符边界 (项目内唯一实现)
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 位置紧跟 base.h: 本模块只依赖 char/wchar_t, 却被 state/log/encoding/workspace/tools
 * 多处使用, 排在最前才能让大家共用同一份判据。
 *
 * 为什么单独成文件: 此前项目里散着 4 份 UTF-8 判据 (encoding.h 的 is_valid_utf8、
 * workspace.h 的 utf8_trim_len / utf8_skip_cont、log.h 里新加的 utf8_seq_len), 宽严不一 ——
 * 最典型的是"本地预检说合法、服务端照样 400 invalid unicode"(预检用的那份过宽)。
 * 收敛成一份严格实现后, "报告的非法"与"服务端拒收的"才真正对齐。
 */

#ifndef CAGENT_UTF8_H
#define CAGENT_UTF8_H

/* 从 s+i 起的合法 UTF-8 序列长度; 0 = 该位置非法
 * (孤立续字节 / 半截序列 / 过长编码 / 代理区 D800-DFFF / 超出 U+10FFFF)。
 *
 * 严格按 RFC 3629。i+adv>n 视为"被截断"直接判非法 —— 预检与安全截取共用同一判据,
 * 保证"本地报告的非法"与"服务端拒收的"一致。 */
static size_t utf8_seq_len(const char *s, size_t i, size_t n) {
    if (i >= n) return 0;
    unsigned char c = (unsigned char)s[i];
    size_t adv = (c < 0x80) ? 1 :
                 (c >= 0xC2 && c <= 0xDF) ? 2 :      /* C0/C1 一定是过长编码, 直接不认 */
                 (c >= 0xE0 && c <= 0xEF) ? 3 :
                 (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    if (!adv || i + adv > n) return 0;                       /* 半截: 越过截取点即非法 */
    for (size_t k = 1; k < adv; k++)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
    if (adv >= 3) {
        unsigned char b1 = (unsigned char)s[i + 1];
        if (adv == 3 && c == 0xE0 && b1 < 0xA0) return 0;    /* 过长 (E0 80..9F) */
        if (adv == 3 && c == 0xED && b1 >= 0xA0) return 0;   /* 代理区 D800..DFFF */
        if (adv == 4 && c == 0xF0 && b1 < 0x90) return 0;    /* 过长 (F0 80..8F) */
        if (adv == 4 && c == 0xF4 && b1 > 0x8F) return 0;    /* 超出 U+10FFFF (F4 90..BF) */
    }
    return adv;
}

/* 整段严格校验: n 字节内全是完整合法序列才返回 1。
 * 用于"合法则原样保留, 否则按 OEM(GBK) 转码"的分支判定。 */
static int is_valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        size_t adv = utf8_seq_len((const char *)s, i, n);
        if (!adv) return 0;
        i += adv;
    }
    return 1;
}

/* 把 s 的前 n 字节以"只保留完整合法 UTF-8 字符"的方式拷进 out (结果必为合法 UTF-8)。
 * 用于参数预览与日志: %.200s 按字节切割会切半多字节字符, 而非法 UTF-8 一旦作为 tool
 * 结果写进 messages, 此后每次请求都会被服务端 400 invalid unicode 拒掉 (自我毒化)。 */
static void utf8_safe_copy(const char *s, size_t n, char *out, size_t cap) {
    if (cap == 0) return;
    size_t o = 0, i = 0;
    while (s[i] && i < n && o + 1 < cap) {
        size_t adv = utf8_seq_len(s, i, n);
        if (adv && o + adv < cap) {
            memcpy(out + o, s + i, adv);
            o += adv; i += adv;
        } else {
            i++;                            /* 坏字节/半截字符/放不下: 丢该字节继续 */
        }
    }
    out[o] = '\0';
}

/* 把截断长度回退到 UTF-8 字符边界: 避免切出半个字符 (非法 UTF-8 会让整段文本被丢弃)。
 * 注意: 数据本身可能不是 UTF-8 (如 cmd 的 GBK 输出), 无法解析时原样返回 len ——
 * 绝不能因为"不像 UTF-8"就把内容截成 0。因此不能用 utf8_seq_len 一票否决, 要区分
 * "尾部正好被切断"(退到边界) 与 "根本不是 UTF-8"(原样返回)。 */
static size_t utf8_trim_len(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t adv = utf8_seq_len(s, i, len);
        if (adv) { i += adv; continue; }
        unsigned char c = (unsigned char)s[i];
        size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 0;
        if (need && i + need > len) return i;   /* 尾部被截断: 这里就是字符边界 */
        return len;                             /* 非 UTF-8 数据: 不做截断 */
    }
    return i;
}

/* 从 offset 起跳过 UTF-8 续字节 (0b10xxxxxx), 让读取起点落在字符边界。
 * 用于 read_file 的任意字节 offset: 起点切在多字节字符中间时向前对齐。
 * 若整段都是续字节 (理论罕见) 返回 n, 调用方按原样处理。 */
static size_t utf8_skip_cont(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] & 0xC0) == 0x80) i++;
    return i;
}

/* 就地净化: 把 s 里"无法解码"的字节各替换成一个 '?' (0x3F), 返回被替换的字节数。
 *
 * 用 '?' 而不是 U+FFFD 是刻意的: 长度不变。messages 与各处缓冲都按固定容量算
 * (BUFSZ), 就地插 3 字节的 U+FFFD 会把"还剩多少空间"的判断全部推翻。
 * '?' 在 JSON 字符串里无需转义, 所以替换不会破坏消息结构。
 *
 * 用途: ① tool 输出转码失败时的兜底 (oem_to_utf8); ② 历史被写坏后的自愈 ——
 * 坏字节若已在持久化的会话里, 光回滚本轮是救不回来的。 */
static size_t utf8_sanitize_inplace(char *s) {
    size_t n = strlen(s), i = 0, fixed = 0;
    while (i < n) {
        size_t adv = utf8_seq_len(s, i, n);
        if (adv) { i += adv; continue; }
        s[i++] = '?';
        fixed++;
    }
    return fixed;
}

#endif /* CAGENT_UTF8_H */
