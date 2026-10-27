/*
 * hassle_weapon.c — необязательный мод «/vgun»: показать игроку с заданным ID оружие в руках. Только на вашем клиенте, сервер ничего не знает.
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите этот файл и пересоберите бандл.
 *
 * Команда в чате (нативная, как /nocol):
 *   /vgun <id> <оружие>     дать игроку <id> оружие: номер SA-MP (30) или имя (AK-47, ak47, m4, deagle, katana …)
 *   /vgun <id> off          вернуть игроку то оружие, что приходит с сервера
 *   /vgun <id>              что сейчас подменено у этого игрока
 *   /vgun list              какие оружия знает игра (номер=имя)
 *   /vgun off               снять все подмены
 *   /vgun status            состояние перехвата и подмен
 *   /vgun unhook            вернуть код игры в исходный вид (подмены снимаются)
 *
 * Как это работает
 *   • Оружие удалённого игрока игра выбирает сама, каждый кадр, в RemotePlayer.Update: берёт байт CurrentWeapon из последнего пакета
 *     синхронизации (m_LastOnFootSyncData), переводит его в строковый ID через WeaponConvertTable.ToID и, если он не совпал с тем,
 *     что в руках, зовёт Items.Has / Items.Add / Items.SetActiveWeapon. Разовая подмена не держится: уже на следующем кадре игра вернёт
 *     серверное оружие (а новый пакет вернул бы его и после записи в поле).
 *   • Поэтому в НАЧАЛО RemotePlayer.Update ставится перехват: 16 байт пролога заменяются на «LDR x16,=адрес; BR x16» (абсолютный прыжок,
 *     дальность не ограничена — рядом с кодом игры свободная память не нужна). Прыжок ведёт в маленькую заглушку в анонимной странице
 *     (где угодно в памяти): она сохраняет x0/x1/x30, зовёт wp_hook(игрок), возвращает регистры, выполняет те 4 инструкции пролога,
 *     что были затёрты, и прыгает на 5-ю инструкцию функции. wp_hook, если у игрока (Actor.m_ID) есть подмена, ПЕРЕД чтением игры кладёт
 *     наш байт в m_LastOnFootSyncData.CurrentWeapon (серверный байт запоминает); при снятии подмены на ближайшем кадре возвращает серверный.
 *     Всё остальное (создание префаба, анимация стойки, загрузка ресурсов) игра делает сама, в главном потоке, своим обычным путём.
 *   • Место патча не зашито адресом: функцию находим по имени (il2cpp_class_get_method_from_name), смещение CurrentWeapon берём из метаданных.
 *     Перед патчем проверяется, что в Update есть чтение CurrentWeapon (узор LDRB + LDR + BL) и что 4 затираемые инструкции не зависят от
 *     адреса (нет ADR/ADRP/B/BL/LDR literal/CBZ/TBZ) — иначе ничего не трогаем.
 *   • Список оружия берётся из WeaponConvertTable.ToID (в этой сборке 17 записей: 0,1,3,4,5,8,22-25,27,29-31,33,34,43). Словарь читается
 *     двумя способами: перебором записей и опросом ключей 0..63 через get_Item самой игры; результаты объединяются.
 *   • Только arm64. На arm32 команда отвечает, что не поддерживается (библиотека игры там другая).
 *   • Если «/proc»-защита или SELinux не дадут записать в код — перехват не ставится, игра работает как обычно (см. konstt.log).
 *   • Подмена привязана к ID, а не к человеку: если игрок вышел и его ID занял другой — подмена перейдёт к нему. /vgun off снимает всё.
 *   • Игрок в машине не затрагивается (игра в это время не меняет оружие по пакету «пешком»), себе оружие этой командой не дать.
 */
#pragma once
#include "hassle_core.c"

#if defined(__aarch64__)
#define WP_ARM64 1
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int mprotect(void*, unsigned long, int);
extern long pwrite(int, const void*, unsigned long, long);
#endif

