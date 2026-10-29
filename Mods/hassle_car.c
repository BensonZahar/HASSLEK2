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
 *   /vtune from 522           взять хендлинг модели 522 (Handlings.CommonData[VehicleDefinition.Handling]) и поставить на вашу машину; from 522 all — вся структура; reset вернёт своё
 *   /vtune find npc           ближайшие NPC с их ID;  /vtune find car|npc 123 — где он;  /vtune go car|npc 123 — телепорт к нему по ID ЭКЗЕМПЛЯРА (Pools.Actors / Pools.Vehicles)
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
extern int clock_gettime(int, void*);       /* как в mem/route/tpl: struct timespec = два long */

#define CAR_NROW 15
#define CAR_NORIG 8
#define CAR_MAXMODELS 32
#define CAR_MAXENT 15
#define CAR_FILE "car_profiles.txt"

/* where: 0 — WheeledVehiclePhysics (BaseVehicle.m_Physics), 1 — CommonVehicleHandling (BaseVehicle.m_Handling), 2 — Engine (m_Physics.m_Engine),
   3 — Transmission (m_Physics.m_Transmission; только множителем) */
typedef struct { const char* cmd; const char* key; int where; const char* field; } CarRow;
static const CarRow g_car_rows[CAR_NROW] = {
    { "speed",   "speed",   0, "MaxSpeedForward" },
    { "speed",   "hspeed",  1, "MaxSpeedForward" },
    { "speed",   "hgear",   1, "MaxGearVelocity" },     /* «максимальная скорость передач»: от неё, скорее всего, игра считает передаточные числа — это и есть потолок */
    { "reverse", "reverse", 0, "MaxSpeedReverse" },
    { "force",   "force",   0, "MaxDriveForce" },
    { "accel",   "haccel",  1, "EngineAcceleration" },
    { "grip",    "grip",    0, "TireFriction" },
    { "brake",   "brake",   0, "MaxBrakeForce" },
    { "steer",   "steer",   0, "MaxSteerAngle" },
    { "drag",    "drag",    0, "AeroDrag" },
    { "power",   "power",   2, "m_MaxPower" },          /* мощность двигателя (Engine.SetPower) — прямой рычаг разгона */
    { "inertia", "inertia", 2, "m_Inertia" },           /* инерция двигателя: меньше — быстрее набирает обороты */
    { "traction","htract",  1, "TractionMultiplier" },  /* сцепление в хендлинге */
    { "gear",    "finalgear", 3, "m_FinalGearRatio" },  /* главная передача: x0.5 — «длиннее», выше потолок скорости, слабее старт */
    { "gear",    "totalgear", 3, "m_TotalGearRatio" },
};

