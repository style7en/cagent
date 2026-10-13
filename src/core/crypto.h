/*
 * core/crypto.h - DPAPI: API Key 加密存储
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== DPAPI Key 加密 =====
 * 加密: 明文 -> "dpapi:<base64>"。解密: "dpapi:<base64>" 或明文 -> 明文。
 * 失败返回 NULL。返回值 malloc, 调用者 free。 */
static char *dpapi_protect(const char *plain) {
    if (!plain) return NULL;
    DATA_BLOB in = { (DWORD)strlen(plain), (BYTE*)plain };
    DATA_BLOB out = {0};
    if (!CryptProtectData(&in, NULL, NULL, NULL, NULL, 0, &out)) return NULL;

    /* CryptBinaryToStringA 两次调用的返回值语义**不一致**, 这里必须小心 (issue #3 的根因):
     *   - 尺寸查询 (pszString == NULL): 返回的长度**含**结尾 NUL;
     *   - 实际写入           : 返回的是写入的**字符数, 不含** NUL。
     * 实测同一段 328 字符的 base64: 查询得 329, 写入后得 328 —— 差 1。
     * 旧代码把同一个 b64len 当成"含 NUL"来用, 于是 malloc(6 + 328) 只拿到 334 字节,
     * 却要装 6 + 328 = 334 个字符: 结尾 NUL 无处安放, 返回的字符串**没有终止符**。
     * 后果是 config_save() 里 fprintf("%s") 一路读过堆尾, 把后面的垃圾字节也写进
     * cagent.ini —— 就是用户看到的"api_key 那行 base64 末尾后有若干非 base64 字节"。
     * 现在不依赖 API 报哪个长度: 统一多留 1 字节, 写完再显式补一个 NUL。 */
    DWORD b64len = 0;
    if (!CryptBinaryToStringA(out.pbData, out.cbData,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64len)) {
        LocalFree(out.pbData); return NULL;
    }
    char *b64 = (char*)malloc((size_t)b64len + 1);   /* +1: 两种语义下都够放下 NUL */
    if (!b64) { LocalFree(out.pbData); return NULL; }
    DWORD written = b64len;
    if (!CryptBinaryToStringA(out.pbData, out.cbData,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &written)) {
        free(b64); LocalFree(out.pbData); return NULL;
    }
    if (written > b64len) written = b64len;          /* 防御: 别越界写 */
    b64[written] = '\0';                             /* 无论 API 是否计入 NUL, 都显式收尾 */
    LocalFree(out.pbData);

    size_t n = strlen(b64);                          /* 真实字符数(不含 NUL) */
    char *res = (char*)malloc(6 + n + 1);            /* +1: 给结尾 NUL 留位 */
    if (!res) { free(b64); return NULL; }
    memcpy(res, "dpapi:", 6);
    memcpy(res + 6, b64, n + 1);                     /* 连 NUL 一起拷 */
    free(b64);
    return res;
}

static char *dpapi_unprotect(const char *stored) {
    if (!stored) return NULL;
    if (strncmp(stored, "dpapi:", 6) != 0) {
        return strdup(stored);   /* 明文兼容 */
    }
    /* 只取 base64 正文再解码: 旧版 dpapi_protect 曾把堆尾垃圾写进 ini(见上方注释), 已存在
     * 的 cagent.ini 里 base64 后面可能挂着字节。**不能只按"首个非法字符"截** —— 实测垃圾
     * 常以合法 base64 字符开头(如 `=rIP7`), 所以还要按 base64 语法在**填充位**处收尾:
     * '=' 只允许出现在末尾且最多两个, 其后一律视为垃圾。合法 base64 串里 '=' 也满足这条,
     * 故对正常值零影响, 只是让老文件也能读回。 */
    const char *b64 = stored + 6;
    size_t b64n = 0;
    int pad = 0;                          /* 已见到的 '=' 个数 */
    while (b64[b64n]) {
        char c = b64[b64n];
        if (c == '=') {
            if (pad >= 2) break;          /* 超过两个 '=' -> 已进入垃圾区 */
            pad++; b64n++; continue;
        }
        if (pad) break;                   /* 填充之后又出现数据字符 -> 垃圾 */
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) break;
        b64n++;
    }
    DWORD binlen = 0;
    if (!CryptStringToBinaryA(b64, (DWORD)b64n, CRYPT_STRING_BASE64, NULL, &binlen, NULL, NULL))
        return NULL;
    BYTE *bin = (BYTE*)malloc(binlen ? binlen : 1);
    if (!bin) return NULL;
    if (!CryptStringToBinaryA(b64, (DWORD)b64n, CRYPT_STRING_BASE64, bin, &binlen, NULL, NULL)) {
        free(bin); return NULL;
    }
    DATA_BLOB in = { binlen, bin };
    DATA_BLOB out = {0};
    BOOL ok = CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out);
    free(bin);
    if (!ok) return NULL;
    char *res = (char*)malloc(out.cbData + 1);
    if (!res) { LocalFree(out.pbData); return NULL; }
    memcpy(res, out.pbData, out.cbData);
    res[out.cbData] = '\0';
    LocalFree(out.pbData);
    return res;
}

