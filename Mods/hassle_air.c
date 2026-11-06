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
 *   Кнопки (как в airbrake из SAMP, только на экране): джойстик — лететь; кнопка ПРЫЖОК (держите) — вверх; ПРИСЕД (двойное касание джойстика и
 *                         удержание, так игра включает присед) — вниз. Камерой вверх/вниз тоже можно (режим 3d). /air keys — выключить/включить эти кнопки.
 *   /air up [м]  /air down [м]   сместиться вверх/вниз сразу на N метров (по умолчанию 1 м)
 *   /air speed N          скорость полёта, м/с (по умолчанию 6, до 1000)
 *   /air flat | 3d        flat — по джойстику летите только горизонтально (высота — командами up/down);
 *                         3d (по умолчанию) — вперёд летите туда, куда смотрит камера, в т.ч. вверх/вниз: посмотрели вниз на кучу — и опускаетесь к ней
 *   /air stand            вкл/выкл (по умолчанию ВКЛ): в полёте персонаж держит обычную анимацию (стоит/идёт по джойстику), а не падает с раскинутыми руками
 *   /air npc              ближайшие NPC: ID, расстояние, на сколько выше/ниже вас (ID — те же, что в /vtune find npc и в серверном коде)
 *   /air npc ID  |  /air to ID    перелететь на 3 м над NPC с этим ID (включит полёт сам)
 *   /air watch            вкл/выкл: пока летите, мод пишет в чат ID ближайшего NPC, когда он меняется (подлетели к другому — увидите, кто это)
 *   /air input            переключить источник джойстика (если «не летит» — смотрите /air status и пробуйте этот вариант)
 *   /air mark [gps|cp|off]  автолёт к красной метке: к маркеру сервера (чекпоинт) или, если его нет, к метке GPS с карты (/gps). Метка сменилась или появилась
 *                         новая — сам летит к ней, на месте ждёт следующую. gps — только метка карты, cp — только маркер сервера, off — выключить. Если полёт был
 *                         выключен — включится сам. Пока вы жмёте джойстик/кнопки — автолёт на паузе (перехват), отпустили — продолжит. Скорость — /air speed
 *   /air exam chat [on|off]  перехват чата: стоять по словам инструктора «Здесь аккуратней» (мгновенно), ехать после «Отлично…» (см. блок про автошколу ниже)
 *   /air exam [on|off|auto]  автошкола: на экзамене по вождению /air mark cp ведёт машину как по правилам — курс по дороге, остановка у знаков STOP (подробно — в блоке
 *                         «автошкола: экзамен по вождению» ниже). auto (по умолчанию) — включается само на старте экзамена; /air exam speed N — скорость экзамена, м/с
 *   /air status           состояние и сырые значения ввода;  /air help — подсказка
 *
 * Метки (/air mark): маркер сервера — статические Hassle.Entities.Checkpoint.Race / .Simple (их ставят SetPlayerRaceCheckpoint / SetPlayerCheckpoint; активный — тот, чей
 *   GameObject включён и позиция не нулевая), метка карты — MiniMap.m_NavigationPoint (+ m_IsNavigationActive), как в /tpl gps. Цель перечитывается раз в 150 мс; её позиция —
 *   в системе Entity.Transform (как у NPC), поэтому пешему добавляем сдвиг «transform движения − Entity.Transform». Маркер сервера — точная точка (летим ровно в неё), у метки
 *   карты высота известна не всегда: нет — летим на своей высоте и останавливаемся над точкой (дальше /air down или /air land).
 *
 * Как это работает (поля — из дампа IL2CPP, читаются по ИМЕНАМ, не по смещениям)
 *   • Позиция держится нашим кодом: каждый кадр главному потоку приходит «тик» (тот же приём, что у /tpl и /route: рабочий поток кладёт
 *     колбэк в UnitySynchronizationContext), в нём мы считаем новую точку = прежняя + направление джойстика × скорость × dt и ставим
 *     персонажа туда ICharacterMovement.SetPosition(Vector3, updateGround=false) — это прямая постановка капсулы, как «/tpl alt»,
 *     и обнуляем ICharacterMovement.velocity. Так игра не может ни уронить вас (гравитацию мы просто не используем), ни толкнуть.
 *   • Кнопки: в JS-интерфейсе (Hud.js) джойстик — это экранный стик Unity Input System «<Gamepad>/leftStick» со значением (dx/max, -dy/max): вверх по экрану = +,
 *     прыжок — «<Keyboard>/leftShift», присед — «<Keyboard>/c» (двойной тап по джойстику, второй тап НЕ отпускать). Их состояние читаем у самих экранных
 *     контролов: статический словарь OnScreenControls.m_Controls (путь → OnWebViewScreenControl), у каждого поле m_Control — ButtonControl, берём isPressed.
 *     Раньше читался Character.Keys (Dictionary<PadKeys,bool>, KEY_JUMP = 5, KEY_CROUCH = 1); по дампу похоже, что он зависит от способностей персонажа,
 *     а Jump/Crouch в воздухе не стартуют (нет земли) — вероятная причина, почему в полёте кнопки «молчали» (код игры в дампе не виден, проверяйте /air status). Character.Keys остался запасным вариантом, если контролов не нашлось.
 *     Короткий тап по кнопке сразу сдвигает на AIR_TAP_STEP (0.5 м), зажатие — летит со скоростью /air speed.
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
 *   • Если игра переставила вас далеко (сервер вернул/телепортировал) — держим новую точку.
 *
 * В МАШИНЕ (за рулём) /air летит вместе с машиной — для сломавшихся/застрявших машин и NPC в них. Пассажиру отказ (машиной управляет водитель).
 *   • Машину держим так же, как пешего: каждый кадр ставим Transform.position машины (и Rigidbody.position) в нашу точку. Чтобы физика не мешала,
 *     у Rigidbody машины на время полёта: isKinematic = true, detectCollisions = false, useGravity = false, interpolation = None (иначе картинка дёргается).
 *     Скорости обнуляются ДО kinematic. При выключении всё возвращается как было, скорости снова обнуляются. Entity.SetPosition (асинхронный, с заморозкой
 *     и подгрузкой карты — см. /tpl) каждый кадр не годится, его зовём только для посадки машины на землю (findHeight=true).
 *   • HUD машины (Hud.js, блок isSpeedometerShown): кнопки W (PedalGas), S (PedalStop), ПРОБЕЛ (HandBrake, всегда) и либо A/D (TurnLeft/TurnRight), либо — при useJoystick —
 *     стик «<Gamepad>/leftStick» (его двойной тап включает «<Keyboard>/c»). Кнопки прыжка (leftShift) в машине нет: она показывается только пешком.
 *   • Куда лететь: сперва читаем те самые экранные кнопки W/S/A/D (OnScreenControls.m_Controls, isPressed); если ни одна не нажата — физику машины
 *     WheeledVehiclePhysics.SteerInput / ThrottleInput / BrakeInput (их пишет VehicleInput.Update; именно так читается руль-стик). Вперёд = газ − тормоз, вправо = руль;
 *     «вперёд» — туда, куда смотрит камера (3d) или горизонтально (flat), «вправо» — вправо от камеры. Машина не поворачивается: ориентация остаётся прежней.
 *   • Вверх/вниз в машине: ЗАЖМИТЕ РУЧНИК (пробел) — пока он зажат, газ двигает вверх, тормоз вниз (по высоте; в кнопочном и в стиковом HUD). Ещё: присед (двойной тап по стику) — вниз,
 *     взгляд камерой вверх/вниз в 3d, /air up|down N. У вертолёта/самолёта/лодки нет WheeledVehiclePhysics, но кнопки W/S в их HUD те же, так что работает и там, если кнопки найдутся.
 *   • Сели в машину, пока летели пешком, — полёт перейдёт на машину; вышли из машины в полёте — машина сама встанет на землю, а вы продолжите лететь пешком.
 *   • Выключение: если машина выше 3 м над точкой включения (или ниже неё), мод ставит её на землю (Entity.SetPosition(pos, findHeight=true)); /air land делает это всегда.
 *   • Серверный античит может отреагировать на скорость/высоту машины; /tpl ghost перед /air не пускает позицию на сервер. Если сервер вернёт машину RPC SetVehiclePos —
 *     мод увидит прыжок и возьмёт новую точку (чтобы сервер не возвращал вообще — /tpl keep on).
 *
 * «Как на ровном» (/air stand) — по дампу IL2CPP и дизассемблированию libil2cpp.so (RVA из dump.cs)
 *   • Левитация в кадре — это способность Fall (клип m_FallDownClip, руки в стороны). Fall.CanStartAbility = «не isOnGround И FindGround(...) ничего не нашёл».
 *     Пока Fall жива, обычная анимация Moving («стоит/идёт», LinearMixer от джойстика) остановлена; сама Moving земли не проверяет.
 *   • Поднять Fall.m_MinFallHeight НЕ помогает: CharacterMovement.FindGround при выключенных коллизиях (_detectCollisions, а /air их выключает) сразу возвращает «земли нет»
 *     (проверено по журналу: Fall стартовал каждый кадр). Поэтому Fall отключается целиком: Character.TryStartAbility выходит, если у способности m_Enabled == false (смещение 0x18).
 *     На время полёта у Fall ставим Enabled = false (Ability.set_Enabled, запасной путь — поле m_Enabled), а если она уже активна — Ability.StopAbility(force). При выключении Enabled возвращается.
 *   • Присед (Crouch) и прыжок (Jump) требуют isOnWalkableGround, в воздухе они не стартуют — анимации приседа в полёте нет; вниз вы летите по состоянию кнопки.
 *
 * Спуск по приседу: двойной тап по джойстику включает «<Keyboard>/c» и держит его, пока палец не отпущен (Hud.js: touchStart / touchEnd). Пока кнопка зажата, высота
 * уменьшается на speed·dt каждый кадр, нижней границы нет (земля не держит: constrainToGround и коллизии выключены), так что можно уходить и под поверхность — к NPC, застрявшим в текстурах.
 *
 * ВАЖНО: код не запускался на устройстве — собран только компилятором. Если что-то не сработало, /air status и HLOG/konstt.log (строки «air:»).
 */
#pragma once
#include "hassle_core.c"
extern int clock_gettime(int, void*);       /* как в mem/route/tpl: struct timespec = два long */

#define AIR_LIFT 2.5f
#define AIR_MAXLIFT 60.0f
#define AIR_SPEED 6.0f
#define AIR_MAXSPEED 1000.0f
#define AIR_TICK_US 8000                          /* период рабочего потока; реально тик идёт раз в кадр (пока прошлый не отработал, новый не шлём) */
#define AIR_NEAR 8
#define AIR_JUMP 6.0f                             /* если игра сама сдвинула вас дальше, чем на столько метров за кадр — считаем это перестановкой и берём новую точку */
#define AIR_VEH_AUTOLAND 3.0f                     /* то же для машины: выше этого над точкой включения (или ниже неё) при выключении ставим машину на землю */
#define AIR_AUTOLAND 5.0f                         /* выше этого над точкой включения при выключении ставим на землю */
#define AIR_TAP_STEP 0.5f                         /* короткий тап по ПРЫЖКУ/ПРИСЕДУ сразу сдвигает на столько метров (иначе тап длиной в 2-3 кадра почти не виден) */

static volatile int g_air_on;
static volatile int g_air_pending;
static volatile long long g_air_pendT;
static int g_air_thread;
static int g_air_modId = -1;
static float g_air_speed = AIR_SPEED;
static int g_air_look3d = 1;
static int g_air_inSrc;                           /* 0 — Character.m_MovementInput, 1 — Character.MovementDirection */
static int g_air_watch;
static int g_air_keys = 1;                         /* 1 — прыжок/присед двигают вверх/вниз */
static int g_air_kj, g_air_kc;                     /* последнее состояние кнопок (для /air status) */
static int g_air_kSrc = -1;                        /* откуда читаем кнопки: 2 — экранные контролы JS-интерфейса, 1 — Character.Keys, 0 — не читается, -1 — ещё не пробовали */
static int g_air_pj, g_air_pc;                     /* прошлое состояние кнопок — чтобы поймать момент нажатия */
static float g_air_hold[3], g_air_last[3], g_air_y0;
static long long g_air_tPrev;
static void* g_air_ctrl;                          /* контроллер, к которому уже применены флаги (сменился — персонаж пересоздан, применяем заново) */
static int g_air_oCg = -1, g_air_oDc = -1, g_air_oRb = -1, g_air_saved;     /* исходные значения флагов (-1 — не прочиталось, считаем «включено») */
static int g_air_jumps;
static int g_air_watchId = -1; static long long g_air_watchT;
static int g_air_stand = 1;                       /* 1 — не пускаем Fall (левитацию), держим обычную анимацию */
static void* g_air_fall;                          /* способность Fall текущего персонажа */
static int g_air_fallSaved;
static float g_air_rawIn[3];                      /* последние сырые значения ввода — для /air status */
static int g_air_mode;                            /* 0 — летим пешком (персонаж), 1 — летим вместе с машиной, в которой вы за рулём */
static void* g_air_veh;                           /* машина, которую держим в воздухе (режим 1) */
static int g_air_vsaved, g_air_vK = -1, g_air_vDc = -1, g_air_vGr = -1, g_air_vIp = -1;   /* исходные флаги Rigidbody машины (-1 — не прочиталось) */
static int g_air_vrbSame;                         /* 1 — Rigidbody на том же Transform, что и Entity машины: Rigidbody.position тоже ставим */
static float g_air_vin[3];                        /* последний ввод в машине: руль, газ, тормоз — для /air status */
static int g_air_vinOk = -1;                      /* 1 — WheeledVehiclePhysics.*Input прочитались, 0 — нет, -1 — не пробовали */
#define AIR_MARK_STOP 1.2f                        /* ближе этого к метке (м) считаем, что прилетели */
#define AIR_MARK_POLL_MS 150
static int g_air_mark;                            /* 0 — выкл; 1 — следим за любой меткой; 2 — только метка карты (gps); 3 — только маркер сервера (cp) */
static int g_air_mkHave, g_air_mkArrived, g_air_mkKind, g_air_mkExact;   /* есть ли цель; прилетели ли; откуда: 1 — маркер сервера, 2 — метка карты; высота точная ли */
static float g_air_mkPos[3];                      /* последняя прочитанная цель (система Entity.Transform) */
static long long g_air_mkT;
static int g_air_vfix;                            /* сколько раз игра возвращала машине обычную физику, а мы снова выключали */
static int g_air_face = 1;                        /* /air face: в машине нос поворачивается по направлению полёта (иначе машина летит боком, с прежним курсом) */

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

