/*
 * hassle_mem.c — необязательный мод «память»: следит за памятью процесса, пишет её рост в ОТДЕЛЬНЫЙ журнал и сам освобождает память,
 * когда её мало или когда персонаж долго стоит. Рассчитан на слабые эмуляторы, где игра при долгом простое вылетает.
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_mem.c — больше ничего править не нужно.
 *
 * ЖУРНАЛ (отдельный файл, рядом с konstt.log):  HLOG/konstt_mem.log
 *   konstt_mem_prev.log — прошлый запуск (если игру убила система, последняя строка покажет, что было с памятью перед этим);
 *   konstt_mem_old.log  — предыдущая часть текущего запуска (журнал ограничен MEM_MAXLINES строк, дальше старая часть переезжает сюда).
 *   Строки: «[чч:мм:сс UTC +Nс] …». Что в них:
 *     «замер»   RSS процесса и его части (анон/файл/shm), VSS, пик, PSS, доступная ОЗУ системы, malloc, куча GC игры, потоки, fd, PSI.
 *     «тренд»   раз в 5 минут: на сколько выросло за это время (RSS, анон, malloc, GC) — сразу видно, что течёт.
 *     «карта»   раз в 10 минут (и по /mem maps): RSS по категориям из /proc/self/smaps (анон. память, [anon:scudo…], /dev/kgsl, ion, libil2cpp…)
 *               и что выросло относительно первого снимка.
 *     «ОЧИСТКА» каждая очистка: причина, что сделано (GC / UnloadUnusedAssets / возврат памяти ОС), сколько освободилось и за сколько.
 *
 * ЧТО ДЕЛАЕТ (всё автоматически; выключатель — /mem auto off или файл HLOG/konstt_mem.txt, см. ниже)
 *   1) Следит. Замер каждые MEM_SAMPLE_S секунд (чаще, когда памяти мало).
 *   2) GC. il2cpp_gc_collect из нативного API (управляемые GC.Collect в этой сборке вырезаны). Ограничивает рост управляемой кучи.
 *   3) Возврат нативной памяти ОС. mallopt(M_PURGE / M_PURGE_ALL) (Android 9+ / 14+); если функции нет — просто пропускается.
 *   4) Выгрузка неиспользуемых текстур/мешей/звуков. Иккол UnityEngine.Resources::UnloadUnusedAssets (управляемая обёртка вырезана, но движок
 *      иккол регистрирует), строго в главном потоке. Игра на секунду-две может подвиснуть, поэтому это делается, когда персонаж долго стоит
 *      на месте (позиция читается ядром), либо когда памяти критически мало.
 *   5) Урезание качества. Если даже после полной очистки памяти критически мало (дважды подряд) — QualitySettings.masterTextureLimit = 1
 *      (текстуры вдвое меньше). Назад не возвращается само: /mem tex 0.
 *
 *   Когда что запускается (по умолчанию; пороги считаются от объёма ОЗУ системы, см. mem_level):
 *     мало памяти (LOW)            → GC + возврат ОС, не чаще раза в MEM_LOW_GAP_S;
 *     критически мало (CRIT)       → полная очистка (GC + выгрузка ассетов + возврат ОС), не чаще раза в MEM_CRIT_GAP_S (с отступом, если не помогает);
 *     стоим ≥ MEM_IDLE_GC_S        → плановый GC + возврат ОС (повтор через MEM_IDLE_GC_EVERY_S);
 *     стоим ≥ MEM_IDLE_FULL_S      → плановая полная очистка (повтор через MEM_IDLE_FULL_EVERY_S).
 *   В 32-битном процессе дополнительно следим за адресным пространством (VSS): его исчерпание тоже убивает игру.
 *
 * КОМАНДА /mem (нативная, как /rec):
 *   /mem              состояние (RSS, доступная ОЗУ, GC, авто, счётчики)      /mem maps   карта памяти по категориям — в журнал
 *   /mem gc           GC + возврат памяти ОС сейчас                           /mem purge  только возврат памяти ОС
 *   /mem full         полная очистка сейчас                                   /mem tex N  masterTextureLimit (0 — полное качество, 1 — вдвое меньше …)
 *   /mem auto on|off  включить / выключить авто-очистку (замеры остаются)
 *   Итоги очисток команда не ждёт — они в HLOG/konstt_mem.log и в /mem.
 *
 * ФАЙЛ-ПЕРЕКЛЮЧАТЕЛЬ  HLOG/konstt_mem.txt  (слова через пробел; читается на лету, перезапуск не нужен) — чтобы проверить, не мешает ли сама очистка:
 *   monitor   только замеры, ничего не чистить        nounload  не вызывать UnloadUnusedAssets        notex  не трогать качество текстур
 *   nogc      не вызывать il2cpp_gc_collect           nopurge   не вызывать mallopt
 *
 * КАК ЭТО ЗАПУСКАЕТСЯ В ГЛАВНОМ ПОТОКЕ. UnloadUnusedAssets и QualitySettings можно звать только в главном потоке Unity. Контекст главного потока
 * (UnitySynchronizationContext) мод находит сам, без команды: у управляемого объекта Thread главного потока в m_ExecutionContext._syncContext лежит
 * этот контекст (потоки перечисляются il2cpp_thread_get_all_attached_threads). Дальше — Post, как у /nocol. Если найти не вышло — GC всё равно
 * делается из потока мода, а ассеты выгрузит только команда /mem, набранная один раз (она выполняется в главном потоке и захватывает контекст).
 */
#pragma once
#include "hassle_core.c"

/* ───────────── настройки (можно переопределить при сборке: -DMEM_SAMPLE_S=30 …) ───────────── */
#ifndef MEM_SAMPLE_S
#define MEM_SAMPLE_S 15                 /* период замера, с */
#endif
#ifndef MEM_SAMPLE_LOW_S
#define MEM_SAMPLE_LOW_S 5              /* период замера, когда памяти мало */
#endif
#ifndef MEM_WARMUP_S
#define MEM_WARMUP_S 60                 /* после готовности игры первую минуту не чистим (идёт загрузка мира) */
#endif
#ifndef MEM_GAP_S
#define MEM_GAP_S 20                    /* минимум между любыми двумя очистками */
#endif
#ifndef MEM_LOW_GAP_S
#define MEM_LOW_GAP_S 60                /* LOW: GC не чаще */
#endif
#ifndef MEM_CRIT_GAP_S
#define MEM_CRIT_GAP_S 90               /* CRIT: полная очистка не чаще (умножается на 1+число неудач подряд, до ×6) */
#endif
#ifndef MEM_IDLE_GC_S
#define MEM_IDLE_GC_S 120               /* стоим столько секунд → плановый GC */
#endif
#ifndef MEM_IDLE_GC_EVERY_S
#define MEM_IDLE_GC_EVERY_S 180
#endif
#ifndef MEM_IDLE_FULL_S
#define MEM_IDLE_FULL_S 300             /* стоим столько секунд → плановая полная очистка */
#endif
#ifndef MEM_IDLE_FULL_EVERY_S
#define MEM_IDLE_FULL_EVERY_S 900
#endif
#ifndef MEM_MOVE_M2
#define MEM_MOVE_M2 2.25f               /* сдвиг больше 1,5 м (в квадрате) — персонаж «двигается» */
#endif
#ifndef MEM_SETTLE_FULL_S
#define MEM_SETTLE_FULL_S 6             /* после UnloadUnusedAssets (он асинхронный) ждём столько, потом возвращаем память ОС */
#endif
#ifndef MEM_SETTLE_GC_S
#define MEM_SETTLE_GC_S 1
#endif
#ifndef MEM_MAXLINES
#define MEM_MAXLINES 3000               /* строк в konstt_mem.log, дальше файл переезжает в konstt_mem_old.log */
#endif
#ifndef MEM_MAPS_EVERY_S
#define MEM_MAPS_EVERY_S 600
#endif
#ifndef MEM_TREND_EVERY_S
#define MEM_TREND_EVERY_S 300
#endif
#ifndef MEM_LOW_PCT
#define MEM_LOW_PCT 18                  /* LOW: доступно меньше 18% ОЗУ (но не меньше 256 МБ и не больше 600 МБ) */
#endif
#ifndef MEM_CRIT_PCT
#define MEM_CRIT_PCT 8                  /* CRIT: меньше 8% (128…256 МБ) */
#endif
#ifndef MEM_VSS32_LOW_KB
#define MEM_VSS32_LOW_KB (2300LL * 1024)    /* 32-бит: адресное пространство почти кончилось */
#endif
#ifndef MEM_VSS32_CRIT_KB
#define MEM_VSS32_CRIT_KB (2700LL * 1024)
#endif
#ifndef MEM_TEX_AUTO_MAX
#define MEM_TEX_AUTO_MAX 1              /* автоматически masterTextureLimit не выше этого (руками — до 3) */
#endif

