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
 *       GET /screen/status        JSON: v, cap (2 — умеет Live: m=, kb=, live=1; нет поля — старая сборка), seq (номер последнего УДАЧНОГО снимка), fails (сколько снимков не вышло), w, h, bytes,
 *                                 pending, hooked, dead. Скрипт ждёт роста seq (готово) или fails (не вышло).
 *       GET /screen/shot?delay=N  заявка на снимок (по умолчанию 0 с). {"ok":true} | {"ok":false,"err":"busy"|"need_cmd"}.
 *                                 need_cmd — подписка на кадр ещё не сделана (первый раз после запуска игры): скрипт шлёт в чат «/screen 0».
 *       GET /screen/last          PNG последнего удачного снимка (image/png)
 *       GET /screen/tg?k=&c=&doc=&cap=  мод сам шлёт последний снимок в Telegram (JNI → HttpURLConnection): {"ok":true,"id":N}
 *                                 Live: + &m=<message_id> — заменить картинку в том сообщении (editMessageMedia) вместо нового фото,
 *                                 + &kb=<reply_markup JSON, urlencoded, только ASCII> — кнопки под фото; /screen/shot?live=1 — кадр на холст SCR_LIVE_W x SCR_LIVE_H.
 *       GET /screen/tgstatus      {"id","state":1 идёт|2 готово|3 ошибка,"mid" (message_id),"code","msg"} — результат отправки
 *                                 (в WebView игры XHR не умеет бинарные POST, поэтому отправка — на стороне мода)
 *   • Снимки по HTTP-заявке всегда пишутся в <HLOG>/shots/remote.png (перезаписывается), чтобы удалённый просмотр не копил файлы.
 *     Файл пишется во временный .tmp и переименовывается — читатель никогда не увидит недописанный PNG.
 */
#pragma once
#include "hassle_core.c"

/* Единый размер снимка.
 *   Размер кадра = размер ОКНА игры (Screen.width/height). В разделённом экране эмулятора (MEmu) окно узкое, и снимок раньше
 *   получался таким же узким (например 633x720). Теперь любой кадр вписывается в холст SCR_OUT_W x SCR_OUT_H с сохранением пропорций:
 *   кадр меньше холста — растягивается (билинейно), больше — сжимается (усреднением), свободное место — чёрные поля.
 *   Раскладку интерфейса это не меняет: HUD игра рисует под текущую форму окна, «как в полноэкранном режиме» её не вернуть.
 *   SCR_NORMALIZE 0 — вернуть прежнее поведение (снимок в размере окна). */
#define SCR_NORMALIZE 1
#define SCR_OUT_W 1280
#define SCR_OUT_H 720
/* Холст для Live-режима в Telegram: кадр меньше — PNG легче, загрузка и кодирование быстрее */
#define SCR_LIVE_W 854
#define SCR_LIVE_H 480

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
static volatile int g_scr_remoteLive;        /* 1 — заявка по HTTP с live=1: снимаем на холст SCR_LIVE_W x SCR_LIVE_H */
static void *g_scr_kTex, *g_scr_kRPM;
static void *g_scr_mW, *g_scr_mH, *g_scr_mCtor, *g_scr_mRead, *g_scr_mPng, *g_scr_mDestroy;
static void *g_scr_mGetPx, *g_scr_mSetPx;      /* GetPixels32() / SetPixels32(Color32[]) — для приведения к единому размеру (не обязательны) */

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
    g_scr_mGetPx = A.class_get_method_from_name(g_scr_kTex, "GetPixels32", 0);
    g_scr_mSetPx = A.class_get_method_from_name(g_scr_kTex, "SetPixels32", 1);
    if (!g_scr_mGetPx || !g_scr_mSetPx) dlog("скрин: нет GetPixels32/SetPixels32 — снимок будет в размере окна");
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

/* Приводит снимок src (w x h) к холсту ow x oh (SCR_OUT_W x SCR_OUT_H или SCR_LIVE_W x SCR_LIVE_H): вписывает с сохранением пропорций, поля чёрные.
   Возвращает новую текстуру (её надо уничтожить) или NULL — тогда используется исходная. */