/* int-геттер/сеттер (enum тоже): dflt — если метода нет или он бросил */
static int air_get_i32(void* obj, const char* getter, int dflt) {
    void *m, *exc = NULL, *r;
    if (!obj) return dflt;
    m = find_method(A.object_get_class(obj), getter, 0);
    if (!m) return dflt;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r) return dflt;
    return *(int*)((char*)r + HDR);
}
static int air_set_i32(void* obj, const char* setter, int v) {
    void *m, *exc = NULL, *args[1]; int x = v;
    if (!obj) return 0;
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = &x;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
/* сеттер с одним Vector3 (position, velocity…) */
static int air_set_v3(void* obj, const char* setter, const float* p) {
    void *m, *exc = NULL, *args[1]; float v[3];
    if (!obj) return 0;
    v[0] = p[0]; v[1] = p[1]; v[2] = p[2];
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = v;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
/* SetPosition(Vector3, bool findHeight) у Player или у машины (Entity.SetPosition) */
static int air_call_set_h(void* obj, const float* pos, u8 findH, const char* what) {
    void *m, *exc = NULL, *args[2]; float v[3]; char et[240]; u8 f = findH;
    v[0] = pos[0]; v[1] = pos[1]; v[2] = pos[2];
    m = find_method(A.object_get_class(obj), "SetPosition", 2);
    if (!m) { dlog2("air: SetPosition не найден у ", what); return 0; }
    args[0] = v; args[1] = &f;
    A.runtime_invoke(m, obj, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("air: SetPosition бросил исключение: ", et); return 0; }
    return 1;
}

/* способность персонажа по имени класса (Character.m_Abilities — List<Ability>) */
static void* air_find_ability(void* ctrl, const char* cls) {
    void *list, *items; int n, i; long len; char* data;
    list = ctrl ? ref_field_s(ctrl, "m_Abilities") : NULL;
    if (!list || !hm_obj_ok(list)) return NULL;
    items = ref_field_s(list, "_items");
    if (!items || !hm_obj_ok(items)) return NULL;
    n = int_field(list, "_size", "size", NULL, -1);
    len = *(long*)((char*)items + 3 * sizeof(void*));
    if (len <= 0 || len > 64) return NULL;
    if (n < 0 || n > len) n = (int)len;
    data = (char*)items + 4 * sizeof(void*);
    for (i = 0; i < n; i++) {
        void* a = *(void**)(data + (uptr)i * sizeof(void*));
        if (!a || !hm_obj_ok(a)) continue;
        if (seq(A.class_get_name(A.object_get_class(a)), cls)) return a;
    }
    return NULL;
}
static void air_stop_ability(void* ab) {                           /* Ability.StopAbility(bool force) */
    void *m, *exc = NULL, *args[1]; u8 force = 1;
    m = find_method(A.object_get_class(ab), "StopAbility", 1);
    if (!m) return;
    args[0] = &force;
    A.runtime_invoke(m, ab, args, &exc);
}

/* ───────────── контекст: игрок и его компоненты (главный поток) ───────────── */
typedef struct { void *me, *ctrl, *mv, *rb, *etr, *mtr, *veh; int drv; } AirCtx;   /* drv: 0 — вы пассажир (машиной правит другой) */
static int air_ctx(AirCtx* c, int quiet) {
    c->me = c->ctrl = c->mv = c->rb = c->etr = c->mtr = c->veh = NULL; c->drv = 1;
    if (!g_safe || !il2cpp_light()) { if (!quiet) chat_say("/air: игра ещё грузится, подождите"); return 0; }
    ensure_attached();
    c->me = local_player();
    if (!c->me || !hm_obj_ok(c->me)) { if (!quiet) chat_say("/air: вы не в игре (персонаж не создан)"); return 0; }
    c->veh = ref_field_s(c->me, "Vehicle");                           /* Player.Vehicle */
    if (c->veh && !hm_obj_ok(c->veh)) c->veh = NULL;
    if (c->veh) { void* d = ref_field_s(c->veh, "m_Driver"); if (d && hm_obj_ok(d) && d != c->me) c->drv = 0; }   /* BaseVehicle.m_Driver; не прочиталось — считаем водителем */
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


/* ───────────── машина: Rigidbody, позиция, флаги ───────────── */
static void* air_veh_rb(void* veh) {                               /* BaseVehicle.m_Rigidbody (как в /tpl) */
    void* rb = ref_field_s(veh, "m_Rigidbody");
    if (!rb || !hm_obj_ok(rb)) rb = ref_field_s(veh, "vehicleRigidbody");
    if (!rb || !hm_obj_ok(rb)) rb = air_obj(veh, "get_Rigidbody");
    return (rb && hm_obj_ok(rb)) ? rb : NULL;
}
static void* air_veh_tr(void* veh) {
    void* tr = air_obj(veh, "get_Transform");
    if (!tr) tr = air_obj(veh, "get_transform");
    return tr;
}
static int air_vpos(void* veh, float* p) { void* tr = veh ? air_veh_tr(veh) : NULL; return tr && air_vec(tr, "get_position", p); }
static void air_rb_zero(void* rb) {                                /* только у не-kinematic тела: у kinematic Unity ругается в лог */
    static const float z[3] = { 0.0f, 0.0f, 0.0f };
    air_set_v3(rb, "set_velocity", z);
    air_set_v3(rb, "set_angularVelocity", z);
}
/* прямая постановка машины: Transform.position (+ Rigidbody.position). Работает мгновенно, в отличие от Entity.SetPosition */
static int air_vset(void* veh, const float* pos) {
    void *tr = air_veh_tr(veh), *rb;
    if (!tr || !air_set_v3(tr, "set_position", pos)) return 0;
    if (g_air_vrbSame) { rb = air_veh_rb(veh); if (rb) air_set_v3(rb, "set_position", pos); }
    return 1;
}
/* on=1: kinematic, без коллизий и гравитации, без интерполяции; on=0: вернуть исходные значения */
static void air_vflags(void* veh, int on) {
    void* rb = veh ? air_veh_rb(veh) : NULL; int okK, okD, okG;
    if (!rb) { dlog("air: у машины не найден Rigidbody — флаги не менял"); return; }
    if (on) {
        if (!g_air_vsaved) {                                       /* исходные значения — один раз за включение (повторные вызовы их не затирают) */
            char lg[200]; Buf x;
            g_air_vK = air_get_bool(rb, "get_isKinematic", -1);
            g_air_vDc = air_get_bool(rb, "get_detectCollisions", -1);
            g_air_vGr = air_get_bool(rb, "get_useGravity", -1);
            g_air_vIp = air_get_i32(rb, "get_interpolation", -1);
            g_air_vsaved = 1;
            b_init(&x, lg, sizeof lg); b_s(&x, "air: машина, исходные флаги: isKinematic "); b_int(&x, g_air_vK); b_s(&x, ", detectCollisions "); b_int(&x, g_air_vDc);
            b_s(&x, ", useGravity "); b_int(&x, g_air_vGr); b_s(&x, ", interpolation "); b_int(&x, g_air_vIp); b_s(&x, ", Rigidbody на том же Transform: "); b_s(&x, g_air_vrbSame ? "да" : "нет");
            dlog(lg);
        }
        air_rb_zero(rb);                                           /* пока тело ещё не kinematic */
        if (g_air_vIp >= 0) air_set_i32(rb, "set_interpolation", 0);
        okG = air_set_bool(rb, "set_useGravity", 0);
        okD = air_set_bool(rb, "set_detectCollisions", 0);
        okK = air_set_bool(rb, "set_isKinematic", 1);
        { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "air: машина, флаги применены: isKinematic "); b_s(&x, okK ? "ок" : "НЕТ");
          b_s(&x, ", detectCollisions "); b_s(&x, okD ? "ок" : "НЕТ"); b_s(&x, ", useGravity "); b_s(&x, okG ? "ок" : "НЕТ"); dlog(lg); }
    } else {
        int k = g_air_vK < 0 ? 0 : g_air_vK, dc = g_air_vDc < 0 ? 1 : g_air_vDc, gr = g_air_vGr < 0 ? 1 : g_air_vGr;
        air_set_bool(rb, "set_isKinematic", k);
        air_set_bool(rb, "set_detectCollisions", dc);
        air_set_bool(rb, "set_useGravity", gr);
        if (g_air_vIp >= 0) air_set_i32(rb, "set_interpolation", g_air_vIp);
        if (!k) air_rb_zero(rb);                                   /* без этого машина продолжила бы с прежней скоростью */
        g_air_vsaved = 0;
        dlog("air: машина, флаги возвращены");
    }
}
/* отпустить машину: вернуть физику и (land) поставить на землю тем же вызовом, что /tpl to — Entity.SetPosition(pos, findHeight=true) */
static void air_vrelease(int land) {
    void* veh = g_air_veh; float p[3];
    g_air_veh = NULL;
    if (!veh || !hm_obj_ok(veh)) { g_air_vsaved = 0; dlog("air: машина пропала — флаги вернуть не на чем"); return; }
    air_vflags(veh, 0);
    if (land && air_vpos(veh, p)) { if (air_call_set_h(veh, p, 1, "машине (посадка)")) dlog("air: машина поставлена на землю"); }
}

/* ───────────── «как на ровном»: отключаем способность Fall ───────────── */
static int g_air_oFallEn = 1;
static void air_stand(const AirCtx* c, int on) {
    if (on) {
        if (!g_air_stand || !c->ctrl) return;
        g_air_fall = air_find_ability(c->ctrl, "Fall");
        if (!g_air_fall) { dlog("air: у персонажа не найдена способность Fall — анимацию «как на ровном» не включить"); return; }
        if (!g_air_fallSaved) { g_air_oFallEn = air_get_bool(g_air_fall, "get_Enabled", 1); g_air_fallSaved = 1; }
        if (!air_set_bool(g_air_fall, "set_Enabled", 0) && !air_field_bool(g_air_fall, "m_Enabled", 0, NULL)) dlog("air: Fall.Enabled не записался");
        else dlog("air: способность Fall отключена на время полёта");
        if (air_get_bool(g_air_fall, "get_IsActive", 0)) { air_stop_ability(g_air_fall); dlog("air: Fall уже была активна — остановлена"); }
    } else {
        void* f = c->ctrl ? air_find_ability(c->ctrl, "Fall") : NULL;
        if (f && g_air_fallSaved) {
            if (!air_set_bool(f, "set_Enabled", g_air_oFallEn)) air_field_bool(f, "m_Enabled", g_air_oFallEn, NULL);
            dlog("air: способность Fall возвращена");
        }
        g_air_fall = NULL; g_air_fallSaved = 0;
    }
}

/* ───────────── флаги «сквозь всё» ───────────── */
static void air_flags(const AirCtx* c, int on) {
    int cgOk = 0, dcOk = 0, rbOk = 0;
    air_stand(c, on);
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
static int air_veh_input(const AirCtx* c, float* x, float* y);     /* ниже, после чтения экранных контролов */
static void air_norm2(float* x, float* z) {
    float l = (float)__builtin_sqrt((double)(*x) * (*x) + (double)(*z) * (*z));
    if (l > 1e-4f) { *x /= l; *z /= l; }
}
/* world-вектор движения (длина 0…1). 1 — получилось прочитать ввод (даже нулевой) */
static int air_move_vec(const AirCtx* c, float* out) {
    float in[3]; float R[3], F[3]; void* ct; int fromCam = 1; float mag;
    out[0] = out[1] = out[2] = 0.0f;
    g_air_rawIn[0] = g_air_rawIn[1] = g_air_rawIn[2] = 0.0f;
    if (g_air_mode == 1) {                                           /* в машине: руль и газ/тормоз физики машины */
        if (!air_veh_input(c, &in[0], &in[1])) return 0;
        goto have_input;
    }
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
have_input:
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


/* ───────────── кнопки: Character.Keys (Dictionary<PadKeys,bool>) ───────────── */
#define AIR_KEY_CROUCH 1
#define AIR_KEY_JUMP 5
static int g_air_kInit, g_air_kOffK = -1, g_air_kOffV = -1, g_air_kOffH = -1, g_air_kEsz;
/* состояние кнопок jump и crouch за один проход по записям словаря. 1 — словарь прочитался */
static int air_keys(void* ctrl, int* jump, int* crouch) {
    void *dict, *arr, *ac, *ec; int cnt, i; long len; char* data;
    *jump = *crouch = 0;
    dict = ctrl ? ref_field_s(ctrl, "Keys") : NULL;
    if (!dict) return 0;
    arr = ref_field_s(dict, "_entries"); if (!arr) arr = ref_field_s(dict, "entries");
    if (!arr || !hm_obj_ok(arr)) return 0;
    if (!g_air_kInit) {                                              /* смещения полей записи — один раз */
        void *fk, *fv, *fh; int shift;
        ac = A.object_get_class(arr); ec = A.class_get_element_class(ac);
        fk = ec ? find_field(ec, "key") : NULL; fv = ec ? find_field(ec, "value") : NULL; fh = ec ? find_field(ec, "hashCode") : NULL;
        g_air_kEsz = ec ? arr_elem_size(ec) : 0;
        if (!fk || !fv || g_air_kEsz <= 0) { g_air_kInit = -1; dlog("air: Character.Keys: не разобрать запись словаря — кнопки вверх/вниз не работают"); return 0; }
        shift = A.class_is_valuetype(ec) ? struct_shift(ec) : 0;
        g_air_kOffK = A.field_get_offset(fk) - shift; g_air_kOffV = A.field_get_offset(fv) - shift;
        g_air_kOffH = fh ? A.field_get_offset(fh) - shift : -1;
        g_air_kInit = 1;
        { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "air: Character.Keys: размер записи "); b_int(&x, g_air_kEsz); b_s(&x, ", смещения key/value/hash "); b_int(&x, g_air_kOffK);
          b_ch(&x, '/'); b_int(&x, g_air_kOffV); b_ch(&x, '/'); b_int(&x, g_air_kOffH); dlog(lg); }
    }
    if (g_air_kInit < 0) return 0;
    cnt = int_field(dict, "_count", "count", NULL, -1);
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 64) return 0;
    if (cnt < 0 || cnt > len) cnt = (int)len;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < cnt; i++) {
        char* e = data + (uptr)i * (uptr)g_air_kEsz; int key;
        if (!hm_range_ok(e, g_air_kEsz)) break;
        if (g_air_kOffH >= 0 && *(int*)(e + g_air_kOffH) < 0) continue;     /* свободная запись */
        key = *(int*)(e + g_air_kOffK);
        if (key == AIR_KEY_JUMP) *jump = *(u8*)(e + g_air_kOffV) ? 1 : 0;
        else if (key == AIR_KEY_CROUCH) *crouch = *(u8*)(e + g_air_kOffV) ? 1 : 0;
    }
    return 1;
}

/* ───────────── кнопки, источник №1: экранные контролы JS-интерфейса (OnScreenControls.m_Controls) ─────────────
 * Hud.js: кнопка ПРЫЖОК зовёт window.onScreenButtonCreate("<Keyboard>/leftShift"), джойстик — то же для "<Keyboard>/c" (присед, двойной тап),
 * а нажатие/отпускание — onScreenControlTouchStart/End. В игре это Hassle.WebView.Controls.OnScreenControls со статическим словарём
 * m_Controls (путь → OnWebViewScreenControl). Каждый такой объект пишет нажатие в свой InputControl (OnScreenControl.m_Control, у клавиши это
 * ButtonControl) — читаем у него isPressed. Это настоящее состояние кнопки на экране: оно не зависит от способностей персонажа (Jump и Crouch
 * в воздухе, скорее всего, не стартуют — «земли» под ногами нет), в отличие от Character.Keys. */
typedef struct { void *jump, *crouch, *w, *s, *a, *d, *sp[2]; int nsp, n; } AirBtnCtx;   /* w/s/a/d/пробел — кнопки HUD машины (Hud.js: PedalGas, PedalStop, TurnLeft, TurnRight, HandBrake) */
static void *g_air_oscFld; static int g_air_oscBad, g_air_oscDumped;
static u16 air_lc(u16 c) { return (c >= 'A' && c <= 'Z') ? (u16)(c + 32) : c; }
static int air_str_is(void* s, const char* a) {                    /* строка .NET == ASCII-строке a, без учёта регистра */
    int n, i; const u16* ch;
    if (!s || !hm_obj_ok(s)) return 0;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 64 || !ch || !hm_readable(ch, (unsigned)n * 2u)) return 0;
    for (i = 0; i < n; i++) if (!a[i] || air_lc(ch[i]) != air_lc((u16)(unsigned char)a[i])) return 0;
    return a[n] == 0;
}
static void air_btn_cb(void* it, void* ctx) {
    AirBtnCtx* c = (AirBtnCtx*)ctx; void* p;
    if (!hm_obj_ok(it)) return;
    p = ref_field_s(it, "m_ControlPath");
    if (!p) return;
    c->n++;
    if (air_str_is(p, "<Keyboard>/leftShift")) c->jump = it;
    else if (air_str_is(p, "<Keyboard>/c")) c->crouch = it;
    else if (air_str_is(p, "<Keyboard>/w")) c->w = it;
    else if (air_str_is(p, "<Keyboard>/s")) c->s = it;
    else if (air_str_is(p, "<Keyboard>/a")) c->a = it;
    else if (air_str_is(p, "<Keyboard>/d")) c->d = it;
    else if (air_str_is(p, "<Keyboard>/space") && c->nsp < 2) c->sp[c->nsp++] = it;   /* «/space» (ручник) и «/Space» (спринт джойстика) — разные записи словаря, но одна клавиша */
}
static void air_btn_dump_cb(void* it, void* ctx) {                  /* один раз пишем в журнал, какие экранные контролы вообще есть */
    void* p = hm_obj_ok(it) ? ref_field_s(it, "m_ControlPath") : NULL; char tmp[96]; int cnt; const u16* ch;
    (void)ctx;
    if (!p || !hm_obj_ok(p)) return;
    cnt = A.string_length(p); ch = A.string_chars(p);
    if (cnt <= 0 || cnt > 40 || !ch) return;
    utf16_to_utf8(ch, cnt, tmp, sizeof tmp);
    dlog2("air: экранный контрол: ", tmp);
}
/* находит в словаре контролы прыжка и приседа. 1 — словарь прочитался */
static int air_osc_find(AirBtnCtx* out) {
    void *dict = NULL, *arr; int total;
    out->jump = out->crouch = out->w = out->s = out->a = out->d = out->sp[0] = out->sp[1] = NULL; out->nsp = 0; out->n = 0;
    if (g_air_oscBad || !g_img) return 0;
    if (!g_air_oscFld) {
        void* cls = A.class_from_name(g_img, "Hassle.WebView.Controls", "OnScreenControls");
        g_air_oscFld = cls ? A.class_get_field_from_name(cls, "m_Controls") : NULL;
        if (!g_air_oscFld) { g_air_oscBad = 1; dlog("air: нет OnScreenControls.m_Controls — кнопки читаю из Character.Keys"); return 0; }
    }
    A.field_static_get_value(g_air_oscFld, &dict);
    if (!dict || !hm_obj_ok(dict)) return 0;
    arr = ref_field_s(dict, "_entries"); if (!arr) arr = ref_field_s(dict, "entries");
    if (!arr || !hm_obj_ok(arr)) return 0;
    total = int_field(dict, "_count", "count", NULL, -1);
    walk_struct_array(arr, total, "value", air_btn_cb, out);
    if (!out->jump && !out->crouch && out->n > 0 && !g_air_oscDumped) {
        g_air_oscDumped = 1;
        dlog("air: в OnScreenControls нет <Keyboard>/leftShift и <Keyboard>/c (в машине так и должно быть); есть:");
        walk_struct_array(arr, total, "value", air_btn_dump_cb, NULL);
    }
    return 1;
}
static int air_ctl_pressed(void* oc) {                              /* OnScreenControl.m_Control.isPressed */
    void* ctl = ref_field_s(oc, "m_Control");
    if (!ctl) ctl = air_obj(oc, "get_control");
    if (!ctl || !hm_obj_ok(ctl)) return 0;
    return air_get_bool(ctl, "get_isPressed", 0);
}
/* кнопки HUD машины (Hud.js, блок isSpeedometerShown): W — газ, S — тормоз/назад, A/D — руль (только при кнопочном управлении; при джойстике руль — стик),
   ПРОБЕЛ — ручник (всегда). Прыжка в машине нет; присед (<Keyboard>/c) есть только при джойстике (двойной тап). 1 — словарь контролов прочитался */
