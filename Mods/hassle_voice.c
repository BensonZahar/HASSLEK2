/*
 * hassle_voice.c — необязательный мод «/voice»: сохранять голосовой чат игроков рядом с вами в папку, чтобы потом слушать.
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите этот файл и пересоберите бандл.
 *
 * Команды в чате (нативные, как /screen):
 *   /voice on        начать запись (и помнить это при следующих запусках игры)
 *   /voice off       остановить запись (и не включать её сама при запуске)
 *   /voice near      писать только тех, кто рядом (по умолчанию): объёмный звук от игрока
 *   /voice all       писать всё, что слышит клиент: рядом + рация, телефон, общий канал
 *   /voice status    состояние: идёт ли запись, стоит ли перехват, сколько пакетов и файлов
 *   /voice folder    куда сохраняются файлы
 *   /voice unhook    вернуть код игры в исходный вид (запись выключается)
 *   /voice tg on     включить запись и слать каждую реплику в Telegram голосовым сообщением с подписью «🎙 Ник[id] · 📏 4.2 м · 3.2 с» (расстояние до говорящего, как в Code3/Near)
 *   /voice tg off    перестать слать в Telegram (запись в файлы не трогается)
 *   /voice tg test   отправить в Telegram последнюю записанную реплику (проверка токена и чата)
 *   /voice tg        состояние отправки: сколько ушло, сколько ошибок, последняя причина
 *
 * Telegram: токен и чат — в файле <папка voice>/tg.txt (строка 1 — токен бота, 2 — chat_id, 3 — необязательная метка аккаунта) или от скрипта
 *   через HTTP 127.0.0.1:17791..17800 (/voice/status, /voice/cfg?k=&c=&who=&tg=0|1&on=0|1&mode=near|all&kb=&test=1; на диск токен не пишется).
 *   kb — reply_markup (JSON, только ASCII: не-ASCII как \uXXXX, urlencoded) — кнопки под каждым голосовым, их строит скрипт (как kb= у /screen).
 *   Отправка идёт отдельным потоком через JNI HttpURLConnection (как /screen): игру не тормозит; очередь на 32 реплики, лишние отбрасываются.
 *
 * Куда и во что
 *   • Папка: <папка журнала HLOG>/voice (обычно /sdcard/Android/data/<пакет>/files/HLOG/voice, видно по USB и в файловом менеджере).
 *   • Формат: .opus (Ogg Opus, моно 48 кГц). Это те самые сжатые пакеты, что пришли по сети, — их НЕ декодируем и не перекодируем: звук
 *     без потерь относительно того, что слышит игра, а файл маленький (порядка 100–250 КБ в минуту речи). Играет в VLC, Telegram
 *     (как голосовое), Chrome, на Android 5+ в штатных плеерах; в mp3/wav переводится одной командой ffmpeg -i файл.opus файл.mp3
 *   • Один файл — одна «реплика»: пока человек говорит, файл растёт; пауза дольше 1,5 с — файл закрывается, следующая фраза в новом.
 *     Имя: ГГГГММДД_ЧЧММСС_<вид>_<id>[_<ник>].opus (время UTC), вид: near, radio, phone, global. Ник и ID записаны ещё и в теги файла.
 *   • Файл читается даже если игра вылетела посреди записи (последняя секунда может не дописаться).
 *
 * Как это работает (по дампу libil2cpp.so этой сборки)
 *   • Голос — собственный: сервер шлёт пакеты Opus (48 кГц, моно, кадр 100 мс). VoiceChat.OnReceivePacket читает из пакета handle потока,
 *     длину и byte[], зовёт AudioPlayback.PutStreamData → AudioStream.PutData(буфер, длина, буфер_PCM, размер_кадра), а та декодирует
 *     Opus.Decode и отдаёт PCM в BASS. Буфер идёт в декодер как есть — без заголовка, то есть это чистый пакет Opus.
 *   • Поток создаёт AudioStreamOpen: читает handle, громкость, use3D, тип привязки и тип эффекта (None/Radio/Phone). При привязке к игроку
 *     (AttachType.Player) и use3D номер игрока кладётся в поток (m_PlayerIndex). Поэтому «рядом со мной» = потоки с use3D, а у радио и
 *     телефона use3D выключен. Кто к вам доходит — решает сервер, мы пишем то, что клиент и так получил.
 *   • В НАЧАЛО AudioStream.PutData ставится перехват (arm64), как у /vgun: 16 байт пролога заменяются на «LDR x16,=адрес; BR x16», прыжок
 *     ведёт в заглушку в анонимной странице: она сохраняет x0–x7 и x30, зовёт vc_hook(поток, массив, длина, …), возвращает регистры,
 *     выполняет затёртые 4 инструкции и прыгает на 5-ю инструкцию функции. vc_hook только копирует пакет в кольцо (без вызовов IL2CPP,
 *     без выделения памяти, спинлок с ограниченным ожиданием — при заминке пакет теряется, а не тормозит игру). Файлы пишет отдельный поток.
 *   • Место патча не зашито адресом: метод находим по имени, а то, что это именно он, проверяем — в первых 48 словах должен быть вызов
 *     Opus.Decode (адрес берётся из метаданных), а четыре затираемые инструкции не должны зависеть от адреса. Иначе ничего не трогаем.
 *   • Смещения полей потока (m_Use3D, m_PlayerIndex, …) берутся из метаданных, не из дампа.
 *   • Ник говорящего: PlayerPool.Get(id) → Tags.m_NickName.text (как в /near), один раз на файл.
 *   • Только arm64. На arm32 команда ответит, что не поддерживается.
 *   • Свои слова вы так не запишете: голос с вашего микрофона уходит на сервер и обратно к вам не приходит.
 */
#pragma once
#include "hassle_core.c"

#if defined(__aarch64__) || defined(VC_TEST_HOOK)
#define VC_HOOK 1
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int mprotect(void*, unsigned long, int);
extern long pwrite(int, const void*, unsigned long, long);
#endif
extern int clock_gettime(int, void*);
extern int unlink(const char*);

typedef unsigned long long vc_u64;

#define VC_PKT_MAX      1024        /* клиент шлёт Opus-пакеты до 512 байт (AudioRecord.m_MaxEncodedBytesSize); что больше — не наше */
#define VC_RING         256         /* пакетов в кольце между игрой и писателем (при ~10 пакетах/с на говорящего — запас на секунды) */
#define VC_SESS         24          /* одновременно открытых файлов */
#define VC_GAP_MS       1500        /* пауза в потоке, после которой файл закрывается */
#define VC_MAX_SEG_MS   600000      /* один файл не дольше 10 минут */
#define VC_PRESKIP      312         /* pre-skip в OpusHead: задержка кодера Opus на 48 кГц */
#define VC_MODE_NEAR    0
#define VC_MODE_ALL     1
#define VC_MAX_SAMPLES  5760        /* максимум в одном пакете Opus: 120 мс при 48 кГц */

/* ───────────── общее состояние ───────────── */
typedef struct {
    long long ms;                   /* CLOCK_MONOTONIC, мс */
    void* st;                       /* AudioStream — только как ключ, не разыменовывается вне хука */
    u32 len; int bass;
    u16 pid; u8 attach, effect, use3d;
    u8 data[VC_PKT_MAX];
} VcPkt;
static VcPkt g_vc_ring[VC_RING];
static volatile unsigned g_vc_head, g_vc_tail;
static volatile int g_vc_lock;
static volatile int g_vc_on;                    /* 1 — пишем */
static volatile int g_vc_mode = VC_MODE_NEAR;
static volatile int g_vc_flush;                 /* 1 — писателю закрыть все файлы */
static volatile unsigned g_vc_calls, g_vc_pkts, g_vc_skip, g_vc_drop, g_vc_bad, g_vc_files, g_vc_errs;
static volatile unsigned g_vc_kb;               /* записано, КБ */
static int g_vc_offAttach = -1, g_vc_offEffect = -1, g_vc_offPid = -1, g_vc_offUse3d = -1, g_vc_offBass = -1;
static int g_vc_state;                          /* 1 — перехват стоит */
static int g_vc_modId = -1;
static char g_vc_err[200];
static char g_vc_dir[320];

