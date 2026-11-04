/*
 * hassle_ids.c — необязательный мод «/ids»: показывает ID прямо над объектами мира — NPC, машинами, объектами карты и пикапами.
 * Только на вашем клиенте, сервер ничего не знает. Нужен, чтобы быстро увидеть «что это за NPC / объект» и взять его ID для
 * /vtune go, /air to, серверных команд и т.п.
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_ids.c и пересоберите бандл — больше ничего править не нужно.
 *
 * Команды в чате:
 *   /ids                 включить (все виды, радиус 50 м) ↔ выключить
 *   /ids on | off        включить / выключить (off убирает все подписи)
 *   /ids npc car obj pick   показывать ТОЛЬКО перечисленное (любые из: npc, car, obj, pick; можно по-русски: нпс, авто, объект, пикап), например «/ids obj» или «/ids npc car»
 *   /ids all             показывать всё
 *   /ids r 80            радиус в метрах (5…300); можно просто «/ids 80»
 *   /ids list            список ближайших в чат (работает и при выключенных подписях)
 *   /ids status          состояние;  /ids help — подсказка
 *
 * Что вы видите: белая подпись над точкой объекта — «NPC 12», «CAR 34», «OBJ 56 [1337]» (в скобках номер модели объекта), «PICK 7».
 *   Номера — те же, что в серверном коде: Actor.m_ID (NPC), BaseVehicle.ID (машина, ID ЭКЗЕМПЛЯРА, не модели), MapObjectPlace.ID (объект),
 *   Pickup.m_ID (пикап). Всегда рисуются не более 48 ближайших (IDS_MAXL), чтобы не завалить экран.
 *
 * Как это работает (по дампу IL2CPP от 20.10.2026)
 *   • Позиции: NPC — Pools.Actors (Actor : Entity → Transform.position), машины — Pools.Vehicles (ключи словаря = ID, сам BaseVehicle берём игровым
 *     VehiclePool.Get(int), как /vtune), объекты — Pools.Objects (MapObjectPlace : WorldItem → get_Position()), пикапы — Pools.Pickups (Pickup : MonoBehaviour).
 *   • Подписи рисует сама игра: Pools.Text3DLables.Create(CreateText3DParams) — тот же путь, каким сервер создаёт 3D-тексты (CreateText3D). Мы кладём в
 *     структуру ID подписи (IDS_BASE…IDS_BASE+47 = 64000…64047, серверные подписи занимают малые номера), белый цвет, мировую позицию, дальность, TestLOS=0
 *     (видно сквозь стены), PlayerID/VehicleID = 0xFFFF («ни к кому не привязана») и текст. Смещения полей берём из метаданных игры по имени, как /tpl.
 *   • Подпись стоит на месте, поэтому раз в ~0,7 с мод пересоздаёт подпись тех, кто сдвинулся больше чем на 1,5 м (Remove + Create), и убирает подписи
 *     ушедших за радиус. Всё Unity — только в главном потоке: фоновый поток лишь шлёт Post в UnitySynchronizationContext (как /air).
 *   • Чужие подписи не трогаем: чистим только свой диапазон ID (в том числе «осиротевшие» после перезагрузки мода).
 *   • Если игра сбросила пулы (реконнект), подписи пропадают сами; мод на следующем тике создаст их заново.
 */
#pragma once
#include "hassle_core.c"

#define IDS_MAXL 48                      /* максимум подписей одновременно (и размер диапазона ID) */
#define IDS_BASE 64000                   /* первый ID подписи мода */
#define IDS_TICK_US 700000               /* период обновления */
#define IDS_NEWPERTICK 16                /* не больше стольких новых подписей за тик (чтобы не подвисало) */
#define IDS_MOVE 1.5f                    /* на сколько метров надо сдвинуться, чтобы подпись переставили */
#define IDS_NPC 1
#define IDS_CAR 2
#define IDS_OBJ 4
#define IDS_PICK 8
#define IDS_ALL 15

typedef struct { int kind, id, model; float d; float p[3]; } IdsC;
typedef struct { int used, kind, id; float p[3]; } IdsSlot;