/* флаги задания очистки */
#define MEM_J_GC     1                  /* il2cpp_gc_collect */
#define MEM_J_UNLOAD 2                  /* Resources.UnloadUnusedAssets (главный поток) */
#define MEM_J_TEX    4                  /* QualitySettings.masterTextureLimit (главный поток) */
#define MEM_J_PURGE  8                  /* mallopt(M_PURGE) */
#define MEM_J_MAPS   16                 /* только снимок карты памяти */
/* слова файла konstt_mem.txt */
#define MEM_C_MONITOR 1
#define MEM_C_NOUNLOAD 2
#define MEM_C_NOTEX 4
#define MEM_C_NOGC 8
#define MEM_C_NOPURGE 16

/* ───────────── дополнительные функции libc (в ядре их нет) ───────────── */
extern void* opendir(const char*);
extern void* readdir(void*);
extern int closedir(void*);
extern int clock_gettime(int, void*);       /* struct timespec = два long (так же объявлен в hassle_route.c) */

typedef struct { uptr f[10]; } MemMallinfo; /* struct mallinfo с полями size_t: arena ordblks smblks hblks hblkhd usmblks fsmblks uordblks fordblks keepcost */
static struct {
    int (*mallopt)(int, int);
    int (*malloc_trim)(uptr);
    MemMallinfo (*mallinfo)(void);
    int (*getpagesize)(void);
} MLC;
static int g_mem_libcTried, g_mem_pageKb = 4;

/* дополнительные функции IL2CPP (свои; ядро не трогаем) */
static struct {
    void (*gc_collect)(int);
    long long (*gc_used)(void);
    long long (*gc_heap)(void);
    void* (*resolve_icall)(const char*);
    void** (*threads_all)(uptr*);
} MX;
static int g_mem_apiTried, g_mem_apiOk;
typedef void* (*mem_icall_t)(void*);
static mem_icall_t g_mem_unloadFn;
static int g_mem_unloadTried;

/* ───────────── состояние ───────────── */
static volatile int g_mem_auto = 1;          /* авто-очистка включена */
static volatile int g_mem_cfg;               /* слова из konstt_mem.txt */
static volatile int g_mem_busy;              /* очистка идёт */
static volatile int g_mem_cmdReq;            /* заказ из чат-команды (флаги MEM_J_*), исполняет поток мода */
static volatile int g_mem_texWant;           /* значение masterTextureLimit для MEM_J_TEX */
static volatile int g_mem_jobKind;           /* что просим сделать главный поток */
static volatile int g_mem_jobDone;
static volatile int g_mem_inGame;
static long g_mem_t0, g_mem_readyAt, g_mem_idleSince, g_mem_lastJobS, g_mem_lastGcS, g_mem_lastFullS, g_mem_nextSample, g_mem_nextTrend, g_mem_nextMaps, g_mem_cfgPoll;
static int g_mem_hasAnchor; static float g_mem_anchor[3];
static int g_mem_jobs, g_mem_failStreak, g_mem_texLimit;
static long long g_mem_freedKB;
static int g_mem_level, g_mem_ctxTries, g_mem_postFail;
static char g_mem_lastSummary[300];

/* ───────────── свой журнал ───────────── */
static char g_mem_dir[300], g_mem_path[330], g_mem_prev[330], g_mem_old[330], g_mem_cfgPath[330];
#define MEM_CONT_LINE "== продолжение (предыдущая часть этого запуска — konstt_mem_old.log)\n"
static int g_mem_logOn, g_mem_lines;
static volatile int g_mem_logLock;

