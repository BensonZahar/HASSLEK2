/*
 * hassle_fakepos.c — необязательный мод «/fpos»: ТЕСТОВАЯ подмена позиции, которую клиент шлёт серверу, БЕЗ перемещения персонажа.
 *
 * Зачем: для проверки серверных систем (метки, зоны, пикапы, завод) на тестовых NPC / тестовом сервере. Вместо телепорта (/tpl to …) персонаж стоит
 * на месте, а серверу уходит пакет «пешком» с заданной позицией. Только на вашем клиенте. КАК УБРАТЬ: удалите hassle_fakepos.c и пересоберите бандл.
 *
 * Как устроено (по дампу 20.10.2026, libil2cpp.so arm64):
 *   • LocalPlayer.SendOnFootFullSyncData(bool) собирает OnFootSyncData на стеке (Position = Transform.position + (0, 1, 0)) и зовёт
 *     SyncData.WriteOnFootPacket(PacketData packet, ref OnFootSyncData data) (RVA 0x135244C), затем отправляет пакет (id 0xCF) и копирует структуру
 *     в m_LastOnFootSyncData.
 *   • Перехват ставится на WriteOnFootPacket НЕ в самом начале, а на 4 инструкции после cbz (по смещению +0x0C): первые три инструкции метода —
 *     «str x20,[sp,#-0x20]! / stp x19,x30,[sp,#0x10] / cbz x0,…» — зависят от адреса (cbz) и перенесены быть не могут. Затираемые 4 инструкции
 *     (mov x20,x1 / ldrh w1,[x1] / mov x2,xzr / mov x19,x0) от адреса не зависят и переносятся в заглушку как есть. Перед патчем ВСЕ семь слов
 *     начала метода сверяются с эталоном этой сборки; не совпало (обновилась игра) — ничего не трогаем и пишем в konstt.log, что нашли.
 *   • Хук получает x0 = PacketData, x1 = указатель на OnFootSyncData; пока подмена включена, в структуру по смещению 0x08 (Vector3 Position) пишется
 *     заданная точка (+1 м по высоте — как у настоящего пакета), и метод идёт дальше как обычно. Хук ВСЕГДА пропускает вызов дальше (метод не обрывается).
 *   • Побочный эффект: структура потом копируется в m_LastOnFootSyncData — там остаётся подставная позиция (у вас самих её читают только моды,
 *     которым нужна позиция из пакета; настоящую даёт Transform). Пока подмена включена, пакет уходит при каждой отправке (структура всегда отличается).
 *   • НЕ перехватываются пакеты машины (Drive/Passenger), прицела и выстрела: в машине и при стрельбе сервер узнаёт настоящую позицию.
 *   • Только arm64 (как у /tpl ghost). Не проверено на устройстве — собрано и проверено компилятором; смотрите HLOG/konstt.log (строки «fpos:»).
 *
 * Команды в чате:
 *   /fpos X Y Z          сервер считает, что вы стоите в этой точке; X Y Z — те же координаты Unity (Transform), что у «/tpl to X Y Z» и у точек /zavod
 *   /fpos samp X Y Z     то же, но координаты из серверного Pawn-кода (SAMP): Transform = (x, z − 1, y) — как ZV_T(x, y, z, 1.0) в hassle_zavod.c
 *   /fpos here           подставлять ту точку, где вы стоите сейчас (сервер «запоминает» вас на месте — затем можно ходить: сервер увидит прежнее место)
 *   /fpos on | off       включить (с последней точкой) / выключить подмену. После «off» следующий пакет несёт настоящую позицию
 *   /fpos status         установлен ли перехват, включена ли подмена, точка, сколько пакетов подменено, настоящая позиция
 *   /fpos unhook         вернуть код игры в исходный вид (перехват снимается; для новой подмены он поставится снова)
 *   /fpos help
 *
 * Для других модов (без зависимости при сборке): konstt_fakepos_get(out) → 1, если подмена включена, и отдаёт точку (Transform);
 * konstt_fakepos_set(pos) — включить с точкой pos (Transform) или выключить, если pos == NULL. Зовут через weak-ссылку (см. hassle_zavod.c).
 */
#pragma once
#include "hassle_core.c"

