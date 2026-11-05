/*
 * hassle_drive.c — необязательный модуль «/drive»: записать маршрут ЕЗДЫ на машине и повторить его «один в один».
 *
 * Зависит только от ядра (hassle_core.c): включает его (#include "hassle_core.c"), сам запускает свой поток и сам регистрирует команду.
 * КАК УБРАТЬ: удалите hassle_drive.c и пересоберите — больше ничего править не нужно.
 *
 * Команда в чате (нативная, как /route и /rec):
 *   /drive rec          начать запись (вы за рулём); запись стартует, когда машина тронется (с места стоянки время не считается)
 *   /drive stop         остановить запись (или повтор); запись сохраняется в HLOG/drive.txt
 *   /drive play         повторить записанный маршрут (если в памяти ничего нет — берёт HLOG/drive.txt)
 *   /drive play force   то же, но без проверок «вы на месте старта, той же модели и в ту же сторону»
 *   /drive play noinput не писать руль/газ/тормоз в физику машины (только положение и скорость)
 *   /drive play kin     машина kinematic на время повтора (скорость в пакете станет 0 — только если без kin что-то мешает)
 *   /drive status       состояние, число точек, длительность, что сейчас уходит на сервер
 *
 * Почему так (по дизассемблеру LocalPlayer.SendDriveFullSyncData, RVA 0x12FA328):
 *   Пакет DriveSyncData игра собирает ИЗ СОСТОЯНИЯ машины в момент отправки (не чаще раза в 0.03 с, только если данные изменились):
 *     Position      = Transform.position машины (Player.Vehicle);   Rotation = Quaternion из Transform.eulerAngles;
 *     MoveSpeed     = Rigidbody.velocity / 51;                       VehicleHealth = BaseVehicle.Health;
 *     LeftRightAnalog = round(WheeledVehiclePhysics.SteerInput × 128);   UpDownAnalog = round(ThrottleInput × 128);
 *     Keys          = Character.GetKeys(): игра сама ставит клавиши, если ThrottleInput/BrakeInput/HandBrakeInput > 0.01.
 *   Поэтому «повтор ввода» (как у /route на ногах) для машины не даёт «один в один»: колёса считаются в FixedUpdate, и то же значение
 *   руля попадает в разные физические шаги. Здесь повторяется СОСТОЯНИЕ: каждый кадр мод читает позицию, поворот и скорость машины
 *   (и четыре физ-ввода), а при повторе ставит записанные значения обратно (по монотонным часам, с интерполяцией между кадрами).
 *   Игра сама собирает из них тот же пакет, ping на траекторию не влияет (пакет — снимок состояния на момент отправки).
 *
 * Как это устроено:
 *   • Тик в главном потоке — тот же приём, что у /route и /air: фоновый поток раз в ~12 мс ставит в очередь SynchronizationContext.Post
 *     делегат (эта же команда), ядро/игра выполняет его в главном потоке. Все вызовы IL2CPP — только из него.
 *   • Запись: пока машина стоит, держим «стартовую позу» (обновляется); когда скорость > 0.3 м/с или газ > 0.05 — старт записи.
 *     Дальше каждый тик (не чаще раза в 15 мс): t(мс), позиция, поворот-кватернион, скорость, руль/газ/тормоз/ручник.
 *     Запись заканчивается командой, выходом из машины, или когда заполнится (30000 точек / 10 минут). Хвост, где машина стоит, срезается.
 *   • Повтор: Transform.position/rotation (и Rigidbody.position/rotation, если это тот же Transform), Rigidbody.velocity, угловая скорость = 0;
 *     у Rigidbody на время повтора: detectCollisions = false, interpolation = None (иначе Unity подменяет Transform сглаженной позой). Всё
 *     возвращается при остановке. Физ-ввод (SteerInput, ThrottleInput, BrakeInput, HandBrakeInput) пишется из записи, но VehicleInput.Update
 *     перезаписывает его каждый кадр значениями с экрана, поэтому в пакет может попасть «ноль»: на положение это не влияет. Что реально
 *     уходит на сервер, видно в /drive status (Player.m_LastDriveSyncData, +0x130) и в konstt.log.
 *   • Защита: если машину сдвинули (откат сервером, RPC SetVehiclePos) — отклонение от записи > 6 м четыре тика подряд — повтор прерывается.
 *     Вышли из машины — прерывается. /drive stop — прерывает.
 *   • Старт повтора: машина в пределах 3 м от точки старта записи, та же модель, примерно в ту же сторону (иначе первый кадр — телепорт;
 *     /drive play force отключает проверки). Серверный античит (AC_TELEPORT) телепорт на старте может засчитать — риск на вас.
 *
 * Файл HLOG/drive.txt (текст):  H <ID модели> <ID машины>   затем строки: <мс> px py pz qx qy qz qw vx vy vz руль газ тормоз ручник
 *
 * ВАЖНО: код не запускался на устройстве — собран только компилятором. Поля читаются через IL2CPP по именам; если чего-то нет,
 * /drive скажет об этом и ничего не запишет. Подробный ход — в konstt.log (строки «drive: …»).
 */
