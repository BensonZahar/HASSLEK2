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
 *   • Поэтому подменяем сам ЧТЕНИЕ байта: в RemotePlayer.Update одна инструкция «LDRB w1,[xN,#CurrentWeapon]» заменяется на переход
 *     в маленькую заглушку (в анонимной странице рядом с кодом игры). Заглушка читает байт как раньше, зовёт нашу функцию wp_hook(игрок, байт)
 *     и кладёт в w1 то, что она вернула. Если у игрока (Actor.m_ID) есть подмена — вернётся наш байт, иначе серверный.
 *     Всё остальное (создание префаба, анимация стойки, загрузка ресурсов) игра делает сама, в главном потоке, своим обычным путём.
 *   • Место патча не зашито адресом: функцию находим по имени (il2cpp_class_get_method_from_name), смещение CurrentWeapon берём из метаданных,
 *     а саму инструкцию ищем по узору LDRB + LDR + BL. Нашлось не ровно одно место — ничего не трогаем.
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

/* ───────────── таблица подмен: ID игрока → байт оружия (читается из главного потока внутри заглушки, пишется командой) ───────────── */
#define WP_MAX 64
static volatile int g_wp_id[WP_MAX];                      /* -1 — ячейка свободна (до установки перехвата заполняется в wp_table_reset) */
static volatile int g_wp_byte[WP_MAX];
static volatile unsigned g_wp_hits;                       /* сколько раз перехват подменил байт — по нему видно, что он живой */
static int g_wp_offId = 0x40;                             /* смещение Actor.m_ID; при установке берём из метаданных */
static void wp_table_reset(void) { int i; for (i = 0; i < WP_MAX; i++) __atomic_store_n(&g_wp_id[i], -1, __ATOMIC_RELEASE); }

/* Зовётся из заглушки (главный поток, внутри RemotePlayer.Update). Никаких вызовов IL2CPP и выделения памяти — только чтение таблицы. */
__attribute__((noinline, used))
static unsigned wp_hook(void* player, unsigned server_byte) {
    int i, id;
    if (!player) return server_byte;
    id = (int)*(volatile u16*)((char*)player + g_wp_offId);
    for (i = 0; i < WP_MAX; i++) {
        if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) == id) {
            __atomic_fetch_add(&g_wp_hits, 1, __ATOMIC_RELAXED);
            return (unsigned)__atomic_load_n(&g_wp_byte[i], __ATOMIC_RELAXED) & 0xFFu;
        }
    }
    return server_byte;
}

/* ───────────── чистые помощники (без обращения к памяти игры; проверяются отдельно на настоящем коде libil2cpp) ───────────── */
/* слово — «LDRB w1,[xN,#off]» (беззнаковое смещение, любой N): N кладётся в *rn */
static int wp_is_ldrb(u32 w, unsigned off, int* rn) {
    if ((w & 0xFFFFFC1Fu) != (0x39400001u | (off << 10))) return 0;
    *rn = (int)((w >> 5) & 31u);
    return 1;
}
/* Узор: LDRB w1,[xN,#off]; LDR x?,[x?,#imm]; BL — загрузка байта и сразу вызов Dictionary.get_Item(ToID, байт). Возврат: индекс слова,
   -1 — не найдено, -2 — найдено больше одного места (патчить нельзя). */
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
static u32 wp_b(uptr from, uptr to) { return 0x14000000u | ((u32)(((long)to - (long)from) >> 2) & 0x03FFFFFFu); }
static int wp_in_range(uptr a, uptr b) { long d = (long)b - (long)a; return d > -0x7E00000L && d < 0x7E00000L; }   /* B достаёт на ±128 МБ, берём с запасом */

#define WP_VENEER_WORDS 14
/* Заглушка (arm64). Сохраняет x0 и x8 (они живы до следующего BL), читает байт как исходная инструкция, зовёт wp_hook(xN, байт),
   результат кладёт в w1, возвращает x0/x8 и прыгает на инструкцию после места патча.
     0  stp x0, x8, [sp, #-32]!        1  str x30, [sp, #16]
     2  <исходная LDRB w1,[xN,#off]>   3  mov x0, xN
     4  ldr x16, =wp_hook (слово 12)   5  blr x16
     6  mov w1, w0                     7  ldr x30, [sp, #16]
     8  ldp x0, x8, [sp], #32          9  b site+4
    10-11 nop                          12-13 адрес wp_hook */