#if defined(__aarch64__)
#define FK_ARM64 1
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int mprotect(void*, unsigned long, int);
extern long pwrite(int, const void*, unsigned long, long);
#endif

#define FK_PATCH_OFF 12                  /* байт от начала WriteOnFootPacket до затираемых 4 инструкций (после str / stp / cbz) */
#define FK_POS_OFF 0x08                  /* OnFootSyncData.Position (Vector3) */
#define FK_SYNC_DY 1.0f                  /* м: в пакет уходит Transform.y + 1.0 (центр тела, как в SA-MP) */

static volatile int g_fk_on;             /* 1 — в исходящий пакет «пешком» подставляется g_fk_pos */
static volatile unsigned g_fk_hits;      /* сколько пакетов подменено — по нему видно, что перехват живой */
static volatile float g_fk_pos[3];       /* где вас «видит» сервер (Transform; к высоте при подстановке прибавляется FK_SYNC_DY) */
static int g_fk_have;                    /* точка когда-либо задавалась */
static int g_fk_state;                   /* 1 — перехват стоит */
static int g_fk_modId = -1;
static char g_fk_err[160];

/* Зовётся из заглушки в середине SyncData.WriteOnFootPacket (главный поток). Всегда возвращает 0 — метод идёт дальше. Только атомики, без вызовов IL2CPP. */
__attribute__((noinline, used))
static int fk_hook(void* packet, void* data) {
    (void)packet;
    if (!__atomic_load_n(&g_fk_on, __ATOMIC_ACQUIRE) || !data || ((uptr)data & 3)) return 0;
    {
        float* p = (float*)((char*)data + FK_POS_OFF);
        p[0] = g_fk_pos[0];
        p[1] = g_fk_pos[1] + FK_SYNC_DY;
        p[2] = g_fk_pos[2];
    }
    __atomic_fetch_add(&g_fk_hits, 1u, __ATOMIC_RELAXED);
    return 0;
}

static int fk_fail(const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof g_fk_err - 1; i++) g_fk_err[i] = why[i];
    g_fk_err[i] = 0;
    dlog2("fpos: перехват не поставлен: ", why);
    return 0;
}

/* Инструкция зависит от своего адреса (при переносе в заглушку сломается)? */
static int fk_pc_rel(u32 w) {
    if ((w & 0x1F000000u) == 0x10000000u) return 1;              /* ADR, ADRP */
    if ((w & 0x3B000000u) == 0x18000000u) return 1;              /* LDR / LDRSW / PRFM (literal) */
    if ((w & 0x7C000000u) == 0x14000000u) return 1;              /* B, BL */
    if ((w & 0xFF000010u) == 0x54000000u) return 1;              /* B.cond */
    if ((w & 0x7E000000u) == 0x34000000u) return 1;              /* CBZ, CBNZ */
    if ((w & 0x7E000000u) == 0x36000000u) return 1;              /* TBZ, TBNZ */
    return 0;
}

#define FK_TRAMP_WORDS 24
/* Заглушка (arm64). Вход — абсолютным прыжком из середины метода; x0, x1 — как были (пакет и указатель на структуру).
   Сохраняем x0–x3 и x30, зовём fk_hook(x0, x1), восстанавливаем, выполняем 4 затёртые инструкции и прыгаем на следующую за ними.
     0  stp x0, x1, [sp, #-48]!    1  stp x2, x3, [sp, #16]     2  str x30, [sp, #32]
     3  ldr x16, =fk_hook (слово 14) 4  blr x16
     5  ldr x30, [sp, #32]         6  ldp x2, x3, [sp, #16]     7  ldp x0, x1, [sp], #48
     8-11 четыре затёртые инструкции (позиционно независимые — проверено fk_pc_rel)
    12  ldr x16, =место+16 (слово 16)  13  br x16
    14-15 адрес fk_hook            16-17 адрес продолжения */
