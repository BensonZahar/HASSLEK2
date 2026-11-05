/*
 * hassle_ofs.c — необязательный мод «/ofs» и «/inv»: правка полей onfoot-пакета (ID 207) прямо перед отправкой на сервер.
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите этот файл и пересоберите бандл.
 *
 * Зачем. /spec (пакет 212 вместо 207) оставляет NPC видимым для других игроков. В Lua (INVIS.lua, SAMP-клиент) невидимость
 * делается иначе: в обычном onfoot-пакете меняются поля — SpecialAction=3 («садится в машину»), SurfingInfo = ID ближней машины,
 * SurfingOffsets = (50.5, 50.5, -50.5) — другие клиенты рисуют игрока «стоящим на крыше машины» в 50 м от неё, под землёй.
 * Здесь та же логика, только на уровне клиента Hassle: клиент по-прежнему шлёт НАСТОЯЩИЙ onfoot (AFK-таймер, PayDay, чекпоинты,
 * позиция — всё как у обычного игрока), но поля подменяются.
 *
 * Команды чата:
 *   /ofs on | off            включить / выключить подмену (при первом on ставится перехват)
 *   /ofs status              состояние: перехват, счётчики, какая анимка уходит от клиента, что подменено
 *   /ofs anim                показать таблицу замен анимаций (по умолчанию 1215 → 1300)
 *   /ofs anim 1215 1300      слать вместо анимки 1215 анимку 1300 (до 8 замен)
 *   /ofs anim off            очистить таблицу замен;   /ofs anim reset — вернуть 1215 → 1300
 *   /ofs inv on | off        невидимость по логике Lua (то же, что /invis)
 *   /ofs inv radius 50.6     радиус поиска машины;  /ofs inv hp rand|<0-255>|off — здоровье в пакете;  /ofs inv anim <id>|off — анимка в режиме inv
 *   /ofs set <поле> <знач…>  принудительно писать поле в каждый пакет (см. /ofs fields);  /ofs unset <поле>|all — снять
 *   /ofs fields              список полей
 *   /ofs unhook              вернуть код игры в исходный вид
 *   /invis                   переключатель невидимости (аналог /inv из Lua; назван иначе, чтобы не перекрыть возможную игровую /inv)
 *
 * Как это устроено (по дампу libil2cpp.so от 20.10.2026):
 *   • LocalPlayer.SendOnFootFullSyncData (RVA 0x12FAC6C) собирает OnFootSyncData (0x48 байт) на своём стеке и зовёт
 *     SyncData.WriteOnFootPacket(PacketData, ref OnFootSyncData) (RVA 0x135244C) — это и есть запись пакета 207 в сеть.
 *     Второй аргумент (x1) — указатель на ту самую структуру. Перехват стоит на входе WriteOnFootPacket: наш обработчик правит
 *     поля по указателю и возвращает управление игре — дальше она сериализует уже изменённые поля.
 *   • Поля структуры: LeftRightAnalog 0x0, UpDownAnalog 0x2, Keys 0x4, Position 0x8, Rotation 0x14, Health 0x24, Armour 0x25,
 *     CurrentWeapon 0x26, SpecialAction 0x27, MoveSpeed 0x28, SurfingOffsets 0x34, SurfingInfo 0x40, Animation 0x44 (int).
 *     Смещения берутся из метаданных при установке, числа выше — только для сверки в журнале.
 *   • Перехват: первые 16 байт функции заменяются на «LDR x16,=адрес; BR x16» (абсолютный прыжок), заглушка в анонимной странице
 *     сохраняет x0,x1,x2,x30, зовёт ofs_hook(packet, data), возвращает регистры, выполняет 4 затёртые инструкции пролога и прыгает
 *     на 5-ю. Третья инструкция пролога — CBZ x0 (проверка packet на null), она зависит от адреса, поэтому в заглушке она переписана
 *     как «CBNZ перепрыгнуть; LDR x16,=цель; BR x16». Остальные три проверяются на позиционную независимость.
 *   • Вызовов IL2CPP и выделений памяти внутри обработчика нет — только запись в структуру.
 *   • ID машины для режима inv ищется в главном потоке (Post, как в /nocol): Pools.Vehicles → VehiclePool.Get(id) → позиция, берётся
 *     ближайшая в радиусе. Нет машины рядом — подмена не делается, режим inv выключается (как в Lua: «CAR NOT FOUND»).
 *   • Побочный эффект: клиент сравнивает новый пакет с последним отправленным; после подмены они всегда различаются, поэтому пакеты
 *     уходят с максимальной частотой, а не раз в 0.5 с у стоящего. Для теста нормально; для долгой работы можно поднять интервал.
 *   • Только arm64. Журнал — HLOG/konstt.log, строки «ofs:».
 */
#pragma once
#include "hassle_core.c"

extern int clock_gettime(int, void*);

#if defined(__aarch64__)
#define OFS_ARM64 1
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int mprotect(void*, unsigned long, int);
extern long pwrite(int, const void*, unsigned long, long);
#endif

extern long lseek(int, long, int);
extern unsigned long pthread_self(void);

/* ───────────── отдельный текстовый журнал: HLOG/ofs_log.txt (рядом с konstt.log) ─────────────
   Каждая строка дописывается в файл сразу (open → write → close), поэтому при вылете последняя строка показывает, где всё оборвалось.
   Файл общий для всех запусков (в начале сессии пишется «===== сессия»), при размере больше OFS_TLOG_KEEP старое уходит в ofs_log_prev.txt.
   Путь — та же папка, что у konstt.log:  /sdcard/Android/data/<пакет>/files/HLOG/ofs_log.txt  (команда «/ofs log» покажет путь в чате).
   ofs_log* пишут и сюда, и в konstt.log; ofs_tlog — только сюда (для горячих мест вроде хука и обработчика вылета). */
#define OFS_TLOG_NAME "ofs_log.txt"
#define OFS_TLOG_PREV "ofs_log_prev.txt"
#define OFS_TLOG_KEEP 400000L
#define OFS_TLOG_SESSION 250000
static char g_ofs_tpath[330], g_ofs_tprev[330];
static volatile int g_ofs_tlock;
static int g_ofs_tstate;                        /* 0 — путь ещё не готов, 1 — пишем, -1 — писать некуда */
static long g_ofs_tbytes, g_ofs_t0;
static const char* volatile g_ofs_stage = "старт";   /* последний «этап» — печатается в отчёте о вылете */
static char g_ofs_lastline[200];                /* последняя строка ofs_log — тоже в отчёте о вылете */
#define OFS_STAGE(s) (g_ofs_stage = (s))