static IdsC g_ids_c[IDS_MAXL]; static int g_ids_nc; static float g_ids_ref[3];
static IdsSlot g_ids_slot[IDS_MAXL];
static int g_ids_present[IDS_MAXL];
static volatile int g_ids_on, g_ids_pending; static long long g_ids_pendT;
static int g_ids_mask = IDS_ALL, g_ids_thread, g_ids_modId = -1, g_ids_errs, g_ids_logged;
static float g_ids_radius = 50.0f;
static int g_ids_scanTotal[4];           /* сколько записей в каждом пуле при последнем скане: npc, car, obj, pick (-1 — пул не прочитался) */
/* раскладка структуры CreateText3DParams (смещения без заголовка) */
static int g_ids_layoutOk, g_ids_oID, g_ids_oCol, g_ids_oPos, g_ids_oDist, g_ids_oLos, g_ids_oPl, g_ids_oVeh, g_ids_oTxt, g_ids_ssize;
static void* g_ids_kParams;

static long long ids_now_ms(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return (long long)ts[0] * 1000LL + ts[1] / 1000000LL; }
static int ids_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
static float ids_dist(const float* a, const float* b) { double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz); }
static void ids_b_xyz(Buf* b, const float* p) { b_fix(b, p[0], 1); b_ch(b, ' '); b_fix(b, p[1], 1); b_ch(b, ' '); b_fix(b, p[2], 1); }
static int ids_num(const char* t, float* out) {          /* «12», «12.5», «12,5» (без знака) */
    float v = 0.0f, f = 0.1f; int k = 0, dot = 0, digits = 0;
    for (; t[k]; k++) {
        if (t[k] >= '0' && t[k] <= '9') {
            digits++;
            if (!dot) v = v * 10.0f + (float)(t[k] - '0');
            else { v += f * (float)(t[k] - '0'); f *= 0.1f; }
        } else if ((t[k] == '.' || t[k] == ',') && !dot) dot = 1;
        else return 0;
    }
    if (!digits) return 0;
    *out = v; return 1;
}
static const char* ids_kname(int kind) { return kind == IDS_NPC ? "NPC" : kind == IDS_CAR ? "CAR" : kind == IDS_OBJ ? "OBJ" : "PICK"; }
static void ids_err(const char* what, void* exc) {       /* первые несколько исключений — в журнал */
    char et[200];
    if (g_ids_errs >= 6) return;
    g_ids_errs++;
    exc_text(exc, et, sizeof et);
    dlog2(what, et);
}