static void wp_make_veneer(u32* v, uptr vaddr, uptr site, u32 orig, int rn, uptr hook) {
    v[0] = 0xA9BE23E0u;
    v[1] = 0xF9000BFEu;
    v[2] = orig;
    v[3] = 0xAA0003E0u | ((u32)rn << 16);
    v[4] = 0x58000110u;
    v[5] = 0xD63F0200u;
    v[6] = 0x2A0003E1u;
    v[7] = 0xF9400BFEu;
    v[8] = 0xA8C223E0u;
    v[9] = wp_b(vaddr + 9 * 4, site + 4);
    v[10] = 0xD503201Fu;
    v[11] = 0xD503201Fu;
    v[12] = (u32)(hook & 0xFFFFFFFFu);
    v[13] = (u32)(((unsigned long long)hook) >> 32);
}

/* ───────────── установка перехвата (только arm64) ───────────── */
#ifdef WP_ARM64
static void* g_wp_veneer;                                  /* страница с заглушкой (после снятия перехвата остаётся — вдруг поток ещё внутри) */
static uptr g_wp_site;                                     /* адрес пропатченной инструкции */
static u32 g_wp_orig;                                      /* её исходное слово */
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
/* Записать одно слово в код игры. Окно с правом записи короткое (одна инструкция), права на страницу возвращаются как были (r-x).
   Запасной путь — запись в /proc/self/mem, если mprotect запрещён. */
static int wp_write_code(uptr addr, u32 w) {
    uptr page = addr & ~(uptr)4095;
    if (mprotect((void*)page, 4096, 7) == 0) {              /* R|W|X: страница живая, другие потоки в ней исполняют код, снимать X нельзя */
        *(volatile u32*)addr = w;
        wp_flush((void*)addr, 4);
        mprotect((void*)page, 4096, 5);                     /* R|X */
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, &w, 4, (long)addr); close(fd); }
        if (r == 4) { wp_flush((void*)addr, 4); return 1; }
    }
    return 0;
}
/* Анонимная страница в пределах досягаемости перехода B от места патча. Подсказка-адрес идёт вокруг места патча с шагом 64 КБ. */
static void* wp_alloc_near(uptr site) {
    uptr base = site & ~(uptr)0xFFFF; long k;
    for (k = 1; k <= 2000; k++) {
        int dir;
        for (dir = 0; dir < 2; dir++) {
            uptr hint = dir ? base + (uptr)k * 0x10000u : base - (uptr)k * 0x10000u;
            void* r = mmap((void*)hint, 4096, 3, 0x22, -1, 0);       /* R|W, MAP_PRIVATE|MAP_ANONYMOUS */
            if (r == (void*)-1) continue;
            if (wp_in_range(site, (uptr)r)) return r;
            munmap(r, 4096);
        }
    }
    return NULL;
}
static int wp_install(void) {
    void *kls, *m, *sf, *sc, *cw, *idf; uptr fn, site; u32 code[256]; int off, shift, rn = 0, idx, i;
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
    idx = wp_scan(code, 256, (unsigned)off, &rn);
    if (idx == -2) return wp_fail("в Update несколько подходящих мест — не рискую");
    if (idx < 0) return wp_fail("в Update не нашёл чтение CurrentWeapon (другая сборка игры?)");
    if (rn == 1 || rn >= 30) return wp_fail("неожиданный регистр с игроком");
    site = fn + (uptr)idx * 4;
    dlog_i("vgun: место патча, байт от начала Update: ", idx * 4);
    if (g_wp_veneer && g_wp_site == site && g_wp_orig == code[idx]) {
        /* повторная установка после /vgun unhook: заглушка уже стоит и осталась прежней */
    } else {
        u32 v[WP_VENEER_WORDS];
        void* page = wp_alloc_near(site);
        if (!page) return wp_fail("нет свободной страницы рядом с кодом игры");
        wp_make_veneer(v, (uptr)page, site, code[idx], rn, (uptr)wp_hook);
        for (i = 0; i < WP_VENEER_WORDS; i++) ((u32*)page)[i] = v[i];
        wp_flush(page, sizeof v);
        if (mprotect(page, 4096, 5) != 0) { munmap(page, 4096); return wp_fail("не удалось сделать страницу заглушки исполняемой"); }
        g_wp_veneer = page; g_wp_site = site; g_wp_orig = code[idx];
    }
    if (!wp_write_code(site, wp_b(site, (uptr)g_wp_veneer))) return wp_fail("запись в код игры запрещена системой");
    g_wp_state = 1;
    dlog("vgun: перехват RemotePlayer.Update установлен");
    return 1;
}
static void wp_unhook(void) {
    if (g_wp_state != 1) return;
    if (wp_write_code(g_wp_site, g_wp_orig)) { g_wp_state = 0; dlog("vgun: перехват снят, код игры восстановлен"); }
    else dlog("vgun: не удалось вернуть исходную инструкцию");
}
#else
static int wp_install(void) { return wp_fail("поддерживается только arm64"); }
static void wp_unhook(void) {}
#endif

