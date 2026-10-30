/*
 * hassle_jlog.c — «/jlog»: JS-скрипты (Dress.js и другие) пишут свой журнал в обычный .txt в папке игры, а не в консоль WebView.
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_jlog.c и пересоберите — больше ничего править не нужно
 * (скрипты сами заметят, что мода нет, и вернутся к console.log).
 *
 * Команда (её шлёт скрипт через sendChatInput; до сервера такая строка не доходит — её забирает реестр команд клиента, как /tpl):
 *   /jlog имя текст строки...    дописать строку в HLOG/имя.txt (рядом с konstt.log: Android/data/<пакет>/files/HLOG/)
 *   /jlog _ping                  проверка связи: мод отвечает в чат «JLog: готов …» (скрипт по этому ответу понимает, что мод установлен)
 *
 *   имя — латиница/цифры/«_»/«-», 1..24 символа, без точек и слэшей (в другую папку записать нельзя): Dress.js пишет в dress.txt.
 *   Первая запись нового запуска игры переименовывает прошлый файл в имя_prev.txt (прошлый запуск не теряется), дальше строки дописываются.
 *   Каждая строка пишется сразу (open → write → close): при вылете игры последняя строка остаётся в файле.
 *   Лимит 1.5 МБ на файл за запуск, до 8 разных имён — дальше строки молча отбрасываются (чтобы забытый цикл не забил память телефона).
 *   Строка приходит как string[] (чат режет её по пробелам), мод склеивает слова одним пробелом; поэтому подряд идущие пробелы схлопнутся.
 *   Скрипту стоит резать длинные строки примерно по 120 символов (поле ввода чата длиннее не пропускает).
 */
#pragma once
#include "hassle_core.c"

#define JL_MAX_TAGS 8
#define JL_TAG_MAX 24
#define JL_LINE_MAX 600
#define JL_FILE_MAX 1572864L                                /* 1.5 МБ на файл за запуск */
static char g_jl_tag[JL_MAX_TAGS][JL_TAG_MAX + 1];
static long g_jl_bytes[JL_MAX_TAGS];
static int g_jl_n;
static int g_jl_modId = -1;

/* папка HLOG со слэшем на конце (берём из пути konstt.log; он появляется после первой записи в журнал ядра) */
static int jl_dir(char* out, int cap) {
    unsigned L = slen(g_flog), i;
    if (!g_flog[0] || !ends_with(g_flog, HM_LOGNAME) || L <= HM_LOGNAME_LEN || (int)(L - HM_LOGNAME_LEN) + 1 >= cap) return 0;
    for (i = 0; i < L - HM_LOGNAME_LEN; i++) out[i] = g_flog[i];
    out[i] = 0;
    return 1;
}

/* строка вида «dress»: только [a-z0-9_-] (+ заглавные), 1..JL_TAG_MAX */
static int jl_tag_ok(const char* t) {
    int n = 0;
    for (; t[n]; n++) {
        char c = t[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
        if (n >= JL_TAG_MAX) return 0;
    }
    return n > 0;
}

/* записать строку; 1 — записали */
static int jl_write(const char* tag, const char* text) {
    char dir[300], path[340], prev[340], line[JL_LINE_MAX + 8]; Buf b; int k = -1, i;
    if (!jl_dir(dir, sizeof dir)) return 0;
    for (i = 0; i < g_jl_n; i++) if (seq(g_jl_tag[i], tag)) { k = i; break; }
    b_init(&b, path, sizeof path); b_s(&b, dir); b_s(&b, tag); b_s(&b, ".txt");
    if (k < 0) {                                              /* первая запись под этим именем в этом запуске игры */
        if (g_jl_n >= JL_MAX_TAGS) return 0;
        k = g_jl_n++;
        for (i = 0; tag[i] && i < JL_TAG_MAX; i++) g_jl_tag[k][i] = tag[i];
        g_jl_tag[k][i] = 0; g_jl_bytes[k] = 0;
        b_init(&b, prev, sizeof prev); b_s(&b, dir); b_s(&b, tag); b_s(&b, "_prev.txt");
        rename(path, prev);                                   /* прошлый запуск сохраняем; файла не было — rename просто не сработает */
        flog_write(path, "", 0, 1);                           /* создаём пустой */
    }
    if (g_jl_bytes[k] > JL_FILE_MAX) return 0;
    b_init(&b, line, sizeof line); b_s(&b, text); b_ch(&b, '\n');
    if (!flog_write(path, b.p, b.n, 0)) return 0;
    g_jl_bytes[k] += b.n;
    return 1;
}

/* Вызывается игрой в главном потоке: (this-цель делегата, string[] args, MethodInfo*) */
static void native_jlog_cmd(void* self, void* args, void* mi) {
    long len; int i, haveTag = 0, bad = 0; char* data; char tag[JL_TAG_MAX + 2]; char text[JL_LINE_MAX]; Buf b;
    (void)self; (void)mi;
    if (!args) return;
    if (seq(A.class_get_name(A.object_get_class(args)), "String")) return;   /* не наш формат (тик из Post) */
    len = *(long*)((char*)args + 3 * sizeof(void*));
    if (len <= 0 || len > 200) return;
    data = (char*)args + 4 * sizeof(void*);
    tag[0] = 0;
    b_init(&b, text, sizeof text);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[400]; int n; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || !ch) continue;
        if (n > 380) n = 380;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        if (!haveTag) {
            if (tmp[0] == '/' || seq(tmp, "jlog")) continue;      /* сама команда может входить в массив */
            if (slen(tmp) > JL_TAG_MAX) { bad = 1; break; }
            { int k; for (k = 0; tmp[k]; k++) tag[k] = tmp[k]; tag[k] = 0; }
            haveTag = 1;
            continue;
        }
        if (b.n) b_ch(&b, ' ');
        b_s(&b, tmp);
    }
    if (bad || !haveTag || !jl_tag_ok(tag)) {
        chat_usage("Используйте: /jlog имя текст  (имя — латиница/цифры, файл HLOG/имя.txt); проверка: /jlog _ping");
        return;
    }
    if (seq(tag, "_ping")) {                                  /* ответ в чат — по нему скрипт узнаёт, что мод есть */
        char m[360], dir[300];
        b_init(&b, m, sizeof m); b_s(&b, "JLog: готов");
        if (jl_dir(dir, sizeof dir)) { b_s(&b, ", папка "); b_s(&b, dir); } else b_s(&b, ", но папка журнала ещё не определена");
        chat_say(m);
        return;
    }
    if (!jl_write(tag, text)) dlog2("jlog: не записано в ", tag);
}

static void* jlog_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("jlog", native_jlog_cmd)) return NULL;   /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /jlog */
    dlog("jlog: мод загружен — /jlog имя текст пишет в HLOG/имя.txt");   /* заодно гарантирует, что файловый журнал ядра уже открыт и путь известен */
    hm_mod_ready(g_jl_modId);
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void jlog_ctor(void) {
    unsigned long t = 0;
    g_jl_modId = hm_mod_register("jlog");
    pthread_create(&t, NULL, jlog_main, NULL);
}
#endif