#pragma once
#include "hassle_core.c"

struct dr_ts { long sec; long nsec; };
extern int clock_gettime(int, void*);

#define DR_MAX 30000                 /* точек записи */
#define DR_FBUF (4 << 20)            /* буфер файла: 30000 строк по ~120 байт */
#define DR_MAXMS 600000              /* запись/повтор не длиннее 10 минут */
#define DR_MINDT 15                  /* не чаще раза в столько мс кладём точку (на 120 к/с не раздувать файл) */
#define DR_START_DIST 3.0            /* повтор: не дальше стольких метров от точки старта записи */
#define DR_ABORT_D 6.0               /* отклонение от записи, после которого повтор прерывается, м */
#define DR_ABORT_N 4                 /* ... столько тиков подряд */
#define DR_FILE "drive.txt"

/* ───────────── данные ───────────── */
typedef struct { unsigned t; float p[3]; float q[4]; float v[3]; float in[4]; } DrS;   /* t — мс от начала; p — позиция; q — поворот; v — скорость; in — руль, газ, тормоз, ручник */
static DrS g_dr_s[DR_MAX];
static volatile int g_dr_n;
static volatile int g_dr_mode;                 /* 0 — ничего, 1 — запись, 2 — повтор */
static volatile int g_dr_tickPending;
static volatile long long g_dr_tickT;
static char g_dr_fbuf[DR_FBUF];
static int g_dr_model = -1, g_dr_vid = -1;     /* ID модели и ID экземпляра машины из записи */
static const char* const g_dr_inname[4] = { "SteerInput", "ThrottleInput", "BrakeInput", "HandBrakeInput" };

static int g_dr_started;                       /* в записи: машина уже тронулась */
static long long g_dr_t0;                      /* запись: момент t=0; повтор: момент старта повтора (мс, монотонные часы) */
static long long g_dr_prevTk;                  /* предыдущий тик записи */
static unsigned g_dr_lastT;                    /* t последней записанной точки */
static void* g_dr_veh;                         /* машина, с которой идёт запись/повтор */
static int g_dr_idx;                           /* повтор: индекс текущего отрезка */
static int g_dr_optNoInput, g_dr_optKin, g_dr_rbSame;
static int g_dr_sDc = -1, g_dr_sIp = -1, g_dr_sK = -1, g_dr_flagsOn;   /* исходные флаги Rigidbody */
static float g_dr_maxDev; static unsigned g_dr_maxDevT;                /* наибольшее отклонение «где машина была до постановки» от записи */
static int g_dr_bad;                           /* тиков подряд с большим отклонением */
static long long g_dr_lastLog;
static int g_dr_ticks;                         /* тиков повтора (для итога) */

/* ───────────── мелочи ───────────── */
static long long dr_now(void) {
    struct dr_ts t;
    if (clock_gettime(1, &t)) return 0;                          /* CLOCK_MONOTONIC */
    return (long long)t.sec * 1000 + t.nsec / 1000000;
}
static int dr_fin(const float* f, int n) { int i; for (i = 0; i < n; i++) if (!(f[i] == f[i]) || f[i] > 1e7f || f[i] < -1e7f) return 0; return 1; }
static double dr_sqrt(double v) { return v > 0 ? __builtin_sqrt(v) : 0; }
static double dr_dist(const float* a, const float* b) { double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return dr_sqrt(dx * dx + dy * dy + dz * dz); }
static double dr_speed(const float* v) { return dr_sqrt((double)v[0] * v[0] + (double)v[1] * v[1] + (double)v[2] * v[2]); }