static long ofs_ms(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return ts[0] * 1000L + ts[1] / 1000000L; }
static void ofs_hex(Buf* b, unsigned long v, int digits) {
    static const char hx[] = "0123456789abcdef"; int i;
    b_s(b, "0x");
    for (i = digits * 4 - 4; i >= 0; i -= 4) b_ch(b, hx[(v >> i) & 15u]);
}
static void ofs_tlog_open(void) {
    unsigned n = 0, k; int fd; long sz = 0;
    if (!g_flog[0]) return;                     /* файловый журнал ядра ещё не создан — попробуем позже */
    while (g_flog[n]) n++;
    if (n <= HM_LOGNAME_LEN || n + 32 >= sizeof g_ofs_tpath) { g_ofs_tstate = -1; return; }
    n -= HM_LOGNAME_LEN;
    for (k = 0; k < n; k++) { g_ofs_tpath[k] = g_flog[k]; g_ofs_tprev[k] = g_flog[k]; }
    for (k = 0; OFS_TLOG_NAME[k]; k++) g_ofs_tpath[n + k] = OFS_TLOG_NAME[k];
    g_ofs_tpath[n + k] = 0;
    for (k = 0; OFS_TLOG_PREV[k]; k++) g_ofs_tprev[n + k] = OFS_TLOG_PREV[k];
    g_ofs_tprev[n + k] = 0;
    fd = open(g_ofs_tpath, 0x441, 0666);        /* O_WRONLY|O_CREAT|O_APPEND */
    if (fd >= 0) { sz = lseek(fd, 0, 2); close(fd); }
    if (sz > OFS_TLOG_KEEP) rename(g_ofs_tpath, g_ofs_tprev);
    g_ofs_t0 = ofs_ms();
    g_ofs_tstate = 1;
    {
        const char* hdr = "\n===== сессия ofs (konstt) =====\n";
        unsigned hn = 0; while (hdr[hn]) hn++;
        fd = open(g_ofs_tpath, 0x441, 0666);
        if (fd >= 0) { write(fd, hdr, hn); close(fd); }
    }
}
static void ofs_tlog(const char* line) {
    char t[900]; Buf b; int owned = 0, spins = 0, fd;
    for (;;) {                                  /* замок с ограничением ожидания: из обработчика сигнала зависать нельзя */
        if (!__sync_lock_test_and_set(&g_ofs_tlock, 1)) { owned = 1; break; }
        if (++spins > 3000) break;
        usleep(100);
    }
    if (g_ofs_tstate == 0) ofs_tlog_open();
    if (g_ofs_tstate == 1 && g_ofs_tbytes < OFS_TLOG_SESSION) {
        long ms = ofs_ms() - g_ofs_t0;
        b_init(&b, t, sizeof t);
        b_ch(&b, '['); b_int(&b, ms / 1000); b_ch(&b, '.');
        b_ch(&b, (char)('0' + (ms % 1000) / 100)); b_ch(&b, (char)('0' + (ms % 100) / 10)); b_ch(&b, (char)('0' + ms % 10));
        b_s(&b, "] "); b_s(&b, line); b_ch(&b, '\n');
        fd = open(g_ofs_tpath, 0x441, 0666);
        if (fd >= 0) { write(fd, t, b.n); close(fd); g_ofs_tbytes += (long)b.n; }
    }
    if (owned) __sync_lock_release(&g_ofs_tlock);
}
static void ofs_log(const char* s) {
    unsigned i = 0;
    for (; s[i] && i < sizeof g_ofs_lastline - 1; i++) g_ofs_lastline[i] = s[i];
    g_ofs_lastline[i] = 0;
    dlog(s); ofs_tlog(s);
}
static void ofs_log2(const char* a, const char* b) { char t[600]; Buf x; b_init(&x, t, sizeof t); b_s(&x, a); b_s(&x, b); ofs_log(t); }
static void ofs_logi(const char* a, long v) { char t[600]; Buf x; b_init(&x, t, sizeof t); b_s(&x, a); b_int(&x, v); ofs_log(t); }

/* ───────────── поля OnFootSyncData ───────────── */
enum { OF_LR, OF_UD, OF_KEYS, OF_POS, OF_ROT, OF_HP, OF_AP, OF_WEAP, OF_SPEC, OF_SPD, OF_SOFF, OF_SURF, OF_ANIM, OF_N };
static const char* const g_ofs_fname[OF_N] = { "LeftRightAnalog", "UpDownAnalog", "Keys", "Position", "Rotation", "Health", "Armour",
                                                "CurrentWeapon", "SpecialAction", "MoveSpeed", "SurfingOffsets", "SurfingInfo", "Animation" };
static const char* const g_ofs_alias[OF_N]  = { "lr", "ud", "keys", "pos", "rot", "health", "armour",
                                                "weapon", "special", "speed", "soff", "surf", "anim" };
static const int g_ofs_nval[OF_N]  = { 1, 1, 1, 3, 4, 1, 1, 1, 1, 3, 3, 1, 1 };            /* сколько чисел во вводе */
static const int g_ofs_size[OF_N]  = { 2, 2, 2, 12, 16, 1, 1, 1, 1, 12, 12, 2, 4 };       /* размер в пакете, байт */
static const int g_ofs_dumpOff[OF_N] = { 0x0, 0x2, 0x4, 0x8, 0x14, 0x24, 0x25, 0x26, 0x27, 0x28, 0x34, 0x40, 0x44 };   /* из дампа, для журнала */
#define OFS_STRUCT 0x48

static int g_ofs_off[OF_N];                                /* смещения внутри структуры (без заголовка объекта) */
static int g_ofs_layoutOk;

typedef struct { volatile int on; float v[4]; } OfsOv;
static OfsOv g_ofs_ov[OF_N];                               /* принудительные значения полей (/ofs set) */

#define OFS_MAXMAP 8
static volatile int g_ofs_map[OFS_MAXMAP][2];              /* замены анимаций: из → в */
static volatile int g_ofs_nmap;

static volatile int g_ofs_on;                              /* подмена включена */
static volatile int g_ofs_inv;                             /* режим невидимости */
static volatile int g_ofs_car = -1;                        /* ID ближней машины (-1 — нет) */
static volatile int g_ofs_carDistX10;                      /* расстояние до неё, дм */
static float g_ofs_radius = 50.6f;
static float g_ofs_invOff[3] = { 50.5f, 50.5f, -50.5f };   /* SurfingOffsets как в Lua */
static float g_ofs_invSpd[3] = { 0.45f, 0.45f, -0.25f };   /* MoveSpeed как в Lua */
static volatile int g_ofs_invHp = -1;                      /* -1 не трогать, -2 случайно 0..100 (как в Lua), 0..255 — число */
static volatile int g_ofs_invAnim = -1;                    /* -1 не трогать (в Lua там 1018 — это номер SAMP, у Hassle своя нумерация) */
static volatile unsigned g_ofs_hits, g_ofs_animHits, g_ofs_invHits;
static volatile unsigned g_ofs_hookCalls;                  /* вызовов хука с момента установки (до проверок структуры) */
static volatile int g_ofs_lastAnim = -1;                   /* Animation, как её собрал клиент (до подмены) */
static volatile int g_ofs_lastSent[4];                     /* что ушло: SpecialAction, SurfingInfo, Animation, Health */
static unsigned g_ofs_rng = 2463534242u;

static void ofs_defaults(void) {
    g_ofs_map[0][0] = 1215; g_ofs_map[0][1] = 1300; g_ofs_nmap = 1;
}

static unsigned ofs_rand(void) { g_ofs_rng ^= g_ofs_rng << 13; g_ofs_rng ^= g_ofs_rng >> 17; g_ofs_rng ^= g_ofs_rng << 5; return g_ofs_rng; }

/* записать число(а) в поле структуры по типу поля */
static void ofs_put(u8* d, int fi, const float* v) {
    u8* p = d + g_ofs_off[fi];
    switch (fi) {
        case OF_LR: case OF_UD: case OF_KEYS: case OF_SURF: *(u16*)p = (u16)(int)v[0]; break;
        case OF_HP: case OF_AP: case OF_WEAP: case OF_SPEC: *p = (u8)(int)v[0]; break;
        case OF_ANIM: *(int*)p = (int)v[0]; break;
        case OF_POS: case OF_SPD: case OF_SOFF: ((float*)p)[0] = v[0]; ((float*)p)[1] = v[1]; ((float*)p)[2] = v[2]; break;
        case OF_ROT: ((float*)p)[0] = v[0]; ((float*)p)[1] = v[1]; ((float*)p)[2] = v[2]; ((float*)p)[3] = v[3]; break;
        default: break;
    }
}

#define OFS_HOOK_LOG_FIRST 12                                    /* первые N вызовов хука пишутся подробно, дальше — каждый 2000-й */
static void ofs_tlog_struct(const char* tag, const u8* d) {
    char m[300]; Buf b; int i; static const char hx[] = "0123456789abcdef";
    b_init(&b, m, sizeof m); b_s(&b, tag);
    for (i = 0; i < OFS_STRUCT; i++) { if (i && (i % 4) == 0) b_ch(&b, ' '); b_ch(&b, hx[(d[i] >> 4) & 15]); b_ch(&b, hx[d[i] & 15]); }
    ofs_tlog(m);
}
static void ofs_hook_log(const char* what, unsigned n, void* packet, const u8* d) {
    char t[420]; Buf b;
    b_init(&b, t, sizeof t);
    b_s(&b, "hook #"); b_int(&b, (long)n); b_s(&b, " "); b_s(&b, what);
    b_s(&b, ": packet="); ofs_hex(&b, (uptr)packet, 12); b_s(&b, " data="); ofs_hex(&b, (uptr)d, 12);
    b_s(&b, " поток="); ofs_hex(&b, pthread_self(), 12);
    b_s(&b, " on="); b_int(&b, g_ofs_on); b_s(&b, " inv="); b_int(&b, g_ofs_inv); b_s(&b, " car="); b_int(&b, g_ofs_car);
    b_s(&b, " раскладка="); b_int(&b, g_ofs_layoutOk);
    ofs_tlog(t);
    if (d && g_ofs_layoutOk) ofs_tlog_struct("hook: байты структуры: ", d);
}