typedef struct { int w, s, a, d, sp; } AirCarKeys;
static AirCarKeys g_air_vkeys; static int g_air_vkFound = -1;
static int air_car_keys(AirCarKeys* k) {
    AirBtnCtx b; int i;
    k->w = k->s = k->a = k->d = k->sp = 0;
    if (!air_osc_find(&b)) return 0;
    if (b.w) k->w = air_ctl_pressed(b.w);
    if (b.s) k->s = air_ctl_pressed(b.s);
    if (b.a) k->a = air_ctl_pressed(b.a);
    if (b.d) k->d = air_ctl_pressed(b.d);
    for (i = 0; i < b.nsp; i++) if (air_ctl_pressed(b.sp[i])) k->sp = 1;
    return (b.w || b.s || b.a || b.d || b.nsp) ? 1 : 0;
}
/* ввод в машине. Источники: (1) кнопки HUD (W/S/A/D) — самое прямое, не зависит от физики; если ни одна не нажата — (2) физика машины WheeledVehiclePhysics
   (SteerInput/ThrottleInput/BrakeInput; их пишет VehicleInput.Update — это даёт стик-руль при управлении джойстиком). Руль: вправо +, газ − тормоз: вперёд +.
   Заодно g_air_vsp — зажат ли ПРОБЕЛ (ручник): тогда газ/тормоз двигают вверх/вниз (режим высоты). 1 — хоть что-то прочиталось */
static int g_air_vsp;
static int air_veh_input(const AirCtx* c, float* x, float* y) {
    void* ph; float st = 0.0f, th = 0.0f, br = 0.0f; int physOk = 0, kOk;
    kOk = air_car_keys(&g_air_vkeys); g_air_vkFound = kOk;
    g_air_vsp = kOk ? g_air_vkeys.sp : 0;
    ph = c->veh ? ref_field_s(c->veh, "m_Physics") : NULL;          /* WheeledVehicle.m_Physics; у вертолёта/самолёта/лодки такого поля нет */
    if (ph && hm_obj_ok(ph) && air_read_f(ph, "SteerInput", &st, 1) && air_read_f(ph, "ThrottleInput", &th, 1)) {
        air_read_f(ph, "BrakeInput", &br, 1);
        g_air_vin[0] = st; g_air_vin[1] = th; g_air_vin[2] = br; physOk = 1;
    }
    g_air_vinOk = physOk;
    if (kOk && (g_air_vkeys.w || g_air_vkeys.s || g_air_vkeys.a || g_air_vkeys.d)) {   /* кнопки нажаты — они главные */
        *x = (float)(g_air_vkeys.d - g_air_vkeys.a); *y = (float)(g_air_vkeys.w - g_air_vkeys.s);
        if (physOk && (st > 0.05f || st < -0.05f) && g_air_vkeys.a == g_air_vkeys.d) *x = st;   /* кнопки газа/тормоза + руль стиком */
        return 1;
    }
    if (physOk) {
        if (st > 1.0f) { st = 1.0f; } else if (st < -1.0f) { st = -1.0f; }
        *x = st; *y = th - br;
        if (*y > 1.0f) { *y = 1.0f; } else if (*y < -1.0f) { *y = -1.0f; }
        return 1;
    }
    if (kOk) { *x = 0.0f; *y = 0.0f; return 1; }                     /* кнопки найдены, не нажаты, физика не читается — стоим на месте */
    return 0;
}
/* состояние ПРЫЖКА и ПРИСЕДА. Возврат — источник: 2 экранные контролы (основной), 1 Character.Keys (запасной), 0 — ничего не прочиталось.
   Если экранные контролы нашлись, Character.Keys НЕ смешиваем: он в воздухе устроен иначе, и «залипший» присед тянул бы вас вниз после отпускания. */