/* ───────────── таблица подмен: ID игрока → байт оружия (читается из главного потока внутри хука, пишется командой) ───────────── */
#define WP_MAX 64
static volatile int g_wp_id[WP_MAX];                      /* -1 — ячейка свободна (до установки перехвата заполняется в wp_table_reset) */
static volatile int g_wp_byte[WP_MAX];                    /* какой байт оружия показываем */
static volatile int g_wp_srv[WP_MAX];                     /* последний серверный байт этого игрока, который хук видел (-1 — ещё не видел) */
static volatile int g_wp_mode[WP_MAX];                    /* 1 — подмена действует; 2 — снята: на ближайшем кадре вернуть серверный байт и освободить ячейку */
static volatile int g_wp_active;                          /* сколько ячеек занято: хук выходит сразу, если 0 */
static volatile unsigned g_wp_hits;                       /* сколько раз хук записал наш байт — по нему видно, что перехват живой */
static int g_wp_offId = 0x40;                             /* смещение Actor.m_ID; при установке берём из метаданных */
static int g_wp_offWeapon = -1;                           /* смещение m_LastOnFootSyncData.CurrentWeapon внутри RemotePlayer; -1 — перехват не настроен */
static void wp_refresh_active(void) { int i, n = 0; for (i = 0; i < WP_MAX; i++) if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) >= 0) n++; __atomic_store_n(&g_wp_active, n, __ATOMIC_RELEASE); }
static void wp_table_reset(void) { int i; for (i = 0; i < WP_MAX; i++) { __atomic_store_n(&g_wp_id[i], -1, __ATOMIC_RELEASE); g_wp_srv[i] = -1; g_wp_mode[i] = 0; } __atomic_store_n(&g_wp_active, 0, __ATOMIC_RELEASE); }

/* Зовётся из заглушки в начале RemotePlayer.Update (главный поток). Никаких вызовов IL2CPP и выделения памяти — только таблица и один байт игрока. */
__attribute__((noinline, used))
static void wp_hook(void* player) {
    int i, id;
    if (!player || g_wp_offWeapon < 0 || !__atomic_load_n(&g_wp_active, __ATOMIC_ACQUIRE)) return;
    id = (int)*(volatile u16*)((char*)player + g_wp_offId);
    for (i = 0; i < WP_MAX; i++) {
        volatile u8* wb; int mine;
        if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) != id) continue;
        wb = (volatile u8*)((char*)player + g_wp_offWeapon);
        mine = __atomic_load_n(&g_wp_byte[i], __ATOMIC_RELAXED) & 0xFF;
        if (__atomic_load_n(&g_wp_mode[i], __ATOMIC_ACQUIRE) == 2) {          /* подмена снята: вернуть серверный байт, ячейку освободить */
            int srv = __atomic_load_n(&g_wp_srv[i], __ATOMIC_RELAXED);
            if (srv >= 0 && *wb == (u8)mine) *wb = (u8)srv;
            __atomic_store_n(&g_wp_id[i], -1, __ATOMIC_RELEASE);
            wp_refresh_active();
        } else if (*wb != (u8)mine) {                                         /* пришёл серверный байт: запомнить и перекрыть нашим */
            __atomic_store_n(&g_wp_srv[i], (int)*wb, __ATOMIC_RELAXED);
            *wb = (u8)mine;
            __atomic_fetch_add(&g_wp_hits, 1, __ATOMIC_RELAXED);
        }
        return;
    }
}

/* ───────────── чистые помощники (без обращения к памяти игры; проверяются отдельно на настоящем коде libil2cpp) ───────────── */
/* слово — «LDRB w1,[xN,#off]» (беззнаковое смещение, любой N): N кладётся в *rn */
static int wp_is_ldrb(u32 w, unsigned off, int* rn) {
    if ((w & 0xFFFFFC1Fu) != (0x39400001u | (off << 10))) return 0;
    *rn = (int)((w >> 5) & 31u);
    return 1;
}
/* Узор: LDRB w1,[xN,#off]; LDR x?,[x?,#imm]; BL — загрузка байта и сразу вызов Dictionary.get_Item(ToID, байт). Возврат: индекс слова,
   -1 — не найдено, -2 — найдено больше одного места. Нужен как проверка, что это та самая функция и смещение верное. */
static int wp_scan(const u32* c, int n, unsigned off, int* rn) {
    int i, hit = -1, r = 0;
    for (i = 0; i + 2 < n; i++) {
        if (!wp_is_ldrb(c[i], off, &r)) continue;
        if ((c[i + 1] & 0xFFC00000u) != 0xF9400000u) continue;       /* LDR x?,[x?,#imm] */
        if ((c[i + 2] & 0xFC000000u) != 0x94000000u) continue;       /* BL */
        if (hit >= 0) return -2;
        hit = i; *rn = r;
    }
    return hit;
}
/* Инструкция зависит от своего адреса (при переносе в заглушку сломается)? */
static int wp_pc_rel(u32 w) {
    if ((w & 0x1F000000u) == 0x10000000u) return 1;              /* ADR, ADRP */
    if ((w & 0x3B000000u) == 0x18000000u) return 1;              /* LDR / LDRSW / PRFM (literal) */
    if ((w & 0x7C000000u) == 0x14000000u) return 1;              /* B, BL */
    if ((w & 0xFF000010u) == 0x54000000u) return 1;              /* B.cond */
    if ((w & 0x7E000000u) == 0x34000000u) return 1;              /* CBZ, CBNZ */
    if ((w & 0x7E000000u) == 0x36000000u) return 1;              /* TBZ, TBNZ */
    return 0;
}