static int vc_lock(int spins) {
    while (spins-- > 0) if (!__atomic_exchange_n(&g_vc_lock, 1, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}
static void vc_unlock(void) { __atomic_store_n(&g_vc_lock, 0, __ATOMIC_RELEASE); }

static int vc_fail(const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof g_vc_err - 1; i++) g_vc_err[i] = why[i];
    g_vc_err[i] = 0;
    dlog2("voice: перехват не поставлен: ", why);
    return 0;
}

/* ───────────── перехват голосового пакета ─────────────
   Зовётся из заглушки в начале AudioStream.PutData(this, byte[] buffer, int length, IntPtr decodingBuffer, int frameSize) в том потоке,
   где игра приняла пакет. Никаких вызовов IL2CPP и выделения памяти: смещения полей, режим и кольцо — готовые. Массив уже в руках игры
   (она сейчас закрепит его через GCHandle), так что он живой; длину массива сверяем с длиной пакета. */
__attribute__((noinline, used))
static void vc_hook(void* st, void* arr, int len, void* dec, int fs) {
    VcPkt* p; unsigned h; int use3d; struct { long s, n; } ts;
    (void)dec; (void)fs;
    __atomic_fetch_add(&g_vc_calls, 1, __ATOMIC_RELAXED);
    if (!__atomic_load_n(&g_vc_on, __ATOMIC_ACQUIRE) || g_vc_offUse3d < 0) return;
    if (!st || !arr || len < 1 || len > VC_PKT_MAX) return;
    if (*(volatile long*)((char*)arr + 3 * sizeof(void*)) < (long)len) return;               /* Il2CppArray.max_length */
    use3d = *(volatile u8*)((char*)st + g_vc_offUse3d) ? 1 : 0;
    if (!use3d && __atomic_load_n(&g_vc_mode, __ATOMIC_RELAXED) == VC_MODE_NEAR) { __atomic_fetch_add(&g_vc_skip, 1, __ATOMIC_RELAXED); return; }
    clock_gettime(1, &ts);                                                                    /* CLOCK_MONOTONIC */
    if (!vc_lock(4000)) { __atomic_fetch_add(&g_vc_drop, 1, __ATOMIC_RELAXED); return; }
    h = g_vc_head;
    if (h - __atomic_load_n(&g_vc_tail, __ATOMIC_ACQUIRE) >= VC_RING) { vc_unlock(); __atomic_fetch_add(&g_vc_drop, 1, __ATOMIC_RELAXED); return; }
    p = &g_vc_ring[h & (VC_RING - 1)];
    p->ms = (long long)ts.s * 1000 + ts.n / 1000000;
    p->st = st; p->len = (u32)len; p->use3d = (u8)use3d;
    p->attach = g_vc_offAttach >= 0 ? (u8)*(volatile int*)((char*)st + g_vc_offAttach) : 0;
    p->effect = g_vc_offEffect >= 0 ? (u8)*(volatile int*)((char*)st + g_vc_offEffect) : 0;
    p->pid = g_vc_offPid >= 0 ? *(volatile u16*)((char*)st + g_vc_offPid) : 0;
    p->bass = g_vc_offBass >= 0 ? *(volatile int*)((char*)st + g_vc_offBass) : 0;
    memcpy(p->data, (const char*)arr + 4 * sizeof(void*), (unsigned long)len);                 /* данные массива — сразу за заголовком */
    __atomic_store_n(&g_vc_head, h + 1, __ATOMIC_RELEASE);
    vc_unlock();
    __atomic_fetch_add(&g_vc_pkts, 1, __ATOMIC_RELAXED);
}

/* ───────────── чистые помощники: Opus и Ogg (без памяти игры) ───────────── */
/* Сколько отсчётов при 48 кГц в пакете Opus (RFC 6716, байт TOC): 0 — пакет негодный. */
static int vc_opus_samples(const u8* p, int n) {
    int cfg, c, fs, frames, tot;
    static const int silk[4] = { 480, 960, 1920, 2880 };      /* 10, 20, 40, 60 мс */
    static const int celt[4] = { 120, 240, 480, 960 };        /* 2,5, 5, 10, 20 мс */
    if (n < 1) return 0;
    cfg = p[0] >> 3; c = p[0] & 3;
    if (cfg < 12) fs = silk[cfg & 3];
    else if (cfg < 16) fs = (cfg & 1) ? 960 : 480;            /* гибридные: 10 и 20 мс */
    else fs = celt[cfg & 3];
    if (c == 0) frames = 1;
    else if (c < 3) frames = 2;
    else { if (n < 2) return 0; frames = p[1] & 63; if (!frames) return 0; }
    tot = fs * frames;
    return tot > VC_MAX_SAMPLES ? 0 : tot;
}

static u32 g_vc_crcTab[256];
static int g_vc_crcInit;
static void vc_crc_init(void) {
    u32 i, k, r;
    if (g_vc_crcInit) return;
    for (i = 0; i < 256; i++) {
        r = i << 24;
        for (k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : (r << 1);
        g_vc_crcTab[i] = r;
    }
    g_vc_crcInit = 1;
}
static u32 vc_crc(const u8* d, int n) {                      /* CRC Ogg: полином 0x04C11DB7, начальное 0, без отражения и без финального XOR */
    u32 crc = 0; int i;
    vc_crc_init();
    for (i = 0; i < n; i++) crc = (crc << 8) ^ g_vc_crcTab[((crc >> 24) ^ d[i]) & 255u];
    return crc;
}
static void vc_put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

/* Страница Ogg с одним пакетом. flags: 2 — первая страница потока, 4 — последняя. Возвращает размер страницы в байтах. */
#define VC_PAGE_MAX (27 + 8 + VC_PKT_MAX + 8)
static int vc_ogg_page(u8* o, int flags, vc_u64 gp, u32 serial, u32 seq, const u8* d, int len) {
    int nseg = len / 255 + 1, i, n;
    o[0] = 'O'; o[1] = 'g'; o[2] = 'g'; o[3] = 'S'; o[4] = 0; o[5] = (u8)flags;
    for (i = 0; i < 8; i++) o[6 + i] = (u8)(gp >> (8 * i));
    vc_put32(o + 14, serial); vc_put32(o + 18, seq); vc_put32(o + 22, 0);
    o[26] = (u8)nseg;
    for (i = 0; i < nseg - 1; i++) o[27 + i] = 255;
    o[27 + nseg - 1] = (u8)(len % 255);
    n = 27 + nseg;
    memcpy(o + n, d, (unsigned long)len); n += len;
    vc_put32(o + 22, vc_crc(o, n));
    return n;
}
static int vc_opus_head(u8* o) {
    static const char sig[] = "OpusHead";
    int i;
    for (i = 0; i < 8; i++) o[i] = (u8)sig[i];
    o[8] = 1; o[9] = 1;                                      /* версия 1, один канал */
    o[10] = (u8)VC_PRESKIP; o[11] = (u8)(VC_PRESKIP >> 8);
    vc_put32(o + 12, 48000);                                 /* частота исходника */
    o[16] = 0; o[17] = 0; o[18] = 0;                         /* усиление 0, раскладка каналов 0 */
    return 19;
}
static int vc_tag_add(u8* o, int n, int cap, const char* key, const char* val) {    /* «ключ=значение»; возвращает новую длину */
    int kl = (int)slen(key), vl = (int)slen(val), i;
    if (n + 4 + kl + vl > cap) return n;
    vc_put32(o + n, (u32)(kl + vl)); n += 4;
    for (i = 0; i < kl; i++) o[n++] = (u8)key[i];
    for (i = 0; i < vl; i++) o[n++] = (u8)val[i];
    return n;
}
static int vc_opus_tags(u8* o, int cap, const char* title, const char* artist, const char* comment) {
    static const char sig[] = "OpusTags", vendor[] = "konstt";
    int n = 0, i, cnt = 0, cntAt;
    for (i = 0; i < 8; i++) o[n++] = (u8)sig[i];
    vc_put32(o + n, (u32)(sizeof vendor - 1)); n += 4;
    for (i = 0; vendor[i]; i++) o[n++] = (u8)vendor[i];
    cntAt = n; n += 4;
    if (title && title[0])   { int m = vc_tag_add(o, n, cap, "TITLE=", title);     if (m != n) { n = m; cnt++; } }
    if (artist && artist[0]) { int m = vc_tag_add(o, n, cap, "ARTIST=", artist);   if (m != n) { n = m; cnt++; } }
    if (comment && comment[0]) { int m = vc_tag_add(o, n, cap, "COMMENT=", comment); if (m != n) { n = m; cnt++; } }
    vc_put32(o + cntAt, (u32)cnt);
    return n;
}

/* время UTC «ГГГГММДД_ЧЧММСС» из секунд (алгоритм «civil from days», как в /screen) */
static void vc_stamp(long t, char* out) {
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
/* ник → безопасная часть имени файла: латиница, цифры, «_-.»; остальное (в т.ч. кириллица) — «_», подряд идущие «_» схлопываются */
static void vc_san(const char* in, char* out, int cap) {
    int o = 0, i; char prev = 0;
    for (i = 0; in[i] && o < cap - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        char r = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.') ? (char)c : '_';
        if (r == '_' && (prev == '_' || o == 0)) { prev = '_'; continue; }
        out[o++] = r; prev = r;
    }
    while (o > 0 && out[o - 1] == '_') o--;
    out[o] = 0;
}

/* ───────────── ник игрока по ID (один вызов на файл) ───────────── */
static int vc_nick(int id, char* out, int cap) {
    void *pool, *kls, *m, *pl, *tags, *lab, *gt, *s, *exc = NULL; void* args[1]; int v = id;
    char raw[160]; int n, i, o = 0, depth = 0; const u16* ch;
    out[0] = 0;
    if (!g_img || !A.runtime_invoke) return 0;
    pool = current_pool();
    if (!pool) return 0;
    kls = A.object_get_class(pool);
    m = kls ? find_method(kls, "Get", 1) : NULL;                                  /* Get(ushort) или Get(int): оба читают младшие байты */
    if (!m) return 0;
    args[0] = &v;
    pl = A.runtime_invoke(m, pool, args, &exc);
    if (exc || !pl) return 0;
    tags = ref_field(pl, "Tags");
    lab = tags ? ref_field(tags, "m_NickName") : NULL;
    if (!lab) return 0;
    gt = find_method(A.object_get_class(lab), "get_text", 0);
    if (!gt) return 0;
    s = A.runtime_invoke(gt, lab, NULL, &exc);
    if (exc || !s) return 0;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 80 || !ch) return 0;
    utf16_to_utf8(ch, n, raw, sizeof raw);
    for (i = 0; raw[i] && o < cap - 1; i++) {                                      /* без rich-text тегов <...> */
        if (raw[i] == '<') { depth++; continue; }
        if (raw[i] == '>' && depth) { depth--; continue; }
        if (!depth) out[o++] = raw[i];
    }
    out[o] = 0;
    for (i = o - 1; i >= 0; i--) if (out[i] == '(') {                              /* «Ник (123)» → «Ник» */
        int j = i + 1, dg = 0;
        while (out[j] >= '0' && out[j] <= '9') { j++; dg++; }
        if (dg && out[j] == ')') { o = i; while (o > 0 && out[o - 1] == ' ') o--; out[o] = 0; }
        break;
    }
    return o > 0;
}

/* ───────────── расстояние до говорящего (3D, как «d» в /near) ─────────────
   Позиции — из последних пакетов синхронизации: говорящий — PlayerPool.Get(id), вы — LocalPlayer (read_pos_k учитывает пешком/водитель/пассажир).
   Зовётся только из писателя (как vc_nick). Нет пула, игрока или позиции — -1. */
static float vc_dist(int id) {
    void *pool, *kls, *m, *pl, *me, *exc = NULL; void* args[1]; int v = id;
    float a[3], c[3]; double dx, dy, dz;
    if (!g_img || !A.runtime_invoke) return -1.0f;
    pool = current_pool();
    if (!pool) return -1.0f;
    kls = A.object_get_class(pool);
    m = kls ? find_method(kls, "Get", 1) : NULL;
    if (!m) return -1.0f;
    args[0] = &v;
    pl = A.runtime_invoke(m, pool, args, &exc);
    if (exc || !pl) return -1.0f;
    me = local_player();
    if (!me || me == pl) return -1.0f;
    if (!read_pos_k(pl, a, NULL) || !read_pos_k(me, c, NULL)) return -1.0f;
    dx = (double)a[0] - c[0]; dy = (double)a[1] - c[1]; dz = (double)a[2] - c[2];
    return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz);
}

/* ───────────── файлы ───────────── */
typedef struct {
    void* st; int fd, active;
    long long lastMs, startMs;
    vc_u64 gp;                                    /* позиция Ogg: pre-skip + отсчёты уже записанных пакетов */
    u32 serial, seq; int pkts;
    u8 held[VC_PKT_MAX]; int heldLen, heldSamples;  /* последний пакет ждёт следующего, чтобы последняя страница получила флаг конца потока */
    u16 pid; u8 use3d, attach, effect; int bass;
    float dist;                                   /* 3D-расстояние до говорящего в начале реплики, м; < 0 — нет данных */
    char path[360], nick[48];
} VcSess;
static VcSess g_vc_sess[VC_SESS];
static u32 g_vc_serialSeed;

static int vc_write_all(int fd, const u8* p, int n) {
    int off = 0;
    while (off < n) { long w = write(fd, p + off, (unsigned long)(n - off)); if (w <= 0) return 0; off += (int)w; }
    return 1;
}

/* папка = <папка журнала>/voice; 1 — готова */
static int vc_prepare_dir(void) {
    unsigned i, last = 0, k; static const char tail[] = "/voice";
    if (g_vc_dir[0]) return 1;
    dlog("voice: готовлю папку");                 /* заодно инициализирует файл журнала и g_flog */
    if (!g_flog[0]) return 0;
    for (i = 0; g_flog[i] && i < sizeof g_vc_dir - 16; i++) { g_vc_dir[i] = g_flog[i]; if (g_flog[i] == '/') last = i; }
    g_vc_dir[last] = 0;
    for (k = 0; tail[k] && last + k + 1 < sizeof g_vc_dir; k++) g_vc_dir[last + k] = tail[k];
    g_vc_dir[last + k] = 0;
    mkdir(g_vc_dir, 0777);
    return 1;
}

static const char* vc_kind(const VcPkt* p) {
    if (p->effect == 1) return "radio";
    if (p->effect == 2) return "phone";
    return p->use3d ? "near" : "global";
}

/* ───────────── отправка в Telegram (sendVoice) ─────────────
   Закрытый файл-реплика (см. vc_sess_close) ставится в очередь, отдельный поток шлёт его в Telegram как голосовое сообщение с подписью:
   ник говорящего, вид (рядом/рация/телефон/общий), длительность. Ogg Opus моно 48 кГц — ровно тот формат, который Telegram показывает как
   голосовое, ничего не перекодируем. Отправка — так же, как у /screen: JNI → java.net.HttpURLConnection (своего TLS в C нет), в фоновом потоке.
   Токен бота и чат берутся из файла <папка voice>/tg.txt (строки: токен, chat_id, необязательная метка аккаунта) или приходят от скрипта
   через HTTP /voice/cfg (на диск не пишутся). Очередь короткая: если Telegram не успевает, самые новые реплики отбрасываются, а не копятся. */
#define VC_TG_Q        32
#define VC_TG_MIN_MS   400                   /* короче — щелчки и обрывки, не шлём */
#define VC_UP_HEAD     4096                  /* место под заголовки multipart перед файлом (подпись + кнопки reply_markup) */
#define VC_UP_MAX      (3 * 1024 * 1024)     /* файл больше (>10 мин речи) не шлём; в .bss — память занимается только по факту записи */
typedef struct { char path[360]; char nick[48]; const char* kind; int pid, durMs; float dist; } VcTgJob;   /* pid < 0 — не игрок; dist < 0 — расстояние неизвестно */
static VcTgJob g_vc_tgq[VC_TG_Q];
static volatile unsigned g_vc_tgHead, g_vc_tgTail;
static VcTgJob g_vc_last;                    /* последняя закрытая реплика — для /voice tg test */
static volatile int g_vc_haveLast;
static volatile int g_vc_tg;                 /* 1 — слать в Telegram */
static volatile int g_vc_tgCfg;              /* 1 — токен и чат заданы */
static char g_vc_tgTok[160], g_vc_tgChat[48], g_vc_tgWho[48];
#define VC_KB_MAX 1400
static char g_vc_tgKb[2][VC_KB_MAX];         /* reply_markup для sendVoice (JSON, только ASCII) — присылает скрипт; два буфера: отправитель не видит недописанную строку */
static volatile int g_vc_tgKbIdx;            /* какой буфер актуален */
static volatile int g_vc_tgKbSet;            /* 1 — клавиатура задана */
static volatile unsigned g_vc_tgSent, g_vc_tgFail, g_vc_tgDrop;
static volatile int g_vc_tgCode;
static char g_vc_tgMsg[200];
static volatile int g_vc_reqRec, g_vc_reqTgFlag, g_vc_reqTest;     /* заявки писателю от HTTP-потока: IL2CPP и файлы трогает только он */
static u8 g_vc_upBuf[VC_UP_HEAD + VC_UP_MAX + 512];

static void vc_tg_msg(const char* m) { unsigned i; for (i = 0; m && m[i] && i < sizeof g_vc_tgMsg - 1; i++) g_vc_tgMsg[i] = m[i]; g_vc_tgMsg[i] = 0; }
static const char* vc_kind_ru(const char* k) {
    return seq(k, "radio") ? "рация" : seq(k, "phone") ? "телефон" : seq(k, "global") ? "общий канал" : "рядом";
}
static void vc_html(Buf* b, const char* t) {
    for (; *t; t++) {
        if (*t == '&') b_s(b, "&amp;"); else if (*t == '<') b_s(b, "&lt;"); else if (*t == '>') b_s(b, "&gt;"); else b_ch(b, *t);
    }
}
/* Подпись (HTML): «[аккаунт] слышит:\n🎙 Ник[id] · 📏 4.2 м · 3.2 с». Вид «рядом» заменён расстоянием (формат как nearFmt в Code3: до 10 м — один знак, дальше — целое);
   нет позиций — пишем «рядом»; рация/телефон/общий канал расстояния не имеют и остаются словом. */
static void vc_tg_caption(const VcTgJob* j, const char* who, char* out, unsigned cap) {
    Buf b; b_init(&b, out, cap);
    if (who[0]) { b_s(&b, "<b>"); vc_html(&b, who); b_s(&b, "</b> слышит:\n"); }
    b_s(&b, "🎙 ");
    if (j->nick[0]) { b_s(&b, "<b>"); vc_html(&b, j->nick); b_s(&b, "</b>"); if (j->pid >= 0) { b_s(&b, "["); b_int(&b, j->pid); b_s(&b, "]"); } }
    else if (j->pid >= 0) { b_s(&b, "игрок["); b_int(&b, j->pid); b_s(&b, "]"); }
    else b_s(&b, "голос");
    b_s(&b, " · ");
    if (seq(j->kind, "near") && j->dist >= 0) { b_s(&b, "📏 "); b_fix(&b, j->dist, j->dist < 10 ? 1 : 0); b_s(&b, " м"); }
    else b_s(&b, vc_kind_ru(j->kind));
    b_s(&b, " · "); b_int(&b, j->durMs / 1000); b_ch(&b, '.'); b_int(&b, (j->durMs % 1000) / 100); b_s(&b, " с");
}

static void vc_tg_push(const VcTgJob* src) {
    unsigned h = g_vc_tgHead;
    if (h - __atomic_load_n(&g_vc_tgTail, __ATOMIC_ACQUIRE) >= VC_TG_Q) { __atomic_fetch_add(&g_vc_tgDrop, 1, __ATOMIC_RELAXED); return; }
    g_vc_tgq[h & (VC_TG_Q - 1)] = *src;
    __atomic_store_n(&g_vc_tgHead, h + 1, __ATOMIC_RELEASE);
}
/* Зовётся из писателя при закрытии файла (единственный поставщик очереди; из заявки /voice tg test — тоже он). */
static void vc_tg_enqueue(const char* path, const char* nick, const char* kind, int pid, int durMs, float dist) {
    VcTgJob j; unsigned i;
    for (i = 0; path[i] && i < sizeof j.path - 1; i++) j.path[i] = path[i];
    j.path[i] = 0;
    for (i = 0; nick[i] && i < sizeof j.nick - 1; i++) j.nick[i] = nick[i];
    j.nick[i] = 0;
    j.kind = kind; j.pid = pid; j.durMs = durMs; j.dist = dist;
    g_vc_last = j; g_vc_haveLast = 1;
    if (!g_vc_tg || !g_vc_tgCfg || durMs < VC_TG_MIN_MS) return;
    vc_tg_push(&j);
}

typedef int (*vcj_getvms_t)(void**, int, int*);
typedef int (*vcj_getenv_t)(void*, void**, int);
typedef int (*vcj_attach_t)(void*, void**, void*);
typedef int (*vcj_detach_t)(void*);
typedef void* (*vcj_find_t)(void*, const char*);
typedef void (*vcj_clr_t)(void*);
typedef void* (*vcj_mid_t)(void*, void*, const char*, const char*);
typedef void* (*vcj_newobj_t)(void*, void*, void*, ...);
typedef void* (*vcj_callo_t)(void*, void*, void*, ...);
typedef int (*vcj_calli_t)(void*, void*, void*, ...);
typedef void (*vcj_callv_t)(void*, void*, void*, ...);
typedef void* (*vcj_str_t)(void*, const char*);
typedef void* (*vcj_newarr_t)(void*, int);
typedef void (*vcj_getreg_t)(void*, void*, int, int, signed char*);
typedef void (*vcj_setreg_t)(void*, void*, int, int, const signed char*);
typedef unsigned char (*vcj_exc_t)(void*);
#define VCJ(env, T, idx) ((T)((*(void***)(env))[idx]))
#define VCJV(vm, T, idx) ((T)((*(void***)(vm))[idx]))
/* индексы JNINativeInterface — те же, что в hassle_screen.c: FindClass 6, ExceptionClear 17, GetMethodID 33, CallObjectMethod 34, CallIntMethod 49,
   CallVoidMethod 61, NewObject 28, NewStringUTF 167, NewByteArray 176, GetByteArrayRegion 200, SetByteArrayRegion 208, ExceptionCheck 228;
   JavaVM: AttachCurrentThread 4, DetachCurrentThread 5, GetEnv 6 */
static int vcj_clean(void* env) { if (VCJ(env, vcj_exc_t, 228)(env)) { VCJ(env, vcj_clr_t, 17)(env); return 0; } return 1; }
static void* vcj_get_vm(void) {
    vcj_getvms_t f = NULL; void *vm = NULL, *h; int n = 0;
    h = dlopen(NULL, RTLD_NOW_); if (h) *(void**)&f = dlsym(h, "JNI_GetCreatedJavaVMs");
    if (!f) { h = dlopen("libnativehelper.so", RTLD_NOW_ | 4); if (h) *(void**)&f = dlsym(h, "JNI_GetCreatedJavaVMs"); }
    if (!f) { h = dlopen("libart.so", RTLD_NOW_ | 4); if (h) *(void**)&f = dlsym(h, "JNI_GetCreatedJavaVMs"); }
    if (!f) return NULL;
    if (f(&vm, 1, &n) != 0 || n < 1) return NULL;
    return vm;
}
/* ответ Telegram: "description":"..." (короткий текст для журнала) */
static void vc_tg_desc(const char* r, unsigned n, char* out, unsigned cap) {
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
static void vc_mp_field(Buf* b, const char* boundary, const char* name) {
    b_s(b, "--"); b_s(b, boundary); b_s(b, "\r\nContent-Disposition: form-data; name=\""); b_s(b, name); b_s(b, "\"\r\n\r\n");
}
/* Собирает multipart в g_vc_upBuf: sendVoice (chat_id, caption, parse_mode, duration, voice) или sendDocument. Возвращает длину тела или 0. */
static unsigned long vc_tg_build(const char* boundary, const char* chat, const VcTgJob* j, const char* cap, const char* kb, int asDoc) {
    Buf b; int fd, tooBig = 0; long off, got = 0;
    fd = open(j->path, 0);
    if (fd < 0) { vc_tg_msg("не открыть файл реплики"); return 0; }
    b_init(&b, (char*)g_vc_upBuf, VC_UP_HEAD);
    vc_mp_field(&b, boundary, "chat_id"); b_s(&b, chat); b_s(&b, "\r\n");
    vc_mp_field(&b, boundary, "caption"); b_s(&b, cap); b_s(&b, "\r\n");
    vc_mp_field(&b, boundary, "parse_mode"); b_s(&b, "HTML\r\n");
    if (kb && kb[0]) { vc_mp_field(&b, boundary, "reply_markup"); b_s(&b, kb); b_s(&b, "\r\n"); }   /* кнопки под голосовым, как у /screen */
    if (!asDoc) { vc_mp_field(&b, boundary, "duration"); b_int(&b, (j->durMs + 500) / 1000); b_s(&b, "\r\n"); }
    b_s(&b, "--"); b_s(&b, boundary); b_s(&b, "\r\nContent-Disposition: form-data; name=\""); b_s(&b, asDoc ? "document" : "voice");
    b_s(&b, "\"; filename=\"voice.ogg\"\r\nContent-Type: audio/ogg\r\n\r\n");
    if (b.n + 8 >= VC_UP_HEAD) { close(fd); vc_tg_msg("подпись слишком длинная"); return 0; }
    off = (long)b.n;
    for (;;) {
        long room = (long)sizeof g_vc_upBuf - 256 - off, n;
        if (room <= 0) { char t; if (read(fd, &t, 1) > 0) tooBig = 1; break; }
        n = read(fd, g_vc_upBuf + off, (unsigned long)room);
        if (n <= 0) break;
        off += n; got += n;
    }
    close(fd);
    if (tooBig) { vc_tg_msg("реплика слишком длинная для отправки"); return 0; }
    if (got < 64) { vc_tg_msg("файл реплики пустой"); return 0; }
    b_init(&b, (char*)g_vc_upBuf + off, 256);
    b_s(&b, "\r\n--"); b_s(&b, boundary); b_s(&b, "--\r\n");
    return (unsigned long)off + b.n;
}

/* Сам POST. HTTP-код (200 — принят) или 0 при сбое до ответа (причина в g_vc_tgMsg). *retry — «retry_after» из ответа 429. */
static int vc_tg_post(const char* tok, const char* method, const char* boundary, unsigned long total, int* retry) {
    void *vm, *env = NULL, *cURL = NULL, *cConn = NULL, *cHttp = NULL, *cOS = NULL, *cIS = NULL;
    void *jurl, *jstr, *conn, *os = NULL, *in = NULL, *arr = NULL, *jm, *jct, *jctv;
    void *mNew, *mOpen, *mCT, *mRT, *mDoOut, *mProp, *mGetOut, *mMeth, *mFix, *mCode, *mErr, *mGetIn, *mDisc, *mWrite, *mFlush, *mClose, *mRead, *mIClose;
    int attached = 0, rc, code = 0; unsigned long sent = 0; char url[300], ct[120]; Buf b;
    char resp[2048]; unsigned rn = 0;
    vcj_find_t FindClass; vcj_mid_t GetMid; vcj_callo_t CallObj; vcj_calli_t CallInt; vcj_callv_t CallVoid;
#define VCJ_FAIL(m) do { vc_tg_msg(m); code = 0; goto out; } while (0)
    *retry = 0;
    vm = vcj_get_vm(); if (!vm) { vc_tg_msg("нет JavaVM (JNI_GetCreatedJavaVMs не найдена)"); return 0; }
    rc = VCJV(vm, vcj_getenv_t, 6)(vm, &env, 0x00010006);
    if (rc == -2) { if (VCJV(vm, vcj_attach_t, 4)(vm, &env, NULL) != 0 || !env) { vc_tg_msg("AttachCurrentThread не удался"); return 0; } attached = 1; }
    else if (rc != 0 || !env) { vc_tg_msg("GetEnv не удался"); return 0; }
    FindClass = VCJ(env, vcj_find_t, 6); GetMid = VCJ(env, vcj_mid_t, 33);
    CallObj = VCJ(env, vcj_callo_t, 34); CallInt = VCJ(env, vcj_calli_t, 49); CallVoid = VCJ(env, vcj_callv_t, 61);
    cURL = FindClass(env, "java/net/URL"); cConn = FindClass(env, "java/net/URLConnection"); cHttp = FindClass(env, "java/net/HttpURLConnection");
    cOS = FindClass(env, "java/io/OutputStream"); cIS = FindClass(env, "java/io/InputStream");
    if (!cURL || !cConn || !cHttp || !cOS || !cIS || !vcj_clean(env)) VCJ_FAIL("нет нужных классов java.net / java.io");
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
    if (!vcj_clean(env) || !mNew || !mOpen || !mCT || !mRT || !mDoOut || !mProp || !mGetOut || !mGetIn || !mMeth || !mFix || !mCode || !mErr || !mDisc || !mWrite || !mFlush || !mClose || !mRead || !mIClose)
        VCJ_FAIL("не нашлись методы Java (HttpURLConnection)");
    b_init(&b, url, sizeof url); b_s(&b, "https://api.telegram.org/bot"); b_s(&b, tok); b_ch(&b, '/'); b_s(&b, method);
    b_init(&b, ct, sizeof ct); b_s(&b, "multipart/form-data; boundary="); b_s(&b, boundary);
    jstr = VCJ(env, vcj_str_t, 167)(env, url); if (!jstr) VCJ_FAIL("NewStringUTF");
    jurl = VCJ(env, vcj_newobj_t, 28)(env, cURL, mNew, jstr); if (!jurl || !vcj_clean(env)) VCJ_FAIL("new URL() не удался");
    conn = CallObj(env, jurl, mOpen); if (!conn || !vcj_clean(env)) VCJ_FAIL("openConnection не удался");
    CallVoid(env, conn, mCT, 15000); CallVoid(env, conn, mRT, 60000); if (!vcj_clean(env)) VCJ_FAIL("таймауты");
    jm = VCJ(env, vcj_str_t, 167)(env, "POST"); CallVoid(env, conn, mMeth, jm); if (!vcj_clean(env)) VCJ_FAIL("setRequestMethod");
    CallVoid(env, conn, mDoOut, 1); if (!vcj_clean(env)) VCJ_FAIL("setDoOutput");
    jct = VCJ(env, vcj_str_t, 167)(env, "Content-Type"); jctv = VCJ(env, vcj_str_t, 167)(env, ct);
    CallVoid(env, conn, mProp, jct, jctv); if (!vcj_clean(env)) VCJ_FAIL("setRequestProperty");
    CallVoid(env, conn, mFix, (int)total); if (!vcj_clean(env)) VCJ_FAIL("setFixedLengthStreamingMode");
    os = CallObj(env, conn, mGetOut); if (!os || !vcj_clean(env)) VCJ_FAIL("нет соединения с api.telegram.org (getOutputStream)");
    arr = VCJ(env, vcj_newarr_t, 176)(env, 65536); if (!arr) VCJ_FAIL("NewByteArray");
    while (sent < total) {
        unsigned long n = total - sent < 65536 ? total - sent : 65536;
        VCJ(env, vcj_setreg_t, 208)(env, arr, 0, (int)n, (const signed char*)(g_vc_upBuf + sent));
        CallVoid(env, os, mWrite, arr, 0, (int)n);
        if (!vcj_clean(env)) VCJ_FAIL("обрыв при отправке данных");
        sent += n;
    }
    CallVoid(env, os, mFlush); CallVoid(env, os, mClose); if (!vcj_clean(env)) VCJ_FAIL("обрыв при завершении отправки");
    code = CallInt(env, conn, mCode); if (!vcj_clean(env)) VCJ_FAIL("нет ответа от Telegram (getResponseCode)");
    in = CallObj(env, conn, code >= 400 ? mErr : mGetIn);
    if (!vcj_clean(env)) in = NULL;
    if (in) {
        for (;;) {
            int r = CallInt(env, in, mRead, arr);
            if (!vcj_clean(env) || r < 0) break;
            if (r == 0) continue;
            if (rn + (unsigned)r > sizeof resp - 1) r = (int)(sizeof resp - 1 - rn);
            if (r > 0) { VCJ(env, vcj_getreg_t, 200)(env, arr, 0, r, (signed char*)(resp + rn)); rn += (unsigned)r; }
            if (rn >= sizeof resp - 1) break;
        }
        CallVoid(env, in, mIClose); vcj_clean(env);
    }
    resp[rn] = 0;
    if (code == 429) {                                                 /* "retry_after":N */
        static const char mk[] = "\"retry_after\":"; unsigned q;
        for (q = 0; q + sizeof mk < rn; q++) {
            if (resp[q] == '"' && sprefix(resp + q, mk)) {
                int v = 0; q += sizeof mk - 1;
                while (q < rn && resp[q] >= '0' && resp[q] <= '9' && v < 1000) { v = v * 10 + (resp[q] - '0'); q++; }
                *retry = v; break;
            }
        }
    }
    if (code != 200) { char d[160]; vc_tg_desc(resp, rn, d, sizeof d); vc_tg_msg(d[0] ? d : "пустой ответ"); }
    CallVoid(env, conn, mDisc); vcj_clean(env);
out:
    if (attached) VCJV(vm, vcj_detach_t, 5)(vm);
    return code;
#undef VCJ_FAIL
}

static unsigned g_vc_boundSeq;
static void vc_tg_send(const VcTgJob* j) {
    char cap[700], bd[64], tok[160], chat[48], who[48], kb[VC_KB_MAX]; Buf b; unsigned long total; int code = 0, asDoc = 0, retry = 0, att, i;
    if (!g_vc_tgCfg) return;
    for (i = 0; g_vc_tgTok[i] && i < 159; i++) tok[i] = g_vc_tgTok[i];
    tok[i] = 0;
    for (i = 0; g_vc_tgChat[i] && i < 47; i++) chat[i] = g_vc_tgChat[i];
    chat[i] = 0;
    for (i = 0; g_vc_tgWho[i] && i < 47; i++) who[i] = g_vc_tgWho[i];
    who[i] = 0;
    {   const char* ks = g_vc_tgKb[__atomic_load_n(&g_vc_tgKbIdx, __ATOMIC_ACQUIRE) & 1];
        for (i = 0; ks[i] && i < VC_KB_MAX - 1; i++) kb[i] = ks[i];
        kb[i] = 0; }
    if (!tok[0] || !chat[0]) return;
    g_vc_tgMsg[0] = 0;
    b_init(&b, bd, sizeof bd); b_s(&b, "----hassle"); b_int(&b, time(NULL)); b_int(&b, (long)++g_vc_boundSeq);
    vc_tg_caption(j, who, cap, sizeof cap);
    total = vc_tg_build(bd, chat, j, cap, kb, 0);
    for (att = 0; total && att < 4; att++) {
        code = vc_tg_post(tok, asDoc ? "sendDocument" : "sendVoice", bd, total, &retry);
        if (code == 200) break;
        if (code == 429) {                                              /* слишком часто: ждём, сколько просит Telegram, и повторяем то же тело */
            int w = retry < 1 ? 3 : retry > 30 ? 30 : retry;
            dlog_i("voice: Telegram 429, жду секунд: ", w);
            while (w-- > 0) usleep(1000000);
            continue;
        }
        if (code == 400 && kb[0]) {                                     /* не приняли кнопки — голосовое важнее: шлём то же без них */
            dlog2("voice: 400 с кнопками, шлю без них: ", g_vc_tgMsg);
            kb[0] = 0; total = vc_tg_build(bd, chat, j, cap, kb, asDoc);
            continue;
        }
        if (code == 400 && !asDoc) {                                    /* не принял как голосовое — отправим файлом */
            dlog2("voice: sendVoice 400: ", g_vc_tgMsg);
            asDoc = 1; total = vc_tg_build(bd, chat, j, cap, kb, 1);
            continue;
        }
        break;
    }
    g_vc_tgCode = code;
    if (code == 200) { __atomic_fetch_add(&g_vc_tgSent, 1, __ATOMIC_RELAXED); }
    else { __atomic_fetch_add(&g_vc_tgFail, 1, __ATOMIC_RELAXED); dlog_i("voice: Telegram не принял, код ", code); dlog2("voice: причина: ", g_vc_tgMsg); }
}
static void* vc_tg_main(void* arg) {
    (void)arg;
    if (!hm_wait_started()) return NULL;                                /* konstt_off — мод выключен целиком */
    for (;;) {
        unsigned t = g_vc_tgTail;
        if (t == __atomic_load_n(&g_vc_tgHead, __ATOMIC_ACQUIRE)) { usleep(250000); continue; }
        {
            static VcTgJob j;
            j = g_vc_tgq[t & (VC_TG_Q - 1)];
            __atomic_store_n(&g_vc_tgTail, t + 1, __ATOMIC_RELEASE);
            if (g_vc_tg && g_vc_tgCfg) { vc_tg_send(&j); usleep(1000000); }     /* пауза: у Telegram лимит ~20 сообщений в минуту на чат */
        }
    }
    return NULL;
}

static void vc_sess_close(VcSess* s) {
    if (!s->active) return;
    if (s->heldLen > 0) {                          /* последний пакет — со флагом конца потока */
        u8 pg[VC_PAGE_MAX]; int n;
        s->gp += (vc_u64)s->heldSamples;
        n = vc_ogg_page(pg, 4, s->gp, s->serial, s->seq++, s->held, s->heldLen);
        if (vc_write_all(s->fd, pg, n)) __atomic_fetch_add(&g_vc_kb, (unsigned)((n + 512) / 1024), __ATOMIC_RELAXED);
        s->heldLen = 0;
    }
    close(s->fd);
    s->fd = -1; s->active = 0;
    if (s->pkts > 0) {                             /* файл закрыт и целый — отдаём на отправку в Telegram (если включено) */
        VcPkt kp; kp.effect = s->effect; kp.use3d = s->use3d;
        if (s->dist < 0 && s->use3d && s->attach == 2 && s->effect == 0) s->dist = vc_dist(s->pid);   /* в начале данных не было — пробуем ещё раз */
        vc_tg_enqueue(s->path, s->nick, vc_kind(&kp), (s->use3d && s->attach == 2) ? (int)s->pid : -1, (int)(((s->gp > VC_PRESKIP ? s->gp - VC_PRESKIP : 0) * 1000) / 48000), s->dist);
    }
}

static int vc_sess_open(VcSess* s, const VcPkt* p) {
    char stamp[40], nick[48] = "", clean[32] = "", name[64], path[360], idp[24]; Buf b;
    u8 pg[VC_PAGE_MAX], tg[512]; int n, k, fd = -1;
    if (!vc_prepare_dir()) return 0;
    if (p->use3d && p->attach == 2 && p->effect == 0) vc_nick(p->pid, nick, sizeof nick);   /* ник известен только у позиционного голоса игрока */
    vc_san(nick, clean, sizeof clean);
    vc_stamp(time(NULL), stamp);
    b_init(&b, idp, sizeof idp);
    if (p->use3d && p->attach == 2) { b_ch(&b, 'p'); b_int(&b, p->pid); } else { b_ch(&b, 's'); b_int(&b, p->bass); }
    for (k = 0; k < 20; k++) {                     /* имя занято (тот же говорящий в ту же секунду) — добавляем счётчик */
        b_init(&b, name, sizeof name);
        b_s(&b, stamp); b_ch(&b, '_'); b_s(&b, vc_kind(p)); b_ch(&b, '_'); b_s(&b, idp);
        if (clean[0]) { b_ch(&b, '_'); b_s(&b, clean); }
        if (k) { b_ch(&b, '_'); b_int(&b, k + 1); }
        b_s(&b, ".opus");
        b_init(&b, path, sizeof path); b_s(&b, g_vc_dir); b_ch(&b, '/'); b_s(&b, name);
        fd = open(path, 0x2C1, 0666);              /* O_WRONLY|O_CREAT|O_EXCL|O_TRUNC */
        if (fd >= 0) break;
    }
    if (fd < 0) {
        if (!(__atomic_fetch_add(&g_vc_errs, 1, __ATOMIC_RELAXED) % 50)) dlog2("voice: не открыть файл: ", path);
        return 0;
    }
    s->fd = fd; s->active = 1; s->st = p->st;
    s->lastMs = s->startMs = p->ms;
    s->gp = VC_PRESKIP; s->seq = 0; s->pkts = 0; s->heldLen = 0; s->heldSamples = 0;
    s->pid = p->pid; s->use3d = p->use3d; s->attach = p->attach; s->effect = p->effect; s->bass = p->bass;
    s->dist = (p->use3d && p->attach == 2 && p->effect == 0) ? vc_dist(p->pid) : -1.0f;   /* расстояние на момент начала фразы (как в Code3: на момент сообщения) */
    g_vc_serialSeed = g_vc_serialSeed * 1664525u + 1013904223u + (u32)p->ms;
    s->serial = g_vc_serialSeed;
    { u32 i; for (i = 0; path[i] && i < sizeof s->path - 1; i++) s->path[i] = path[i]; s->path[i] = 0;
      for (i = 0; nick[i] && i < sizeof s->nick - 1; i++) s->nick[i] = nick[i]; s->nick[i] = 0; }
    {   /* заголовки: OpusHead (первая страница) и OpusTags (вторая) */
        char title[96], artist[48], com[40]; Buf t;
        b_init(&t, title, sizeof title);
        if (clean[0]) b_s(&t, nick); else b_s(&t, vc_kind(p));
        if (p->use3d && p->attach == 2) { b_s(&t, " #"); b_int(&t, p->pid); }
        b_s(&t, " "); b_s(&t, stamp); b_s(&t, " UTC");
        b_init(&t, artist, sizeof artist); b_s(&t, nick);
        b_init(&t, com, sizeof com); b_s(&t, vc_kind(p));
        n = vc_ogg_page(pg, 2, 0, s->serial, s->seq++, tg, vc_opus_head(tg));
        if (!vc_write_all(fd, pg, n)) goto bad;
        n = vc_ogg_page(pg, 0, 0, s->serial, s->seq++, tg, vc_opus_tags(tg, (int)sizeof tg, title, artist, com));
        if (!vc_write_all(fd, pg, n)) goto bad;
    }
    __atomic_fetch_add(&g_vc_files, 1, __ATOMIC_RELAXED);
    return 1;
bad:
    close(fd); s->active = 0; s->fd = -1;
    __atomic_fetch_add(&g_vc_errs, 1, __ATOMIC_RELAXED);
    return 0;
}

/* Один принятый пакет → нужный файл (поток писателя). */
static void vc_on_packet(const VcPkt* p) {
    int samples = vc_opus_samples(p->data, (int)p->len), i, free_ = -1, oldest = 0; VcSess* s = NULL;
    if (!samples) { __atomic_fetch_add(&g_vc_bad, 1, __ATOMIC_RELAXED); return; }
    for (i = 0; i < VC_SESS; i++) {
        if (g_vc_sess[i].active) {
            if (g_vc_sess[i].lastMs < g_vc_sess[oldest].lastMs || !g_vc_sess[oldest].active) oldest = i;
            if (g_vc_sess[i].st == p->st) { s = &g_vc_sess[i]; break; }
        } else if (free_ < 0) free_ = i;
    }
    if (s && (p->ms - s->lastMs > VC_GAP_MS || p->ms - s->startMs > VC_MAX_SEG_MS ||
              s->pid != p->pid || s->use3d != p->use3d || s->effect != p->effect)) { vc_sess_close(s); s = NULL; free_ = (int)(s ? 0 : (long)(&g_vc_sess[0] == NULL)); }
    if (!s) {
        for (i = 0; i < VC_SESS; i++) if (!g_vc_sess[i].active) { free_ = i; break; }
        if (free_ < 0) { vc_sess_close(&g_vc_sess[oldest]); free_ = oldest; }       /* все заняты — вытесняем самый давний */
        s = &g_vc_sess[free_];
        if (!vc_sess_open(s, p)) return;
    }
    if (s->heldLen > 0) {                                                             /* предыдущий пакет теперь не последний */
        u8 pg[VC_PAGE_MAX]; int n;
        s->gp += (vc_u64)s->heldSamples;
        n = vc_ogg_page(pg, 0, s->gp, s->serial, s->seq++, s->held, s->heldLen);
        if (!vc_write_all(s->fd, pg, n)) { __atomic_fetch_add(&g_vc_errs, 1, __ATOMIC_RELAXED); vc_sess_close(s); return; }
        __atomic_fetch_add(&g_vc_kb, (unsigned)((n + 512) / 1024), __ATOMIC_RELAXED);
    }
    memcpy(s->held, p->data, (unsigned long)p->len);
    s->heldLen = (int)p->len; s->heldSamples = samples;
    s->lastMs = p->ms; s->pkts++;
}

static long long vc_now_ms(void) { struct { long s, n; } ts; clock_gettime(1, &ts); return (long long)ts.s * 1000 + ts.n / 1000000; }

/* один шаг писателя: забрать пакеты из кольца, закрыть замолчавшие файлы; возвращает, сколько файлов ещё открыто */
static int vc_tick(void) {
    static VcPkt tmp; int i, open_ = 0; long long now;
    for (;;) {
        int spins = 0;
        while (!vc_lock(2000)) { if (++spins > 200) usleep(50); }
        if (g_vc_tail == g_vc_head) { vc_unlock(); break; }
        tmp = g_vc_ring[g_vc_tail & (VC_RING - 1)];
        __atomic_store_n(&g_vc_tail, g_vc_tail + 1, __ATOMIC_RELEASE);
        vc_unlock();
        vc_on_packet(&tmp);
    }
    now = vc_now_ms();
    for (i = 0; i < VC_SESS; i++) {
        VcSess* s = &g_vc_sess[i];
        if (!s->active) continue;
        if (__atomic_load_n(&g_vc_flush, __ATOMIC_ACQUIRE) || now - s->lastMs > VC_GAP_MS) vc_sess_close(s);
        else open_++;
    }
    if (__atomic_load_n(&g_vc_flush, __ATOMIC_ACQUIRE) && !open_) __atomic_store_n(&g_vc_flush, 0, __ATOMIC_RELEASE);
    return open_;
}

/* ───────────── установка перехвата (только arm64) ───────────── */
#ifdef VC_HOOK
#define VC_TRAMP_WORDS 24
/* Инструкция зависит от своего адреса (при переносе в заглушку сломается)? */
static int vc_pc_rel(u32 w) {
    if ((w & 0x1F000000u) == 0x10000000u) return 1;              /* ADR, ADRP */
    if ((w & 0x3B000000u) == 0x18000000u) return 1;              /* LDR / LDRSW / PRFM (literal) */
    if ((w & 0x7C000000u) == 0x14000000u) return 1;              /* B, BL */
    if ((w & 0xFF000010u) == 0x54000000u) return 1;              /* B.cond */
    if ((w & 0x7E000000u) == 0x34000000u) return 1;              /* CBZ, CBNZ */
    if ((w & 0x7E000000u) == 0x36000000u) return 1;              /* TBZ, TBNZ */
    return 0;
}
/* Есть ли среди n слов (функция по адресу base) вызов BL на адрес target? */
static int vc_has_bl_to(const u32* c, int n, uptr base, uptr target) {
    int i;
    for (i = 0; i < n; i++) {
        long imm;
        if ((c[i] & 0xFC000000u) != 0x94000000u) continue;
        imm = (long)(c[i] & 0x03FFFFFFu);
        if (imm & 0x02000000L) imm -= 0x04000000L;
        if (base + (uptr)i * 4 + (uptr)(imm * 4) == target) return 1;
    }
    return 0;
}
/* Заглушка (arm64), 96 байт. Вход — абсолютным прыжком из начала PutData; регистры как при входе (x0 — поток, x1 — массив, w2 — длина,
   x3 — буфер PCM, w4 — размер кадра, x5 — MethodInfo).
     0  stp x0, x1, [sp, #-80]!       1  stp x2, x3, [sp, #16]      2  stp x4, x5, [sp, #32]     3  stp x6, x7, [sp, #48]
     4  str x30, [sp, #64]            5  ldr x16, =vc_hook (слово 18)                            6  blr x16
     7  ldr x30, [sp, #64]            8  ldp x6, x7, [sp, #48]       9  ldp x4, x5, [sp, #32]     10 ldp x2, x3, [sp, #16]
     11 ldp x0, x1, [sp], #80
     12-15 четыре затёртые инструкции пролога (позиционно независимые — проверено vc_pc_rel)
     16 ldr x16, =Update+16 (слово 20)  17 br x16
     18-19 адрес vc_hook    20-21 адрес PutData+16 */
/* ВАЖНО: x2..x7 лежат в кадре заглушки строго по смещениям 16/32/48 (x30 — 64); раньше x4/x5 и x6/x7 были записаны по 64 и 96:
   x30 затирал сохранённый x4 (frameSize = младшие 32 бита адреса возврата → гигантский буфер, malloc до ~2,5 ГБ), а x6/x7 портили кадр вызывающего. */
static void vc_make_tramp(u32* t, const u32* orig4, uptr hook, uptr resume) {
    int i;
    for (i = 0; i < VC_TRAMP_WORDS; i++) t[i] = 0;
    t[0] = 0xA9BB07E0u; t[1] = 0xA9010FE2u; t[2] = 0xA90217E4u; t[3] = 0xA9031FE6u;
    t[4] = 0xF90023FEu; t[5] = 0x580001B0u; t[6] = 0xD63F0200u;
    t[7] = 0xF94023FEu; t[8] = 0xA9431FE6u; t[9] = 0xA94217E4u; t[10] = 0xA9410FE2u; t[11] = 0xA8C507E0u;
    for (i = 0; i < 4; i++) t[12 + i] = orig4[i];
    t[16] = 0x58000090u; t[17] = 0xD61F0200u;
    t[18] = (u32)(hook & 0xFFFFFFFFu);   t[19] = (u32)(((unsigned long long)hook) >> 32);
    t[20] = (u32)(resume & 0xFFFFFFFFu); t[21] = (u32)(((unsigned long long)resume) >> 32);
}
/* 16 байт, которыми заменяется начало PutData: ldr x16,[pc,#8]; br x16; .quad адрес заглушки */
static void vc_make_entry(u32* e, uptr tramp) {
    e[0] = 0x58000050u; e[1] = 0xD61F0200u;
    e[2] = (u32)(tramp & 0xFFFFFFFFu); e[3] = (u32)(((unsigned long long)tramp) >> 32);
}
/* Проверка места патча по словам функции (чистая: проверяется отдельно на настоящем коде libil2cpp). 0 — можно, иначе номер причины. */
static int vc_check_site(const u32* code, int n, uptr fn, uptr decodeFn) {
    int i;
    if (n < 48) return 1;
    for (i = 0; i < 4; i++) if (vc_pc_rel(code[i])) return 2;
    if (!vc_has_bl_to(code, 48, fn, decodeFn)) return 3;
    return 0;
}
static const char* vc_site_why(int r) {
    return r == 1 ? "мало кода для проверки" : r == 2 ? "пролог PutData зависит от адреса — перенести нельзя" :
           r == 3 ? "в PutData нет вызова Opus.Decode (другая сборка игры?)" : "?";
}

static void* g_vc_veneer;                                   /* страница с заглушкой (после снятия перехвата остаётся — вдруг поток ещё внутри) */
static uptr g_vc_site;                                      /* адрес начала PutData */
static u32 g_vc_orig[4];                                    /* его исходные 4 слова */

static void vc_flush_cache(void* p, unsigned n) {          /* синхронизация кэшей после записи кода */
#if defined(__aarch64__)
    uptr ctr, dl, il, a, e;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    dl = 4u << ((ctr >> 16) & 15u); il = 4u << (ctr & 15u);
    e = (uptr)p + n;
    for (a = (uptr)p & ~(dl - 1); a < e; a += dl) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uptr)p & ~(il - 1); a < e; a += il) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
#else
    (void)p; (void)n;
#endif
}
/* Записать n слов в код игры: короткое окно с правом записи, права возвращаются (r-x); слова — с конца к началу.
   Запасной путь — запись в /proc/self/mem, если mprotect запрещён. */