static void fk_make_tramp(u32* t, const u32* orig4, uptr hook, uptr resume) {
    int i;
    t[0] = 0xA9BD07E0u;
    t[1] = 0xA9010FE2u;
    t[2] = 0xF90013FEu;
    t[3] = 0x58000170u;                                          /* ldr x16, [pc, #44] → слово 14 */
    t[4] = 0xD63F0200u;
    t[5] = 0xF94013FEu;
    t[6] = 0xA9410FE2u;
    t[7] = 0xA8C307E0u;
    for (i = 0; i < 4; i++) t[8 + i] = orig4[i];
    t[12] = 0x58000090u;                                         /* ldr x16, [pc, #16] → слово 16 */
    t[13] = 0xD61F0200u;
    t[14] = (u32)(hook & 0xFFFFFFFFu);
    t[15] = (u32)(((unsigned long long)hook) >> 32);
    t[16] = (u32)(resume & 0xFFFFFFFFu);
    t[17] = (u32)(((unsigned long long)resume) >> 32);
}
/* 16 байт, которыми заменяются 4 инструкции: ldr x16,[pc,#8]; br x16; .quad адрес заглушки */
static void fk_make_entry(u32* e, uptr tramp) {
    e[0] = 0x58000050u;
    e[1] = 0xD61F0200u;
    e[2] = (u32)(tramp & 0xFFFFFFFFu);
    e[3] = (u32)(((unsigned long long)tramp) >> 32);
}

#ifdef FK_ARM64
static void* g_fk_veneer;                /* страница с заглушкой (после снятия остаётся — вдруг поток ещё внутри) */
static uptr g_fk_site;                   /* адрес затираемого места (метод + FK_PATCH_OFF) */
static u32 g_fk_orig[4];                 /* исходные 4 слова */
static int g_fk_patched;

static void fk_flush(void* p, unsigned n) {              /* синхронизация кэшей после записи кода */
    uptr ctr, dl, il, a, e;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    dl = 4u << ((ctr >> 16) & 15u); il = 4u << (ctr & 15u);
    e = (uptr)p + n;
    for (a = (uptr)p & ~(dl - 1); a < e; a += dl) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uptr)p & ~(il - 1); a < e; a += il) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
/* Записать n слов в код игры; права на страницы возвращаются как были (r-x). Слова пишутся с конца к началу: первым ложится адрес,
   последним — первая инструкция. Запасной путь — /proc/self/mem. */