#define WP_TRAMP_WORDS 16
/* Заглушка (arm64), 64 байта. Вход — абсолютным прыжком из начала Update, регистры как при входе в функцию (x0 — игрок, x1 — MethodInfo).
     0  stp x0, x1, [sp, #-32]!        1  str x30, [sp, #16]
     2  ldr x16, =wp_hook (слово 12)   3  blr x16
     4  ldr x30, [sp, #16]             5  ldp x0, x1, [sp], #32
     6-9 четыре затёртые инструкции пролога (позиционно независимые — проверено wp_pc_rel)
    10  ldr x16, =Update+16 (слово 14) 11 br x16
    12-13 адрес wp_hook                14-15 адрес Update+16 */
static void wp_make_tramp(u32* t, const u32* orig4, uptr hook, uptr resume) {
    int i;
    t[0] = 0xA9BE07E0u;
    t[1] = 0xF9000BFEu;
    t[2] = 0x58000150u;
    t[3] = 0xD63F0200u;
    t[4] = 0xF9400BFEu;
    t[5] = 0xA8C207E0u;
    for (i = 0; i < 4; i++) t[6 + i] = orig4[i];
    t[10] = 0x58000090u;
    t[11] = 0xD61F0200u;
    t[12] = (u32)(hook & 0xFFFFFFFFu);
    t[13] = (u32)(((unsigned long long)hook) >> 32);
    t[14] = (u32)(resume & 0xFFFFFFFFu);
    t[15] = (u32)(((unsigned long long)resume) >> 32);
}
/* 16 байт, которыми заменяется начало Update: ldr x16,[pc,#8]; br x16; .quad адрес заглушки */
static void wp_make_entry(u32* e, uptr tramp) {
    e[0] = 0x58000050u;
    e[1] = 0xD61F0200u;
    e[2] = (u32)(tramp & 0xFFFFFFFFu);
    e[3] = (u32)(((unsigned long long)tramp) >> 32);
}

/* ───────────── установка перехвата (только arm64) ───────────── */
#ifdef WP_ARM64
static void* g_wp_veneer;                                  /* страница с заглушкой (после снятия перехвата остаётся — вдруг поток ещё внутри) */
static uptr g_wp_site;                                     /* адрес начала RemotePlayer.Update */
static u32 g_wp_orig[4];                                   /* его исходные 4 слова */
#endif
static int g_wp_state;                                     /* 1 — перехват стоит */
static char g_wp_err[160];

static int wp_fail(const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof g_wp_err - 1; i++) g_wp_err[i] = why[i];
    g_wp_err[i] = 0;
    dlog2("vgun: перехват не поставлен: ", why);
    return 0;
}

#ifdef WP_ARM64
static void wp_flush(void* p, unsigned n) {                /* синхронизация кэшей после записи кода */
    uptr ctr, dl, il, a, e;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    dl = 4u << ((ctr >> 16) & 15u); il = 4u << (ctr & 15u);
    e = (uptr)p + n;
    for (a = (uptr)p & ~(dl - 1); a < e; a += dl) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uptr)p & ~(il - 1); a < e; a += il) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
/* Записать n слов в код игры. Окно с правом записи короткое, права на страницы возвращаются как были (r-x). Слова пишутся с конца к началу:
   первым ложится адрес, последним — первая инструкция. Запасной путь — запись в /proc/self/mem, если mprotect запрещён. */