/* ───────────── доступ к игре ───────────── */
static void* ids_pool(const char* field) {                          /* Pools.<field> */
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}
static void* ids_obj(void* obj, const char* name) {                 /* метод без аргументов, вернувший ссылочный объект */
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int ids_vec(void* obj, const char* name, float* out) {       /* метод без аргументов, вернувший Vector3 (в боксе: 3 float после заголовка) */
    void *m, *exc = NULL, *r; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!ids_finite(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
static int ids_ent_pos(void* ent, float* out) {                     /* Entity.Transform.position (у MonoBehaviour — transform.position) */
    void* tr = ids_obj(ent, "get_Transform");
    if (!tr) tr = ids_obj(ent, "get_transform");
    return tr && ids_vec(tr, "get_position", out);
}
static int ids_myref(float* out) {                                  /* ваша позиция (центр поиска) */
    void* me = local_player();
    if (!me || !hm_obj_ok(me)) return 0;
    if (ids_ent_pos(me, out)) return 1;
    return read_pos(me, out);
}

/* ───────────── сбор кандидатов ───────────── */
static void ids_add(int kind, int id, const float* p, int model) {
    float d; int i, j;
    d = ids_dist(p, g_ids_ref);
    if (d > g_ids_radius) return;
    for (i = 0; i < g_ids_nc && g_ids_c[i].d <= d; i++) {}
    if (i >= IDS_MAXL) return;
    for (j = (g_ids_nc < IDS_MAXL ? g_ids_nc : IDS_MAXL - 1); j > i; j--) g_ids_c[j] = g_ids_c[j - 1];
    g_ids_c[i].kind = kind; g_ids_c[i].id = id; g_ids_c[i].model = model; g_ids_c[i].d = d;
    g_ids_c[i].p[0] = p[0]; g_ids_c[i].p[1] = p[1]; g_ids_c[i].p[2] = p[2];
    if (g_ids_nc < IDS_MAXL) g_ids_nc++;
}
static void ids_npc_cb(void* it, void* ctx) {
    float p[3]; (void)ctx;
    if (!hm_obj_ok(it) || !ids_ent_pos(it, p)) return;
    ids_add(IDS_NPC, (int)num_field(it, "m_ID", NULL), p, 0);
}
static void ids_obj_cb(void* it, void* ctx) {
    float p[3]; (void)ctx;
    if (!hm_obj_ok(it) || !ids_vec(it, "get_Position", p)) return;
    ids_add(IDS_OBJ, (int)num_field(it, "ID", NULL), p, (int)num_field(it, "Model", NULL));
}
static void ids_pick_cb(void* it, void* ctx) {
    float p[3]; (void)ctx;
    if (!hm_obj_ok(it) || !ids_ent_pos(it, p)) return;
    ids_add(IDS_PICK, int_field(it, "m_ID", NULL, NULL, -1), p, 0);
}
/* значения словаря пула (BasePool<T>.m_Items). -1 — не прочиталось, иначе число записей */
static int ids_scan_pool(const char* field, item_cb cb) {
    void *pool = ids_pool(field), *dict, *arr; int total;
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (!arr) return -1;
    total = int_field(dict, "_count", "count", NULL, -1);
    walk_struct_array(arr, total, "value", cb, NULL);
    return total;
}
/* ключи (int) словаря пула — у Pools.Vehicles значение это PoolObject, а нужен BaseVehicle, его даёт VehiclePool.Get(id) */
static int ids_dict_int_keys(void* dict, int* out, int max) {
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
        if (fh && *(int*)(e + offh) < 0) continue;                  /* свободная запись словаря */
        out[n++] = *(int*)(e + offk);
    }
    return n;
}
static int ids_scan_cars(void) {
    void *pool = ids_pool("Vehicles"), *dict, *m; int keys[256], n, i;
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    if (!dict) return -1;
    m = find_method(A.object_get_class(pool), "Get", 1);
    if (!m) { if (!g_ids_errs) dlog("ids: у Pools.Vehicles нет Get(int)"); g_ids_errs++; return -1; }
    n = ids_dict_int_keys(dict, keys, 256);
    for (i = 0; i < n; i++) {
        void *args[1], *exc = NULL, *r; int id = keys[i]; float p[3];
        args[0] = &id;
        r = A.runtime_invoke(m, pool, args, &exc);
        if (exc) { ids_err("ids: VehiclePool.Get бросил исключение: ", exc); continue; }
        if (!r || !hm_obj_ok(r) || !ids_ent_pos(r, p)) continue;
        ids_add(IDS_CAR, id, p, 0);
    }
    return n;
}
static void ids_collect(int mask) {                                 /* заполняет g_ids_c ближайшими к g_ids_ref */
    int k;
    g_ids_nc = 0;
    for (k = 0; k < 4; k++) g_ids_scanTotal[k] = -2;                /* -2 — не сканировали (вид выключен) */
    if (mask & IDS_NPC) g_ids_scanTotal[0] = ids_scan_pool("Actors", ids_npc_cb);
    if (mask & IDS_CAR) g_ids_scanTotal[1] = ids_scan_cars();
    if (mask & IDS_OBJ) g_ids_scanTotal[2] = ids_scan_pool("Objects", ids_obj_cb);
    if (mask & IDS_PICK) g_ids_scanTotal[3] = ids_scan_pool("Pickups", ids_pick_cb);
}

/* ───────────── подписи: свои ID, создание, удаление ───────────── */
static int ids_lb_layout(void) {
    void *sc, *f; int shift;
    if (g_ids_layoutOk) return 1;
    if (!g_img) return 0;
    sc = A.class_from_name(g_img, "Hassle.Global.Pooling", "CreateText3DParams");
    if (!sc || !A.class_is_valuetype(sc)) { dlog("ids: нет структуры Hassle.Global.Pooling.CreateText3DParams"); return 0; }
    shift = struct_shift(sc);
#define IDS_OFF(var, nm) do { f = find_field(sc, nm); if (!f) { dlog2("ids: нет поля CreateText3DParams.", nm); return 0; } var = A.field_get_offset(f) - shift; } while (0)
    IDS_OFF(g_ids_oID, "ID"); IDS_OFF(g_ids_oCol, "Color"); IDS_OFF(g_ids_oPos, "Position"); IDS_OFF(g_ids_oDist, "Distance");
    IDS_OFF(g_ids_oLos, "TestLOS"); IDS_OFF(g_ids_oPl, "PlayerID"); IDS_OFF(g_ids_oVeh, "VehicleID"); IDS_OFF(g_ids_oTxt, "Text");
#undef IDS_OFF
    g_ids_ssize = A.class_instance_size ? A.class_instance_size(sc) - shift : 0x28;
    if (g_ids_ssize < 0x28) g_ids_ssize = 0x28;
    if (g_ids_ssize > 120 || g_ids_oTxt + 8 > 120 || g_ids_oTxt < 0 || g_ids_oPos < 0 || g_ids_oID < 0) { dlog_i("ids: странная раскладка CreateText3DParams, размер ", g_ids_ssize); return 0; }
    g_ids_kParams = sc;
    g_ids_layoutOk = 1;
    { char m[200]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "ids: CreateText3DParams размер "); b_int(&b, g_ids_ssize); b_s(&b, ", смещения ID/Color/Pos/Dist/LOS/Pl/Veh/Text: ");
      b_int(&b, g_ids_oID); b_ch(&b, '/'); b_int(&b, g_ids_oCol); b_ch(&b, '/'); b_int(&b, g_ids_oPos); b_ch(&b, '/'); b_int(&b, g_ids_oDist); b_ch(&b, '/');
      b_int(&b, g_ids_oLos); b_ch(&b, '/'); b_int(&b, g_ids_oPl); b_ch(&b, '/'); b_int(&b, g_ids_oVeh); b_ch(&b, '/'); b_int(&b, g_ids_oTxt); dlog(m); }
    return 1;
}
static int ids_lb_remove(int slot) {                                /* Pools.Text3DLables.Remove(ID) */
    void *pool = ids_pool("Text3DLables"), *m, *exc = NULL, *args[1]; int id = IDS_BASE + slot;
    if (!pool) return 0;
    m = find_method(A.object_get_class(pool), "Remove", 1);
    if (!m) { if (g_ids_errs < 6) { g_ids_errs++; dlog("ids: у пула подписей нет Remove(int)"); } return 0; }
    args[0] = &id;
    A.runtime_invoke(m, pool, args, &exc);
    if (exc) { ids_err("ids: Remove бросил исключение: ", exc); return 0; }
    return 1;
}
static int ids_lb_create(int slot, const IdsC* c) {                 /* Pools.Text3DLables.Create(CreateText3DParams) */
    void *pool = ids_pool("Text3DLables"), *m, *exc = NULL, *args[1]; unsigned long long raw[16]; char* buf = (char*)raw; char txt[48]; Buf b; int k;
    if (!pool || !ids_lb_layout()) return 0;
    m = find_method(A.object_get_class(pool), "Create", 1);
    if (!m) { if (g_ids_errs < 6) { g_ids_errs++; dlog("ids: у пула подписей нет Create(1 аргумент)"); } return 0; }
    for (k = 0; k < (int)sizeof raw; k++) buf[k] = 0;
    b_init(&b, txt, sizeof txt); b_s(&b, ids_kname(c->kind)); b_ch(&b, ' '); b_int(&b, c->id);
    if (c->kind == IDS_OBJ && c->model > 0) { b_s(&b, " ["); b_int(&b, c->model); b_ch(&b, ']'); }
    *(unsigned short*)(buf + g_ids_oID) = (unsigned short)(IDS_BASE + slot);
    *(int*)(buf + g_ids_oCol) = -1;                                 /* все биты — белый и при ARGB, и при RGBA */
    ((float*)(buf + g_ids_oPos))[0] = c->p[0]; ((float*)(buf + g_ids_oPos))[1] = c->p[1] + 0.6f; ((float*)(buf + g_ids_oPos))[2] = c->p[2];
    *(float*)(buf + g_ids_oDist) = g_ids_radius + 10.0f;
    *(unsigned char*)(buf + g_ids_oLos) = 0;                        /* без проверки видимости: видно и сквозь стены */
    *(unsigned short*)(buf + g_ids_oPl) = 0xFFFF; *(unsigned short*)(buf + g_ids_oVeh) = 0xFFFF;
    *(void**)(buf + g_ids_oTxt) = A.string_new(txt);
    args[0] = buf;
    A.runtime_invoke(m, pool, args, &exc);
    if (exc) { ids_err("ids: Create бросил исключение: ", exc); return 0; }
    if (g_ids_logged < 3) { g_ids_logged++; dlog2("ids: создана подпись ", txt); }
    return 1;
}
static void ids_lb_cb(void* it, void* ctx) {                        /* какие из НАШИХ ID сейчас есть в пуле подписей (читаем m_CreateParams.ID, как /tpl) */
    void *sf, *sc, *idf; char* sp; int shift, ioff, id; (void)ctx;
    if (!hm_obj_ok(it)) return;
    sf = find_field(A.object_get_class(it), "m_CreateParams");
    if (!sf) return;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return;
    idf = find_field(sc, "ID");
    if (!idf) return;
    sp = (char*)it + A.field_get_offset(sf);
    shift = struct_shift(sc);
    ioff = A.field_get_offset(idf) - shift;
    if (!hm_readable(sp + ioff, 2)) return;
    id = *(unsigned short*)(sp + ioff);
    if (id >= IDS_BASE && id < IDS_BASE + IDS_MAXL) g_ids_present[id - IDS_BASE] = 1;
}
static int ids_scan_present(void) {                                 /* 1 — пул подписей прочитан */
    int i;
    for (i = 0; i < IDS_MAXL; i++) g_ids_present[i] = 0;
    return ids_scan_pool("Text3DLables", ids_lb_cb) >= 0;
}
static int ids_any(void) { int i; for (i = 0; i < IDS_MAXL; i++) if (g_ids_slot[i].used) return 1; return 0; }
static void ids_clear_all(void) {                                   /* убрать все свои подписи (и осиротевшие) */
    int i;
    if (ids_scan_present()) for (i = 0; i < IDS_MAXL; i++) if (g_ids_present[i]) ids_lb_remove(i);
    for (i = 0; i < IDS_MAXL; i++) g_ids_slot[i].used = 0;
}

/* ───────────── тик (главный поток) ───────────── */
static void ids_tick(void) {
    int i, c, created = 0; int matched[IDS_MAXL];
    if (!g_safe || !il2cpp_light()) return;
    ensure_attached();
    if (!ids_pool("Text3DLables") || !ids_myref(g_ids_ref)) return;  /* не в игре / пулы сброшены — подписей нет, ждём */
    if (!ids_lb_layout()) { g_ids_on = 0; return; }
    if (!ids_scan_present()) return;
    for (i = 0; i < IDS_MAXL; i++) {                                 /* сверка с реальным пулом */
        if (g_ids_slot[i].used && !g_ids_present[i]) g_ids_slot[i].used = 0;       /* игра её уже убрала (реконнект) */
        else if (!g_ids_slot[i].used && g_ids_present[i]) ids_lb_remove(i);        /* осиротевшая */
    }
    ids_collect(g_ids_mask);
    for (c = 0; c < g_ids_nc; c++) matched[c] = 0;
    for (i = 0; i < IDS_MAXL; i++) {
        IdsSlot* s = &g_ids_slot[i]; int hit = -1;
        if (!s->used) continue;
        for (c = 0; c < g_ids_nc; c++) if (g_ids_c[c].kind == s->kind && g_ids_c[c].id == s->id) { hit = c; break; }
        if (hit < 0) { ids_lb_remove(i); s->used = 0; continue; }       /* ушёл за радиус / пропал */
        matched[hit] = 1;
        if (ids_dist(s->p, g_ids_c[hit].p) > IDS_MOVE) {                /* сдвинулся — переставить подпись */
            ids_lb_remove(i);
            if (ids_lb_create(i, &g_ids_c[hit])) { s->p[0] = g_ids_c[hit].p[0]; s->p[1] = g_ids_c[hit].p[1]; s->p[2] = g_ids_c[hit].p[2]; }
            else s->used = 0;
        }
    }
    for (c = 0; c < g_ids_nc && created < IDS_NEWPERTICK; c++) {
        int fs = -1;
        if (matched[c]) continue;
        for (i = 0; i < IDS_MAXL; i++) if (!g_ids_slot[i].used) { fs = i; break; }
        if (fs < 0) break;
        if (ids_lb_create(fs, &g_ids_c[c])) {
            g_ids_slot[fs].used = 1; g_ids_slot[fs].kind = g_ids_c[c].kind; g_ids_slot[fs].id = g_ids_c[c].id;
            g_ids_slot[fs].p[0] = g_ids_c[c].p[0]; g_ids_slot[fs].p[1] = g_ids_c[c].p[1]; g_ids_slot[fs].p[2] = g_ids_c[c].p[2];
            created++;
        }
    }
}

/* ───────────── фоновый поток: Post в главный ───────────── */
static void native_ids_cmd(void* self, void* args, void* mi);
static void ids_post(void) {
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { g_ids_pending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_ids_cmd);
    st = A.string_new("ids-tick");
    if (!cb || !st) { g_ids_pending = 0; dlog("ids: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("ids: Post бросил исключение: ", et); g_ids_pending = 0; }
}
static void* ids_worker(void* arg) {
    (void)arg;
    for (;;) {
        if (!__atomic_load_n(&g_ids_on, __ATOMIC_ACQUIRE)) { usleep(200000); continue; }
        if (!g_ids_pending || ids_now_ms() - g_ids_pendT > 3000) {   /* прошлый тик не дошёл (игра на паузе/грузится) — шлём новый */
            g_ids_pending = 1; g_ids_pendT = ids_now_ms();
            ids_post();
        }
        usleep(IDS_TICK_US);
    }
    return NULL;
}

/* ───────────── команда ───────────── */
static int ids_tokens(void* arr, char tok[][32], int maxn) {
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
            if (!q || w[0] == '/' || seq(w, "ids")) continue;        /* сама команда может входить в массив (как у /tpl) */
            if (n < maxn) { int z; for (z = 0; z <= q; z++) tok[n][z] = w[z]; n++; }
        }
    }
    return n;
}
static int ids_kind_word(const char* w) {                           /* 0 — не вид */
    if (seq(w, "npc") || seq(w, "нпс") || seq(w, "n") || seq(w, "actor") || seq(w, "actors")) return IDS_NPC;
    if (seq(w, "car") || seq(w, "cars") || seq(w, "veh") || seq(w, "vehicle") || seq(w, "авто") || seq(w, "машина") || seq(w, "машины") || seq(w, "c")) return IDS_CAR;
    if (seq(w, "obj") || seq(w, "object") || seq(w, "objects") || seq(w, "объект") || seq(w, "объекты") || seq(w, "o")) return IDS_OBJ;
    if (seq(w, "pick") || seq(w, "pickup") || seq(w, "pickups") || seq(w, "пикап") || seq(w, "пикапы") || seq(w, "p")) return IDS_PICK;
    return 0;
}
static void ids_b_mask(Buf* b, int mask) {
    int first = 1, k;
    for (k = 1; k <= 8; k <<= 1) if (mask & k) { if (!first) b_ch(b, ' '); b_s(b, ids_kname(k)); first = 0; }
    if (first) b_s(b, "—");
}
static void ids_usage(void) {
    chat_usage("/ids — ID над NPC, машинами, объектами и пикапами (вкл/выкл).  /ids npc car obj pick — показывать только перечисленное;  /ids all — всё;  /ids r 80 — радиус, м");
    chat_usage("/ids list — ближайшие в чат;  /ids off — убрать подписи;  /ids status — состояние");
}
static void ids_status(void) {
    char m[300]; Buf b; int n = 0, i;
    for (i = 0; i < IDS_MAXL; i++) if (g_ids_slot[i].used) n++;
    b_init(&b, m, sizeof m);
    b_s(&b, "/ids: "); b_s(&b, g_ids_on ? "ВКЛ" : "выкл"); b_s(&b, ", виды: "); ids_b_mask(&b, g_ids_mask);
    b_s(&b, ", радиус "); b_fix(&b, g_ids_radius, 0); b_s(&b, " м, подписей сейчас "); b_int(&b, n);
    chat_say(m);
    b_init(&b, m, sizeof m);
    b_s(&b, "  записей в пулах: NPC "); b_int(&b, g_ids_scanTotal[0]); b_s(&b, ", машин "); b_int(&b, g_ids_scanTotal[1]);
    b_s(&b, ", объектов "); b_int(&b, g_ids_scanTotal[2]); b_s(&b, ", пикапов "); b_int(&b, g_ids_scanTotal[3]); b_s(&b, " (-1 — пул не прочитался, -2 — вид выключен)");
    chat_say(m);
}
static void ids_list(void) {
    char m[240]; Buf b; int i, shown;
    if (!g_safe || !il2cpp_light()) { chat_say("/ids: игра ещё грузится, подождите"); return; }
    ensure_attached();
    if (!ids_myref(g_ids_ref)) { chat_say("/ids: ваша позиция не прочиталась — подробности в konstt.log"); return; }
    ids_collect(g_ids_mask);
    b_init(&b, m, sizeof m); b_s(&b, "/ids: в радиусе "); b_fix(&b, g_ids_radius, 0); b_s(&b, " м найдено "); b_int(&b, g_ids_nc); b_s(&b, g_ids_nc >= IDS_MAXL ? " (показаны ближайшие)" : "");
    chat_say(m);
    shown = g_ids_nc < 12 ? g_ids_nc : 12;
    for (i = 0; i < shown; i++) {
        b_init(&b, m, sizeof m); b_s(&b, "  "); b_s(&b, ids_kname(g_ids_c[i].kind)); b_ch(&b, ' '); b_int(&b, g_ids_c[i].id);
        if (g_ids_c[i].kind == IDS_OBJ && g_ids_c[i].model > 0) { b_s(&b, " [модель "); b_int(&b, g_ids_c[i].model); b_ch(&b, ']'); }
        b_s(&b, "  "); b_fix(&b, g_ids_c[i].d, 1); b_s(&b, " м  ("); ids_b_xyz(&b, g_ids_c[i].p); b_s(&b, ")");
        chat_say(m);
    }
    if (g_ids_nc > shown) chat_say("  … остальные видны подписями (/ids on)");
}
static int ids_enable(void) {
    float r[3];
    if (!g_safe || !il2cpp_light()) { chat_say("/ids: игра ещё грузится, подождите"); return 0; }
    ensure_attached();
    if (!ids_pool("Text3DLables")) { chat_say("/ids: пул 3D-подписей не найден (вы не в игре?) — подробности в konstt.log"); dlog("ids: нет Pools.Text3DLables"); return 0; }
    if (!ids_myref(r)) { chat_say("/ids: ваша позиция не прочиталась — подробности в konstt.log"); dlog("ids: не прочиталась позиция игрока"); return 0; }
    if (!ids_lb_layout()) { chat_say("/ids: не нашлась структура CreateText3DParams — подробности в konstt.log"); return 0; }
    mt_capture_ctx();                                                /* нужен главный поток — мы как раз в нём (обработчик команды) */
    ids_tick();                                                      /* первые подписи сразу */
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) {
        chat_say("/ids: подписи расставил один раз, но канала в главный поток нет — обновляться они не будут (подробности в konstt.log). /ids off уберёт их");
        dlog("ids: нет g_mt_ctx — только разовая расстановка");
        return 1;
    }
    g_ids_pending = 0;
    __atomic_store_n(&g_ids_on, 1, __ATOMIC_RELEASE);
    if (!g_ids_thread) {
        unsigned long t = 0;
        if (pthread_create(&t, NULL, ids_worker, NULL) != 0) { __atomic_store_n(&g_ids_on, 0, __ATOMIC_RELEASE); chat_say("/ids: не создался рабочий поток"); dlog("ids: pthread_create не вышел"); return 0; }
        g_ids_thread = 1;
    }
    return 1;
}
static void ids_disable(void) {
    __atomic_store_n(&g_ids_on, 0, __ATOMIC_RELEASE);
    if (g_safe && il2cpp_light()) { ensure_attached(); ids_clear_all(); }
    else { int i; for (i = 0; i < IDS_MAXL; i++) g_ids_slot[i].used = 0; }
}
static void ids_say_on(void) {
    char m[200]; Buf b; b_init(&b, m, sizeof m);
    b_s(&b, "/ids: ВКЛ, виды: "); ids_b_mask(&b, g_ids_mask); b_s(&b, ", радиус "); b_fix(&b, g_ids_radius, 0); b_s(&b, " м. /ids off — выключить, /ids help — все команды");
    chat_say(m);
}
static void native_ids_cmd(void* self, void* args, void* mi) {
    char tok[6][32]; int nt, i, mask = 0; float v;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) {      /* тик worker-а, а не команда */
        if (g_ids_on) ids_tick();
        g_ids_pending = 0;
        return;
    }
    mt_capture_ctx();
    nt = ids_tokens(args, tok, 6);
    dlog_i("ids: команда вызвана, слов ", nt);
    if (nt == 0) {                                                   /* переключатель */
        if (g_ids_on || ids_any()) { ids_disable(); chat_say("/ids: выключен, подписи убраны"); }
        else if (ids_enable()) ids_say_on();
        return;
    }
    if (seq(tok[0], "help") || seq(tok[0], "помощь") || seq(tok[0], "?")) { ids_usage(); return; }
    if (seq(tok[0], "status") || seq(tok[0], "статус") || seq(tok[0], "diag")) { ids_status(); return; }
    if (seq(tok[0], "list") || seq(tok[0], "список") || seq(tok[0], "near")) { ids_list(); return; }
    if (seq(tok[0], "off") || seq(tok[0], "выкл") || seq(tok[0], "0")) {
        if (!g_ids_on && !ids_any()) { chat_say("/ids: и так выключен"); return; }
        ids_disable(); chat_say("/ids: выключен, подписи убраны"); return;
    }
    if (seq(tok[0], "on") || seq(tok[0], "вкл") || seq(tok[0], "1")) {
        if (nt >= 2 && ids_num(tok[1], &v)) { g_ids_radius = v < 5.0f ? 5.0f : v > 300.0f ? 300.0f : v; }
        if (ids_enable()) ids_say_on();
        return;
    }
    if (seq(tok[0], "all") || seq(tok[0], "всё") || seq(tok[0], "все")) { g_ids_mask = IDS_ALL; if (ids_enable()) ids_say_on(); return; }
    if (seq(tok[0], "r") || seq(tok[0], "radius") || seq(tok[0], "радиус") || ids_num(tok[0], &v)) {
        if (!ids_num(tok[0], &v)) { if (nt < 2 || !ids_num(tok[1], &v)) { chat_usage("/ids r 80 — радиус в метрах (5…300)"); return; } }
        g_ids_radius = v < 5.0f ? 5.0f : v > 300.0f ? 300.0f : v;
        if (g_ids_on || ids_any()) { ids_tick(); ids_say_on(); }
        else { char m[120]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "/ids: радиус "); b_fix(&b, g_ids_radius, 0); b_s(&b, " м (подписи включит /ids)"); chat_say(m); }
        return;
    }
    for (i = 0; i < nt; i++) { int k = ids_kind_word(tok[i]); if (k) mask |= k; }
    if (mask) {
        g_ids_mask = mask;
        if (g_ids_on || ids_any()) { ids_tick(); ids_say_on(); }      /* уже включено — просто сменили виды (лишнее уберёт тик) */
        else if (ids_enable()) ids_say_on();
        return;
    }
    ids_usage();
}
static void* ids_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("ids", native_ids_cmd)) return NULL;         /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /ids */
    hm_mod_ready(g_ids_modId);
    dlog("ids: мод загружен, команда /ids работает");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void ids_ctor(void) {
    unsigned long t = 0;
    g_ids_modId = hm_mod_register("ids");
    pthread_create(&t, NULL, ids_main, NULL);
}
#endif
