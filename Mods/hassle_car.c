/*
 * hassle_car.c — необязательный модуль «/vtune» (раньше /car — оно занято): параметры МАШИНЫ, в которой вы за рулём (скорость, тяга, сцепление…), и профили по ID МОДЕЛИ.
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_car.c и пересоберите.
 *
 * Команды:
 *   /vtune                    инфо о текущей машине: ID экземпляра, ID модели, имя, имя хендлинга и текущие значения параметров
 *   /vtune speed 90           абсолютное значение;   /vtune speed x1.5 — во столько раз от ОРИГИНАЛА (оригинал запоминается при первом изменении)
 *   /vtune force x2           параметры: speed, reverse, force, accel, grip, brake, steer, drag  (список — /vtune help)
 *   /vtune reset              вернуть этой машине оригинальные значения
 *   /vtune save               запомнить отличия текущей машины от оригинала как ПРОФИЛЬ её МОДЕЛИ (по ID модели, не по экземпляру)
 *   /vtune model 123 speed x1.8   задать профиль модели 123 вручную (машина не нужна); /vtune model 123 del — удалить профиль
 *   /vtune list               все профили;   /vtune auto on|off — автоматически применять профиль, когда вы садитесь за руль этой модели
 *
 * Какие ID бывают (из дампа IL2CPP):
 *   BaseVehicle.ID            ushort — ID ЭКЗЕМПЛЯРА машины на сервере (тот же, что в RPC SetVehiclePos). Меняется от машины к машине.
 *   BaseVehicle.ModelInfo.ID  int    — ID МОДЕЛИ (VehicleDefinition : BaseDefinition). Одинаков у всех машин одной модели — по нему профили.
 *   VehicleDefinition.Handling string — имя набора хендлинга; Handlings.CommonData[имя] — исходные цифры (CommonVehicleHandling).
 *
 * Что пишем (поля по дампу):
 *   WheeledVehicle.m_Physics (WheeledVehiclePhysics): MaxSpeedForward, MaxSpeedReverse, MaxDriveForce, MaxBrakeForce, TireFriction, MaxSteerAngle, AeroDrag
 *   BaseVehicle.m_Handling (CommonVehicleHandling, структура ВНУТРИ объекта): MaxSpeedForward, EngineAcceleration
 *   «speed» пишет оба MaxSpeedForward (физика и хендлинг): игра может пересобирать физику из хендлинга (OnHandlingsUpdate), и тогда правка
 *   только физики откатилась бы. Единицы измерения (м/с или км/ч) не проверены — смотрите значения в «/vtune» и подбирайте множителем xN.
 *
 * ВАЖНО: код не запускался на устройстве — собран только компилятором. Поля читаются через IL2CPP по именам (не по зашитым смещениям);
 * если какого-то поля нет, /vtune скажет об этом и ничего не запишет. Профили лежат в HLOG/vtune_profiles.txt: «ID_модели ключ значение …».
 */
#pragma once
#include "hassle_core.c"

#define CAR_NROW 9
#define CAR_NORIG 8
#define CAR_MAXMODELS 32
#define CAR_MAXENT 9
#define CAR_FILE "car_profiles.txt"

/* where: 0 — WheeledVehiclePhysics (BaseVehicle.m_Physics), 1 — CommonVehicleHandling (BaseVehicle.m_Handling) */
typedef struct { const char* cmd; const char* key; int where; const char* field; } CarRow;
static const CarRow g_car_rows[CAR_NROW] = {
    { "speed",   "speed",   0, "MaxSpeedForward" },
    { "speed",   "hspeed",  1, "MaxSpeedForward" },
    { "reverse", "reverse", 0, "MaxSpeedReverse" },
    { "force",   "force",   0, "MaxDriveForce" },
    { "accel",   "haccel",  1, "EngineAcceleration" },
    { "grip",    "grip",    0, "TireFriction" },
    { "brake",   "brake",   0, "MaxBrakeForce" },
    { "steer",   "steer",   0, "MaxSteerAngle" },
    { "drag",    "drag",    0, "AeroDrag" },
};

typedef struct { void* veh; int id; unsigned have; float orig[CAR_NROW]; } CarOrig;
static CarOrig g_car_orig[CAR_NORIG];
static int g_car_origNext;

