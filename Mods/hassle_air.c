/*
 * hassle_air.c — необязательный модуль «/air»: режим «полёта» для разбора завис­ших NPC и машин. Вы поднимаетесь над землёй, ходите по воздуху
 * (куда смотрит камера — туда и летите) и проходите сквозь машины, NPC и карту, поэтому можно спокойно рассмотреть кучу изнутри и сверху.
 * Дополняет /vtune (ID NPC и телепорт к нему) и /tpl (точки, призрак): /air npc показывает те же ID, что /vtune find npc.
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_air.c и пересоберите — больше ничего править не нужно.
 *
 * Команды:
 *   /air                  включить (поднимет на 2.5 м) ↔ выключить
 *   /air on [м]           включить, подняться на столько метров (0.5…60);  /air off — выключить;  /air land — выключить и поставить на землю
 *   /air up [м]  /air down [м]   сместиться вверх/вниз (по умолчанию 1 м)
 *   /air speed N          скорость полёта, м/с (по умолчанию 6, до 40)
 *   /air flat | 3d        flat — по джойстику летите только горизонтально (высота — командами up/down);
 *                         3d (по умолчанию) — вперёд летите туда, куда смотрит камера, в т.ч. вверх/вниз: посмотрели вниз на кучу — и опускаетесь к ней
 *   /air npc              ближайшие NPC: ID, расстояние, на сколько выше/ниже вас (ID — те же, что в /vtune find npc и в серверном коде)
 *   /air npc ID  |  /air to ID    перелететь на 3 м над NPC с этим ID (включит полёт сам)
 *   /air watch            вкл/выкл: пока летите, мод пишет в чат ID ближайшего NPC, когда он меняется (подлетели к другому — увидите, кто это)
 *   /air input            переключить источник джойстика (если «не летит» — смотрите /air status и пробуйте этот вариант)
 *   /air status           состояние и сырые значения ввода;  /air help — подсказка
 *
 * Как это работает (поля — из дампа IL2CPP, читаются по ИМЕНАМ, не по смещениям)
 *   • Позиция держится нашим кодом: каждый кадр главному потоку приходит «тик» (тот же приём, что у /tpl и /route: рабочий поток кладёт
 *     колбэк в UnitySynchronizationContext), в нём мы считаем новую точку = прежняя + направление джойстика × скорость × dt и ставим
 *     персонажа туда ICharacterMovement.SetPosition(Vector3, updateGround=false) — это прямая постановка капсулы, как «/tpl alt»,
 *     и обнуляем ICharacterMovement.velocity. Так игра не может ни уронить вас (гравитацию мы просто не используем), ни толкнуть.
 *   • Направление: Character.m_MovementInput (Vector2: x — вправо, y — вперёд) и камера Game.MainCamera: «вправо» берётся по горизонтали,
 *     «вперёд» — по камере (3d) либо горизонтально (flat). Если джойстик не читается, /air input переключает на Character.MovementDirection
 *     (мировой вектор; осторожно: если игра не обнуляет его в покое, вас будет сносить — тогда верните /air input ещё раз).
 *   • Сквозь объекты: пока режим включён, у движения выключено detectCollisions (CharacterMovement._detectCollisions /
 *     SimpleCharacterMovement.m_DetectCollisions) и constrainToGround (не «прилипать» к земле), а у Rigidbody персонажа — detectCollisions:
 *     иначе вы толкали бы кузова машин и капсулы NPC. Всё это возвращается обратно при выключении (запоминаются исходные значения).
 *   • Сервер НИЧЕГО не знает про режим: позиция уходит на него как обычная — другие игроки видят вас в воздухе, а серверный античит может
 *     среагировать на высоту/скорость. Чтобы сервер вас «не видел», включите /tpl ghost до /air (позиция на сервер не уходит вовсе);
 *     учтите: в призраке сервер и его NPC считают, что вы стоите на старом месте.
 *   • Выключение: настоящая гравитация вернётся, и вы упадёте. Если вы выше 5 м над точкой включения, мод сам ставит вас на землю
 *     (Player.SetPosition(pos, findHeight=true) — тот же вызов, что у «/tpl to X Z»), чтобы не разбиться; /air land делает это всегда.
 *   • В машине режим не работает (при посадке выключается сам). Если игра переставила вас далеко (сервер вернул/телепортировал) — держим новую точку.
 *
 * ВАЖНО: код не запускался на устройстве — собран только компилятором. Если что-то не сработало, /air status и HLOG/konstt.log (строки «air:»).
 */
#pragma once
#include "hassle_core.c"
extern int clock_gettime(int, void*);       /* как в mem/route/tpl: struct timespec = два long */

#define AIR_LIFT 2.5f
#define AIR_MAXLIFT 60.0f
#define AIR_SPEED 6.0f
#define AIR_MAXSPEED 40.0f
#define AIR_TICK_US 8000                          /* период рабочего потока; реально тик идёт раз в кадр (пока прошлый не отработал, новый не шлём) */
#define AIR_NEAR 8
#define AIR_JUMP 6.0f                             /* если игра сама сдвинула вас дальше, чем на столько метров за кадр — считаем это перестановкой и берём новую точку */
#define AIR_AUTOLAND 5.0f                         /* выше этого над точкой включения при выключении ставим на землю */

