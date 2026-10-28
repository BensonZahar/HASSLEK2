/*
 * hassle_screen.c — команда /screen [секунд 0–30]: скриншот игры в PNG. Зависит только от ядра (hassle_core.c).
 * КАК УБРАТЬ: удалите hassle_screen.c и пересоберите — остальные моды работают как раньше.
 *
 * Как это работает
 *   • ScreenCapture в этой сборке Unity вырезан, поэтому снимок делаем вручную: Texture2D(w,h) + ReadPixels(весь экран) +
 *     ImageConversion.EncodeToPNG + запись файла обычным open/write.
 *   • ReadPixels из экрана работает только в момент отрисовки кадра, а не из Update. Поэтому /screen ставит заявку, а читаем мы
 *     в нативном обработчике события RenderPipelineManager.endFrameRendering (URP: после отрисовки всех камер кадра). Подписка
 *     делается один раз, при первом /screen; обработчик без заявки ничего не делает (одна проверка флага за кадр).
 *   • Если подписаться не вышло или событие не приходит — снимаем сразу из обработчика команды (кадр может получиться чёрным,
 *     об этом пишем в журнал и в чат).
 *   • Задержка по умолчанию 3 с — чтобы успеть закрыть чат/меню (интерфейс может попасть на снимок). /screen 0 — сразу.
 *   • Папка: <папка журнала HLOG>/shots (обычно /sdcard/Android/data/<пакет>/files/HLOG/shots), файлы screen_ГГГГММДД_ЧЧММСС_N.png
 *     (время UTC, N — номер снимка за запуск). Путь пишется в чат и в konstt.log.
 *   • /screen diag — состояние в чат.
 *
 * Скрин из Telegram (кнопка «📸 Скрин» в меню «Управление» скрипта HassleB)
 *   • Свой крошечный HTTP-сервер на 127.0.0.1:17781..17790 (Near занимает 17771..17780 — не пересекаемся). Скрипт (WebView) ходит
 *     на него обычным XHR, как Near.js. Игровой поток сервер не трогает: только атомарные флаги и чтение файла.
 *       GET /screen/status        JSON: v, seq (номер последнего УДАЧНОГО снимка), fails (сколько снимков не вышло), w, h, bytes,
 *                                 pending, hooked, dead. Скрипт ждёт роста seq (готово) или fails (не вышло).
 *       GET /screen/shot?delay=N  заявка на снимок (по умолчанию 0 с). {"ok":true} | {"ok":false,"err":"busy"|"need_cmd"}.
 *                                 need_cmd — подписка на кадр ещё не сделана (первый раз после запуска игры): скрипт шлёт в чат «/screen 0».
 *       GET /screen/last          PNG последнего удачного снимка (image/png)
 *   • Снимки по HTTP-заявке всегда пишутся в <HLOG>/shots/remote.png (перезаписывается), чтобы удалённый просмотр не копил файлы.
 *     Файл пишется во временный .tmp и переименовывается — читатель никогда не увидит недописанный PNG.
 */
#pragma once
#include "hassle_core.c"

#define SCR_DEF_DELAY_S 3
#define SCR_MAX_DELAY_S 30
#define SCR_MAX_PNG (64 * 1024 * 1024)

static volatile int g_scr_pending;           /* 1 — заявка на снимок, ждём конец кадра */
static volatile long g_scr_due;              /* не раньше этого времени (time()) */
static volatile int g_scr_busy;
static volatile int g_scr_hookDead;          /* 1 — событие кадра так и не пришло: снимаем из команды напрямую */
static int g_scr_hooked, g_scr_hookTried, g_scr_resolved, g_scr_seq, g_scr_modId = -1;
static int g_scr_lastW, g_scr_lastH, g_scr_lastOk;
static char g_scr_dir[320], g_scr_lastPath[400];
static volatile int g_scr_remote;            /* 1 — заявка пришла по HTTP: пишем в shots/remote.png и не шумим в чат игры */
static volatile int g_scr_fails;             /* сколько снимков не вышло */
static volatile int g_scr_doneSeq;           /* номер последнего удачного снимка (0 — ещё не было) */
static volatile long g_scr_lastLen;          /* длина PNG последнего удачного снимка */
static int g_scr_httpPort;
static void *g_scr_kTex, *g_scr_kRPM;
static void *g_scr_mW, *g_scr_mH, *g_scr_mCtor, *g_scr_mRead, *g_scr_mPng, *g_scr_mDestroy;