static int vc_write_code(uptr addr, const u32* w, int n) {
    uptr p0 = addr & ~(uptr)4095, p1 = (addr + (uptr)n * 4 - 1) & ~(uptr)4095;
    unsigned long len = (unsigned long)(p1 - p0) + 4096;
    int i;
    if (mprotect((void*)p0, len, 7) == 0) {
        for (i = n - 1; i >= 0; i--) ((volatile u32*)addr)[i] = w[i];
        vc_flush_cache((void*)addr, (unsigned)n * 4);
        mprotect((void*)p0, len, 5);
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, w, (unsigned long)n * 4, (long)addr); close(fd); }
        if (r == (long)n * 4) { vc_flush_cache((void*)addr, (unsigned)n * 4); return 1; }
    }
    return 0;
}

static int vc_field_off(void* kls, const char* name, int* out) {
    void* f = find_field(kls, name);
    if (!f) return 0;
    *out = A.field_get_offset(f);
    return *out > 0 && *out < 256;
}

static int vc_install_do(void) {
    void *kls, *m, *ko, *md; uptr fn, dec; u32 code[64]; int i, r;
    if (g_vc_state == 1) return 1;
    if (!g_img) return vc_fail("игра ещё не готова");
    kls = A.class_from_name(g_img, "Hassle.Audio", "AudioStream");
    if (!kls) return vc_fail("нет класса Hassle.Audio.AudioStream");
    m = A.class_get_method_from_name(kls, "PutData", 4);
    ko = A.class_from_name(g_img, "Hassle.Audio", "Opus");
    md = ko ? A.class_get_method_from_name(ko, "Decode", 6) : NULL;
    if (!m || !md) return vc_fail("нет AudioStream.PutData или Opus.Decode");
    fn = *(uptr*)m; dec = *(uptr*)md;
    if (!fn || (fn & 3) || !dec || !hm_range_ok((void*)fn, sizeof code)) return vc_fail("адрес PutData нечитаем");
    {   /* смещения полей потока — из метаданных; без них поток не опознать */
        int a = -1, e = -1, p = -1, u = -1, b = -1;
        if (!vc_field_off(kls, "m_Use3D", &u)) return vc_fail("нет поля AudioStream.m_Use3D");
        vc_field_off(kls, "m_AttachType", &a); vc_field_off(kls, "m_EffectType", &e);
        vc_field_off(kls, "m_PlayerIndex", &p); vc_field_off(kls, "m_BASS_Handle", &b);
        g_vc_offAttach = a; g_vc_offEffect = e; g_vc_offPid = p; g_vc_offBass = b;
        { Buf bb; char t[160]; b_init(&bb, t, sizeof t); b_s(&bb, "voice: смещения use3D/attach/effect/pid/bass: "); b_int(&bb, u); b_ch(&bb, '/'); b_int(&bb, a); b_ch(&bb, '/');
          b_int(&bb, e); b_ch(&bb, '/'); b_int(&bb, p); b_ch(&bb, '/'); b_int(&bb, b); dlog(t); }
        g_vc_offUse3d = u;
    }
    for (i = 0; i < 64; i++) code[i] = ((const u32*)fn)[i];
    if (g_vc_veneer && g_vc_site == fn) for (i = 0; i < 4; i++) code[i] = g_vc_orig[i];     /* уже патчили (после неудачного снятия) — исходные слова из сохранённых */
    r = vc_check_site(code, 64, fn, dec);
    if (r) return vc_fail(vc_site_why(r));
    if (!(g_vc_veneer && g_vc_site == fn)) {
        u32 v[VC_TRAMP_WORDS];
        void* page = mmap(NULL, 4096, 3, 0x22, -1, 0);        /* R|W, MAP_PRIVATE|MAP_ANONYMOUS; адрес любой — прыжок абсолютный */
        if (page == (void*)-1 || !page) return vc_fail("не удалось выделить страницу под заглушку");
        vc_make_tramp(v, code, (uptr)vc_hook, fn + 16);
        for (i = 0; i < VC_TRAMP_WORDS; i++) ((u32*)page)[i] = v[i];
        vc_flush_cache(page, sizeof v);
        if (mprotect(page, 4096, 5) != 0) { munmap(page, 4096); return vc_fail("не удалось сделать страницу заглушки исполняемой"); }
        g_vc_veneer = page; g_vc_site = fn;
        for (i = 0; i < 4; i++) g_vc_orig[i] = code[i];
    }
    {
        u32 e[4];
        vc_make_entry(e, (uptr)g_vc_veneer);
        if (!vc_write_code(fn, e, 4)) return vc_fail("запись в код игры запрещена системой");
    }
    g_vc_state = 1;
    dlog("voice: перехват AudioStream.PutData установлен (вход функции, абсолютный прыжок)");
    return 1;
}
static volatile int g_vc_instBusy;
static int vc_install(void) {
    int r;
    if (__sync_lock_test_and_set(&g_vc_instBusy, 1)) return g_vc_state == 1;
    r = vc_install_do();
    __sync_lock_release(&g_vc_instBusy);
    return r;
}
static void vc_unhook(void) {
    if (g_vc_state != 1) return;
    if (vc_write_code(g_vc_site, g_vc_orig, 4)) { g_vc_state = 0; dlog("voice: перехват снят, код игры восстановлен"); }
    else dlog("voice: не удалось вернуть исходные инструкции");
}
#else
static int vc_install(void) { return vc_fail("поддерживается только arm64"); }
static void vc_unhook(void) {}
#endif

