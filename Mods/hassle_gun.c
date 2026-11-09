/*
 * hassle_gun.c — необязательный модуль «/gun»: выдать оружие своему персонажу (тестовому NPC на нашем клиенте), чтобы проверить тир
 * и другие системы со стрельбой. Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите файл и пересоберите бандл.
 *
 * Команды в чате (нативные, как /die):
 *   /gun <оружие> [патроны]   оружие — номер SA-MP (30) или имя (ak47, m4, deagle, katana …); патронов по умолчанию 200
 *   /gun list                 какие оружия знает игра (номер=имя)
 *
 * Как это работает (по dump.cs и разбору libil2cpp.so этого APK)
 *   • Серверный RPC GivePlayerWeapon (RVA 0x1347998) читает два Int32 (номер оружия, патроны), переводит номер в строковый ID через
 *     WeaponConvertTable.ToID и хвостовым вызовом отдаёт EntityItems.Add(string id, int count) (RVA 0x12F825C) — Player.Items (+0xC8).
 *     Мы делаем ровно то же самое у своего LocalPlayer, затем EntityItems.SetActiveWeapon(id) (RVA 0x12F88B8), чтобы оружие оказалось в руках.
 *   • Дальше игра сама: OnWeaponsChanged у LocalPlayer шлёт серверу пакет оружия, стрельба идёт обычным путём. Принимает ли сервер выстрелы
 *     из оружия, которого он не выдавал, — вопрос сервера: это и есть то, что проверяет тест.
 *   • Команда вызывается игрой в главном потоке. Перегрузок Add две (ItemInfo, int) и (string, int) — берём ту, где первый параметр string.
 */
#pragma once
#include "hassle_core.c"

static int g_gun_modId = -1;

/* ───────────── перегрузка по типу первого параметра (в ядре такого нет: find_method смотрит только на число параметров) ───────────── */
static struct {
    void* (*class_get_methods)(void*, void**);
    const char* (*method_get_name)(void*);
    unsigned (*method_get_param_count)(void*);
    void* (*method_get_param)(void*, unsigned);
} GNA;
static int g_gun_apiTried;
static int gun_api(void) {
    if (!g_gun_apiTried) {
        g_gun_apiTried = 1;
        if (g_il2cpp) {
            *(void**)&GNA.class_get_methods = dlsym(g_il2cpp, "il2cpp_class_get_methods");
            *(void**)&GNA.method_get_name = dlsym(g_il2cpp, "il2cpp_method_get_name");
            *(void**)&GNA.method_get_param_count = dlsym(g_il2cpp, "il2cpp_method_get_param_count");
            *(void**)&GNA.method_get_param = dlsym(g_il2cpp, "il2cpp_method_get_param");
        }
    }
    return GNA.class_get_methods && GNA.method_get_name && GNA.method_get_param_count && GNA.method_get_param;
}
static void* gun_find_method_str(void* k, const char* name, int argc) {      /* метод, у которого первый параметр string */
    if (!gun_api()) return NULL;
    for (; k; k = A.class_get_parent(k)) {
        void* it = NULL; void* m;
        while ((m = GNA.class_get_methods(k, &it))) {
            const char* mn = GNA.method_get_name(m); void* t; const char* tn;
            if (!mn || !seq(mn, name) || (int)GNA.method_get_param_count(m) != argc) continue;
            t = GNA.method_get_param(m, 0);
            tn = t ? A.type_get_name(t) : NULL;
            if (tn && ends_with(tn, "String")) return m;
        }
    }
    return NULL;
}

