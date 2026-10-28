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

/* Сам снимок. Вызывать из главного потока в момент отрисовки (или, как запасной путь, из команды). */
static void scr_capture(void) {
    void *tex = NULL, *exc = NULL, *png; void* a[3]; int w, h, zero = 0; float rect[4];
    char et[256], msg[460], st[32], path[400]; Buf b; long len, off = 0; const char* data; int fd;
    ensure_attached();
    if (!scr_resolve() || !scr_prepare_dir()) { chat_say("Скрин: не вышло (нет классов Unity или папки), подробности в konstt.log"); return; }
    w = scr_get_int(g_scr_mW); h = scr_get_int(g_scr_mH);
    if (w < 16 || h < 16 || w > 8192 || h > 8192) { dlog_i("скрин: странный размер экрана, ширина ", w); chat_say("Скрин: не вышло (размер экрана)"); return; }
    tex = A.object_new(g_scr_kTex);
    a[0] = &w; a[1] = &h;
    A.runtime_invoke(g_scr_mCtor, tex, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("скрин: Texture2D..ctor бросил: ", et); chat_say("Скрин: не вышло (создание текстуры)"); return; }
    rect[0] = 0; rect[1] = 0; rect[2] = (float)w; rect[3] = (float)h;
    a[0] = rect; a[1] = &zero; a[2] = &zero;
    A.runtime_invoke(g_scr_mRead, tex, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("скрин: ReadPixels бросил: ", et); chat_say("Скрин: не вышло (чтение экрана)"); goto done; }
    a[0] = tex;
    png = A.runtime_invoke(g_scr_mPng, NULL, a, &exc);
    if (exc || !png) { exc_text(exc, et, sizeof et); dlog2("скрин: EncodeToPNG: ", exc ? et : "вернул null"); chat_say("Скрин: не вышло (кодирование PNG)"); goto done; }
    len = *(long*)((char*)png + 3 * sizeof(void*));
    if (len <= 0 || len > SCR_MAX_PNG) { dlog_i("скрин: странная длина PNG ", len); chat_say("Скрин: не вышло (размер PNG)"); goto done; }
    data = (const char*)png + 4 * sizeof(void*);
    scr_stamp(time(NULL), st);
    b_init(&b, path, sizeof path); b_s(&b, g_scr_dir); b_s(&b, "/screen_"); b_s(&b, st); b_ch(&b, '_'); b_int(&b, ++g_scr_seq); b_s(&b, ".png");
    fd = open(path, 0x241, 0666);                        /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0) { dlog2("скрин: не открыть файл: ", path); chat_say("Скрин: не вышло (нет доступа к папке), путь в konstt.log"); goto done; }
    while (off < len) { long n = write(fd, data + off, (unsigned long)(len - off)); if (n <= 0) break; off += n; }
    close(fd);
    if (off < len) { dlog2("скрин: файл записан не целиком: ", path); chat_say("Скрин: файл записан не целиком"); goto done; }
    g_scr_lastW = w; g_scr_lastH = h; g_scr_lastOk = 1;
    { int k; for (k = 0; k < (int)sizeof g_scr_lastPath - 1 && path[k]; k++) g_scr_lastPath[k] = path[k]; g_scr_lastPath[k] = 0; }
    dlog2("скрин: сохранён ", path); dlog_i("скрин: размер PNG, байт ", len);
    b_init(&b, msg, sizeof msg); b_s(&b, "Скрин сохранён ("); b_int(&b, w); b_ch(&b, 'x'); b_int(&b, h); b_s(&b, ", "); b_int(&b, len / 1024); b_s(&b, " КБ): "); b_s(&b, path);
    chat_say(msg);
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
            g_scr_hookDead = 1;
            dlog("скрин: событие endFrameRendering не пришло за 5 с — отключаю его, дальше снимаю напрямую");
        }
    }
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void scr_ctor(void) {
    unsigned long t = 0;
    g_scr_modId = hm_mod_register("screen");
    pthread_create(&t, NULL, scr_main, NULL);
}
#endif
