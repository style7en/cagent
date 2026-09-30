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
    DWORD b64len = 0;
    CryptBinaryToStringA(out.pbData, out.cbData,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64len);
    char *b64 = (char*)malloc(b64len);
    if (!b64) { LocalFree(out.pbData); return NULL; }
    CryptBinaryToStringA(out.pbData, out.cbData,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64len);
    LocalFree(out.pbData);
    char *res = (char*)malloc(6 + b64len);
    if (!res) { free(b64); return NULL; }
    memcpy(res, "dpapi:", 6);
    memcpy(res + 6, b64, b64len);   /* b64len 含 '\0' */
    free(b64);
    return res;
}

static char *dpapi_unprotect(const char *stored) {
    if (!stored) return NULL;
    if (strncmp(stored, "dpapi:", 6) != 0) {
        return strdup(stored);   /* 明文兼容 */
    }
    const char *b64 = stored + 6;
    DWORD binlen = 0;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, NULL, &binlen, NULL, NULL))
        return NULL;
    BYTE *bin = (BYTE*)malloc(binlen);
    if (!bin) return NULL;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, bin, &binlen, NULL, NULL)) {
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