static int wp_write_code(uptr addr, const u32* w, int n) {
    uptr p0 = addr & ~(uptr)4095, p1 = (addr + (uptr)n * 4 - 1) & ~(uptr)4095;
    unsigned long len = (unsigned long)(p1 - p0) + 4096;
    int i;
    if (mprotect((void*)p0, len, 7) == 0) {                 /* R|W|X: страница живая, другие потоки в ней исполняют код, снимать X нельзя */
        for (i = n - 1; i >= 0; i--) ((volatile u32*)addr)[i] = w[i];
        wp_flush((void*)addr, (unsigned)n * 4);
        mprotect((void*)p0, len, 5);                        /* R|X */
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, w, (unsigned long)n * 4, (long)addr); close(fd); }
        if (r == (long)n * 4) { wp_flush((void*)addr, (unsigned)n * 4); return 1; }
    }
    return 0;
}
static int wp_install(void) {
    void *kls, *m, *sf, *sc, *cw, *idf; uptr fn; u32 code[256]; int off, shift, rn = 0, idx, i;
    if (g_wp_state == 1) return 1;
    if (!g_img) return wp_fail("игра ещё не готова");
    kls = A.class_from_name(g_img, "Hassle.Players", "RemotePlayer");
    if (!kls) return wp_fail("нет класса RemotePlayer");
    m = A.class_get_method_from_name(kls, "Update", 0);
    if (!m || !hm_readable(m, sizeof(void*))) return wp_fail("нет метода RemotePlayer.Update");
    fn = *(uptr*)m;
    if (!fn || (fn & 3) || !hm_range_ok((void*)fn, sizeof code)) return wp_fail("адрес RemotePlayer.Update нечитаем");
    sf = find_field(kls, "m_LastOnFootSyncData");
    sc = sf ? A.class_from_type(A.field_get_type(sf)) : NULL;
    cw = sc ? find_field(sc, "CurrentWeapon") : NULL;
    if (!sf || !sc || !cw || !A.class_is_valuetype(sc)) return wp_fail("нет поля m_LastOnFootSyncData.CurrentWeapon");
    shift = struct_shift(sc);
    off = A.field_get_offset(sf) + A.field_get_offset(cw) - shift;
    dlog_i("vgun: смещение CurrentWeapon в игроке: ", off);
    if (off < 0 || off > 4095) return wp_fail("смещение CurrentWeapon вне диапазона");
    idf = find_field(kls, "m_ID");
    if (!idf || A.type_get_type(A.field_get_type(idf)) != 7) return wp_fail("нет поля Actor.m_ID (ushort)");
    g_wp_offId = A.field_get_offset(idf);
    dlog_i("vgun: смещение Actor.m_ID: ", g_wp_offId);
    for (i = 0; i < 256; i++) code[i] = ((const u32*)fn)[i];
    /* если функция уже пропатчена нами (после неудачного снятия) — исходные слова берём из сохранённых */
    if (g_wp_veneer && g_wp_site == fn) for (i = 0; i < 4; i++) code[i] = g_wp_orig[i];
    idx = wp_scan(code, 256, (unsigned)off, &rn);
    if (idx == -2) return wp_fail("в Update несколько подходящих мест — не рискую");
    if (idx < 0) return wp_fail("в Update не нашёл чтение CurrentWeapon (другая сборка игры?)");
    dlog_i("vgun: чтение CurrentWeapon в Update, байт от начала: ", idx * 4);
    if (idx < 4) return wp_fail("чтение CurrentWeapon слишком близко к началу Update");
    for (i = 0; i < 4; i++) if (wp_pc_rel(code[i])) return wp_fail("пролог Update зависит от адреса — перенести нельзя");
    if (!(g_wp_veneer && g_wp_site == fn)) {
        u32 v[WP_TRAMP_WORDS];
        void* page = mmap(NULL, 4096, 3, 0x22, -1, 0);       /* R|W, MAP_PRIVATE|MAP_ANONYMOUS; адрес любой — прыжок абсолютный */
        if (page == (void*)-1 || !page) return wp_fail("не удалось выделить страницу под заглушку");
        wp_make_tramp(v, code, (uptr)wp_hook, fn + 16);
        for (i = 0; i < WP_TRAMP_WORDS; i++) ((u32*)page)[i] = v[i];
        wp_flush(page, sizeof v);
        if (mprotect(page, 4096, 5) != 0) { munmap(page, 4096); return wp_fail("не удалось сделать страницу заглушки исполняемой"); }
        g_wp_veneer = page; g_wp_site = fn;
        for (i = 0; i < 4; i++) g_wp_orig[i] = code[i];
    }
    {
        u32 e[4];
        wp_make_entry(e, (uptr)g_wp_veneer);
        g_wp_offWeapon = off;                                /* до записи патча: хук с первого же вызова знает, куда писать */
        if (!wp_write_code(fn, e, 4)) { g_wp_offWeapon = -1; return wp_fail("запись в код игры запрещена системой"); }
    }
    g_wp_state = 1;
    dlog("vgun: перехват RemotePlayer.Update установлен (вход функции, абсолютный прыжок)");
    return 1;
}
static void wp_unhook(void) {
    if (g_wp_state != 1) return;
    if (wp_write_code(g_wp_site, g_wp_orig, 4)) { g_wp_state = 0; dlog("vgun: перехват снят, код игры восстановлен"); }
    else dlog("vgun: не удалось вернуть исходные инструкции");
}
#else
static int wp_install(void) { return wp_fail("поддерживается только arm64"); }
static void wp_unhook(void) {}
#endif