/* Зовётся из заглушки на входе WriteOnFootPacket (главный поток). x1 игры — указатель на OnFootSyncData. */
__attribute__((noinline, used))
static void ofs_hook(void* packet, void* data) {
    u8* d = (u8*)data; int i;
    unsigned n = __atomic_add_fetch(&g_ofs_hookCalls, 1, __ATOMIC_RELAXED);
    int logIt = (n <= OFS_HOOK_LOG_FIRST) || (n % 2000u) == 0;
    OFS_STAGE("хук: вход");
    if (logIt && d && n <= OFS_HOOK_LOG_FIRST && !hm_range_ok(d, OFS_STRUCT)) { ofs_hook_log("вход", n, packet, 0); ofs_tlog("hook: структура нечитаема — выхожу без правок"); return; }
    if (logIt) ofs_hook_log("вход", n, packet, d);
    if (!d || ((uptr)d & 3) || !g_ofs_layoutOk) { if (n <= OFS_HOOK_LOG_FIRST) ofs_tlog("hook: пропуск (нет данных / не выровнено / раскладка не прочитана)"); return; }
    __atomic_fetch_add(&g_ofs_hits, 1, __ATOMIC_RELAXED);
    g_ofs_lastAnim = *(int*)(d + g_ofs_off[OF_ANIM]);
    if (!__atomic_load_n(&g_ofs_on, __ATOMIC_ACQUIRE)) { if (logIt) ofs_tlog("hook: подмена выкл, пакет уходит без изменений"); OFS_STAGE("хук: выход (подмена выкл)"); return; }
    for (i = 0; i < OF_N; i++)                                              /* 1. принудительные поля */
        if (__atomic_load_n(&g_ofs_ov[i].on, __ATOMIC_ACQUIRE)) ofs_put(d, i, g_ofs_ov[i].v);
    {                                                                      /* 2. замены анимок */
        int n = __atomic_load_n(&g_ofs_nmap, __ATOMIC_ACQUIRE), cur = *(int*)(d + g_ofs_off[OF_ANIM]);
        for (i = 0; i < n; i++)
            if (g_ofs_map[i][0] == cur) { *(int*)(d + g_ofs_off[OF_ANIM]) = g_ofs_map[i][1]; __atomic_fetch_add(&g_ofs_animHits, 1, __ATOMIC_RELAXED); break; }
    }
    if (__atomic_load_n(&g_ofs_inv, __ATOMIC_ACQUIRE)) {                   /* 3. невидимость (логика INVIS.lua) */
        int car = __atomic_load_n(&g_ofs_car, __ATOMIC_ACQUIRE);
        if (car >= 0) {
            float one;
            one = 3.0f;               ofs_put(d, OF_SPEC, &one);           /* specialAction = 3 */
            one = (float)car;         ofs_put(d, OF_SURF, &one);           /* surfingVehicleId */
            ofs_put(d, OF_SOFF, g_ofs_invOff);                              /* surfingOffsets */
            ofs_put(d, OF_SPD, g_ofs_invSpd);                               /* moveSpeed */
            if (g_ofs_invHp == -2) { one = (float)(ofs_rand() % 101u); ofs_put(d, OF_HP, &one); }
            else if (g_ofs_invHp >= 0) { one = (float)g_ofs_invHp; ofs_put(d, OF_HP, &one); }
            if (g_ofs_invAnim >= 0) { one = (float)g_ofs_invAnim; ofs_put(d, OF_ANIM, &one); }
            __atomic_fetch_add(&g_ofs_invHits, 1, __ATOMIC_RELAXED);
        }
    }
    g_ofs_lastSent[0] = d[g_ofs_off[OF_SPEC]];
    g_ofs_lastSent[1] = *(u16*)(d + g_ofs_off[OF_SURF]);
    g_ofs_lastSent[2] = *(int*)(d + g_ofs_off[OF_ANIM]);
    g_ofs_lastSent[3] = d[g_ofs_off[OF_HP]];
    if (logIt) ofs_hook_log("выход (после правок)", n, packet, d);
    OFS_STAGE("хук: выход");
}

/* ───────────── раскладка структуры из метаданных ───────────── */
static int ofs_layout(void) {
    void* sc; int shift, i;
    if (g_ofs_layoutOk) return 1;
    if (!g_img) { ofs_log("ofs: игра ещё не готова"); return 0; }
    sc = A.class_from_name(g_img, "Hassle.Network", "OnFootSyncData");
    if (!sc || !A.class_is_valuetype(sc)) { ofs_log("ofs: нет структуры Hassle.Network.OnFootSyncData"); return 0; }
    shift = struct_shift(sc);
    for (i = 0; i < OF_N; i++) {
        void* f = find_field(sc, g_ofs_fname[i]); int off;
        if (!f) { ofs_log2("ofs: нет поля OnFootSyncData.", g_ofs_fname[i]); return 0; }
        off = A.field_get_offset(f) - shift;
        if (off < 0 || off + g_ofs_size[i] > OFS_STRUCT) { ofs_log2("ofs: смещение поля вне структуры: ", g_ofs_fname[i]); return 0; }
        g_ofs_off[i] = off;
        if (off != g_ofs_dumpOff[i]) { char m[120]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "ofs: смещение "); b_s(&b, g_ofs_fname[i]); b_s(&b, " = "); b_int(&b, off);
                                       b_s(&b, " (в дампе "); b_int(&b, g_ofs_dumpOff[i]); b_s(&b, ") — другая сборка игры?"); ofs_log(m); }
    }
    __atomic_store_n(&g_ofs_layoutOk, 1, __ATOMIC_RELEASE);
    ofs_log("ofs: раскладка OnFootSyncData прочитана");
    return 1;
}

/* ───────────── чистые помощники перехвата ───────────── */
static int ofs_pc_rel(u32 w) {                                  /* инструкция зависит от своего адреса? */
    if ((w & 0x1F000000u) == 0x10000000u) return 1;              /* ADR, ADRP */
    if ((w & 0x3B000000u) == 0x18000000u) return 1;              /* LDR (literal), PRFM */
    if ((w & 0x7C000000u) == 0x14000000u) return 1;              /* B, BL */
    if ((w & 0xFF000010u) == 0x54000000u) return 1;              /* B.cond */
    if ((w & 0x7E000000u) == 0x34000000u) return 1;              /* CBZ, CBNZ */
    if ((w & 0x7E000000u) == 0x36000000u) return 1;              /* TBZ, TBNZ */
    return 0;
}
static int ofs_is_cbz(u32 w) { return (w & 0x7F000000u) == 0x34000000u; }     /* CBZ (32/64) */
/* STP/LDP (64-бит) без «магических» чисел. mode: 0 — post-index, 1 — со смещением, 2 — pre-index; load: 0 — STP, 1 — LDP; off — байт, кратно 8. */
static u32 ofs_pair(int load, int mode, int rt, int rt2, int rn, int off) {
    u32 base = (mode == 0) ? 0xA8800000u : (mode == 1) ? 0xA9000000u : 0xA9800000u;
    return base | ((u32)load << 22) | (((u32)(off / 8) & 0x7Fu) << 15) | ((u32)rt2 << 10) | ((u32)rn << 5) | (u32)rt;
}
#define OFS_TRAMP_WORDS 20
/* Заглушка (arm64), 80 байт. Вход — абсолютным прыжком из начала функции; x0 — packet, x1 — &OnFootSyncData, x2 — MethodInfo.
     0 stp x0,x1,[sp,#-32]!     1 stp x2,x30,[sp,#16]     2 ldr x16,=ofs_hook (слово 14)   3 blr x16
     4 ldp x2,x30,[sp,#16]      5 ldp x0,x1,[sp],#32
     6,7  исходные инструкции 0 и 1 пролога
     8    CBNZ (инверсия исходного CBZ) → слово 11     9 ldr x16,=цель CBZ (слово 16)     10 br x16
     11   исходная инструкция 3 пролога
     12   ldr x16,=функция+16 (слово 18)     13 br x16
     14-15 ofs_hook   16-17 цель CBZ   18-19 функция+16 */