typedef struct { void* veh; int id; unsigned have, wset; float orig[CAR_NROW], want[CAR_NROW]; } CarOrig;   /* orig — что было; want — что мы записали */
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
            char w[32]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 31) w[q++] = tmp[p]; p++; }
            w[q] = 0;
            if (!q || w[0] == '/' || seq(w, "vtune")) continue;     /* сама команда может входить в массив (как у /tpl) */
            if (n < maxn) { int z; for (z = 0; z <= q; z++) tok[n][z] = w[z]; n++; }
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
    } else if (where == 2 || where == 3) {
        void* ph = ref_field_s(veh, "m_Physics");
        host = ph && hm_obj_ok(ph) ? ref_field_s(ph, where == 2 ? "m_Engine" : "m_Transmission") : NULL;
        if (!host || !hm_obj_ok(host)) return NULL;       /* у техники нет двигателя/коробки (объект не создан) */
        f = find_field(A.object_get_class(host), field);
        if (!f) return NULL;
        if (A.type_get_type(A.field_get_type(f)) != 12) return NULL;
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
    g_car_orig[i].veh = veh; g_car_orig[i].id = id; g_car_orig[i].have = 0; g_car_orig[i].wset = 0;
    return &g_car_orig[i];
}
/* записать строку row: mul=1 — orig*val, иначе val. 1 — записано */
static int car_write_row(void* veh, int row, float val, int mul) {
    float* p = car_fptr(veh, g_car_rows[row].where, g_car_rows[row].field);
    CarOrig* o; float nv;
    if (!p) return 0;
    if (g_car_rows[row].where == 3 && !mul) return 0;            /* передаточные числа задают только множителем: /vtune gear x0.6 */
    o = car_orig_get(veh, 1);
    if (!(o->have & (1u << row))) { o->orig[row] = *p; o->have |= (1u << row); }
    nv = mul ? o->orig[row] * val : val;
    if (!car_finite(nv) || nv < 0.0f) return 0;
    if (*p != nv) *p = nv;
    o->want[row] = nv; o->wset |= (1u << row);
    return 1;
}
/* Пересчитать физику из хендлинга: BaseVehicle.OnHandlingsUpdate() — тот же метод, что игра зовёт при обновлении хендлингов. */
static int car_call_update(void* veh) {
    void *m, *exc = NULL; char et[200];
    m = find_method(A.object_get_class(veh), "OnHandlingsUpdate", 0);
    if (!m) { dlog("vtune: у машины нет OnHandlingsUpdate"); return 0; }
    A.runtime_invoke(m, veh, NULL, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("vtune: OnHandlingsUpdate бросил исключение: ", et); return 0; }
    return 1;
}
/* После пересчёта: наши значения физики/двигателя возвращаем на место; если игра откатила сам ХЕНДЛИНГ — считаем (reverted) и пишем обратно. */
static int car_post_update(void* veh) {
    CarOrig* o = car_orig_get(veh, 0); int i, reverted = 0;
    if (!car_call_update(veh)) return 0;
    if (!o) return 0;
    for (i = 0; i < CAR_NROW; i++) if (o->wset & (1u << i)) {
        float* p = car_fptr(veh, g_car_rows[i].where, g_car_rows[i].field);
        if (p && *p != o->want[i]) { if (g_car_rows[i].where == 1) reverted++; *p = o->want[i]; }
    }
    return reverted;
}
/* параметр cmd → все его строки; хендлинг пишем первым, затем (если был хендлинг) пересчёт физики. n — сколько записано, *rev — сколько хендлинг-полей игра откатила */
static int car_set_cmd(void* veh, const char* cmd, float v, int mul, int* rev) {
    int i, n = 0, hand = 0;
    if (seq(cmd, "speed") && !mul) {                              /* «speed 200»: единицы физики/хендлинга/передач могут различаться — переводим в множитель от оригинала физики */
        float* p0 = car_fptr(veh, g_car_rows[0].where, g_car_rows[0].field); CarOrig* o = car_orig_get(veh, 1);
        if (!p0 || !o) return 0;
        if (!(o->have & 1u)) { o->orig[0] = *p0; o->have |= 1u; }
        if (o->orig[0] < 0.0001f) return 0;
        v = v / o->orig[0]; mul = 1;
    }
    for (i = 0; i < CAR_NROW; i++) if (g_car_rows[i].where == 1 && seq(g_car_rows[i].cmd, cmd)) { int w = car_write_row(veh, i, v, mul); n += w; hand += w; }
    for (i = 0; i < CAR_NROW; i++) if (g_car_rows[i].where != 1 && seq(g_car_rows[i].cmd, cmd)) n += car_write_row(veh, i, v, mul);
    if (rev) *rev = hand ? car_post_update(veh) : 0;
    return n;
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
static int car_path_f(char* out, int cap, const char* fname) {
    int i, last = -1, fl = 0; Buf b;
    if (!g_flog[0]) return 0;
    while (fname[fl]) fl++;
    for (i = 0; g_flog[i]; i++) if (g_flog[i] == '/') last = i;
    if (last < 0 || last + fl + 8 > cap) return 0;
    b_init(&b, out, (unsigned)cap);
    for (i = 0; i <= last; i++) b_ch(&b, g_flog[i]);
    b_s(&b, fname);
    return 1;
}
static int car_path(char* out, int cap) { return car_path_f(out, cap, CAR_FILE); }
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
    int k, ok = 0, hand = 0;
    for (k = 0; k < p->n; k++) { int r = car_row_by_key(p->e[k].key); if (r >= 0 && g_car_rows[r].where == 1 && car_write_row(veh, r, p->e[k].val, p->e[k].mul)) { ok++; hand++; } }
    for (k = 0; k < p->n; k++) { int r = car_row_by_key(p->e[k].key); if (r >= 0 && g_car_rows[r].where != 1 && car_write_row(veh, r, p->e[k].val, p->e[k].mul)) ok++; }
    if (hand) car_post_update(veh);
    return ok;
}

/* ───────────── список всех моделей машин ───────────── */
/* DefinitionDatabase.DefinitionsByID (Dictionary<int, BaseDefinition>) — все определения (машины, пеши, оружие…); берём только VehicleDefinition */
#define CAR_MAXMDL 1500
typedef struct { int id, type; char name[40]; char hand[24]; } CarMdl;
static CarMdl g_car_mdl[CAR_MAXMDL];
static int g_car_nmdl;
static const char* const g_car_typeName[9] = { "авто", "мото", "квадро", "вело", "самолёт", "вертолёт", "лодка", "поезд", "прицеп" };
static void car_mdl_cb(void* it, void* ctx) {
    CarMdl* m; void* f;
    (void)ctx;
    if (g_car_nmdl >= CAR_MAXMDL || !hm_obj_ok(it)) return;
    if (!seq(A.class_get_name(A.object_get_class(it)), "VehicleDefinition")) return;
    m = &g_car_mdl[g_car_nmdl];
    m->id = int_field(it, "ID", NULL, NULL, -1);
    car_str_field(it, "Name", m->name, sizeof m->name);
    car_str_field(it, "Handling", m->hand, sizeof m->hand);
    m->type = -1;
    f = find_field(A.object_get_class(it), "Type");                  /* VehicleDefinition.Type — enum int32 */
    if (f) { int off = A.field_get_offset(f); if (off > 0 && off < 0x400 && hm_readable((char*)it + off, 4)) m->type = *(int*)((char*)it + off); }
    g_car_nmdl++;
}
static int car_mdl_load(void) {
    void *cls, *f, *dict = NULL, *arr; int i, j, cnt;
    g_car_nmdl = 0;
    if (!g_safe || !il2cpp_light() || !g_img) { dlog("car: список моделей: IL2CPP ещё не готов"); return 0; }
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Definitions", "DefinitionDatabase");
    if (!cls) { dlog("car: список моделей: класс Hassle.Definitions.DefinitionDatabase не найден"); return 0; }
    f = A.class_get_field_from_name(cls, "DefinitionsByID");
    if (!f) { dlog("car: список моделей: поле DefinitionsByID не найдено"); return 0; }
    A.field_static_get_value(f, &dict);
    if (!dict || !hm_obj_ok(dict)) { dlog("car: список моделей: DefinitionsByID пуст или нечитаем (определения ещё не загружены?)"); return 0; }
    arr = ref_field_s(dict, "_entries");
    if (!arr) arr = ref_field_s(dict, "entries");
    if (!arr) { dlog("car: список моделей: у словаря нет _entries"); return 0; }
    cnt = int_field(dict, "_count", "count", NULL, -1);
    dlog_i("car: список моделей: записей в словаре ", cnt);
    walk_struct_array(arr, cnt, "value", car_mdl_cb, NULL);
    dlog_i("car: список моделей: найдено машин ", g_car_nmdl);
    for (i = 1; i < g_car_nmdl; i++) {                               /* сортировка по ID (вставками) */
        CarMdl t = g_car_mdl[i];
        for (j = i - 1; j >= 0 && g_car_mdl[j].id > t.id; j--) g_car_mdl[j + 1] = g_car_mdl[j];
        g_car_mdl[j + 1] = t;
    }
    return g_car_nmdl;
}
static int car_save_models(void) {                                  /* весь список — в HLOG/car_models.txt: «ID<tab>имя<tab>хендлинг<tab>тип» */
    static char buf[90000]; char path[320]; Buf b; int i;
    if (!car_path_f(path, sizeof path, "car_models.txt")) return 0;
    b_init(&b, buf, sizeof buf);
    for (i = 0; i < g_car_nmdl; i++) {
        CarMdl* m = &g_car_mdl[i];
        b_int(&b, m->id); b_ch(&b, '\t'); b_s(&b, m->name); b_ch(&b, '\t'); b_s(&b, m->hand); b_ch(&b, '\t');
        b_s(&b, (m->type >= 0 && m->type < 9) ? g_car_typeName[m->type] : "?"); b_ch(&b, '\n');
    }
    return car_write_all(path, b.p, b.n);
}
static int car_has(const char* hay, const char* needle) {           /* подстрока без учёта регистра (ASCII) */
    int i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            char a = hay[i + j], c = needle[j];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != c) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}
