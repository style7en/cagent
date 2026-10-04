/*
 * core/config.h - ini 配置读写 (含 DPAPI Key)
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* 简单 key=value 解析器 */
static CAGENT_MAYBE_UNUSED void config_load(void) {
    g_log_enabled = 1;   /* 运行日志默认开 (ini 里 log=0 可关); 测试不进本函数故不落盘 */
    char path[MAX_PATH];
    get_ini_path(path, sizeof(path));
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;

        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r')) {
            val[--vlen] = '\0';
        }

        if (strcmp(key, "url_base") == 0)
            snprintf(g_api_url, sizeof(g_api_url), "%s", val);
        else if (strcmp(key, "api_key") == 0) {
            char *dec = dpapi_unprotect(val);
            if (dec) {
                snprintf(g_api_key, sizeof(g_api_key), "%s", dec);
                free(dec);
            } else {
                g_api_key[0] = '\0';
                g_key_decrypt_failed = 1;
            }
        }
        else if (strcmp(key, "model") == 0)
            snprintf(g_model, sizeof(g_model), "%s", val);
        else if (strcmp(key, "max_tokens") == 0)
            g_max_tokens = atoi(val);   /* 单次回复输出预算 (token); 默认见 state.h, 0=不下发 */
        else if (strcmp(key, "skip_cert_verify") == 0)
            g_skip_cert_verify = (atoi(val) != 0);
        else if (strcmp(key, "context_tokens") == 0)
            g_context_tokens = atol(val);   /* 模型上下文窗口 (token), 0=未知则按字节水位 */
        else if (strcmp(key, "workspace") == 0)
            snprintf(g_workspace, sizeof(g_workspace), "%s", val);
        else if (strcmp(key, "skills_dir") == 0)
            snprintf(g_skills_dir, sizeof(g_skills_dir), "%s", val);
        else if (strcmp(key, "last_session") == 0)
            snprintf(g_last_session, sizeof(g_last_session), "%s", val);
        else if (strcmp(key, "log") == 0)
            g_log_enabled = (atoi(val) != 0);   /* 0 = 关闭运行日志 */
    }
    fclose(f);
}

/* 把当前全局配置写回 ini (若 cagent_read_config_ui 已设置, 先从中同步)。 */
static CAGENT_MAYBE_UNUSED void config_save(void) {
    if (cagent_read_config_ui) cagent_read_config_ui();

    /* 三个全空就不写,避免覆盖出"空文件" */
    if (!g_api_url[0] && !g_api_key[0] && !g_model[0]) return;

    char path[MAX_PATH];
    get_ini_path(path, sizeof(path));
    FILE *f = fopen_utf8(path, "wb");
    if (!f) return;
    /* 文件头注释块: 集中说明各配置项, 顺序与下方配置项保持一致 */
    fprintf(f, "# ============================================================\r\n");
    fprintf(f, "# cagent 配置文件\r\n");
    fprintf(f, "# 格式: UTF-8（无 BOM），启动时自动读取，退出时自动保存\r\n");
    fprintf(f, "# 注释行: 以 # 或 ; 开头的行会被忽略，可任意添加说明\r\n");
    fprintf(f, "# ============================================================\r\n");
    fprintf(f, "#\r\n");
    fprintf(f, "# 配置项说明：\r\n");
    fprintf(f, "#\r\n");
    fprintf(f, "#   url_base          API 基础 URL，实际请求拼接为 <url_base>/chat/completions\r\n");
    fprintf(f, "#   api_key           API 鉴权密钥（DPAPI 加密串），请勿公开分享包含此值的文件\r\n");
    fprintf(f, "#   model             模型名称，需与 API 支持的模型一致\r\n");
    fprintf(f, "#   workspace         工作目录，文件工具（读/写/编辑）限制在此目录内；留空默认 exe 目录\r\n");
    fprintf(f, "#   context_tokens    模型上下文窗口大小（token），用于裁剪超长历史；\r\n");
    fprintf(f, "#                     0 = 未知，退化为按字节水位估算，并非不限制\r\n");
    fprintf(f, "#   max_tokens        单次回复输出预算（token），0 = 不下发该字段，由服务端决定\r\n");
    fprintf(f, "#   skills_dir        技能根目录，可用 ; 分隔多个；留空默认 <exe>\\skills\r\n");
    fprintf(f, "#   skip_cert_verify  1 = 跳过 SSL 证书校验（仅自签/调试用），0 = 正常校验\r\n");
    fprintf(f, "#   last_session      最近使用的会话文件，由程序自动维护，通常无需手动修改\r\n");
    fprintf(f, "#   log               运行日志开关，1 = 开启（默认），0 = 关闭\r\n");
    fprintf(f, "#\r\n");
    fprintf(f, "# ============================================================\r\n");
    fprintf(f, "# 提示:\r\n");
    fprintf(f, "#   - 修改本文件前请先退出 cagent，否则程序退出时会用内存中的配置覆盖本文件。\r\n");
    fprintf(f, "#   - 如遇问题，可删除本文件后重新启动，程序将自动生成默认配置。\r\n");
    fprintf(f, "# ============================================================\r\n");
    fprintf(f, "\r\n");

    /* 配置项: 顺序须与上方注释块保持一致 */
    fprintf(f, "url_base=%s\r\n", g_api_url);
    {
        char *enc = dpapi_protect(g_api_key);
        if (enc) { fprintf(f, "api_key=%s\r\n", enc); free(enc); }
    }
    fprintf(f, "model=%s\r\n",    g_model);
    fprintf(f, "workspace=%s\r\n", g_workspace);
    fprintf(f, "context_tokens=%ld\r\n", g_context_tokens);
    fprintf(f, "max_tokens=%d\r\n", g_max_tokens);
    fprintf(f, "skills_dir=%s\r\n", g_skills_dir);
    fprintf(f, "skip_cert_verify=%d\r\n", g_skip_cert_verify);
    fprintf(f, "last_session=%s\r\n", g_last_session);
    fprintf(f, "log=%d\r\n", g_log_enabled);
    fclose(f);
}