static int ofs_ldr_lit(int cur, int lit) { return (int)(0x58000000u | ((unsigned)(lit - cur) << 5) | 16u); }
static void ofs_make_tramp(u32* t, const u32* o4, uptr hook, uptr target, uptr resume) {
    t[0] = ofs_pair(0, 2, 0, 1, 31, -32); t[1] = ofs_pair(0, 1, 2, 30, 31, 16);          /* stp x0,x1,[sp,#-32]! ; stp x2,x30,[sp,#16] (x30 = LR; раньше по ошибке был x29 — отсюда вылет) */
    t[2] = (u32)ofs_ldr_lit(2, 14); t[3] = 0xD63F0200u;
    t[4] = ofs_pair(1, 1, 2, 30, 31, 16); t[5] = ofs_pair(1, 0, 0, 1, 31, 32);          /* ldp x2,x30,[sp,#16] ; ldp x0,x1,[sp],#32 */
    t[6] = o4[0]; t[7] = o4[1];
    t[8] = (o4[2] ^ 0x01000000u);                                /* CBZ ↔ CBNZ (бит 24) */
    t[8] = (t[8] & ~(0x7FFFFu << 5)) | (3u << 5);                /* перепрыгнуть 2 слова: на слово 11 */
    t[9] = (u32)ofs_ldr_lit(9, 16); t[10] = 0xD61F0200u;
    t[11] = o4[3];
    t[12] = (u32)ofs_ldr_lit(12, 18); t[13] = 0xD61F0200u;
    t[14] = (u32)(hook & 0xFFFFFFFFu);   t[15] = (u32)(((unsigned long long)hook) >> 32);
    t[16] = (u32)(target & 0xFFFFFFFFu); t[17] = (u32)(((unsigned long long)target) >> 32);
    t[18] = (u32)(resume & 0xFFFFFFFFu); t[19] = (u32)(((unsigned long long)resume) >> 32);
}
/* Независимая проверка заглушки перед установкой: LR (x30) обязан сохраняться и восстанавливаться, стек — сходиться. */
#define OFS_PAIR_IS(w, rt, rt2, rn) ((((w) & 31u) == (u32)(rt)) && ((((w) >> 10) & 31u) == (u32)(rt2)) && ((((w) >> 5) & 31u) == (u32)(rn)))
static int ofs_tramp_check(const u32* v) {
    return OFS_PAIR_IS(v[0], 0, 1, 31) && ((v[0] >> 22) & 1u) == 0 && OFS_PAIR_IS(v[1], 2, 30, 31) && ((v[1] >> 22) & 1u) == 0 &&
           OFS_PAIR_IS(v[4], 2, 30, 31) && ((v[4] >> 22) & 1u) == 1 && OFS_PAIR_IS(v[5], 0, 1, 31) && ((v[5] >> 22) & 1u) == 1 &&
           v[3] == 0xD63F0200u && v[10] == 0xD61F0200u && v[13] == 0xD61F0200u &&
           (v[2] & 0xFF00001Fu) == 0x58000010u && (v[9] & 0xFF00001Fu) == 0x58000010u && (v[12] & 0xFF00001Fu) == 0x58000010u;
}
static void ofs_make_entry(u32* e, uptr tramp) {               /* ldr x16,[pc,#8]; br x16; .quad адрес */
    e[0] = 0x58000050u; e[1] = 0xD61F0200u;
    e[2] = (u32)(tramp & 0xFFFFFFFFu); e[3] = (u32)(((unsigned long long)tramp) >> 32);
}
static uptr ofs_cbz_target(uptr at, u32 w) {                   /* абсолютная цель CBZ/CBNZ по адресу инструкции */
    long imm = (long)((w >> 5) & 0x7FFFFu);
    if (imm & 0x40000L) imm -= 0x80000L;
    return at + (uptr)(imm * 4);
}

/* ───────────── логирование установки/хука: при вылете последняя строка HLOG/konstt.log должна указать на точный шаг ───────────── */
static void b_hex32(Buf* b, u32 v) {
    static const char* hx = "0123456789abcdef";
    int i;
    b_s(b, "0x");
    for (i = 28; i >= 0; i -= 4) b_ch(b, hx[(v >> i) & 0xF]);
}
static void ofs_dlog_words(const char* tag, const u32* w, int n) {
    char m[300]; Buf b; int i;
    b_init(&b, m, sizeof m);
    b_s(&b, tag);
    for (i = 0; i < n; i++) { b_ch(&b, ' '); b_hex32(&b, w[i]); }
    ofs_log(m);
}
static void ofs_dlog_u2(const char* a, long v, const char* b2, long v2) {
    char m[200]; Buf b; b_init(&b, m, sizeof m);
    b_s(&b, a); b_int(&b, v); b_s(&b, b2); b_int(&b, v2); ofs_log(m);
}
/* первые 0x48 байт структуры как hex — печатается не на каждый пакет (журнал на диск пишется синхронно, дорого), а только первые несколько раз */
static void ofs_dlog_struct(const char* tag, const u8* d) {
    char m[300]; Buf b; int i;
    b_init(&b, m, sizeof m);
    b_s(&b, tag);
    for (i = 0; i < OFS_STRUCT; i++) {
        static const char* hx = "0123456789abcdef";
        if (i && (i % 4) == 0) b_ch(&b, ' ');
        b_ch(&b, hx[(d[i] >> 4) & 0xF]); b_ch(&b, hx[d[i] & 0xF]);
    }
    ofs_log(m);
}
/* ожидаемые слова пролога WriteOnFootPacket по дампу/дизассемблированию от 20.10.2026:
   STR x20,[sp,#-32]! ; STP x19,x30,[sp,#16] ; CBZ x0,… ; MOV x20,x1 — если сборка игры другая, эти 4 слова не совпадут */
static const u32 OFS_EXPECT_PROLOGUE[4] = { 0xf81e0ff4u, 0xa9017bf3u, 0xb4000b80u, 0xaa0103f4u };

static int g_ofs_state;                                        /* 1 — перехват стоит */
static char g_ofs_err[160];
static int ofs_fail(const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof g_ofs_err - 1; i++) g_ofs_err[i] = why[i];
    g_ofs_err[i] = 0;
    ofs_log2("ofs: перехват не поставлен: ", why);
    return 0;
}

#ifdef OFS_ARM64
static void* g_ofs_veneer;                                     /* страница с заглушкой */
static uptr g_ofs_site;                                        /* адрес начала WriteOnFootPacket */
static u32 g_ofs_orig[4];                                      /* его исходные 4 слова */

/* ───────────── отчёт о вылете в ofs_log.txt ─────────────
   Ставится один раз при первой установке перехвата на SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGFPE. Пишет: сигнал, адрес ошибки, PC/LR/SP, x0/x1/x29,
   попал ли PC в заглушку или в начало WriteOnFootPacket, последний этап и последнюю строку журнала. Потом возвращает прежний обработчик
   (Unity/система) и выходит — упавшая инструкция выполнится ещё раз и вылет пойдёт обычным путём. */