static void car_mdl_line(const CarMdl* m) {
    char line[140]; Buf b; b_init(&b, line, sizeof line);
    b_int(&b, m->id); b_s(&b, "  "); b_s(&b, m->name[0] ? m->name : "?"); b_s(&b, "  [");
    b_s(&b, m->hand[0] ? m->hand : "?"); b_s(&b, ", "); b_s(&b, (m->type >= 0 && m->type < 9) ? g_car_typeName[m->type] : "?"); b_s(&b, "]");
    chat_say(line);
}
/* /vtune cars [страница | текст-фильтр] */
static void car_cmd_cars(const char* arg) {
    int i, shown = 0, total, page = 1, pages; char head[140]; Buf b; float fv;
    if (!car_mdl_load()) { chat_say("vtune: список моделей не прочитался (DefinitionDatabase ещё не загружена или поле не найдено) — подробности в konstt.log"); return; }
    total = g_car_nmdl;
    car_save_models();
    b_init(&b, head, sizeof head);
    if (arg && arg[0] && !car_num(arg, &fv)) {                       /* фильтр по имени модели или хендлинга */
        for (i = 0; i < total && shown < 20; i++) if (car_has(g_car_mdl[i].name, arg) || car_has(g_car_mdl[i].hand, arg)) { car_mdl_line(&g_car_mdl[i]); shown++; }
        b_s(&b, "Найдено по «"); b_s(&b, arg); b_s(&b, "»: "); b_int(&b, shown); b_s(&b, shown >= 20 ? "+ (показаны первые 20)" : "");
        chat_usage(head);
        return;
    }
    if (arg && arg[0]) page = (int)(fv + 0.5f);
    pages = (total + 9) / 10; if (page < 1) page = 1; if (page > pages) page = pages;
    for (i = (page - 1) * 10; i < total && i < page * 10; i++) car_mdl_line(&g_car_mdl[i]);
    b_s(&b, "Моделей машин: "); b_int(&b, total); b_s(&b, ", страница "); b_int(&b, page); b_s(&b, "/"); b_int(&b, pages);
    b_s(&b, ".  /vtune cars N — страница, /vtune cars текст — поиск. Весь список — файл HLOG/car_models.txt");
    chat_usage(head);
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
    chat_usage("/vtune — инфо о машине;  /vtune speed 90 | speed x1.5 — скорость (абсолют или множитель оригинала);  параметры: speed reverse force accel power inertia grip traction brake steer drag");
    chat_usage("/vtune reset — вернуть оригинал;  /vtune save — запомнить как профиль этой МОДЕЛИ (по ID модели);  /vtune model ID параметр значение — профиль вручную;  /vtune model ID del");
    chat_usage("/vtune cars [N | текст] — список всех моделей машин (ID, имя, хендлинг); потолок скорости: speed xN + power xN + drag x0.3, при упоре — gear x0.6");
    chat_usage("/vtune list — профили;  /vtune auto on|off — применять профиль автоматически, когда садитесь за руль этой модели");
    chat_usage("/vtune from ID [all] — взять хендлинг МОДЕЛИ ID (из /vtune cars) и поставить на вашу машину (all — вся структура);  /vtune reset вернёт свой");
    chat_usage("/vtune find npc — ближайшие NPC и их ID;  /vtune find car|npc ID — где он (координаты, расстояние);  /vtune go car|npc ID — телепорт к нему (ID экземпляра; тот же способ, что /tpl)");
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

/* ───────────── /vtune from ID: хендлинг МОДЕЛИ ID → на вашу текущую машину ───────────── */
/* VehicleDefinition(ID).Handling — имя набора; Handlings.CommonData[имя] — CommonVehicleHandling (структура внутри записи словаря).
   Копируем её поля в BaseVehicle.m_Handling вашей машины (структура внутри объекта) и просим игру пересчитать физику. */
static void* g_car_findDef; static int g_car_findId;
static void car_find_cb(void* it, void* ctx) {
    (void)ctx;
    if (g_car_findDef || !hm_obj_ok(it)) return;
    if (int_field(it, "ID", NULL, NULL, -1) == g_car_findId) g_car_findDef = it;
}
static void* car_def_by_id(int id) {                                /* определение с этим ID из DefinitionDatabase.DefinitionsByID (или NULL) */
    void *cls, *f, *dict = NULL, *arr;
    g_car_findDef = NULL; g_car_findId = id;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Definitions", "DefinitionDatabase");
    f = cls ? A.class_get_field_from_name(cls, "DefinitionsByID") : NULL;
    if (!f) { dlog("car: from: нет DefinitionDatabase.DefinitionsByID"); return NULL; }
    A.field_static_get_value(f, &dict);
    if (!dict || !hm_obj_ok(dict)) { dlog("car: from: DefinitionsByID пуст или нечитаем"); return NULL; }
    arr = ref_field_s(dict, "_entries"); if (!arr) arr = ref_field_s(dict, "entries");
    if (!arr) { dlog("car: from: у словаря нет _entries"); return NULL; }
    walk_struct_array(arr, int_field(dict, "_count", "count", NULL, -1), "value", car_find_cb, NULL);
    return g_car_findDef;
}
/* Handlings.CommonData[имя] → адрес структуры CommonVehicleHandling внутри записи словаря (и её класс в *scOut) */
static char* car_hand_src(const char* hname, void** scOut) {
    void *cls, *f, *dict = NULL, *arr, *ac, *ec, *fk, *fv, *fh; int esz, i, cnt, shift, offk, offv, offh = 0; long len; char* data;
    cls = A.class_from_name(g_img, "Hassle.Vehicles", "Handlings");
    f = cls ? A.class_get_field_from_name(cls, "CommonData") : NULL;
    if (!f) { dlog("car: from: нет Handlings.CommonData"); return NULL; }
    A.field_static_get_value(f, &dict);
    if (!dict || !hm_obj_ok(dict)) { dlog("car: from: Handlings.CommonData пуст или нечитаем"); return NULL; }
    arr = ref_field_s(dict, "_entries"); if (!arr) arr = ref_field_s(dict, "entries");
    if (!arr || !hm_obj_ok(arr)) { dlog("car: from: у CommonData нет _entries"); return NULL; }
    ac = A.object_get_class(arr); ec = A.class_get_element_class(ac); esz = arr_elem_size(ec);
    fk = ec ? find_field(ec, "key") : NULL; fv = ec ? find_field(ec, "value") : NULL; fh = ec ? find_field(ec, "hashCode") : NULL;
    if (!fk || !fv || esz <= 0) { dlog("car: from: не разобрать запись CommonData"); return NULL; }
    shift = A.class_is_valuetype(ec) ? struct_shift(ec) : 0;
    offk = A.field_get_offset(fk) - shift; offv = A.field_get_offset(fv) - shift; if (fh) offh = A.field_get_offset(fh) - shift;
    *scOut = A.class_from_type(A.field_get_type(fv));
    cnt = int_field(dict, "_count", "count", NULL, -1);
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 100000) return NULL;
    if (cnt < 0 || cnt > len) cnt = (int)len;
    dlog_i("car: from: наборов хендлинга в CommonData ", cnt);
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < cnt; i++) {
        char* e = data + (uptr)i * (uptr)esz; void* key; char nm[64]; int n;
        if (!hm_range_ok(e, esz)) break;
        if (fh && *(int*)(e + offh) < 0) continue;
        key = *(void**)(e + offk);
        if (!key || !hm_obj_ok(key)) continue;
        n = A.string_length(key); if (n <= 0 || n > 60) continue;
        utf16_to_utf8(A.string_chars(key), n, nm, sizeof nm);
        if (seq(nm, hname)) return e + offv;
    }
    return NULL;
}
static struct { void* veh; int id; int n; unsigned char b[256]; } g_car_hbak;    /* копия ВСЕЙ структуры хендлинга до /vtune from — для /vtune reset */
/* mode 0 — скопировать поля src→dst; mode 1 — проверить и вернуть, если игра откатила. all=0 — только «ездовые» поля. Возврат: полей скопировано/откатилось */
static int car_bytes_diff(const char* a, const char* b, int n) { int i; for (i = 0; i < n; i++) if (a[i] != b[i]) return 1; return 0; }
static int car_copy_fields(char* src, char* dst, void* sc, int all, int mode, int* total_out) {
    static const char* const want[] = { "DragMultiplier", "TractionMultiplier", "TractionLoss", "TractionBias", "NumberOfGears", "MaxGearVelocity",
        "EngineAcceleration", "EngineInertia", "DriveType", "EngineType", "BrakeDeceleration", "BrakeBias", "SteeringLock", "MaxSpeedForward" };
    void *it = NULL, *f; int sh = struct_shift(sc), offs[64], n = 0, i, k, res = 0, total;
    while ((f = A.class_get_fields(sc, &it)) && n < 64) { if (A.field_get_flags(f) & 0x10) continue; offs[n++] = A.field_get_offset(f) - sh; }
    total = A.class_instance_size ? A.class_instance_size(sc) - (int)HDR : 0;
    if (total <= 0 || total > 255) { int mx = 0; for (i = 0; i < n; i++) if (offs[i] > mx) mx = offs[i]; total = mx + 4; }
    if (total_out) *total_out = total;
    it = NULL;
    while ((f = A.class_get_fields(sc, &it))) {
        int off, size, ok = all; const char* nm;
        if (A.field_get_flags(f) & 0x10) continue;
        off = A.field_get_offset(f) - sh; nm = A.field_get_name(f);
        if (!ok && nm) for (k = 0; k < (int)(sizeof want / sizeof want[0]); k++) if (seq(nm, want[k])) { ok = 1; break; }
        if (!ok) continue;
        size = total - off;                                          /* до конца структуры или до ближайшего следующего поля */
        for (i = 0; i < n; i++) if (offs[i] > off && offs[i] - off < size) size = offs[i] - off;
        if (off < 0 || size <= 0 || size > 16) continue;
        if (mode == 0) { memcpy(dst + off, src + off, (unsigned long)size); res++; }
        else if (car_bytes_diff(dst + off, src + off, size)) { memcpy(dst + off, src + off, (unsigned long)size); res++; }
    }
    return res;
}
static void car_cmd_from(void* veh, int model, int all) {
    void *def, *sc = NULL, *hf, *hsc; char hn[64], *src, *dst; int n, total = 0, back, i; char m[160]; Buf b; CarOrig* o;
    if (!car_mdl_load()) { chat_say("vtune: список моделей не прочитался — подробности в konstt.log"); return; }
    def = car_def_by_id(model);
    if (!def) { chat_say("vtune: модель с таким ID не найдена (/vtune cars — список)"); return; }
    if (!seq(A.class_get_name(A.object_get_class(def)), "VehicleDefinition")) { chat_say("vtune: этот ID — не машина (оружие/скин/предмет)"); return; }
    car_str_field(def, "Handling", hn, sizeof hn);
    if (!hn[0]) { chat_say("vtune: у этой модели не прочиталось имя хендлинга"); return; }
    src = car_hand_src(hn, &sc);
    if (!src || !sc) { dlog2("car: from: набор хендлинга не найден: ", hn); chat_say("vtune: набор хендлинга этой модели не найден в Handlings.CommonData (подробности в konstt.log)"); return; }
    hf = find_field(A.object_get_class(veh), "m_Handling");
    hsc = hf ? A.class_from_type(A.field_get_type(hf)) : NULL;
    if (!hf || !hsc) { chat_say("vtune: у вашей техники нет m_Handling"); return; }
    dst = (char*)veh + A.field_get_offset(hf);
    n = 0; (void)car_copy_fields(src, dst, hsc, all, 1, &total);          /* только чтобы узнать размер структуры */
    if (total <= 0 || total > 255 || !hm_range_ok(src, total) || !hm_range_ok(dst, total)) { chat_say("vtune: структура хендлинга нечитаема"); return; }
    /* оригиналы: целиком (для reset) и по строкам таблицы (для save/auto) */
    if (!(g_car_hbak.veh == veh && g_car_hbak.id == car_veh_id(veh) && g_car_hbak.n > 0)) { g_car_hbak.veh = veh; g_car_hbak.id = car_veh_id(veh); g_car_hbak.n = total; memcpy(g_car_hbak.b, dst, (unsigned long)total); }
    o = car_orig_get(veh, 1);
    for (i = 0; i < CAR_NROW; i++) if (g_car_rows[i].where == 1) { float* p = car_fptr(veh, 1, g_car_rows[i].field); if (p && !(o->have & (1u << i))) { o->orig[i] = *p; o->have |= (1u << i); } }
    n = car_copy_fields(src, dst, hsc, all, 0, NULL);
    car_call_update(veh);                                                 /* игра пересобирает физику из хендлинга */
    back = car_copy_fields(src, dst, hsc, all, 1, NULL);                  /* если игра при пересчёте вернула старое — пишем обратно (физику не трогаем) */
    for (i = 0; i < CAR_NROW; i++) if (g_car_rows[i].where == 1) { float* p = car_fptr(veh, 1, g_car_rows[i].field); if (p) { o->want[i] = *p; o->wset |= (1u << i); } }
    if (back) car_call_update(veh);
    g_car_lastVeh = veh; g_car_lastId = car_veh_id(veh);
    b_init(&b, m, sizeof m);
    b_s(&b, "vtune: хендлинг модели "); b_int(&b, model); b_s(&b, " («"); b_s(&b, hn); b_s(&b, "») применён к вашей машине, полей: "); b_int(&b, n);
    if (back) { b_s(&b, ", игра откатила: "); b_int(&b, back); }
    b_s(&b, all ? " (вся структура)" : "");
    chat_say(m);
    chat_say("vtune: /vtune — посмотреть значения; /vtune reset — вернуть свой хендлинг");
    dlog2("car: from: набор ", hn);
}