static int scr_resolve(void) {
    void *core, *icm, *kScr, *kObj, *kIC;
    if (g_scr_resolved) return g_scr_resolved > 0;
    core = find_image_pre("UnityEngine.CoreModule");
    icm = find_image_pre("UnityEngine.ImageConversionModule");
    if (!core || !icm) { dlog("скрин: нет UnityEngine.CoreModule / ImageConversionModule"); g_scr_resolved = -1; return 0; }
    g_scr_kTex = A.class_from_name(core, "UnityEngine", "Texture2D");
    kScr = A.class_from_name(core, "UnityEngine", "Screen");
    kObj = A.class_from_name(core, "UnityEngine", "Object");
    kIC = A.class_from_name(icm, "UnityEngine", "ImageConversion");
    g_scr_kRPM = A.class_from_name(core, "UnityEngine.Rendering", "RenderPipelineManager");
    if (!g_scr_kTex || !kScr || !kObj || !kIC) { dlog("скрин: не найден один из классов Texture2D/Screen/Object/ImageConversion"); g_scr_resolved = -1; return 0; }
    g_scr_mW = A.class_get_method_from_name(kScr, "get_width", 0);
    g_scr_mH = A.class_get_method_from_name(kScr, "get_height", 0);
    g_scr_mCtor = A.class_get_method_from_name(g_scr_kTex, ".ctor", 2);                 /* Texture2D(int width, int height) */
    g_scr_mRead = A.class_get_method_from_name(g_scr_kTex, "ReadPixels", 3);            /* ReadPixels(Rect, int, int) */
    g_scr_mPng = A.class_get_method_from_name(kIC, "EncodeToPNG", 1);
    g_scr_mDestroy = A.class_get_method_from_name(kObj, "Destroy", 1);
    if (!g_scr_mW || !g_scr_mH || !g_scr_mCtor || !g_scr_mRead || !g_scr_mPng) {
        dlog("скрин: не найден один из методов Screen.get_width/height, Texture2D..ctor/ReadPixels, EncodeToPNG");
        g_scr_resolved = -1; return 0;
    }
    g_scr_resolved = 1;
    dlog("скрин: классы и методы Unity найдены");
    return 1;
}

static int scr_get_int(void* m) {
    void *exc = NULL, *r = A.runtime_invoke(m, NULL, NULL, &exc);
    if (exc || !r) return 0;
    return *(int*)((char*)r + HDR);                      /* значение в боксе лежит сразу за заголовком объекта */
}

/* папка снимков = <папка журнала>/shots; возвращает 1, если папка готова */
static int scr_prepare_dir(void) {
    unsigned i, last = 0;
    if (g_scr_dir[0]) return 1;
    dlog("скрин: готовлю папку");                       /* заодно инициализирует файл журнала и g_flog */
    if (!g_flog[0]) return 0;
    for (i = 0; g_flog[i] && i < sizeof g_scr_dir - 8; i++) { g_scr_dir[i] = g_flog[i]; if (g_flog[i] == '/') last = i; }
    g_scr_dir[last] = 0;
    {   /* b_init обнуляет первый байт буфера и стирает путь, поэтому дописываем «/shots» вручную */
        static const char tail[] = "/shots"; unsigned k;
        for (k = 0; tail[k] && last + k + 1 < sizeof g_scr_dir; k++) g_scr_dir[last + k] = tail[k];
        g_scr_dir[last + k] = 0;
        mkdir(g_scr_dir, 0777);
    }
    return 1;
}