/* ───────────── оружия, которые знает игра: WeaponConvertTable.ToID (Dictionary<int,string>), опрос ключей через get_Item ───────────── */
typedef struct { int key; char name[28]; } GunItem;
static GunItem g_gun_items[64];
static int g_gun_n;
static int gun_load(void) {
    void *kls, *f, *dict = NULL, *gi; int k;
    g_gun_n = 0;
    kls = A.class_from_name(g_img, "Hassle.Items", "WeaponConvertTable");
    f = kls ? A.class_get_field_from_name(kls, "ToID") : NULL;
    if (!f) { dlog("gun: нет WeaponConvertTable.ToID"); return 0; }
    A.field_static_get_value(f, &dict);
    if (!dict || !hm_obj_ok(dict)) { dlog("gun: WeaponConvertTable.ToID пока пуст"); return 0; }
    gi = find_method(A.object_get_class(dict), "get_Item", 1);
    if (!gi) { dlog("gun: у словаря оружия нет get_Item"); return 0; }
    for (k = 0; k < 64 && g_gun_n < 64; k++) {              /* нет ключа — игра бросает исключение, runtime_invoke его ловит */
        void *args[1], *r, *exc = NULL; int key = k; int n;
        args[0] = &key;
        r = A.runtime_invoke(gi, dict, args, &exc);
        if (exc || !r || !hm_obj_ok(r)) continue;
        n = A.string_length(r);
        if (n <= 0 || n > 27) continue;
        g_gun_items[g_gun_n].key = k;
        utf16_to_utf8(A.string_chars(r), n, g_gun_items[g_gun_n].name, sizeof g_gun_items[g_gun_n].name);
        g_gun_n++;
    }
    return g_gun_n;
}
static void gun_norm(const char* s, char* out, int cap) {     /* нижний регистр, только буквы и цифры: «AK-47» → «ak47» */
    int o = 0;
    for (; *s && o + 1 < cap; s++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = c;
    }
    out[o] = 0;
}
static int gun_all_digits(const char* s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static int gun_num(const char* s) { int v = 0; while (*s >= '0' && *s <= '9' && v < 100000) v = v * 10 + (*s++ - '0'); return v; }
static const char* g_gun_alias[][2] = {
    { "deagle", "deserteagle" }, { "colt", "colt45" }, { "pistol", "colt45" }, { "spas", "combatshotgun" }, { "spas12", "combatshotgun" },
    { "sniper", "sniperrifle" }, { "ak", "ak47" }, { "m4a1", "m4" }, { "brass", "brassknuckles" }, { "knuckles", "brassknuckles" },
};
/* индекс в g_gun_items по тексту (номер SA-MP или имя); -1 — не нашли или неоднозначно */
static int gun_find(const char* tok) {
    char t[32], n[32]; int i, hit = -1, cnt = 0;
    gun_norm(tok, t, sizeof t);
    if (!t[0]) return -1;
    if (gun_all_digits(tok)) {
        int v = gun_num(tok);
        for (i = 0; i < g_gun_n; i++) if (g_gun_items[i].key == v) return i;
        return -1;
    }
    for (i = 0; i < (int)(sizeof g_gun_alias / sizeof g_gun_alias[0]); i++) if (seq(t, g_gun_alias[i][0])) { gun_norm(g_gun_alias[i][1], t, sizeof t); break; }
    for (i = 0; i < g_gun_n; i++) { gun_norm(g_gun_items[i].name, n, sizeof n); if (seq(n, t)) return i; }
    for (i = 0; i < g_gun_n; i++) { gun_norm(g_gun_items[i].name, n, sizeof n); if (sprefix(n, t)) { hit = i; cnt++; } }   /* единственное имя, начинающееся с введённого */
    return cnt == 1 ? hit : -1;
}

/* аргументы команды как строки (саму «/gun» пропускаем: в массив она может входить, а может и нет) */
static int gun_tokens(void* arr, char tok[][40], int max) {
    long len; int i, n = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len && n < max; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[40]; int k; const u16* ch;
        if (!s) continue;
        k = A.string_length(s); ch = A.string_chars(s);
        if (k <= 0 || k > 36 || !ch) continue;
        utf16_to_utf8(ch, k, tmp, sizeof tmp);
        if (seq(tmp, "/gun") || seq(tmp, "gun")) continue;
        { int j; for (j = 0; tmp[j] && j < 39; j++) tok[n][j] = tmp[j]; tok[n][j] = 0; }
        n++;
    }
    return n;
}