/* ───────────── вызовы методов IL2CPP (только главный поток) ───────────── */
static void* dr_obj(void* obj, const char* name) {
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int dr_getf(void* obj, const char* name, float* out, int n) {        /* Vector3 (n=3) или Quaternion (n=4) из геттера без аргументов */
    void *m, *exc = NULL, *r; const float* f; int i;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, (unsigned)n * 4)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!dr_fin(f, n)) return 0;
    for (i = 0; i < n; i++) out[i] = f[i];
    return 1;
}
static int dr_setf(void* obj, const char* setter, const float* p, int n) {  /* сеттер с одним Vector3/Quaternion */
    void *m, *exc = NULL, *args[1]; float v[4]; int i;
    if (!obj) return 0;
    for (i = 0; i < n; i++) v[i] = p[i];
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = v;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
static int dr_getb(void* obj, const char* getter, int dflt) {
    void *m, *exc = NULL, *r;
    if (!obj) return dflt;
    m = find_method(A.object_get_class(obj), getter, 0);
    if (!m) return dflt;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r) return dflt;
    return *((u8*)r + HDR) ? 1 : 0;
}
static int dr_setb(void* obj, const char* setter, int v) {
    void *m, *exc = NULL, *args[1]; u8 b = v ? 1 : 0;
    if (!obj) return 0;
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = &b;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
static int dr_geti(void* obj, const char* getter, int dflt) {
    void *m, *exc = NULL, *r;
    if (!obj) return dflt;
    m = find_method(A.object_get_class(obj), getter, 0);
    if (!m) return dflt;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r) return dflt;
    return *(int*)((char*)r + HDR);
}
static int dr_seti(void* obj, const char* setter, int v) {
    void *m, *exc = NULL, *args[1]; int x = v;
    if (!obj) return 0;
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = &x;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
static int dr_fld(void* obj, const char* name, float* val, int set) {       /* float-поле объекта по имени: чтение или запись */
    void* f; int off; char* a;
    if (!obj) return 0;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return 0;
    off = A.field_get_offset(f);
    if (off < 0 || off > 0x4000) return 0;
    a = (char*)obj + off;
    if (!hm_readable(a, 4)) return 0;
    if (set) *(float*)a = *val; else *val = *(float*)a;
    return 1;
}

/* ───────────── машина игрока ───────────── */
static void* dr_my_vehicle(void** meOut, int* isDriver) {
    void *me, *veh, *drv;
    if (isDriver) *isDriver = 0;
    if (meOut) *meOut = NULL;
    if (!g_safe || !il2cpp_light()) return NULL;
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) return NULL;
    if (meOut) *meOut = me;
    veh = ref_field_s(me, "Vehicle");                            /* Player.Vehicle (+0xC0) */
    if (!veh || !hm_obj_ok(veh)) return NULL;
    drv = ref_field_s(veh, "m_Driver");
    if (isDriver) *isDriver = (!drv || drv == me);
    return veh;
}
static void* dr_tr(void* veh) {
    void* tr = dr_obj(veh, "get_Transform");
    if (!tr) tr = dr_obj(veh, "get_transform");
    return tr;
}
static void* dr_rb(void* veh) {                                  /* BaseVehicle.m_Rigidbody */
    void* rb = ref_field_s(veh, "m_Rigidbody");
    if (!rb || !hm_obj_ok(rb)) rb = dr_obj(veh, "get_Rigidbody");
    return (rb && hm_obj_ok(rb)) ? rb : NULL;
}
static void* dr_phys(void* veh) {                                /* WheeledVehicle.m_Physics (WheeledVehiclePhysics); у лодки/вертолёта его нет */
    void* ph = ref_field_s(veh, "m_Physics");
    return (ph && hm_obj_ok(ph)) ? ph : NULL;
}
static int dr_model_id(void* veh) {
    void* mi = ref_field_s(veh, "ModelInfo");
    return (mi && hm_obj_ok(mi)) ? int_field(mi, "ID", NULL, NULL, -1) : -1;
}

/* состояние машины: положение, поворот, скорость, физ-ввод */
static int dr_state(void* veh, DrS* s) {
    void *tr = dr_tr(veh), *rb = dr_rb(veh), *ph; int i;
    if (!tr || !rb) return 0;
    if (!dr_getf(tr, "get_position", s->p, 3) || !dr_getf(tr, "get_rotation", s->q, 4) || !dr_getf(rb, "get_velocity", s->v, 3)) return 0;
    ph = dr_phys(veh);
    for (i = 0; i < 4; i++) { s->in[i] = 0; if (ph) dr_fld(ph, g_dr_inname[i], &s->in[i], 0); }
    return 1;
}

/* что игра отправила на сервер последним (Player.m_LastDriveSyncData, +0x130): позиция, руль, газ, клавиши */
static int dr_pkt(void* me, float* pos, int* lr, int* ud, unsigned* keys) {
    void* f; char* a;
    if (!me) return 0;
    f = find_field(A.object_get_class(me), "m_LastDriveSyncData");
    if (!f) return 0;
    a = (char*)me + A.field_get_offset(f);
    if (!hm_readable(a, 0x4C)) return 0;
    pos[0] = *(float*)(a + 0x18); pos[1] = *(float*)(a + 0x1C); pos[2] = *(float*)(a + 0x20);
    *lr = *(short*)(a + 2); *ud = *(short*)(a + 4); *keys = *(u16*)(a + 6);
    return dr_fin(pos, 3);
}

/* ───────────── флаги Rigidbody на время повтора ───────────── */
static void dr_flags(void* veh, int on) {
    void* rb = veh ? dr_rb(veh) : NULL; static const float z[3] = { 0, 0, 0 };
    if (!rb) { if (!on) g_dr_flagsOn = 0; return; }
    if (on) {
        if (g_dr_flagsOn) return;
        g_dr_sDc = dr_getb(rb, "get_detectCollisions", -1);
        g_dr_sIp = dr_geti(rb, "get_interpolation", -1);
        g_dr_sK = dr_getb(rb, "get_isKinematic", -1);
        if (g_dr_sIp >= 0) dr_seti(rb, "set_interpolation", 0);        /* иначе Unity подменяет Transform сглаженной позой */
        dr_setb(rb, "set_detectCollisions", 0);
        if (g_dr_optKin) { dr_setf(rb, "set_velocity", z, 3); dr_setf(rb, "set_angularVelocity", z, 3); dr_setb(rb, "set_isKinematic", 1); }
        g_dr_flagsOn = 1;
    } else {
        if (!g_dr_flagsOn) return;
        if (g_dr_optKin) dr_setb(rb, "set_isKinematic", g_dr_sK < 0 ? 0 : g_dr_sK);
        dr_setb(rb, "set_detectCollisions", g_dr_sDc < 0 ? 1 : g_dr_sDc);
        if (g_dr_sIp >= 0) dr_seti(rb, "set_interpolation", g_dr_sIp);
        g_dr_flagsOn = 0;
    }
}