static int air_buttons(void* ctrl, int* jump, int* crouch) {
    AirBtnCtx b;
    *jump = *crouch = 0;
    if (air_osc_find(&b) && (b.jump || b.crouch)) {
        if (b.jump) *jump = air_ctl_pressed(b.jump);
        if (b.crouch) *crouch = air_ctl_pressed(b.crouch);
        return 2;
    }
    return air_keys(ctrl, jump, crouch) ? 1 : 0;
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

/* ───────────── автошкола: экзамен по вождению ─────────────
 * Как экзамен проверяет игрока (серверный autoschool.pwn — по нему это и сделано):
 *   • маршрут — цепочка SetPlayerRaceCheckpoint (тип RCP_ACTION_TYPE_DRIVING_EXAM, размер 3.5 м); следующая точка выдаётся при входе в текущую;
 *   • 7 знаков STOP — динамические сферы радиусом 7 м. Вошли в сферу — сервер сверяет курс машины: знак вообще участвует только если курс в пределах ±65° от угла
 *     знака (угол = направление, в котором к знаку подъезжают; с другой стороны знак молча не считается и не штрафуется). Дальше каждые 500 мс: машина ещё в сфере
 *     и скорость < 1 → знак засчитан; скорость ≥ 1 → ждёт ещё 500 мс; вышли из сферы без остановки → предупреждение;
 *   • в конце: засчитанных знаков не меньше минимума (экзамен №1 — 2, №2 — 3, №3 — 6), иначе провал. Предупреждений можно: №1 — одно, №2 и №3 — ни одного.
 * Обычный автолёт /air mark cp этого не умеет: летит с постоянной скоростью, курс машины не меняет (у знаков курс «не тот» — знак не засчитан) и проносится через
 * сферу знака быстрее, чем за 500 мс (предупреждение).
 * Режим экзамена (включается сам, когда маркер сервера — старт экзамена; вручную: /air exam on|off|auto):
 *   – курс: машина разворачивается по направлению движения (до 150°/с; если поворот больше 25° — стоит и крутится на месте);
 *   – скорость своя: /air exam speed N (по умолчанию 14 м/с ≈ 50 км/ч, потолок 30);
 *   – знак впереди и курс подходит (±60°): с 30 м плавно тормозим, в 5 м от центра знака (внутри сферы 7 м) останавливаемся и стоим 2.2 с (4 проверки сервера), едем дальше;
 *   – машина пропала (экзамен закончился) — режим сам выключается.
 * Остановки по чату (/air exam chat on): сервер сам сообщает, что знак засчитан в работу («[Инструктор]: Здесь аккуратней…» — машина вошла в сферу с подходящим курсом),
 *   мод по этой строке ставит машину на месте мгновенно (скорость 0) и стоит до «[Инструктор]: Отлично, вы не пропустили дорожный знак…» (но не дольше 5 с). Таблица знаков
 *   при этом остаётся запасным путём: она плавно сбавляет скорость к знаку. Приветствие инструктора включает режим экзамена само (если летите на машине).
 * Чего режим НЕ делает: не пристёгивает ремень (пристегните сами ДО второй точки — иначе предупреждение) и не смотрит на светофоры (красный/жёлтый на выезде из зоны —
 *   предупреждение; на маршруте №1 один такой перекрёсток есть, на №2 и №3 — несколько). Данные знаков и точка старта — из серверного autoschool.inc.
 * Координаты: SAMP (x, y, z) → Unity (x, z, y); курс — единичный вектор (x, z) в Unity, (sin ψ, cos ψ), ψ = −(угол SAMP).
 */
#define AIR_EX_NSIGN 7
#define AIR_EX_HALT_R 5.0f                        /* останавливаемся, когда до центра знака столько (сфера сервера — 7 м) */
#define AIR_EX_SLOW_R 30.0f                       /* с такого расстояния до знака тормозим */
#define AIR_EX_REARM_R 45.0f                      /* дальше этого от знака он снова «не пройден» */
#define AIR_EX_HOLD_MS 2200                       /* стоим у знака (сервер смотрит скорость каждые 500 мс) */
#define AIR_EX_SPEED 14.0f                        /* скорость по экзамену, м/с */
#define AIR_EX_MAXSPEED 30.0f
#define AIR_EX_VAPP_K 0.35f                       /* скорость подъезда к знаку = K × расстояние до него, но не меньше VAPP_MIN */
#define AIR_EX_VAPP_MIN 1.2f
#define AIR_EX_GATE_COS 0.5f                      /* cos 60°: допуск курса у знака (сервер — ±65°) */
#define AIR_EX_ALIGN_COS 0.906f                   /* cos 25°: пока курс отличается больше — крутимся на месте */
#define AIR_EX_TURN_DEG 150.0f                    /* скорость поворота, °/с */
#define AIR_EX_START_R 4.0f                       /* маркер ближе этого к первой точке маршрута — это старт экзамена */
#define AIR_EX_LOST_MS 8000                       /* маркера нет столько — в авто-режиме считаем, что экзамен закончился */
static const float g_air_exSigns[AIR_EX_NSIGN][4] = {   /* Unity x, z; единичный вектор курса подъезда (−sin A, cos A), A — угол знака на сервере */
    { 2072.3721f, 2942.4087f,  0.00000f,  1.00000f },  /* №1: SAMP (2072.37, 2942.41, 11.55), угол 360 */
    {   18.0644f,  895.9708f, -0.34202f, -0.93969f },  /* №2: SAMP (18.06, 895.97, 11.42), угол 160 */
    { 1888.0110f, 2067.9128f,  0.00000f, -1.00000f },  /* №3: SAMP (1888.01, 2067.91, 15.34), угол 180 */
    { -440.2103f, 1240.0844f,  0.00000f, -1.00000f },  /* №4: SAMP (-440.21, 1240.08, 17.14), угол 180 */
    {  -53.6132f,  540.8301f,  0.17365f,  0.98481f },  /* №5: SAMP (-53.61, 540.83, 11.61), угол 350 */
    {  373.4376f,  713.9597f,  0.34202f,  0.93969f },  /* №6: SAMP (373.44, 713.96, 11.59), угол 340 */
    {  687.2881f,  595.4058f,  0.34202f,  0.93969f },  /* №7: SAMP (687.29, 595.41, 11.61), угол 340 */
};
static const float g_air_exStart[2] = { 1896.4409f, 1918.2350f };   /* первая точка всех трёх маршрутов (Unity x, z) */
static int g_air_exMode;                          /* /air exam: 0 — авто (по старту экзамена), 1 — включён всегда, 2 — выключен */
static int g_air_exOn;                            /* сейчас ведём себя как на экзамене */
static float g_air_exSpeed = AIR_EX_SPEED;
static float g_air_hx, g_air_hz;                  /* курс машины, который держим: единичный вектор (Unity x, z) */
static int g_air_hOk, g_air_exRotWarn;
static unsigned g_air_exDone;                     /* биты знаков, у которых уже стояли на этом проходе */
static int g_air_exSign = -1;                     /* у какого знака стоим сейчас (-1 — ни у какого) */
static long long g_air_exUntil, g_air_exLost;
static int g_air_exStops;                         /* сколько остановок у знаков сделано (для /air status) */

/* ───────────── чат инструктора: перехват WebViewSystem.AddChatMessage (только arm64) ─────────────
 * Зачем: сервер сам говорит, когда знак STOP «взят»: при входе в сферу с подходящим курсом он пишет «[Инструктор]: Здесь аккуратней, сосредоточьтесь.»,
 * а когда увидел остановку — «[Инструктор]: Отлично, вы не пропустили дорожный знак, можете ехать дальше.». Мод ловит эти строки и ставит машину на месте
 * мгновенно (машина kinematic — тормозного пути нет), а по «Отлично» едет дальше. Координаты, углы и маршруты знать не нужно: поменяли знаки на сервере —
 * мод ничего не заметит. Приветствие «Приветствую на практической части…» включает режим экзамена само.
 * Откуда строки: серверный SendClientMessage → Hassle.Network.RPCs.OnClientMessage читает цвет и строку и сразу зовёт WebViewSystem.AddChatMessage(message, color)
 * (проверено по libil2cpp.so: в функции нет ничего, кроме ReadInt32, ReadString, int.ToString и этого вызова), поэтому строка приходит целиком, с «{FFFFFF}» внутри.
 * Как перехвачено: у AddChatMessage первые 16 байт заменяются прыжком в заглушку (как AudioStream.PutData в hassle_voice.c): заглушка
 * сохраняет x0–x7, зовёт airc_hook(message, color), восстанавливает и выполняет затёртые инструкции. Отличие от voice: 4-я инструкция AddChatMessage — ADRP x22
 * (зависит от адреса), поэтому заглушка кладёт в x22 готовую константу — адрес страницы, посчитанный здесь же (проверено на байтах настоящего libil2cpp.so).
 * По умолчанию ВЫКЛЮЧЕНО (код правит игру в памяти и на устройстве не проверялся): /air exam chat on — поставить, /air exam chat off — снять. Без хука режим экзамена
 * работает по таблице знаков (см. выше). Перехват ставится из команды (главный поток, игра уже загружена).
 */
#define AIRC_TRAMP_WORDS 24
#define AIRC_HOLD_MAX_MS 5000                     /* «Здесь аккуратней» без «Отлично» дольше этого — едем дальше сами (на случай, если сервер знак не засчитал) */
#if defined(__aarch64__) || defined(AIRC_TEST_HOOK)
#define AIRC_HOOK 1
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int mprotect(void*, unsigned long, int);
extern long pwrite(int, const void*, unsigned long, long);
#endif
static volatile int g_airc_state;                 /* 0 — не ставили, 1 — стоит, -1 — не вышло */
static volatile int g_airc_seen;                  /* сколько строк инструктора увидели */
static volatile long long g_airc_stopT, g_airc_okT, g_airc_startT;   /* когда пришли «Здесь аккуратней» / «Отлично…» / приветствие (монотонные мс; 0 — ещё не было) */
static long long g_airc_startSeen;                /* приветствие, на которое мод уже ответил */
static char g_airc_why[120];
static int airc_fail(const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof g_airc_why - 1; i++) g_airc_why[i] = why[i];
    g_airc_why[i] = 0;
    g_airc_state = -1;
    dlog2("air: exam: перехват чата не установлен: ", why);
    return 0;
}
static int airc_has(const char* h, const char* n) {                 /* n входит в h? */
    int i, k;
    for (i = 0; h[i]; i++) { for (k = 0; n[k] && h[i + k] == n[k]; k++) {} if (!n[k]) return 1; }
    return 0;
}
/* Разбор строки чата (UTF-8). Отдельно от хука — чтобы проверять без игры. Реагирует только на строки с «[Инструктор]» (чужая реплика игрока не остановит машину).
   В хуке — только запись меток времени: ни журнала, ни выделения памяти, ни вызовов игры. */
static void airc_handle_text(const char* t, long long nowMs) {
    if (!airc_has(t, "[Инструктор]")) return;
    __atomic_fetch_add(&g_airc_seen, 1, __ATOMIC_RELAXED);
    if (airc_has(t, "Здесь аккуратней")) __atomic_store_n(&g_airc_stopT, nowMs, __ATOMIC_RELEASE);
    else if (airc_has(t, "не пропустили дорожный знак") || airc_has(t, "проехали мимо дорожного знака")) __atomic_store_n(&g_airc_okT, nowMs, __ATOMIC_RELEASE);
    else if (airc_has(t, "Приветствую на практической части")) __atomic_store_n(&g_airc_startT, nowMs, __ATOMIC_RELEASE);
}
/* Зовётся из заглушки в начале AddChatMessage — до того как игра обработает строку. Без выделения памяти и вызовов IL2CPP, кроме чтения строки */
__attribute__((noinline, used))
static void airc_hook(void* msg, void* color) {
    char t[220]; int n;
    (void)color;
    if (!msg || !A.string_length || !A.string_chars) return;
    n = A.string_length(msg);
    if (n < 10 || n > 800) return;
    if (n > 200) n = 200;
    utf16_to_utf8(A.string_chars(msg), n, t, sizeof t);
    airc_handle_text(t, air_now_ns() / 1000000);
}
/* Инструкция зависит от своего адреса? (то же, что vc_pc_rel в hassle_voice.c) */
static int airc_pc_rel(u32 w) {
    if ((w & 0x1F000000u) == 0x10000000u) return 1;              /* ADR, ADRP */
    if ((w & 0x3B000000u) == 0x18000000u) return 1;              /* LDR / LDRSW / PRFM (literal) */
    if ((w & 0x7C000000u) == 0x14000000u) return 1;              /* B, BL */
    if ((w & 0xFF000010u) == 0x54000000u) return 1;              /* B.cond */
    if ((w & 0x7E000000u) == 0x34000000u) return 1;              /* CBZ, CBNZ */
    if ((w & 0x7E000000u) == 0x36000000u) return 1;              /* TBZ, TBNZ */
    return 0;
}
static int airc_is_adrp(u32 w) { return (w & 0x9F000000u) == 0x90000000u; }
/* Что кладёт ADRP по адресу pc: адрес страницы (pc без 12 младших бит + imm21 × 4096) */
static uptr airc_adrp_page(u32 w, uptr pc) {
    long imm = (long)((((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u));
    if (imm & 0x100000L) imm -= 0x200000L;
    return (pc & ~(uptr)4095) + (uptr)(imm * 4096L);
}
/* Заглушка (arm64), 24 слова. 0–11: сохранить x0–x7, x30, позвать hook, восстановить (как в hassle_voice.c); 12–14: три затёртые инструкции как есть;
   15: четвёртая — как есть, а если это ADRP Xd — «ldr Xd, =страница» (слова 22–23); 16–17: «ldr x16, =fn+16; br x16» (слова 20–21); 18–19: адрес hook.
   fn — адрес начала функции (нужен для ADRP). Возвращает 0, если затёртые инструкции перенести нельзя. */
static int airc_make_tramp(u32* t, const u32* orig4, uptr fn, uptr hook) {
    int i; uptr resume = fn + 16, page;
    for (i = 0; i < AIRC_TRAMP_WORDS; i++) t[i] = 0;
    t[0] = 0xA9BB07E0u; t[1] = 0xA9010FE2u; t[2] = 0xA90217E4u; t[3] = 0xA9031FE6u;
    t[4] = 0xF90023FEu; t[5] = 0x580001B0u; t[6] = 0xD63F0200u;
    t[7] = 0xF94023FEu; t[8] = 0xA9431FE6u; t[9] = 0xA94217E4u; t[10] = 0xA9410FE2u; t[11] = 0xA8C507E0u;
    for (i = 0; i < 3; i++) { if (airc_pc_rel(orig4[i])) return 0; t[12 + i] = orig4[i]; }
    if (airc_is_adrp(orig4[3])) {
        page = airc_adrp_page(orig4[3], fn + 12);
        t[15] = 0x58000000u | (7u << 5) | (orig4[3] & 31u);                 /* ldr Xd, [pc, +28] → слово 22 */
        t[22] = (u32)(page & 0xFFFFFFFFu); t[23] = (u32)(((unsigned long long)page) >> 32);
    } else {
        if (airc_pc_rel(orig4[3])) return 0;
        t[15] = orig4[3];
    }
    t[16] = 0x58000090u; t[17] = 0xD61F0200u;
    t[18] = (u32)(hook & 0xFFFFFFFFu);   t[19] = (u32)(((unsigned long long)hook) >> 32);
    t[20] = (u32)(resume & 0xFFFFFFFFu); t[21] = (u32)(((unsigned long long)resume) >> 32);
    return 1;
}
static void airc_make_entry(u32* e, uptr tramp) {                   /* ldr x16,[pc,#8]; br x16; .quad адрес заглушки */
    e[0] = 0x58000050u; e[1] = 0xD61F0200u;
    e[2] = (u32)(tramp & 0xFFFFFFFFu); e[3] = (u32)(((unsigned long long)tramp) >> 32);
}
#ifdef AIRC_HOOK
static void* g_airc_veneer; static uptr g_airc_site; static u32 g_airc_orig[4];
static void airc_flush_cache(void* p, unsigned n) {
#if defined(__aarch64__)
    uptr ctr, dl, il, a, e;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    dl = 4u << ((ctr >> 16) & 15u); il = 4u << (ctr & 15u);
    e = (uptr)p + n;
    for (a = (uptr)p & ~(dl - 1); a < e; a += dl) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uptr)p & ~(il - 1); a < e; a += il) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
#else
    (void)p; (void)n;
#endif
}
static int airc_write_code(uptr addr, const u32* w, int n) {         /* слова пишутся с конца к началу; запасной путь — /proc/self/mem */
    uptr p0 = addr & ~(uptr)4095, p1 = (addr + (uptr)n * 4 - 1) & ~(uptr)4095;
    unsigned long len = (unsigned long)(p1 - p0) + 4096;
    int i;
    if (mprotect((void*)p0, len, 7) == 0) {
        for (i = n - 1; i >= 0; i--) ((volatile u32*)addr)[i] = w[i];
        airc_flush_cache((void*)addr, (unsigned)n * 4);
        mprotect((void*)p0, len, 5);
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, w, (unsigned long)n * 4, (long)addr); close(fd); }
        if (r == (long)n * 4) { airc_flush_cache((void*)addr, (unsigned)n * 4); return 1; }
    }
    return 0;
}
static int airc_install(void) {
    void *kls, *m; uptr fn; u32 code[16], v[AIRC_TRAMP_WORDS], e[4]; int i;
    if (g_airc_state == 1) return 1;
    if (!g_safe || !il2cpp_light() || !g_img) return airc_fail("игра ещё не готова");
    kls = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    m = kls ? A.class_get_method_from_name(kls, "AddChatMessage", 2) : NULL;
    if (!m) return airc_fail("нет WebViewSystem.AddChatMessage(string, string)");
    fn = *(uptr*)m;
    if (!fn || (fn & 3) || !hm_range_ok((void*)fn, sizeof code)) return airc_fail("адрес AddChatMessage нечитаем");
    for (i = 0; i < 16; i++) code[i] = ((const u32*)fn)[i];
    if (g_airc_veneer && g_airc_site == fn) for (i = 0; i < 4; i++) code[i] = g_airc_orig[i];     /* уже патчили (снятие не удалось) — берём сохранённые исходные слова */
    if (!airc_make_tramp(v, code, fn, (uptr)airc_hook)) return airc_fail("начало AddChatMessage зависит от адреса — перенести нельзя (другая сборка игры?)");
    if (!(g_airc_veneer && g_airc_site == fn)) {
        void* page = mmap(NULL, 4096, 3, 0x22, -1, 0);                 /* R|W, MAP_PRIVATE|MAP_ANONYMOUS */
        if (page == (void*)-1 || !page) return airc_fail("не удалось выделить страницу под заглушку");
        for (i = 0; i < AIRC_TRAMP_WORDS; i++) ((u32*)page)[i] = v[i];
        airc_flush_cache(page, sizeof v);
        if (mprotect(page, 4096, 5) != 0) { munmap(page, 4096); return airc_fail("не удалось сделать страницу заглушки исполняемой"); }
        g_airc_veneer = page; g_airc_site = fn;
        for (i = 0; i < 4; i++) g_airc_orig[i] = code[i];
    }
    airc_make_entry(e, (uptr)g_airc_veneer);
    if (!airc_write_code(fn, e, 4)) return airc_fail("запись в код игры запрещена системой");
    g_airc_state = 1;
    dlog("air: exam: перехват AddChatMessage установлен");
    return 1;
}
static void airc_unhook(void) {
    if (g_airc_state != 1) return;
    if (airc_write_code(g_airc_site, g_airc_orig, 4)) { g_airc_state = 0; dlog("air: exam: перехват AddChatMessage снят, код игры восстановлен"); }
    else dlog("air: exam: не удалось вернуть исходные инструкции AddChatMessage");
}
#else
static int airc_install(void) { return airc_fail("поддерживается только arm64"); }
static void airc_unhook(void) {}
#endif
static int g_air_chHold;                          /* сейчас стоим по команде инструктора (чтобы писать в чат только смену состояния) */
static int g_air_chStops;                         /* сколько раз встали по «Здесь аккуратней» (для /air status) */