/* ───────────── оружия, которые знает игра: WeaponConvertTable.ToID (Dictionary<int,string>) ───────────── */
typedef struct { int key; char name[28]; } WpItem;
static WpItem g_wp_items[64];
static int g_wp_nitems;
static int g_wp_dropObj, g_wp_dropLen, g_wp_dropKey;       /* сколько записей перебор отбросил и почему (для журнала) */
static int wp_have(int key) { int i; for (i = 0; i < g_wp_nitems; i++) if (g_wp_items[i].key == key) return 1; return 0; }
static int wp_add_item(int k, void* val) {                  /* 1 — добавлено */
    WpItem* it; int n;
    if (g_wp_nitems >= 64 || !val) return 0;
    if (!hm_obj_ok(val)) { g_wp_dropObj++; return 0; }
    n = A.string_length(val);
    if (n <= 0 || n > 27) { g_wp_dropLen++; return 0; }
    if (k < 0 || k > 255) { g_wp_dropKey++; return 0; }
    if (wp_have(k)) return 0;
    it = &g_wp_items[g_wp_nitems++];
    it->key = k;
    utf16_to_utf8(A.string_chars(val), n, it->name, sizeof it->name);
    return 1;
}
static void wp_item_cb(void* key, void* val, void* ctx) {
    (void)ctx;
    wp_add_item((int)((uptr)key & 0xFFFFFFFFu), val);        /* ключ int лежит в младших 4 байтах прочитанного слова */
}
/* Опрос ключей через get_Item самой игры: не зависит от внутреннего устройства словаря. Нет ключа — игра бросает исключение, runtime_invoke его ловит. */
static int wp_probe_items(void* dict) {
    void *dk, *gi; int k, added = 0;
    dk = A.object_get_class(dict);
    gi = dk ? A.class_get_method_from_name(dk, "get_Item", 1) : NULL;
    if (!gi) { dlog("vgun: у словаря оружия не нашёл get_Item"); return 0; }
    for (k = 0; k < 64; k++) {
        void* args[1]; void* r; void* exc = NULL; int key = k;
        if (wp_have(k)) continue;
        args[0] = &key;
        r = A.runtime_invoke(gi, dict, args, &exc);
        if (exc || !r) continue;
        if (wp_add_item(k, r)) added++;
    }
    return added;
}
static int wp_load_items(void) {
    void *kls, *f, *dict = NULL; int i, j, byIter, byProbe;
    g_wp_nitems = 0; g_wp_dropObj = g_wp_dropLen = g_wp_dropKey = 0;
    kls = A.class_from_name(g_img, "Hassle.Items", "WeaponConvertTable");
    f = kls ? A.class_get_field_from_name(kls, "ToID") : NULL;
    if (!f) { dlog("vgun: нет WeaponConvertTable.ToID"); return 0; }
    A.field_static_get_value(f, &dict);
    if (!dict) { dlog("vgun: WeaponConvertTable.ToID пока пуст (null)"); return 0; }
    dlog_i("vgun: ToID, Count по полю _count: ", int_field(dict, "_count", "count", NULL, -1));
    dict_each(dict, wp_item_cb, NULL);
    byIter = g_wp_nitems;
    byProbe = wp_probe_items(dict);
    dlog_i("vgun: оружий найдено перебором записей: ", byIter);
    dlog_i("vgun: оружий добавлено опросом get_Item: ", byProbe);
    dlog_i("vgun: отброшено: объект нечитаем / длина имени / ключ вне диапазона: ", (long)g_wp_dropObj * 10000 + (long)g_wp_dropLen * 100 + g_wp_dropKey);
    for (i = 1; i < g_wp_nitems; i++) {                      /* по номеру */
        WpItem t = g_wp_items[i];
        for (j = i - 1; j >= 0 && g_wp_items[j].key > t.key; j--) g_wp_items[j + 1] = g_wp_items[j];
        g_wp_items[j + 1] = t;
    }
    return g_wp_nitems;
}
static void wp_norm(const char* s, char* out, int cap) {     /* нижний регистр, только буквы и цифры: «AK-47» → «ak47» */
    int o = 0;
    for (; *s && o + 1 < cap; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
    }
    out[o] = 0;
}
static int wp_all_digits(const char* s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static int wp_num(const char* s) { int v = 0; while (*s >= '0' && *s <= '9' && v < 100000) v = v * 10 + (*s++ - '0'); return v; }
static const char* g_wp_alias[][2] = {
    { "deagle", "deserteagle" }, { "colt", "colt45" }, { "pistol", "colt45" }, { "spas", "combatshotgun" }, { "spas12", "combatshotgun" },
    { "sniper", "sniperrifle" }, { "ak", "ak47" }, { "m4a1", "m4" }, { "brass", "brassknuckles" }, { "knuckles", "brassknuckles" },
};
/* номер оружия по тексту (номер SA-MP или имя), -1 — не нашли или неоднозначно; *name получает имя из таблицы игры */
static int wp_find_weapon(const char* tok, const char** name) {
    char t[32], n[32]; int i, hit = -1, cnt = 0;
    wp_norm(tok, t, sizeof t);
    if (!t[0]) return -1;
    if (wp_all_digits(tok)) {
        int v = wp_num(tok);
        for (i = 0; i < g_wp_nitems; i++) if (g_wp_items[i].key == v) { *name = g_wp_items[i].name; return v; }
        return -1;
    }
    for (i = 0; i < (int)(sizeof g_wp_alias / sizeof g_wp_alias[0]); i++) if (seq(t, g_wp_alias[i][0])) { wp_norm(g_wp_alias[i][1], t, sizeof t); break; }
    for (i = 0; i < g_wp_nitems; i++) { wp_norm(g_wp_items[i].name, n, sizeof n); if (seq(n, t)) { *name = g_wp_items[i].name; return g_wp_items[i].key; } }
    for (i = 0; i < g_wp_nitems; i++) {                      /* единственное имя, начинающееся с введённого */
        wp_norm(g_wp_items[i].name, n, sizeof n);
        if (sprefix(n, t)) { hit = i; cnt++; }
    }
    if (cnt == 1) { *name = g_wp_items[hit].name; return g_wp_items[hit].key; }
    return -1;
}
static const char* wp_name_of(int key) { int i; for (i = 0; i < g_wp_nitems; i++) if (g_wp_items[i].key == key) return g_wp_items[i].name; return NULL; }

/* ───────────── игроки ───────────── */
typedef struct { int id; int found; } WpFind;
static void wp_find_cb(void* item, void* ctx) {
    WpFind* f = (WpFind*)ctx; int ok = 0; double v;
    v = num_field(item, "m_ID", &ok);
    if (ok && (int)v == f->id) f->found = 1;
}
static int wp_in_stream(int id) {                            /* игрок с таким ID сейчас в зоне видимости (PlayersInStream) */
    void *pool, *stream, *arr; WpFind f;
    f.id = id; f.found = 0;
    pool = current_pool();
    stream = pool ? ref_field_s(pool, "PlayersInStream") : NULL;
    if (!stream || !hm_obj_ok(stream)) return 0;
    arr = ref_field(stream, "_items"); if (!arr) arr = ref_field(stream, "items");
    walk_ref_array(arr, int_field(stream, "_size", "size", NULL, -1), 1, wp_find_cb, &f);
    return f.found;
}
static int wp_my_id(void) {
    void* lp = local_player(); int ok = 0; double v;
    if (!lp) return -1;
    v = num_field(lp, "m_ID", &ok);
    return ok ? (int)v : -1;
}
static int wp_slot(int id) { int i; for (i = 0; i < WP_MAX; i++) if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) == id) return i; return -1; }
static int wp_count(void) { int i, n = 0; for (i = 0; i < WP_MAX; i++) if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) >= 0 && __atomic_load_n(&g_wp_mode[i], __ATOMIC_ACQUIRE) == 1) n++; return n; }
static int wp_is_set(int id) { int s = wp_slot(id); return s >= 0 && __atomic_load_n(&g_wp_mode[s], __ATOMIC_ACQUIRE) == 1; }
static void wp_clear_slot(int s) {                           /* вернуть серверное оружие: если хук уже видел серверный байт — вернёт его на ближайшем кадре, иначе просто освободить */
    if (s < 0) return;
    if (__atomic_load_n(&g_wp_srv[s], __ATOMIC_RELAXED) >= 0) __atomic_store_n(&g_wp_mode[s], 2, __ATOMIC_RELEASE);
    else { __atomic_store_n(&g_wp_id[s], -1, __ATOMIC_RELEASE); wp_refresh_active(); }
}
static void wp_clear_id(int id) { wp_clear_slot(wp_slot(id)); }
static void wp_clear_all(void) { int i; for (i = 0; i < WP_MAX; i++) if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) >= 0) wp_clear_slot(i); }
static int wp_set(int id, int key) {                         /* 1 — готово, 0 — таблица полна */
    int s = wp_slot(id), i, fresh = 0;
    if (s < 0) { for (i = 0; i < WP_MAX; i++) if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) < 0) { s = i; fresh = 1; break; } }
    if (s < 0) return 0;
    __atomic_store_n(&g_wp_byte[s], key, __ATOMIC_RELAXED);
    if (fresh) __atomic_store_n(&g_wp_srv[s], -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wp_mode[s], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wp_id[s], id, __ATOMIC_RELEASE);     /* ID публикуется последним: хук не увидит ячейку без байта */
    wp_refresh_active();
    return 1;
}