static void* scr_normalize(void* src, int w, int h, int ow, int oh) {
    void *dst, *exc = NULL, *sa, *da; void* a[1]; int dw, dh, ox, oy, x, y; long i, sn, dn, hdr = 4 * (long)sizeof(void*);
    float s, sw, sh; const u8* sp; u8* dp; void* ca[2];
    if (!SCR_NORMALIZE || !g_scr_mGetPx || !g_scr_mSetPx) return NULL;
    if (w == ow && h == oh) return NULL;
    sa = A.runtime_invoke(g_scr_mGetPx, src, NULL, &exc);
    if (exc || !sa) { dlog("скрин: GetPixels32 источника не вышел — оставляю размер окна"); return NULL; }
    sn = *(long*)((char*)sa + 3 * sizeof(void*));
    if (sn < (long)w * h) { dlog_i("скрин: странная длина Color32[] источника ", sn); return NULL; }
    dst = A.object_new(g_scr_kTex);
    ca[0] = &ow; ca[1] = &oh;
    A.runtime_invoke(g_scr_mCtor, dst, ca, &exc);
    if (exc) { dlog("скрин: Texture2D для холста не создалась — оставляю размер окна"); return NULL; }
    da = A.runtime_invoke(g_scr_mGetPx, dst, NULL, &exc);
    dn = da ? *(long*)((char*)da + 3 * sizeof(void*)) : 0;
    if (exc || !da || dn < (long)ow * oh) { dlog("скрин: GetPixels32 холста не вышел — оставляю размер окна"); goto fail; }
    sp = (const u8*)sa + hdr; dp = (u8*)da + hdr;
    for (i = 0; i < (long)ow * oh; i++) { dp[4 * i] = 0; dp[4 * i + 1] = 0; dp[4 * i + 2] = 0; dp[4 * i + 3] = 255; }
    sw = (float)ow / (float)w; sh = (float)oh / (float)h; s = sw < sh ? sw : sh;
    dw = (int)((float)w * s + 0.5f); dh = (int)((float)h * s + 0.5f);
    if (dw > ow) dw = ow; if (dh > oh) dh = oh; if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    ox = (ow - dw) / 2; oy = (oh - dh) / 2;
    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            u8* o = dp + 4 * ((long)(oy + y) * ow + (ox + x));
            if (s < 1.0f) {                                  /* уменьшение — среднее по блоку исходных пикселей */
                int x0 = (int)((long)x * w / dw), x1 = (int)((long)(x + 1) * w / dw), y0 = (int)((long)y * h / dh), y1 = (int)((long)(y + 1) * h / dh), xx, yy;
                unsigned long r = 0, g = 0, b = 0, n = 0;
                if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
                for (yy = y0; yy < y1; yy++) for (xx = x0; xx < x1; xx++) { const u8* p = sp + 4 * ((long)yy * w + xx); r += p[0]; g += p[1]; b += p[2]; n++; }
                if (!n) n = 1;
                o[0] = (u8)(r / n); o[1] = (u8)(g / n); o[2] = (u8)(b / n);
            } else {                                         /* увеличение — билинейная выборка */
                float fx = ((float)x + 0.5f) * (float)w / (float)dw - 0.5f, fy = ((float)y + 0.5f) * (float)h / (float)dh - 0.5f, tx, ty;
                int ix, iy, ix1, iy1, k;
                if (fx < 0) fx = 0; if (fy < 0) fy = 0;
                ix = (int)fx; iy = (int)fy; tx = fx - (float)ix; ty = fy - (float)iy;
                ix1 = ix + 1 < w ? ix + 1 : w - 1; iy1 = iy + 1 < h ? iy + 1 : h - 1;
                if (ix > w - 1) ix = w - 1; if (iy > h - 1) iy = h - 1;
                for (k = 0; k < 3; k++) {
                    float p00 = sp[4 * ((long)iy * w + ix) + k], p10 = sp[4 * ((long)iy * w + ix1) + k];
                    float p01 = sp[4 * ((long)iy1 * w + ix) + k], p11 = sp[4 * ((long)iy1 * w + ix1) + k];
                    float v = (p00 * (1 - tx) + p10 * tx) * (1 - ty) + (p01 * (1 - tx) + p11 * tx) * ty;
                    o[k] = (u8)(v + 0.5f);
                }
            }
        }
    }
    a[0] = da; exc = NULL;
    A.runtime_invoke(g_scr_mSetPx, dst, a, &exc);
    if (exc) { dlog("скрин: SetPixels32 холста бросил — оставляю размер окна"); goto fail; }
    dlog_i("скрин: приведён к холсту, ширина окна была ", w); dlog_i("скрин: высота окна была ", h);
    return dst;
fail:
    if (g_scr_mDestroy) { void* da2[1]; void* e2 = NULL; da2[0] = dst; A.runtime_invoke(g_scr_mDestroy, NULL, da2, &e2); }
    return NULL;
}