static int air_set_q(void* obj, const char* setter, const float* q) {   /* сеттер с одним Quaternion (4 float) */
    void *m, *exc = NULL, *args[1]; float v[4];
    if (!obj) return 0;
    v[0] = q[0]; v[1] = q[1]; v[2] = q[2]; v[3] = q[3];
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = v;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
/* поставить машине курс (поворот вокруг вертикали): Transform.rotation (+ Rigidbody.rotation, если он на том же Transform). (hx, hz) — единичный вектор (sin ψ, cos ψ) */
static void air_vyaw(void* veh, float hx, float hz) {
    void *tr = veh ? air_veh_tr(veh) : NULL, *rb; float q[4], a = (1.0f + hz) * 0.5f, b = (1.0f - hz) * 0.5f;
    if (!tr) return;
    if (a < 0.0f) a = 0.0f;
    if (b < 0.0f) b = 0.0f;
    q[0] = 0.0f; q[1] = (hx < 0.0f ? -1.0f : 1.0f) * (float)__builtin_sqrt((double)b); q[2] = 0.0f; q[3] = (float)__builtin_sqrt((double)a);   /* (0, sin ψ/2, 0, cos ψ/2) */
    if (!air_set_q(tr, "set_rotation", q)) {
        if (!g_air_exRotWarn) { g_air_exRotWarn = 1; dlog("air: exam: Transform.set_rotation не сработал — курс машины не меняется, знаки STOP не засчитаются"); }
        return;
    }
    if (g_air_vrbSame) { rb = air_veh_rb(veh); if (rb) air_set_q(rb, "set_rotation", q); }
}
/* куда сейчас смотрит машина (Transform.forward по горизонтали). 0 — не прочиталось или смотрит почти вертикально */
static int air_vheading(void* veh, float* hx, float* hz) {
    float f[3], l; void* tr = veh ? air_veh_tr(veh) : NULL;
    if (!tr || !air_vec(tr, "get_forward", f)) return 0;
    l = (float)__builtin_sqrt((double)f[0] * f[0] + (double)f[2] * f[2]);
    if (l < 0.3f) return 0;
    *hx = f[0] / l; *hz = f[2] / l;
    return 1;
}
/* довернуть курс (g_air_hx, g_air_hz) к направлению (tx, tz) — не быстрее AIR_EX_TURN_DEG °/с. Возвращает cos угла между курсом и целью после поворота */
static float air_turn(float tx, float tz, float dt, float deg) {
    float dot = g_air_hx * tx + g_air_hz * tz, sd = tx * g_air_hz - tz * g_air_hx;      /* sd > 0 — цель правее (ψ больше) */
    float a = deg * dt * 0.0174533f, a2 = a * a, nx, nz, l;
    float cm = 1.0f - a2 * 0.5f + a2 * a2 * (1.0f / 24.0f), sm = a - a2 * a * (1.0f / 6.0f) + a2 * a2 * a * (1.0f / 120.0f);
    if (dot >= cm) { g_air_hx = tx; g_air_hz = tz; return 1.0f; }          /* угол меньше шага — сразу на цель */
    if (sd < 0.0f) sm = -sm;
    nx = g_air_hx * cm + g_air_hz * sm; nz = g_air_hz * cm - g_air_hx * sm;
    l = (float)__builtin_sqrt((double)nx * nx + (double)nz * nz);
    if (l > 1e-4f) { g_air_hx = nx / l; g_air_hz = nz / l; }
    return g_air_hx * tx + g_air_hz * tz;
}
static float air_ex_turn(float tx, float tz, float dt) { return air_turn(tx, tz, dt, AIR_EX_TURN_DEG); }

/* Курс машины в обычном полёте (не экзамен): нос смотрит туда, куда летим. Раньше курс ставился только на экзамене (air_vyaw под g_air_exOn), а в остальное время
   мы двигали одну позицию, и машина летела к метке боком — с той ориентацией, в которой стояла при включении.
   d — смещение точки hold за этот кадр (м); по горизонтали берём направление, медленнее AIR_FACE_MIN_V м/с курс не меняем (стоим/висим/движемся только вверх-вниз).
   allowTurn = 0 — едем назад (S): курс не трогаем, иначе машина развернулась бы кормой вперёд. */
#define AIR_FACE_TURN_DEG 360.0f
#define AIR_FACE_MIN_V 0.6f
static void air_face_step(void* veh, const float* d, float dt, int allowTurn) {
    float hl = (float)__builtin_sqrt((double)d[0] * d[0] + (double)d[2] * d[2]);
    if (allowTurn && dt > 1e-4f && hl / dt >= AIR_FACE_MIN_V) {
        float tx = d[0] / hl, tz = d[2] / hl;
        if (!g_air_hOk) {                                            /* первый раз: стартуем с того курса, в котором машина стоит сейчас — без рывка */
            if (!air_vheading(veh, &g_air_hx, &g_air_hz)) { g_air_hx = tx; g_air_hz = tz; }
            g_air_hOk = 1;
        }
        air_turn(tx, tz, dt, AIR_FACE_TURN_DEG);
    }
    if (g_air_hOk) air_vyaw(veh, g_air_hx, g_air_hz);                /* курс держим каждый кадр (ставит и Rigidbody, если он на том же Transform) */
}
static void air_ex_start(const char* why, long long nowMs) {
    g_air_exOn = 1; g_air_hOk = 0; g_air_exDone = 0; g_air_exSign = -1; g_air_exStops = 0; g_air_exLost = nowMs; g_air_chHold = 0;
    { char m[220]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "/air exam: "); b_s(&b, why); b_s(&b, " — курс по дороге, остановки у знаков STOP. Ремень — пристегните сами, светофоры мод не смотрит. /air exam off — выключить"); chat_say(m); }
    dlog2("air: exam: включён: ", why);
}
static void air_ex_end(const char* why) {                           /* why == NULL — молча (выход из полёта) */
    if (g_air_exOn && why) { char m[160]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "/air exam: "); b_s(&b, why); chat_say(m); }
    if (g_air_exOn) dlog2("air: exam: выключен: ", why ? why : "выход из полёта");
    g_air_exOn = 0; g_air_hOk = 0; g_air_exDone = 0; g_air_exSign = -1; g_air_chHold = 0;
}
/* вызывается при каждом чтении цели автолёта (раз в 150 мс): включает/выключает режим экзамена. kind: 1 — маркер сервера (np — его позиция), иначе — нет маркера сервера */
static void air_ex_watch(int kind, const float* np, long long nowMs) {
    if (g_air_exMode == 2) return;
    if (g_airc_state == 1) {                                        /* перехват чата: инструктор поздоровался — экзамен начался */
        long long gT = __atomic_load_n(&g_airc_startT, __ATOMIC_ACQUIRE);
        if (gT != g_airc_startSeen) { g_airc_startSeen = gT; if (!g_air_exOn && g_air_mode == 1) air_ex_start("инструктор начал экзамен (по чату)", nowMs); }
    }
    if (kind == 1) {
        g_air_exLost = nowMs;
        if (!g_air_exOn && g_air_mode == 1) {
            if (g_air_exMode == 1) air_ex_start("режим экзамена включён вручную", nowMs);
            else {
                float dx = np[0] - g_air_exStart[0], dz = np[2] - g_air_exStart[1];
                if (dx * dx + dz * dz <= AIR_EX_START_R * AIR_EX_START_R) air_ex_start("вижу старт экзамена в автошколе", nowMs);
            }
        }
    } else if (g_air_exOn && g_air_exMode == 0 && nowMs - g_air_exLost > AIR_EX_LOST_MS) air_ex_end("маркера экзамена давно нет — режим выключен");
}
/* Один шаг «вождения по экзамену». hold — где машина сейчас (Unity), d — вектор до цели. Возвращает 0, если на этом тике двигаться не надо (стоим у знака или
   разворачиваемся на месте), иначе 1 и в *spd — скорость, м/с */