/* ───────────── /vtune find|go car|npc ID — найти по ID ЭКЗЕМПЛЯРА и телепортироваться ───────────── */
/* ID экземпляра — это ключ в пулах игры: Pools.Vehicles (BaseVehicle.ID, ushort) и Pools.Actors (NPC: Actor.m_ID, ushort). Ищем игровым же методом
   пула Get(int id) (его зовут RPC сервера), позицию берём из Entity.Transform, телепорт — тот же Entity.SetPosition(Vector3, findHeight), что у /tpl
   (игра сама переставляет игрока/машину по приказу сервера). Клиент знает только тех, кого сервер ему «стримит» (рядом с вами): далёкого NPC в пуле
   может не быть — тогда «не найден». Зависит только от ядра (код телепорта скопирован из /tpl, чтобы car не требовал hassle_tpl.c). */
static int car_tp_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
static void* car_tp_obj(void* obj, const char* name) {            /* метод без аргументов → ссылочный объект */
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int car_tp_vec(void* obj, const char* name, float* out) {  /* метод без аргументов → Vector3 (в боксе после заголовка) */
    void *m, *exc = NULL, *r; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!car_tp_finite(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
static int car_ent_pos(void* ent, float* out) {                    /* Entity.Transform.position */
    void* tr = car_tp_obj(ent, "get_Transform");
    if (!tr) tr = car_tp_obj(ent, "get_transform");
    return tr && car_tp_vec(tr, "get_position", out);
}
static int car_tp_call_set(void* obj, const float* pos, u8 flag) { /* SetPosition(Vector3, bool) */
    void *m, *exc = NULL, *args[2]; float v[3]; char et[240];
    v[0] = pos[0]; v[1] = pos[1]; v[2] = pos[2];
    m = find_method(A.object_get_class(obj), "SetPosition", 2);
    if (!m) { dlog("car: go: SetPosition не найден"); return 0; }
    args[0] = v; args[1] = &flag;
    A.runtime_invoke(m, obj, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("car: go: SetPosition бросил исключение: ", et); return 0; }
    return 1;
}
static void car_tp_stop_vehicle(void* veh) {                       /* обнулить скорость машины после переноса */
    void *rb, *m, *exc = NULL, *args[1]; float z[3];
    z[0] = z[1] = z[2] = 0.0f;
    rb = ref_field_s(veh, "m_Rigidbody");
    if (!rb || !hm_obj_ok(rb)) return;
    args[0] = z;
    m = find_method(A.object_get_class(rb), "set_velocity", 1);
    if (m) A.runtime_invoke(m, rb, args, &exc);
    exc = NULL;
    m = find_method(A.object_get_class(rb), "set_angularVelocity", 1);
    if (m) A.runtime_invoke(m, rb, args, &exc);
}
static void* car_pool(const char* field) {                         /* Pools.<field> (Vehicles | Actors) */
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}
static void* car_ent_by_id(int kind, int id) {                     /* kind 0 — машина, 1 — NPC; NULL — нет такого (или не стримится) */
    void *pool = car_pool(kind ? "Actors" : "Vehicles"), *m, *exc = NULL, *args[1], *r; int a = id; char et[200];
    if (!pool) { dlog("car: find: пул Pools.Vehicles/Actors не найден"); return NULL; }
    m = find_method(A.object_get_class(pool), "Get", 1);
    if (!m) { dlog("car: find: у пула нет Get(int)"); return NULL; }
    args[0] = &a;
    r = A.runtime_invoke(m, pool, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("car: find: Get бросил исключение (ID нет в пуле?): ", et); return NULL; }
    return (r && hm_obj_ok(r)) ? r : NULL;
}
static void car_b_xyz(Buf* b, const float* p) { b_fix(b, p[0], 1); b_ch(b, ' '); b_fix(b, p[1], 1); b_ch(b, ' '); b_fix(b, p[2], 1); }
static int car_kind(const char* t) {                               /* 0 машина, 1 NPC, -1 не понял */
    if (seq(t, "car") || seq(t, "veh") || seq(t, "vehicle") || seq(t, "auto") || seq(t, "авто") || seq(t, "машина") || seq(t, "тачка")) return 0;
    if (seq(t, "npc") || seq(t, "actor") || seq(t, "ped") || seq(t, "нпс") || seq(t, "нпц") || seq(t, "бот")) return 1;
    return -1;
}
/* где вы и что двигаем: вас или вашу машину (за рулём). 0 — нельзя (причина в чате) */
static int car_my_target(void** me, void** tgt, void** vehOut, float* pos) {
    void *m, *veh;
    if (!g_safe || !il2cpp_light()) { chat_say("vtune: игра ещё грузится"); return 0; }
    ensure_attached();
    m = local_player();
    if (!m || !hm_obj_ok(m)) { chat_say("vtune: вы не в игре (персонаж не создан)"); return 0; }
    veh = ref_field_s(m, "Vehicle"); if (veh && !hm_obj_ok(veh)) veh = NULL;
    if (veh) { void* drv = ref_field_s(veh, "m_Driver"); if (drv && drv != m) { chat_say("vtune: вы пассажир — машину двигает водитель"); return 0; } }
    *me = m; *vehOut = veh; *tgt = veh ? veh : m;
    if (pos && !car_ent_pos(*tgt, pos)) { chat_say("vtune: не прочиталась ваша позиция"); return 0; }
    return 1;
}
static float car_dist(const float* a, const float* b) { double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz); }
/* /vtune find car|npc ID — показать, где он; go — перенести вас к нему */
static void car_cmd_find(int kind, int id, int go) {
    void *ent, *me = NULL, *tgt = NULL, *veh = NULL; float tp[3], mp[3], np[3]; int haveMe; char m[240]; Buf b; const char* what = kind ? "NPC " : "Машина ";
    ent = car_ent_by_id(kind, id);
    if (!ent) {
        b_init(&b, m, sizeof m); b_s(&b, "vtune: "); b_s(&b, kind ? "NPC " : "машина "); b_int(&b, id);
        b_s(&b, " не найден(а) среди созданных у вас в игре — сервер «стримит» только тех, кто рядом (подойдите ближе или проверьте ID)");
        chat_say(m); return;
    }
    if (!car_ent_pos(ent, tp)) { chat_say("vtune: нашёл, но позиция не прочиталась — подробности в konstt.log"); return; }
    haveMe = car_my_target(&me, &tgt, &veh, mp);
    if (!go) {
        b_init(&b, m, sizeof m); b_s(&b, what); b_int(&b, id); b_s(&b, ": "); car_b_xyz(&b, tp);
        if (haveMe) { b_s(&b, ", в "); b_fix(&b, car_dist(tp, mp), 1); b_s(&b, " м от вас"); }
        if (kind) { int ok = 0; double h = num_field(ent, "m_Health", &ok); if (ok) { b_s(&b, ", здоровье "); b_fix(&b, h, 0); } }
        else { int mid = car_model_id(ent); if (mid >= 0) { b_s(&b, ", модель "); b_int(&b, mid); } }
        chat_say(m);
        b_init(&b, m, sizeof m); b_s(&b, "/tpl to "); car_b_xyz(&b, tp); chat_usage(m);
        return;
    }
    if (!haveMe) return;
    if (tgt == ent) { chat_say("vtune: это и есть ваша машина"); return; }
    np[0] = tp[0] + (veh ? 4.0f : 0.0f); np[1] = tp[1] + (veh ? 1.0f : 1.5f); np[2] = tp[2];     /* пешком — на 1.5 м над ним; в машине — рядом (+4 м), чтобы не сесть внутрь */
    if (!car_tp_call_set(tgt, np, 0)) { chat_say("vtune: телепорт не вышел — подробности в konstt.log (можно попробовать /tpl to ...)"); return; }
    if (veh) car_tp_stop_vehicle(veh);
    b_init(&b, m, sizeof m); b_s(&b, "vtune: перенёс к "); b_s(&b, kind ? "NPC " : "машине "); b_int(&b, id); b_s(&b, ": "); car_b_xyz(&b, np);
    b_s(&b, ", это в "); b_fix(&b, car_dist(np, mp), 1); b_s(&b, " м от вас");
    if (veh) b_s(&b, " (вместе с машиной; на большом расстоянии она двинется, когда подгрузится карта)");
    chat_say(m);
    dlog2("car: go: ", m);
}
/* /vtune find npc — 8 ближайших NPC (чтобы узнать ID завис­шего) */
typedef struct { int id; float d; float p[3]; } CarNear;
static CarNear g_car_near[8]; static int g_car_nnear; static float g_car_myp[3];
static void car_near_cb(void* it, void* ctx) {
    float p[3], d; int i, j, id;
    (void)ctx;
    if (!hm_obj_ok(it) || !car_ent_pos(it, p)) return;
    id = (int)num_field(it, "m_ID", NULL);
    d = car_dist(p, g_car_myp);
    for (i = 0; i < g_car_nnear && g_car_near[i].d <= d; i++) {}
    if (i >= 8) return;
    for (j = (g_car_nnear < 8 ? g_car_nnear : 7); j > i; j--) g_car_near[j] = g_car_near[j - 1];
    g_car_near[i].id = id; g_car_near[i].d = d; g_car_near[i].p[0] = p[0]; g_car_near[i].p[1] = p[1]; g_car_near[i].p[2] = p[2];
    if (g_car_nnear < 8) g_car_nnear++;
}
static void car_cmd_near_npc(void) {
    void *me = NULL, *tgt = NULL, *veh = NULL, *pool, *dict, *arr; int i, total; char m[200]; Buf b;
    if (!car_my_target(&me, &tgt, &veh, g_car_myp)) return;
    pool = car_pool("Actors");
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (!arr) { chat_say("vtune: пул NPC не прочитался — подробности в konstt.log"); dlog("car: near: нет Pools.Actors.m_Items._entries"); return; }
    total = int_field(dict, "_count", "count", NULL, -1);
    g_car_nnear = 0;
    walk_struct_array(arr, total, "value", car_near_cb, NULL);
    b_init(&b, m, sizeof m); b_s(&b, "NPC у вас в игре: "); b_int(&b, total); b_s(&b, g_car_nnear ? ", ближайшие:" : " — рядом никого");
    chat_say(m);
    for (i = 0; i < g_car_nnear; i++) {
        b_init(&b, m, sizeof m); b_s(&b, "  NPC "); b_int(&b, g_car_near[i].id); b_s(&b, "  "); b_fix(&b, g_car_near[i].d, 1); b_s(&b, " м  ("); car_b_xyz(&b, g_car_near[i].p); b_s(&b, ")");
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
    if (nt >= 1 && (seq(tok[0], "cars") || seq(tok[0], "models"))) { car_cmd_cars(nt >= 2 ? tok[1] : NULL); return; }
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
        if (nt < 4 || !car_val(tok[3], &v, &mul)) { chat_usage("Формат: /vtune model ID параметр значение  (значение: 90 или x1.5; параметры: speed reverse force accel power inertia grip traction brake steer drag)"); return; }
        p = car_prof_get(model);
        if (!p) { chat_say("car: слишком много профилей"); return; }
        r = 0;
        for (i = 0; i < CAR_NROW; i++) if (seq(g_car_rows[i].cmd, tok[2])) { car_prof_put(p, g_car_rows[i].key, v, mul); r++; }
        if (!r) { chat_usage("Неизвестный параметр. Доступно: speed reverse force accel power inertia grip traction brake steer drag"); return; }
        car_prof_save(); g_car_dirty = 1;
        chat_say(g_car_auto ? "car: профиль модели сохранён (применится при посадке; авто — вкл)" : "car: профиль модели сохранён (включите /vtune auto on, чтобы применялся сам)");
        return;
    }
    if (nt >= 2 && (seq(tok[0], "find") || seq(tok[0], "go") || seq(tok[0], "найти") || seq(tok[0], "к"))) {   /* /vtune find|go car|npc ID — по ID экземпляра; работает и пешком */
        int kind = car_kind(tok[1]), go = !seq(tok[0], "find") && !seq(tok[0], "найти"); float fid;
        if (kind < 0) { chat_usage("Формат: /vtune find car|npc ID  — где он;  /vtune go car|npc ID — телепорт к нему;  /vtune find npc — ближайшие NPC с их ID"); return; }
        if (nt == 2) { if (kind == 1 && !go) { car_cmd_near_npc(); return; } chat_usage("Нужен ID экземпляра: /vtune go npc 123 (список ближайших NPC — /vtune find npc; ID своей машины покажет /vtune)"); return; }
        if (!car_num(tok[2], &fid)) { chat_usage("ID — число: /vtune go npc 123"); return; }
        car_cmd_find(kind, (int)(fid + 0.5f), go);
        return;
    }
    veh = car_my_vehicle(&drv);
    if (!veh) { chat_say("car: вы не в машине (или игра ещё грузится)"); return; }
    if (!drv) { chat_say("car: вы пассажир — настраивать может только водитель"); return; }
    if (nt >= 2 && (seq(tok[0], "from") || seq(tok[0], "copy"))) {      /* /vtune from 522 [all] — хендлинг модели 522 на вашу машину */
        float fid;
        if (!car_num(tok[1], &fid)) { chat_usage("Формат: /vtune from ID_модели [all]  (ID — из /vtune cars; all — скопировать ВСЮ структуру хендлинга, иначе только ездовые поля)"); return; }
        car_cmd_from(veh, (int)(fid + 0.5f), nt >= 3 && seq(tok[2], "all"));
        return;
    }
    if (nt == 0 || seq(tok[0], "info")) { car_info(veh); return; }
    if (seq(tok[0], "reset")) {
        CarOrig* o = car_orig_get(veh, 0); int n = 0;
        int hb = (g_car_hbak.veh == veh && g_car_hbak.id == car_veh_id(veh) && g_car_hbak.n > 0);
        if (!o && !hb) { chat_say("car: эта машина не менялась — сбрасывать нечего"); return; }
        if (hb) {                                                      /* вернуть ВСЮ структуру хендлинга, какой она была до /vtune from */
            void* hf = find_field(A.object_get_class(veh), "m_Handling");
            if (hf && hm_range_ok((char*)veh + A.field_get_offset(hf), g_car_hbak.n)) { memcpy((char*)veh + A.field_get_offset(hf), g_car_hbak.b, (unsigned long)g_car_hbak.n); n++; }
            g_car_hbak.n = 0;
        }
        if (o) for (i = 0; i < CAR_NROW; i++) if (o->have & (1u << i)) { float* p = car_fptr(veh, g_car_rows[i].where, g_car_rows[i].field); if (p) { *p = o->orig[i]; n++; } }
        if (o) o->wset = 0;
        car_call_update(veh);                         /* игра заново считает физику из вернувшегося хендлинга */
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
        int n, rev = 0; float *pw = car_fptr(veh, 2, "m_MaxPower"), pw0 = pw ? *pw : 0.0f;
        n = car_set_cmd(veh, tok[0], v, mul, &rev);
        if (!n) { chat_usage("vtune: не вышло — параметра нет или у этой техники нет такого поля (список: /vtune help)"); return; }
        g_car_lastVeh = veh; g_car_lastId = car_veh_id(veh);
        if (rev) chat_say("vtune: ВНИМАНИЕ — игра при пересчёте откатила значение хендлинга (берёт его из своей таблицы). Пришлите konstt.log и вывод /vtune");
        if (seq(tok[0], "accel")) {                                  /* разгон: хендлинг → пересчёт; если мощность двигателя не изменилась, умножаем её сами */
            pw = car_fptr(veh, 2, "m_MaxPower");
            if (pw && pw0 > 0.0f) {
                float d = *pw > pw0 ? *pw - pw0 : pw0 - *pw; char m[140]; Buf b; b_init(&b, m, sizeof m);
                if (d <= pw0 * 0.001f) {
                    CarOrig* o = car_orig_get(veh, 0); int ra = car_row_by_key("haccel");
                    float ratio = (o && ra >= 0 && (o->have & (1u << ra)) && o->orig[ra] > 0.0001f) ? o->want[ra] / o->orig[ra] : (mul ? v : 1.0f);
                    car_write_row(veh, car_row_by_key("power"), ratio, 1);
                    b_s(&b, "vtune: игра мощность из accel не пересчитала — умножил мощность двигателя напрямую (x"); b_fix(&b, ratio, 2); b_s(&b, ")");
                } else { b_s(&b, "vtune: игра пересчитала мощность двигателя: "); b_fix(&b, pw0, 1); b_s(&b, " -> "); b_fix(&b, *pw, 1); }
                chat_say(m);
            }
        }
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