/* ───────────── команда ───────────── */
static int wp_tokens(void* arr, char tok[][24], int max) {   /* слова из string[] (элемент может содержать несколько слов); слово «vgun» отбрасываем */
    long len; int i, nt = 0; char* data;
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
            char t[24]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 23) t[q++] = tmp[p]; p++; }
            t[q] = 0;
            if (!q || seq(t, "vgun") || seq(t, "/vgun")) continue;
            if (nt < max) { unsigned k = 0; for (; t[k]; k++) tok[nt][k] = t[k]; tok[nt][k] = 0; nt++; }
        }
    }
    return nt;
}
static int wp_is(const char* t, const char* a, const char* b) { return seq(t, a) || (b && seq(t, b)); }

static void wp_usage(void) {
    chat_usage("Используйте: /vgun <id> <оружие> — дать игроку оружие в руки (только у вас на экране)");
    chat_usage("/vgun <id> off — вернуть как у сервера;  /vgun off — снять всё;  /vgun list — список оружия;  /vgun status");
}
static void wp_say_list(void) {
    char msg[220]; Buf b; int i, n = 0;
    if (!wp_load_items()) { chat_say("vgun: таблица оружия игры пока недоступна"); return; }
    b_init(&b, msg, sizeof msg);
    for (i = 0; i < g_wp_nitems; i++) {
        if (b.n > 150) { chat_say(msg); b_init(&b, msg, sizeof msg); n = 0; }
        if (n++) b_s(&b, "  ");
        b_int(&b, g_wp_items[i].key); b_ch(&b, '='); b_s(&b, g_wp_items[i].name);
    }
    if (b.n) chat_say(msg);
}
static void wp_say_status(void) {
    char msg[260]; Buf b; int i, first = 1;
    b_init(&b, msg, sizeof msg);
    b_s(&b, "vgun: перехват "); b_s(&b, g_wp_state == 1 ? "стоит" : "не стоит");
    b_s(&b, ", подмен "); b_int(&b, wp_count()); b_s(&b, ", сработал раз "); b_int(&b, (long)__atomic_load_n(&g_wp_hits, __ATOMIC_RELAXED));
    if (g_wp_state != 1 && g_wp_err[0]) { b_s(&b, ". Причина: "); b_s(&b, g_wp_err); }
    chat_say(msg);
    wp_load_items();
    b_init(&b, msg, sizeof msg);
    for (i = 0; i < WP_MAX; i++) {
        int id = __atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE);
        const char* nm;
        if (id < 0 || __atomic_load_n(&g_wp_mode[i], __ATOMIC_ACQUIRE) != 1) continue;
        nm = wp_name_of(__atomic_load_n(&g_wp_byte[i], __ATOMIC_RELAXED));
        if (b.n > 170) { chat_say(msg); b_init(&b, msg, sizeof msg); first = 1; }
        if (!first) b_s(&b, ";  ");
        first = 0;
        b_s(&b, "ID "); b_int(&b, id); b_s(&b, " — "); b_s(&b, nm ? nm : "?");
    }
    if (b.n) chat_say(msg);
}