static int air_ex_drive(void* veh, const float* hold, const float* d, long long nowMs, float dt, float* spd) {
    float hl = (float)__builtin_sqrt((double)d[0] * d[0] + (double)d[2] * d[2]), tx, tz, ac, bestD = 1e9f; int i, best = -1;
    *spd = g_air_exSpeed;
    if (hl > 0.5f) { tx = d[0] / hl; tz = d[2] / hl; } else { tx = g_air_hx; tz = g_air_hz; }   /* почти над точкой — курс не меняем */
    if (!g_air_hOk) {                                              /* первый шаг: курс — как машина стоит сейчас */
        char lg[120]; Buf x;
        if (!air_vheading(veh, &g_air_hx, &g_air_hz)) { g_air_hx = hl > 0.5f ? tx : 0.0f; g_air_hz = hl > 0.5f ? tz : 1.0f; }
        g_air_hOk = 1;
        b_init(&x, lg, sizeof lg); b_s(&x, "air: exam: начальный курс машины (x z): "); b_fix(&x, g_air_hx, 2); b_ch(&x, ' '); b_fix(&x, g_air_hz, 2); dlog(lg);
        if (hl <= 0.5f) { tx = g_air_hx; tz = g_air_hz; }
    }
    ac = air_ex_turn(tx, tz, dt);
    if (g_airc_state == 1) {                                       /* чат: «Здесь аккуратней» — встаём сразу; едем после «Отлично» (или через AIRC_HOLD_MAX_MS) */
        long long sT = __atomic_load_n(&g_airc_stopT, __ATOMIC_ACQUIRE), oT = __atomic_load_n(&g_airc_okT, __ATOMIC_ACQUIRE);
        int hd = sT > 0 && sT > oT && nowMs - sT < AIRC_HOLD_MAX_MS;
        if (hd && !g_air_chHold) {
            char lg[120]; Buf x;
            g_air_chHold = 1; g_air_chStops++;
            b_init(&x, lg, sizeof lg); b_s(&x, "air: exam: чат: «Здесь аккуратней» — встал, с прихода строки прошло мс: "); b_int(&x, (int)(nowMs - sT)); dlog(lg);
            chat_say("/air exam: инструктор: «Здесь аккуратней» — встал");
        } else if (!hd && g_air_chHold) {
            g_air_chHold = 0;
            if (oT >= sT) {                                        /* знак засчитан: у ближайшего знака таблицы больше не стоим */
                int k, nb = -1; float bd = 15.0f;
                for (k = 0; k < AIR_EX_NSIGN; k++) {
                    float qx = g_air_exSigns[k][0] - hold[0], qz = g_air_exSigns[k][1] - hold[2], qd = (float)__builtin_sqrt((double)qx * qx + (double)qz * qz);
                    if (qd < bd) { bd = qd; nb = k; }
                }
                if (nb >= 0) g_air_exDone |= 1u << nb;
                dlog("air: exam: чат: «Отлично» — едем дальше");
                chat_say("/air exam: инструктор: «Отлично» — еду дальше");
            } else {
                dlog("air: exam: чат: «Отлично» не пришло за 5 с — едем дальше сами");
                chat_say("/air exam: «Отлично» не пришло за 5 с — еду дальше");
            }
        }
        if (hd) { *spd = 0.0f; return 0; }
    }
    if (g_air_exSign >= 0) {                                       /* стоим у знака */
        if (nowMs < g_air_exUntil) { *spd = 0.0f; return 0; }
        g_air_exDone |= 1u << g_air_exSign;
        { char m[100]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "/air exam: знак STOP №"); b_int(&b, g_air_exSign + 1); b_s(&b, " — отстояли, едем дальше"); chat_say(m); }
        g_air_exSign = -1;
    }
    if (ac < AIR_EX_ALIGN_COS) { *spd = 0.0f; return 0; }          /* курс ещё не тот — крутимся на месте */
    for (i = 0; i < AIR_EX_NSIGN; i++) {
        float sx = g_air_exSigns[i][0] - hold[0], sz = g_air_exSigns[i][1] - hold[2], dn = (float)__builtin_sqrt((double)sx * sx + (double)sz * sz);
        if (dn > AIR_EX_REARM_R) { g_air_exDone &= ~(1u << i); continue; }
        if ((g_air_exDone >> i) & 1u) continue;                    /* на этом проходе уже стояли */
        if (dn > AIR_EX_SLOW_R) continue;
        if (g_air_hx * g_air_exSigns[i][2] + g_air_hz * g_air_exSigns[i][3] < AIR_EX_GATE_COS) continue;   /* курс не тот: сервер этот знак не засчитает (и не накажет) */
        if (sx * g_air_hx + sz * g_air_hz < -2.0f && dn > 2.0f) continue;                                /* знак уже позади */
        if (dn < bestD) { bestD = dn; best = i; }
    }
    if (best >= 0) {
        if (bestD <= AIR_EX_HALT_R) {                              /* подъехали — встаём */
            char m[120]; Buf b;
            g_air_exSign = best; g_air_exUntil = nowMs + AIR_EX_HOLD_MS; g_air_exStops++;
            b_init(&b, m, sizeof m); b_s(&b, "/air exam: знак STOP №"); b_int(&b, best + 1); b_s(&b, " — стою, до центра "); b_fix(&b, bestD, 1); b_s(&b, " м");
            chat_say(m);
            *spd = 0.0f; return 0;
        } else {                                                   /* подъезд: плавно тормозим */
            float v = AIR_EX_VAPP_K * bestD;
            if (v < AIR_EX_VAPP_MIN) v = AIR_EX_VAPP_MIN;
            if (v < *spd) *spd = v;
        }
    }
    return 1;
}

/* ───────────── красные метки: маркер сервера и метка карты ───────────── */
static int air_obj_active(void* comp) {                             /* GameObject.activeInHierarchy; не прочиталось — считаем активным */
    void *go = air_obj(comp, "get_gameObject"), *m, *exc = NULL, *r;
    if (!go) return 0;
    m = find_method(A.object_get_class(go), "get_activeInHierarchy", 0);
    if (!m) return 1;
    r = A.runtime_invoke(m, go, NULL, &exc);
    return (!exc && r && hm_readable((char*)r + HDR, 1)) ? (*((u8*)r + HDR) != 0) : 1;
}
static int air_chk_pos(const char* fld, float* out) {              /* Checkpoint.Simple / Checkpoint.Race */
    void *cls, *f, *obj = NULL, *tr;
    if (!g_safe || !il2cpp_light() || !g_img) return 0;
    cls = A.class_from_name(g_img, "Hassle.Entities", "Checkpoint");
    f = cls ? A.class_get_field_from_name(cls, fld) : NULL;
    if (!f) return 0;
    A.field_static_get_value(f, &obj);
    if (!obj || !hm_obj_ok(obj) || !air_obj_active(obj)) return 0;
    tr = air_obj(obj, "get_transform");
    if (!tr || !air_vec(tr, "get_position", out)) return 0;
    return !(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);   /* спрятанный маркер уезжает в ноль */
}
static void* air_minimap(void) {                                    /* UI.MiniMap (статический геттер, запасной путь — UI.m_Instance.m_MiniMap) */
    void *cls, *m, *exc = NULL, *r, *f, *inst = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    cls = A.class_from_name(g_img, "Hassle.Graphics.UserInterfaces", "UI");
    if (!cls) return NULL;
    m = find_method(cls, "get_MiniMap", 0);
    if (m) { r = A.runtime_invoke(m, NULL, NULL, &exc); if (!exc && r && hm_obj_ok(r)) return r; }
    f = A.class_get_field_from_name(cls, "m_Instance");
    if (f) {
        A.field_static_get_value(f, &inst);
        if (inst && hm_obj_ok(inst)) { r = ref_field(inst, "m_MiniMap"); if (r && hm_obj_ok(r)) return r; }
    }
    return NULL;
}
/* MiniMap.m_NavigationPoint — мировые координаты Unity; active: m_IsNavigationActive (-1 — поля нет) */
static int air_nav_point(void* mm, float* p, int* active) {
    void* k = A.object_get_class(mm); void* f = find_field(k, "m_NavigationPoint"); void* fa; const float* v;
    if (active) *active = -1;
    if (!f) return 0;
    v = (const float*)((char*)mm + A.field_get_offset(f));
    if (!hm_readable(v, 12) || !air_finite(v)) return 0;
    p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
    fa = find_field(k, "m_IsNavigationActive");
    if (fa && active) { const u8* bp = (const u8*)mm + A.field_get_offset(fa); if (hm_readable(bp, 1)) *active = *bp ? 1 : 0; }
    return 1;
}
/* текущая цель: 1 — маркер сервера, 2 — метка карты, 0 — нет. mode: 1 любая (маркер важнее), 2 только карта, 3 только маркер. exact: высота точная */
static int air_mark_target(int mode, float* out, int* exact) {
    float cp[3], np[3]; int act = -1;
    *exact = 1;
    if (mode != 2 && (air_chk_pos("Race", cp) || air_chk_pos("Simple", cp))) { out[0] = cp[0]; out[1] = cp[1]; out[2] = cp[2]; return 1; }
    if (mode != 3) {
        void* mm = air_minimap();
        if (mm && air_nav_point(mm, np, &act) && act != 0 && (np[0] != 0.0f || np[1] != 0.0f || np[2] != 0.0f)) {
            out[0] = np[0]; out[2] = np[2];
            if (np[1] > -50.0f && np[1] < 3000.0f && np[1] != 0.0f) out[1] = np[1] + 1.5f; else { out[1] = np[1]; *exact = 0; }
            return 2;
        }
    }
    return 0;
}
/* один шаг автолёта (главный поток, из тика). manual — игрок сам жмёт джойстик/кнопки: автолёт на паузе */
static void air_mark_step(const AirCtx* c, float dt, int manual) {
    long long nowMs = air_now_ns() / 1000000; float off[3], tg[3], d[3], dist, step, spd; char m[200]; Buf b;
    if (nowMs - g_air_mkT >= AIR_MARK_POLL_MS) {                    /* цель перечитываем раз в 150 мс */
        float np[3]; int ex = 1, kind;
        g_air_mkT = nowMs;
        kind = air_mark_target(g_air_mark == 2 ? 2 : g_air_mark == 3 ? 3 : 1, np, &ex);
        air_ex_watch(kind, np, nowMs);                              /* автошкола: старт экзамена → режим экзамена (курс, остановки у знаков STOP) */
        if (!kind) {
            if (g_air_mkHave) { g_air_mkHave = 0; g_air_mkArrived = 0; chat_say("/air mark: метка пропала — жду следующую"); }
        } else if (!g_air_mkHave || air_dist(np, g_air_mkPos) > 1.0f || kind != g_air_mkKind) {
            g_air_mkHave = 1; g_air_mkArrived = 0; g_air_mkKind = kind; g_air_mkExact = ex;
            g_air_mkPos[0] = np[0]; g_air_mkPos[1] = np[1]; g_air_mkPos[2] = np[2];
            b_init(&b, m, sizeof m); b_s(&b, kind == 1 ? "/air mark: лечу к маркеру сервера, " : "/air mark: лечу к метке карты, ");
            { float cur[3]; if (air_mypos(c, NULL, cur)) { b_fix(&b, air_dist(cur, np), 0); b_s(&b, " м"); } else b_s(&b, "расстояние неизвестно"); }
            chat_say(m);
            { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "air: mark: цель "); b_int(&x, kind); b_s(&x, " в "); air_b_xyz(&x, np); dlog(lg); }
        } else { g_air_mkPos[0] = np[0]; g_air_mkPos[1] = np[1]; g_air_mkPos[2] = np[2]; g_air_mkExact = ex; }
    }
    if (!g_air_mkHave || manual) return;
    off[0] = off[1] = off[2] = 0.0f;
    if (g_air_mode == 0) {                                          /* пешком hold — в системе transform движения, цель — в Entity.Transform */
        float mP[3], eP[3];
        if (!air_mypos(c, mP, eP)) return;
        off[0] = mP[0] - eP[0]; off[1] = mP[1] - eP[1]; off[2] = mP[2] - eP[2];
    }
    tg[0] = g_air_mkPos[0] + off[0]; tg[1] = g_air_mkExact ? g_air_mkPos[1] + off[1] : g_air_hold[1]; tg[2] = g_air_mkPos[2] + off[2];
    d[0] = tg[0] - g_air_hold[0]; d[1] = tg[1] - g_air_hold[1]; d[2] = tg[2] - g_air_hold[2];
    dist = (float)__builtin_sqrt((double)d[0] * d[0] + (double)d[1] * d[1] + (double)d[2] * d[2]);
    if (dist <= AIR_MARK_STOP) {
        if (!g_air_mkArrived) { g_air_mkArrived = 1; chat_say("/air mark: на месте — жду следующую метку (/air mark off — выключить, /air land — на землю)"); }
        return;
    }
    g_air_mkArrived = 0;
    spd = g_air_speed;
    if (g_air_exOn && g_air_mode == 1 && g_air_mkKind == 1 && !air_ex_drive(c->veh, g_air_hold, d, nowMs, dt, &spd)) return;   /* экзамен: стоим у знака STOP или разворачиваемся на месте */
    step = spd * dt; if (step > dist) step = dist;
    g_air_hold[0] += d[0] / dist * step; g_air_hold[1] += d[1] / dist * step; g_air_hold[2] += d[2] / dist * step;
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
static void air_state_init(const float* hold) {
    g_air_hold[0] = g_air_last[0] = hold[0]; g_air_hold[1] = g_air_last[1] = hold[1]; g_air_hold[2] = g_air_last[2] = hold[2];
    g_air_tPrev = air_now_ns(); g_air_jumps = 0; g_air_watchId = -1; g_air_watchT = 0; g_air_pending = 0;
    g_air_hOk = 0;                                                   /* новая машина/новый полёт — курс перечитаем с неё */
    g_air_pj = g_air_pc = 1; g_air_kSrc = -1;                        /* если кнопка уже зажата в момент включения — это не «новое нажатие» */
}
/* пешком: флаги + постановка в точку hold (система transform движения). 0 — не вышло, флаги возвращены */
static int air_begin_foot(const AirCtx* c, const float* hold) {
    air_flags(c, 1);
    if (!air_call_setpos(c->mv, hold, "движению")) { air_flags(c, 0); return 0; }
    air_zero_vel(c->mv);
    air_state_init(hold);
    g_air_mode = 0; g_air_veh = NULL;
    return 1;
}
/* за рулём: флаги Rigidbody машины + постановка в точку hold (система Entity.Transform машины). 0 — не вышло, флаги возвращены */
static int air_begin_car(const AirCtx* c, const float* hold) {
    void *rb = air_veh_rb(c->veh), *tr = air_veh_tr(c->veh);
    if (!rb || !tr) { dlog(!rb ? "air: у машины нет Rigidbody (m_Rigidbody)" : "air: у машины нет Transform"); return 0; }
    g_air_vrbSame = (air_obj(rb, "get_transform") == tr);
    g_air_veh = c->veh;
    air_vflags(c->veh, 1);
    if (!air_vset(c->veh, hold)) { air_vflags(c->veh, 0); g_air_veh = NULL; dlog("air: машину в точку поставить не вышло (Transform.set_position)"); return 0; }
    air_state_init(hold);
    g_air_mode = 1;
    return 1;
}
static int air_enter(float lift, const float* target /* NULL — подняться на lift над собой; иначе лететь в точку (система Entity.Transform) */) {
    AirCtx c; float mP[3], eP[3], hold[3]; int car;
    if (!air_ctx(&c, 0)) return 0;
    car = c.veh ? 1 : 0;
    if (car) {
        if (!c.drv) { chat_say("/air: вы пассажир — машиной правит водитель. Сядьте за руль или выйдите"); return 0; }
        if (!air_vpos(c.veh, eP)) { chat_say("/air: позиция машины не прочиталась — подробности в konstt.log"); dlog("air: нет Transform.position у машины"); return 0; }
    } else {
        if (!c.mv) { chat_say("/air: у вашего персонажа не найден компонент движения (подробности в konstt.log)"); dlog("air: нет Actor.m_Controller.Movement"); return 0; }
        if (!air_mypos(&c, mP, eP)) { chat_say("/air: ваша позиция не прочиталась — подробности в konstt.log"); return 0; }
    }
    mt_capture_ctx();                                                /* нужен главный поток — мы как раз в нём (обработчик команды) */
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { chat_say("/air: нет канала в главный поток — полёт не удержать (подробности в konstt.log)"); dlog("air: нет g_mt_ctx"); return 0; }
    if (car) {
        if (target) { hold[0] = target[0]; hold[1] = target[1]; hold[2] = target[2]; }
        else { hold[0] = eP[0]; hold[1] = eP[1] + lift; hold[2] = eP[2]; }
        if (!air_begin_car(&c, hold)) { chat_say("/air: не получилось поднять машину — подробности в konstt.log"); return 0; }
        g_air_y0 = eP[1];
    } else {
        if (target) { hold[0] = target[0] + (mP[0] - eP[0]); hold[1] = target[1] + (mP[1] - eP[1]); hold[2] = target[2] + (mP[2] - eP[2]); }
        else { hold[0] = mP[0]; hold[1] = mP[1] + lift; hold[2] = mP[2]; }
        if (!air_begin_foot(&c, hold)) { chat_say("/air: не получилось поставить персонажа в воздух — подробности в konstt.log"); return 0; }
        g_air_y0 = mP[1];
    }
    if (!g_air_thread) {
        unsigned long t = 0;
        if (pthread_create(&t, NULL, air_worker, NULL) != 0) {
            if (car) air_vrelease(0); else air_flags(&c, 0);
            chat_say("/air: не создался рабочий поток"); dlog("air: pthread_create не вышел"); return 0;
        }
        g_air_thread = 1;
    }
    __atomic_store_n(&g_air_on, 1, __ATOMIC_RELEASE);
    dlog(car ? "air: включён (в машине)" : "air: включён");
    return 1;
}
static void air_exit(const char* why, int land) {
    AirCtx c; float eP[3]; int doLand = land, car = (g_air_mode == 1);
    if (!__atomic_load_n(&g_air_on, __ATOMIC_ACQUIRE) && !g_air_saved && !g_air_vsaved) return;
    {
        float lim = car ? AIR_VEH_AUTOLAND : AIR_AUTOLAND;
        if (g_air_hold[1] - g_air_y0 > lim || g_air_y0 - g_air_hold[1] > 1.0f) doLand = 1;    /* высоко над точкой включения или ниже неё (спустились под землю) — ставим на поверхность */
    }
    __atomic_store_n(&g_air_on, 0, __ATOMIC_RELEASE);
    g_air_mark = 0; g_air_mkHave = 0; g_air_mkArrived = 0;
    air_ex_end(NULL);
    if (car) {                                                       /* машина: вернуть физику и (если надо) поставить на землю */
        air_vrelease(doLand);
        g_air_mode = 0;
        dlog2("air: выключен (машина): ", why);
        return;
    }
    if (air_ctx(&c, 1)) {
        air_flags(&c, 0);
        if (c.mv) air_zero_vel(c.mv);
        if (doLand && !c.veh && c.me && air_mypos(&c, NULL, eP)) {
            if (air_call_set_h(c.me, eP, 1, "Player (посадка)")) {}   /* Player.SetPosition(Vector3, findHeight) — тот же вызов, что «/tpl to X Z» */
        }
    } else { g_air_saved = 0; g_air_ctrl = NULL; g_air_fall = NULL; g_air_fallSaved = 0; }
    dlog2("air: выключен: ", why);
}