extern int sigaction(int, const void*, void*);
typedef struct { int flags; void (*fn)(int, void*, void*); unsigned long mask; void* restorer; } OfsSigAct;
static OfsSigAct g_ofs_oldact[5];
static const int g_ofs_sigs[5] = { 11, 7, 4, 6, 8 };            /* SEGV, BUS, ILL, ABRT, FPE */
static int g_ofs_crashInstalled;
static void ofs_crash_handler(int sig, void* info, void* uc) {
    char t[700]; Buf b; int i;
    unsigned long* mc = (unsigned long*)((char*)uc + 176);      /* arm64: ucontext.uc_mcontext: fault_address, x0..x30, sp, pc */
    unsigned long fa = mc[0], pc = mc[33], lr = mc[31], sp = mc[32];
    b_init(&b, t, sizeof t);
    b_s(&b, "!!! ВЫЛЕТ: сигнал "); b_int(&b, sig); b_s(&b, " code="); b_int(&b, info ? ((int*)info)[2] : -1);
    b_s(&b, " адрес ошибки="); ofs_hex(&b, fa, 12); b_s(&b, " поток="); ofs_hex(&b, pthread_self(), 12);
    ofs_tlog(t);
    b_init(&b, t, sizeof t);
    b_s(&b, "!!! pc="); ofs_hex(&b, pc, 12); b_s(&b, " lr="); ofs_hex(&b, lr, 12); b_s(&b, " sp="); ofs_hex(&b, sp, 12);
    b_s(&b, " x0="); ofs_hex(&b, mc[1], 12); b_s(&b, " x1="); ofs_hex(&b, mc[2], 12); b_s(&b, " x29="); ofs_hex(&b, mc[30], 12);
    ofs_tlog(t);
    b_init(&b, t, sizeof t);
    b_s(&b, "!!! pc ");
    if (g_ofs_veneer && pc >= (uptr)g_ofs_veneer && pc < (uptr)g_ofs_veneer + 4096) { b_s(&b, "ВНУТРИ ЗАГЛУШКИ, слово "); b_int(&b, (long)((pc - (uptr)g_ofs_veneer) / 4)); }
    else if (g_ofs_site && pc >= g_ofs_site && pc < g_ofs_site + 64) { b_s(&b, "в начале WriteOnFootPacket, +"); b_int(&b, (long)(pc - g_ofs_site)); }
    else b_s(&b, "вне заглушки и вне WriteOnFootPacket");
    b_s(&b, "; lr ");
    if (g_ofs_veneer && lr >= (uptr)g_ofs_veneer && lr < (uptr)g_ofs_veneer + 4096) b_s(&b, "ВНУТРИ ЗАГЛУШКИ (LR испорчен?)"); else b_s(&b, "не в заглушке");
    b_s(&b, "; WriteOnFootPacket=" ); ofs_hex(&b, g_ofs_site, 12); b_s(&b, " заглушка="); ofs_hex(&b, (uptr)g_ofs_veneer, 12);
    ofs_tlog(t);
    b_init(&b, t, sizeof t);
    b_s(&b, "!!! этап: "); b_s(&b, g_ofs_stage); b_s(&b, " | on="); b_int(&b, g_ofs_on); b_s(&b, " inv="); b_int(&b, g_ofs_inv); b_s(&b, " car="); b_int(&b, g_ofs_car);
    b_s(&b, " вызовов хука="); b_int(&b, (long)g_ofs_hookCalls); b_s(&b, " перехват="); b_int(&b, g_ofs_state);
    ofs_tlog(t);
    b_init(&b, t, sizeof t); b_s(&b, "!!! последняя строка журнала: "); b_s(&b, g_ofs_lastline); ofs_tlog(t);
    for (i = 0; i < 5; i++) if (g_ofs_sigs[i] == sig) { sigaction(sig, &g_ofs_oldact[i], 0); break; }
}
static void ofs_crash_install(void) {
    int i, ok = 0; OfsSigAct a;
    if (g_ofs_crashInstalled) return;
    g_ofs_crashInstalled = 1;
    for (i = 0; i < 5; i++) {
        a.flags = 4; a.fn = ofs_crash_handler; a.mask = 0; a.restorer = 0;           /* SA_SIGINFO */
        if (sigaction(g_ofs_sigs[i], &a, &g_ofs_oldact[i]) == 0) ok++;
    }
    ofs_logi("ofs: обработчик вылета поставлен на сигналов: ", ok);
}

static void ofs_flush(void* p, unsigned n) {
    uptr ctr, dl, il, a, e;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    dl = 4u << ((ctr >> 16) & 15u); il = 4u << (ctr & 15u);
    e = (uptr)p + n;
    for (a = (uptr)p & ~(dl - 1); a < e; a += dl) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uptr)p & ~(il - 1); a < e; a += il) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
static int ofs_write_code(uptr addr, const u32* w, int n) {     /* слова пишутся с конца к началу: последней ложится первая инструкция */
    uptr p0 = addr & ~(uptr)4095, p1 = (addr + (uptr)n * 4 - 1) & ~(uptr)4095;
    unsigned long len = (unsigned long)(p1 - p0) + 4096;
    int i;
    if (mprotect((void*)p0, len, 7) == 0) {
        for (i = n - 1; i >= 0; i--) ((volatile u32*)addr)[i] = w[i];
        ofs_flush((void*)addr, (unsigned)n * 4);
        mprotect((void*)p0, len, 5);
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, w, (unsigned long)n * 4, (long)addr); close(fd); }
        if (r == (long)n * 4) { ofs_flush((void*)addr, (unsigned)n * 4); return 1; }
    }
    return 0;
}
static int ofs_install(void) {
    void *kls, *m; uptr fn; u32 code[8]; int i;
    ofs_log("ofs: install: начало");
    OFS_STAGE("install: начало");
    ofs_crash_install();
    if (g_ofs_state == 1) { ofs_log("ofs: install: уже стоит"); return 1; }
    if (!ofs_layout()) return ofs_fail("раскладка OnFootSyncData не прочиталась");
    ofs_log("ofs: install: раскладка ок, ищу Hassle.Network.SyncData");
    kls = A.class_from_name(g_img, "Hassle.Network", "SyncData");
    if (!kls) return ofs_fail("нет класса Hassle.Network.SyncData");
    ofs_log("ofs: install: класс найден, ищу метод WriteOnFootPacket(2)");
    m = A.class_get_method_from_name(kls, "WriteOnFootPacket", 2);
    if (!m || !hm_readable(m, sizeof(void*))) return ofs_fail("нет метода SyncData.WriteOnFootPacket");
    fn = *(uptr*)m;
    ofs_dlog_u2("ofs: install: MethodInfo=", (long)(uptr)m, " адрес функции=", (long)fn);
    if (!fn || (fn & 3) || !hm_range_ok((void*)fn, 64)) return ofs_fail("адрес WriteOnFootPacket нечитаем");
    for (i = 0; i < 8; i++) code[i] = ((const u32*)fn)[i];
    ofs_dlog_words("ofs: install: сырые слова функции (из памяти сейчас):", code, 8);
    if (g_ofs_veneer && g_ofs_site == fn) {
        for (i = 0; i < 4; i++) code[i] = g_ofs_orig[i];   /* уже пропатчено нами — исходные из сохранённых */
        ofs_log("ofs: install: функция уже была пропатчена нами ранее, беру исходные 4 слова из сохранённых");
    }
    ofs_dlog_words("ofs: install: 4 слова, которые будут перенесены в заглушку:", code, 4);
    if (code[0] != OFS_EXPECT_PROLOGUE[0] || code[1] != OFS_EXPECT_PROLOGUE[1] ||
        code[2] != OFS_EXPECT_PROLOGUE[2] || code[3] != OFS_EXPECT_PROLOGUE[3]) {
        ofs_dlog_words("ofs: install: ВНИМАНИЕ, не совпадает с ожидаемым по дампу 20.10.2026:", OFS_EXPECT_PROLOGUE, 4);
        ofs_log("ofs: install: это не обязательно ошибка (другая сборка игры может иметь другой пролог), но тогда проверки ниже решают, ставить перехват или нет");
    }
    if (!ofs_is_cbz(code[2])) return ofs_fail("третья инструкция пролога не CBZ — другая сборка игры?");
    if (ofs_pc_rel(code[0]) || ofs_pc_rel(code[1]) || ofs_pc_rel(code[3])) return ofs_fail("пролог зависит от адреса — перенести нельзя");
    ofs_log("ofs: install: проверки пролога пройдены (CBZ на месте, 0/1/3 не зависят от адреса)");
    if (!(g_ofs_veneer && g_ofs_site == fn)) {
        u32 v[OFS_TRAMP_WORDS];
        uptr cbzTarget = ofs_cbz_target(fn + 8, code[2]);
        void* page;
        ofs_dlog_u2("ofs: install: цель CBZ=", (long)cbzTarget, ", resume(fn+16)=", (long)(fn + 16));
        page = mmap(NULL, 4096, 3, 0x22, -1, 0);
        if (page == (void*)-1 || !page) return ofs_fail("не удалось выделить страницу под заглушку");
        ofs_dlog_u2("ofs: install: страница заглушки выделена по адресу ", (long)(uptr)page, ", размер=", 4096);
        OFS_STAGE("install: сборка заглушки");
        ofs_make_tramp(v, code, (uptr)ofs_hook, cbzTarget, fn + 16);
        if (!ofs_tramp_check(v)) { munmap(page, 4096); return ofs_fail("самопроверка заглушки не прошла (LR/стек/прыжки)"); }
        ofs_log("ofs: install: самопроверка заглушки пройдена (LR x30 сохраняется и восстанавливается)");
        ofs_dlog_words("ofs: install: заглушка, слова 0-7:", v, 8);
        ofs_dlog_words("ofs: install: заглушка, слова 8-19:", v + 8, OFS_TRAMP_WORDS - 8);
        for (i = 0; i < OFS_TRAMP_WORDS; i++) ((u32*)page)[i] = v[i];
        ofs_flush(page, sizeof v);
        ofs_log("ofs: install: заглушка записана и промыта из кэшей, делаю страницу R-X");
        if (mprotect(page, 4096, 5) != 0) { munmap(page, 4096); return ofs_fail("не удалось сделать страницу заглушки исполняемой"); }
        g_ofs_veneer = page; g_ofs_site = fn;
        for (i = 0; i < 4; i++) g_ofs_orig[i] = code[i];
        ofs_log("ofs: install: заглушка готова и исполняема");
    } else {
        ofs_log("ofs: install: заглушка уже стояла, переиспользую");
    }
    {
        u32 e[4];
        ofs_make_entry(e, (uptr)g_ofs_veneer);
        ofs_dlog_words("ofs: install: вход (4 слова, пишутся в начало функции):", e, 4);
        OFS_STAGE("install: запись входа в код игры");
        ofs_log("ofs: install: пишу вход в код игры...");
        if (!ofs_write_code(fn, e, 4)) return ofs_fail("запись в код игры запрещена системой");
        ofs_log("ofs: install: вход записан успешно");
    }
    g_ofs_state = 1;
    OFS_STAGE("install: готово, ждём вызовы хука");
    __atomic_store_n(&g_ofs_hookCalls, 0, __ATOMIC_RELEASE);
    ofs_log("ofs: перехват SyncData.WriteOnFootPacket установлен");
    return 1;
}
static void ofs_unhook(void) {
    if (g_ofs_state != 1) { ofs_log("ofs: unhook: перехват и так не стоит"); return; }
    ofs_log("ofs: unhook: возвращаю исходные 4 слова...");
    if (ofs_write_code(g_ofs_site, g_ofs_orig, 4)) { g_ofs_state = 0; ofs_log("ofs: перехват снят, код игры восстановлен"); }
    else ofs_log("ofs: не удалось вернуть исходные инструкции");
}
#else
static int ofs_install(void) { return ofs_fail("поддерживается только arm64"); }
static void ofs_unhook(void) {}
#endif