typedef struct { char key[12]; float val; int mul; } CarEnt;       /* mul=1 — val это множитель оригинала, 0 — абсолютное значение */
typedef struct { int model; int n; CarEnt e[CAR_MAXENT]; } CarProf;
static CarProf g_car_prof[CAR_MAXMODELS];
static int g_car_np, g_car_loaded;

static volatile int g_car_auto;                          /* 1 — worker шлёт тики в главный поток */
static volatile int g_car_pending;
static volatile long long g_car_pendT;
static void* g_car_lastVeh; static int g_car_lastId = -1; static int g_car_lastNp = -1; static int g_car_dirty;
static int g_car_modId = -1;

static long long car_now_ms(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return (long long)ts[0] * 1000 + ts[1] / 1000000; }

/* ───────────── числа и токены ───────────── */
static int car_num(const char* t, float* out) {          /* «12», «12.5», «12,5» */
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
/* значение: «90» → абсолют; «x1.5» / «х1.5» (кириллическая х тоже) / «*1.5» → множитель. 1 — разобрано */
static int car_val(const char* t, float* v, int* mul) {
    *mul = 0;
    if (t[0] == 'x' || t[0] == '*') { *mul = 1; t++; }
    else if ((u8)t[0] == 0xD1 && (u8)t[1] == 0x85) { *mul = 1; t += 2; }   /* «х» (U+0445) */
    return car_num(t, v);
}
static int car_tokens(void* arr, char tok[][32], int maxn) {
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
            int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 31 && n < maxn) tok[n][q++] = tmp[p]; p++; }
            if (n < maxn && q) { tok[n][q] = 0; n++; }
        }
    }
    return n;
}

/* ───────────── доступ к полям машины ───────────── */
static float* car_fptr(void* veh, int where, const char* field) {
    void *host, *f, *sc, *sf; int off, tk; float* p;
    if (!veh || !hm_obj_ok(veh)) return NULL;
    if (where == 0) {
        host = ref_field_s(veh, "m_Physics");
        if (!host || !hm_obj_ok(host)) return NULL;       /* не колёсная техника (вертолёт, самолёт, лодка) — физики нет */
        f = find_field(A.object_get_class(host), field);
        if (!f) return NULL;
        tk = A.type_get_type(A.field_get_type(f));
        if (tk != 12) return NULL;
        off = A.field_get_offset(f);
        p = (float*)((char*)host + off);
    } else {
        f = find_field(A.object_get_class(veh), "m_Handling");
        if (!f) return NULL;
        sc = A.class_from_type(A.field_get_type(f));
        sf = sc ? A.class_get_field_from_name(sc, field) : NULL;
        if (!sf) return NULL;
        tk = A.type_get_type(A.field_get_type(sf));
        if (tk != 12) return NULL;
        off = A.field_get_offset(f) + A.field_get_offset(sf) - struct_shift(sc);   /* структура внутри объекта: смещение поля минус «заголовок» */
        p = (float*)((char*)veh + off);
    }
    if (off < 0 || off > 0x2000 || !hm_readable(p, 4)) return NULL;
    return p;
}
static int car_finite(float v) { return v == v && v < 1e7f && v > -1e7f; }

static void* car_my_vehicle(int* isDriver) {
    void *me, *veh, *drv;
    if (isDriver) *isDriver = 0;
    if (!g_safe || !il2cpp_light()) return NULL;
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) return NULL;
    veh = ref_field_s(me, "Vehicle");
    if (!veh || !hm_obj_ok(veh)) return NULL;
    drv = ref_field_s(veh, "m_Driver");
    if (isDriver) *isDriver = (!drv || drv == me);
    return veh;
}
static int car_veh_id(void* veh) { return (int)num_field(veh, "ID", NULL); }                  /* BaseVehicle.ID (ushort) */
static void* car_model_info(void* veh) { void* mi = ref_field_s(veh, "ModelInfo"); return (mi && hm_obj_ok(mi)) ? mi : NULL; }
static int car_model_id(void* veh) { void* mi = car_model_info(veh); return mi ? int_field(mi, "ID", NULL, NULL, -1) : -1; }  /* ModelInfo.ID */
static void car_str_field(void* obj, const char* name, char* out, int cap) {
    void* s = obj ? ref_field_s(obj, name) : NULL; int n; const u16* ch;
    out[0] = 0;
    if (!s || !hm_obj_ok(s)) return;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 60 || !ch) return;
    utf16_to_utf8(ch, n, out, cap);
}