static int fk_write_code(uptr addr, const u32* w, int n) {
    uptr p0 = addr & ~(uptr)4095, p1 = (addr + (uptr)n * 4 - 1) & ~(uptr)4095;
    unsigned long len = (unsigned long)(p1 - p0) + 4096;
    int i;
    if (mprotect((void*)p0, len, 7) == 0) {
        for (i = n - 1; i >= 0; i--) ((volatile u32*)addr)[i] = w[i];
        fk_flush((void*)addr, (unsigned)n * 4);
        mprotect((void*)p0, len, 5);
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, w, (unsigned long)n * 4, (long)addr); close(fd); }
        if (r == (long)n * 4) { fk_flush((void*)addr, (unsigned)n * 4); return 1; }
    }
    return 0;
}
static void fk_hex(Buf* b, u32 w) {
    int i;
    for (i = 7; i >= 0; i--) { unsigned d = (w >> (i * 4)) & 15u; b_ch(b, (char)(d < 10 ? '0' + d : 'a' + d - 10)); }
}
static int fk_install(void) {
    void *kls, *m; uptr fn; u32 w[7]; int i;
    if (g_fk_state == 1) return 1;
    if (!g_img) return fk_fail("игра ещё не готова");
    kls = A.class_from_name(g_img, "Hassle.Network", "SyncData");
    if (!kls) return fk_fail("нет класса Hassle.Network.SyncData");
    m = find_method(kls, "WriteOnFootPacket", 2);
    if (!m || !hm_readable(m, sizeof(void*))) return fk_fail("нет метода SyncData.WriteOnFootPacket(PacketData, ref OnFootSyncData)");
    fn = *(uptr*)m;
    if (!fn || (fn & 3) || !hm_range_ok((void*)fn, 64)) return fk_fail("адрес WriteOnFootPacket нечитаем");
    for (i = 0; i < 7; i++) w[i] = ((const u32*)fn)[i];
    if (g_fk_veneer && g_fk_site == fn + FK_PATCH_OFF) { for (i = 0; i < 4; i++) w[3 + i] = g_fk_orig[i]; }      /* уже патчен нами — берём сохранённое */
    /* эталон начала метода в этой сборке: str x20,[sp,#-0x20]! / stp x19,x30,[sp,#0x10] / cbz x0,… / mov x20,x1 / ldrh w1,[x1] / mov x2,xzr / mov x19,x0 */
    if (w[0] != 0xF81E0FF4u || w[1] != 0xA9017BF3u || (w[2] & 0xFF00001Fu) != 0xB4000000u ||
        w[3] != 0xAA0103F4u || w[4] != 0x79400021u || w[5] != 0xAA1F03E2u || w[6] != 0xAA0003F3u) {
        char lg[240]; Buf x;
        b_init(&x, lg, sizeof lg); b_s(&x, "fpos: начало WriteOnFootPacket не как в эталонной сборке (другая версия игры?): ");
        for (i = 0; i < 7; i++) { fk_hex(&x, w[i]); b_ch(&x, ' '); }
        dlog(lg);
        return fk_fail("начало WriteOnFootPacket не совпало с эталоном (другая версия игры?) — слова в konstt.log");
    }
    for (i = 0; i < 4; i++) if (fk_pc_rel(w[3 + i])) return fk_fail("затираемые инструкции зависят от адреса — перенести нельзя");
    if (g_fk_veneer && g_fk_site && g_fk_site != fn + FK_PATCH_OFF) return fk_fail("адрес метода изменился после первой установки");
    if (!g_fk_veneer) {
        u32 v[FK_TRAMP_WORDS];
        void* page = mmap(NULL, 4096, 3, 0x22, -1, 0);           /* R|W, MAP_PRIVATE|MAP_ANONYMOUS; адрес любой — прыжок абсолютный */
        if (page == (void*)-1 || !page) return fk_fail("не удалось выделить страницу под заглушку");
        for (i = 0; i < FK_TRAMP_WORDS; i++) v[i] = 0xD503201Fu;     /* nop */
        fk_make_tramp(v, w + 3, (uptr)fk_hook, fn + FK_PATCH_OFF + 16);
        for (i = 0; i < FK_TRAMP_WORDS; i++) ((u32*)page)[i] = v[i];
        fk_flush(page, FK_TRAMP_WORDS * 4);
        if (mprotect(page, 4096, 5) != 0) { munmap(page, 4096); return fk_fail("не удалось сделать страницу заглушки исполняемой"); }
        g_fk_veneer = page;
        g_fk_site = fn + FK_PATCH_OFF;
        for (i = 0; i < 4; i++) g_fk_orig[i] = w[3 + i];
    }
    if (!g_fk_patched) {
        u32 e[4];
        fk_make_entry(e, (uptr)g_fk_veneer);
        if (!fk_write_code(g_fk_site, e, 4)) return fk_fail("запись в код игры запрещена системой");
        g_fk_patched = 1;
    }
    g_fk_state = 1;
    dlog("fpos: перехват SyncData.WriteOnFootPacket установлен");
    return 1;
}
static void fk_unhook(void) {
    __atomic_store_n(&g_fk_on, 0, __ATOMIC_RELEASE);
    if (g_fk_patched) {
        if (fk_write_code(g_fk_site, g_fk_orig, 4)) { g_fk_patched = 0; g_fk_state = 0; dlog("fpos: перехват снят, код игры восстановлен"); }
        else dlog("fpos: не удалось вернуть исходные инструкции");
    }
}
#else
static int fk_install(void) { return fk_fail("перехват есть только для arm64"); }
static void fk_unhook(void) { __atomic_store_n(&g_fk_on, 0, __ATOMIC_RELEASE); }
#endif