/* ───────────── оружия, которые знает игра: WeaponConvertTable.ToID (Dictionary<int,string>) ───────────── */
typedef struct { int key; char name[28]; } WpItem;
static WpItem g_wp_items[64];
static int g_wp_nitems;
static void wp_item_cb(void* key, void* val, void* ctx) {
    WpItem* it; int k, n;
    (void)ctx;
    if (g_wp_nitems >= 64 || !val || !hm_obj_ok(val)) return;
    k = (int)((uptr)key & 0xFFFFFFFFu);                      /* ключ int лежит в младших 4 байтах прочитанного слова */
    n = A.string_length(val);
    if (n <= 0 || n > 27 || k < 0 || k > 255) return;
    it = &g_wp_items[g_wp_nitems++];
    it->key = k;
    utf16_to_utf8(A.string_chars(val), n, it->name, sizeof it->name);
}
static int wp_load_items(void) {
    void *kls, *f, *dict = NULL; int i, j;
    g_wp_nitems = 0;
    kls = A.class_from_name(g_img, "Hassle.Items", "WeaponConvertTable");
    f = kls ? A.class_get_field_from_name(kls, "ToID") : NULL;
    if (!f) return 0;
    A.field_static_get_value(f, &dict);
    if (!dict) return 0;
    dict_each(dict, wp_item_cb, NULL);
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
static int wp_count(void) { int i, n = 0; for (i = 0; i < WP_MAX; i++) if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) >= 0) n++; return n; }
static void wp_clear_id(int id) { int s = wp_slot(id); if (s >= 0) __atomic_store_n(&g_wp_id[s], -1, __ATOMIC_RELEASE); }
static void wp_clear_all(void) { wp_table_reset(); }
static int wp_set(int id, int key) {                         /* 1 — готово, 0 — таблица полна */
    int s = wp_slot(id), i;
    if (s < 0) { for (i = 0; i < WP_MAX; i++) if (__atomic_load_n(&g_wp_id[i], __ATOMIC_ACQUIRE) < 0) { s = i; break; } }
    if (s < 0) return 0;
    __atomic_store_n(&g_wp_byte[s], key, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wp_id[s], id, __ATOMIC_RELEASE);     /* ID публикуется последним: заглушка не увидит ячейку без байта */
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
        if (id < 0) continue;
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
    if (wp_is(tok[0], "unhook", NULL)) { wp_clear_all(); wp_unhook(); chat_say(g_wp_state == 1 ? "vgun: не удалось снять перехват, см. konstt.log" : "vgun: перехват снят, подмены сброшены"); return; }
    if (wp_is(tok[0], "off", "clear") || wp_is(tok[0], "reset", "stop")) { wp_clear_all(); chat_say("vgun: все подмены сняты, у игроков оружие как на сервере"); return; }
    if (wp_all_digits(tok[0])) {
        int id = wp_num(tok[0]), key, me; const char* nm = NULL;
        if (id > 65535) { chat_say("vgun: такой ID не бывает"); return; }
        if (nt == 1) {                                       /* что подменено у игрока */
            int s = wp_slot(id);
            b_init(&b, msg, sizeof msg);
            if (s < 0) { b_s(&b, "vgun: у игрока "); b_int(&b, id); b_s(&b, " подмены нет"); }
            else { wp_load_items(); nm = wp_name_of(__atomic_load_n(&g_wp_byte[s], __ATOMIC_RELAXED)); b_s(&b, "vgun: у игрока "); b_int(&b, id); b_s(&b, " подменено оружие: "); b_s(&b, nm ? nm : "?"); }
            chat_say(msg);
            return;
        }
        if (wp_is(tok[1], "off", "clear") || wp_is(tok[1], "del", "none")) {
            b_init(&b, msg, sizeof msg);
            if (wp_slot(id) < 0) { b_s(&b, "vgun: у игрока "); b_int(&b, id); b_s(&b, " подмены и не было"); }
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