/* ───────────── оригиналы (по экземпляру) ───────────── */
static CarOrig* car_orig_get(void* veh, int create) {
    int i, id = car_veh_id(veh);
    for (i = 0; i < CAR_NORIG; i++) if (g_car_orig[i].veh == veh && g_car_orig[i].id == id) return &g_car_orig[i];
    if (!create) return NULL;
    i = g_car_origNext; g_car_origNext = (g_car_origNext + 1) % CAR_NORIG;
    g_car_orig[i].veh = veh; g_car_orig[i].id = id; g_car_orig[i].have = 0;
    return &g_car_orig[i];
}
/* записать строку row: mul=1 — orig*val, иначе val. 1 — записано */
static int car_write_row(void* veh, int row, float val, int mul) {
    float* p = car_fptr(veh, g_car_rows[row].where, g_car_rows[row].field);
    CarOrig* o; float nv;
    if (!p) return 0;
    o = car_orig_get(veh, 1);
    if (!(o->have & (1u << row))) { o->orig[row] = *p; o->have |= (1u << row); }
    nv = mul ? o->orig[row] * val : val;
    if (!car_finite(nv) || nv < 0.0f) return 0;
    if (*p != nv) *p = nv;
    return 1;
}

/* ───────────── профили ───────────── */
static int car_row_by_key(const char* k) { int i; for (i = 0; i < CAR_NROW; i++) if (seq(g_car_rows[i].key, k)) return i; return -1; }
static CarProf* car_prof_find(int model) { int i; for (i = 0; i < g_car_np; i++) if (g_car_prof[i].model == model) return &g_car_prof[i]; return NULL; }
static CarProf* car_prof_get(int model) {
    CarProf* p = car_prof_find(model);
    if (p) return p;
    if (g_car_np >= CAR_MAXMODELS) return NULL;
    p = &g_car_prof[g_car_np++]; p->model = model; p->n = 0;
    return p;
}
static void car_prof_put(CarProf* p, const char* key, float v, int mul) {
    int i;
    for (i = 0; i < p->n; i++) if (seq(p->e[i].key, key)) break;
    if (i == p->n) { if (p->n >= CAR_MAXENT) return; p->n++; }
    { int k = 0; for (; key[k] && k < 11; k++) p->e[i].key[k] = key[k]; p->e[i].key[k] = 0; }
    p->e[i].val = v; p->e[i].mul = mul;
}
static void car_prof_del(int model) {
    int i;
    for (i = 0; i < g_car_np; i++) if (g_car_prof[i].model == model) { g_car_prof[i] = g_car_prof[g_car_np - 1]; g_car_np--; return; }
}
static int car_path(char* out, int cap) {
    int i, last = -1; Buf b;
    if (!g_flog[0]) return 0;
    for (i = 0; g_flog[i]; i++) if (g_flog[i] == '/') last = i;
    if (last < 0 || last + (int)sizeof CAR_FILE + 8 > cap) return 0;
    b_init(&b, out, (unsigned)cap);
    for (i = 0; i <= last; i++) b_ch(&b, g_flog[i]);
    b_s(&b, CAR_FILE);
    return 1;
}
static int car_write_all(const char* path, const char* data, unsigned n) {
    int fd = open(path, 0x241, 0666); long w;
    if (fd < 0) return 0;
    w = n ? write(fd, data, n) : 0;
    close(fd);
    return w == (long)n;
}
static int car_prof_save(void) {
    static char buf[8192]; char path[320], tmp[336]; Buf b, t; int i, k;
    if (!car_path(path, sizeof path)) return 0;
    b_init(&b, buf, sizeof buf);
    for (i = 0; i < g_car_np; i++) {
        if (!g_car_prof[i].n) continue;
        b_int(&b, g_car_prof[i].model);
        for (k = 0; k < g_car_prof[i].n; k++) {
            b_ch(&b, ' '); b_s(&b, g_car_prof[i].e[k].key); b_ch(&b, ' ');
            if (g_car_prof[i].e[k].mul) b_ch(&b, 'x');
            b_fix(&b, g_car_prof[i].e[k].val, 3);
        }
        b_ch(&b, '\n');
    }
    b_init(&t, tmp, sizeof tmp); b_s(&t, path); b_s(&t, ".tmp");
    if (car_write_all(tmp, b.p, b.n) && rename(tmp, path) == 0) return 1;
    return car_write_all(path, b.p, b.n);
}
static void car_prof_load(void) {
    static char buf[8192]; char path[320]; int fd, i = 0; long n;
    if (g_car_loaded) return;
    g_car_loaded = 1; g_car_np = 0;
    if (!car_path(path, sizeof path)) return;
    fd = open(path, 0, 0);
    if (fd < 0) return;
    n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    while (i < n) {                                       /* строка: «модель ключ значение [ключ значение …]» */
        char tk[2 * CAR_MAXENT + 2][24]; int nt = 0; CarProf* p; float fv; int model;
        while (i < n && buf[i] != '\n') {
            int q = 0;
            while (i < n && (buf[i] == ' ' || buf[i] == '\r' || buf[i] == '\t')) i++;
            while (i < n && buf[i] != ' ' && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') { if (q < 23) tk[nt < 2 * CAR_MAXENT + 2 ? nt : 0][q++] = buf[i]; i++; }
            if (q && nt < 2 * CAR_MAXENT + 2) { tk[nt][q] = 0; nt++; }
        }
        i++;
        if (nt < 3 || !car_num(tk[0], &fv)) continue;
        model = (int)(fv + 0.5f);
        p = car_prof_get(model);
        if (!p) continue;
        { int k; for (k = 1; k + 1 < nt; k += 2) { float v; int mul; if (car_row_by_key(tk[k]) >= 0 && car_val(tk[k + 1], &v, &mul)) car_prof_put(p, tk[k], v, mul); } }
    }
}

/* применить профиль модели к машине; возвращает число записанных полей */
static int car_apply_prof(void* veh, const CarProf* p) {
    int k, ok = 0;
    for (k = 0; k < p->n; k++) { int r = car_row_by_key(p->e[k].key); if (r >= 0 && car_write_row(veh, r, p->e[k].val, p->e[k].mul)) ok++; }
    return ok;
}

/* ───────────── вывод ───────────── */
static void car_info(void* veh) {
    char nm[64], hd[64], line[220]; Buf b; int i; void* mi = car_model_info(veh);
    car_str_field(mi, "Name", nm, sizeof nm); car_str_field(mi, "Handling", hd, sizeof hd);
    b_init(&b, line, sizeof line);
    b_s(&b, "Машина: ID экземпляра "); b_int(&b, car_veh_id(veh)); b_s(&b, ", ID модели "); b_int(&b, car_model_id(veh));
    b_s(&b, ", «"); b_s(&b, nm[0] ? nm : "?"); b_s(&b, "», хендлинг «"); b_s(&b, hd[0] ? hd : "?"); b_s(&b, "»");
    chat_say(line);
    b_init(&b, line, sizeof line);
    for (i = 0; i < CAR_NROW; i++) {
        float* p = car_fptr(veh, g_car_rows[i].where, g_car_rows[i].field);
        if (b.n > 150) { chat_usage(line); b_init(&b, line, sizeof line); }
        b_s(&b, g_car_rows[i].key); b_ch(&b, '=');
        if (p) b_fix(&b, *p, 2); else b_ch(&b, '-');
        b_s(&b, "  ");
    }
    chat_usage(line);
}
static void car_help(void) {
    chat_usage("/vtune — инфо о машине;  /vtune speed 90 | speed x1.5 — скорость (абсолют или множитель оригинала);  параметры: speed reverse force accel grip brake steer drag");
    chat_usage("/vtune reset — вернуть оригинал;  /vtune save — запомнить как профиль этой МОДЕЛИ (по ID модели);  /vtune model ID параметр значение — профиль вручную;  /vtune model ID del");
    chat_usage("/vtune list — профили;  /vtune auto on|off — применять профиль автоматически, когда садитесь за руль этой модели");
}
static void car_list(void) {
    int i, k; char line[200]; Buf b;
    car_prof_load();
    if (!g_car_np) { chat_say("car: профилей нет (/vtune save или /vtune model ID параметр значение)"); return; }
    for (i = 0; i < g_car_np; i++) {
        b_init(&b, line, sizeof line);
        b_s(&b, "модель "); b_int(&b, g_car_prof[i].model); b_s(&b, ":");
        for (k = 0; k < g_car_prof[i].n; k++) { b_ch(&b, ' '); b_s(&b, g_car_prof[i].e[k].key); b_ch(&b, '='); if (g_car_prof[i].e[k].mul) b_ch(&b, 'x'); b_fix(&b, g_car_prof[i].e[k].val, 2); }
        chat_say(line);
    }
}

/* ───────────── команда ───────────── */
static void native_car_cmd(void* self, void* args, void* mi);
static void car_post_tick(void) {                        /* worker-поток: тик в главный поток (как у /tpl) */
    void *cb, *st, *a[2], *exc = NULL;
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { g_car_pending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_car_cmd);
    st = A.string_new("car-tick");
    if (!cb || !st) { g_car_pending = 0; return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) g_car_pending = 0;
}
static void* car_worker(void* arg) {
    (void)arg;
    for (;;) {
        if (!__atomic_load_n(&g_car_auto, __ATOMIC_ACQUIRE)) { usleep(200000); continue; }
        if (!g_car_pending || car_now_ms() - g_car_pendT > 1500) { g_car_pending = 1; g_car_pendT = car_now_ms(); car_post_tick(); }
        usleep(500000);
    }
    return NULL;
}
static void car_tick(void) {                             /* главный поток: сели на новую машину (или профили поменялись) → применяем профиль её модели */
    int drv = 0; void* veh; CarProf* p; int id;
    g_car_pending = 0;
    if (!__atomic_load_n(&g_car_auto, __ATOMIC_ACQUIRE)) return;
    veh = car_my_vehicle(&drv);
    if (!veh || !drv) { g_car_lastVeh = NULL; g_car_lastId = -1; return; }
    id = car_veh_id(veh);
    if (veh == g_car_lastVeh && id == g_car_lastId && !g_car_dirty && g_car_lastNp == g_car_np) return;
    g_car_lastVeh = veh; g_car_lastId = id; g_car_dirty = 0; g_car_lastNp = g_car_np;
    car_prof_load();
    p = car_prof_find(car_model_id(veh));
    if (p && p->n) {
        int ok = car_apply_prof(veh, p); char m[100]; Buf b; b_init(&b, m, sizeof m);
        b_s(&b, "car: профиль модели "); b_int(&b, p->model); b_s(&b, " применён, полей: "); b_int(&b, ok);
        chat_say(m);
    }
}

static void native_car_cmd(void* self, void* args, void* mi) {
    char tok[8][32]; int nt, drv = 0, r, i; void* veh; float v; int mul;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { car_tick(); return; }   /* тик worker-а, а не команда */
    mt_capture_ctx();
    nt = car_tokens(args, tok, 8);
    car_prof_load();
    if (nt >= 1 && (seq(tok[0], "help") || seq(tok[0], "?"))) { car_help(); return; }
    if (nt >= 1 && seq(tok[0], "list")) { car_list(); return; }
    if (nt >= 1 && seq(tok[0], "auto")) {
        int on = (nt >= 2) ? !(seq(tok[1], "off") || seq(tok[1], "0")) : !g_car_auto;
        __atomic_store_n(&g_car_auto, on, __ATOMIC_RELEASE); g_car_dirty = 1;
        chat_say(on ? "car: авто-применение профилей ВКЛ (работает, пока игра открыта)" : "car: авто-применение профилей выкл");
        return;
    }
    if (nt >= 2 && seq(tok[0], "model")) {                 /* /vtune model ID del | /vtune model ID параметр значение */
        float fid; int model; CarProf* p;
        if (!car_num(tok[1], &fid)) { chat_usage("Нужен ID модели: /vtune model 123 speed x1.8 (ID модели покажет /vtune, когда вы в машине)"); return; }
        model = (int)(fid + 0.5f);
        if (nt >= 3 && (seq(tok[2], "del") || seq(tok[2], "delete"))) { car_prof_del(model); car_prof_save(); g_car_dirty = 1; chat_say("car: профиль модели удалён"); return; }
        if (nt < 4 || !car_val(tok[3], &v, &mul)) { chat_usage("Формат: /vtune model ID параметр значение  (значение: 90 или x1.5; параметры: speed reverse force accel grip brake steer drag)"); return; }
        p = car_prof_get(model);
        if (!p) { chat_say("car: слишком много профилей"); return; }
        r = 0;
        for (i = 0; i < CAR_NROW; i++) if (seq(g_car_rows[i].cmd, tok[2])) { car_prof_put(p, g_car_rows[i].key, v, mul); r++; }
        if (!r) { chat_usage("Неизвестный параметр. Доступно: speed reverse force accel grip brake steer drag"); return; }
        car_prof_save(); g_car_dirty = 1;
        chat_say(g_car_auto ? "car: профиль модели сохранён (применится при посадке; авто — вкл)" : "car: профиль модели сохранён (включите /vtune auto on, чтобы применялся сам)");
        return;
    }
    veh = car_my_vehicle(&drv);
    if (!veh) { chat_say("car: вы не в машине (или игра ещё грузится)"); return; }
    if (!drv) { chat_say("car: вы пассажир — настраивать может только водитель"); return; }
    if (nt == 0 || seq(tok[0], "info")) { car_info(veh); return; }
    if (seq(tok[0], "reset")) {
        CarOrig* o = car_orig_get(veh, 0); int n = 0;
        if (!o) { chat_say("car: эта машина не менялась — сбрасывать нечего"); return; }
        for (i = 0; i < CAR_NROW; i++) if (o->have & (1u << i)) { float* p = car_fptr(veh, g_car_rows[i].where, g_car_rows[i].field); if (p) { *p = o->orig[i]; n++; } }
        g_car_lastVeh = veh; g_car_lastId = car_veh_id(veh);       /* авто-режим не должен тут же вернуть профиль */
        { char m[80]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "car: оригинал возвращён, полей: "); b_int(&b, n); chat_say(m); }
        return;
    }
    if (seq(tok[0], "save")) {
        CarOrig* o = car_orig_get(veh, 0); int model = car_model_id(veh), n = 0; CarProf* p;
        if (!o) { chat_say("car: нечего сохранять — сначала измените параметр (например /vtune speed x1.5)"); return; }
        if (model < 0) { chat_say("car: ID модели не прочитался — сохранить профиль не могу"); return; }
        p = car_prof_get(model);
        if (!p) { chat_say("car: слишком много профилей"); return; }
        for (i = 0; i < CAR_NROW; i++) if (o->have & (1u << i)) {
            float* q = car_fptr(veh, g_car_rows[i].where, g_car_rows[i].field);
            if (q && o->orig[i] > 0.0001f && (*q > o->orig[i] * 1.001f || *q < o->orig[i] * 0.999f)) { car_prof_put(p, g_car_rows[i].key, *q / o->orig[i], 1); n++; }
        }
        car_prof_save(); g_car_dirty = 1;
        { char m[100]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "car: профиль модели "); b_int(&b, model); b_s(&b, " сохранён (множители), полей: "); b_int(&b, n); chat_say(m); }
        return;
    }
    if (nt >= 2 && car_val(tok[1], &v, &mul)) {            /* /vtune параметр значение — текущей машине */
        int n = 0;
        for (i = 0; i < CAR_NROW; i++) if (seq(g_car_rows[i].cmd, tok[0])) n += car_write_row(veh, i, v, mul);
        if (!n) { chat_usage("car: не вышло — параметра нет или у этой техники нет такого поля (список: /vtune help)"); return; }
        g_car_lastVeh = veh; g_car_lastId = car_veh_id(veh);
        car_info(veh);
        return;
    }
    car_help();
}

static void* car_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("vtune", native_car_cmd)) return NULL;
    hm_mod_ready(g_car_modId);
    dlog("car: мод загружен, команда /vtune [параметр значение | reset | save | model ID … | list | auto]");
    car_worker(NULL);                                    /* этот же поток дальше шлёт тики авто-режима */
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void car_ctor(void) {
    unsigned long t = 0;
    g_car_modId = hm_mod_register("car");
    pthread_create(&t, NULL, car_main, NULL);
}
#endif