/* ───────────── поиск ближайшей машины (главный поток) ───────────── */
static int ofs_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
static void* ofs_pool(const char* field) {                       /* Pools.<field> */
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}
static void* ofs_obj0(void* obj, const char* name) {              /* метод без аргументов → ссылочный объект */
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int ofs_vec0(void* obj, const char* name, float* out) {    /* метод без аргументов → Vector3 (в боксе) */
    void *m, *exc = NULL, *r; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!ofs_finite(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
static int ofs_ent_pos(void* ent, float* out) {
    void* tr = ofs_obj0(ent, "get_Transform");
    if (!tr) tr = ofs_obj0(ent, "get_transform");
    return tr && ofs_vec0(tr, "get_position", out);
}
static int ofs_dict_keys(void* dict, int* out, int max) {         /* ключи (int) Dictionary<int,…> */
    void *arr, *ac, *ec, *fk, *fh; int esz, i, shift, cnt, offk, offh = 0, n = 0; char* data; long len;
    arr = ref_field_s(dict, "_entries");
    if (!arr || !hm_obj_ok(arr)) return 0;
    ac = A.object_get_class(arr); ec = A.class_get_element_class(ac); esz = arr_elem_size(ec);
    if (!ec || esz <= 0) return 0;
    fk = find_field(ec, "key"); fh = find_field(ec, "hashCode");
    if (!fk) return 0;
    shift = A.class_is_valuetype(ec) ? struct_shift(ec) : 0;
    offk = A.field_get_offset(fk) - shift;
    if (fh) offh = A.field_get_offset(fh) - shift;
    cnt = int_field(dict, "_count", "count", NULL, -1);
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 100000) return 0;
    if (cnt < 0 || cnt > len) cnt = (int)len;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < cnt && n < max; i++) {
        char* e = data + (uptr)i * (uptr)esz;
        if (!hm_range_ok(e, esz)) break;
        if (fh && *(int*)(e + offh) < 0) continue;
        out[n++] = *(int*)(e + offk);
    }
    return n;
}
/* Возврат: ID ближайшей машины в радиусе или -1; *dist — расстояние, -2 в *dist — ошибка чтения (машину не нашли из-за сбоя, а не из-за её отсутствия) */
static int ofs_find_car(float* dist) {
    void *me, *pool, *dict, *get; float mp[3]; int keys[256], n, i, best = -1; float bd = 1e9f;
    *dist = -2.0f;
    me = local_player();
    if (!me || !hm_obj_ok(me)) return -1;
    if (!ofs_ent_pos(me, mp) && !read_pos(me, mp)) return -1;
    pool = ofs_pool("Vehicles");
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    get = pool ? find_method(A.object_get_class(pool), "Get", 1) : NULL;
    if (!dict || !get) return -1;
    n = ofs_dict_keys(dict, keys, 256);
    *dist = -1.0f;
    for (i = 0; i < n; i++) {
        void *args[1], *exc = NULL, *r; int id = keys[i]; float p[3], dx, dy, dz, d;
        if (id < 0 || id > 0xFFFE) continue;
        args[0] = &id;
        r = A.runtime_invoke(get, pool, args, &exc);
        if (exc || !r || !hm_obj_ok(r) || !ofs_ent_pos(r, p)) continue;
        dx = p[0] - mp[0]; dy = p[1] - mp[1]; dz = p[2] - mp[2];
        d = (float)__builtin_sqrt((double)(dx * dx + dy * dy + dz * dz));
        if (d <= g_ofs_radius && d < bd) { bd = d; best = id; }
    }
    if (best >= 0) *dist = bd;
    return best;
}

/* ───────────── разбор ввода ───────────── */
#define OFS_MAXTOK 12
typedef struct { char t[OFS_MAXTOK][24]; int n; } OfsTok;
/* Игра может положить в массив и само имя команды («ofs», «/ofs», «/ofs:»…) — выкидываем все ведущие слова, которые начинаются с «/» или равны «ofs»/«invis» */
static void ofs_strip_cmd(OfsTok* tk) {
    while (tk->n > 0 && (tk->t[0][0] == '/' || seq(tk->t[0], "ofs") || seq(tk->t[0], "invis"))) {
        int k, j;
        for (k = 1; k < tk->n; k++) for (j = 0; j < 24; j++) tk->t[k - 1][j] = tk->t[k][j];
        tk->n--;
    }
}
static void ofs_tokens(void* arr, OfsTok* tk) {                    /* все слова всех элементов string[], нижний регистр */
    long len; int i; char* data;
    tk->n = 0;
    if (!arr) return;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[96]; int n, p = 0; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 80 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        while (tmp[p]) {
            int q = 0;
            while (tmp[p] == ' ') p++;
            if (!tmp[p]) break;
            if (tk->n >= OFS_MAXTOK) { ofs_strip_cmd(tk); return; }
            while (tmp[p] && tmp[p] != ' ') { if (q < 23) { char c = tmp[p]; tk->t[tk->n][q++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } p++; }
            tk->t[tk->n][q] = 0; tk->n++;
        }
    }
    ofs_strip_cmd(tk);
}
static int ofs_num(const char* s, float* out) {                    /* «-12», «0.45», «0,45» */
    float v = 0.0f, f = 0.1f; int neg = 0, dot = 0, digits = 0, k = 0;
    if (s[0] == '-') { neg = 1; k = 1; } else if (s[0] == '+') k = 1;
    for (; s[k]; k++) {
        if (s[k] >= '0' && s[k] <= '9') {
            digits++;
            if (!dot) v = v * 10.0f + (float)(s[k] - '0');
            else { v += f * (float)(s[k] - '0'); f *= 0.1f; }
        } else if ((s[k] == '.' || s[k] == ',') && !dot) dot = 1;
        else return 0;
    }
    if (!digits) return 0;
    *out = neg ? -v : v; return 1;
}
static int ofs_field_by_name(const char* s) {
    int i;
    for (i = 0; i < OF_N; i++) if (seq(s, g_ofs_alias[i])) return i;
    for (i = 0; i < OF_N; i++) { char lo[24]; int k = 0; const char* f = g_ofs_fname[i]; for (; f[k] && k < 23; k++) lo[k] = (f[k] >= 'A' && f[k] <= 'Z') ? (char)(f[k] + 32) : f[k]; lo[k] = 0; if (seq(s, lo)) return i; }
    return -1;
}

/* ───────────── команды ───────────── */
static void ofs_say_status(void) {
    Buf b; char msg[480]; int i, n = 0;
    b_init(&b, msg, sizeof msg);
    b_s(&b, "ofs: перехват "); b_s(&b, g_ofs_state == 1 ? "стоит" : "не стоит"); b_s(&b, ", подмена "); b_s(&b, g_ofs_on ? "ВКЛ" : "выкл");
    b_s(&b, ", вызовов "); b_int(&b, (long)g_ofs_hits);
    if (g_ofs_state != 1 && g_ofs_err[0]) { b_s(&b, ". Причина: "); b_s(&b, g_ofs_err); }
    chat_say(msg);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "ofs: анимка клиента сейчас "); b_int(&b, g_ofs_lastAnim);
    b_s(&b, ", ушла "); b_int(&b, g_ofs_lastSent[2]); b_s(&b, " (замен "); b_int(&b, (long)g_ofs_animHits); b_s(&b, "); таблица:");
    n = g_ofs_nmap;
    if (!n) b_s(&b, " пуста");
    for (i = 0; i < n; i++) { b_ch(&b, ' '); b_int(&b, g_ofs_map[i][0]); b_s(&b, "→"); b_int(&b, g_ofs_map[i][1]); }
    chat_say(msg);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "ofs: inv "); b_s(&b, g_ofs_inv ? "ВКЛ" : "выкл"); b_s(&b, ", машина ");
    if (g_ofs_car >= 0) { b_int(&b, g_ofs_car); b_s(&b, " (~"); b_int(&b, g_ofs_carDistX10 / 10); b_s(&b, " м)"); } else b_s(&b, "не найдена");
    b_s(&b, ", радиус "); b_fix(&b, g_ofs_radius, 1); b_s(&b, ", подмен inv "); b_int(&b, (long)g_ofs_invHits);
    b_s(&b, "; ушло: special "); b_int(&b, g_ofs_lastSent[0]); b_s(&b, ", surf "); b_int(&b, g_ofs_lastSent[1]); b_s(&b, ", hp "); b_int(&b, g_ofs_lastSent[3]);
    chat_say(msg);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "ofs: принудительные поля:");
    n = 0;
    for (i = 0; i < OF_N; i++) if (g_ofs_ov[i].on) {
        int k; n++; b_ch(&b, ' '); b_s(&b, g_ofs_alias[i]); b_ch(&b, '=');
        for (k = 0; k < g_ofs_nval[i]; k++) { if (k) b_ch(&b, ','); b_fix(&b, g_ofs_ov[i].v[k], g_ofs_nval[i] == 1 ? 0 : 2); }
    }
    if (!n) b_s(&b, " нет");
    chat_say(msg);
}