/* время UTC из секунд (алгоритм «civil from days») */
static void scr_stamp(long t, char* out) {
    long z = t / 86400, sod = t % 86400, era, doe, yoe, y, doy, mp, d, m;
    int hh = (int)(sod / 3600), mi = (int)(sod % 3600 / 60), ss = (int)(sod % 60);
    Buf b;
    z += 719468; era = z / 146097; doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100); mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1; m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++;
    b_init(&b, out, 32);
    b_int(&b, y); if (m < 10) b_ch(&b, '0'); b_int(&b, m); if (d < 10) b_ch(&b, '0'); b_int(&b, d);
    b_ch(&b, '_'); if (hh < 10) b_ch(&b, '0'); b_int(&b, hh); if (mi < 10) b_ch(&b, '0'); b_int(&b, mi); if (ss < 10) b_ch(&b, '0'); b_int(&b, ss);
}

/* снимок не вышел: считаем (скрипт по этому счётчику не ждёт таймаут) и пишем в чат игры */
static void scr_fail(const char* m) { __atomic_add_fetch(&g_scr_fails, 1, __ATOMIC_RELEASE); chat_say(m); }

/* Сам снимок. Вызывать из главного потока в момент отрисовки (или, как запасной путь, из команды). */
static void scr_capture(void) {
    void *tex = NULL, *exc = NULL, *png; void* a[3]; int w, h, zero = 0, remote, seq; float rect[4];
    char et[256], msg[460], st[32], path[400], tmpath[420]; Buf b; long len, off = 0; const char* data; int fd;
    ensure_attached();
    remote = __atomic_exchange_n(&g_scr_remote, 0, __ATOMIC_ACQ_REL);   /* заявка по HTTP? флаг гасим сразу, даже если снимок не выйдет */
    if (!scr_resolve() || !scr_prepare_dir()) { scr_fail("Скрин: не вышло (нет классов Unity или папки), подробности в konstt.log"); return; }
    w = scr_get_int(g_scr_mW); h = scr_get_int(g_scr_mH);
    if (w < 16 || h < 16 || w > 8192 || h > 8192) { dlog_i("скрин: странный размер экрана, ширина ", w); scr_fail("Скрин: не вышло (размер экрана)"); return; }
    tex = A.object_new(g_scr_kTex);
    a[0] = &w; a[1] = &h;
    A.runtime_invoke(g_scr_mCtor, tex, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("скрин: Texture2D..ctor бросил: ", et); scr_fail("Скрин: не вышло (создание текстуры)"); return; }
    rect[0] = 0; rect[1] = 0; rect[2] = (float)w; rect[3] = (float)h;
    a[0] = rect; a[1] = &zero; a[2] = &zero;
    A.runtime_invoke(g_scr_mRead, tex, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("скрин: ReadPixels бросил: ", et); scr_fail("Скрин: не вышло (чтение экрана)"); goto done; }
    a[0] = tex;
    png = A.runtime_invoke(g_scr_mPng, NULL, a, &exc);
    if (exc || !png) { exc_text(exc, et, sizeof et); dlog2("скрин: EncodeToPNG: ", exc ? et : "вернул null"); scr_fail("Скрин: не вышло (кодирование PNG)"); goto done; }
    len = *(long*)((char*)png + 3 * sizeof(void*));
    if (len <= 0 || len > SCR_MAX_PNG) { dlog_i("скрин: странная длина PNG ", len); scr_fail("Скрин: не вышло (размер PNG)"); goto done; }
    data = (const char*)png + 4 * sizeof(void*);
    scr_stamp(time(NULL), st);
    seq = ++g_scr_seq;
    b_init(&b, path, sizeof path); b_s(&b, g_scr_dir);
    if (remote) b_s(&b, "/remote.png");
    else { b_s(&b, "/screen_"); b_s(&b, st); b_ch(&b, '_'); b_int(&b, seq); b_s(&b, ".png"); }
    b_init(&b, tmpath, sizeof tmpath); b_s(&b, path); b_s(&b, ".tmp");
    fd = open(tmpath, 0x241, 0666);                      /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0) { dlog2("скрин: не открыть файл: ", path); scr_fail("Скрин: не вышло (нет доступа к папке), путь в konstt.log"); goto done; }
    while (off < len) { long n = write(fd, data + off, (unsigned long)(len - off)); if (n <= 0) break; off += n; }
    close(fd);
    if (off < len) { dlog2("скрин: файл записан не целиком: ", path); scr_fail("Скрин: файл записан не целиком"); goto done; }
    if (rename(tmpath, path) != 0) { dlog2("скрин: не переименовать во временного в итоговый: ", path); scr_fail("Скрин: не вышло (переименование файла)"); goto done; }
    g_scr_lastW = w; g_scr_lastH = h;
    { int k; for (k = 0; k < (int)sizeof g_scr_lastPath - 1 && path[k]; k++) g_scr_lastPath[k] = path[k]; g_scr_lastPath[k] = 0; }
    g_scr_lastLen = len; g_scr_lastOk = 1;
    __atomic_store_n(&g_scr_doneSeq, seq, __ATOMIC_RELEASE);          /* последним: после этого HTTP может отдавать файл */
    dlog2("скрин: сохранён ", path); dlog_i("скрин: размер PNG, байт ", len);
    b_init(&b, msg, sizeof msg); b_s(&b, "Скрин сохранён ("); b_int(&b, w); b_ch(&b, 'x'); b_int(&b, h); b_s(&b, ", "); b_int(&b, len / 1024); b_s(&b, " КБ): "); b_s(&b, path);
    if (!remote) chat_say(msg);                           /* по HTTP-заявке чат игры не засоряем */