/* ───────────── включение/выключение и автозапуск ───────────── */
static void vc_marker_path(char* out, unsigned cap) {
    Buf b; b_init(&b, out, cap); b_s(&b, g_vc_dir); b_s(&b, "/auto_on");
}
static void vc_marker_set(int on) {
    char p[360]; int fd;
    if (!vc_prepare_dir()) return;
    vc_marker_path(p, sizeof p);
    if (on) { fd = open(p, 0x241, 0666); if (fd >= 0) { write(fd, "1\n", 2); close(fd); } }
    else unlink(p);
}
static int vc_marker_get(void) {
    char p[360]; int fd;
    if (!vc_prepare_dir()) return 0;
    vc_marker_path(p, sizeof p);
    fd = open(p, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}
static int vc_start(void) {
    if (!vc_prepare_dir()) return 0;
    if (!vc_install()) return 0;
    __atomic_store_n(&g_vc_on, 1, __ATOMIC_RELEASE);
    return 1;
}
static void vc_stop(void) {
    __atomic_store_n(&g_vc_on, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_vc_flush, 1, __ATOMIC_RELEASE);
}

/* ───────────── команда /voice ───────────── */
static int vc_tokens(void* arr, char t[4][24]) {
    long len; int i, n = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len && n < 4; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[24]; int k, L; const u16* ch;
        if (!s) continue;
        L = A.string_length(s); ch = A.string_chars(s);
        if (L <= 0 || L > 20 || !ch) continue;
        utf16_to_utf8(ch, L, tmp, sizeof tmp);
        for (k = 0; tmp[k]; k++) if (tmp[k] >= 'A' && tmp[k] <= 'Z') tmp[k] = (char)(tmp[k] + 32);
        if (seq(tmp, "voice") || seq(tmp, "/voice")) continue;       /* само имя команды в массив может входить, а может и нет */
        for (k = 0; tmp[k] && k < 23; k++) t[n][k] = tmp[k];
        t[n][k] = 0; n++;
    }
    return n;
}
static void vc_say_status(void) {
    char msg[480]; Buf b;
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Voice: запись "); b_s(&b, g_vc_on ? "ВКЛ" : "выкл");
    b_s(&b, g_vc_mode == VC_MODE_NEAR ? ", режим: только рядом" : ", режим: всё, что слышно");
    b_s(&b, ", перехват: "); b_s(&b, g_vc_state == 1 ? "стоит" : "не стоит");
    b_s(&b, ", голосовых пакетов от игры: "); b_int(&b, (long)g_vc_calls);
    b_s(&b, ", записано: "); b_int(&b, (long)g_vc_pkts);
    b_s(&b, ", пропущено (не рядом): "); b_int(&b, (long)g_vc_skip);
    b_s(&b, ", файлов: "); b_int(&b, (long)g_vc_files);
    b_s(&b, ", ~"); b_int(&b, (long)g_vc_kb); b_s(&b, " КБ");
    if (g_vc_drop) { b_s(&b, ", потеряно: "); b_int(&b, (long)g_vc_drop); }
    if (g_vc_errs) { b_s(&b, ", ошибок записи: "); b_int(&b, (long)g_vc_errs); }
    if (g_vc_bad) { b_s(&b, ", негодных пакетов: "); b_int(&b, (long)g_vc_bad); }
    chat_say(msg);
    if (g_vc_state != 1 && g_vc_err[0]) { b_init(&b, msg, sizeof msg); b_s(&b, "Voice: перехват не поставлен: "); b_s(&b, g_vc_err); chat_say(msg); }
}
static int vc_cfg_load(void);
static void vc_flag_set(const char* name, int on);
static void native_voice_cmd(void* self, void* args, void* mi) {
    char t[4][24]; int n; char msg[420]; Buf b;
    (void)self; (void)mi;
    n = vc_tokens(args, t);
    if (n == 0 || seq(t[0], "status") || seq(t[0], "diag")) { vc_say_status(); return; }
    if (seq(t[0], "on")) {
        if (!vc_start()) {
            b_init(&b, msg, sizeof msg); b_s(&b, "Voice: не получилось включить: "); b_s(&b, g_vc_err[0] ? g_vc_err : "нет папки для записи"); chat_say(msg);
            return;
        }
        vc_marker_set(1);
        b_init(&b, msg, sizeof msg); b_s(&b, "Voice: запись включена ("); b_s(&b, g_vc_mode == VC_MODE_NEAR ? "только рядом" : "всё, что слышно");
        b_s(&b, "). Файлы: "); b_s(&b, g_vc_dir); chat_say(msg);
        return;
    }
    if (seq(t[0], "off")) { vc_stop(); vc_marker_set(0); chat_say("Voice: запись выключена"); return; }
    if (seq(t[0], "near")) { __atomic_store_n(&g_vc_mode, VC_MODE_NEAR, __ATOMIC_RELEASE); chat_say("Voice: пишу только тех, кто рядом"); return; }
    if (seq(t[0], "all")) { __atomic_store_n(&g_vc_mode, VC_MODE_ALL, __ATOMIC_RELEASE); chat_say("Voice: пишу всё, что слышит клиент (рядом, рация, телефон, общий канал)"); return; }
    if (seq(t[0], "tg")) {
        if (n > 1 && seq(t[1], "on")) {
            if (!g_vc_tgCfg) vc_cfg_load();
            if (!g_vc_tgCfg) { chat_say("Voice: нет токена и чата. Положите в папку voice файл tg.txt: 1-я строка токен бота, 2-я chat_id (3-я — метка аккаунта), либо включайте из скрипта"); return; }
            if (!vc_start()) { b_init(&b, msg, sizeof msg); b_s(&b, "Voice: не получилось включить запись: "); b_s(&b, g_vc_err[0] ? g_vc_err : "нет папки"); chat_say(msg); return; }
            vc_marker_set(1); vc_flag_set("tg_on", 1); __atomic_store_n(&g_vc_tg, 1, __ATOMIC_RELEASE);
            chat_say("Voice: запись включена, реплики уходят в Telegram голосовыми с ником");
            return;
        }
        if (n > 1 && seq(t[1], "off")) { __atomic_store_n(&g_vc_tg, 0, __ATOMIC_RELEASE); vc_flag_set("tg_on", 0); chat_say("Voice: отправка в Telegram выключена (запись в файлы идёт как шла)"); return; }
        if (n > 1 && seq(t[1], "test")) {
            if (!g_vc_tgCfg) vc_cfg_load();
            if (!g_vc_tgCfg) { chat_say("Voice: Telegram не настроен (нет tg.txt)"); return; }
            if (!g_vc_haveLast) { chat_say("Voice: ещё не было ни одной реплики — отправлять нечего"); return; }
            { VcTgJob j = g_vc_last; vc_tg_push(&j); }
            chat_say("Voice: последняя реплика поставлена в очередь, итог — в /voice tg");
            return;
        }
        b_init(&b, msg, sizeof msg);
        b_s(&b, "Voice → Telegram: "); b_s(&b, g_vc_tg ? "ВКЛ" : "выкл"); b_s(&b, ", настроен: "); b_s(&b, g_vc_tgCfg ? "да" : "нет");
        b_s(&b, ", отправлено: "); b_int(&b, (long)g_vc_tgSent); b_s(&b, ", ошибок: "); b_int(&b, (long)g_vc_tgFail);
        b_s(&b, ", отброшено: "); b_int(&b, (long)g_vc_tgDrop); b_s(&b, ", в очереди: "); b_int(&b, (long)(g_vc_tgHead - g_vc_tgTail));
        chat_say(msg);
        if (g_vc_tgFail && g_vc_tgMsg[0]) { b_init(&b, msg, sizeof msg); b_s(&b, "Voice: последняя причина: "); b_s(&b, g_vc_tgMsg); chat_say(msg); }
        return;
    }
    if (seq(t[0], "folder")) {
        if (!vc_prepare_dir()) { chat_say("Voice: папка недоступна, подробности в konstt.log"); return; }
        b_init(&b, msg, sizeof msg); b_s(&b, "Voice: файлы лежат в "); b_s(&b, g_vc_dir); chat_say(msg);
        return;
    }
    if (seq(t[0], "unhook")) { vc_stop(); vc_marker_set(0); vc_unhook(); chat_say(g_vc_state == 1 ? "Voice: перехват снять не вышло" : "Voice: перехват снят, запись выключена"); return; }
    chat_usage("Используйте: /voice on | off | near | all | status | folder | unhook | tg on|off|test");
}