/* ───────────── интерполяция записи ───────────── */
static void dr_sample_at(double t, DrS* out, int* idx) {
    int i = *idx, n = g_dr_n, k; const DrS *a, *b; double span, f, dot, len;
    if (i < 0) i = 0;
    while (i + 2 < n && (double)g_dr_s[i + 1].t <= t) i++;
    *idx = i;
    a = &g_dr_s[i]; b = &g_dr_s[i + 1 < n ? i + 1 : i];
    span = (double)b->t - (double)a->t;
    f = span > 0 ? (t - (double)a->t) / span : 0;
    if (f < 0) f = 0; else if (f > 1) f = 1;
    for (k = 0; k < 3; k++) { out->p[k] = (float)(a->p[k] + (b->p[k] - a->p[k]) * f); out->v[k] = (float)(a->v[k] + (b->v[k] - a->v[k]) * f); }
    for (k = 0; k < 4; k++) out->in[k] = (float)(a->in[k] + (b->in[k] - a->in[k]) * f);
    dot = (double)a->q[0] * b->q[0] + (double)a->q[1] * b->q[1] + (double)a->q[2] * b->q[2] + (double)a->q[3] * b->q[3];
    {
        double sg = dot < 0 ? -1.0 : 1.0, q[4];                    /* кратчайший путь */
        for (k = 0; k < 4; k++) q[k] = a->q[k] + (sg * b->q[k] - a->q[k]) * f;
        len = dr_sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        if (len < 1e-6) for (k = 0; k < 4; k++) out->q[k] = a->q[k];
        else for (k = 0; k < 4; k++) out->q[k] = (float)(q[k] / len);
    }
    out->t = (unsigned)t;
}