/* ───────────── тик (главный поток) ───────────── */
static void air_tick(void) {
    AirCtx c; float cur[3], mv3[3], h0[3], dt; long long now; int manual = 0;
    g_air_pending = 0;
    if (!__atomic_load_n(&g_air_on, __ATOMIC_ACQUIRE)) return;
    if (!air_ctx(&c, 1) || (!c.veh && !c.mv)) { air_exit("персонаж недоступен", 0); chat_say("/air: персонаж пропал (смерть/смена мира) — полёт выключен"); return; }
    if (g_air_mode == 0 && c.veh) {                                  /* сели в машину, пока летели пешком */
        float vp[3];
        if (!c.drv) { air_exit("вы сели пассажиром", 0); chat_say("/air: вы пассажир — полёт выключен (за рулём он работает)"); return; }
        air_flags(&c, 0);                                            /* пешие флаги снимаем, дальше держим машину */
        if (!air_vpos(c.veh, vp) || !air_begin_car(&c, vp)) { air_exit("не удалось перейти на машину", 0); chat_say("/air: не получилось перейти на машину — полёт выключен (подробности в konstt.log)"); return; }
        chat_say("/air: вы за рулём — теперь летит машина");
        return;
    }
    if (g_air_mode == 1 && (!c.veh || c.veh != g_air_veh)) {         /* вышли из машины (или пересели) в полёте */
        float hp[3];
        if (g_air_exOn) air_ex_end("машина пропала — экзамен закончился, режим экзамена выключен");
        air_vrelease(1);                                             /* брошенная машина иначе осталась бы висеть/упала бы с высоты — на землю */
        g_air_mode = 0;
        if (c.veh) {
            if (!c.drv || !air_vpos(c.veh, hp) || !air_begin_car(&c, hp)) { air_exit("пересели в машину, не удалось продолжить", 0); chat_say("/air: полёт выключен"); return; }
            chat_say("/air: вы пересели — теперь летит другая машина");
        } else {
            if (!c.mv || !air_mypos(&c, hp, NULL) || !air_begin_foot(&c, hp)) { air_exit("вышли из машины, персонаж недоступен", 0); chat_say("/air: вы вышли из машины — полёт выключен"); return; }
            chat_say("/air: вы вышли из машины — она поставлена на землю, вы продолжаете лететь пешком");
        }
        return;
    }
    if (g_air_mode == 0) {
        if (c.ctrl != g_air_ctrl) air_flags(&c, 1);                  /* персонаж пересоздан: флаги — на новые объекты */
        if (g_air_stand && g_air_fall && hm_obj_ok(g_air_fall) && air_get_bool(g_air_fall, "get_IsActive", 0)) air_stop_ability(g_air_fall);   /* Fall всё же стартовал (под вами пусто) — гасим */
        if (!air_mypos(&c, cur, NULL)) return;
    } else {
        void* rb;
        if (!air_vpos(c.veh, cur)) return;
        rb = air_veh_rb(c.veh);                                      /* игра (Entity.SetPosition по приказу сервера) могла вернуть машине обычную физику — снова выключаем */
        if (rb && air_get_bool(rb, "get_isKinematic", 1) == 0) { air_vflags(c.veh, 1); g_air_vfix++; dlog("air: игра вернула машине физику — выключил снова"); }
    }
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
    h0[0] = g_air_hold[0]; h0[1] = g_air_hold[1]; h0[2] = g_air_hold[2];   /* точка до движения этого кадра — по смещению считаем курс машины */
    if (g_air_keys) {                                                /* прыжок — вверх, присед — вниз (как клавиши высоты в airbrake) */
        int kj = 0, kc = 0, src = air_buttons(g_air_mode == 1 ? NULL : c.ctrl, &kj, &kc);   /* в машине Character.Keys не читаем: там другие клавиши */
        if (src != g_air_kSrc) {
            g_air_kSrc = src;
            dlog(src == 2 ? "air: кнопки ПРЫЖОК/ПРИСЕД читаю из экранных контролов (OnScreenControls)" : src == 1 ? "air: экранные контролы не найдены — кнопки из Character.Keys (в воздухе могут не работать)" : "air: кнопки ПРЫЖОК/ПРИСЕД не читаются ни из контролов, ни из Character.Keys");
        }
        if (src) {
            g_air_kj = kj; g_air_kc = kc;
            if (kj || kc) manual = 1;
            if (kj && !kc) { g_air_hold[1] += g_air_speed * dt; if (!g_air_pj) g_air_hold[1] += AIR_TAP_STEP; }
            else if (kc && !kj) { g_air_hold[1] -= g_air_speed * dt; if (!g_air_pc) g_air_hold[1] -= AIR_TAP_STEP; }
            g_air_pj = kj; g_air_pc = kc;
        }
    }
    if (air_move_vec(&c, mv3)) {
        if (g_air_rawIn[0] * g_air_rawIn[0] + g_air_rawIn[1] * g_air_rawIn[1] > 0.04f) manual = 1;    /* игрок сам держит джойстик/газ — автолёт к метке на паузе */
        if (g_air_mode == 1 && g_air_vsp) {                          /* ручник зажат: газ — вверх, тормоз — вниз (только по высоте) */
            g_air_hold[1] += g_air_rawIn[1] * g_air_speed * dt;
        } else {
            g_air_hold[0] += mv3[0] * g_air_speed * dt;
            g_air_hold[1] += mv3[1] * g_air_speed * dt;
            g_air_hold[2] += mv3[2] * g_air_speed * dt;
        }
    }
    if (g_air_mark) air_mark_step(&c, dt, manual);
    if (g_air_mode == 1) {
        if (g_air_exOn) { if (g_air_hOk) air_vyaw(c.veh, g_air_hx, g_air_hz); }          /* экзамен: курс машины держим каждый кадр */
        else if (g_air_face) {                                                           /* обычный полёт: нос — по направлению движения */
            float dd[3]; dd[0] = g_air_hold[0] - h0[0]; dd[1] = 0.0f; dd[2] = g_air_hold[2] - h0[2];
            air_face_step(c.veh, dd, dt, g_air_rawIn[1] >= -0.2f);
        }
    }
    if (g_air_mode == 1) air_vset(c.veh, g_air_hold);
    else if (air_call_setpos(c.mv, g_air_hold, "движению")) air_zero_vel(c.mv);
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
    chat_usage("/air — полёт вкл/выкл (и пешком, и за рулём — тогда летит машина). Джойстик — лететь, ПРЫЖОК (держать) — вверх, ПРИСЕД (двойной тап по джойстику, второй тап держать) — вниз;  /air speed N;  /air flat|3d;  /air land — на землю");
    chat_usage("В машине: W/газ — вперёд, S/тормоз — назад, руль (A/D или стик) — вбок, всё относительно камеры. Вверх/вниз: зажмите РУЧНИК (пробел) — тогда газ летит вверх, тормоз вниз; или взгляд камерой в 3d, или /air up|down N");
    chat_usage("Вышли из машины в полёте — она встанет на землю, вы продолжите лететь пешком");
    chat_usage("/air mark — автолёт к красной метке (маркер сервера или метка карты); появилась/сменилась — летит к новой. /air mark gps|cp — только карта|только маркер, /air mark off — выключить");
    chat_usage("/air exam [on|off|auto] — режим автошколы для /air mark cp: курс машины по дороге и остановки у знаков STOP (авто — включается на старте экзамена);  /air exam speed N — скорость экзамена, м/с (3…30, по умолчанию 14)");
    chat_usage("/air face [on|off] — в машине нос по направлению полёта (по умолчанию ВКЛ; выкл — машина летит боком с прежним курсом)");
    chat_usage("/air stand — вкл/выкл анимацию «как на ровном» (без левитации);  /air npc — ближайшие NPC с ID;  /air to ID — к NPC (3 м над ним);  /air watch — писать ID ближайшего NPC;  /air status;  /air input");
}
static void air_status(void) {
    char m[300]; Buf b; AirCtx c; float p[3];
    b_init(&b, m, sizeof m);
    b_s(&b, "/air: "); b_s(&b, g_air_on ? "ВКЛ" : "выкл");
    b_s(&b, ", скорость "); b_fix(&b, g_air_speed, 1); b_s(&b, " м/с, режим "); b_s(&b, g_air_look3d ? "3d (куда смотрит камера)" : "flat (горизонтально)");
    b_s(&b, ", летим: "); b_s(&b, g_air_on ? (g_air_mode == 1 ? "МАШИНОЙ" : "пешком") : "—");
    b_s(&b, ", нос по курсу (/air face): "); b_s(&b, g_air_face ? "да" : "нет");
    b_s(&b, ", слежка за NPC: "); b_s(&b, g_air_watch ? "да" : "нет");
    b_s(&b, ", как на ровном: "); b_s(&b, g_air_stand ? (g_air_fall ? "да (Fall найден)" : "да") : "нет");
    chat_say(m);
    if (g_air_on && g_air_mode == 1) {
        void* rb = g_air_veh ? air_veh_rb(g_air_veh) : NULL;
        b_init(&b, m, sizeof m); b_s(&b, "  машина: Rigidbody "); b_s(&b, rb ? "найден" : "НЕ НАЙДЕН"); b_s(&b, ", kinematic "); b_int(&b, rb ? air_get_bool(rb, "get_isKinematic", -1) : -1);
        b_s(&b, ", коллизии "); b_int(&b, rb ? air_get_bool(rb, "get_detectCollisions", -1) : -1); b_s(&b, ", возвратов физики игрой: "); b_int(&b, g_air_vfix);
        chat_say(m);
        b_init(&b, m, sizeof m); b_s(&b, "  кнопки HUD машины W S A D пробел: ");
        if (g_air_vkFound == 1) { b_int(&b, g_air_vkeys.w); b_ch(&b, ' '); b_int(&b, g_air_vkeys.s); b_ch(&b, ' '); b_int(&b, g_air_vkeys.a); b_ch(&b, ' '); b_int(&b, g_air_vkeys.d); b_ch(&b, ' '); b_int(&b, g_air_vkeys.sp); b_s(&b, " — зажмите газ: W станет 1"); }
        else b_s(&b, "НЕ НАЙДЕНЫ (список экранных контролов — в konstt.log, строки «экранный контрол»)");
        chat_say(m);
        b_init(&b, m, sizeof m); b_s(&b, "  физика машины, руль/газ/тормоз: ");
        if (g_air_vinOk == 1) { b_fix(&b, g_air_vin[0], 2); b_ch(&b, ' '); b_fix(&b, g_air_vin[1], 2); b_ch(&b, ' '); b_fix(&b, g_air_vin[2], 2); }
        else b_s(&b, "не читается (нет WheeledVehiclePhysics — вертолёт/самолёт/лодка? тогда кнопки или /air up|down|to)");
        chat_say(m);
    }
    if (g_air_on && air_ctx(&c, 1) && (g_air_mode == 1 ? air_vpos(c.veh, p) : air_mypos(&c, p, NULL))) {
        b_init(&b, m, sizeof m); b_s(&b, "  держу "); air_b_xyz(&b, g_air_hold); b_s(&b, ", сейчас "); air_b_xyz(&b, p);
        b_s(&b, ", высота над точкой включения "); b_fix(&b, g_air_hold[1] - g_air_y0, 1); b_s(&b, " м, перестановок игрой: "); b_int(&b, g_air_jumps);
        chat_say(m);
    }
    b_init(&b, m, sizeof m); b_s(&b, "  автолёт к метке: "); b_s(&b, g_air_mark == 0 ? "выкл" : g_air_mark == 2 ? "вкл (только метка карты)" : g_air_mark == 3 ? "вкл (только маркер сервера)" : "вкл (любая метка)");
    if (g_air_mark) { b_s(&b, g_air_mkHave ? (g_air_mkKind == 1 ? ", цель: маркер сервера " : ", цель: метка карты ") : ", цели нет"); if (g_air_mkHave) { air_b_xyz(&b, g_air_mkPos); b_s(&b, g_air_mkArrived ? " (на месте)" : ""); } }
    chat_say(m);
    b_init(&b, m, sizeof m); b_s(&b, "  автошкола (/air exam): "); b_s(&b, g_air_exMode == 1 ? "включена вручную" : g_air_exMode == 2 ? "выключена" : "авто");
    b_s(&b, g_air_exOn ? ", СЕЙЧАС экзамен" : ", сейчас не экзамен"); b_s(&b, ", скорость "); b_fix(&b, g_air_exSpeed, 1); b_s(&b, " м/с, остановок у знаков "); b_int(&b, g_air_exStops);
    if (g_air_exSign >= 0) { b_s(&b, ", стою у знака №"); b_int(&b, g_air_exSign + 1); }
    b_s(&b, "; чат: "); b_s(&b, g_airc_state == 1 ? "перехват вкл" : g_airc_state < 0 ? "перехват не вышел" : "перехват выкл");
    if (g_airc_state == 1) { b_s(&b, ", строк "); b_int(&b, g_airc_seen); b_s(&b, ", остановок "); b_int(&b, g_air_chStops); if (g_air_chHold) b_s(&b, ", СТОЮ по чату"); }
    if (g_air_exOn && g_air_hOk) { b_s(&b, ", курс (x z) "); b_fix(&b, g_air_hx, 2); b_ch(&b, ' '); b_fix(&b, g_air_hz, 2); }
    chat_say(m);
    b_init(&b, m, sizeof m); b_s(&b, "  ввод ("); b_s(&b, g_air_inSrc ? "MovementDirection" : "m_MovementInput"); b_s(&b, "): ");
    b_fix(&b, g_air_rawIn[0], 2); b_ch(&b, ' '); b_fix(&b, g_air_rawIn[1], 2); b_ch(&b, ' '); b_fix(&b, g_air_rawIn[2], 2);
    b_s(&b, " — подвигайте джойстик: числа должны меняться (/air input — сменить источник)");
    chat_say(m);
    b_init(&b, m, sizeof m); b_s(&b, "  кнопки вверх/вниз: "); b_s(&b, g_air_keys ? "вкл" : "выкл");
    b_s(&b, ", источник: "); b_s(&b, g_air_kSrc == 2 ? "экранные контролы" : g_air_kSrc == 1 ? "Character.Keys (запасной)" : g_air_kSrc == 0 ? "НЕ ЧИТАЕТСЯ" : "не пробовали (включите полёт)");
    b_s(&b, ", прыжок "); b_s(&b, g_air_kj ? "НАЖАТ" : "нет");
    b_s(&b, ", присед "); b_s(&b, g_air_kc ? "НАЖАТ" : "нет"); b_s(&b, " — зажмите прыжок: должно стать «НАЖАТ»");
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
        if (nt < 2 || !air_num(tok[1], &v)) { b_init(&b, m, sizeof m); b_s(&b, "/air: скорость сейчас "); b_fix(&b, g_air_speed, 1); b_s(&b, " м/с. Задать: /air speed 12 (до 1000)"); chat_say(m); return; }
        if (v < 0.5f) v = 0.5f;
        if (v > AIR_MAXSPEED) v = AIR_MAXSPEED;
        g_air_speed = v;
        b_init(&b, m, sizeof m); b_s(&b, "/air: скорость "); b_fix(&b, v, 1); b_s(&b, " м/с"); chat_say(m);
        return;
    }
    if (seq(tok[0], "mark") || seq(tok[0], "метка") || seq(tok[0], "марк") || seq(tok[0], "gps") || seq(tok[0], "гпс")) {
        int mode = 1;
        if (nt >= 2 && (seq(tok[1], "off") || seq(tok[1], "выкл") || seq(tok[1], "0"))) {
            g_air_mark = 0; g_air_mkHave = 0; g_air_mkArrived = 0; chat_say("/air mark: автолёт к метке выключен (полёт остался)"); return;
        }
        if (seq(tok[0], "gps") || seq(tok[0], "гпс") || (nt >= 2 && (seq(tok[1], "gps") || seq(tok[1], "гпс") || seq(tok[1], "map") || seq(tok[1], "карта")))) mode = 2;
        else if (nt >= 2 && (seq(tok[1], "cp") || seq(tok[1], "маркер") || seq(tok[1], "чекпоинт") || seq(tok[1], "server"))) mode = 3;
        if (g_air_mark && nt == 1 && !seq(tok[0], "gps") && !seq(tok[0], "гпс")) {   /* голое «/air mark» при включённом — выключает */
            g_air_mark = 0; g_air_mkHave = 0; g_air_mkArrived = 0; chat_say("/air mark: автолёт к метке выключен (полёт остался)"); return;
        }
        if (!g_air_on && !air_enter(AIR_LIFT, NULL)) return;         /* полёт выключен — включаем */
        g_air_mark = mode; g_air_mkHave = 0; g_air_mkArrived = 0; g_air_mkT = 0;
        chat_say(mode == 2 ? "/air mark: слежу за меткой карты (/gps). Поставите/смените — полечу к ней. Джойстик — перехват; /air mark off — выключить"
               : mode == 3 ? "/air mark: слежу за маркером сервера. Появится/сменится — полечу к нему. Джойстик — перехват; /air mark off — выключить"
               : "/air mark: слежу за красной меткой (маркер сервера, иначе метка карты). Появится/сменится — полечу к ней. Джойстик — перехват; /air mark off — выключить");
        return;
    }
    if (seq(tok[0], "exam") || seq(tok[0], "экзамен") || seq(tok[0], "автошкола")) {
        if (nt >= 2 && (seq(tok[1], "on") || seq(tok[1], "вкл"))) {
            g_air_exMode = 1; chat_say("/air exam: режим экзамена включён (работает, когда летите на машине к маркеру сервера: /air mark cp). Выключить: /air exam off");
        } else if (nt >= 2 && (seq(tok[1], "off") || seq(tok[1], "выкл") || seq(tok[1], "0"))) {
            g_air_exMode = 2; air_ex_end(NULL); chat_say("/air exam: выключен — автолёт к метке обычный (прямо, без остановок у знаков). Вернуть: /air exam auto");
        } else if (nt >= 2 && (seq(tok[1], "auto") || seq(tok[1], "авто"))) {
            g_air_exMode = 0; chat_say("/air exam: авто — включится сам, когда маркер сервера окажется стартом экзамена в автошколе");
        } else if (nt >= 2 && (seq(tok[1], "chat") || seq(tok[1], "чат"))) {
            if (nt >= 3 && (seq(tok[2], "on") || seq(tok[2], "вкл"))) {
                if (airc_install()) chat_say("/air exam chat: перехват чата включён — встану по «Здесь аккуратней», поеду после «Отлично». Снять: /air exam chat off");
                else { b_init(&b, m, sizeof m); b_s(&b, "/air exam chat: не вышло — "); b_s(&b, g_airc_why); chat_say(m); }
            } else if (nt >= 3 && (seq(tok[2], "off") || seq(tok[2], "выкл") || seq(tok[2], "0"))) {
                airc_unhook(); g_air_chHold = 0;
                chat_say(g_airc_state == 1 ? "/air exam chat: снять не удалось (смотрите HLOG/konstt.log)" : "/air exam chat: перехват чата снят — дальше только таблица знаков");
            } else {
                b_init(&b, m, sizeof m); b_s(&b, "/air exam chat: "); b_s(&b, g_airc_state == 1 ? "включён" : g_airc_state < 0 ? "не вышло" : "выключен");
                b_s(&b, ", строк инструктора "); b_int(&b, g_airc_seen); b_s(&b, ", остановок по чату "); b_int(&b, g_air_chStops); b_s(&b, ". /air exam chat on|off"); chat_say(m);
            }
        } else if (nt >= 2 && (seq(tok[1], "speed") || seq(tok[1], "скорость"))) {
            if (nt < 3 || !air_num(tok[2], &v)) { b_init(&b, m, sizeof m); b_s(&b, "/air exam: скорость сейчас "); b_fix(&b, g_air_exSpeed, 1); b_s(&b, " м/с. Задать: /air exam speed 14 (3…30)"); chat_say(m); return; }
            if (v < 3.0f) v = 3.0f;
            if (v > AIR_EX_MAXSPEED) v = AIR_EX_MAXSPEED;
            g_air_exSpeed = v;
            b_init(&b, m, sizeof m); b_s(&b, "/air exam: скорость экзамена "); b_fix(&b, v, 1); b_s(&b, " м/с (≈ "); b_fix(&b, v * 3.6f, 0); b_s(&b, " км/ч)"); chat_say(m);
        } else {
            b_init(&b, m, sizeof m); b_s(&b, "/air exam: "); b_s(&b, g_air_exMode == 1 ? "включён вручную" : g_air_exMode == 2 ? "выключен" : "авто"); b_s(&b, g_air_exOn ? ", сейчас экзамен" : ", сейчас не экзамен");
            b_s(&b, ". /air exam on|off|auto, /air exam speed N, /air exam chat on|off"); chat_say(m);
        }
        return;
    }
    if (seq(tok[0], "flat") || seq(tok[0], "ровно")) { g_air_look3d = 0; chat_say("/air: режим flat — по джойстику летите горизонтально, высота — /air up|down"); return; }
    if (seq(tok[0], "3d") || seq(tok[0], "look") || seq(tok[0], "камера")) { g_air_look3d = 1; chat_say("/air: режим 3d — вперёд летите туда, куда смотрит камера"); return; }
    if (seq(tok[0], "keys") || seq(tok[0], "кнопки")) {
        g_air_keys = !g_air_keys;
        chat_say(g_air_keys ? "/air: кнопки включены — ПРЫЖОК (держать) вверх, ПРИСЕД (двойной тап по джойстику и держать) вниз" : "/air: кнопки вверх/вниз выключены (высота — камерой в 3d или /air up|down)");
        return;
    }
    if (seq(tok[0], "face") || seq(tok[0], "нос") || seq(tok[0], "курс")) {
        if (nt >= 2 && (seq(tok[1], "off") || seq(tok[1], "выкл") || seq(tok[1], "0"))) g_air_face = 0;
        else if (nt >= 2 && (seq(tok[1], "on") || seq(tok[1], "вкл") || seq(tok[1], "1"))) g_air_face = 1;
        else g_air_face = !g_air_face;
        g_air_hOk = 0;
        chat_say(g_air_face ? "/air face: вкл — в машине нос поворачивается по направлению полёта (к метке — носом вперёд)" : "/air face: выкл — машина летит с прежним курсом (может лететь боком)");
        return;
    }
    if (seq(tok[0], "stand") || seq(tok[0], "ровно2") || seq(tok[0], "анимация")) {
        AirCtx c;
        g_air_stand = !g_air_stand;
        if (g_air_on && g_air_mode == 0 && air_ctx(&c, 1)) air_stand(&c, g_air_stand);
        chat_say(g_air_stand ? "/air: анимация «как на ровном» включена (в полёте идёте/стоите, а не падаете с раскинутыми руками)" : "/air: анимация «как на ровном» выключена — в полёте обычная анимация падения");
        return;
    }
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
            if (g_air_on && g_air_mode == 1) {                           /* машиной: точка в системе Entity.Transform — та же, что у NPC */
                AirCtx c;
                if (air_ctx(&c, 0) && c.veh && c.veh == g_air_veh) {
                    g_air_hold[0] = tp[0]; g_air_hold[1] = tp[1]; g_air_hold[2] = tp[2];
                    air_vset(c.veh, g_air_hold);
                    g_air_last[0] = g_air_hold[0]; g_air_last[1] = g_air_hold[1]; g_air_last[2] = g_air_hold[2];
                } else return;
            } else if (g_air_on) {
                AirCtx c; float mP[3], eP[3];
                if (air_ctx(&c, 0) && air_mypos(&c, mP, eP)) {
                    g_air_hold[0] = tp[0] + (mP[0] - eP[0]); g_air_hold[1] = tp[1] + (mP[1] - eP[1]); g_air_hold[2] = tp[2] + (mP[2] - eP[2]);
                    air_call_setpos(c.mv, g_air_hold, "движению");
                    g_air_last[0] = g_air_hold[0]; g_air_last[1] = g_air_hold[1]; g_air_last[2] = g_air_hold[2];
                } else return;
            } else if (!air_enter(0.0f, tp)) return;
            b_init(&b, m, sizeof m); b_s(&b, g_air_mode == 1 ? "/air: машина над NPC " : "/air: над NPC "); b_int(&b, id); b_s(&b, " (3 м выше него): "); air_b_xyz(&b, tp);
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