/* Сам снимок. Вызывать из главного потока в момент отрисовки (или, как запасной путь, из команды). */
static void scr_capture(void) {
    void *tex = NULL, *tex2 = NULL, *exc = NULL, *png; void* a[3]; int w, h, ww, wh, cw, ch, zero = 0, remote, live, seq; float rect[4];
    char et[256], msg[460], st[32], path[400], tmpath[420]; Buf b; long len, off = 0; const char* data; int fd;
    ensure_attached();
    remote = __atomic_exchange_n(&g_scr_remote, 0, __ATOMIC_ACQ_REL);
    live = __atomic_exchange_n(&g_scr_remoteLive, 0, __ATOMIC_ACQ_REL);
    cw = live ? SCR_LIVE_W : SCR_OUT_W; ch = live ? SCR_LIVE_H : SCR_OUT_H;   /* заявка по HTTP? флаг гасим сразу, даже если снимок не выйдет */
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
    ww = w; wh = h;                                       /* размер окна игры */
    tex2 = scr_normalize(tex, w, h, cw, ch);              /* единый размер: вписываем в холст cw x ch */
    if (tex2) { w = cw; h = ch; }
    a[0] = tex2 ? tex2 : tex;
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
    b_init(&b, msg, sizeof msg); b_s(&b, "Скрин сохранён ("); if (tex2) { b_s(&b, "окно "); b_int(&b, ww); b_ch(&b, 'x'); b_int(&b, wh); b_s(&b, " -> "); } b_int(&b, w); b_ch(&b, 'x'); b_int(&b, h); b_s(&b, ", "); b_int(&b, len / 1024); b_s(&b, " КБ): "); b_s(&b, path);
    if (!remote) chat_say(msg);                           /* по HTTP-заявке чат игры не засоряем */
done:
    if (tex2 && g_scr_mDestroy) { void* da[1]; void* e2 = NULL; da[0] = tex2; A.runtime_invoke(g_scr_mDestroy, NULL, da, &e2); }
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


/* ───────────── отправка PNG в Telegram самим модом ─────────────
   Почему так: WebView игры (Cohtml) не умеет отправлять бинарное тело в XHR (нет FormData/fetch, а Uint8Array/ArrayBuffer/Blob Telegram
   принимает как 400 с пустым ответом). Поэтому скрипт только просит мод: GET /screen/tg?... — а мод сам делает multipart-POST на
   api.telegram.org через java.net.HttpURLConnection (тот же путь, которым загрузчик качает бандл; своего TLS в C нет).
   Работает в фоновом потоке, игровой поток и HTTP-сервер не блокирует. Результат скрипт читает из /screen/tgstatus.
   Только JNI-вызовы по индексам из jni.h (как в загрузчике и hassle_mem.c); исключения Java сбрасываем и превращаем в текст ошибки. */
#define SCR_UP_MAX (12 * 1024 * 1024)                      /* PNG больше — не шлём (лимит Telegram: sendPhoto 10 МБ, sendDocument 50 МБ) */
#define SCR_UP_PHOTO_MAX (9 * 1024 * 1024)
#define SCR_UP_HEAD 4096                                   /* место под заголовки multipart перед файлом */
static u8 g_scr_upBuf[SCR_UP_HEAD + SCR_UP_MAX + 512];    /* тело multipart целиком; в .bss — память занимается только по факту записи */
static volatile int g_scr_tgState;                         /* 0 — не запускали, 1 — идёт, 2 — готово, 3 — ошибка */
static volatile int g_scr_tgCode;                          /* HTTP-код Telegram (или 0) */
static volatile int g_scr_tgId;                            /* номер задания: растёт на каждый запуск */
static char g_scr_tgMsg[300];                              /* причина ошибки (description от Telegram или наш текст) */
static char g_scr_tgTok[160], g_scr_tgChat[48], g_scr_tgCap[760], g_scr_tgPath[400], g_scr_tgKb[900];
static int g_scr_tgDoc;
static long g_scr_tgEditId;                                /* >0 — не слать новое фото, а заменить картинку в сообщении с этим message_id (Live) */
static volatile long g_scr_tgMid;                          /* message_id из ответа Telegram (для Live: первое сообщение, потом редактируем его) */

typedef int (*scj_getvms_t)(void**, int, int*);
typedef int (*scj_getenv_t)(void*, void**, int);
typedef int (*scj_attach_t)(void*, void**, void*);
typedef int (*scj_detach_t)(void*);
typedef void* (*scj_find_t)(void*, const char*);
typedef void (*scj_clr_t)(void*);
typedef void* (*scj_mid_t)(void*, void*, const char*, const char*);
typedef void* (*scj_newobj_t)(void*, void*, void*, ...);
typedef void* (*scj_callo_t)(void*, void*, void*, ...);
typedef int (*scj_calli_t)(void*, void*, void*, ...);
typedef void (*scj_callv_t)(void*, void*, void*, ...);
typedef void* (*scj_str_t)(void*, const char*);
typedef void* (*scj_newarr_t)(void*, int);
typedef void (*scj_getreg_t)(void*, void*, int, int, signed char*);
typedef void (*scj_setreg_t)(void*, void*, int, int, const signed char*);
typedef unsigned char (*scj_exc_t)(void*);
#define SCJ(env, T, idx) ((T)((*(void***)(env))[idx]))
#define SCJV(vm, T, idx) ((T)((*(void***)(vm))[idx]))
/* индексы JNINativeInterface: FindClass 6, ExceptionClear 17, GetMethodID 33, CallObjectMethod 34, CallIntMethod 49, CallVoidMethod 61,
   NewObject 28, NewStringUTF 167, NewByteArray 176, GetByteArrayRegion 200, SetByteArrayRegion 208, ExceptionCheck 228;
   JavaVM: AttachCurrentThread 4, DetachCurrentThread 5, GetEnv 6 */
static int scj_clean(void* env) { if (SCJ(env, scj_exc_t, 228)(env)) { SCJ(env, scj_clr_t, 17)(env); return 0; } return 1; }
static void* scj_get_vm(void) {
    scj_getvms_t f = NULL; void *vm = NULL, *h; int n = 0;
    h = dlopen(NULL, RTLD_NOW_); if (h) *(void**)&f = dlsym(h, "JNI_GetCreatedJavaVMs");
    if (!f) { h = dlopen("libnativehelper.so", RTLD_NOW_ | 4); if (h) *(void**)&f = dlsym(h, "JNI_GetCreatedJavaVMs"); }
    if (!f) { h = dlopen("libart.so", RTLD_NOW_ | 4); if (h) *(void**)&f = dlsym(h, "JNI_GetCreatedJavaVMs"); }
    if (!f) return NULL;
    if (f(&vm, 1, &n) != 0 || n < 1) return NULL;
    return vm;
}
static void scr_tg_msg(const char* m) { unsigned i; for (i = 0; m && m[i] && i < sizeof g_scr_tgMsg - 1; i++) g_scr_tgMsg[i] = m[i]; g_scr_tgMsg[i] = 0; }
/* ответ Telegram: достаём "description":"..." (без разбора экранирования — нам нужен только короткий текст для чата) */
static void scr_tg_desc(const char* r, unsigned n, char* out, unsigned cap) {
    static const char key[] = "\"description\":\""; unsigned i, k = 0;
    out[0] = 0;
    for (i = 0; i + sizeof key < n; i++) {
        if (r[i] == '"' && sprefix(r + i, key)) {
            i += sizeof key - 1;
            while (i < n && r[i] != '"' && k + 1 < cap) { if (r[i] == '\\' && i + 1 < n) i++; out[k++] = r[i++]; }
            out[k] = 0; return;
        }
    }
    for (i = 0; i < n && k + 1 < cap; i++) out[k++] = (r[i] == '\r' || r[i] == '\n') ? ' ' : r[i];
    out[k] = 0;
}

/* JSON-строка без кавычек вокруг: экранируем " и \, управляющие символы заменяем пробелом */
static void scr_json_esc(Buf* b, const char* t) {
    for (; *t; t++) {
        if (*t == '"' || *t == '\\') { b_ch(b, '\\'); b_ch(b, *t); }
        else if ((unsigned char)*t < 32) b_ch(b, ' ');
        else b_ch(b, *t);
    }
}
static void scr_mp_field(Buf* b, const char* boundary, const char* name) {
    b_s(b, "--"); b_s(b, boundary); b_s(b, "\r\nContent-Disposition: form-data; name=\""); b_s(b, name); b_s(b, "\"\r\n\r\n");
}

/* Собирает multipart в g_scr_upBuf: возвращает полную длину тела или 0 (причина в g_scr_tgMsg).
   Обычная отправка: sendPhoto/sendDocument (chat_id, caption, reply_markup, файл).
   Правка (g_scr_tgEditId > 0): editMessageMedia (chat_id, message_id, media = {"type":"photo","media":"attach://scr",...}, reply_markup, файл «scr»). */
static unsigned long scr_tg_build(const char* boundary, int asDoc) {
    Buf b; int fd; long off, got = 0; int edit = g_scr_tgEditId > 0;
    fd = open(g_scr_tgPath, 0);
    if (fd < 0) { scr_tg_msg("не открыть файл снимка"); return 0; }
    b_init(&b, (char*)g_scr_upBuf, SCR_UP_HEAD);
    scr_mp_field(&b, boundary, "chat_id"); b_s(&b, g_scr_tgChat); b_s(&b, "\r\n");
    if (edit) {
        scr_mp_field(&b, boundary, "message_id"); b_int(&b, g_scr_tgEditId); b_s(&b, "\r\n");
        scr_mp_field(&b, boundary, "media");
        b_s(&b, "{\"type\":\"photo\",\"media\":\"attach://scr\"");
        if (g_scr_tgCap[0]) { b_s(&b, ",\"caption\":\""); scr_json_esc(&b, g_scr_tgCap); b_s(&b, "\",\"parse_mode\":\"HTML\""); }
        b_s(&b, "}\r\n");
    } else if (g_scr_tgCap[0]) {
        scr_mp_field(&b, boundary, "caption"); b_s(&b, g_scr_tgCap); b_s(&b, "\r\n");
        scr_mp_field(&b, boundary, "parse_mode"); b_s(&b, "HTML\r\n");
    }
    if (g_scr_tgKb[0]) { scr_mp_field(&b, boundary, "reply_markup"); b_s(&b, g_scr_tgKb); b_s(&b, "\r\n"); }
    b_s(&b, "--"); b_s(&b, boundary); b_s(&b, "\r\nContent-Disposition: form-data; name=\"");
    b_s(&b, edit ? "scr" : (asDoc ? "document" : "photo")); b_s(&b, "\"; filename=\"screen.png\"\r\nContent-Type: image/png\r\n\r\n");
    if (b.n + 8 >= SCR_UP_HEAD) { close(fd); scr_tg_msg("подпись или кнопки слишком длинные"); return 0; }
    off = (long)b.n;
    for (;;) {
        long room = (long)sizeof g_scr_upBuf - 256 - off, n;
        if (room <= 0) break;
        n = read(fd, g_scr_upBuf + off, (unsigned long)room);
        if (n <= 0) break;
        off += n; got += n;
    }
    close(fd);
    if (got < 8) { scr_tg_msg("файл снимка пустой"); return 0; }
    if (got > SCR_UP_MAX) { scr_tg_msg("снимок слишком большой для отправки"); return 0; }
    b_init(&b, (char*)g_scr_upBuf + off, 256);
    b_s(&b, "\r\n--"); b_s(&b, boundary); b_s(&b, "--\r\n");
    return (unsigned long)off + b.n;
}

/* Сам POST. Возвращает HTTP-код (200 — принят) или 0 при сбое до ответа (причина в g_scr_tgMsg). */
static int scr_tg_post(const char* method, const char* boundary, unsigned long total) {
    void *vm, *env = NULL, *cURL = NULL, *cConn = NULL, *cHttp = NULL, *cOS = NULL, *cIS = NULL;
    void *jurl, *jstr, *conn, *os = NULL, *in = NULL, *arr = NULL, *jm, *jct, *jctv;
    void *mNew, *mOpen, *mCT, *mRT, *mDoOut, *mProp, *mGetOut, *mMeth, *mFix, *mCode, *mErr, *mGetIn, *mDisc, *mWrite, *mFlush, *mClose, *mRead, *mIClose;
    int attached = 0, rc, code = 0; unsigned long sent = 0; char url[300], ct[120]; Buf b;
    char resp[2048]; unsigned rn = 0;
    scj_find_t FindClass; scj_mid_t GetMid; scj_callo_t CallObj; scj_calli_t CallInt; scj_callv_t CallVoid;
#define SCJ_FAIL(m) do { scr_tg_msg(m); code = 0; goto out; } while (0)
    vm = scj_get_vm(); if (!vm) { scr_tg_msg("нет JavaVM (JNI_GetCreatedJavaVMs не найдена)"); return 0; }
    rc = SCJV(vm, scj_getenv_t, 6)(vm, &env, 0x00010006);
    if (rc == -2) { if (SCJV(vm, scj_attach_t, 4)(vm, &env, NULL) != 0 || !env) { scr_tg_msg("AttachCurrentThread не удался"); return 0; } attached = 1; }
    else if (rc != 0 || !env) { scr_tg_msg("GetEnv не удался"); return 0; }
    FindClass = SCJ(env, scj_find_t, 6); GetMid = SCJ(env, scj_mid_t, 33);
    CallObj = SCJ(env, scj_callo_t, 34); CallInt = SCJ(env, scj_calli_t, 49); CallVoid = SCJ(env, scj_callv_t, 61);
    cURL = FindClass(env, "java/net/URL"); cConn = FindClass(env, "java/net/URLConnection"); cHttp = FindClass(env, "java/net/HttpURLConnection");
    cOS = FindClass(env, "java/io/OutputStream"); cIS = FindClass(env, "java/io/InputStream");
    if (!cURL || !cConn || !cHttp || !cOS || !cIS || !scj_clean(env)) SCJ_FAIL("нет нужных классов java.net / java.io");
    mNew = GetMid(env, cURL, "<init>", "(Ljava/lang/String;)V");
    mOpen = GetMid(env, cURL, "openConnection", "()Ljava/net/URLConnection;");
    mCT = GetMid(env, cConn, "setConnectTimeout", "(I)V");
    mRT = GetMid(env, cConn, "setReadTimeout", "(I)V");
    mDoOut = GetMid(env, cConn, "setDoOutput", "(Z)V");
    mProp = GetMid(env, cConn, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    mGetOut = GetMid(env, cConn, "getOutputStream", "()Ljava/io/OutputStream;");
    mGetIn = GetMid(env, cConn, "getInputStream", "()Ljava/io/InputStream;");
    mMeth = GetMid(env, cHttp, "setRequestMethod", "(Ljava/lang/String;)V");
    mFix = GetMid(env, cHttp, "setFixedLengthStreamingMode", "(I)V");
    mCode = GetMid(env, cHttp, "getResponseCode", "()I");
    mErr = GetMid(env, cHttp, "getErrorStream", "()Ljava/io/InputStream;");
    mDisc = GetMid(env, cHttp, "disconnect", "()V");
    mWrite = GetMid(env, cOS, "write", "([BII)V"); mFlush = GetMid(env, cOS, "flush", "()V"); mClose = GetMid(env, cOS, "close", "()V");
    mRead = GetMid(env, cIS, "read", "([B)I"); mIClose = GetMid(env, cIS, "close", "()V");
    if (!scj_clean(env) || !mNew || !mOpen || !mCT || !mRT || !mDoOut || !mProp || !mGetOut || !mGetIn || !mMeth || !mFix || !mCode || !mErr || !mDisc || !mWrite || !mFlush || !mClose || !mRead || !mIClose)
        SCJ_FAIL("не нашлись методы Java (HttpURLConnection)");
    b_init(&b, url, sizeof url); b_s(&b, "https://api.telegram.org/bot"); b_s(&b, g_scr_tgTok); b_ch(&b, '/'); b_s(&b, method);
    b_init(&b, ct, sizeof ct); b_s(&b, "multipart/form-data; boundary="); b_s(&b, boundary);
    jstr = SCJ(env, scj_str_t, 167)(env, url); if (!jstr) SCJ_FAIL("NewStringUTF");
    jurl = SCJ(env, scj_newobj_t, 28)(env, cURL, mNew, jstr); if (!jurl || !scj_clean(env)) SCJ_FAIL("new URL() не удался");
    conn = CallObj(env, jurl, mOpen); if (!conn || !scj_clean(env)) SCJ_FAIL("openConnection не удался");
    CallVoid(env, conn, mCT, 15000); CallVoid(env, conn, mRT, 60000); if (!scj_clean(env)) SCJ_FAIL("таймауты");
    jm = SCJ(env, scj_str_t, 167)(env, "POST"); CallVoid(env, conn, mMeth, jm); if (!scj_clean(env)) SCJ_FAIL("setRequestMethod");
    CallVoid(env, conn, mDoOut, 1); if (!scj_clean(env)) SCJ_FAIL("setDoOutput");
    jct = SCJ(env, scj_str_t, 167)(env, "Content-Type"); jctv = SCJ(env, scj_str_t, 167)(env, ct);
    CallVoid(env, conn, mProp, jct, jctv); if (!scj_clean(env)) SCJ_FAIL("setRequestProperty");
    CallVoid(env, conn, mFix, (int)total); if (!scj_clean(env)) SCJ_FAIL("setFixedLengthStreamingMode");
    os = CallObj(env, conn, mGetOut); if (!os || !scj_clean(env)) SCJ_FAIL("нет соединения с api.telegram.org (getOutputStream)");
    arr = SCJ(env, scj_newarr_t, 176)(env, 65536); if (!arr) SCJ_FAIL("NewByteArray");
    while (sent < total) {
        unsigned long n = total - sent < 65536 ? total - sent : 65536;
        SCJ(env, scj_setreg_t, 208)(env, arr, 0, (int)n, (const signed char*)(g_scr_upBuf + sent));
        CallVoid(env, os, mWrite, arr, 0, (int)n);
        if (!scj_clean(env)) SCJ_FAIL("обрыв при отправке данных");
        sent += n;
    }
    CallVoid(env, os, mFlush); CallVoid(env, os, mClose); if (!scj_clean(env)) SCJ_FAIL("обрыв при завершении отправки");
    code = CallInt(env, conn, mCode); if (!scj_clean(env)) SCJ_FAIL("нет ответа от Telegram (getResponseCode)");
    in = CallObj(env, conn, code >= 400 ? mErr : mGetIn);
    if (!scj_clean(env)) in = NULL;
    if (in) {
        for (;;) {
            int r = CallInt(env, in, mRead, arr);
            if (!scj_clean(env) || r < 0) break;
            if (r == 0) continue;
            if (rn + (unsigned)r > sizeof resp - 1) r = (int)(sizeof resp - 1 - rn);
            if (r > 0) { SCJ(env, scj_getreg_t, 200)(env, arr, 0, r, (signed char*)(resp + rn)); rn += (unsigned)r; }
            if (rn >= sizeof resp - 1) break;
        }
        CallVoid(env, in, mIClose); scj_clean(env);
    }
    resp[rn] = 0;
    if (code == 200) {                                                 /* "message_id":N — понадобится Live, чтобы править то же сообщение */
        static const char mk[] = "\"message_id\":"; unsigned q;
        for (q = 0; q + sizeof mk < rn; q++) {
            if (resp[q] == '"' && sprefix(resp + q, mk)) {
                long v = 0; q += sizeof mk - 1;
                while (q < rn && resp[q] >= '0' && resp[q] <= '9' && v < 2000000000L) { v = v * 10 + (resp[q] - '0'); q++; }
                if (v > 0) g_scr_tgMid = v;
                break;
            }
        }
    }
    if (code != 200) { char d[260]; scr_tg_desc(resp, rn, d, sizeof d); scr_tg_msg(d[0] ? d : "пустой ответ"); }
    CallVoid(env, conn, mDisc); scj_clean(env);
out:
    if (attached) SCJV(vm, scj_detach_t, 5)(vm);
    return code;
#undef SCJ_FAIL
}

static void* scr_tg_thread(void* arg) {
    char boundary[64]; Buf b; unsigned long total; int code, asDoc;
    (void)arg;
    g_scr_tgMsg[0] = 0; g_scr_tgCode = 0; g_scr_tgMid = g_scr_tgEditId > 0 ? g_scr_tgEditId : 0;
    b_init(&b, boundary, sizeof boundary); b_s(&b, "----hassle"); b_int(&b, time(NULL)); b_int(&b, g_scr_tgId);
    asDoc = g_scr_tgEditId > 0 ? 0 : g_scr_tgDoc;
    total = scr_tg_build(boundary, asDoc);
    if (!total) { dlog2("скрин: отправка в Telegram: ", g_scr_tgMsg); __atomic_store_n(&g_scr_tgState, 3, __ATOMIC_RELEASE); return NULL; }
    if (!asDoc && total > SCR_UP_PHOTO_MAX) {                   /* для sendPhoto слишком тяжёлый — пересоберём как документ */
        asDoc = 1; total = scr_tg_build(boundary, 1);
        if (!total) { dlog2("скрин: отправка в Telegram: ", g_scr_tgMsg); __atomic_store_n(&g_scr_tgState, 3, __ATOMIC_RELEASE); return NULL; }
    }
    code = scr_tg_post(g_scr_tgEditId > 0 ? "editMessageMedia" : asDoc ? "sendDocument" : "sendPhoto", boundary, total);
    if (code == 400 && !asDoc && g_scr_tgEditId <= 0) {                                /* Telegram не принял как фото (размеры/пропорции) — файлом */
        dlog2("скрин: sendPhoto 400: ", g_scr_tgMsg);
        total = scr_tg_build(boundary, 1);
        code = total ? scr_tg_post("sendDocument", boundary, total) : 0;
    }
    g_scr_tgCode = code;
    if (code == 200) { dlog_i("скрин: отправлено в Telegram, байт ", (long)total); __atomic_store_n(&g_scr_tgState, 2, __ATOMIC_RELEASE); }
    else { dlog_i("скрин: Telegram не принял, код ", code); dlog2("скрин: причина: ", g_scr_tgMsg); __atomic_store_n(&g_scr_tgState, 3, __ATOMIC_RELEASE); }
    return NULL;
}

/* значение параметра из строки запроса «GET /путь?a=1&b=2 HTTP/1.1» → копия значения (с %XX и '+' → пробел), cap включая 0 */
static int scr_qparam(const char* rq, const char* name, char* out, unsigned cap) {
    unsigned i = 0, nl = slen(name), k = 0;
    while (rq[i] && rq[i] != ' ' && rq[i] != '\r' && rq[i] != '\n') i++;          /* пропускаем «GET» */
    if (rq[i] == ' ') i++;
    while (rq[i] && rq[i] != ' ' && rq[i] != '\r' && rq[i] != '\n' && rq[i] != '?') i++;   /* пропускаем путь */
    if (rq[i] != '?') { out[0] = 0; return 0; }
    i++;
    while (rq[i] && rq[i] != ' ' && rq[i] != '\r' && rq[i] != '\n') {
        if ((rq[i - 1] == '?' || rq[i - 1] == '&') && sprefix(rq + i, name) && rq[i + nl] == '=') {
            i += nl + 1;
            while (rq[i] && rq[i] != '&' && rq[i] != ' ' && rq[i] != '\r' && rq[i] != '\n' && k + 1 < cap) {
                char c = rq[i];
                if (c == '%' && rq[i + 1] && rq[i + 2]) {
                    int hi = rq[i + 1], lo = rq[i + 2];
                    hi = hi >= '0' && hi <= '9' ? hi - '0' : (hi | 32) >= 'a' && (hi | 32) <= 'f' ? (hi | 32) - 'a' + 10 : -1;
                    lo = lo >= '0' && lo <= '9' ? lo - '0' : (lo | 32) >= 'a' && (lo | 32) <= 'f' ? (lo | 32) - 'a' + 10 : -1;
                    if (hi >= 0 && lo >= 0) { out[k++] = (char)(hi * 16 + lo); i += 3; continue; }
                }
                out[k++] = c == '+' ? ' ' : c; i++;
            }
            out[k] = 0; return 1;
        }
        i++;
    }
    out[0] = 0; return 0;
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
    b_s(&b, "{\"v\":1,\"cap\":2,\"seq\":"); b_int(&b, __atomic_load_n(&g_scr_doneSeq, __ATOMIC_ACQUIRE));
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
    int delay = 0, i, live = 0;
    static const char okj[] = "{\"ok\":true}", busy[] = "{\"ok\":false,\"err\":\"busy\"}", need[] = "{\"ok\":false,\"err\":\"need_cmd\"}";
    for (i = 0; rq[i] && rq[i] != '\r' && rq[i] != '\n'; i++) {
        if (rq[i] == 'd' && sprefix(rq + i, "delay=")) {
            int k = i + 6; delay = 0;
            while (rq[k] >= '0' && rq[k] <= '9' && delay < 1000) { delay = delay * 10 + (rq[k] - '0'); k++; }
            break;
        }
    }
    if (delay > SCR_MAX_DELAY_S) delay = SCR_MAX_DELAY_S;
    for (i = 0; rq[i] && rq[i] != '\r' && rq[i] != '\n'; i++) if (rq[i] == 'l' && sprefix(rq + i, "live=1")) { live = 1; break; }
    if (__atomic_load_n(&g_scr_pending, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_scr_busy, __ATOMIC_ACQUIRE)) {
        scr_respond(c, "200 OK", "application/json", busy, sizeof busy - 1); return;
    }
    if (!g_scr_hooked || g_scr_hookDead || g_scr_resolved <= 0 || !g_scr_dir[0]) {
        scr_respond(c, "200 OK", "application/json", need, sizeof need - 1); return;
    }
    g_scr_due = time(NULL) + delay;
    __atomic_store_n(&g_scr_remote, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_scr_remoteLive, live, __ATOMIC_RELEASE);
    __atomic_store_n(&g_scr_pending, 1, __ATOMIC_RELEASE);
    dlog(live ? "скрин: заявка по HTTP (live)" : "скрин: заявка по HTTP");
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

/* /screen/tg?k=ТОКЕН&c=ЧАТ&doc=0|1&cap=ПОДПИСЬ(urlencoded) — запуск отправки последнего снимка в Telegram: {"ok":true,"id":N}; результат — /screen/tgstatus */
static void scr_http_tg(int c, const char* rq) {
    static const char busy[] = "{\"ok\":false,\"err\":\"busy\"}", nosnap[] = "{\"ok\":false,\"err\":\"no_snapshot\"}", bad[] = "{\"ok\":false,\"err\":\"bad_args\"}";
    char doc[8], js[64]; unsigned long t = 0; int k; Buf b;
    if (__atomic_load_n(&g_scr_tgState, __ATOMIC_ACQUIRE) == 1) { scr_respond(c, "200 OK", "application/json", busy, sizeof busy - 1); return; }
    if (__atomic_load_n(&g_scr_doneSeq, __ATOMIC_ACQUIRE) <= 0 || g_scr_lastLen <= 0) { scr_respond(c, "200 OK", "application/json", nosnap, sizeof nosnap - 1); return; }
    if (!scr_qparam(rq, "k", g_scr_tgTok, sizeof g_scr_tgTok) || !scr_qparam(rq, "c", g_scr_tgChat, sizeof g_scr_tgChat) || !g_scr_tgTok[0] || !g_scr_tgChat[0]) {
        scr_respond(c, "200 OK", "application/json", bad, sizeof bad - 1); return;
    }
    scr_qparam(rq, "cap", g_scr_tgCap, sizeof g_scr_tgCap);
    scr_qparam(rq, "kb", g_scr_tgKb, sizeof g_scr_tgKb);                /* reply_markup (JSON, только ASCII: не-ASCII как \uXXXX) */
    {   char ms[16]; unsigned mi; long mv = 0;
        scr_qparam(rq, "m", ms, sizeof ms);                             /* message_id для правки (Live); пусто — новое сообщение */
        for (mi = 0; ms[mi] >= '0' && ms[mi] <= '9' && mv < 2000000000L; mi++) mv = mv * 10 + (ms[mi] - '0');
        g_scr_tgEditId = mv;
    }
    scr_qparam(rq, "doc", doc, sizeof doc); g_scr_tgDoc = doc[0] == '1';
    for (k = 0; k < (int)sizeof g_scr_tgPath - 1 && g_scr_lastPath[k]; k++) g_scr_tgPath[k] = g_scr_lastPath[k];
    g_scr_tgPath[k] = 0;
    g_scr_tgMsg[0] = 0; g_scr_tgCode = 0; g_scr_tgId++;
    __atomic_store_n(&g_scr_tgState, 1, __ATOMIC_RELEASE);
    dlog("скрин: заявка на отправку в Telegram");
    if (pthread_create(&t, NULL, scr_tg_thread, NULL) != 0) {
        scr_tg_msg("не создать поток отправки"); __atomic_store_n(&g_scr_tgState, 3, __ATOMIC_RELEASE);
    }
    b_init(&b, js, sizeof js); b_s(&b, "{\"ok\":true,\"id\":"); b_int(&b, g_scr_tgId); b_s(&b, "}");
    scr_respond(c, "200 OK", "application/json", js, b.n);
}
static void scr_http_tgstatus(int c) {
    char js[560], m[300]; Buf b; unsigned i, k = 0;
    for (i = 0; g_scr_tgMsg[i] && k + 2 < sizeof m; i++) {          /* для JSON: убираем кавычки/слэши/переводы строк */
        char ch = g_scr_tgMsg[i];
        m[k++] = (ch == '"' || ch == '\\' || (unsigned char)ch < 32) ? ' ' : ch;
    }
    m[k] = 0;
    b_init(&b, js, sizeof js);
    b_s(&b, "{\"id\":"); b_int(&b, g_scr_tgId); b_s(&b, ",\"state\":"); b_int(&b, __atomic_load_n(&g_scr_tgState, __ATOMIC_ACQUIRE));
    b_s(&b, ",\"mid\":"); b_int(&b, g_scr_tgMid); b_s(&b, ",\"code\":"); b_int(&b, g_scr_tgCode); b_s(&b, ",\"msg\":\""); b_s(&b, m); b_s(&b, "\"}");
    scr_respond(c, "200 OK", "application/json", js, b.n);
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
            char rq[3600]; long n; struct scr_tv t;
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
                else if (sprefix(rq, "GET /screen/tgstatus")) scr_http_tgstatus(c);
                else if (sprefix(rq, "GET /screen/tg")) scr_http_tg(c, rq);
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
            __atomic_store_n(&g_scr_remoteLive, 0, __ATOMIC_RELEASE);
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