done:
    if (tex && g_scr_mDestroy) { void* da[1]; void* e2 = NULL; da[0] = tex; A.runtime_invoke(g_scr_mDestroy, NULL, da, &e2); }
}

/* Вызывается движком в главном потоке в конце кадра (аргументы не нужны, поэтому сигнатура делегата нам безразлична). */
static void scr_frame_cb(void* self, void* a, void* b, void* c) {
    (void)self; (void)a; (void)b; (void)c;
    if (!__atomic_load_n(&g_scr_pending, __ATOMIC_ACQUIRE)) return;
    if (time(NULL) < g_scr_due) return;
    if (__sync_lock_test_and_set(&g_scr_busy, 1)) return;
    __atomic_store_n(&g_scr_pending, 0, __ATOMIC_RELEASE);
    scr_capture();
    __sync_lock_release(&g_scr_busy);
}

/* Подписка на RenderPipelineManager.endFrameRendering нативным делегатом (клон образца делегата чат-команд с подменой method_ptr). */
static int scr_hook(void) {
    void *f, *t, *dk, *nd, *m, *exc = NULL, *args[1]; char et[256];
    if (g_scr_hooked) return 1;
    if (g_scr_hookTried) return 0;
    g_scr_hookTried = 1;
    if (!scr_resolve() || !g_scr_kRPM) { dlog("скрин: подписка: нет RenderPipelineManager"); return 0; }
    if (!g_tplDelegate || !g_cmdKls || !g_cmdSize) { dlog("скрин: подписка: нет образца делегата"); return 0; }
    f = A.class_get_field_from_name(g_scr_kRPM, "endFrameRendering");
    t = f ? A.field_get_type(f) : NULL;
    dk = t ? A.class_from_type(t) : NULL;
    m = A.class_get_method_from_name(g_scr_kRPM, "add_endFrameRendering", 1);
    if (!dk || !m) { dlog("скрин: подписка: нет поля endFrameRendering или add_endFrameRendering"); return 0; }
    if (!A.class_instance_size || A.class_instance_size(dk) != g_cmdSize) { dlog("скрин: подписка: размер делегата события не совпал с образцом"); return 0; }
    while (__sync_lock_test_and_set(&g_cmdLock, 1)) usleep(200);
    nd = clone_cmd_delegate(dk, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)scr_frame_cb);
    __sync_lock_release(&g_cmdLock);
    if (!nd) { dlog("скрин: подписка: не создать делегат"); return 0; }
    args[0] = nd;
    A.runtime_invoke(m, NULL, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("скрин: add_endFrameRendering бросил: ", et); return 0; }
    g_scr_hooked = 1;
    dlog("скрин: подписались на endFrameRendering");
    return 1;
}