/* ───────────── настройки Telegram: файл tg.txt и флаг tg_on ───────────── */
static void vc_flag_path(const char* name, char* out, unsigned cap) {
    Buf b; b_init(&b, out, cap); b_s(&b, g_vc_dir); b_ch(&b, '/'); b_s(&b, name);
}
static void vc_flag_set(const char* name, int on) {
    char p[360]; int fd;
    if (!vc_prepare_dir()) return;
    vc_flag_path(name, p, sizeof p);
    if (on) { fd = open(p, 0x241, 0666); if (fd >= 0) { write(fd, "1\n", 2); close(fd); } }
    else unlink(p);
}
static int vc_flag_get(const char* name) {
    char p[360]; int fd;
    if (!vc_prepare_dir()) return 0;
    vc_flag_path(name, p, sizeof p);
    fd = open(p, 0);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}
static void vc_copy(char* dst, unsigned cap, const char* src) { unsigned i; for (i = 0; src[i] && i + 1 < cap; i++) dst[i] = src[i]; dst[i] = 0; }
/* <папка voice>/tg.txt: 1-я строка — токен бота, 2-я — chat_id, 3-я (необязательно) — метка аккаунта в подписи. Пустые строки пропускаются. 1 — прочитан и годен. */
static int vc_cfg_load(void) {
    char p[360], buf[512], tok[160], chat[48], who[48]; int fd, n, i, line = 0, k = 0;
    tok[0] = chat[0] = who[0] = 0;
    if (!vc_prepare_dir()) return 0;
    vc_flag_path("tg.txt", p, sizeof p);
    fd = open(p, 0);
    if (fd < 0) return 0;
    n = (int)read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    for (i = 0; i <= n && line < 3; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r' || c == 0) { if (k > 0) { line++; k = 0; } continue; }
        {   char* dst = line == 0 ? tok : line == 1 ? chat : who; int cap = line == 0 ? (int)sizeof tok : line == 1 ? (int)sizeof chat : (int)sizeof who;
            if (k + 1 < cap && !(k == 0 && (c == ' ' || c == '\t'))) { dst[k++] = c; dst[k] = 0; } }
    }
    if (!tok[0] || !chat[0]) return 0;
    __atomic_store_n(&g_vc_tgCfg, 0, __ATOMIC_RELEASE);
    vc_copy(g_vc_tgTok, sizeof g_vc_tgTok, tok); vc_copy(g_vc_tgChat, sizeof g_vc_tgChat, chat); vc_copy(g_vc_tgWho, sizeof g_vc_tgWho, who);
    __atomic_store_n(&g_vc_tgCfg, 1, __ATOMIC_RELEASE);
    return 1;
}

