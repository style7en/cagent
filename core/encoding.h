/*
 * core/encoding.h - 编码转换: UTF-8/OEM 检测与转换、字符边界
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

static void oem_to_utf8(char *buf, size_t cap) {
    UINT cp = GetOEMCP();
    if (cp == CP_UTF8 || buf[0] == '\0') return;

    int wlen = MultiByteToWideChar(cp, 0, buf, -1, NULL, 0);
    if (wlen <= 0) return;
    WCHAR *w = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (!w) return;
    MultiByteToWideChar(cp, 0, buf, -1, w, wlen);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8len > 0 && (size_t)u8len <= cap) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
    }
    free(w);
}

/* 严格 UTF-8 校验: 合法则原样保留, 否则按 OEM(GBK) 转换。
   命令输出可能来自本地 cmd (GBK) 或网络 (curl 拿到的 UTF-8), 无法先验, 只能检测。 */
static int is_valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int len; unsigned int cp;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return 0;
        if (i + (size_t)len > n) return 0;
        for (int k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (unsigned)(s[i + k] & 0x3F);
        }
        if (len == 2 && cp < 0x80) return 0;          /* 超长编码 */
        if (len == 3 && cp < 0x800) return 0;
        if (len == 4 && cp < 0x10000) return 0;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += (size_t)len;
    }
    return 1;
}