/* Вызывается игрой в главном потоке при вводе /screen [секунды] */
static void native_screen_cmd(void* self, void* args, void* mi) {
    int diag = 0, t; Buf b; char msg[420];
    (void)self; (void)mi;
    t = parse_cmd_args(args, &diag);
    dlog_i("нативная /screen: вызвана, секунд ", t);
    if (diag) {
        b_init(&b, msg, sizeof msg);
        b_s(&b, "Screen: подписка на кадр: "); b_s(&b, g_scr_hooked ? "да" : "нет");
        b_s(&b, ", событие не приходило: "); b_s(&b, g_scr_hookDead ? "да" : "нет");
        b_s(&b, ", снимков за запуск: "); b_int(&b, g_scr_seq);
        if (g_scr_lastOk) { b_s(&b, ", последний: "); b_s(&b, g_scr_lastPath); }
        b_s(&b, ", HTTP-порт: "); b_int(&b, g_scr_httpPort);
        chat_say(msg);
        return;
    }
    if (t < 0) t = SCR_DEF_DELAY_S;
    if (t > SCR_MAX_DELAY_S) { chat_usage("Используйте: /screen [секунды (0-30)]"); return; }
    if (__atomic_load_n(&g_scr_pending, __ATOMIC_ACQUIRE)) { chat_say("Скрин: уже жду предыдущий снимок"); return; }
    if (!scr_resolve()) { chat_say("Скрин: не найдены нужные классы Unity, подробности в konstt.log"); return; }
    if (!scr_prepare_dir()) { chat_say("Скрин: не удалось подготовить папку, подробности в konstt.log"); return; }
    scr_hook();
    if (!g_scr_hooked || g_scr_hookDead) {
        /* запасной путь: без события кадра. Задержку выдерживаем тут же нельзя (блокировать главный поток не будем) — снимаем сразу */
        chat_say("Скрин: конец кадра недоступен, снимаю сразу (кадр может быть чёрным)");
        dlog("скрин: запасной путь — снимок прямо из команды");
        scr_capture();
        return;
    }
    g_scr_due = time(NULL) + t;
    __atomic_store_n(&g_scr_pending, 1, __ATOMIC_RELEASE);
    b_init(&b, msg, sizeof msg);
    if (t > 0) { b_s(&b, "Скрин через "); b_int(&b, t); b_s(&b, " с — закройте чат, если он мешает"); }
    else b_s(&b, "Скрин: снимаю");
    chat_say(msg);
}


/* ───────────── HTTP для кнопки «Скрин» в Telegram ───────────── */
extern int socket(int, int, int);
extern int setsockopt(int, int, int, const void*, unsigned);
extern int bind(int, const void*, unsigned);
extern int listen(int, int);
extern int accept(int, void*, void*);
extern long recv(int, void*, unsigned long, int);
extern long send(int, const void*, unsigned long, int);
#define SCR_PORT_FIRST 17781
#define SCR_PORT_COUNT 10
struct scr_sa4 { u16 family; u16 port; u32 addr; u8 zero[8]; };
struct scr_tv { long sec; long usec; };