/* Items.Add(id, ammo) + Items.SetActiveWeapon(id) у своего игрока. 1 — оружие выдано */
static int gun_give(void* lp, const char* id, int ammo) {
    void *items, *mAdd, *mSet, *exc = NULL, *args[2]; char et[240];
    items = ref_field_s(lp, "Items");
    if (!items || !hm_obj_ok(items)) { dlog("gun: Player.Items не найден"); return 0; }
    mAdd = gun_find_method_str(A.object_get_class(items), "Add", 2);
    if (!mAdd) { dlog("gun: EntityItems.Add(string,int) не найден"); return 0; }
    args[0] = A.string_new(id); args[1] = &ammo;
    A.runtime_invoke(mAdd, items, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("gun: Items.Add бросил исключение: ", et); return 0; }
    mSet = find_method(A.object_get_class(items), "SetActiveWeapon", 1);
    if (mSet) {
        void* a1[1]; a1[0] = A.string_new(id);
        A.runtime_invoke(mSet, items, a1, &exc);
        if (exc) { exc_text(exc, et, sizeof et); dlog2("gun: SetActiveWeapon бросил исключение: ", et); }
    }
    return 1;
}

/* Вызывается игрой в главном потоке при вводе /gun */
static void native_gun_cmd(void* self, void* args, void* mi) {
    char tok[3][40]; int n, i, ix, ammo = 200; void* lp; char msg[400]; Buf b;
    (void)self; (void)mi;
    ensure_attached();
    n = gun_tokens(args, tok, 3);
    if (!gun_load()) { chat_say("/gun: список оружия игры недоступен (подробности в konstt.log)"); return; }
    if (n == 0) { chat_usage("/gun <номер или имя> [патроны] — выдать оружие себе; /gun list — какие бывают"); return; }
    if (seq(tok[0], "list")) {
        b_init(&b, msg, sizeof msg);
        b_s(&b, "/gun: ");
        for (i = 0; i < g_gun_n; i++) { if (i) b_s(&b, ", "); b_int(&b, g_gun_items[i].key); b_ch(&b, '='); b_s(&b, g_gun_items[i].name); }
        chat_say(msg);
        return;
    }
    ix = gun_find(tok[0]);
    if (ix < 0) { chat_usage("/gun: такого оружия нет или имя неоднозначно. /gun list — список"); return; }
    if (n >= 2 && gun_all_digits(tok[1])) { ammo = gun_num(tok[1]); if (ammo > 9999) ammo = 9999; }
    lp = local_player();
    if (!lp || !hm_obj_ok(lp)) { chat_say("/gun: персонаж недоступен (ещё не в игре?)"); dlog("gun: local_player не найден"); return; }
    if (!gun_give(lp, g_gun_items[ix].name, ammo)) { chat_say("/gun: не получилось, подробности в konstt.log"); return; }
    b_init(&b, msg, sizeof msg);
    b_s(&b, "gun: выдано "); b_s(&b, g_gun_items[ix].name); b_s(&b, " ("); b_int(&b, g_gun_items[ix].key); b_s(&b, "), патронов "); b_int(&b, ammo);
    dlog(msg);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "/gun: "); b_s(&b, g_gun_items[ix].name); b_s(&b, ", патронов "); b_int(&b, ammo);
    chat_say(msg);
}

static void* gun_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("gun", native_gun_cmd)) return NULL;        /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /gun */
    hm_mod_ready(g_gun_modId);
    dlog("gun: мод загружен, команда /gun работает");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void gun_ctor(void) {
    unsigned long t = 0;
    g_gun_modId = hm_mod_register("gun");
    pthread_create(&t, NULL, gun_main, NULL);
}
#endif