/* Заявки от HTTP-потока: писатель (поток с IL2CPP) выполняет их сам. */
static void vc_requests(void) {
    int r = __atomic_exchange_n(&g_vc_reqRec, 0, __ATOMIC_ACQ_REL);
    if (r == 1) { if (vc_start()) vc_marker_set(1); else dlog2("voice: запись по заявке не включилась: ", g_vc_err); }
    else if (r == 2) { vc_stop(); vc_marker_set(0); }
    r = __atomic_exchange_n(&g_vc_reqTgFlag, 0, __ATOMIC_ACQ_REL);
    if (r == 1) vc_flag_set("tg_on", 1); else if (r == 2) vc_flag_set("tg_on", 0);
    if (__atomic_exchange_n(&g_vc_reqTest, 0, __ATOMIC_ACQ_REL) && g_vc_haveLast) { VcTgJob j = g_vc_last; vc_tg_push(&j); }
}

/* ───────────── HTTP для скрипта (127.0.0.1:17791..17800) ─────────────
   /voice/status                                   — состояние (JSON, без токена)
   /voice/cfg?k=ТОКЕН&c=ЧАТ&who=МЕТКА&tg=0|1&on=0|1&mode=near|all&test=1   — любые из параметров; k и c только вместе; на диск не пишется */