static void native_vgun_cmd(void* self, void* args, void* mi) {
    char tok[4][24]; int nt; Buf b; char msg[300];
    (void)self; (void)mi;
    nt = wp_tokens(args, tok, 4);
    dlog_i("нативная /vgun: вызвана, слов ", nt);
    if (nt == 0) { wp_usage(); return; }
    if (wp_is(tok[0], "list", "l")) { wp_say_list(); return; }
    if (wp_is(tok[0], "status", "diag")) { wp_say_status(); return; }
    if (wp_is(tok[0], "unhook", NULL)) { wp_unhook(); if (g_wp_state != 1) wp_table_reset(); chat_say(g_wp_state == 1 ? "vgun: не удалось снять перехват, см. konstt.log" : "vgun: перехват снят, подмены сброшены (оружие у игроков вернётся с их следующим пакетом)"); return; }
    if (wp_is(tok[0], "off", "clear") || wp_is(tok[0], "reset", "stop")) { wp_clear_all(); chat_say("vgun: все подмены сняты, у игроков оружие как на сервере"); return; }
    if (wp_all_digits(tok[0])) {
        int id = wp_num(tok[0]), key, me; const char* nm = NULL;
        if (id > 65535) { chat_say("vgun: такой ID не бывает"); return; }
        if (nt == 1) {                                       /* что подменено у игрока */
            int s = wp_is_set(id) ? wp_slot(id) : -1;
            b_init(&b, msg, sizeof msg);
            if (s < 0) { b_s(&b, "vgun: у игрока "); b_int(&b, id); b_s(&b, " подмены нет"); }
            else { wp_load_items(); nm = wp_name_of(__atomic_load_n(&g_wp_byte[s], __ATOMIC_RELAXED)); b_s(&b, "vgun: у игрока "); b_int(&b, id); b_s(&b, " подменено оружие: "); b_s(&b, nm ? nm : "?"); }
            chat_say(msg);
            return;
        }
        if (wp_is(tok[1], "off", "clear") || wp_is(tok[1], "del", "none")) {
            b_init(&b, msg, sizeof msg);
            if (!wp_is_set(id)) { b_s(&b, "vgun: у игрока "); b_int(&b, id); b_s(&b, " подмены и не было"); }
            else { wp_clear_id(id); b_s(&b, "vgun: игроку "); b_int(&b, id); b_s(&b, " возвращено оружие с сервера"); }
            chat_say(msg);
            return;
        }
        if (!wp_load_items()) { chat_say("vgun: таблица оружия игры пока недоступна (игра ещё не загрузила предметы), попробуйте позже"); return; }
        key = wp_find_weapon(tok[1], &nm);
        if (key < 0) {
            b_init(&b, msg, sizeof msg);
            b_s(&b, "vgun: не знаю оружие «"); b_s(&b, tok[1]); b_s(&b, "» (или под это начало подходит несколько). Список: /vgun list");
            chat_say(msg);
            return;
        }
        me = wp_my_id();
        if (me >= 0 && me == id) { chat_say("vgun: себе оружие этой командой не дать — только другим игрокам"); return; }
        if (!wp_install()) {
            b_init(&b, msg, sizeof msg);
            b_s(&b, "vgun: не вышло поставить перехват — "); b_s(&b, g_wp_err); b_s(&b, " (подробности в HLOG/konstt.log)");
            chat_say(msg);
            return;
        }
        if (!wp_set(id, key)) { chat_say("vgun: слишком много подмен, снимите лишние: /vgun <id> off"); return; }
        b_init(&b, msg, sizeof msg);
        b_s(&b, "vgun: игрок "); b_int(&b, id); b_s(&b, " — в руках "); b_s(&b, nm); b_s(&b, " (только у вас)");
        if (!wp_in_stream(id)) b_s(&b, ". Сейчас его нет рядом: оружие появится, когда он войдёт в зону видимости");
        b_s(&b, ". Убрать: /vgun "); b_int(&b, id); b_s(&b, " off");
        chat_say(msg);
        return;
    }
    wp_usage();
}

/* ───────────── самозапуск ───────────── */
static int g_wp_modId = -1;
static void* wp_main(void* arg) {
    (void)arg;
    wp_table_reset();                                        /* ячейки «свободно» — до того, как заглушка впервые увидит таблицу */
    if (!hm_cmd_install("vgun", native_vgun_cmd)) return NULL;
    hm_mod_ready(g_wp_modId);
    dlog("vgun: мод загружен, команда /vgun работает (перехват ставится при первой подмене)");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void wp_ctor(void) {
    unsigned long t = 0;
    g_wp_modId = hm_mod_register("vgun");
    pthread_create(&t, NULL, wp_main, NULL);
}
#endif