static void mem_copy(char* dst, const char* src, unsigned cap) {
    unsigned i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void mem_paths(void) {                   /* имена файлов из каталога HLOG */
    Buf b;
    b_init(&b, g_mem_path, sizeof g_mem_path); b_s(&b, g_mem_dir); b_s(&b, "konstt_mem.log");
    b_init(&b, g_mem_prev, sizeof g_mem_prev); b_s(&b, g_mem_dir); b_s(&b, "konstt_mem_prev.log");
    b_init(&b, g_mem_old, sizeof g_mem_old); b_s(&b, g_mem_dir); b_s(&b, "konstt_mem_old.log");
    b_init(&b, g_mem_cfgPath, sizeof g_mem_cfgPath); b_s(&b, g_mem_dir); b_s(&b, "konstt_mem.txt");
}
static void mem_log(const char* msg) {
    char t[1100]; Buf b; long now = time(NULL), d; int hh, mm, ss;
    if (!g_mem_logOn) return;
    d = now % 86400; hh = (int)(d / 3600); mm = (int)((d / 60) % 60); ss = (int)(d % 60);
    b_init(&b, t, sizeof t);
    b_ch(&b, '['); if (hh < 10) b_ch(&b, '0'); b_int(&b, hh); b_ch(&b, ':'); if (mm < 10) b_ch(&b, '0'); b_int(&b, mm);
    b_ch(&b, ':'); if (ss < 10) b_ch(&b, '0'); b_int(&b, ss);
    b_s(&b, " UTC +"); b_int(&b, now - g_mem_t0); b_s(&b, "с] "); b_s(&b, msg); b_ch(&b, '\n');
    while (__sync_lock_test_and_set(&g_mem_logLock, 1)) usleep(200);
    if (g_mem_lines >= MEM_MAXLINES) {          /* файл разросся: переносим в _old и начинаем новый */
        rename(g_mem_path, g_mem_old);
        flog_write(g_mem_path, "", 0, 1);
        flog_write(g_mem_path, MEM_CONT_LINE, slen(MEM_CONT_LINE), 0);
        g_mem_lines = 1;
    }
    flog_write(g_mem_path, t, b.n, 0);
    g_mem_lines++;
    __sync_lock_release(&g_mem_logLock);
}
static void mem_log2(const char* a, const char* b2) { char t[700]; Buf b; b_init(&b, t, sizeof t); b_s(&b, a); b_s(&b, b2); mem_log(t); }
static void mem_log_i(const char* a, long v) { char t[700]; Buf b; b_init(&b, t, sizeof t); b_s(&b, a); b_int(&b, v); mem_log(t); }
/* важное — ещё и в общий konstt.log */
static void mem_both(const char* msg) { char t[700]; Buf b; b_init(&b, t, sizeof t); b_s(&b, "mem: "); b_s(&b, msg); dlog(t); mem_log(msg); }

static long long mem_now_ms(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return (long long)ts[0] * 1000 + ts[1] / 1000000; }

/* ───────────── чтение /proc ───────────── */
static int mem_read_file(const char* path, char* buf, int cap) {
    int fd = open(path, 0), n = 0; long r;
    if (fd < 0) { buf[0] = 0; return -1; }
    while (n < cap - 1) {
        r = read(fd, buf + n, (unsigned long)(cap - 1 - n));
        if (r <= 0) break;
        n += (int)r;
    }
    close(fd); buf[n] = 0;
    return n;
}
/* число после «ключ:» в начале строки; −1, если ключа нет */
static long long mem_kv(const char* t, const char* key) {
    unsigned kl = slen(key); const char* p = t;
    while (*p) {
        if (sprefix(p, key) && p[kl] == ':') {
            const char* q = p + kl + 1; long long v = 0; int any = 0;
            while (*q == ' ' || *q == '\t') q++;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; any = 1; }
            return any ? v : -1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}
static void mem_libc(void) {
    void* h;
    if (g_mem_libcTried) return;
    g_mem_libcTried = 1;
    h = dlopen("libc.so", RTLD_NOW_);
    if (!h) h = dlopen(NULL, RTLD_NOW_);
    if (!h) return;
    *(void**)&MLC.mallopt = dlsym(h, "mallopt");
    *(void**)&MLC.malloc_trim = dlsym(h, "malloc_trim");
    *(void**)&MLC.mallinfo = dlsym(h, "mallinfo2");
#ifdef __ANDROID__
    if (!MLC.mallinfo) *(void**)&MLC.mallinfo = dlsym(h, "mallinfo");     /* в bionic поля size_t */
#endif
    *(void**)&MLC.getpagesize = dlsym(h, "getpagesize");
    if (MLC.getpagesize) { int ps = MLC.getpagesize(); if (ps >= 4096 && ps <= 65536) g_mem_pageKb = ps / 1024; }
}

typedef struct {
    long long vss, rss, hwm, anon, file, shmem, swap, pss;                  /* кБ процесса; −1 — нет данных */
    long long total, avail, memFree, cached, swapTot, swapFree;             /* кБ системы */
    long long natUsed, natMapped;                                           /* malloc (libc), кБ */
    long long gcUsed, gcHeap;                                               /* куча GC игры, кБ */
    int threads, fds;
    char psi[24];                                                           /* «some/full» avg10 из /proc/pressure/memory */
} MemSample;

static int mem_api(void);
static void mem_gc_sizes(long long* used, long long* heap) {
    *used = -1; *heap = -1;
    if (!g_ready || !mem_api() || !MX.gc_used || !MX.gc_heap) return;
    ensure_attached();
    *used = MX.gc_used() / 1024; *heap = MX.gc_heap() / 1024;
}
static int mem_count_fds(void) {
    void* d = opendir("/proc/self/fd"); int n = 0;
    if (!d) return -1;
    while (readdir(d)) n++;
    closedir(d);
    return n > 3 ? n - 3 : 0;                   /* без «.», «..» и самого дескриптора каталога */
}
static void mem_psi(char* out, int cap) {
    char t[300]; int i, k = 0; const char* p; out[0] = 0;
    if (mem_read_file("/proc/pressure/memory", t, sizeof t) <= 0) return;
    for (i = 0; i < 2; i++) {
        const char* key = i == 0 ? "some avg10=" : "full avg10=";
        for (p = t; *p; p++) if (sprefix(p, key)) break;
        if (!*p) continue;
        p += slen(key);
        if (i) { if (k + 1 < cap) out[k++] = '/'; }
        while (*p && *p != ' ' && *p != '\n' && k + 1 < cap) out[k++] = *p++;
    }
    out[k] = 0;
}
static void mem_take(MemSample* s, int deep) {
    static char buf[6144]; int n; long long v;
    s->vss = s->rss = s->hwm = s->anon = s->file = s->shmem = s->swap = s->pss = -1;
    s->total = s->avail = s->memFree = s->cached = s->swapTot = s->swapFree = -1;
    s->natUsed = s->natMapped = -1; s->threads = -1; s->fds = -1; s->psi[0] = 0;
    n = mem_read_file("/proc/self/status", buf, sizeof buf);
    if (n > 0) {
        s->vss = mem_kv(buf, "VmSize"); s->rss = mem_kv(buf, "VmRSS"); s->hwm = mem_kv(buf, "VmHWM");
        s->anon = mem_kv(buf, "RssAnon"); s->file = mem_kv(buf, "RssFile"); s->shmem = mem_kv(buf, "RssShmem");
        s->swap = mem_kv(buf, "VmSwap"); v = mem_kv(buf, "Threads"); s->threads = v >= 0 ? (int)v : -1;
    }
    if (s->rss < 0) {                           /* запасной путь: statm в страницах («размер резидент …») */
        n = mem_read_file("/proc/self/statm", buf, sizeof buf);
        if (n > 0) {
            long long a = 0, b2 = 0; const char* p = buf;
            while (*p >= '0' && *p <= '9') a = a * 10 + (*p++ - '0');
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') b2 = b2 * 10 + (*p++ - '0');
            s->vss = a * g_mem_pageKb; s->rss = b2 * g_mem_pageKb;
        }
    }
    n = mem_read_file("/proc/meminfo", buf, sizeof buf);
    if (n > 0) {
        s->total = mem_kv(buf, "MemTotal"); s->memFree = mem_kv(buf, "MemFree"); s->avail = mem_kv(buf, "MemAvailable");
        s->cached = mem_kv(buf, "Cached"); s->swapTot = mem_kv(buf, "SwapTotal"); s->swapFree = mem_kv(buf, "SwapFree");
        if (s->avail < 0 && s->memFree >= 0) s->avail = s->memFree + (s->cached > 0 ? s->cached : 0);     /* ядро старше 3.14 */
    }
    mem_libc();
    if (MLC.mallinfo) { MemMallinfo mi = MLC.mallinfo(); s->natUsed = (long long)mi.f[7] / 1024; s->natMapped = (long long)mi.f[4] / 1024; }
    mem_gc_sizes(&s->gcUsed, &s->gcHeap);
    if (deep) {
        n = mem_read_file("/proc/self/smaps_rollup", buf, sizeof buf);
        if (n > 0) s->pss = mem_kv(buf, "Pss");
        s->fds = mem_count_fds();
        mem_psi(s->psi, sizeof s->psi);
    }
}
static long long mem_rss_now(void) {            /* быстрый замер только RSS (для отсчёта после очистки) */
    char t[2048];
    if (mem_read_file("/proc/self/status", t, sizeof t) > 0) return mem_kv(t, "VmRSS");
    return -1;
}
static void mem_b_mb(Buf* b, long long kb) { if (kb < 0) b_ch(b, '?'); else b_int(b, (long)(kb / 1024)); }
static void mem_b_dmb(Buf* b, long long dkb) {   /* со знаком */
    if (dkb < 0) { b_ch(b, '-'); dkb = -dkb; } else b_ch(b, '+');
    b_int(b, (long)(dkb / 1024));
}
static void mem_sample_line(const MemSample* s, long idle) {
    char t[760]; Buf b; b_init(&b, t, sizeof t);
    b_s(&b, "замер: RSS "); mem_b_mb(&b, s->rss);
    b_s(&b, " [анон "); mem_b_mb(&b, s->anon); b_s(&b, " файл "); mem_b_mb(&b, s->file); b_s(&b, " shm "); mem_b_mb(&b, s->shmem);
    b_s(&b, "] VSS "); mem_b_mb(&b, s->vss); b_s(&b, " пик "); mem_b_mb(&b, s->hwm);
    b_s(&b, " swap "); mem_b_mb(&b, s->swap); b_s(&b, " PSS "); mem_b_mb(&b, s->pss);
    b_s(&b, " | ОЗУ доступно "); mem_b_mb(&b, s->avail); b_ch(&b, '/'); mem_b_mb(&b, s->total);
    b_s(&b, " своп "); mem_b_mb(&b, s->swapTot > 0 ? s->swapTot - s->swapFree : (s->swapTot == 0 ? 0 : -1)); b_ch(&b, '/'); mem_b_mb(&b, s->swapTot);
    b_s(&b, " | malloc "); mem_b_mb(&b, s->natUsed); b_ch(&b, '/'); mem_b_mb(&b, s->natMapped);
    b_s(&b, " | GC "); mem_b_mb(&b, s->gcUsed); b_ch(&b, '/'); mem_b_mb(&b, s->gcHeap);
    b_s(&b, " | потоков "); b_int(&b, s->threads); b_s(&b, " fd "); b_int(&b, s->fds);
    if (s->psi[0]) { b_s(&b, " | PSI "); b_s(&b, s->psi); }
    if (g_mem_inGame) { b_s(&b, " | стоим "); b_int(&b, idle); b_s(&b, "с"); } else b_s(&b, " | не в игре");
    b_s(&b, " | ур. "); b_s(&b, g_mem_level >= 2 ? "КРИТ" : g_mem_level == 1 ? "мало" : "ок");
    mem_log(t);
}

/* ───────────── уровень нехватки памяти ───────────── */
static int mem_level(const MemSample* s) {
    long long low, crit; int lvl = 0;
    if (s->total > 0 && s->avail >= 0) {
        low = s->total * MEM_LOW_PCT / 100;
        if (low < 256LL * 1024) low = 256LL * 1024;
        if (low > 600LL * 1024) low = 600LL * 1024;
        crit = s->total * MEM_CRIT_PCT / 100;
        if (crit < 128LL * 1024) crit = 128LL * 1024;
        if (crit > 256LL * 1024) crit = 256LL * 1024;
        if (s->avail < crit) lvl = 2; else if (s->avail < low) lvl = 1;
    }
    if (sizeof(void*) == 4 && s->vss > 0) {      /* 32-битный процесс: адресное пространство конечно */
        if (s->vss > MEM_VSS32_CRIT_KB) lvl = 2;
        else if (s->vss > MEM_VSS32_LOW_KB && lvl < 1) lvl = 1;
    }
    return lvl;
}

/* ───────────── карта памяти по категориям (/proc/self/smaps) ───────────── */
typedef struct { char name[32]; long long rss, pss, base; } MemCat;
#define MEM_NCAT 56
static MemCat g_mem_cat[MEM_NCAT];
static int g_mem_ncat, g_mem_haveBase;
static MemCat* g_mem_curCat;
static const char* mem_base_name(const char* p) { const char* r = p; for (; *p; p++) if (*p == '/') r = p + 1; return r; }
static int mem_has(const char* s, const char* sub) {            /* sub есть в s */
    for (; *s; s++) if (sprefix(s, sub)) return 1;
    return 0;
}
static void mem_classify(const char* p, char* out) {            /* имя категории по полю «путь» строки smaps */
    unsigned i = 0;
    if (!*p) { mem_copy(out, "анон без имени", 32); return; }
    if (sprefix(p, "[anon:")) {
        for (; p[i] && p[i] != ']' && i < 28; i++) {
            if (p[i] == ':' && p[i + 1] >= '0' && p[i + 1] <= '9') break;                  /* «…:12345» — хвост-номер отбрасываем */
            out[i] = p[i];
        }
        out[i++] = ']'; out[i] = 0; return;
    }
    if (sprefix(p, "[heap]")) { mem_copy(out, "[heap]", 32); return; }
    if (sprefix(p, "[stack")) { mem_copy(out, "[stack]", 32); return; }
    if (p[0] == '[') { mem_copy(out, "[sys]", 32); return; }
    if (sprefix(p, "/dev/")) { for (; p[i] && i < 24; i++) out[i] = p[i]; out[i] = 0; return; }
    if (sprefix(p, "/memfd:")) { for (; p[i] && p[i] != '(' && i < 26; i++) out[i] = p[i]; out[i] = 0; return; }
    if (sprefix(p, "/system/") || sprefix(p, "/apex/") || sprefix(p, "/vendor/") || sprefix(p, "/product/") || sprefix(p, "/system_ext/") || sprefix(p, "/odm/")) {
        mem_copy(out, "система (/system,/apex..)", 32); return;
    }
    if (mem_has(p, ".so")) {
        const char* bn = mem_base_name(p);
        if (mem_has(bn, "il2cpp") || mem_has(bn, "libunity") || mem_has(bn, "cohtml") || mem_has(bn, "RakNet") || mem_has(bn, "libmain")) {
            out[0] = 0; mem_copy(out, bn, 28); return;
        }
        mem_copy(out, "прочие .so", 32); return;
    }
    if (mem_has(p, ".apk") || mem_has(p, ".obb") || mem_has(p, ".dex") || mem_has(p, ".vdex") || mem_has(p, ".odex") || mem_has(p, ".oat") || mem_has(p, ".art") || mem_has(p, ".jar")) {
        mem_copy(out, "apk/obb/dex/oat", 32); return;
    }
    mem_copy(out, "файлы прочие", 32);
}
static MemCat* mem_cat_get(const char* nm) {
    int i;
    for (i = 0; i < g_mem_ncat; i++) if (seq(g_mem_cat[i].name, nm)) return &g_mem_cat[i];
    if (g_mem_ncat >= MEM_NCAT) return &g_mem_cat[MEM_NCAT - 1];          /* таблица полна: всё остальное — в последнюю */
    mem_copy(g_mem_cat[g_mem_ncat].name, nm, 32);
    g_mem_cat[g_mem_ncat].rss = g_mem_cat[g_mem_ncat].pss = g_mem_cat[g_mem_ncat].base = 0;
    return &g_mem_cat[g_mem_ncat++];
}
static long long mem_num_after(const char* p) { long long v = 0; while (*p == ' ' || *p == '\t') p++; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0'); return v; }
static void mem_smaps_line(const char* ln) {
    if ((ln[0] >= '0' && ln[0] <= '9') || (ln[0] >= 'a' && ln[0] <= 'f')) {       /* «адрес-адрес права смещение устройство inode [путь]» */
        int t; const char* p = ln; char nm[32];
        for (t = 0; t < 5; t++) { while (*p && *p != ' ') p++; while (*p == ' ') p++; }
        mem_classify(p, nm);
        g_mem_curCat = mem_cat_get(nm);
    } else if (g_mem_curCat) {
        if (sprefix(ln, "Rss:")) g_mem_curCat->rss += mem_num_after(ln + 4);
        else if (sprefix(ln, "Pss:")) g_mem_curCat->pss += mem_num_after(ln + 4);
    }
}
static int mem_smaps_scan(void) {
    static char buf[65536 + 2048];
    int fd, keep = 0, i;
    fd = open("/proc/self/smaps", 0);
    if (fd < 0) return 0;
    for (i = 0; i < g_mem_ncat; i++) g_mem_cat[i].rss = g_mem_cat[i].pss = 0;
    g_mem_curCat = NULL;
    for (;;) {
        long n = read(fd, buf + keep, 65536); int total, start = 0, j;
        total = keep + (n > 0 ? (int)n : 0);
        for (j = 0; j < total; j++) if (buf[j] == '\n') { buf[j] = 0; mem_smaps_line(buf + start); start = j + 1; }
        keep = total - start;
        if (keep > 2000) keep = 0;                       /* строка длиннее любого пути — мусор */
        for (j = 0; j < keep; j++) buf[j] = buf[start + j];
        if (n <= 0) { if (keep > 0) { buf[keep] = 0; mem_smaps_line(buf); } break; }
    }
    close(fd);
    return 1;
}
static void mem_log_maps(const char* why) {
    char t[900]; Buf b; int i, k, order[MEM_NCAT], n; long long tot = 0;
    if (!mem_smaps_scan()) { mem_log("карта: /proc/self/smaps недоступен"); return; }
    if (!g_mem_haveBase) { for (i = 0; i < g_mem_ncat; i++) g_mem_cat[i].base = g_mem_cat[i].rss; g_mem_haveBase = 1; }
    n = g_mem_ncat;
    for (i = 0; i < n; i++) { order[i] = i; tot += g_mem_cat[i].rss; }
    for (i = 0; i < n; i++) for (k = i + 1; k < n; k++) if (g_mem_cat[order[k]].rss > g_mem_cat[order[i]].rss) { int x = order[i]; order[i] = order[k]; order[k] = x; }
    b_init(&b, t, sizeof t); b_s(&b, "карта ("); b_s(&b, why); b_s(&b, "): всего RSS "); mem_b_mb(&b, tot); b_s(&b, " МБ; крупнейшие:");
    for (i = 0; i < n && i < 8; i++) {
        MemCat* c = &g_mem_cat[order[i]];
        b_s(&b, i ? "; " : " "); b_s(&b, c->name); b_ch(&b, ' '); mem_b_mb(&b, c->rss);
        b_s(&b, " (Δ"); mem_b_dmb(&b, c->rss - c->base); b_ch(&b, ')');
    }
    mem_log(t);
    for (i = 0; i < n; i++) { order[i] = i; }                      /* кто вырос сильнее всего относительно первого снимка */
    for (i = 0; i < n; i++) for (k = i + 1; k < n; k++)
        if (g_mem_cat[order[k]].rss - g_mem_cat[order[k]].base > g_mem_cat[order[i]].rss - g_mem_cat[order[i]].base) { int x = order[i]; order[i] = order[k]; order[k] = x; }
    b_init(&b, t, sizeof t); b_s(&b, "карта: выросло с первого снимка:");
    {
        int shown = 0;
        for (i = 0; i < n && i < 3; i++) {
            MemCat* c = &g_mem_cat[order[i]];
            if (c->rss - c->base < 10 * 1024) break;
            b_s(&b, shown ? "; " : " "); b_s(&b, c->name); b_ch(&b, ' '); mem_b_dmb(&b, c->rss - c->base); shown++;
        }
        if (!shown) b_s(&b, " ничего заметного (>10 МБ)");
    }
    mem_log(t);
}

/* ───────────── API IL2CPP ───────────── */
static int mem_api(void) {
    if (g_mem_apiTried) return g_mem_apiOk;
    if (!g_il2cpp) return 0;
    g_mem_apiTried = 1;
    *(void**)&MX.gc_collect = dlsym(g_il2cpp, "il2cpp_gc_collect");
    *(void**)&MX.gc_used = dlsym(g_il2cpp, "il2cpp_gc_get_used_size");
    *(void**)&MX.gc_heap = dlsym(g_il2cpp, "il2cpp_gc_get_heap_size");
    *(void**)&MX.resolve_icall = dlsym(g_il2cpp, "il2cpp_resolve_icall");
    *(void**)&MX.threads_all = dlsym(g_il2cpp, "il2cpp_thread_get_all_attached_threads");
    g_mem_apiOk = MX.gc_collect != NULL;
    return g_mem_apiOk;
}
static void mem_log_api(void) {
    char t[400]; Buf b; b_init(&b, t, sizeof t);
    b_s(&b, "API: il2cpp_gc_collect "); b_s(&b, MX.gc_collect ? "есть" : "НЕТ");
    b_s(&b, ", gc_get_used/heap_size "); b_s(&b, MX.gc_used && MX.gc_heap ? "есть" : "НЕТ");
    b_s(&b, ", resolve_icall "); b_s(&b, MX.resolve_icall ? "есть" : "НЕТ");
    b_s(&b, ", thread_get_all_attached_threads "); b_s(&b, MX.threads_all ? "есть" : "НЕТ");
    b_s(&b, ", mallopt "); b_s(&b, MLC.mallopt ? "есть" : "НЕТ"); b_s(&b, ", mallinfo "); b_s(&b, MLC.mallinfo ? "есть" : "НЕТ");
    mem_log(t);
}

/* ───────────── главный поток: поиск контекста и Post ───────────── */
static void native_mem_cmd(void* self, void* args, void* mi);
/* Ищем UnitySynchronizationContext у управляемых Thread всех присоединённых потоков (у главного он лежит в m_ExecutionContext._syncContext).
   Значения кладём в глобальные g_mt_* ядра — тот же контекст использует /nocol и /route, повторный захват там просто пропускается. */
static int mem_grab_ctx(void) {
    void *img, **th, *kcb, *pm; uptr n = 0, i;
    if (g_mt_ctx && g_mt_postM && g_mt_kCb) return 1;
    if (g_mem_ctxTries >= 8) return 0;
    if (!g_ready || g_tplState != 1 || !g_tplDelegate || !mem_api() || !MX.threads_all) return 0;
    g_mem_ctxTries++;
    ensure_attached();
    th = MX.threads_all(&n);
    if (!th || n == 0 || n > 4096) { mem_log("главный поток: список потоков IL2CPP пуст"); return 0; }
    for (i = 0; i < n; i++) {
        void *t = th[i], *ec, *sc;
        if (!t || !hm_obj_ok(t)) continue;
        ec = ref_field_s(t, "m_ExecutionContext"); if (!ec) continue;
        sc = ref_field_s(ec, "_syncContext"); if (!sc) sc = ref_field_s(ec, "_syncContextNoFlow");
        if (!sc || !hm_obj_ok(sc)) continue;
        if (!seq(A.class_get_name(A.object_get_class(sc)), "UnitySynchronizationContext")) continue;
        pm = find_method(A.object_get_class(sc), "Post", 2);
        if (!pm) { mem_log("главный поток: у UnitySynchronizationContext нет Post"); return 0; }
        img = find_image_pre("mscorlib");
        kcb = img ? A.class_from_name(img, "System.Threading", "SendOrPostCallback") : NULL;
        g_mt_kCb = (kcb && A.class_instance_size && A.class_instance_size(kcb) == g_cmdSize) ? kcb : g_cmdKls;
        g_mt_postM = pm; g_mt_ctx = sc;                  /* ctx — последним: остальные моды проверяют его первым */
        mem_log_i("главный поток: контекст найден сам (потоков просмотрено ", (long)(i + 1)); mem_log("главный поток: готов к Post");
        return 1;
    }
    mem_log_i("главный поток: контекст не найден среди потоков, попытка ", g_mem_ctxTries);
    return 0;
}
static int mem_main_ready(void) {
    if (g_mem_postFail >= 3) return 0;
    if (g_mt_ctx && g_mt_postM && g_mt_kCb) return 1;
    return mem_grab_ctx();
}
static int mem_post_main(void) {
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_mem_cmd);
    st = A.string_new("mem-job");
    if (!cb || !st) { mem_log("главный поток: не создался делегат"); g_mem_postFail++; return 0; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); mem_log2("главный поток: Post бросил: ", et); g_mem_postFail++; return 0; }
    return 1;
}

/* ───────────── сами действия очистки ───────────── */
static void mem_do_gc(const char* where) {
    long long u0, h0, u1, h1, t0, t1; char t[400]; Buf b;
    if (!mem_api() || !MX.gc_collect) { mem_log("GC: нет il2cpp_gc_collect"); return; }
    mem_gc_sizes(&u0, &h0);
    t0 = mem_now_ms(); MX.gc_collect(2); t1 = mem_now_ms();
    mem_gc_sizes(&u1, &h1);
    b_init(&b, t, sizeof t); b_s(&b, "GC ("); b_s(&b, where); b_s(&b, "): используется "); mem_b_mb(&b, u0); b_s(&b, " → "); mem_b_mb(&b, u1);
    b_s(&b, " МБ, куча "); mem_b_mb(&b, h0); b_s(&b, " → "); mem_b_mb(&b, h1); b_s(&b, " МБ, "); b_int(&b, (long)(t1 - t0)); b_s(&b, " мс");
    mem_log(t);
}
static void mem_do_unload(void) {                       /* только главный поток */
    static const char* names[] = { "UnityEngine.Resources::UnloadUnusedAssets()", "UnityEngine.Resources::UnloadUnusedAssets" };
    void* exc[2]; long long t0, t1; int i;
    if (!g_mem_unloadFn && !g_mem_unloadTried) {
        g_mem_unloadTried = 1;
        if (mem_api() && MX.resolve_icall) {
            for (i = 0; i < 2 && !g_mem_unloadFn; i++) {
                *(void**)&g_mem_unloadFn = MX.resolve_icall(names[i]);
                if (g_mem_unloadFn) mem_log2("UnloadUnusedAssets: иккол найден под именем ", names[i]);
            }
        }
        if (!g_mem_unloadFn) mem_log("UnloadUnusedAssets: иккол НЕ найден (il2cpp_resolve_icall вернул NULL) — выгрузка ассетов недоступна");
    }
    if (!g_mem_unloadFn) return;
    exc[0] = NULL; exc[1] = NULL;
    t0 = mem_now_ms();
    g_mem_unloadFn((void*)exc);                          /* лишний аргумент безвреден: на ARM/x86 его просто не читают; результат (AsyncOperation) не нужен */
    t1 = mem_now_ms();
    mem_log_i("UnloadUnusedAssets: запущена (асинхронно), вызов занял мс: ", (long)(t1 - t0));
}
static void mem_do_tex(int lim) {                        /* только главный поток */
    void *img, *k, *m, *exc = NULL; int v = lim; void* a[1]; char et[200];
    img = find_image_pre("UnityEngine.CoreModule");
    k = img ? A.class_from_name(img, "UnityEngine", "QualitySettings") : NULL;
    m = k ? A.class_get_method_from_name(k, "set_masterTextureLimit", 1) : NULL;
    if (!m) { mem_log("masterTextureLimit: у QualitySettings нет set_masterTextureLimit"); return; }
    a[0] = &v;
    A.runtime_invoke(m, NULL, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); mem_log2("masterTextureLimit: исключение: ", et); return; }
    g_mem_texLimit = lim;
    mem_log_i("masterTextureLimit установлен: ", lim);
}
/* то, что исполняется в главном потоке (из Post или из команды /mem) */
static void mem_main_job(void) {
    int mf = g_mem_jobKind;
    if (!mf) return;                                    /* заказ уже снят (поток мода не дождался) */
    if (mf & MEM_J_GC) mem_do_gc("главный поток");
    if (mf & MEM_J_UNLOAD) mem_do_unload();
    if (mf & MEM_J_TEX) mem_do_tex(g_mem_texWant);
    g_mem_jobDone = 1;
}
static void mem_do_purge(void) {
    int r1 = -999, r2 = -999, r3 = -999; char t[300]; Buf b;
    mem_libc();
    if (MLC.mallopt) { r1 = MLC.mallopt(-101, 0); r2 = MLC.mallopt(-104, 0); }          /* M_PURGE, M_PURGE_ALL */
    if (MLC.malloc_trim) r3 = MLC.malloc_trim(0);
    b_init(&b, t, sizeof t); b_s(&b, "возврат памяти ОС: mallopt(M_PURGE) "); b_int(&b, r1); b_s(&b, ", M_PURGE_ALL "); b_int(&b, r2);
    b_s(&b, ", malloc_trim "); b_int(&b, r3); b_s(&b, "  (−999 — функции нет, 0 у mallopt — не поддерживается этой Android)");
    mem_log(t);
}