static volatile int g_air_on;
static volatile int g_air_pending;
static volatile long long g_air_pendT;
static int g_air_thread;
static int g_air_modId = -1;
static float g_air_speed = AIR_SPEED;
static int g_air_look3d = 1;
static int g_air_inSrc;                           /* 0 — Character.m_MovementInput, 1 — Character.MovementDirection */
static int g_air_watch;
static float g_air_hold[3], g_air_last[3], g_air_y0;
static long long g_air_tPrev;
static void* g_air_ctrl;                          /* контроллер, к которому уже применены флаги (сменился — персонаж пересоздан, применяем заново) */
static int g_air_oCg = -1, g_air_oDc = -1, g_air_oRb = -1, g_air_saved;     /* исходные значения флагов (-1 — не прочиталось, считаем «включено») */
static int g_air_jumps;
static int g_air_watchId = -1; static long long g_air_watchT;
static float g_air_rawIn[3];                      /* последние сырые значения ввода — для /air status */

/* ───────────── мелкие помощники ───────────── */
static long long air_now_ns(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return (long long)ts[0] * 1000000000LL + ts[1]; }
static int air_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
static float air_dist(const float* a, const float* b) { double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz); }
static void air_b_xyz(Buf* b, const float* p) { b_fix(b, p[0], 1); b_ch(b, ' '); b_fix(b, p[1], 1); b_ch(b, ' '); b_fix(b, p[2], 1); }
static int air_num(const char* t, float* out) {          /* «12», «12.5», «12,5» (без знака) */
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
/* метод без аргументов, вернувший ссылочный объект */
static void* air_obj(void* obj, const char* name) {
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
/* метод без аргументов, вернувший Vector3 (в боксе: 3 float после заголовка) */
static int air_vec(void* obj, const char* name, float* out) {
    void *m, *exc = NULL, *r; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!air_finite(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
/* геттер bool: dflt — если метода нет или он бросил */
static int air_get_bool(void* obj, const char* getter, int dflt) {
    void *m, *exc = NULL, *r;
    if (!obj) return dflt;
    m = find_method(A.object_get_class(obj), getter, 0);
    if (!m) return dflt;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r) return dflt;
    return *((u8*)r + HDR) ? 1 : 0;
}
/* сеттер с одним bool-аргументом. 1 — вызвали без исключения */
static int air_set_bool(void* obj, const char* setter, int v) {
    void *m, *exc = NULL, *args[1]; u8 b = v ? 1 : 0;
    if (!obj) return 0;
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = &b;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
/* bool-поле по имени (в самом объекте). 1 — записали */
static int air_field_bool(void* obj, const char* name, int v, int* old) {
    void* f = find_field(A.object_get_class(obj), name); u8* p;
    if (!f || (A.field_get_flags(f) & 0x10)) return 0;
    p = (u8*)obj + A.field_get_offset(f);
    if (!hm_readable(p, 1)) return 0;
    if (old) *old = *p ? 1 : 0;
    *p = v ? 1 : 0;
    return 1;
}
/* Transform камеры (Game.MainCamera → m_Camera → transform), как в /tpl и /route */
static void* air_cam_tr(void) {
    void *fn, *cam = NULL, *co;
    if (!g_cGame) return NULL;
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) return NULL;
    A.field_static_get_value(fn, &cam);
    if (!cam || !hm_obj_ok(cam)) return NULL;
    co = ref_field_s(cam, "m_Camera");
    if (!co || !hm_obj_ok(co)) return NULL;
    return air_obj(co, "get_transform");
}
/* поставить компонент движения в точку: ICharacterMovement.SetPosition(Vector3, updateGround=false) */
static int air_call_setpos(void* mv, const float* pos, const char* what) {
    void *m, *exc = NULL, *args[2]; float v[3]; u8 flag = 0; char et[240];
    v[0] = pos[0]; v[1] = pos[1]; v[2] = pos[2];
    m = find_method(A.object_get_class(mv), "SetPosition", 2);
    if (!m) { dlog2("air: SetPosition не найден у ", what); return 0; }
    args[0] = v; args[1] = &flag;
    A.runtime_invoke(m, mv, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("air: SetPosition бросил исключение: ", et); return 0; }
    return 1;
}
static void air_zero_vel(void* mv) {
    void *m, *exc = NULL, *args[1]; float z[3];
    z[0] = z[1] = z[2] = 0.0f;
    m = find_method(A.object_get_class(mv), "set_velocity", 1);
    if (!m) return;
    args[0] = z;
    A.runtime_invoke(m, mv, args, &exc);
}

/* ───────────── контекст: игрок и его компоненты (главный поток) ───────────── */
typedef struct { void *me, *ctrl, *mv, *rb, *etr, *mtr, *veh; } AirCtx;
static int air_ctx(AirCtx* c, int quiet) {
    c->me = c->ctrl = c->mv = c->rb = c->etr = c->mtr = c->veh = NULL;
    if (!g_safe || !il2cpp_light()) { if (!quiet) chat_say("/air: игра ещё грузится, подождите"); return 0; }
    ensure_attached();
    c->me = local_player();
    if (!c->me || !hm_obj_ok(c->me)) { if (!quiet) chat_say("/air: вы не в игре (персонаж не создан)"); return 0; }
    c->veh = ref_field_s(c->me, "Vehicle");                           /* Player.Vehicle */
    if (c->veh && !hm_obj_ok(c->veh)) c->veh = NULL;
    c->ctrl = ref_field_s(c->me, "m_Controller");                     /* Actor.m_Controller (ThirdPersonCharacter) */
    if (c->ctrl && !hm_obj_ok(c->ctrl)) c->ctrl = NULL;
    if (c->ctrl) {
        c->mv = ref_field_s(c->ctrl, "Movement");                     /* Character.Movement (ICharacterMovement) */
        if (c->mv && !hm_obj_ok(c->mv)) c->mv = NULL;
        c->rb = ref_field_s(c->ctrl, "m_Rigidbody");                  /* Character.m_Rigidbody */
        if (c->rb && !hm_obj_ok(c->rb)) c->rb = NULL;
    }
    c->etr = air_obj(c->me, "get_Transform");                         /* Entity.Transform */
    if (!c->etr) c->etr = air_obj(c->me, "get_transform");
    if (c->mv) c->mtr = air_obj(c->mv, "get_transform");              /* transform компонента движения */
    return 1;
}
/* mP — позиция в системе transform движения (в ней же ставим SetPosition), eP — Entity.Transform (в ней же позиции NPC); любой указатель можно NULL */
static int air_mypos(const AirCtx* c, float* mP, float* eP) {
    int ok = 0;
    if (mP) { if (c->mtr && air_vec(c->mtr, "get_position", mP)) ok = 1; else if (c->etr && air_vec(c->etr, "get_position", mP)) ok = 1; }
    else ok = 1;
    if (eP) { if (c->etr && air_vec(c->etr, "get_position", eP)) {} else if (mP) { eP[0] = mP[0]; eP[1] = mP[1]; eP[2] = mP[2]; } else ok = 0; }
    return ok;
}

/* ───────────── флаги «сквозь всё» ───────────── */
static void air_flags(const AirCtx* c, int on) {
    int cgOk = 0, dcOk = 0, rbOk = 0;
    if (!c->mv) return;
    if (on && !g_air_saved) {                                        /* исходные значения запоминаем один раз за включение */
        int old = -1;
        g_air_oCg = air_get_bool(c->mv, "get_constrainToGround", -1);
        g_air_oDc = air_get_bool(c->mv, "get_detectCollisions", -1);   /* есть у CharacterMovement; у SimpleCharacterMovement — только поле */
        if (g_air_oDc < 0 && (air_field_bool(c->mv, "m_DetectCollisions", 1, &old) || air_field_bool(c->mv, "_detectCollisions", 1, &old))) g_air_oDc = old;
        g_air_oRb = c->rb ? air_get_bool(c->rb, "get_detectCollisions", -1) : -1;
        g_air_saved = 1;
        { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "air: исходные флаги: constrainToGround "); b_int(&x, g_air_oCg); b_s(&x, ", detectCollisions движения "); b_int(&x, g_air_oDc);
          b_s(&x, ", Rigidbody "); b_int(&x, g_air_oRb); b_s(&x, ", движение: "); b_s(&x, A.class_get_name(A.object_get_class(c->mv))); dlog(lg); }
    }
    {
        int cg = on ? 0 : (g_air_oCg < 0 ? 1 : g_air_oCg);
        int dc = on ? 0 : (g_air_oDc < 0 ? 1 : g_air_oDc);
        int rb = on ? 0 : (g_air_oRb < 0 ? 1 : g_air_oRb);
        cgOk = air_set_bool(c->mv, "set_constrainToGround", cg);
        dcOk = air_set_bool(c->mv, "set_detectCollisions", dc);
        if (!dcOk) dcOk = air_field_bool(c->mv, "m_DetectCollisions", dc, NULL) || air_field_bool(c->mv, "_detectCollisions", dc, NULL);
        rbOk = c->rb ? air_set_bool(c->rb, "set_detectCollisions", rb) : 0;
    }
    if (on) {
        g_air_ctrl = c->ctrl;
        { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "air: флаги применены: constrainToGround "); b_s(&x, cgOk ? "ок" : "НЕТ"); b_s(&x, ", detectCollisions движения "); b_s(&x, dcOk ? "ок" : "НЕТ");
          b_s(&x, ", Rigidbody "); b_s(&x, rbOk ? "ок" : "НЕТ"); dlog(lg); }
    } else {
        g_air_ctrl = NULL; g_air_saved = 0;
    }
}

/* ───────────── ввод: куда лететь ───────────── */
static int air_read_f(void* obj, const char* name, float* out, int n) {
    void* f = find_field(A.object_get_class(obj), name); const float* p; int i;
    if (!f || (A.field_get_flags(f) & 0x10)) return 0;
    p = (const float*)((char*)obj + A.field_get_offset(f));
    if (!hm_readable(p, (unsigned)n * 4u)) return 0;
    for (i = 0; i < n; i++) { if (!(p[i] == p[i]) || p[i] > 1e4f || p[i] < -1e4f) return 0; out[i] = p[i]; }
    return 1;
}
static void air_norm2(float* x, float* z) {
    float l = (float)__builtin_sqrt((double)(*x) * (*x) + (double)(*z) * (*z));
    if (l > 1e-4f) { *x /= l; *z /= l; }
}
/* world-вектор движения (длина 0…1). 1 — получилось прочитать ввод (даже нулевой) */
static int air_move_vec(const AirCtx* c, float* out) {
    float in[3]; float R[3], F[3]; void* ct; int fromCam = 1; float mag;
    out[0] = out[1] = out[2] = 0.0f;
    g_air_rawIn[0] = g_air_rawIn[1] = g_air_rawIn[2] = 0.0f;
    if (!c->ctrl) return 0;
    if (g_air_inSrc == 1) {                                          /* Character.MovementDirection — мировой вектор, только горизонталь */
        if (!air_read_f(c->ctrl, "MovementDirection", in, 3)) return 0;
        g_air_rawIn[0] = in[0]; g_air_rawIn[1] = in[1]; g_air_rawIn[2] = in[2];
        mag = (float)__builtin_sqrt((double)in[0] * in[0] + (double)in[2] * in[2]);
        if (mag < 0.05f) return 1;
        if (mag > 1.0f) { in[0] /= mag; in[2] /= mag; }
        out[0] = in[0]; out[2] = in[2];
        return 1;
    }
    if (!air_read_f(c->ctrl, "m_MovementInput", in, 2)) return 0;    /* Vector2: x — вправо, y — вперёд */
    g_air_rawIn[0] = in[0]; g_air_rawIn[1] = in[1];
    mag = (float)__builtin_sqrt((double)in[0] * in[0] + (double)in[1] * in[1]);
    if (mag < 0.05f) return 1;
    if (mag > 1.0f) { in[0] /= mag; in[1] /= mag; }
    ct = air_cam_tr();
    if (!ct || !air_vec(ct, "get_right", R) || !air_vec(ct, "get_forward", F)) {
        fromCam = 0;                                                 /* камеры нет — по самому игроку */
        if (!c->etr || !air_vec(c->etr, "get_right", R) || !air_vec(c->etr, "get_forward", F)) return 0;
    }
    (void)fromCam;
    air_norm2(&R[0], &R[2]); R[1] = 0.0f;                             /* «вправо» — по горизонтали */
    if (g_air_look3d) {                                              /* «вперёд» — куда смотрит камера, вместе с наклоном */
        float l = (float)__builtin_sqrt((double)F[0] * F[0] + (double)F[1] * F[1] + (double)F[2] * F[2]);
        if (l < 1e-3f) return 1;
        F[0] /= l; F[1] /= l; F[2] /= l;
    } else {                                                         /* flat: вперёд = поворот «вправо» на 90° вокруг вертикали */
        F[0] = -R[2]; F[1] = 0.0f; F[2] = R[0];
    }
    out[0] = R[0] * in[0] + F[0] * in[1];
    out[1] = F[1] * in[1];
    out[2] = R[2] * in[0] + F[2] * in[1];
    return 1;
}

/* ───────────── NPC: ближайшие и поиск по ID (как в /vtune) ───────────── */
typedef struct { int id; float d, dy; float p[3]; } AirNpc;
static AirNpc g_air_npc[AIR_NEAR]; static int g_air_nnpc; static float g_air_ref[3];
static void* air_pool(const char* field) {                          /* Pools.<field> */
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}
static int air_ent_pos(void* ent, float* out) {                     /* Entity.Transform.position */
    void* tr = air_obj(ent, "get_Transform");
    if (!tr) tr = air_obj(ent, "get_transform");
    return tr && air_vec(tr, "get_position", out);
}
static void air_npc_cb(void* it, void* ctx) {
    float p[3], d; int i, j, id;
    (void)ctx;
    if (!hm_obj_ok(it) || !air_ent_pos(it, p)) return;
    id = (int)num_field(it, "m_ID", NULL);
    d = air_dist(p, g_air_ref);
    for (i = 0; i < g_air_nnpc && g_air_npc[i].d <= d; i++) {}
    if (i >= AIR_NEAR) return;
    for (j = (g_air_nnpc < AIR_NEAR ? g_air_nnpc : AIR_NEAR - 1); j > i; j--) g_air_npc[j] = g_air_npc[j - 1];
    g_air_npc[i].id = id; g_air_npc[i].d = d; g_air_npc[i].dy = p[1] - g_air_ref[1];
    g_air_npc[i].p[0] = p[0]; g_air_npc[i].p[1] = p[1]; g_air_npc[i].p[2] = p[2];
    if (g_air_nnpc < AIR_NEAR) g_air_nnpc++;
}
/* заполняет g_air_npc (ближайшие к ref). -1 — пул не прочитался, иначе число NPC всего */
static int air_scan_npc(const float* ref) {
    void *pool, *dict, *arr; int total;
    pool = air_pool("Actors");
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (!arr) return -1;
    total = int_field(dict, "_count", "count", NULL, -1);
    g_air_ref[0] = ref[0]; g_air_ref[1] = ref[1]; g_air_ref[2] = ref[2];
    g_air_nnpc = 0;
    walk_struct_array(arr, total, "value", air_npc_cb, NULL);
    return total;
}
static void* air_npc_by_id(int id) {                                /* Pools.Actors.Get(int); NULL — нет такого (или не стримится) */
    void *pool = air_pool("Actors"), *m, *exc = NULL, *args[1], *r; int a = id; char et[200];
    if (!pool) return NULL;
    m = find_method(A.object_get_class(pool), "Get", 1);
    if (!m) { dlog("air: у пула NPC нет Get(int)"); return NULL; }
    args[0] = &a;
    r = A.runtime_invoke(m, pool, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("air: Get бросил исключение (ID нет в пуле?): ", et); return NULL; }
    return (r && hm_obj_ok(r)) ? r : NULL;
}
static void air_cmd_npc_list(void) {
    AirCtx c; float mP[3], eP[3]; int total, i; char m[240]; Buf b;
    if (!air_ctx(&c, 0)) return;
    if (!air_mypos(&c, mP, eP)) { chat_say("/air: ваша позиция не прочиталась — подробности в konstt.log"); return; }
    total = air_scan_npc(eP);
    if (total < 0) { chat_say("/air: пул NPC не прочитался — подробности в konstt.log"); dlog("air: npc: нет Pools.Actors.m_Items._entries"); return; }
    b_init(&b, m, sizeof m); b_s(&b, "NPC у вас в игре: "); b_int(&b, total); b_s(&b, g_air_nnpc ? ", ближайшие:" : " — рядом никого");
    chat_say(m);
    for (i = 0; i < g_air_nnpc; i++) {
        b_init(&b, m, sizeof m); b_s(&b, "  NPC "); b_int(&b, g_air_npc[i].id); b_s(&b, "  "); b_fix(&b, g_air_npc[i].d, 1); b_s(&b, " м, ");
        b_s(&b, g_air_npc[i].dy >= 0 ? "выше на " : "ниже на "); b_fix(&b, g_air_npc[i].dy >= 0 ? g_air_npc[i].dy : -g_air_npc[i].dy, 1); b_s(&b, " м  (");
        air_b_xyz(&b, g_air_npc[i].p); b_s(&b, ")");
        chat_say(m);
    }
}

/* ───────────── включить / выключить ───────────── */
static void air_post(void);
static void* air_worker(void* arg) {
    (void)arg;
    for (;;) {
        if (!__atomic_load_n(&g_air_on, __ATOMIC_ACQUIRE)) { usleep(100000); continue; }
        if (!g_air_pending || air_now_ns() / 1000000 - g_air_pendT > 1000) {     /* прошлый тик не дошёл (игра на паузе/грузится) — шлём новый */
            g_air_pending = 1; g_air_pendT = air_now_ns() / 1000000;
            air_post();
        }
        usleep(AIR_TICK_US);
    }
    return NULL;
}
static int air_enter(float lift, const float* target /* NULL — подняться на lift над собой; иначе лететь в точку (система Entity.Transform) */) {
    AirCtx c; float mP[3], eP[3], hold[3];
    if (!air_ctx(&c, 0)) return 0;
    if (c.veh) { chat_say("/air: в машине не работает — сначала выйдите"); return 0; }
    if (!c.mv) { chat_say("/air: у вашего персонажа не найден компонент движения (подробности в konstt.log)"); dlog("air: нет Actor.m_Controller.Movement"); return 0; }
    if (!air_mypos(&c, mP, eP)) { chat_say("/air: ваша позиция не прочиталась — подробности в konstt.log"); return 0; }
    mt_capture_ctx();                                                /* нужен главный поток — мы как раз в нём (обработчик команды) */
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { chat_say("/air: нет канала в главный поток — полёт не удержать (подробности в konstt.log)"); dlog("air: нет g_mt_ctx"); return 0; }
    if (target) { hold[0] = target[0] + (mP[0] - eP[0]); hold[1] = target[1] + (mP[1] - eP[1]); hold[2] = target[2] + (mP[2] - eP[2]); }
    else { hold[0] = mP[0]; hold[1] = mP[1] + lift; hold[2] = mP[2]; }
    air_flags(&c, 1);
    if (!air_call_setpos(c.mv, hold, "движению")) {
        air_flags(&c, 0);
        chat_say("/air: не получилось поставить персонажа в воздух — подробности в konstt.log");
        return 0;
    }
    air_zero_vel(c.mv);
    g_air_hold[0] = g_air_last[0] = hold[0]; g_air_hold[1] = g_air_last[1] = hold[1]; g_air_hold[2] = g_air_last[2] = hold[2];
    g_air_y0 = mP[1];
    g_air_tPrev = air_now_ns(); g_air_jumps = 0; g_air_watchId = -1; g_air_watchT = 0; g_air_pending = 0;
    if (!g_air_thread) {
        unsigned long t = 0;
        if (pthread_create(&t, NULL, air_worker, NULL) != 0) { air_flags(&c, 0); chat_say("/air: не создался рабочий поток"); dlog("air: pthread_create не вышел"); return 0; }
        g_air_thread = 1;
    }
    __atomic_store_n(&g_air_on, 1, __ATOMIC_RELEASE);
    dlog("air: включён");
    return 1;
}
static void air_exit(const char* why, int land) {
    AirCtx c; float eP[3]; int doLand = land;
    if (!__atomic_load_n(&g_air_on, __ATOMIC_ACQUIRE) && !g_air_saved) return;
    if (g_air_hold[1] - g_air_y0 > AIR_AUTOLAND) doLand = 1;
    __atomic_store_n(&g_air_on, 0, __ATOMIC_RELEASE);
    if (air_ctx(&c, 1)) {
        air_flags(&c, 0);
        if (c.mv) air_zero_vel(c.mv);
        if (doLand && !c.veh && c.me && air_mypos(&c, NULL, eP)) {
            void *m, *exc = NULL, *args[2]; u8 findH = 1; float v[3]; char et[240];
            v[0] = eP[0]; v[1] = eP[1]; v[2] = eP[2];
            m = find_method(A.object_get_class(c.me), "SetPosition", 2);   /* Player.SetPosition(Vector3, findHeight) — тот же вызов, что «/tpl to X Z» */
            if (m) {
                args[0] = v; args[1] = &findH;
                A.runtime_invoke(m, c.me, args, &exc);
                if (exc) { exc_text(exc, et, sizeof et); dlog2("air: посадка: SetPosition бросил исключение: ", et); }
            }
        }
    } else { g_air_saved = 0; g_air_ctrl = NULL; }
    dlog2("air: выключен: ", why);
}

/* ───────────── тик (главный поток) ───────────── */
static void air_tick(void) {
    AirCtx c; float cur[3], mv3[3], dt; long long now;
    g_air_pending = 0;
    if (!__atomic_load_n(&g_air_on, __ATOMIC_ACQUIRE)) return;
    if (!air_ctx(&c, 1) || !c.mv) { air_exit("персонаж недоступен", 0); chat_say("/air: персонаж пропал (смерть/смена мира) — полёт выключен"); return; }
    if (c.veh) { air_exit("вы сели в машину", 0); chat_say("/air: вы в машине — полёт выключен"); return; }
    if (c.ctrl != g_air_ctrl) air_flags(&c, 1);                      /* персонаж пересоздан: флаги — на новые объекты */
    if (!air_mypos(&c, cur, NULL)) return;
    now = air_now_ns();
    dt = (float)((double)(now - g_air_tPrev) / 1e9);
    g_air_tPrev = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;                                        /* после паузы не прыгаем */
    if (air_dist(cur, g_air_last) > AIR_JUMP) {                      /* игра/сервер переставили нас — держим новую точку */
        g_air_hold[0] = cur[0]; g_air_hold[1] = cur[1]; g_air_hold[2] = cur[2];
        g_air_jumps++;
        dlog("air: вас переставили далеко — держу новую точку");
    }
    if (air_move_vec(&c, mv3)) {
        g_air_hold[0] += mv3[0] * g_air_speed * dt;
        g_air_hold[1] += mv3[1] * g_air_speed * dt;
        g_air_hold[2] += mv3[2] * g_air_speed * dt;
    }
    if (air_call_setpos(c.mv, g_air_hold, "движению")) air_zero_vel(c.mv);
    g_air_last[0] = g_air_hold[0]; g_air_last[1] = g_air_hold[1]; g_air_last[2] = g_air_hold[2];
    if (g_air_watch && now / 1000000 - g_air_watchT >= 700) {        /* кто ближайший NPC — пишем, когда меняется */
        float eP[3]; g_air_watchT = now / 1000000;
        if (air_mypos(&c, NULL, eP) && air_scan_npc(eP) > 0 && g_air_nnpc > 0 && g_air_npc[0].id != g_air_watchId && g_air_npc[0].d < 4.0f) {
            char m[160]; Buf b;
            g_air_watchId = g_air_npc[0].id;
            b_init(&b, m, sizeof m); b_s(&b, "ближайший NPC "); b_int(&b, g_air_watchId); b_s(&b, " — "); b_fix(&b, g_air_npc[0].d, 1); b_s(&b, " м");
            chat_say(m);
        }
    }
}
static void native_air_cmd(void* self, void* args, void* mi);
static void air_post(void) {                                         /* рабочий поток: колбэк в главный поток со строкой-меткой вместо string[] */
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { g_air_pending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_air_cmd);
    st = A.string_new("air-tick");
    if (!cb || !st) { g_air_pending = 0; dlog("air: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("air: Post бросил исключение: ", et); g_air_pending = 0; }
}

/* ───────────── команда ───────────── */
static int air_tokens(void* arr, char tok[][32], int maxn) {
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
            if (!q || w[0] == '/' || seq(w, "air")) continue;        /* сама команда может входить в массив (как у /tpl) */
            if (n < maxn) { int z; for (z = 0; z <= q; z++) tok[n][z] = w[z]; n++; }
        }
    }
    return n;
}
static void air_usage(void) {
    chat_usage("/air — полёт: вкл/выкл (2.5 м вверх);  /air up|down [м];  /air speed N;  /air flat|3d;  /air land — выключить и на землю");
    chat_usage("/air npc — ближайшие NPC с ID;  /air to ID — к NPC (3 м над ним);  /air watch — писать ID ближайшего NPC;  /air status;  /air input");
}
static void air_status(void) {
    char m[300]; Buf b; AirCtx c; float p[3];
    b_init(&b, m, sizeof m);
    b_s(&b, "/air: "); b_s(&b, g_air_on ? "ВКЛ" : "выкл");
    b_s(&b, ", скорость "); b_fix(&b, g_air_speed, 1); b_s(&b, " м/с, режим "); b_s(&b, g_air_look3d ? "3d (куда смотрит камера)" : "flat (горизонтально)");
    b_s(&b, ", слежка за NPC: "); b_s(&b, g_air_watch ? "да" : "нет");
    chat_say(m);
    if (g_air_on && air_ctx(&c, 1) && air_mypos(&c, p, NULL)) {
        b_init(&b, m, sizeof m); b_s(&b, "  держу "); air_b_xyz(&b, g_air_hold); b_s(&b, ", сейчас "); air_b_xyz(&b, p);
        b_s(&b, ", высота над точкой включения "); b_fix(&b, g_air_hold[1] - g_air_y0, 1); b_s(&b, " м, перестановок игрой: "); b_int(&b, g_air_jumps);
        chat_say(m);
    }
    b_init(&b, m, sizeof m); b_s(&b, "  ввод ("); b_s(&b, g_air_inSrc ? "MovementDirection" : "m_MovementInput"); b_s(&b, "): ");
    b_fix(&b, g_air_rawIn[0], 2); b_ch(&b, ' '); b_fix(&b, g_air_rawIn[1], 2); b_ch(&b, ' '); b_fix(&b, g_air_rawIn[2], 2);
    b_s(&b, " — подвигайте джойстик: числа должны меняться (/air input — сменить источник)");
    chat_say(m);
}
static void native_air_cmd(void* self, void* args, void* mi) {
    char tok[6][32]; int nt; char m[240]; Buf b; float v;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { air_tick(); return; }   /* тик worker-а, а не команда */
    nt = air_tokens(args, tok, 6);
    dlog_i("air: команда вызвана, слов ", nt);
    if (nt == 0) {                                                   /* переключатель */
        if (g_air_on) { air_exit("команда /air", 0); chat_say("/air: выключен"); }
        else if (air_enter(AIR_LIFT, NULL)) chat_say("/air: полёт включён. Джойстик — лететь (вперёд — туда, куда смотрит камера); /air off — выключить, /air help — все команды");
        return;
    }
    if (seq(tok[0], "help") || seq(tok[0], "помощь") || seq(tok[0], "?")) { air_usage(); return; }
    if (seq(tok[0], "status") || seq(tok[0], "статус") || seq(tok[0], "diag")) { air_status(); return; }
    if (seq(tok[0], "off") || seq(tok[0], "выкл") || seq(tok[0], "0")) {
        if (!g_air_on) { chat_say("/air: и так выключен"); return; }
        air_exit("команда /air off", 0); chat_say("/air: выключен"); return;
    }
    if (seq(tok[0], "land") || seq(tok[0], "посадка")) {
        if (!g_air_on) { chat_say("/air: и так выключен"); return; }
        air_exit("команда /air land", 1); chat_say("/air: выключен, поставил вас на землю"); return;
    }
    if (seq(tok[0], "on") || seq(tok[0], "вкл") || seq(tok[0], "1") || air_num(tok[0], &v)) {
        float lift = AIR_LIFT;
        if (seq(tok[0], "on") || seq(tok[0], "вкл") || seq(tok[0], "1")) { if (nt >= 2 && air_num(tok[1], &v)) lift = v; }
        else lift = v;
        if (lift < 0.5f) lift = 0.5f;
        if (lift > AIR_MAXLIFT) lift = AIR_MAXLIFT;
        if (g_air_on) {                                              /* уже летим: «подняться ещё на N» */
            g_air_hold[1] += lift; chat_say("/air: уже включён — поднял точку удержания"); return;
        }
        if (air_enter(lift, NULL)) { b_init(&b, m, sizeof m); b_s(&b, "/air: полёт включён, поднялся на "); b_fix(&b, lift, 1); b_s(&b, " м. Выключить: /air off"); chat_say(m); }
        return;
    }
    if (seq(tok[0], "up") || seq(tok[0], "вверх") || seq(tok[0], "down") || seq(tok[0], "вниз")) {
        int up = seq(tok[0], "up") || seq(tok[0], "вверх"); float d = 1.0f;
        if (nt >= 2 && air_num(tok[1], &v)) d = v;
        if (d > AIR_MAXLIFT) d = AIR_MAXLIFT;
        if (!g_air_on) { chat_say("/air: сначала включите полёт: /air"); return; }
        g_air_hold[1] += up ? d : -d;
        b_init(&b, m, sizeof m); b_s(&b, "/air: "); b_s(&b, up ? "вверх на " : "вниз на "); b_fix(&b, d, 1); b_s(&b, " м (высота над точкой включения "); b_fix(&b, g_air_hold[1] - g_air_y0, 1); b_s(&b, " м)");
        chat_say(m);
        return;
    }
    if (seq(tok[0], "speed") || seq(tok[0], "скорость")) {
        if (nt < 2 || !air_num(tok[1], &v)) { b_init(&b, m, sizeof m); b_s(&b, "/air: скорость сейчас "); b_fix(&b, g_air_speed, 1); b_s(&b, " м/с. Задать: /air speed 12 (до 40)"); chat_say(m); return; }
        if (v < 0.5f) v = 0.5f;
        if (v > AIR_MAXSPEED) v = AIR_MAXSPEED;
        g_air_speed = v;
        b_init(&b, m, sizeof m); b_s(&b, "/air: скорость "); b_fix(&b, v, 1); b_s(&b, " м/с"); chat_say(m);
        return;
    }
    if (seq(tok[0], "flat") || seq(tok[0], "ровно")) { g_air_look3d = 0; chat_say("/air: режим flat — по джойстику летите горизонтально, высота — /air up|down"); return; }
    if (seq(tok[0], "3d") || seq(tok[0], "look") || seq(tok[0], "камера")) { g_air_look3d = 1; chat_say("/air: режим 3d — вперёд летите туда, куда смотрит камера"); return; }
    if (seq(tok[0], "watch") || seq(tok[0], "слежка")) {
        g_air_watch = !g_air_watch; g_air_watchId = -1;
        chat_say(g_air_watch ? "/air: слежка включена — пока летите, пишу ID ближайшего NPC (ближе 4 м), когда он меняется" : "/air: слежка выключена");
        return;
    }
    if (seq(tok[0], "input") || seq(tok[0], "ввод")) {
        g_air_inSrc = !g_air_inSrc;
        chat_say(g_air_inSrc ? "/air: источник ввода — Character.MovementDirection (мировой вектор). Если вас сносит само — снова /air input" : "/air: источник ввода — Character.m_MovementInput (джойстик + камера)");
        return;
    }
    if (seq(tok[0], "npc") || seq(tok[0], "нпс") || seq(tok[0], "нпц") || seq(tok[0], "to") || seq(tok[0], "к")) {
        int isTo = seq(tok[0], "to") || seq(tok[0], "к"); float fid; int idx = nt >= 2 ? 1 : -1;
        if (idx >= 0 && !air_num(tok[idx], &fid) && nt >= 3) { idx = 2; if (!air_num(tok[2], &fid)) idx = -1; }   /* «/air to npc 123» */
        else if (idx >= 0 && !air_num(tok[idx], &fid)) idx = -1;
        if (idx < 0) {
            if (isTo) { chat_usage("Формат: /air to ID — лететь к NPC с этим ID (список ближайших: /air npc)"); return; }
            air_cmd_npc_list(); return;
        }
        {
            int id = (int)(fid + 0.5f); void* ent = air_npc_by_id(id); float tp[3];
            if (!ent) {
                b_init(&b, m, sizeof m); b_s(&b, "/air: NPC "); b_int(&b, id); b_s(&b, " не найден среди созданных у вас в игре — сервер «стримит» только тех, кто рядом (подойдите ближе или проверьте ID)");
                chat_say(m); return;
            }
            if (!air_ent_pos(ent, tp)) { chat_say("/air: нашёл NPC, но позиция не прочиталась — подробности в konstt.log"); return; }
            tp[1] += 3.0f;
            if (g_air_on) {
                AirCtx c; float mP[3], eP[3];
                if (air_ctx(&c, 0) && air_mypos(&c, mP, eP)) {
                    g_air_hold[0] = tp[0] + (mP[0] - eP[0]); g_air_hold[1] = tp[1] + (mP[1] - eP[1]); g_air_hold[2] = tp[2] + (mP[2] - eP[2]);
                    air_call_setpos(c.mv, g_air_hold, "движению");
                    g_air_last[0] = g_air_hold[0]; g_air_last[1] = g_air_hold[1]; g_air_last[2] = g_air_hold[2];
                } else return;
            } else if (!air_enter(0.0f, tp)) return;
            b_init(&b, m, sizeof m); b_s(&b, "/air: над NPC "); b_int(&b, id); b_s(&b, " (3 м выше него): "); air_b_xyz(&b, tp);
            chat_say(m);
        }
        return;
    }
    air_usage();
}

/* ───────────── самозапуск ───────────── */
static void* air_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("air", native_air_cmd)) return NULL;           /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /air */
    hm_mod_ready(g_air_modId);
    dlog("air: мод загружен, /air готов");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void air_ctor(void) {
    unsigned long t = 0;
    g_air_modId = hm_mod_register("air");
    pthread_create(&t, NULL, air_main, NULL);
}
#endif
