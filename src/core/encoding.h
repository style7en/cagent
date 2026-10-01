/*
 * core/encoding.h - 编码转换: UTF-8 / OEM(GBK) 检测与转换
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * UTF-8 的判据与字符边界统一在 core/utf8.h (utf8_seq_len / is_valid_utf8 /
 * utf8_trim_len / utf8_sanitize_inplace), 本文件只保留"判定不是 UTF-8 时怎么转码"。
 */

/* 把 buf 里按当前 OEM 代码页 (中文 Windows 上是 GBK) 编码的内容原地转成 UTF-8。
 *
 * 调用方已用 is_valid_utf8 判过, 这里只处理"确实需要转码"的情况。
 * 关键约束: **返回时 buf 必为合法 UTF-8** —— 旧实现在转码结果超出 cap 时什么都不做
 * 就返回, 把原始 GBK 字节原封不动留在缓冲里; 那串非法字节随后会作为工具结果写进
 * messages, 此后每次请求都被服务端 400 invalid unicode 拒掉, 用户还看不到原因
 * (会话就此永久卡死)。所以三条出口都保证产出合法内容: 正常转码 / 按字符边界截断 /
 * 转不了就地净化。 */
static void oem_to_utf8(char *buf, size_t cap) {
    if (cap == 0) return;
    UINT cp = GetOEMCP();
    if (cp == CP_UTF8 || buf[0] == '\0') return;

    int wlen = MultiByteToWideChar(cp, 0, buf, -1, NULL, 0);
    if (wlen <= 0) { utf8_sanitize_inplace(buf); return; }
    WCHAR *w = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
    if (!w) { utf8_sanitize_inplace(buf); return; }
    if (MultiByteToWideChar(cp, 0, buf, -1, w, wlen) <= 0) {
        free(w);
        utf8_sanitize_inplace(buf);
        return;
    }

    /* 先整段转成 UTF-8 落在临时缓冲: 直接往 buf 里写就没有余量判断的余地,
     * 也不好在字符边界上收尾。 */
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8len <= 1) { free(w); utf8_sanitize_inplace(buf); return; }
    char *u8 = (char*)malloc((size_t)u8len);
    if (!u8) { free(w); utf8_sanitize_inplace(buf); return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, u8len, NULL, NULL);
    free(w);

    size_t n = (size_t)u8len - 1;                          /* 去掉结尾 NUL */
    if (n > cap - 1) n = utf8_trim_len(u8, cap - 1);       /* 放不下: 截到完整字符边界 */
    memcpy(buf, u8, n);
    buf[n] = '\0';
    free(u8);
}