/* ───────────── файл HLOG/drive.txt ───────────── */
static int dr_path(char* out, int cap, const char* name) {
    int i, last = -1; Buf b;
    if (!g_flog[0]) return 0;
    for (i = 0; g_flog[i]; i++) if (g_flog[i] == '/') last = i;
    if (last < 0 || last + 24 > cap) return 0;
    b_init(&b, out, (unsigned)cap);
    for (i = 0; i <= last; i++) b_ch(&b, g_flog[i]);
    b_s(&b, name);
    return 1;
}
static void dr_put_num(Buf* b, double v, int dec) { b_ch(b, ' '); b_fix(b, v, dec); }
static int dr_save(const char* name) {
    char path[320]; static char buf[8192]; Buf b; int i, n = g_dr_n, first = 1, k;
    if (!dr_path(path, sizeof path, name)) return 0;
    b_init(&b, buf, sizeof buf);
    b_s(&b, "H "); b_int(&b, g_dr_model); b_ch(&b, ' '); b_int(&b, g_dr_vid); b_ch(&b, '\n');
    for (i = 0; i < n; i++) {
        const DrS* s = &g_dr_s[i];
        b_int(&b, (long)s->t);
        for (k = 0; k < 3; k++) dr_put_num(&b, s->p[k], 3);
        for (k = 0; k < 4; k++) dr_put_num(&b, s->q[k], 5);
        for (k = 0; k < 3; k++) dr_put_num(&b, s->v[k], 3);
        for (k = 0; k < 4; k++) dr_put_num(&b, s->in[k], 3);
        b_ch(&b, '\n');
        if (b.n > sizeof buf - 220) {
            if (!flog_write(path, b.p, b.n, first)) return 0;
            first = 0; b_init(&b, buf, sizeof buf);
        }
    }
    if (b.n || first) if (!flog_write(path, b.p, b.n, first)) return 0;
    return 1;
}
static const char* dr_num(const char* s, double* out) {
    double v = 0, f = 1; int neg = 0, any = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { f /= 10; v += (*s - '0') * f; s++; any = 1; } }
    if (!any) return NULL;
    *out = neg ? -v : v;
    return s;
}
static int dr_load(const char* name) {
    char path[320]; int fd, cnt = 0; long tot = 0; const char* p;
    if (!dr_path(path, sizeof path, name)) return 0;
    fd = open(path, 0);
    if (fd < 0) return 0;
    for (;;) { long r = read(fd, g_dr_fbuf + tot, DR_FBUF - 1 - tot); if (r <= 0) break; tot += r; if (tot >= DR_FBUF - 1) break; }
    close(fd);
    g_dr_fbuf[tot] = 0;
    g_dr_model = -1; g_dr_vid = -1;
    for (p = g_dr_fbuf; *p; ) {
        const char* q = p; double v[15];
        if (*q == 'H') {
            q++;
            if ((q = dr_num(q, &v[0]))) { g_dr_model = (int)v[0]; if ((q = dr_num(q, &v[1]))) g_dr_vid = (int)v[1]; }
        } else if (*q >= '0' && *q <= '9') {
            int k, ok = 1;
            for (k = 0; k < 15 && ok; k++) { q = dr_num(q, &v[k]); if (!q) ok = 0; }
            if (ok && cnt < DR_MAX) {
                DrS* s = &g_dr_s[cnt];
                s->t = (unsigned)v[0];
                for (k = 0; k < 3; k++) s->p[k] = (float)v[1 + k];
                for (k = 0; k < 4; k++) s->q[k] = (float)v[4 + k];
                for (k = 0; k < 3; k++) s->v[k] = (float)v[8 + k];
                for (k = 0; k < 4; k++) s->in[k] = (float)v[11 + k];
                cnt++;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    g_dr_n = cnt;
    return cnt;
}

static double dr_path_len(void) {
    double d = 0; int i;
    for (i = 1; i < g_dr_n; i++) d += dr_dist(g_dr_s[i - 1].p, g_dr_s[i].p);
    return d;
}

/* ───────────── очередь тиков в главный поток ───────────── */
static void native_drive_cmd(void* self, void* args, void* mi);
static void dr_post(void) {                                      /* фоновый поток: поставить тик в очередь главного потока */
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { g_dr_tickPending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_drive_cmd);
    st = A.string_new("drive-tick");
    if (!cb || !st) { g_dr_tickPending = 0; dlog("drive: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("drive: Post бросил: ", et); g_dr_tickPending = 0; }
}
static void* dr_worker(void* arg) {
    (void)arg;
    for (;;) {
        int mode = __atomic_load_n(&g_dr_mode, __ATOMIC_ACQUIRE);
        if (mode == 0) { usleep(40000); continue; }
        if (!g_dr_tickPending || dr_now() - g_dr_tickT > 1000) {   /* предыдущий тик ещё не выполнен — очередь не копим */
            g_dr_tickPending = 1; g_dr_tickT = dr_now();
            dr_post();
        }
        usleep(12000);
    }
    return NULL;
}

/* ───────────── запись ───────────── */
static void b_pos3(Buf* b, const float* p) { b_fix(b, p[0], 1); b_s(b, ", "); b_fix(b, p[1], 1); b_s(b, ", "); b_fix(b, p[2], 1); }

static void dr_rec_finish(const char* why) {                     /* главный поток: закончить запись, обрезать стоячий хвост, сохранить */
    Buf b; char msg[400]; int n;
    g_dr_mode = 0;
    n = g_dr_n;
    if (!g_dr_started || n < 2) {
        g_dr_n = 0;
        dlog2("drive: запись отменена: ", why);
        chat_say("Запись отменена: машина так и не тронулась");
        return;
    }
    while (n > 2 && dr_speed(g_dr_s[n - 1].v) < 0.3 && dr_speed(g_dr_s[n - 2].v) < 0.3) n--;   /* срезать хвост, где стоим */
    g_dr_n = n;
    b_init(&b, msg, sizeof msg);
    if (dr_save(DR_FILE)) {
        b_s(&b, "Запись сохранена ("); b_s(&b, why); b_s(&b, "): "); b_int(&b, n); b_s(&b, " точек, ");
        b_fix(&b, (double)g_dr_s[n - 1].t / 1000.0, 1); b_s(&b, " с, путь ");
        b_fix(&b, dr_path_len(), 0); b_s(&b, " м. /drive play — повторить");
    } else {
        b_s(&b, "Запись есть в памяти ("); b_int(&b, n); b_s(&b, " точек), но файл не записался: см. konstt.log. /drive play всё равно повторит её");
    }
    dlog2("drive: ", msg);
    chat_say(msg);
}

static void dr_rec_step(void) {
    long long now = dr_now(); void *me, *veh; int drv; DrS cur;
    veh = dr_my_vehicle(&me, &drv);
    if (!veh || !drv || (g_dr_veh && veh != g_dr_veh)) { dr_rec_finish(!veh ? "вы вышли из машины" : !drv ? "вы не водитель" : "другая машина"); return; }
    if (!dr_state(veh, &cur)) {
        static int warned; if (!warned) { warned = 1; dlog("drive: состояние машины не читается (Transform/Rigidbody)"); }
        return;
    }
    if (!g_dr_started) {                                         /* ждём, пока машина тронется; стартовая поза — самая свежая */
        double sp = dr_speed(cur.v);
        if (sp > 0.3 || cur.in[1] > 0.05f || cur.in[1] < -0.05f) {
            if (g_dr_n < 1) {                                    /* /drive rec дали уже на ходу: стартовой позы покоя нет — начинаем с этой точки */
                g_dr_started = 1; g_dr_t0 = now; cur.t = 0; g_dr_s[0] = cur; g_dr_n = 1; g_dr_lastT = 0; g_dr_prevTk = now;
                dlog("drive: запись начата на ходу (машина уже ехала)");
                return;
            }
            g_dr_started = 1;
            g_dr_t0 = g_dr_prevTk ? g_dr_prevTk : now - 16;      /* t=0 — предыдущий тик: там машина ещё стояла в стартовой позе */
            cur.t = (unsigned)(now - g_dr_t0);
            g_dr_s[1] = cur; g_dr_n = 2; g_dr_lastT = cur.t;
            dlog("drive: машина тронулась — идёт запись");
        } else {
            cur.t = 0; cur.v[0] = cur.v[1] = cur.v[2] = 0; cur.in[0] = cur.in[1] = cur.in[2] = cur.in[3] = 0;
            g_dr_s[0] = cur; g_dr_n = 1; g_dr_prevTk = now;
        }
        return;
    }
    {
        long long dt = now - g_dr_t0;
        g_dr_prevTk = now;
        if (dt - (long long)g_dr_lastT < DR_MINDT) return;
        if (g_dr_n >= DR_MAX) { dr_rec_finish("запись заполнена"); return; }
        if (dt > DR_MAXMS) { dr_rec_finish("прошло 10 минут"); return; }
        cur.t = (unsigned)dt;
        g_dr_s[g_dr_n++] = cur; g_dr_lastT = cur.t;
    }
}

/* ───────────── повтор ───────────── */
static void dr_play_end(const char* why, int ok) {
    void *veh = g_dr_veh; Buf b; char msg[500]; DrS last; int hasLast = g_dr_n > 0;
    if (hasLast) last = g_dr_s[g_dr_n - 1];
    g_dr_mode = 0;
    if (veh && hm_obj_ok(veh)) {
        if (ok && hasLast && !g_dr_optKin) { void* rb = dr_rb(veh); if (rb) dr_setf(rb, "set_velocity", last.v, 3); }   /* дальше — как в записи: докатываемся сами */
        dr_flags(veh, 0);
    } else g_dr_flagsOn = 0;
    b_init(&b, msg, sizeof msg);
    b_s(&b, ok ? "Повтор закончен" : "Повтор прерван"); b_s(&b, " ("); b_s(&b, why); b_s(&b, ")");
    if (g_dr_ticks > 0) { b_s(&b, ". Наибольшее отклонение от записи за кадр: "); b_fix(&b, g_dr_maxDev, 2); b_s(&b, " м (на "); b_fix(&b, (double)g_dr_maxDevT / 1000.0, 1); b_s(&b, " с)"); }
    if (ok && hasLast) {
        float pos[3];
        void* tr = veh ? dr_tr(veh) : NULL;
        if (tr && dr_getf(tr, "get_position", pos, 3)) { b_s(&b, ", до конца записи осталось "); b_fix(&b, dr_dist(pos, last.p), 2); b_s(&b, " м"); }
    }
    dlog2("drive: ", msg);
    chat_say(msg);
    g_dr_veh = NULL;
}

static void dr_play_step(void) {
    long long now = dr_now(); double t; void *me, *veh, *tr, *rb, *ph; int drv, k; DrS tg, act; float dev;
    veh = dr_my_vehicle(&me, &drv);
    if (!veh || !drv || veh != g_dr_veh) { dr_play_end(!veh ? "вы вышли из машины" : !drv ? "вы не водитель" : "другая машина", 0); return; }
    t = (double)(now - g_dr_t0);
    if (t > DR_MAXMS + 1000) { dr_play_end("прошло слишком много времени", 0); return; }
    if (t >= (double)g_dr_s[g_dr_n - 1].t) {                     /* конец записи: ставим последнее состояние и заканчиваем */
        tg = g_dr_s[g_dr_n - 1];
        tr = dr_tr(veh); rb = dr_rb(veh);
        if (tr) { dr_setf(tr, "set_position", tg.p, 3); dr_setf(tr, "set_rotation", tg.q, 4); }
        if (rb && g_dr_rbSame) { dr_setf(rb, "set_position", tg.p, 3); dr_setf(rb, "set_rotation", tg.q, 4); }
        dr_play_end("конец записи", 1);
        return;
    }
    dr_sample_at(t, &tg, &g_dr_idx);
    tr = dr_tr(veh); rb = dr_rb(veh);
    if (!tr || !rb) { dr_play_end("у машины пропал Transform/Rigidbody", 0); return; }
    /* где машина оказалась до нашей постановки (физика за кадр + любые чужие толчки) — по этому судим о точности и о «машину сдвинули» */
    if (!dr_getf(tr, "get_position", act.p, 3)) { dr_play_end("позиция машины не читается", 0); return; }
    dev = (float)dr_dist(act.p, tg.p);
    if (g_dr_ticks > 2 && dev > g_dr_maxDev) { g_dr_maxDev = dev; g_dr_maxDevT = (unsigned)t; }
    if (dev > DR_ABORT_D && g_dr_ticks > 2) { if (++g_dr_bad >= DR_ABORT_N) { dr_play_end("машину сдвинули (откат сервера?)", 0); return; } }
    else g_dr_bad = 0;
    /* постановка состояния */
    if (!dr_setf(tr, "set_position", tg.p, 3) || !dr_setf(tr, "set_rotation", tg.q, 4)) { dr_play_end("Transform.set_position/rotation не сработал", 0); return; }
    if (g_dr_rbSame) { dr_setf(rb, "set_position", tg.p, 3); dr_setf(rb, "set_rotation", tg.q, 4); }
    if (!g_dr_optKin) {
        static const float z[3] = { 0, 0, 0 };
        dr_setf(rb, "set_velocity", tg.v, 3);
        dr_setf(rb, "set_angularVelocity", z, 3);
    }
    if (!g_dr_optNoInput) { ph = dr_phys(veh); if (ph) for (k = 0; k < 4; k++) dr_fld(ph, g_dr_inname[k], &tg.in[k], 1); }
    g_dr_ticks++;
    if (now - g_dr_lastLog >= 2000) {                            /* журнал раз в 2 с: цель, факт, что ушло на сервер */
        char lg[420]; Buf b; float pp[3]; int lr = 0, ud = 0; unsigned keys = 0; int hp = dr_pkt(me, pp, &lr, &ud, &keys);
        g_dr_lastLog = now;
        b_init(&b, lg, sizeof lg);
        b_s(&b, "drive: повтор t="); b_fix(&b, t / 1000.0, 1); b_s(&b, " с, цель ("); b_pos3(&b, tg.p); b_s(&b, "), откл. за кадр "); b_fix(&b, dev, 2);
        b_s(&b, " м, скорость "); b_fix(&b, dr_speed(tg.v), 1);
        if (hp) { b_s(&b, " | пакет: ("); b_pos3(&b, pp); b_s(&b, ") руль "); b_int(&b, lr); b_s(&b, " газ "); b_int(&b, ud); b_s(&b, " клав "); b_int(&b, (long)keys); }
        b_s(&b, " | записано: руль "); b_fix(&b, tg.in[0] * 128.0, 0); b_s(&b, " газ "); b_fix(&b, tg.in[1] * 128.0, 0);
        dlog(lg);
    }
}

static void dr_tick(void) {                                      /* главный поток */
    int mode = __atomic_load_n(&g_dr_mode, __ATOMIC_ACQUIRE);
    g_dr_tickPending = 0;
    if (mode == 1) dr_rec_step(); else if (mode == 2) dr_play_step();
}

/* ───────────── команда ───────────── */
/* слова: 1 rec, 2 stop, 4 play, 16 status, 64 force, 128 noinput, 256 kin */
static int dr_parse(void* arr) {
    long len; int i, w = 0; char* data;
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
            if (seq(tok, "rec") || seq(tok, "record") || seq(tok, "запись")) w |= 1;
            else if (seq(tok, "stop") || seq(tok, "cancel") || seq(tok, "стоп")) w |= 2;
            else if (seq(tok, "play") || seq(tok, "повтор")) w |= 4;
            else if (seq(tok, "status") || seq(tok, "diag") || seq(tok, "статус")) w |= 16;
            else if (seq(tok, "force")) w |= 64;
            else if (seq(tok, "noinput")) w |= 128;
            else if (seq(tok, "kin")) w |= 256;
        }
    }
    return w;
}

static void native_drive_cmd(void* self, void* args, void* mi) {
    int w, drv = 0; Buf b; char msg[700]; void *me = NULL, *veh;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { dr_tick(); return; }   /* вызов из Post (тик), не из чата */
    mt_capture_ctx();                                            /* главный поток: берём контекст для тиков */
    w = dr_parse(args);
    dlog_i("нативная /drive: вызвана, флаги ", w);
    b_init(&b, msg, sizeof msg);

    if (w & 16) {                                                /* status */
        float pp[3]; int lr = 0, ud = 0; unsigned keys = 0;
        veh = dr_my_vehicle(&me, &drv);
        b_s(&b, "Drive: "); b_s(&b, g_dr_mode == 1 ? (g_dr_started ? "идёт запись" : "запись ждёт, пока машина тронется") : g_dr_mode == 2 ? "идёт повтор" : "ничего не идёт");
        b_s(&b, ", точек в памяти "); b_int(&b, g_dr_n);
        if (g_dr_n > 1) { b_s(&b, " ("); b_fix(&b, (double)g_dr_s[g_dr_n - 1].t / 1000.0, 1); b_s(&b, " с, "); b_fix(&b, dr_path_len(), 0); b_s(&b, " м)"); }
        b_s(&b, g_mt_ctx ? ", тики: да" : ", тики: нет");
        if (veh) {
            b_s(&b, drv ? " | вы за рулём, модель " : " | вы пассажир, модель "); b_int(&b, dr_model_id(veh));
            if (me && dr_pkt(me, pp, &lr, &ud, &keys)) { b_s(&b, " | в пакете: ("); b_pos3(&b, pp); b_s(&b, "), руль "); b_int(&b, lr); b_s(&b, ", газ "); b_int(&b, ud); b_s(&b, ", клавиши "); b_int(&b, (long)keys); }
        } else b_s(&b, " | вы не в машине");
        chat_say(msg);
        return;
    }
    if (w & 2) {                                                 /* stop */
        if (g_dr_mode == 1) { dr_rec_finish("остановлено командой"); return; }
        if (g_dr_mode == 2) { dr_play_end("остановлено командой", 0); return; }
        chat_say("Drive: сейчас ничего не идёт");
        return;
    }
    if (w & 1) {                                                 /* rec */
        if (g_dr_mode != 0) { chat_say("Drive: сначала /drive stop"); return; }
        veh = dr_my_vehicle(&me, &drv);
        if (!veh) { chat_say("Drive: сядьте в машину за руль и повторите /drive rec"); return; }
        if (!drv) { chat_say("Drive: вы пассажир — запись только за рулём"); return; }
        {
            DrS cur;
            if (!dr_state(veh, &cur)) { chat_say("Drive: состояние машины не читается (подробности в konstt.log)"); dlog("drive: rec: Transform/Rigidbody не прочитались"); return; }
            g_dr_veh = veh; g_dr_started = 0; g_dr_n = 0; g_dr_prevTk = 0; g_dr_lastT = 0;
            g_dr_model = dr_model_id(veh); g_dr_vid = (int)num_field(veh, "ID", NULL);
        }
        g_dr_mode = 1;
        chat_say("Drive: запись пошла — трогайтесь, время с места не считается. /drive stop — закончить");
        return;
    }
    if (w & 4) {                                                 /* play */
        DrS cur; int force = (w & 64) != 0; void *tr, *rb; double d; float dot;
        if (g_dr_mode != 0) { chat_say("Drive: сначала /drive stop"); return; }
        if (g_dr_n < 2 && dr_load(DR_FILE) < 2) { chat_say("Drive: записи нет — сначала /drive rec (файл drive.txt пуст или его нет)"); return; }
        veh = dr_my_vehicle(&me, &drv);
        if (!veh) { chat_say("Drive: сядьте в машину за руль"); return; }
        if (!drv) { chat_say("Drive: вы пассажир — повтор только за рулём"); return; }
        if (!dr_state(veh, &cur)) { chat_say("Drive: состояние машины не читается (подробности в konstt.log)"); return; }
        d = dr_dist(cur.p, g_dr_s[0].p);
        dot = cur.q[0] * g_dr_s[0].q[0] + cur.q[1] * g_dr_s[0].q[1] + cur.q[2] * g_dr_s[0].q[2] + cur.q[3] * g_dr_s[0].q[3];
        if (dot < 0) dot = -dot;
        if (!force) {
            if (g_dr_model >= 0 && dr_model_id(veh) != g_dr_model) {
                b_s(&b, "Drive: запись сделана на другой модели (в записи "); b_int(&b, g_dr_model); b_s(&b, ", у вас "); b_int(&b, dr_model_id(veh)); b_s(&b, "). /drive play force — всё равно");
                chat_say(msg); return;
            }
            if (d > DR_START_DIST) {
                b_s(&b, "Drive: вы в "); b_fix(&b, d, 1); b_s(&b, " м от места старта записи (нужно не дальше "); b_fix(&b, DR_START_DIST, 0); b_s(&b, " м). Старт: ");
                b_pos3(&b, g_dr_s[0].p); b_s(&b, ". /drive play force — всё равно");
                chat_say(msg); return;
            }
            if (dot < 0.966f) { chat_say("Drive: машина развёрнута больше чем на 30° от положения при старте записи. /drive play force — всё равно"); return; }
        }
        tr = dr_tr(veh); rb = dr_rb(veh);
        if (!tr || !rb) { chat_say("Drive: у машины не найден Transform/Rigidbody"); return; }
        g_dr_optNoInput = (w & 128) != 0; g_dr_optKin = (w & 256) != 0;
        g_dr_rbSame = (dr_obj(rb, "get_transform") == tr);
        g_dr_veh = veh; g_dr_idx = 0; g_dr_bad = 0; g_dr_ticks = 0; g_dr_maxDev = 0; g_dr_maxDevT = 0; g_dr_lastLog = 0;
        dr_flags(veh, 1);
        g_dr_t0 = dr_now();
        g_dr_mode = 2;
        b_s(&b, "Drive: повтор пошёл ("); b_int(&b, g_dr_n); b_s(&b, " точек, "); b_fix(&b, (double)g_dr_s[g_dr_n - 1].t / 1000.0, 1); b_s(&b, " с"); if (force && d > DR_START_DIST) { b_s(&b, ", старт в "); b_fix(&b, d, 1); b_s(&b, " м от записи"); } b_s(&b, "). /drive stop — прервать");
        dlog2("drive: ", msg);
        chat_say(msg);
        dr_play_step();                                          /* первое состояние — сразу, не ждать тика */
        return;
    }
    chat_usage("Drive: /drive rec — запись, /drive stop — стоп, /drive play [force] [noinput] [kin] — повтор, /drive status");
}

/* Ждёт ядро, готовность игры и образец делегата, регистрирует /drive (hm_cmd_install), потом запускает поток-«тикер». */
static int g_drive_modId = -1;                           /* номер в реестре модов (экран загрузки) */
static void* dr_main(void* arg) {
    unsigned long t = 0;
    (void)arg;
    if (!hm_cmd_install("drive", native_drive_cmd)) return NULL;
    hm_mod_ready(g_drive_modId);
    pthread_create(&t, NULL, dr_worker, NULL);
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void drive_ctor(void) {
    unsigned long t = 0;
    g_drive_modId = hm_mod_register("drive");
    pthread_create(&t, NULL, dr_main, NULL);
}
#endif