/* ── включить / выключить ── */
static int fk_finite(const float* v) {
    int i;
    for (i = 0; i < 3; i++) if (!(v[i] == v[i]) || v[i] > 1e6f || v[i] < -1e6f) return 0;
    return 1;
}
static int fk_set(const float* pos) {                    /* pos == NULL — выключить */
    if (!pos) { __atomic_store_n(&g_fk_on, 0, __ATOMIC_RELEASE); return 1; }
    if (!fk_finite(pos)) return 0;
    if (!fk_install()) return 0;
    g_fk_pos[0] = pos[0]; g_fk_pos[1] = pos[1]; g_fk_pos[2] = pos[2];
    g_fk_have = 1;
    __atomic_store_n(&g_fk_on, 1, __ATOMIC_RELEASE);
    return 1;
}
/* Для других модов (weak-ссылка): 1 — подмена включена, out — точка (Transform) */
__attribute__((used))
int konstt_fakepos_get(float* out) {
    if (!out || !__atomic_load_n(&g_fk_on, __ATOMIC_ACQUIRE)) return 0;
    out[0] = g_fk_pos[0]; out[1] = g_fk_pos[1]; out[2] = g_fk_pos[2];
    return 1;
}
__attribute__((used))
int konstt_fakepos_set(const float* pos) { return fk_set(pos); }