static void ofs_scan_apply(int announce) {                        /* главный поток: найти машину, обновить g_ofs_car */
    float d; int id = ofs_find_car(&d);
    if (d == -2.0f) return;                                       /* сбой чтения: ничего не меняем, попробуем в следующий раз */
    g_ofs_carDistX10 = (int)(d * 10.0f);
    __atomic_store_n(&g_ofs_car, id, __ATOMIC_RELEASE);
    if (id < 0 && g_ofs_inv) {
        __atomic_store_n(&g_ofs_inv, 0, __ATOMIC_RELEASE);
        chat_say("inv: машины рядом нет — невидимость выключена (CAR NOT FOUND)");
    } else if (announce && id >= 0) {
        Buf b; char m[120]; b_init(&b, m, sizeof m); b_s(&b, "inv: машина "); b_int(&b, id); b_s(&b, ", ~"); b_fix(&b, d, 1); b_s(&b, " м"); chat_say(m);
    }
}

static int ofs_arm(void) {                                        /* поставить перехват и включить подмену; 0 — не вышло (сообщение уже в чат) */
    if (!ofs_install()) {
        Buf b; char m[260]; b_init(&b, m, sizeof m); b_s(&b, "ofs: перехват не поставлен — "); b_s(&b, g_ofs_err); chat_say(m);
        return 0;
    }
    __atomic_store_n(&g_ofs_on, 1, __ATOMIC_RELEASE);
    return 1;
}
static void ofs_set_inv(int want) {
    if (want) {
        if (!ofs_arm()) return;
        mt_capture_ctx();
        __atomic_store_n(&g_ofs_car, -1, __ATOMIC_RELEASE);
        {
            float d; int id = ofs_find_car(&d);
            if (id < 0) { chat_say(d == -2.0f ? "inv: не удалось прочитать машины/позицию — см. konstt.log" : "inv: машины рядом нет (CAR NOT FOUND) — выйдите ближе к любой машине"); return; }
            g_ofs_carDistX10 = (int)(d * 10.0f);
            __atomic_store_n(&g_ofs_car, id, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&g_ofs_inv, 1, __ATOMIC_RELEASE);
        { Buf b; char m[140]; b_init(&b, m, sizeof m); b_s(&b, "inv: ВКЛ, машина "); b_int(&b, g_ofs_car); b_s(&b, " (~"); b_int(&b, g_ofs_carDistX10 / 10); b_s(&b, " м)"); chat_say(m); }
    } else {
        __atomic_store_n(&g_ofs_inv, 0, __ATOMIC_RELEASE);
        chat_say("inv: выкл");
    }
}

static volatile int g_ofs_pending; static volatile long g_ofs_postT;
static void native_ofs_cmd(void* self, void* args, void* mi) {
    OfsTok tk; int i;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { g_ofs_pending = 0; if (g_ofs_inv) ofs_scan_apply(0); return; }   /* вызов из Post (state — строка) */
    ofs_tokens(args, &tk);
    ofs_logi("ofs: команда /ofs, слов: ", tk.n);
    if (tk.n > 0) ofs_log2("ofs: команда /ofs, первое слово: ", tk.t[0]);
    OFS_STAGE("команда /ofs");
    if (tk.n > 0 && seq(tk.t[0], "log")) {
        Buf b; char m[420]; ofs_tlog("ofs: /ofs log — запрошен путь");
        b_init(&b, m, sizeof m); b_s(&b, "ofs: журнал: "); b_s(&b, g_ofs_tpath[0] ? g_ofs_tpath : "(путь ещё не создан)");
        chat_say(m);
        return;
    }
    if (tk.n == 0 || seq(tk.t[0], "status") || seq(tk.t[0], "diag")) { ofs_say_status(); return; }
    if (seq(tk.t[0], "on") || seq(tk.t[0], "1")) {
        if (ofs_arm()) chat_say("ofs: подмена полей onfoot ВКЛ (анимки по таблице; /ofs status — подробности)");
        return;
    }
    if (seq(tk.t[0], "off") || seq(tk.t[0], "0")) { __atomic_store_n(&g_ofs_on, 0, __ATOMIC_RELEASE); chat_say("ofs: подмена выкл (перехват остаётся, пакеты уходят как есть)"); return; }
    if (seq(tk.t[0], "unhook")) {
        __atomic_store_n(&g_ofs_on, 0, __ATOMIC_RELEASE); __atomic_store_n(&g_ofs_inv, 0, __ATOMIC_RELEASE);
        ofs_unhook(); chat_say(g_ofs_state == 1 ? "ofs: не удалось снять перехват, см. konstt.log" : "ofs: перехват снят, код игры в исходном виде");
        return;
    }
    if (seq(tk.t[0], "fields")) {
        Buf b; char m[420]; b_init(&b, m, sizeof m); b_s(&b, "ofs: поля (для /ofs set):");
        for (i = 0; i < OF_N; i++) { b_ch(&b, ' '); b_s(&b, g_ofs_alias[i]); if (g_ofs_nval[i] > 1) { b_ch(&b, '['); b_int(&b, g_ofs_nval[i]); b_ch(&b, ']'); } }
        chat_say(m);
        return;
    }
    if (seq(tk.t[0], "anim")) {
        float a, c;
        if (tk.n == 1) { ofs_say_status(); return; }
        if (seq(tk.t[1], "off") || seq(tk.t[1], "clear")) { __atomic_store_n(&g_ofs_nmap, 0, __ATOMIC_RELEASE); chat_say("ofs: таблица замен анимок очищена"); return; }
        if (seq(tk.t[1], "reset")) { __atomic_store_n(&g_ofs_nmap, 0, __ATOMIC_RELEASE); ofs_defaults(); chat_say("ofs: замены анимок: 1215 → 1300"); return; }
        if (tk.n >= 3 && ofs_num(tk.t[1], &a) && ofs_num(tk.t[2], &c) && a >= 0 && c >= 0) {
            int n = g_ofs_nmap, k;
            for (k = 0; k < n; k++) if (g_ofs_map[k][0] == (int)a) break;
            if (k == n) { if (n >= OFS_MAXMAP) { chat_say("ofs: таблица замен полна (8). Очистите: /ofs anim off"); return; } }
            g_ofs_map[k][0] = (int)a; g_ofs_map[k][1] = (int)c;
            if (k == n) __atomic_store_n(&g_ofs_nmap, n + 1, __ATOMIC_RELEASE);
            { Buf b; char m[100]; b_init(&b, m, sizeof m); b_s(&b, "ofs: слать вместо анимки "); b_int(&b, (int)a); b_s(&b, " анимку "); b_int(&b, (int)c);
              b_s(&b, g_ofs_on ? "" : " (подмена выключена: /ofs on)"); chat_say(m); }
            return;
        }
        chat_usage("Используйте: /ofs anim <из> <в> | /ofs anim off | /ofs anim reset");
        return;
    }
    if (seq(tk.t[0], "set")) {
        int fi, k; float v[4] = { 0, 0, 0, 0 };
        if (tk.n < 3 || (fi = ofs_field_by_name(tk.t[1])) < 0) { chat_usage("Используйте: /ofs set <поле> <значение…>  (поля: /ofs fields)"); return; }
        if (tk.n - 2 < g_ofs_nval[fi]) { chat_usage("ofs: для этого поля нужно больше чисел (/ofs fields)"); return; }
        for (k = 0; k < g_ofs_nval[fi]; k++) if (!ofs_num(tk.t[2 + k], &v[k])) { chat_usage("ofs: значение должно быть числом"); return; }
        for (k = 0; k < 4; k++) g_ofs_ov[fi].v[k] = v[k];
        __atomic_store_n(&g_ofs_ov[fi].on, 1, __ATOMIC_RELEASE);
        if (ofs_arm()) { Buf b; char m[100]; b_init(&b, m, sizeof m); b_s(&b, "ofs: поле "); b_s(&b, g_ofs_alias[fi]); b_s(&b, " теперь принудительное"); chat_say(m); }
        return;
    }
    if (seq(tk.t[0], "unset")) {
        if (tk.n >= 2 && seq(tk.t[1], "all")) { for (i = 0; i < OF_N; i++) g_ofs_ov[i].on = 0; chat_say("ofs: принудительные поля сняты"); return; }
        if (tk.n >= 2 && (i = ofs_field_by_name(tk.t[1])) >= 0) { g_ofs_ov[i].on = 0; chat_say("ofs: поле снято"); return; }
        chat_usage("Используйте: /ofs unset <поле>|all");
        return;
    }
    if (seq(tk.t[0], "inv")) {
        float v;
        if (tk.n == 1) { ofs_set_inv(!g_ofs_inv); return; }
        if (seq(tk.t[1], "on") || seq(tk.t[1], "1")) { ofs_set_inv(1); return; }
        if (seq(tk.t[1], "off") || seq(tk.t[1], "0")) { ofs_set_inv(0); return; }
        if (seq(tk.t[1], "radius") && tk.n >= 3 && ofs_num(tk.t[2], &v) && v > 1.0f && v < 500.0f) { g_ofs_radius = v; chat_say("inv: радиус поиска машины изменён"); return; }
        if (seq(tk.t[1], "hp") && tk.n >= 3) {
            if (seq(tk.t[2], "rand")) { g_ofs_invHp = -2; chat_say("inv: здоровье в пакете — случайное 0..100 (как в Lua)"); return; }
            if (seq(tk.t[2], "off")) { g_ofs_invHp = -1; chat_say("inv: здоровье в пакете не трогаю"); return; }
            if (ofs_num(tk.t[2], &v) && v >= 0 && v <= 255) { g_ofs_invHp = (int)v; chat_say("inv: здоровье в пакете задано"); return; }
        }
        if (seq(tk.t[1], "anim") && tk.n >= 3) {
            if (seq(tk.t[2], "off")) { g_ofs_invAnim = -1; chat_say("inv: анимку в режиме inv не трогаю"); return; }
            if (ofs_num(tk.t[2], &v) && v >= 0) { g_ofs_invAnim = (int)v; chat_say("inv: анимка в режиме inv задана"); return; }
        }
        chat_usage("Используйте: /ofs inv [on|off] | radius <м> | hp rand|<0-255>|off | anim <id>|off");
        return;
    }
    { Buf b; char m[200]; b_init(&b, m, sizeof m); b_s(&b, "Используйте: /ofs on|off|status|anim|inv|set|unset|fields|log|unhook   и   /invis (получено слов: ");
      b_int(&b, tk.n); b_s(&b, ", первое: «"); b_s(&b, tk.t[0]); b_s(&b, "»)"); chat_usage(m); }
}
static void native_inv_cmd(void* self, void* args, void* mi) {
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) return;
    ofs_log("ofs: команда /invis");
    OFS_STAGE("команда /invis");
    ofs_set_inv(!g_ofs_inv);
}