extern int socket(int, int, int);
extern int setsockopt(int, int, int, const void*, unsigned);
extern int bind(int, const void*, unsigned);
extern int listen(int, int);
extern int accept(int, void*, void*);
extern long recv(int, void*, unsigned long, int);
extern long send(int, const void*, unsigned long, int);
#define VC_PORT_FIRST 17791
#define VC_PORT_COUNT 10
struct vc_sa4 { u16 family; u16 port; u32 addr; u8 zero[8]; };
struct vc_tv { long sec; long usec; };
static int g_vc_httpPort;

static int vc_qparam(const char* rq, const char* name, char* out, unsigned cap) {
    unsigned i = 0, nl = slen(name), k = 0;
    while (rq[i] && rq[i] != ' ' && rq[i] != '\r' && rq[i] != '\n') i++;
    if (rq[i] == ' ') i++;
    while (rq[i] && rq[i] != ' ' && rq[i] != '\r' && rq[i] != '\n' && rq[i] != '?') i++;
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
static int vc_send_all(int fd, const char* p, unsigned long n) {
    while (n) { long w = send(fd, p, n, 0x4000); if (w <= 0) return 0; p += w; n -= (unsigned long)w; }
    return 1;
}
static void vc_respond(int fd, const char* status, const char* ctype, const char* body, unsigned long blen) {
    char h[420]; Buf b;
    b_init(&b, h, sizeof h);
    b_s(&b, "HTTP/1.1 "); b_s(&b, status); b_s(&b, "\r\nContent-Type: "); b_s(&b, ctype);
    b_s(&b, "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
    b_int(&b, (long)blen); b_s(&b, "\r\n\r\n");
    if (vc_send_all(fd, h, b.n) && blen) vc_send_all(fd, body, blen);
}
static void vc_http_status(int c) {
    char js[640], m[200]; Buf b; unsigned i, k = 0;
    for (i = 0; g_vc_tgMsg[i] && k + 2 < sizeof m; i++) { char ch = g_vc_tgMsg[i]; m[k++] = (ch == '"' || ch == '\\' || (unsigned char)ch < 32) ? ' ' : ch; }
    m[k] = 0;
    b_init(&b, js, sizeof js);
    b_s(&b, "{\"v\":1,\"on\":"); b_int(&b, g_vc_on ? 1 : 0);
    b_s(&b, ",\"hooked\":"); b_int(&b, g_vc_state == 1 ? 1 : 0);
    b_s(&b, ",\"mode\":\""); b_s(&b, g_vc_mode == VC_MODE_NEAR ? "near" : "all");
    b_s(&b, "\",\"tg\":"); b_int(&b, g_vc_tg ? 1 : 0);
    b_s(&b, ",\"cfg\":"); b_int(&b, g_vc_tgCfg ? 1 : 0);
    b_s(&b, ",\"kb\":1,\"kbset\":"); b_int(&b, g_vc_tgKbSet ? 1 : 0);     /* kb:1 — сборка умеет кнопки под голосовыми (нет поля — старая) */
    b_s(&b, ",\"files\":"); b_int(&b, (long)g_vc_files);
    b_s(&b, ",\"queued\":"); b_int(&b, (long)(g_vc_tgHead - g_vc_tgTail));
    b_s(&b, ",\"sent\":"); b_int(&b, (long)g_vc_tgSent);
    b_s(&b, ",\"fail\":"); b_int(&b, (long)g_vc_tgFail);
    b_s(&b, ",\"drop\":"); b_int(&b, (long)g_vc_tgDrop);
    b_s(&b, ",\"code\":"); b_int(&b, g_vc_tgCode);
    b_s(&b, ",\"msg\":\""); b_s(&b, m); b_s(&b, "\"}");
    vc_respond(c, "200 OK", "application/json", js, b.n);
}
static void vc_http_cfg(int c, const char* rq) {
    static const char okj[] = "{\"ok\":true}", bad[] = "{\"ok\":false,\"err\":\"bad_args\"}", nocfg[] = "{\"ok\":false,\"err\":\"no_token\"}";
    char tok[160], chat[48], who[48], v[16], kbq[VC_KB_MAX]; int hasK, hasC;
    hasK = vc_qparam(rq, "k", tok, sizeof tok); hasC = vc_qparam(rq, "c", chat, sizeof chat);
    if (hasK != hasC || (hasK && (!tok[0] || !chat[0]))) { vc_respond(c, "200 OK", "application/json", bad, sizeof bad - 1); return; }
    if (hasK) {
        __atomic_store_n(&g_vc_tgCfg, 0, __ATOMIC_RELEASE);
        vc_copy(g_vc_tgTok, sizeof g_vc_tgTok, tok); vc_copy(g_vc_tgChat, sizeof g_vc_tgChat, chat);
        __atomic_store_n(&g_vc_tgCfg, 1, __ATOMIC_RELEASE);
    }
    if (vc_qparam(rq, "who", who, sizeof who)) vc_copy(g_vc_tgWho, sizeof g_vc_tgWho, who);
    if (vc_qparam(rq, "kb", kbq, sizeof kbq)) {                         /* reply_markup под голосовыми: пусто — убрать кнопки */
        unsigned kl = slen(kbq);
        if (kl && (kbq[0] != '{' || kbq[kl - 1] != '}' || kl + 2 >= sizeof kbq)) { kbq[0] = 0; dlog("voice: kb не принят (обрезан или не JSON) — голосовые пойдут без кнопок"); }
        {   int w = 1 - (__atomic_load_n(&g_vc_tgKbIdx, __ATOMIC_ACQUIRE) & 1);
            vc_copy(g_vc_tgKb[w], sizeof g_vc_tgKb[w], kbq);
            __atomic_store_n(&g_vc_tgKbIdx, w, __ATOMIC_RELEASE);
            __atomic_store_n(&g_vc_tgKbSet, kbq[0] ? 1 : 0, __ATOMIC_RELEASE); }
    }
    if (vc_qparam(rq, "mode", v, sizeof v)) __atomic_store_n(&g_vc_mode, seq(v, "all") ? VC_MODE_ALL : VC_MODE_NEAR, __ATOMIC_RELEASE);
    if (vc_qparam(rq, "tg", v, sizeof v)) {
        if (v[0] == '1' && g_vc_tgCfg) { __atomic_store_n(&g_vc_tg, 1, __ATOMIC_RELEASE); __atomic_store_n(&g_vc_reqRec, 1, __ATOMIC_RELEASE); __atomic_store_n(&g_vc_reqTgFlag, 1, __ATOMIC_RELEASE); }
        else if (v[0] == '0') { __atomic_store_n(&g_vc_tg, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_vc_reqTgFlag, 2, __ATOMIC_RELEASE); }
        else { vc_respond(c, "200 OK", "application/json", nocfg, sizeof nocfg - 1); return; }
    }
    if (vc_qparam(rq, "on", v, sizeof v)) __atomic_store_n(&g_vc_reqRec, v[0] == '1' ? 1 : 2, __ATOMIC_RELEASE);
    if (vc_qparam(rq, "test", v, sizeof v) && v[0] == '1') __atomic_store_n(&g_vc_reqTest, 1, __ATOMIC_RELEASE);
    vc_respond(c, "200 OK", "application/json", okj, sizeof okj - 1);
}
static void* vc_http_main(void* arg) {
    int fd = -1, i, one = 1;
    (void)arg;
    if (!hm_wait_started()) return NULL;                                /* konstt_off — мод выключен целиком */
    for (;;) {
        for (i = 0; i < VC_PORT_COUNT && fd < 0; i++) {
            struct vc_sa4 sa; int s = socket(2, 1, 0), z;
            if (s < 0) break;
            setsockopt(s, 1, 2, &one, sizeof one);
            sa.family = 2; sa.port = (u16)(((VC_PORT_FIRST + i) >> 8) | (((VC_PORT_FIRST + i) & 255) << 8));
            sa.addr = 0x0100007F;
            for (z = 0; z < 8; z++) sa.zero[z] = 0;
            if (bind(s, &sa, sizeof sa) == 0 && listen(s, 4) == 0) { fd = s; g_vc_httpPort = VC_PORT_FIRST + i; }
            else close(s);
        }
        if (fd < 0) { usleep(3000000); continue; }
        dlog_i("voice: HTTP слушает порт ", g_vc_httpPort);
        for (;;) {
            int c = accept(fd, NULL, NULL);
            char rq[4096]; long n; struct vc_tv t;
            if (c < 0) { usleep(100000); continue; }
            t.sec = 5; t.usec = 0;
            setsockopt(c, 1, 20, &t, sizeof t);
            setsockopt(c, 1, 21, &t, sizeof t);
            n = recv(c, rq, sizeof rq - 1, 0);
            if (n > 0) {
                rq[n] = 0;
                if (sprefix(rq, "OPTIONS ")) vc_respond(c, "204 No Content", "text/plain", "", 0);
                else if (sprefix(rq, "GET /voice/status")) vc_http_status(c);
                else if (sprefix(rq, "GET /voice/cfg")) vc_http_cfg(c, rq);
                else vc_respond(c, "404 Not Found", "text/plain", "no", 2);
            }
            close(c);
        }
    }
    return NULL;
}

/* ───────────── запуск ───────────── */
static void* vc_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("voice", native_voice_cmd)) return NULL;     /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /voice */
    ensure_attached();                                                /* ник игрока читаем через IL2CPP из этого потока */
    hm_mod_ready(g_vc_modId);
    dlog("voice: мод загружен, команда /voice");
    if (vc_marker_get()) {                                            /* раньше включали /voice on — продолжаем */
        if (vc_start()) dlog("voice: запись включена автоматически (флаг auto_on)");
        else dlog2("voice: автозапуск не вышел: ", g_vc_err);
    }
    if (vc_cfg_load()) {                                              /* tg.txt есть — Telegram настроен; tg_on помнит, что было включено */
        if (vc_flag_get("tg_on")) { __atomic_store_n(&g_vc_tg, 1, __ATOMIC_RELEASE); dlog("voice: отправка в Telegram включена (флаг tg_on)"); }
    }
    for (;;) {                                                        /* писатель: забирает пакеты и ведёт файлы */
        int open_;
        vc_requests();
        open_ = vc_tick();
        usleep((open_ || g_vc_on) ? 40000 : 250000);
    }
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void vc_ctor(void) {
    unsigned long t = 0;
    g_vc_modId = hm_mod_register("voice");                           /* учесть мод в счётчике — только в конструкторе */
    pthread_create(&t, NULL, vc_main, NULL);
    pthread_create(&t, NULL, vc_tg_main, NULL);
    pthread_create(&t, NULL, vc_http_main, NULL);
}
#endif