/* ───────────── очистка целиком (поток мода) ───────────── */
static void mem_run_job(int flags, const char* why) {
    MemSample b, a; long long t0 = mem_now_ms(), freed; int mainOk = 0, mf = flags & (MEM_J_GC | MEM_J_UNLOAD | MEM_J_TEX), sec, i; long now = time(NULL);
    char t[800]; Buf x; long long r[8];
    if (g_mem_busy) return;
    g_mem_busy = 1; g_mem_jobs++;
    g_mem_lastJobS = now;
    if (flags & MEM_J_GC) g_mem_lastGcS = now;
    if (flags & MEM_J_UNLOAD) g_mem_lastFullS = now;
    mem_take(&b, 1);
    b_init(&x, t, sizeof t); b_s(&x, "ОЧИСТКА #"); b_int(&x, g_mem_jobs); b_s(&x, " начало, причина: "); b_s(&x, why);
    b_s(&x, "; шаги:");
    if (flags & MEM_J_GC) b_s(&x, " GC");
    if (flags & MEM_J_UNLOAD) b_s(&x, " выгрузка-ассетов");
    if (flags & MEM_J_TEX) b_s(&x, " текстуры");
    if (flags & MEM_J_PURGE) b_s(&x, " возврат-ОС");
    b_s(&x, "; до: RSS "); mem_b_mb(&x, b.rss); b_s(&x, ", ОЗУ доступно "); mem_b_mb(&x, b.avail); b_s(&x, ", GC "); mem_b_mb(&x, b.gcUsed); b_ch(&x, '/'); mem_b_mb(&x, b.gcHeap);
    b_s(&x, " МБ");
    mem_log(t);
    if (mf) {
        if (mem_main_ready()) {
            g_mem_jobDone = 0; g_mem_jobKind = mf;
            if (mem_post_main()) {
                for (i = 0; i < 400 && !g_mem_jobDone; i++) usleep(50000);                 /* до 20 с: главный поток мог быть на паузе */
                mainOk = g_mem_jobDone;
                if (!mainOk) mem_log("главный поток не выполнил заказ за 20 с (игра на паузе/в фоне?) — заказ снят");
            }
            g_mem_jobKind = 0;
        }
        if (!mainOk) {                                                                       /* запасной путь: GC можно и из потока мода */
            if (mf & MEM_J_GC) mem_do_gc("поток мода");
            if (mf & (MEM_J_UNLOAD | MEM_J_TEX)) mem_log("выгрузка ассетов/качество пропущены: нет доступа к главному потоку (наберите /mem один раз)");
        }
    }
    sec = (mainOk && (flags & MEM_J_UNLOAD) && g_mem_unloadFn) ? MEM_SETTLE_FULL_S : MEM_SETTLE_GC_S;
    for (i = 0; i < 8; i++) r[i] = -1;
    for (i = 1; i <= sec; i++) { usleep(1000000); if (i == 1 || i == 3 || i == sec) r[i < 8 ? i : 7] = mem_rss_now(); }
    if (flags & MEM_J_PURGE) mem_do_purge();
    mem_take(&a, 1);
    freed = (b.rss >= 0 && a.rss >= 0) ? b.rss - a.rss : 0;
    g_mem_freedKB += freed > 0 ? freed : 0;
    b_init(&x, t, sizeof t); b_s(&x, "ОЧИСТКА #"); b_int(&x, g_mem_jobs); b_s(&x, " итог: RSS "); mem_b_mb(&x, b.rss); b_s(&x, " → "); mem_b_mb(&x, a.rss);
    b_s(&x, " МБ ("); mem_b_dmb(&x, a.rss - b.rss); b_s(&x, "), ОЗУ доступно "); mem_b_mb(&x, b.avail); b_s(&x, " → "); mem_b_mb(&x, a.avail);
    b_s(&x, " МБ, GC "); mem_b_mb(&x, b.gcUsed); b_s(&x, " → "); mem_b_mb(&x, a.gcUsed); b_s(&x, "/"); mem_b_mb(&x, a.gcHeap);
    b_s(&x, " МБ, malloc "); mem_b_mb(&x, b.natUsed); b_s(&x, " → "); mem_b_mb(&x, a.natUsed); b_s(&x, "/"); mem_b_mb(&x, a.natMapped);
    b_s(&x, " МБ; всего за "); b_fix(&x, (double)(mem_now_ms() - t0) / 1000.0, 1); b_s(&x, " с");
    if (sec > 1) { b_s(&x, "; RSS по секундам после: 1с "); mem_b_mb(&x, r[1]); b_s(&x, ", 3с "); mem_b_mb(&x, r[3]); b_s(&x, ", "); b_int(&x, sec); b_s(&x, "с "); mem_b_mb(&x, r[sec < 8 ? sec : 7]); }
    mem_log(t);
    { char d[300]; Buf y; b_init(&y, d, sizeof d); b_s(&y, "#"); b_int(&y, g_mem_jobs); b_s(&y, ": RSS "); mem_b_mb(&y, b.rss); b_s(&y, " → "); mem_b_mb(&y, a.rss); b_s(&y, " МБ, доступно "); mem_b_mb(&y, a.avail); b_s(&y, " МБ; причина: "); b_s(&y, why);
      mem_copy(g_mem_lastSummary, d, sizeof g_mem_lastSummary); mem_both(d); }
    g_mem_level = mem_level(&a);
    /* помогло ли: если и после полной очистки памяти критически мало, а освободилось мало — копим неудачи и в итоге режем текстуры */
    if ((flags & MEM_J_UNLOAD) && g_mem_level >= 2 && freed < 30 * 1024) g_mem_failStreak++;
    else if (g_mem_level < 2) g_mem_failStreak = 0;
    g_mem_busy = 0;
}