/* ── настоящая позиция (Transform) ── */
static void* fk_obj(void* obj, const char* name) {       /* метод без аргументов, вернувший ссылочный объект */
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int fk_real_pos(float* out) {
    void *me, *tr, *m, *exc = NULL, *r;
    if (!g_safe || !il2cpp_light()) return 0;
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) return 0;
    tr = fk_obj(me, "get_Transform");
    if (!tr) tr = fk_obj(me, "get_transform");
    if (!tr) return 0;
    m = find_method(A.object_get_class(tr), "get_position", 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, tr, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    {
        const float* f = (const float*)((char*)r + HDR);
        if (!fk_finite(f)) return 0;
        out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    }
    return 1;
}

/* ── разбор команды ── */
static int fk_tokens(void* arr, char tok[][32], int maxn) {
    long len; int i, n = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[96]; int cnt, p = 0, k; const u16* ch;
        if (!s) continue;
        cnt = A.string_length(s); ch = A.string_chars(s);
        if (cnt <= 0 || cnt > 40 || !ch) continue;
        utf16_to_utf8(ch, cnt, tmp, sizeof tmp);
        for (k = 0; tmp[k]; k++) if (tmp[k] >= 'A' && tmp[k] <= 'Z') tmp[k] = (char)(tmp[k] + 32);
        while (tmp[p]) {
            char w[32]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 31) w[q++] = tmp[p]; p++; }
            w[q] = 0;
            if (!q || w[0] == '/' || seq(w, "fpos")) continue;     /* сама команда может входить в массив (как у /tpl, /zavod) */
            if (n < maxn) { int z; for (z = 0; z <= q; z++) tok[n][z] = w[z]; n++; }
        }
    }
    return n;
}
static int fk_num(const char* s, float* out) {           /* -12.5 / 3,75 / +7 (без экспоненты) */
    int neg = 0, any = 0; double v = 0, f = 1;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.' || *s == ',') { s++; while (*s >= '0' && *s <= '9') { f /= 10; v += (*s - '0') * f; s++; any = 1; } }
    if (!any || *s) return 0;
    *out = (float)(neg ? -v : v);
    return 1;
}
static void fk_say_xyz(const char* head, const float* p, const char* tail) {
    char m[200]; Buf b;
    b_init(&b, m, sizeof m); b_s(&b, head);
    b_fix(&b, p[0], 2); b_ch(&b, ' '); b_fix(&b, p[1], 2); b_ch(&b, ' '); b_fix(&b, p[2], 2);
    b_s(&b, tail);
    chat_say(m);
}
static void fk_usage(void) {
    chat_usage("/fpos X Y Z — сервер считает, что вы в точке (координаты Unity, как у /tpl to), персонаж не двигается");
    chat_usage("/fpos samp X Y Z — то же, координаты из серверного кода (SAMP) | /fpos here — подставлять текущее место");
    chat_usage("/fpos on | off | status | unhook — включить с прежней точкой / выключить / состояние / снять перехват");
}
static void fk_status(void) {
    char m[220]; Buf b; float rp[3], tp[3];
    b_init(&b, m, sizeof m);
    b_s(&b, "/fpos: перехват "); b_s(&b, g_fk_state == 1 ? "стоит" : "не стоит");
    b_s(&b, ", подмена "); b_s(&b, __atomic_load_n(&g_fk_on, __ATOMIC_ACQUIRE) ? "ВКЛ" : "выкл");
    b_s(&b, ", подменено пакетов: "); b_int(&b, (long)g_fk_hits);
    chat_say(m);
    if (g_fk_state != 1 && g_fk_err[0]) chat_usage(g_fk_err);
    if (g_fk_have) { tp[0] = g_fk_pos[0]; tp[1] = g_fk_pos[1]; tp[2] = g_fk_pos[2]; fk_say_xyz("Точка для сервера: ", tp, " (Transform)"); }
    if (fk_real_pos(rp)) fk_say_xyz("Вы на самом деле: ", rp, " (Transform)");
}
static void fk_cmd_run(char tok[][32], int nt) {
    float p[3];
    if (nt == 0 || seq(tok[0], "help") || seq(tok[0], "?") || seq(tok[0], "помощь")) { fk_usage(); return; }
    if (seq(tok[0], "status") || seq(tok[0], "статус")) { fk_status(); return; }
    if (seq(tok[0], "unhook")) { fk_unhook(); chat_say(g_fk_state ? "/fpos: снять не вышло — подробности в konstt.log" : "/fpos: перехват снят, подмена выключена"); return; }
    if (seq(tok[0], "off") || seq(tok[0], "выкл") || seq(tok[0], "0")) { fk_set(NULL); chat_say("/fpos: подмена выключена — сервер снова получает настоящую позицию"); return; }
    if (seq(tok[0], "on") || seq(tok[0], "вкл") || seq(tok[0], "1")) {
        float q[3];
        if (!g_fk_have) { chat_usage("/fpos: точка ещё не задана — /fpos X Y Z, /fpos samp X Y Z или /fpos here"); return; }
        q[0] = g_fk_pos[0]; q[1] = g_fk_pos[1]; q[2] = g_fk_pos[2];
        if (!fk_set(q)) { chat_say("/fpos: перехват не поставлен — подробности в konstt.log"); if (g_fk_err[0]) chat_usage(g_fk_err); return; }
        fk_say_xyz("/fpos: подмена ВКЛ, сервер видит вас в ", q, "");
        return;
    }
    if (seq(tok[0], "here") || seq(tok[0], "тут")) {
        if (!fk_real_pos(p)) { chat_say("/fpos here: ваша позиция не прочиталась — подробности в konstt.log"); return; }
        if (!fk_set(p)) { chat_say("/fpos: перехват не поставлен — подробности в konstt.log"); if (g_fk_err[0]) chat_usage(g_fk_err); return; }
        fk_say_xyz("/fpos: сервер запомнил вас здесь: ", p, " — можно ходить, он увидит это место");
        return;
    }
    {
        int samp = seq(tok[0], "samp") || seq(tok[0], "самп"), o = samp ? 1 : 0;
        if (nt >= o + 3 && fk_num(tok[o], &p[0]) && fk_num(tok[o + 1], &p[1]) && fk_num(tok[o + 2], &p[2])) {
            if (samp) { float x = p[0], y = p[1], z = p[2]; p[0] = x; p[1] = z - 1.0f; p[2] = y; }     /* SAMP (x, y, z) → Transform (x, z − 1, y) */
            if (!fk_set(p)) { chat_say("/fpos: не вышло — координаты или перехват (подробности в konstt.log)"); if (g_fk_err[0]) chat_usage(g_fk_err); return; }
            fk_say_xyz("/fpos: подмена ВКЛ, сервер видит вас в ", p, " (персонаж стоит на месте)");
            dlog("fpos: подмена включена");
            return;
        }
    }
    fk_usage();
}
static void native_fk_cmd(void* self, void* args, void* mi) {
    char tok[8][32]; int nt;
    (void)self; (void)mi;
    nt = fk_tokens(args, tok, 8);
    fk_cmd_run(tok, nt);
}
static void* fk_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("fpos", native_fk_cmd)) return NULL;       /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /fpos */
    hm_mod_ready(g_fk_modId);
    dlog("fpos: мод загружен, команда /fpos работает");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void fk_ctor(void) {
    unsigned long t = 0;
    g_fk_modId = hm_mod_register("fpos");
    pthread_create(&t, NULL, fk_main, NULL);
}
#endif