/* ───────────── Post в главный поток и самозапуск ───────────── */
static long ofs_now(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return ts[0]; }
static void ofs_post(void) {
    void *cb, *st, *args[2], *exc = NULL;
    OFS_STAGE("post: присоединение потока к IL2CPP");
    ensure_attached();                                              /* runtime_invoke из фонового потока без attach роняет игру */
    OFS_STAGE("post: делегат + Post");
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_ofs_cmd);
    st = A.string_new("ofs-auto");
    if (!cb || !st) return;
    args[0] = cb; args[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, args, &exc);
    if (exc) { ofs_log("ofs: Post бросил исключение"); return; }
    g_ofs_pending = 1; g_ofs_postT = ofs_now();
    OFS_STAGE("post: поставлен");
}
static void ofs_bg_tick(void) {                                   /* ~каждые 300 мс; раз в секунду просит главный поток обновить машину */
    long now;
    if (!g_ofs_inv || !g_mt_ctx || !g_mt_postM || !g_mt_kCb) return;
    now = ofs_now();
    if (g_ofs_pending && now - g_ofs_postT < 5) return;            /* колбэк ещё не отработал */
    g_ofs_pending = 0;
    if (now - g_ofs_postT < 1) return;
    ofs_post();
}
static int g_ofs_modId = -1;
static void* ofs_main(void* arg) {
    (void)arg;
    ofs_defaults();
    if (!hm_cmd_install("ofs", native_ofs_cmd)) return NULL;
    hm_cmd_install("invis", native_inv_cmd);
    hm_mod_ready(g_ofs_modId);
    ofs_log("ofs: мод загружен: /ofs, /invis");
    for (;;) { ofs_bg_tick(); usleep(TICK_US); }
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void ofs_ctor(void) {
    unsigned long t = 0;
    g_ofs_modId = hm_mod_register("ofs");
    pthread_create(&t, NULL, ofs_main, NULL);
}
#endif