/* ───────────── файл-переключатель ───────────── */
static void mem_cfg_poll(long now) {
    char t[160]; int n, f = 0, i = 0;
    if (now < g_mem_cfgPoll) return;
    g_mem_cfgPoll = now + 5;
    n = mem_read_file(g_mem_cfgPath, t, sizeof t);
    if (n > 0) {
        while (t[i]) {
            char w[24]; int q = 0;
            while (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t' || t[i] == ',') i++;
            while (t[i] && t[i] != ' ' && t[i] != '\n' && t[i] != '\r' && t[i] != '\t' && t[i] != ',') { if (q < 23) { char c = t[i]; w[q++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } i++; }
            w[q] = 0;
            if (seq(w, "monitor")) f |= MEM_C_MONITOR; else if (seq(w, "nounload")) f |= MEM_C_NOUNLOAD; else if (seq(w, "notex")) f |= MEM_C_NOTEX;
            else if (seq(w, "nogc")) f |= MEM_C_NOGC; else if (seq(w, "nopurge")) f |= MEM_C_NOPURGE;
        }
    }
    if (f != g_mem_cfg) {
        char m[200]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "konstt_mem.txt: режим:");
        if (!f) b_s(&b, " обычный (всё включено)");
        if (f & MEM_C_MONITOR) b_s(&b, " monitor (только замеры)");
        if (f & MEM_C_NOUNLOAD) b_s(&b, " nounload");
        if (f & MEM_C_NOTEX) b_s(&b, " notex");
        if (f & MEM_C_NOGC) b_s(&b, " nogc");
        if (f & MEM_C_NOPURGE) b_s(&b, " nopurge");
        mem_both(m);
        g_mem_cfg = f;
    }
}

/* ───────────── «стоим на месте» ───────────── */
static void mem_idle_tick(long now) {
    void* me; float p[3], dx, dy, dz;
    if (!g_ready || !g_poolSrcField || !g_domain) { g_mem_inGame = 0; return; }
    ensure_attached();
    me = local_player();
    if (!me) { g_mem_inGame = 0; g_mem_hasAnchor = 0; g_mem_idleSince = 0; return; }
    g_mem_inGame = 1;
    if (!read_pos(me, p)) return;
    if (!g_mem_hasAnchor) { g_mem_anchor[0] = p[0]; g_mem_anchor[1] = p[1]; g_mem_anchor[2] = p[2]; g_mem_hasAnchor = 1; g_mem_idleSince = now; return; }
    dx = p[0] - g_mem_anchor[0]; dy = p[1] - g_mem_anchor[1]; dz = p[2] - g_mem_anchor[2];
    if (dx * dx + dy * dy + dz * dz > MEM_MOVE_M2) { g_mem_anchor[0] = p[0]; g_mem_anchor[1] = p[1]; g_mem_anchor[2] = p[2]; g_mem_idleSince = now; }
}

/* ───────────── политика ───────────── */
static void mem_policy(const MemSample* s, long now, long idle) {
    int flags = 0; char why[120]; Buf w; long gap;
    if (!g_mem_auto || (g_mem_cfg & MEM_C_MONITOR) || !g_ready || g_mem_busy) return;
    if (now - g_mem_readyAt < MEM_WARMUP_S) return;
    if (now - g_mem_lastJobS < MEM_GAP_S) return;
    b_init(&w, why, sizeof why);
    if (g_mem_level >= 2) {
        gap = (long)MEM_CRIT_GAP_S * (1 + (g_mem_failStreak < 5 ? g_mem_failStreak : 5));
        if (now - g_mem_lastFullS >= gap) { flags = MEM_J_GC | MEM_J_UNLOAD | MEM_J_PURGE; b_s(&w, "критически мало памяти (доступно "); mem_b_mb(&w, s->avail); b_s(&w, " МБ)"); }
        if (flags && g_mem_failStreak >= 2 && g_mem_texLimit < MEM_TEX_AUTO_MAX && !(g_mem_cfg & MEM_C_NOTEX)) {
            flags |= MEM_J_TEX; g_mem_texWant = g_mem_texLimit + 1; g_mem_failStreak = 0; b_s(&w, ", очистка не помогает — режу текстуры");
        }
    } else if (g_mem_level == 1) {
        if (idle >= 60 && now - g_mem_lastFullS >= 300) { flags = MEM_J_GC | MEM_J_UNLOAD | MEM_J_PURGE; b_s(&w, "мало памяти и стоим "); b_int(&w, idle); b_s(&w, " с"); }
        else if (now - g_mem_lastGcS >= MEM_LOW_GAP_S) { flags = MEM_J_GC | MEM_J_PURGE; b_s(&w, "мало памяти (доступно "); mem_b_mb(&w, s->avail); b_s(&w, " МБ)"); }
    } else if (g_mem_inGame) {
        if (idle >= MEM_IDLE_FULL_S && now - g_mem_lastFullS >= MEM_IDLE_FULL_EVERY_S) { flags = MEM_J_GC | MEM_J_UNLOAD | MEM_J_PURGE; b_s(&w, "плановая, стоим "); b_int(&w, idle); b_s(&w, " с"); }
        else if (idle >= MEM_IDLE_GC_S && now - g_mem_lastGcS >= MEM_IDLE_GC_EVERY_S) { flags = MEM_J_GC | MEM_J_PURGE; b_s(&w, "плановая (GC), стоим "); b_int(&w, idle); b_s(&w, " с"); }
    }
    if (g_mem_cfg & MEM_C_NOUNLOAD) flags &= ~MEM_J_UNLOAD;
    if (g_mem_cfg & MEM_C_NOGC) flags &= ~MEM_J_GC;
    if (g_mem_cfg & MEM_C_NOPURGE) flags &= ~MEM_J_PURGE;
    if (g_mem_cfg & MEM_C_NOTEX) flags &= ~MEM_J_TEX;
    if (!flags) return;
    mem_run_job(flags, why);
}

/* ───────────── поток мода ───────────── */
static MemSample g_mem_last, g_mem_trendBase;
static int g_mem_haveTrend; static long g_mem_trendT;
static void mem_trend(const MemSample* s, long now) {
    char t[500]; Buf b;
    if (g_mem_haveTrend && s->rss >= 0 && g_mem_trendBase.rss >= 0) {
        b_init(&b, t, sizeof t); b_s(&b, "тренд за "); b_int(&b, now - g_mem_trendT); b_s(&b, " с: RSS "); mem_b_dmb(&b, s->rss - g_mem_trendBase.rss);
        b_s(&b, " МБ, анон "); mem_b_dmb(&b, (s->anon >= 0 && g_mem_trendBase.anon >= 0) ? s->anon - g_mem_trendBase.anon : 0);
        b_s(&b, ", malloc "); mem_b_dmb(&b, (s->natUsed >= 0 && g_mem_trendBase.natUsed >= 0) ? s->natUsed - g_mem_trendBase.natUsed : 0);
        b_s(&b, ", GC "); mem_b_dmb(&b, (s->gcUsed >= 0 && g_mem_trendBase.gcUsed >= 0) ? s->gcUsed - g_mem_trendBase.gcUsed : 0);
        b_s(&b, ", потоков "); b_int(&b, s->threads - g_mem_trendBase.threads); b_s(&b, ", fd "); b_int(&b, s->fds - g_mem_trendBase.fds);
        b_s(&b, " (вычитание: «+» — растёт, «−» — освободилось; очистки в этом окне тоже учтены)");
        mem_log(t);
    }
    g_mem_trendBase = *s; g_mem_haveTrend = 1; g_mem_trendT = now;
}
static void mem_tail_of_prev(void) {                                          /* последние строки прошлого запуска — в начало нового журнала */
    char buf[1536], tmp[512]; int fd, n = 0, i, e; long r;
    fd = open(g_mem_prev, 0);
    if (fd < 0) { mem_log("прошлого журнала нет (первый запуск или файл удалён)"); return; }
    for (;;) {                                                                 /* читаем весь файл, оставляя в buf только хвост */
        r = read(fd, tmp, sizeof tmp);
        if (r <= 0) break;
        if (n + r >= (long)sizeof buf) { int drop = n + (int)r - (int)sizeof buf + 1; for (i = 0; i + drop < n; i++) buf[i] = buf[i + drop]; n -= drop; if (n < 0) n = 0; }
        for (i = 0; i < r && n + 1 < (int)sizeof buf; i++) buf[n++] = tmp[i];
    }
    close(fd); buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (n == 0) { mem_log("прошлый журнал пуст"); return; }
    e = n; for (i = n - 1, r = 0; i >= 0; i--) if (buf[i] == '\n') { r++; if (r == 2) { e = i + 1; break; } if (r == 1) e = i + 1; }
    if (r < 2) e = 0;
    mem_log2("прошлый запуск, последние строки журнала: ", ""); mem_log(buf + e);
}
static int mem_log_open(void) {
    unsigned L;
#ifdef MEM_LOG_DIR
    mem_copy(g_mem_dir, MEM_LOG_DIR, sizeof g_mem_dir);
#else
    int w;
    for (w = 0; w < 50 && !g_flog[0]; w++) usleep(100000);                    /* ядро создаёт путь при первой записи в konstt.log */
    L = slen(g_flog);
    if (L <= HM_LOGNAME_LEN || L >= 290) return 0;
    { unsigned i; for (i = 0; i < L - HM_LOGNAME_LEN; i++) g_mem_dir[i] = g_flog[i]; g_mem_dir[i] = 0; }   /* папка со слэшем (без «konstt.log») */
#endif
    (void)L;
    mem_paths();
    rename(g_mem_path, g_mem_prev);
    if (!flog_write(g_mem_path, "", 0, 1)) return 0;
    g_mem_logOn = 1;
    return 1;
}
static void mem_pkg_line(void) {
    char t[400], pk[100]; Buf b; MemSample s;
    mem_take(&s, 1);
    pk[0] = 0; pkg_name(pk, sizeof pk);
    b_init(&b, t, sizeof t); b_s(&b, "== konstt_mem: сборка "); b_s(&b, HM_BUILD_ID); b_s(&b, "; пакет "); b_s(&b, pk); b_s(&b, "; процесс "); b_int(&b, (long)(sizeof(void*) * 8));
    b_s(&b, "-бит; ОЗУ системы "); mem_b_mb(&b, s.total); b_s(&b, " МБ, доступно "); mem_b_mb(&b, s.avail); b_s(&b, " МБ; страница "); b_int(&b, g_mem_pageKb); b_s(&b, " кБ");
    mem_log(t);
    b_init(&b, t, sizeof t); b_s(&b, "пороги: мало — доступно < "); {
        long long low = s.total > 0 ? s.total * MEM_LOW_PCT / 100 : 0, crit = s.total > 0 ? s.total * MEM_CRIT_PCT / 100 : 0;
        if (low < 256LL * 1024) low = 256LL * 1024;
        if (low > 600LL * 1024) low = 600LL * 1024;
        if (crit < 128LL * 1024) crit = 128LL * 1024;
        if (crit > 256LL * 1024) crit = 256LL * 1024;
        mem_b_mb(&b, low); b_s(&b, " МБ, критично — < "); mem_b_mb(&b, crit); b_s(&b, " МБ; авто-очистка: "); b_s(&b, g_mem_auto ? "вкл" : "выкл");
    }
    b_s(&b, "; замер каждые "); b_int(&b, MEM_SAMPLE_S); b_s(&b, " с");
    mem_log(t);
}
static void* mem_main(void* arg) {
    long now; MemSample s; int apiLogged = 0, firstMaps = 0;
    (void)arg;
    if (!hm_wait_started()) return NULL;                                      /* konstt_off — мод выключен целиком */
    g_mem_t0 = time(NULL);
    if (!mem_log_open()) { dlog("mem: журнал HLOG/konstt_mem.log открыть не удалось — замеры идут только в память"); }
    else dlog2("mem: журнал памяти: ", g_mem_path);
    mem_pkg_line();
    mem_tail_of_prev();
    g_mem_nextSample = time(NULL) + 2; g_mem_nextTrend = time(NULL) + MEM_TREND_EVERY_S;
    for (;;) {
        usleep(1000000);
        now = time(NULL);
        mem_cfg_poll(now);
        if (g_ready && !g_mem_readyAt) { g_mem_readyAt = now; g_mem_nextMaps = now + 30; mem_both("игра готова, начинаю следить и чистить"); }
        if (g_ready && !apiLogged && mem_api()) { apiLogged = 1; mem_log_api(); }
        if (g_ready && !g_mem_ctxTries && g_tplState == 1) mem_grab_ctx();     /* контекст главного потока заранее, а не в момент нужды */
        mem_idle_tick(now);
        if (g_mem_cmdReq) {                                                    /* заказ из чата */
            int r = g_mem_cmdReq; g_mem_cmdReq = 0;
            if (r & MEM_J_MAPS) mem_log_maps("по команде");
            if (r & ~MEM_J_MAPS) mem_run_job(r & ~MEM_J_MAPS, "команда /mem");
        }
        if (now >= g_mem_nextSample) {
            long idle = (g_mem_inGame && g_mem_idleSince) ? now - g_mem_idleSince : 0;
            mem_take(&s, 1);
            g_mem_level = mem_level(&s);
            g_mem_last = s;
            mem_sample_line(&s, idle);
            if (now >= g_mem_nextTrend) { mem_trend(&s, now); g_mem_nextTrend = now + MEM_TREND_EVERY_S; }
            if (g_mem_readyAt && now >= g_mem_nextMaps) { mem_log_maps(firstMaps ? "плановая" : "первый снимок"); firstMaps = 1; g_mem_nextMaps = now + MEM_MAPS_EVERY_S; }
            mem_policy(&s, now, idle);
            g_mem_nextSample = time(NULL) + (g_mem_level ? MEM_SAMPLE_LOW_S : MEM_SAMPLE_S);
        }
    }
    return NULL;
}

/* ───────────── чат-команда /mem ───────────── */
#define MEM_W_GC 1
#define MEM_W_FULL 2
#define MEM_W_PURGE 4
#define MEM_W_TEX 8
#define MEM_W_AUTO 16
#define MEM_W_ON 32
#define MEM_W_OFF 64
#define MEM_W_MAPS 128
static int mem_parse(void* arr, int* num) {
    long len; int i, w = 0; char* data;
    *num = -1;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[96]; int n, p = 0; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 60 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        while (tmp[p]) {
            char tok[24]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 23) { char c = tmp[p]; tok[q++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } p++; }
            tok[q] = 0;
            if (seq(tok, "gc")) w |= MEM_W_GC; else if (seq(tok, "full")) w |= MEM_W_FULL; else if (seq(tok, "purge")) w |= MEM_W_PURGE;
            else if (seq(tok, "tex")) w |= MEM_W_TEX; else if (seq(tok, "auto")) w |= MEM_W_AUTO; else if (seq(tok, "on") || seq(tok, "1")) w |= MEM_W_ON;
            else if (seq(tok, "off") || seq(tok, "0")) w |= MEM_W_OFF; else if (seq(tok, "maps")) w |= MEM_W_MAPS;
            else if (tok[0] >= '0' && tok[0] <= '9' && *num < 0) { int v = 0, k = 0; while (tok[k] >= '0' && tok[k] <= '9' && v < 1000) v = v * 10 + (tok[k++] - '0'); *num = v; }
        }
    }
    return w;
}
static void mem_say_status(void) {
    char m[420]; Buf b; MemSample s = g_mem_last;
    b_init(&b, m, sizeof m);
    b_s(&b, "Память: RSS "); mem_b_mb(&b, s.rss); b_s(&b, " МБ, ОЗУ доступно "); mem_b_mb(&b, s.avail); b_s(&b, " из "); mem_b_mb(&b, s.total);
    b_s(&b, " МБ, GC "); mem_b_mb(&b, s.gcUsed); b_ch(&b, '/'); mem_b_mb(&b, s.gcHeap); b_s(&b, " МБ; уровень: "); b_s(&b, g_mem_level >= 2 ? "КРИТИЧНО" : g_mem_level == 1 ? "мало" : "норма");
    chat_say(m);
    b_init(&b, m, sizeof m);
    b_s(&b, "Авто-очистка: "); b_s(&b, g_mem_auto ? (g_mem_cfg & MEM_C_MONITOR ? "выкл (файл monitor)" : "вкл") : "выкл");
    b_s(&b, "; очисток "); b_int(&b, g_mem_jobs); b_s(&b, ", освобождено ≈ "); b_int(&b, (long)(g_mem_freedKB / 1024)); b_s(&b, " МБ; текстуры: ограничение "); b_int(&b, g_mem_texLimit);
    b_s(&b, "; главный поток: "); b_s(&b, (g_mt_ctx && g_mt_postM) ? "найден" : "не найден"); b_s(&b, "; журнал: HLOG/konstt_mem.log");
    chat_say(m);
    if (g_mem_lastSummary[0]) { b_init(&b, m, sizeof m); b_s(&b, "Последняя очистка: "); b_s(&b, g_mem_lastSummary); chat_say(m); }
}
static void native_mem_cmd(void* self, void* args, void* mi) {
    int w, num; (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { mem_main_job(); return; }   /* Post из потока мода (state — строка), а не чат (string[]) */
    mt_capture_ctx();                                                   /* мы в главном потоке: заодно запомним его контекст (запасной путь) */
    w = mem_parse(args, &num);
    if (w & MEM_W_AUTO) {
        if (w & MEM_W_OFF) { g_mem_auto = 0; chat_say("Память: авто-очистка выключена (замеры продолжаются). Включить: /mem auto on"); }
        else if (w & MEM_W_ON) { g_mem_auto = 1; chat_say("Память: авто-очистка включена"); }
        else chat_say(g_mem_auto ? "Память: авто-очистка включена. Выключить: /mem auto off" : "Память: авто-очистка выключена. Включить: /mem auto on");
        mem_log2("команда /mem auto: ", g_mem_auto ? "вкл" : "выкл");
        return;
    }
    if (w & MEM_W_TEX) {
        if (num < 0 || num > 3) {
            char m[200]; Buf b; b_init(&b, m, sizeof m);
            b_s(&b, "Память: /mem tex N — N от 0 (полное качество) до 3 (текстуры в 8 раз меньше). Сейчас: "); b_int(&b, g_mem_texLimit);
            chat_say(m); return;
        }
        g_mem_texWant = num; g_mem_cmdReq |= MEM_J_TEX;
        chat_say("Память: качество текстур меняю (подробности в HLOG/konstt_mem.log)");
        return;
    }
    if (w & MEM_W_MAPS) { g_mem_cmdReq |= MEM_J_MAPS; chat_say("Память: снимок карты памяти будет в HLOG/konstt_mem.log через секунду"); return; }
    if (w & MEM_W_FULL) { g_mem_cmdReq |= MEM_J_GC | MEM_J_UNLOAD | MEM_J_PURGE; chat_say("Память: полная очистка запущена (может подвиснуть на секунду-две). Итог — /mem и HLOG/konstt_mem.log"); return; }
    if (w & MEM_W_GC) { g_mem_cmdReq |= MEM_J_GC | MEM_J_PURGE; chat_say("Память: GC и возврат памяти ОС запущены. Итог — /mem и HLOG/konstt_mem.log"); return; }
    if (w & MEM_W_PURGE) { g_mem_cmdReq |= MEM_J_PURGE; chat_say("Память: возврат памяти ОС запущен. Итог — HLOG/konstt_mem.log"); return; }
    mem_say_status();
}
static void* mem_cmd_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("mem", native_mem_cmd)) return NULL;
    mem_log("команда /mem зарегистрирована");
    return NULL;
}

#ifndef KONSTT_TEST
__attribute__((constructor)) static void mem_ctor(void) {      /* только создаёт потоки-ждуны; до готовности игры они в IL2CPP ничего не трогают */
    unsigned long t = 0, t2 = 0;
    pthread_create(&t, NULL, mem_main, NULL);
    pthread_create(&t2, NULL, mem_cmd_main, NULL);
}
#endif