static int scr_send_all(int fd, const char* p, unsigned long n) {
    while (n) {
        long w = send(fd, p, n, 0x4000);                 /* MSG_NOSIGNAL */
        if (w <= 0) return 0;
        p += w; n -= (unsigned long)w;
    }
    return 1;
}
static int scr_send_head(int fd, const char* status, const char* ctype, unsigned long blen) {
    char h[420]; Buf b;
    b_init(&b, h, sizeof h);
    b_s(&b, "HTTP/1.1 "); b_s(&b, status);
    b_s(&b, "\r\nContent-Type: "); b_s(&b, ctype);
    b_s(&b, "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
    b_int(&b, (long)blen);
    b_s(&b, "\r\n\r\n");
    return scr_send_all(fd, h, b.n);
}
static void scr_respond(int fd, const char* status, const char* ctype, const char* body, unsigned long blen) {
    if (scr_send_head(fd, status, ctype, blen) && blen) scr_send_all(fd, body, blen);
}

static void scr_http_status(int c) {
    char js[320]; Buf b;
    b_init(&b, js, sizeof js);
    b_s(&b, "{\"v\":1,\"seq\":"); b_int(&b, __atomic_load_n(&g_scr_doneSeq, __ATOMIC_ACQUIRE));
    b_s(&b, ",\"fails\":"); b_int(&b, __atomic_load_n(&g_scr_fails, __ATOMIC_ACQUIRE));
    b_s(&b, ",\"w\":"); b_int(&b, g_scr_lastW); b_s(&b, ",\"h\":"); b_int(&b, g_scr_lastH);
    b_s(&b, ",\"bytes\":"); b_int(&b, g_scr_lastLen);
    b_s(&b, ",\"pending\":"); b_int(&b, __atomic_load_n(&g_scr_pending, __ATOMIC_ACQUIRE) ? 1 : 0);
    b_s(&b, ",\"hooked\":"); b_int(&b, g_scr_hooked ? 1 : 0);
    b_s(&b, ",\"dead\":"); b_int(&b, g_scr_hookDead ? 1 : 0);
    b_s(&b, "}");
    scr_respond(c, "200 OK", "application/json", js, b.n);
}

/* /screen/shot?delay=N — заявка на снимок. Подписку на кадр (IL2CPP) отсюда не делаем: это только из команды в главном потоке. */
static void scr_http_shot(int c, const char* rq) {
    int delay = 0, i;
    static const char okj[] = "{\"ok\":true}", busy[] = "{\"ok\":false,\"err\":\"busy\"}", need[] = "{\"ok\":false,\"err\":\"need_cmd\"}";
    for (i = 0; rq[i] && rq[i] != '\r' && rq[i] != '\n'; i++) {
        if (rq[i] == 'd' && sprefix(rq + i, "delay=")) {
            int k = i + 6; delay = 0;
            while (rq[k] >= '0' && rq[k] <= '9' && delay < 1000) { delay = delay * 10 + (rq[k] - '0'); k++; }
            break;
        }
    }
    if (delay > SCR_MAX_DELAY_S) delay = SCR_MAX_DELAY_S;
    if (__atomic_load_n(&g_scr_pending, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_scr_busy, __ATOMIC_ACQUIRE)) {
        scr_respond(c, "200 OK", "application/json", busy, sizeof busy - 1); return;
    }
    if (!g_scr_hooked || g_scr_hookDead || g_scr_resolved <= 0 || !g_scr_dir[0]) {
        scr_respond(c, "200 OK", "application/json", need, sizeof need - 1); return;
    }
    g_scr_due = time(NULL) + delay;
    __atomic_store_n(&g_scr_remote, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_scr_pending, 1, __ATOMIC_RELEASE);
    dlog("скрин: заявка по HTTP");
    scr_respond(c, "200 OK", "application/json", okj, sizeof okj - 1);
}

/* /screen/last — PNG последнего удачного снимка. Путь и длину берём копией: следующий снимок может их перезаписать. */
static void scr_http_last(int c) {
    static char buf[65536];
    char path[400]; long len, sent = 0; int fd, k;
    if (__atomic_load_n(&g_scr_doneSeq, __ATOMIC_ACQUIRE) <= 0) { scr_respond(c, "404 Not Found", "text/plain", "none", 4); return; }
    for (k = 0; k < (int)sizeof path - 1 && g_scr_lastPath[k]; k++) path[k] = g_scr_lastPath[k];
    path[k] = 0; len = g_scr_lastLen;
    if (len <= 0 || len > SCR_MAX_PNG) { scr_respond(c, "404 Not Found", "text/plain", "none", 4); return; }
    fd = open(path, 0);                                   /* O_RDONLY */
    if (fd < 0) { scr_respond(c, "404 Not Found", "text/plain", "nofile", 6); return; }
    if (!scr_send_head(c, "200 OK", "image/png", (unsigned long)len)) { close(fd); return; }
    while (sent < len) {
        long want = len - sent < (long)sizeof buf ? len - sent : (long)sizeof buf, n = read(fd, buf, (unsigned long)want);
        if (n <= 0) break;                                /* файл короче заявленного — рвём соединение: XHR получит ошибку, а не битую картинку */
        if (!scr_send_all(c, buf, (unsigned long)n)) break;
        sent += n;
    }
    close(fd);
}

static void* scr_http_main(void* arg) {
    int fd = -1, i, one = 1;
    (void)arg;
    for (;;) {
        for (i = 0; i < SCR_PORT_COUNT && fd < 0; i++) {
            struct scr_sa4 sa; int s = socket(2, 1, 0), z;
            if (s < 0) break;
            setsockopt(s, 1, 2, &one, sizeof one);                /* SOL_SOCKET, SO_REUSEADDR */
            sa.family = 2; sa.port = (u16)(((SCR_PORT_FIRST + i) >> 8) | (((SCR_PORT_FIRST + i) & 255) << 8));
            sa.addr = 0x0100007F;                                 /* 127.0.0.1 */
            for (z = 0; z < 8; z++) sa.zero[z] = 0;
            if (bind(s, &sa, sizeof sa) == 0 && listen(s, 4) == 0) { fd = s; g_scr_httpPort = SCR_PORT_FIRST + i; }
            else close(s);
        }
        if (fd < 0) { usleep(3000000); continue; }
        dlog_i("скрин: HTTP слушает порт ", g_scr_httpPort);
        for (;;) {
            int c = accept(fd, NULL, NULL);
            char rq[512]; long n; struct scr_tv t;
            if (c < 0) { usleep(100000); continue; }
            t.sec = 5; t.usec = 0;
            setsockopt(c, 1, 20, &t, sizeof t);                   /* SO_RCVTIMEO */
            setsockopt(c, 1, 21, &t, sizeof t);                   /* SO_SNDTIMEO */
            n = recv(c, rq, sizeof rq - 1, 0);
            if (n > 0) {
                rq[n] = 0;
                if (sprefix(rq, "OPTIONS ")) scr_respond(c, "204 No Content", "text/plain", "", 0);
                else if (sprefix(rq, "GET /screen/status")) scr_http_status(c);
                else if (sprefix(rq, "GET /screen/shot")) scr_http_shot(c, rq);
                else if (sprefix(rq, "GET /screen/last")) scr_http_last(c);
                else scr_respond(c, "404 Not Found", "text/plain", "no", 2);
            }
            close(c);
        }
    }
    return NULL;
}
static void* scr_http_thread(void* arg) {
    if (!hm_wait_started()) return NULL;                          /* konstt_off — мод выключен целиком */
    return scr_http_main(arg);
}

/* Сторож: если событие кадра не пришло в течение 5 с после срока — помечаем его мёртвым (следующий /screen снимет напрямую). */
static void* scr_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("screen", native_screen_cmd)) return NULL;
    hm_mod_ready(g_scr_modId);
    dlog("скрин: команда /screen работает");
    for (;;) {
        usleep(500000);
        if (__atomic_load_n(&g_scr_pending, __ATOMIC_ACQUIRE) && time(NULL) > g_scr_due + 5) {
            __atomic_store_n(&g_scr_pending, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&g_scr_remote, 0, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_scr_fails, 1, __ATOMIC_RELEASE);   /* скрипт перестаёт ждать этот снимок */
            g_scr_hookDead = 1;
            dlog("скрин: событие endFrameRendering не пришло за 5 с — отключаю его, дальше снимаю напрямую");
        }
    }
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void scr_ctor(void) {
    unsigned long t = 0, t2 = 0;
    g_scr_modId = hm_mod_register("screen");
    pthread_create(&t, NULL, scr_main, NULL);
    pthread_create(&t2, NULL, scr_http_thread, NULL);
}
#endif
