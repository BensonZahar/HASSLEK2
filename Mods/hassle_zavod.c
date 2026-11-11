/*
 * hassle_zavod.c — необязательный мод «/zavod»: ТЕСТОВЫЙ автоцикл завода на тестовых NPC / тестовом сервере (по образцу hassle_shaxta.c).
 *
 * Цикл: телепорт туда, где берут предмет → телепорт на СВОБОДНЫЙ станок → быстрое завершение работы (как в Code2.js) → телепорт на склад сдать → по кругу.
 * Только на вашем клиенте, сервер ничего не знает. Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_zavod.c и пересоберите бандл.
 * С /shaxta не связан (свои имена, своя кнопка меню) — можно держать оба сразу.
 *
 * Команды в чате:
 *   /zavod                 меню на экране (кнопка-«цех» справа + панель): «Автозавод», «Быстрый Turner», режим, счётчик. Работает на мобилке
 *   /zavod on | off        включить / выключить автоцикл (то же, что переключатель в меню)
 *   /zavod mode 0|1        режим: 0 — «Завод» (временная работа: металл → рабочее место → склад), 1 — «Цех тюрьмы» (бревно → токарный станок → склад фигурок).
 *                          Слова тоже можно: завод | цех | тюрьма
 *   /zavod bind take|put   запомнить ТЕКУЩЕЕ место как точку «взять» / «сдать» (приоритет над всем остальным, до перезапуска игры)
 *   /zavod bind mach       добавить текущее место как станок (до 8); /zavod bind clear — сбросить всё запомненное
 *   /zavod list            ближайшие 3D-подписи (текст + расстояние): по ним видно, как на вашем сервере называются склады и станки (ключевые слова — ZV_KW_* ниже)
 *   /zavod status          что мод сейчас видит: режим, откуда взяты точки, сколько станков, свободны ли
 *   /zavod fz [on|off]     ручная заморозка персонажа (автоцикл замораживает сам и отпускает при остановке)
 *   /zavod help
 *
 * Откуда мод берёт точки (по порядку): 1) /zavod bind  2) 3D-подпись сервера («Склад брёвен», «Склад фигурок», «Токарный станок №N / Статус: Свободен|Занят»,
 *   «Рабочее место №N / Свободно|Занято») — по тексту, поэтому правка расстановки на сервере ничего не ломает  3) встроенные координаты из серверного Pawn-кода
 *   (user_13: factory_take_metall_pos, factory_desk, factory_put_zone; PRISON_TURNER_*). Подписи сервер стримит только рядом (у «Рабочего места» радиус 5 м), поэтому
 *   у станков вне зоны стрима статус «неизвестно», и мод идёт по ближайшему из не помеченных занятыми; если станок не принял — берёт следующий.
 *   Координаты в Unity: SAMP (x, y, z) → Unity (x, z, y); Transform игрока на 1 м ниже серверной позиции (в пакет уходит Transform.y + 1) — как у /tpl pick и /shaxta.
 *
 * Быстрое завершение работы — перенесено из Code2.js (модуль ЗАВОД): хук window.openInterface. На Turner: до открытия глушится блокировка курсора и прятание HUD/чата (движение
 *   не пропадает), после открытия окно прячется через opacity, Turner_OnPlayerEnd уходит на следующем тике (~0 мс), страховочное closeInterface через 200 мс. Меню взаимодействий
 *   ловится тем же хуком: пункт нужной фазы (в т.ч. тип 399 «Начать производство») нажимается через ~80 мс после открытия — Interactions.onClick(индекс), иначе OnInteractionsClick.
 *   Включается переключателем «Быстрый Turner» (по умолчанию вкл), работает и без автоцикла. Не включайте одновременно /zon из Code2.js. Между кликами меню — не меньше 1 с:
 *   так считает серверный кулдаун interactions.pwn (INTERACTIONS_COLDDOWN_CLICK, целые секунды). Точка «взять»: метка сервера (/tpl gps go), если нет /zavod bind take.
 *   ВАЖНО: в серверном Pawn-коде (user_13) у «Завода» (TEMP_JOB_FACTORY) продукт делает серверный ТАЙМЕР 18 с (CreateFactoryProd) — его клиент ускорить не может;
 *   быстрым бывает только Turner (цех тюрьмы там и в новой версии «Завода», где есть «Начать производство»).
 *   Минимальный круг «Завода» на сервере — TJ_RACE_TIME = 3 с (между сдачей и взятием металла, иначе AntiCheat AC_WORK_BOT): цикл держит паузу PACE (3.3 с).
 *
 * Замороженный персонаж: перед каждым телепортом цикл замораживает персонажа штатным Entity.ToggleFreeze("zavod", true, false) — как /shaxta — иначе капсула у точки
 *   проваливается; при остановке цикла персонаж ставится на землю под точкой и отпускается. Цикл сам выключается, если вы тронули джойстик / кнопку / клавишу.
 *
 * Меню внедряется JS через WebViewSystem.ExecuteJavaScript (APK пересобирать не нужно). Нажатия меню уходят как «/zavod q …» через window.sendChatInput (q — «тихо»),
 *   состояние мод присылает обратно тем же ExecuteJavaScript. Если страница перезагрузилась (реконнект) — введите /zavod ещё раз. Текст меню — между метками
 *   ZV_MENU_BEGIN/END; править tools/zavod_menu.js и запускать tools/embed_zavod_menu.py. (v2: хук чата через Hud.$refs.chat.add; фигурка «в руках» только по ответу сервера.)
 *
 * НЕ ПРОВЕРЕНО НА УСТРОЙСТВЕ (собрано и проверено компилятором). Если что-то не так — смотрите HLOG/konstt.log (строки «zavod:») и пришлите.
 */
#pragma once
#include "hassle_core.c"

#define ZV_MAXLB 24                     /* сколько подписей станков запоминаем за скан */
#define ZV_MAXM 24                      /* станков в списке для меню */
#define ZV_MAXB 8                       /* запомненных станков (/zavod bind mach) */
#define ZV_LAND_UP 1.0f                 /* при отпускании заморозки: ищем пол вниз от точки на столько выше (как у /shaxta) */
#define ZV_NEAR 2.5f                    /* подпись станка в пределах этих метров (по горизонтали) от вас — «станок рядом» (nb) */
#define ZV_OVERLAY_R 2.0f               /* подпись «Свободно/Занято» относится к встроенному станку, если стоит в этих метрах от него */
#define ZV_FZ_TYPE "zavod"              /* имя типа заморозки в Entity.m_FreezeTypes */

/* Ключевые слова подписей сервера (подстрока, регистр важен). «Склад бр» — чтобы не зависеть от е/ё. Меняйте под свой сервер (смотрите /zavod list). */
#define ZV_KW_TAKE "Склад бр"           /* цех тюрьмы: «Склад брёвен» */
#define ZV_KW_PUT "Склад фигурок"       /* цех тюрьмы: «Склад фигурок» */
#define ZV_KW_MACH0 "Рабочее место"     /* завод: «Рабочее место №N» + «Свободно»/«Занято» */
#define ZV_KW_MACH1 "Токарный станок"   /* цех тюрьмы: «Токарный станок №N» + «Статус: Свободен»/«Занят» */

/* ── встроенные координаты (из серверного Pawn-кода user_13) ──
   SAMP (x, y, z) → Transform игрока в Unity (x, z - d, y): d = 1.0 — игрок стоит на серверной точке (в пакет уходит Transform.y + 1);
   у пикапа металла d = 1.4 (пикап создан на 0.4 ниже серверной точки, как «/tpl pick»: Transform на 1 м ниже точки пикапа). */
#define ZV_T(x, y, z, d) { (float)(x), (float)((z) - (d)), (float)(y) }
/* «Завод» (factory_take_metall_pos / factory_desk / factory_put_zone, gamemodes/new.pwn) */
static const float g_zv_f0_take[4][3] = {
    ZV_T(-2934.938232, -1171.546265, 1000.997803, 1.4), ZV_T(-2925.985596, -1171.346191, 1000.997803, 1.4),
    ZV_T(-2925.736572, -1204.637329, 1000.997803, 1.4), ZV_T(-2934.308105, -1204.633057, 1000.997803, 1.4)
};
static const float g_zv_f0_put[3] = ZV_T(-2893.060000, -1179.900000, 1000.997803, 1.0);   /* центр factory_put_zone */
static const float g_zv_f0_desk[18][3] = {
    ZV_T(-2937.900879, -1193.578369, 1001.052490, 1.0), ZV_T(-2930.924805, -1193.578247, 1001.052490, 1.0), ZV_T(-2923.886230, -1193.584106, 1001.052490, 1.0),
    ZV_T(-2917.043701, -1193.580200, 1001.052490, 1.0), ZV_T(-2910.961426, -1193.579224, 1001.052490, 1.0), ZV_T(-2903.857666, -1193.579590, 1001.052490, 1.0),
    ZV_T(-2897.634277, -1193.578247, 1001.052490, 1.0), ZV_T(-2890.964600, -1193.578125, 1001.052490, 1.0), ZV_T(-2883.352783, -1193.578125, 1001.052490, 1.0),
    ZV_T(-2883.521484, -1188.647949, 1000.997803, 1.0), ZV_T(-2890.507568, -1188.648315, 1000.997803, 1.0), ZV_T(-2897.538330, -1188.647583, 1001.004089, 1.0),
    ZV_T(-2904.463135, -1188.642822, 1000.997803, 1.0), ZV_T(-2910.676025, -1188.644897, 1000.997803, 1.0), ZV_T(-2917.568359, -1188.647217, 1000.997803, 1.0),
    ZV_T(-2923.545654, -1188.604370, 1000.997803, 1.0), ZV_T(-2930.464600, -1188.647217, 1000.997803, 1.0), ZV_T(-2938.051758, -1188.526367, 1000.997803, 1.0)
};
/* «Цех тюрьмы» (PRISON_TURNER_STORAGE_LOG_POS / _FIGUR_POS / g_prison_turner_machine_info, include/system/prison/jobs/turner.pwn) */
static const float g_zv_f1_take[3] = ZV_T(-1680.867554, -2748.788818, 1001.395935, 1.0);
static const float g_zv_f1_put[3] = ZV_T(-1699.583374, -2749.176758, 1001.395935, 1.0);
static const float g_zv_f1_mach[5][3] = {
    ZV_T(-1681.178589, -2761.033447, 1001.403748, 1.0), ZV_T(-1694.441650, -2760.176514, 1001.401001, 1.0), ZV_T(-1694.648560, -2748.269043, 1001.403748, 1.0),
    ZV_T(-1687.295288, -2747.760498, 1001.403748, 1.0), ZV_T(-1687.381592, -2753.534424, 1001.403748, 1.0)
};

typedef struct { float p[3]; int st; } ZvM;          /* станок: Transform-цель + статус 0 неизвестно / 1 свободен / 2 занят */
typedef struct { float p[3]; int st; } ZvLb;         /* подпись станка: мировая точка подписи + статус */

static int g_zv_mode;                                /* 0 завод, 1 цех тюрьмы */
static float g_zv_ref[3];                            /* ваш Transform */
static ZvLb g_zv_lm[ZV_MAXLB]; static int g_zv_nlm; /* подписи станков, что видит клиент */
static float g_zv_lbTk[3], g_zv_lbPt[3], g_zv_lbTkD, g_zv_lbPtD; static int g_zv_hasLbTk, g_zv_hasLbPt;
static float g_zv_bTk[3], g_zv_bPt[3], g_zv_bM[ZV_MAXB][3]; static int g_zv_hasBTk, g_zv_hasBPt, g_zv_nbM;   /* /zavod bind */
static float g_zv_tk[3], g_zv_pt[3]; static int g_zv_hasTk, g_zv_hasPt, g_zv_srcTk, g_zv_srcPt, g_zv_srcM;    /* итог: источник 0 нет, 1 bind, 2 подпись, 3 встроено */
static ZvM g_zv_m[ZV_MAXM]; static int g_zv_nm;
static int g_zv_nb, g_zv_mark, g_zv_poolN = -1;
static int g_zv_modId = -1, g_zv_errs, g_zv_q, g_zv_uiOk, g_zv_fz, g_zv_fzErr, g_zv_layoutOk, g_zv_oPos, g_zv_oTxt;
static char g_zv_lastJs[3800];

/* /zavod list: ближайшие подписи */
#define ZV_LISTN 8
static char g_zv_ltxt[ZV_LISTN][60]; static float g_zv_ld[ZV_LISTN]; static int g_zv_nl;

static float zv_dist(const float* a, const float* b) { double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz); }
static float zv_hdist(const float* a, const float* b) { double dx = a[0] - b[0], dz = a[2] - b[2]; return (float)__builtin_sqrt(dx * dx + dz * dz); }
static int zv_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
static void zv_cp3(float* d, const float* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
static int zv_has(const char* h, const char* n) {
    int i, k;
    for (i = 0; h[i]; i++) { for (k = 0; n[k] && h[i + k] == n[k]; k++) {} if (!n[k]) return 1; }
    return 0;
}
static void zv_err(const char* what, void* exc) {
    char et[200];
    if (g_zv_errs >= 6) return;
    g_zv_errs++;
    exc_text(exc, et, sizeof et);
    dlog2(what, et);
}
static void zv_say(const char* s) { if (!g_zv_q) chat_say(s); }      /* информационные; ошибки всегда идут в чат через chat_say */

/* ───────────── доступ к игре (как в hassle_shaxta.c) ───────────── */
static int zv_js_run(const char* js) {                              /* WebViewSystem.ExecuteJavaScript(js) — только из главного потока */
    void *k, *m, *exc = NULL, *args[1];
    if (!g_safe || !il2cpp_light() || !g_img) return 0;
    ensure_attached();
    k = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    m = k ? find_method(k, "ExecuteJavaScript", 1) : NULL;
    if (!m) { if (g_zv_errs < 6) { g_zv_errs++; dlog("zavod: нет WebViewSystem.ExecuteJavaScript(string)"); } return 0; }
    args[0] = A.string_new(js);
    if (!args[0]) return 0;
    A.runtime_invoke(m, NULL, args, &exc);
    if (exc) { zv_err("zavod: ExecuteJavaScript бросил исключение: ", exc); return 0; }
    return 1;
}
static void* zv_pool(const char* field) {                           /* Pools.<field> */
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}
static void* zv_obj(void* obj, const char* name) {                  /* метод без аргументов, вернувший ссылочный объект */
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int zv_vec(void* obj, const char* name, float* out) {        /* метод без аргументов, вернувший Vector3 (в боксе: 3 float после заголовка) */
    void *m, *exc = NULL, *r; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!zv_finite(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
static int zv_myref(float* out) {                                   /* ваша позиция (Transform) */
    void* me = local_player();
    void* tr;
    if (!me || !hm_obj_ok(me)) return 0;
    tr = zv_obj(me, "get_Transform");
    if (!tr) tr = zv_obj(me, "get_transform");
    if (tr && zv_vec(tr, "get_position", out)) return 1;
    return read_pos(me, out);
}
static int zv_scan_pool(const char* field, item_cb cb) {            /* значения словаря пула (BasePool<T>.m_Items); -1 — не прочиталось, иначе число записей */
    void *pool = zv_pool(field), *dict, *arr; int total;
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (!arr) return -1;
    total = int_field(dict, "_count", "count", NULL, -1);
    walk_struct_array(arr, total, "value", cb, NULL);
    return total;
}

/* ───────────── заморозка персонажа (см. шапку) ───────────── */
static int zv_is_frozen(void) {
    void *me, *m, *exc = NULL, *r;
    if (!g_safe || !il2cpp_light()) return 0;
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) return 0;
    m = find_method(A.object_get_class(me), "get_IsFreezed", 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, me, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 1)) return 0;
    return *((u8*)r + HDR) ? 1 : 0;
}
static int zv_set_pos(void* me, const float* pos, u8 findH) {       /* Player.SetPosition(Vector3, findHeight) — как в /tpl и /shaxta */
    void *m, *exc = NULL, *args[2]; float v[3]; char et[200];
    v[0] = pos[0]; v[1] = pos[1]; v[2] = pos[2];
    m = find_method(A.object_get_class(me), "SetPosition", 2);
    if (!m) return 0;
    args[0] = v; args[1] = &findH;
    A.runtime_invoke(m, me, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); if (g_zv_fzErr < 6) { g_zv_fzErr++; dlog2("zavod: SetPosition бросил исключение: ", et); } return 0; }
    return 1;
}
/* on=1 — заморозить (идемпотентно: accumulated=false); on=0 — сначала на землю под точкой, потом отпустить. 1 — вызов прошёл */
static int zv_freeze(int on) {
    void *me, *m, *exc = NULL, *args[3]; u8 st = on ? 1 : 0, acc = 0; char et[200];
    if (!g_safe || !il2cpp_light()) return 0;
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) { g_zv_fz = 0; return 0; }
    m = find_method(A.object_get_class(me), "ToggleFreeze", 3);     /* Actor.ToggleFreeze(string type, bool status, bool accumulated) */
    if (!m) { if (g_zv_fzErr < 6) { g_zv_fzErr++; dlog("zavod: нет Entity.ToggleFreeze(string,bool,bool)"); } return 0; }
    if (!on && g_zv_fz) {
        float p[3];
        if (zv_myref(p)) { p[1] += ZV_LAND_UP; zv_set_pos(me, p, 1); }
    }
    args[0] = A.string_new(ZV_FZ_TYPE); args[1] = &st; args[2] = &acc;
    if (!args[0]) return 0;
    A.runtime_invoke(m, me, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); if (g_zv_fzErr < 6) { g_zv_fzErr++; dlog2("zavod: ToggleFreeze бросил исключение: ", et); } return 0; }
    g_zv_fz = on ? 1 : 0;
    return 1;
}

/* ───────────── маркер сервера (Checkpoint.Simple / Race) — запасной путь, если нет ни подписей, ни точек ───────────── */
static void* zv_call0(void* obj, const char* name) { return zv_obj(obj, name); }
static int zv_marker_one(const char* fld) {
    void *cls, *f, *obj = NULL, *tr, *go, *m, *exc = NULL, *r; const float* v;
    if (!g_safe || !il2cpp_light() || !g_img) return 0;
    cls = A.class_from_name(g_img, "Hassle.Entities", "Checkpoint");
    f = cls ? A.class_get_field_from_name(cls, fld) : NULL;
    if (!f) return 0;
    A.field_static_get_value(f, &obj);
    if (!obj || !hm_obj_ok(obj)) return 0;
    go = zv_call0(obj, "get_gameObject");
    if (go) {
        m = find_method(A.object_get_class(go), "get_activeInHierarchy", 0);
        if (m) { r = A.runtime_invoke(m, go, NULL, &exc); if (!exc && r && hm_readable((char*)r + HDR, 1) && *((u8*)r + HDR) == 0) return 0; }
    }
    tr = zv_call0(obj, "get_transform");
    if (!tr) return 0;
    m = find_method(A.object_get_class(tr), "get_position", 0);
    if (!m) return 0;
    exc = NULL; r = A.runtime_invoke(m, tr, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    v = (const float*)((char*)r + HDR);
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return 0;
    return !(v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f);
}
static int zv_marker_active(void) { return zv_marker_one("Simple") || zv_marker_one("Race"); }

/* ───────────── 3D-подписи сервера: склады и станки ───────────── */
static int zv_lb_layout(void) {                                     /* смещения Position и Text внутри CreateText3DParams (нужны только они) */
    void *sc, *f; int shift;
    if (g_zv_layoutOk) return 1;
    if (!g_img) return 0;
    sc = A.class_from_name(g_img, "Hassle.Global.Pooling", "CreateText3DParams");
    if (!sc || !A.class_is_valuetype(sc)) { dlog("zavod: нет структуры Hassle.Global.Pooling.CreateText3DParams"); return 0; }
    shift = struct_shift(sc);
    f = find_field(sc, "Position"); if (!f) { dlog("zavod: нет поля CreateText3DParams.Position"); return 0; }
    g_zv_oPos = A.field_get_offset(f) - shift;
    f = find_field(sc, "Text"); if (!f) { dlog("zavod: нет поля CreateText3DParams.Text"); return 0; }
    g_zv_oTxt = A.field_get_offset(f) - shift;
    if (g_zv_oPos < 0 || g_zv_oTxt < 0 || g_zv_oPos > 120 || g_zv_oTxt > 120) { dlog_i("zavod: странная раскладка CreateText3DParams, Position ", g_zv_oPos); return 0; }
    g_zv_layoutOk = 1;
    return 1;
}
static int zv_lb_read(void* it, char* tmp, int cap, float* p) {     /* текст и мировая точка подписи; 0 — не прочиталась */
    void *sf, *sc, *s; char* sp; int n; const u16* ch;
    if (!hm_obj_ok(it)) return 0;
    sf = find_field(A.object_get_class(it), "m_CreateParams");
    if (!sf) return 0;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return 0;
    sp = (char*)it + A.field_get_offset(sf);
    if (!hm_readable(sp + g_zv_oPos, 12) || !hm_readable(sp + g_zv_oTxt, 8)) return 0;
    s = *(void**)(sp + g_zv_oTxt);
    if (!s || !hm_obj_ok(s)) return 0;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 200 || !ch) return 0;
    utf16_to_utf8(ch, n, tmp, cap);
    p[0] = ((float*)(sp + g_zv_oPos))[0]; p[1] = ((float*)(sp + g_zv_oPos))[1]; p[2] = ((float*)(sp + g_zv_oPos))[2];
    return zv_finite(p);
}
static int zv_status_of(const char* t) { return zv_has(t, "Свобод") ? 1 : zv_has(t, "Занят") ? 2 : 0; }
static void zv_lb_cb(void* it, void* ctx) {
    char tmp[220]; float p[3], d; int i, j; (void)ctx;
    if (!zv_lb_read(it, tmp, sizeof tmp, p)) return;
    d = zv_dist(p, g_zv_ref);
    /* список для /zavod list: ближайшие */
    for (i = 0; i < g_zv_nl && g_zv_ld[i] <= d; i++) {}
    if (i < ZV_LISTN) {
        int cnt = g_zv_nl < ZV_LISTN ? g_zv_nl : ZV_LISTN - 1;
        for (j = cnt; j > i; j--) { int k; for (k = 0; k < 60; k++) g_zv_ltxt[j][k] = g_zv_ltxt[j - 1][k]; g_zv_ld[j] = g_zv_ld[j - 1]; }
        for (j = 0; j < 59 && tmp[j]; j++) g_zv_ltxt[i][j] = tmp[j] == '\n' ? '|' : tmp[j];
        g_zv_ltxt[i][j] = 0; g_zv_ld[i] = d;
        if (g_zv_nl < ZV_LISTN) g_zv_nl++;
    }
    if (g_zv_mode == 1 && zv_has(tmp, ZV_KW_TAKE)) {
        if (!g_zv_hasLbTk || d < g_zv_lbTkD) { zv_cp3(g_zv_lbTk, p); g_zv_lbTkD = d; g_zv_hasLbTk = 1; }
    } else if (g_zv_mode == 1 && zv_has(tmp, ZV_KW_PUT)) {
        if (!g_zv_hasLbPt || d < g_zv_lbPtD) { zv_cp3(g_zv_lbPt, p); g_zv_lbPtD = d; g_zv_hasLbPt = 1; }
    } else if (zv_has(tmp, g_zv_mode == 1 ? ZV_KW_MACH1 : ZV_KW_MACH0)) {
        if (g_zv_nlm < ZV_MAXLB) { zv_cp3(g_zv_lm[g_zv_nlm].p, p); g_zv_lm[g_zv_nlm].st = zv_status_of(tmp); g_zv_nlm++; }
    }
}
static void zv_collect(int wantList) {
    int i;
    g_zv_nlm = 0; g_zv_hasLbTk = 0; g_zv_hasLbPt = 0; g_zv_nl = 0; (void)wantList;
    g_zv_mark = zv_marker_active();
    if (!zv_lb_layout()) { g_zv_poolN = -1; return; }
    g_zv_poolN = zv_scan_pool("Text3DLables", zv_lb_cb);
    g_zv_nb = 0;                                                    /* статус станка рядом с вами: ближайшая подпись в ZV_NEAR м по горизонтали */
    {
        float best = 1e9f;
        for (i = 0; i < g_zv_nlm; i++) {
            float h = zv_hdist(g_zv_lm[i].p, g_zv_ref);
            if (h < ZV_NEAR && h < best && g_zv_lm[i].p[1] - g_zv_ref[1] < 4.0f && g_zv_lm[i].p[1] - g_zv_ref[1] > -4.0f) { best = h; g_zv_nb = g_zv_lm[i].st; }
        }
    }
}

/* ───────────── итоговые точки: bind → подпись → встроено ───────────── */
static int zv_overlay(const float* t) {                             /* статус по ближайшей подписи станка в ZV_OVERLAY_R м от встроенного станка */
    int i, st = 0; float best = 1e9f;
    for (i = 0; i < g_zv_nlm; i++) {
        float h = zv_hdist(g_zv_lm[i].p, t);
        if (h < ZV_OVERLAY_R && h < best) { best = h; st = g_zv_lm[i].st; }
    }
    return st;
}
static void zv_add_m(const float* t, int st) { if (g_zv_nm < ZV_MAXM) { zv_cp3(g_zv_m[g_zv_nm].p, t); g_zv_m[g_zv_nm].st = st; g_zv_nm++; } }
static void zv_resolve(void) {
    int i;
    g_zv_nm = 0; g_zv_hasTk = 0; g_zv_hasPt = 0; g_zv_srcTk = 0; g_zv_srcPt = 0; g_zv_srcM = 0;
    if (g_zv_hasBTk) { zv_cp3(g_zv_tk, g_zv_bTk); g_zv_hasTk = 1; g_zv_srcTk = 1; }
    if (g_zv_hasBPt) { zv_cp3(g_zv_pt, g_zv_bPt); g_zv_hasPt = 1; g_zv_srcPt = 1; }
    if (g_zv_mode == 1) {
        if (!g_zv_hasTk) {
            if (g_zv_hasLbTk) { zv_cp3(g_zv_tk, g_zv_lbTk); g_zv_tk[1] -= 1.0f; g_zv_srcTk = 2; }       /* подпись стоит на серверной точке: Transform на 1 м ниже */
            else { zv_cp3(g_zv_tk, g_zv_f1_take); g_zv_srcTk = 3; }
            g_zv_hasTk = 1;
        }
        if (!g_zv_hasPt) {
            if (g_zv_hasLbPt) { zv_cp3(g_zv_pt, g_zv_lbPt); g_zv_pt[1] -= 1.0f; g_zv_srcPt = 2; }
            else { zv_cp3(g_zv_pt, g_zv_f1_put); g_zv_srcPt = 3; }
            g_zv_hasPt = 1;
        }
        if (g_zv_nbM > 0) { for (i = 0; i < g_zv_nbM; i++) zv_add_m(g_zv_bM[i], zv_overlay(g_zv_bM[i])); g_zv_srcM = 1; }
        else if (g_zv_nlm > 0) {                                    /* подпись станка — на 1 м выше серверной точки: Transform на 2 м ниже подписи */
            for (i = 0; i < g_zv_nlm; i++) { float t[3]; zv_cp3(t, g_zv_lm[i].p); t[1] -= 2.0f; zv_add_m(t, g_zv_lm[i].st); }
            g_zv_srcM = 2;
        } else { for (i = 0; i < 5; i++) zv_add_m(g_zv_f1_mach[i], 0); g_zv_srcM = 3; }
    } else {
        if (!g_zv_hasTk) {                                          /* ближайший из четырёх пикапов металла */
            int b = 0; float bd = 1e18f;
            for (i = 0; i < 4; i++) { float h = zv_dist(g_zv_f0_take[i], g_zv_ref); if (h < bd) { bd = h; b = i; } }
            zv_cp3(g_zv_tk, g_zv_f0_take[b]); g_zv_hasTk = 1; g_zv_srcTk = 3;
        }
        if (!g_zv_hasPt) { zv_cp3(g_zv_pt, g_zv_f0_put); g_zv_hasPt = 1; g_zv_srcPt = 3; }
        if (g_zv_nbM > 0) { for (i = 0; i < g_zv_nbM; i++) zv_add_m(g_zv_bM[i], zv_overlay(g_zv_bM[i])); g_zv_srcM = 1; }
        else { for (i = 0; i < 18; i++) zv_add_m(g_zv_f0_desk[i], zv_overlay(g_zv_f0_desk[i])); g_zv_srcM = 3; }
    }
}

/* ───────────── меню: внедрение JS и обмен состоянием ───────────── */
/* ZV_MENU_BEGIN */
static const char g_zv_menu_js[] =
"(function(){\n"
"if(window.__zv)return;\n"
"/* Состояние от C (мод шлёт его через window.__zv.u): mo — режим (0 завод-цех продуктов, 1 цех тюрьмы), me — ваша позиция,\n"
"   tk/pt — куда ставить игрока для «взять»/«сдать» (Transform, уже со сдвигом), mc — станки [x,y,z,статус 0?/1 свободен/2 занят],\n"
"   nb — статус станка рядом с вами (0 нет подписи, 1 свободен, 2 занят), k — сервер показывает маркер, p — подписи/пул прочитались */\n"
"var S={mo:0,me:[0,0,0],tk:null,pt:null,mc:[],nb:0,k:0,z:0,p:0,bt:0,bp:0,bm:0};\n"
"var cyc=false,fast=true,open=false,syncT=null,cycT=null,fzHeld=false,dragged=false;\n"
"var N={ok:0,bad:0,skip:0,turn:0};\n"
"var ph='take',hasFig=0,skipRun=0,phAt=0,tpAt=0,actLast=0,clkLast=0,clk=0,syncLast=0,tries=0,makeAt=0,turnAt=0,putAt=0,noTake=0,okAt=0,turnPoll=0,hintPh='',hintAt=0,hintTxt='',putWaitAt=0,rt=0,usedMk=0,lastMenu='',LOGI=true;\n"
"var bad={},cur=-1,curSt=0,why='';\n"
"var UI_CD=1000,SETTLE=250,GAP=250,SYNC=250,TICK=60,MENU_WAIT=1200,MAKE_MAX=26000,MAKE_FAST=7000,PACE=3300,TAKE_WAIT=1100,TAKE_TRIES=3,PUT_WAIT=20000,pend=false;   /* UI_CD — серверный кулдаун кликов меню (interactions.pwn: 1 с) */\n"
"var RX={take:/бревн|взять|металл|материал/i,mach:/станок|производ|использ/i,put:/фигур|положить|продукт|сдать/i};\n"
"var NAMES=['Завод (цех продуктов)','Цех тюрьмы (Turner)'];\n"
"function cmd(x){try{window.sendChatInput('/zavod q '+x);}catch(e){}}\n"
"function frz(v){fzHeld=!!v;cmd('fz '+(v?1:0));}\n"
"function f2(v){return (Math.round(v*100)/100).toFixed(2);}\n"
"function tp(p){jl('TP '+f2(p[0])+' '+f2(p[2]));if(cyc)frz(1);try{window.sendChatInput('/tpl to '+f2(p[0])+' '+f2(p[1])+' '+f2(p[2]));}catch(e){}}\n"
"function tpGps(){jl('TP gps');if(cyc)frz(1);try{window.sendChatInput('/tpl gps go');}catch(e){}}\n"
"function ev(n,a){try{if(typeof sendClientEvent==='function'&&typeof gm!=='undefined'){sendClientEvent(gm.EVENT_EXECUTE_PUBLIC,n,a);return 1;}}catch(e){}   /* та же форма вызова, что у Interactions.onClick в игре */\n"
" try{window.sendClientEventHandle(0,n,a);return 1;}catch(e){}try{window.sendClientEvent(gm.EVENT_EXECUTE_PUBLIC,{ignoreChat:true},n,a);return 1;}catch(e){}return 0;}\n"
"function turnerEnd(){   /* как в Code2.js: Turner_OnPlayerEnd сразу, не дожидаясь, пока заполнятся прямоугольники */\n"
" try{if(typeof sendClientEvent==='function'&&typeof gm!=='undefined'){sendClientEvent(gm.EVENT_EXECUTE_PUBLIC,'Turner_OnPlayerEnd');N.turn++;turnAt=Date.now();actLast=turnAt+400;jl('FIG Turner закрыт — жду ответ сервера');return 1;}}catch(e){}\n"
" if(ev('Turner_OnPlayerEnd',0)){N.turn++;turnAt=Date.now();actLast=turnAt+400;jl('FIG Turner закрыт — жду ответ сервера');return 1;}return 0;}\n"
"function turnerOpen(){try{return !!(window.getInterfaceStatus&&window.getInterfaceStatus('Turner'));}catch(e){return false;}}\n"
"/* ───── журнал интерфейсов → HLOG/zvui.txt (через мод /jlog) ─────\n"
"   Пишет: открытие/закрытие каждого интерфейса (имя + кусок текста), вызовы openInterface/closeInterface с параметрами, появление/пропажу плашки «Направляйтесь к станку» (с цепочкой классов и именами интерфейсов-владельцев). */\n"
"var logQ=[],logT=null,logDrop=0;\n"
"function p2(n,l){n=String(n);while(n.length<l)n='0'+n;return n;}\n"
"function jl(s){if(!LOGI)return;var d=new Date();s=String(s).replace(/[\\r\\n\\t]+/g,' ').replace(/\\s+/g,' ');if(s.charAt(0)==='/')s='_'+s;\n"
" var t=p2(d.getHours(),2)+':'+p2(d.getMinutes(),2)+':'+p2(d.getSeconds(),2)+'.'+p2(d.getMilliseconds(),3)+' '+s;\n"
" if(logQ.length>300){logDrop++;return;}\n"
" while(t.length>0){var c=t.length>100?(t.lastIndexOf(' ',100)>40?t.lastIndexOf(' ',100):100):t.length;logQ.push(t.slice(0,c));t=t.slice(c).replace(/^\\s+/,'');if(t)t='  '+t;}\n"
" if(!logT)logT=setInterval(function(){var l=logQ.shift();if(l===undefined){clearInterval(logT);logT=null;return;}try{window.sendChatInput('/jlog zvui '+l);}catch(e){}},110);}\n"
"function shortJ(v,n){var t;try{t=typeof v==='string'?v:JSON.stringify(v);}catch(e){t='?';}if(t===undefined)t='';t=String(t).replace(/\\s+/g,' ');return t.length>n?t.slice(0,n)+'…':t;}\n"
"function refsNow(){try{var r=window.App&&window.App.$refs;return r||null;}catch(e){return null;}}\n"
"function refComp(r,k){var c=r[k];if(c&&c.length!==undefined&&typeof c!=='string')c=c[0];return c||null;}\n"
"function compText(c){try{var e=c&&c.$el;var t=e?(e.innerText||e.textContent||''):'';return String(t).replace(/\\s+/g,' ').trim();}catch(e){return '';}}\n"
"var knownUi=null;\n"
"function uiPoll(){if(!LOGI)return;var r=refsNow();if(!r)return;var cur={},k,first=(knownUi===null);if(first)knownUi={};\n"
" for(k in r){if(refComp(r,k))cur[k]=1;}\n"
" for(k in cur){if(!knownUi[k]){knownUi[k]=Date.now();jl((first?'= ':'+ ')+k);(function(n){setTimeout(function(){var c=refsNow();c=c&&refComp(c,n);var t=c?compText(c):'';if(t)jl('  txt '+n+': '+t.slice(0,90));},500);})(k);}}\n"
" for(k in knownUi){if(!cur[k]){delete knownUi[k];jl('- '+k);}}}\n"
"/* ───── подсказка TrainingHint — точный шаг работы на заводе ─────\n"
"   сервер шлёт openInterface('TrainingHint',[\"текст\",время]): «Возьмите заготовку со склада.» → «Направляйтесь к станку.» → (меню «Начать производство», Turner) → «Отнесите изделие на ленту.» → снова «Возьмите…» */\n"
"function hintClass(t){t=String(t||'');if(/станк/i.test(t))return 'mach';if(/ленту|изделие|отнесите|сдайте/i.test(t))return 'put';if(/заготовк|металл|возьмите|со\\s+склада/i.test(t))return 'take';return '';}\n"
"function onHint(t,src){var k=hintClass(t);if(k===hintPh&&(src==='poll'||t===hintTxt))return;hintPh=k;hintTxt=t;hintAt=Date.now();jl('HINT '+(k||'?')+' | '+String(t).slice(0,50)+' ('+src+')');\n"
" if(!cyc)return;\n"
" if(k==='mach'){if(ph==='take'||ph==='put')go('mach');}\n"
" else if(k==='put'){if(ph==='take'||ph==='mach'||ph==='make')go('put');}\n"
" else if(k==='take'){if(ph==='put'){countOk();go('take');}else if(ph==='mach'||ph==='make')go('take');}\n"
" render();}\n"
"function hintPoll(now){var r=refsNow(),c=r&&refComp(r,'TrainingHint');   /* запасной путь, если хук openInterface не сработал */\n"
" if(c){var t=compText(c);if(t)onHint(t,'poll');}\n"
" else if(hintPh&&now-hintAt>1500){hintPh='';hintTxt='';}}\n"
"function nudge(now){var m=S.me;tp([m[0]+3.5,m[1],m[2]]);why='отхожу и возвращаюсь';tpAt=0;actLast=now+SETTLE+150;}   /* пикап/зона срабатывают на ВХОД — выходим и заходим заново */\n"
"function noteMenu(list){var a=[];for(var i=0;i<list.length&&i<4;i++)a.push(String(list[i].title).replace(/<[^>]*>/g,''));lastMenu=a.join(' / ');}\n"
"function intr(){try{var d=document.querySelectorAll('.interactions-list').length;if(!d&&!window.getInterfaceStatus('Interactions'))return null;var v=window.interface('Interactions');if(v&&v.list&&v.list.length)return v.list;}catch(e){}return null;}\n"
"function el(tag,id,cls,txt){var e=document.createElement(tag);if(id)e.id=id;if(cls)e.className=cls;if(txt!==undefined)e.textContent=txt;return e;}\n"
"var css='#zv-fab{position:fixed;right:1.4vh;top:44vh;width:6.6vh;height:6.6vh;border-radius:50%;background:rgba(12,10,6,.82);border:0.35vh solid #4fc3f7;z-index:999990;pointer-events:auto;display:flex;align-items:center;justify-content:center}'\n"
"+'#zv-fab.on{background:rgba(79,195,247,.9)}'\n"
"+'#zv-fab svg{width:3.6vh;height:3.6vh}'\n"
"+'#zv-panel{position:fixed;left:50%;top:50%;margin-left:-30vh;margin-top:-30vh;width:60vh;background:rgba(12,10,6,.95);border:0.3vh solid #4fc3f7;border-radius:1.6vh;z-index:999991;pointer-events:auto;color:#eeeee6;font-family:\"Open Sans\",sans-serif;display:none;padding:1.2vh 1.6vh 1.4vh 1.6vh;box-sizing:border-box}'\n"
"+'#zv-panel.show{display:block}'\n"
"+'.zv-h{display:flex;align-items:center;justify-content:space-between;height:6vh}'\n"
"+'.zv-t{font-size:2.9vh;font-weight:700;color:#4fc3f7}'\n"
"+'.zv-sub{font-size:1.7vh;color:rgba(238,238,230,.55);margin:-0.4vh 0 0.8vh 0}'\n"
"+'.zv-x{width:5.6vh;height:5.6vh;border-radius:50%;background:rgba(255,255,255,.1);display:flex;align-items:center;justify-content:center;font-size:3.4vh;line-height:3.4vh;color:#eeeee6}'\n"
"+'.zv-row{display:flex;align-items:center;justify-content:space-between;height:7.6vh;border-top:0.2vh solid rgba(255,255,255,.1)}'\n"
"+'.zv-l{display:flex;flex-direction:column;justify-content:center;flex:1;margin-right:1vh;min-width:0}'\n"
"+'.zv-n{font-size:2.6vh;font-weight:700}'\n"
"+'.zv-s{font-size:1.9vh;color:rgba(238,238,230,.62);margin-top:0.3vh;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}'\n"
"+'.zv-sw{width:8.4vh;height:4.6vh;border-radius:2.3vh;background:rgba(255,255,255,.18);position:relative;flex-shrink:0}'\n"
"+'.zv-sw i{position:absolute;top:0.5vh;left:0.5vh;width:3.6vh;height:3.6vh;border-radius:50%;background:#eeeee6}'\n"
"+'.zv-sw.on{background:#4fc3f7}'\n"
"+'.zv-sw.on i{left:4.3vh;background:#0c0a06}'\n"
"+'.zv-b{min-width:5.6vh;height:5.2vh;border-radius:1.2vh;background:rgba(255,255,255,.12);display:flex;align-items:center;justify-content:center;font-size:2.1vh;font-weight:700;padding:0 1.2vh;box-sizing:border-box}'\n"
"+'.zv-f{display:flex;align-items:center;justify-content:space-between;border-top:0.2vh solid rgba(255,255,255,.1);padding-top:1vh;height:7vh;box-sizing:border-box}'\n"
"+'.zv-hint{font-size:1.7vh;color:rgba(238,238,230,.55);flex:1;margin-right:1.2vh}'\n"
"+'.zv-rs{background:rgba(226,85,68,.22);color:#ffb3a8}';\n"
"var st=el('style');st.textContent=css;document.head.appendChild(st);\n"
"var fab=el('div','zv-fab','');\n"
"fab.innerHTML='<svg viewBox=\"0 0 24 24\"><path d=\"M3 21V9l6 4V9l6 4V5h6v16z\" fill=\"#4fc3f7\" stroke=\"#0c0a06\" stroke-width=\"1.3\" stroke-linejoin=\"round\"/><path d=\"M7 17h2M12 17h2M17 17h2\" stroke=\"#0c0a06\" stroke-width=\"1.4\"/></svg>';\n"
"var panel=el('div','zv-panel','');\n"
"var hd=el('div',null,'zv-h');hd.appendChild(el('div',null,'zv-t','Завод: автоцикл (тест)'));\n"
"var xb=el('div',null,'zv-x','\\u00d7');hd.appendChild(xb);panel.appendChild(hd);\n"
"panel.appendChild(el('div',null,'zv-sub','Взять → свободный станок → быстро доделать → сдать → по кругу'));\n"
"function mkSw(){var s=el('div',null,'zv-sw');s.appendChild(el('i'));return s;}\n"
"var cr=el('div',null,'zv-row');var cl=el('div',null,'zv-l');cl.appendChild(el('div',null,'zv-n','Автозавод'));\n"
"var csub=el('div',null,'zv-s','выкл');cl.appendChild(csub);cr.appendChild(cl);var csw=mkSw();cr.appendChild(csw);panel.appendChild(cr);\n"
"var lr=el('div',null,'zv-row');var ll=el('div',null,'zv-l');ll.appendChild(el('div',null,'zv-n','Лог интерфейсов'));\n"
"var lsub=el('div',null,'zv-s','');ll.appendChild(lsub);lr.appendChild(ll);var lsw=mkSw();lr.appendChild(lsw);panel.appendChild(lr);\n"
"var fr=el('div',null,'zv-row');var fl=el('div',null,'zv-l');fl.appendChild(el('div',null,'zv-n','Быстрый Turner'));\n"
"var fsub=el('div',null,'zv-s','');fl.appendChild(fsub);fr.appendChild(fl);var fsw=mkSw();fr.appendChild(fsw);panel.appendChild(fr);\n"
"var mr=el('div',null,'zv-row');var ml=el('div',null,'zv-l');ml.appendChild(el('div',null,'zv-n','Режим'));\n"
"var msub=el('div',null,'zv-s','');ml.appendChild(msub);mr.appendChild(ml);var mb=el('div',null,'zv-b','Сменить');mr.appendChild(mb);panel.appendChild(mr);\n"
"var sr=el('div',null,'zv-row');var sl=el('div',null,'zv-l');sl.appendChild(el('div',null,'zv-n','Счётчик'));\n"
"var ssub=el('div',null,'zv-s','');sl.appendChild(ssub);sr.appendChild(sl);panel.appendChild(sr);\n"
"var ft=el('div',null,'zv-f');var hint=el('div',null,'zv-hint','');var rs=el('div',null,'zv-b zv-rs','Сброс');\n"
"ft.appendChild(hint);ft.appendChild(rs);panel.appendChild(ft);\n"
"document.body.appendChild(fab);document.body.appendChild(panel);\n"
"function render(){\n"
" fab.className=cyc?'on':'';csw.className='zv-sw'+(cyc?' on':'');lsw.className='zv-sw'+(LOGI?' on':'');lsub.textContent=LOGI?('вкл · HLOG/zvui.txt'+(logDrop?' · потеряно строк '+logDrop:'')):'выкл';fsw.className='zv-sw'+(fast?' on':'');\n"
" csub.textContent=cyc?(ph+' · '+why):(why||'выкл');\n"
" fsub.textContent=fast?'вкл · Turner закрывается сразу, как в Code2.js':'выкл';\n"
" msub.textContent=NAMES[S.mo]||'?';\n"
" ssub.textContent='сдано '+N.ok+' · брак '+N.bad+' · пропусков '+N.skip+' · Turner '+N.turn+' · шаг '+(hintPh||'?');\n"
" hint.textContent=S.p?('станков: '+S.mc.length+(S.tk?'':' · нет точки «взять»')+(S.pt?'':' · нет точки «сдать»')+(S.k?' · метка есть':' · метки нет')+(lastMenu?' · меню: '+lastMenu.slice(0,40):'')):'данные не прочитались (вы не в игре?)';\n"
"}\n"
"function setOpen(v){open=v;panel.className=v?'show':'';\n"
" if(syncT){clearInterval(syncT);syncT=null;}\n"
" if(v){cmd('sync');syncT=setInterval(function(){if(!cyc)cmd('sync');},2000);}}\n"
"fab.addEventListener('click',function(){if(dragged){dragged=false;return;}setOpen(!open);});\n"
"xb.addEventListener('click',function(){setOpen(false);});\n"
"fr.addEventListener('click',function(){fast=!fast;render();});\n"
"lr.addEventListener('click',function(){LOGI=!LOGI;if(LOGI){knownUi=null;jl('--- лог включён ---');}render();});\n"
"setInterval(function(){try{if(LOGI)uiPoll();hintPoll(Date.now());hookChat();}catch(e){}},400);\n"
"jl('--- zavod: лог интерфейсов запущен ---');\n"
"mb.addEventListener('click',function(){cmd('mode '+(S.mo?0:1));});\n"
"rs.addEventListener('click',function(){N={ok:0,bad:0,skip:0,turn:0};render();});\n"
"/* ───── автоцикл ───── */\n"
"function go(p){ph=p;skipRun=0;phAt=Date.now();jl('PH '+p+' | '+(why||''));tpAt=0;tries=0;rt=0;putWaitAt=0;usedMk=0;if(p==='take'){bad={};noTake=putAt?(S.mo===0?putAt+PACE:0):0;}render();}\n"
"function pickMach(){\n"
" for(var pass=0;pass<2;pass++){\n"
"  var L=S.mc,b=-1,bd=1e18,me=S.me;\n"
"  for(var i=0;i<L.length;i++){if(L[i][3]===2||bad[i])continue;var dx=L[i][0]-me[0],dz=L[i][2]-me[2],d=dx*dx+dz*dz;if(d<bd){bd=d;b=i;}}\n"
"  if(b>=0)return b;\n"
"  bad={};\n"
" }\n"
" return -1;}\n"
"function titleOf(x){return String(x.title).replace(/<[^>]*>/g,' ');}\n"
"function findBy(L,rx,tp){for(var i=0;i<L.length;i++){if((tp&&L[i].type===tp)||rx.test(titleOf(L[i])))return L[i];}return null;}\n"
"/* что нажимать. «Начать производство» нажимаем и в фазе «взять», если мы уже у станка (телепорт по метке сервера мог привести сюда раньше, чем цикл сменил фазу) */\n"
"function pickItem(L){\n"
" if(ph==='make'){return (hintPh==='mach'&&turnAt<=makeAt&&Date.now()-clkLast>1800)?findBy(L,RX.mach,399):null;}   /* клик не дошёл: меню и подсказка всё ещё «к станку» — жмём ещё раз */\n"
" if(ph==='take'){var a=findBy(L,RX.take);if(a)return a;return hintPh==='take'?null:findBy(L,RX.mach,399);}\n"
" if(ph==='mach')return findBy(L,RX.mach,399);\n"
" return findBy(L,RX.put);}\n"
"/* клик по пункту меню: как в Code2.js — Interactions.onClick(индекс), иначе событие OnInteractionsClick; фаза двигается сразу */\n"
"function clickItem(it,idx){var now=Date.now();pend=false;\n"
" var ok=ev('OnInteractionsClick',it.type);jl('CLICK '+titleOf(it).trim().slice(0,30)+' type='+it.type+' ph='+ph+' ev='+ok);\n"
" clk++;clkLast=now;actLast=now+GAP;var tt=titleOf(it),kind=(RX.put.test(tt)&&!RX.mach.test(tt))?'put':(RX.take.test(tt)&&!RX.mach.test(tt)&&it.type!==399)?'take':'mach';afterClick(now,kind);render();}\n"
"function schedClick(it,idx,d){if(pend)return;pend=true;var w=Math.max(d||80,clkLast+UI_CD-Date.now());setTimeout(function(){if(!cyc){pend=false;return;}clickItem(it,idx);},w);}\n"
"function onInter(params){   /* меню взаимодействий только что открылось: решаем сразу, без опроса */\n"
" if(!cyc)return;var list=[];\n"
" try{var pr=(typeof params==='object')?params:JSON.parse(params);for(var k in pr)list.push({type:pr[k][0],title:pr[k][1]});}catch(e){return;}\n"
" noteMenu(list);var it=pickItem(list);jl('MENU ph='+ph+' hint='+(hintPh||'?')+' pick='+(it?titleOf(it).trim().slice(0,24):'нет'));if(it)schedClick(it,list.indexOf(it),60);}\n"
"function countOk(){var n=Date.now();if(n-okAt>800){N.ok++;okAt=n;}putAt=n;}\n"
"function afterClick(now,kind){\n"
" if(kind==='take')go('mach');\n"
" else if(kind==='put'){hasFig=0;countOk();go('take');}\n"
" else{go('make');makeAt=now;}\n"
"}\n"
"function doTake(now){\n"
" if(now<noTake){why='пауза между кругами';return;}\n"
" if(hintPh==='mach'){why='заготовка взята (подсказка «к станку»)';go('mach');return;}\n"
" if(rt){rt=0;nudge(now);return;}\n"
" if(!tpAt){var p=S.tk;if(S.k&&S.bt!==1)tpGps();else if(p)tp(p);else{why='нет точки «взять»: встаньте и /zavod bind take';return;}\n"
"  why='к месту, где берут'+(tries?' (повтор '+tries+')':'');tpAt=now;actLast=now+SETTLE;tries++;return;}\n"
" if(S.mo===0){                                  /* завод: заготовку даёт пикап при касании; взяли — подсказка меняется на «к станку» */\n"
"  if(now-tpAt<SETTLE+TAKE_WAIT)return;\n"
"  if(!hintPh&&tries>=TAKE_TRIES){why='подсказки TrainingHint не видно — иду к станку без проверки';go('mach');return;}\n"
"  if(tries%TAKE_TRIES===0){why='не берёт ('+tries+'): на складе пусто? пауза 3 с';noTake=now+3000;}else why='не взял — повтор';\n"
"  rt=1;tpAt=0;return;}\n"
" if(now-tpAt<SETTLE+MENU_WAIT)return;\n"
" if(tries>=3){why='взять не удалось (3 попытки)';tries=0;}\n"
" tpAt=0;}\n"
"function doMach(now){\n"
" if(!tpAt){var i=pickMach();if(i<0){why='станков не видно';return;}\n"
"  cur=i;curSt=S.mc[i][3];tp(S.mc[i]);why='к станку '+(i+1);tpAt=now;actLast=now+SETTLE;return;}\n"
" if(now-tpAt<SETTLE+MENU_WAIT)return;\n"
" if(S.nb===2&&curSt!==2){go('make');makeAt=now;why='станок занят нами, ждём';return;}   /* без меню: на станок встаём — он стал «занят» (таймерный завод) */\n"
" bad[cur]=1;N.skip++;why='станок не принял — следующий';tpAt=0;\n"
" if(S.mo===1&&++skipRun>=3){skipRun=0;hasFig=1;jl('FIG станки не принимают 3 раза — возможно, фигурка в руках');}}\n"
"function doMake(now){\n"
" why='делаем';\n"
" if(turnAt&&turnAt>makeAt&&now-turnAt>1200){hasFig=1;jl('FIG ответа чата нет — пробую сдать один раз');go('put');return;}     /* Turner закрыт, ответа в чате не было — считаем, что фигурка есть */\n"
" if(S.mo===0&&S.nb===1&&now-makeAt>1500){go('put');return;}        /* таймерный завод: станок снова «свободен» — продукт готов */\n"
" if(now-makeAt>(S.mo===0?MAKE_MAX:MAKE_FAST)){N.bad++;why='долго нет результата';go('take');}}\n"
"function doPut(now){\n"
" if(S.mo===0){                                  /* завод: сдача — только на метку сервера, когда она появится (центр зоны не трогаем); /zavod bind put — приоритетнее */\n"
"  var useB=!!(S.pt&&S.bp===1);\n"
"  if(rt){rt=0;nudge(now);return;}\n"
"  if(!tpAt){\n"
"   if(useB){tp(S.pt);usedMk=0;}\n"
"   else if(S.k){tpGps();usedMk=1;}\n"
"   else{if(!putWaitAt)putWaitAt=now;why='ждём метку «сдать»';return;}\n"
"   putWaitAt=0;why='сдаём (на метку)';tpAt=now;actLast=now+SETTLE;tries++;return;}\n"
"  if(usedMk&&!S.k&&now-tpAt>SETTLE+300){countOk();go('take');why='метка исчезла — сдано';return;}\n"
"  if(now-tpAt<SETTLE+MENU_WAIT)return;\n"
"  if(usedMk&&tries<3){why='метка ещё стоит — повтор';rt=1;tpAt=0;return;}\n"
"  putAt=now;go('take');return;}\n"
" if(rt){rt=0;nudge(now);return;}                                  /* зона срабатывает на ВХОД — отходим и заходим заново */\n"
" if(!tpAt){var p=S.pt;if(p)tp(p);else if(S.k)tpGps();else{why='нет точки «сдать»: встаньте и /zavod bind put';return;}\n"
"  why='сдаём'+(tries?' (повтор '+tries+')':'');tpAt=now;actLast=now+SETTLE;tries++;return;}\n"
" if(now-tpAt<SETTLE+MENU_WAIT)return;\n"
" if(hasFig&&tries<2){why='сдача не подтверждена сервером — повтор '+tries;jl('PUT нет ответа, повтор '+tries+' меню='+lastMenu);rt=1;tpAt=0;return;}   /* с фигуркой в руках за бревном не идём */\n"
" hasFig=0;putAt=now;go('take');}                                   /* 5 повторов без ответа или «Отнесите…» в чате не видели: старое поведение */\n"
"function tick(){try{\n"
" var now=Date.now();\n"
" if(now-syncLast>=SYNC){cmd('sync');syncLast=now;}\n"
" if(fast&&now-turnPoll>400&&now-turnAt>1000){turnPoll=now;if(turnerOpen())turnerEnd();}   /* запасной путь, если хук openInterface не сработал */\n"
" if(S.mo===1&&hasFig&&ph!=='put'){why='фигурка в руках — несу на склад';go('put');}   /* сервер молча игнорирует «Взять бревно», пока фигурка не сдана — без этого цикл крутится take↔станок и на сдачу не идёт */\n"
" var L=intr();\n"
" if(L){\n"
"  noteMenu(L);\n"
"  var it=pickItem(L);\n"
"  if(it&&clk<3){schedClick(it,L.indexOf(it),0);return;}\n"
" }else clk=0;\n"
" if(now<actLast)return;\n"
" if(ph==='take')doTake(now);else if(ph==='mach')doMach(now);else if(ph==='make')doMake(now);else doPut(now);\n"
" render();\n"
"}catch(e){why='ошибка: '+e;}}\n"
"function cycSet(v,reason){if(v===cyc)return;cyc=v;if(cycT){clearInterval(cycT);cycT=null;}\n"
" if(cyc){clk=0;actLast=0;clkLast=0;syncLast=0;turnAt=0;putAt=0;hasFig=0;bad={};go('take');if(hintPh==='mach')go('mach');else if(hintPh==='put')go('put');why='старт';cmd('sync');cycT=setInterval(tick,TICK);}\n"
" else{if(fzHeld)frz(0);why=reason||'';}\n"
" render();}\n"
"cr.addEventListener('click',function(){cycSet(!cyc);});\n"
"function manual(src){if(cyc)cycSet(false,'стоп: ручное управление ('+src+')');}\n"
"function hookCtl(n,onlyStick){var f=window[n];if(typeof f!=='function'||f.__zv)return;var w=function(p){try{if(cyc&&typeof p==='string'&&p.indexOf('<Mouse>/')!==0&&(!onlyStick||p==='<Gamepad>/leftStick'))manual(p==='<Gamepad>/leftStick'?'джойстик':'кнопка');}catch(e){}return f.apply(this,arguments);};w.__zv=1;window[n]=w;}\n"
"hookCtl('onScreenControlTouchStart',false);hookCtl('onScreenControlTouchMove',true);\n"
"window.addEventListener('keydown',function(e){try{if(!cyc)return;var t=e.target,tg=t&&t.tagName;if(tg==='INPUT'||tg==='TEXTAREA'||(t&&t.isContentEditable))return;manual('клавиша');}catch(x){}},true);\n"
"/* быстрый Turner — перенесено из Code2.js (модуль ЗАВОД): ДО открытия окна глушим блокировку курсора и прятание HUD/чата (иначе движок блокирует движение), после открытия\n"
"   прячем .turner через opacity, Turner_OnPlayerEnd шлём на следующем тике (~0 мс), страховочно закрываем окно через 200 мс. Меню взаимодействий ловим тем же хуком. */\n"
"(function(){var oi=window.openInterface;if(typeof oi!=='function'||oi.__zv)return;\n"
" var w=function(name,params){\n"
"  try{if(LOGI&&name!=='Interactions')jl('openInterface '+name+' '+shortJ(params,60));else if(LOGI)jl('openInterface Interactions '+shortJ(params,80));}catch(e){}\n"
"  var isT=false,oc=null,hud=null,sH=null,sC=null,cp=false;\n"
"  try{isT=!!fast&&name==='Turner';}catch(e){}\n"
"  if(isT){\n"
"   try{oc=window.setCursorStatus;window.setCursorStatus=function(t,st){if(st===true||st===1)return;return typeof oc==='function'?oc.apply(this,arguments):undefined;};cp=true;}catch(e){}\n"
"   try{hud=typeof window.interface==='function'?window.interface('Hud'):null;if(hud){sH=hud.manualHideHud;sC=hud.manualHideChat;hud.manualHideHud=function(){};hud.manualHideChat=function(){};}}catch(e){}\n"
"   setTimeout(function(){try{if(cp){window.setCursorStatus=oc;cp=false;}}catch(e){}\n"
"    try{var h=typeof window.interface==='function'?window.interface('Hud'):null;if(h){if(sH)h.manualHideHud=sH;if(sC)h.manualHideChat=sC;}}catch(e){}},600);\n"
"  }\n"
"  var r=oi.apply(this,arguments);\n"
"  try{\n"
"   if(isT){\n"
"    var hid=false;[0,16,32,80,150].forEach(function(d){setTimeout(function(){if(hid)return;var e=document.querySelector('.turner');if(e){e.style.setProperty('opacity','0','important');e.style.setProperty('pointer-events','none','important');e.style.setProperty('background','none','important');hid=true;}},d);});\n"
"    setTimeout(turnerEnd,0);\n"
"    setTimeout(function(){try{if(window.getInterfaceStatus&&window.getInterfaceStatus('Turner'))window.closeInterface('Turner');}catch(e){}},200);\n"
"   }\n"
"   if(name==='Interactions')onInter(params);\n"
"   else if(name==='TrainingHint'){var pp=params;if(typeof pp==='string'){try{pp=JSON.parse(pp);}catch(e){pp=[pp];}}onHint(pp&&pp[0]!==undefined?pp[0]:pp,'hook');}\n"
"  }catch(e){}\n"
"  return r;};\n"
" w.__zv=1;window.openInterface=w;})();\n"
"(function(){var cf=window.closeInterface;if(typeof cf!=='function'||cf.__zv)return;var w=function(n){try{if(LOGI)jl('closeInterface '+n);}catch(e){}return cf.apply(this,arguments);};w.__zv=1;window.closeInterface=w;})();\n"
"/* сообщения сервера в чате — быстрые подсказки для цикла (если чат так не вызывается, цикл работает по таймерам) */\n"
"var CHATRX=/станк|бревн|металл|продукт|фигур|склад|завод|взя[лт]|положи|Turner|Срок|вознагражд|сначала/i;\n"
"function hookChat(){try{var h=typeof window.interface==='function'?window.interface('Hud'):null;var c=h&&h.$refs&&h.$refs.chat;   /* игра шлёт чат через interface('Hud').$refs['chat'].add(текст, цвет), а не через window.onChatMessage */\n"
" if(!c||typeof c.add!=='function'||c.add.__zv)return;var f=c.add;\n"
" var w=function(m){try{var q=String(m).replace(/[{][0-9a-fA-F]{6}[}]/g,'');if(LOGI&&CHATRX.test(q))jl('chat '+q.slice(0,100));chatEv(q);}catch(e){}return f.apply(this,arguments);};\n"
" w.__zv=1;c.add=w;jl('chat hook ok');}catch(e){}}\n"
"function chatEv(t){\n"
" if(!cyc)return;\n"
" if(t.indexOf('Отнесите сделанную фигурку')>=0||t.indexOf('Вы уже сделали фигурку')>=0){hasFig=1;jl('FIG есть');if(ph!=='put')go('put');}\n"
" else if(t.indexOf('сначала сделать её')>=0||t.indexOf('Вы не сделали фигурку')>=0){hasFig=0;jl('FIG сервер: фигурки нет');if(ph==='put'||ph==='make')go('take');}\n"
" else if(ph==='put'&&(t.indexOf('За выполненную фигурку')>=0||t.indexOf('Срок заключения')>=0)){hasFig=0;jl('FIG сдана (по награде)');countOk();go('take');}\n"
" else if(t.indexOf('сломалась фигурка')>=0){N.bad++;hasFig=0;go('take');}\n"
" else if(t.indexOf('Вы взяли бревно')>=0||t.indexOf('уже есть в руках бревно')>=0){hasFig=0;if(ph==='take')go('mach');}\n"
" else if(t.indexOf('Станок занят')>=0){if(cur>=0)bad[cur]=1;N.skip++;if(ph==='mach'||ph==='make'){ph='mach';tpAt=0;}}\n"
" else if(t.indexOf('положили сделанную фигурку')>=0||t.indexOf('Продукт доставлен на склад')>=0){hasFig=0;jl('FIG сдана');countOk();go('take');}\n"
" else if(t.indexOf('Возьмите металл')>=0){if(ph!=='take')go('take');}\n"
" else if(t.indexOf('Вы делаете продукт')>=0){if(ph==='take')go('mach');}\n"
"}\n"
"(function(){var oc=window.onChatMessage;if(typeof oc!=='function'||oc.__zv)return;\n"
" var w=function(t){try{var q=String(t);if(LOGI&&/станк|бревн|металл|продукт|фигур|склад|завод|взя[лт]|положи|Turner/i.test(q))jl('chat '+q.replace(/\\{[0-9a-fA-F]{6}\\}/g,'').slice(0,100));chatEv(q);}catch(e){}return oc.apply(this,arguments);};w.__zv=1;window.onChatMessage=w;})();\n"
"/* перетаскивание кнопки */\n"
"var dx=0,dy=0,sx=0,sy=0,tch=false;\n"
"fab.addEventListener('touchstart',function(e){var t=e.touches[0];sx=t.clientX;sy=t.clientY;var b=fab.getBoundingClientRect();dx=t.clientX-b.left;dy=t.clientY-b.top;tch=true;dragged=false;});\n"
"fab.addEventListener('touchmove',function(e){if(!tch)return;var t=e.touches[0];if(Math.abs(t.clientX-sx)+Math.abs(t.clientY-sy)<12&&!dragged)return;dragged=true;\n"
" fab.style.right='auto';fab.style.left=Math.max(0,Math.min(window.innerWidth-fab.offsetWidth,t.clientX-dx))+'px';fab.style.top=Math.max(0,Math.min(window.innerHeight-fab.offsetHeight,t.clientY-dy))+'px';});\n"
"fab.addEventListener('touchend',function(){tch=false;});\n"
"window.__zv={u:function(s){for(var k in s){S[k]=s[k];}render();},o:function(w){if(w)setOpen(true);},c:function(v){cycSet(!!v);}};\n"
"render();\n"
"})();\n"
;
/* ZV_MENU_END */
static void zv_ui_inject(void) {                                    /* идемпотентно: если меню уже есть на странице, JS сразу выходит */
    if (zv_js_run(g_zv_menu_js)) { g_zv_uiOk = 1; g_zv_lastJs[0] = 0; }
}
static void zv_b_vec(Buf* b, const float* v) {
    b_ch(b, '['); b_fix(b, v[0], 2); b_ch(b, ','); b_fix(b, v[1], 2); b_ch(b, ','); b_fix(b, v[2], 2); b_ch(b, ']');
}
static void zv_ui_push(int force) {                                 /* состояние -> меню: window.__zv.u({...}) */
    char js[3800]; Buf b; int i, k;
    if (!g_zv_uiOk) return;
    b_init(&b, js, sizeof js);
    b_s(&b, "window.__zv&&window.__zv.u({mo:"); b_int(&b, g_zv_mode);
    b_s(&b, ",me:"); zv_b_vec(&b, g_zv_ref);
    b_s(&b, ",tk:"); if (g_zv_hasTk) zv_b_vec(&b, g_zv_tk); else b_s(&b, "null");
    b_s(&b, ",pt:"); if (g_zv_hasPt) zv_b_vec(&b, g_zv_pt); else b_s(&b, "null");
    b_s(&b, ",nb:"); b_int(&b, g_zv_nb);
    b_s(&b, ",k:"); b_int(&b, g_zv_mark);
    b_s(&b, ",p:"); b_int(&b, g_zv_poolN >= 0 ? 1 : 0);
    b_s(&b, ",z:"); b_int(&b, g_zv_fz);
    b_s(&b, ",bt:"); b_int(&b, g_zv_srcTk); b_s(&b, ",bp:"); b_int(&b, g_zv_srcPt);     /* источник точек: 1 — /zavod bind (приоритет над меткой сервера) */
    b_s(&b, ",mc:[");
    for (i = 0; i < g_zv_nm; i++) {
        if (i) b_ch(&b, ',');
        b_ch(&b, '['); b_fix(&b, g_zv_m[i].p[0], 2); b_ch(&b, ','); b_fix(&b, g_zv_m[i].p[1], 2); b_ch(&b, ','); b_fix(&b, g_zv_m[i].p[2], 2);
        b_ch(&b, ','); b_int(&b, g_zv_m[i].st); b_ch(&b, ']');
    }
    b_s(&b, "]})");
    if (!force && seq(js, g_zv_lastJs)) return;
    for (k = 0; js[k] && k < (int)sizeof g_zv_lastJs - 1; k++) g_zv_lastJs[k] = js[k];
    g_zv_lastJs[k] = 0;
    zv_js_run(js);
}
static void zv_sync(int wantList) {                                 /* свежий скан подписей и пересчёт точек; результат — в меню */
    if (!g_safe || !il2cpp_light()) return;
    ensure_attached();
    if (!zv_myref(g_zv_ref)) return;
    zv_collect(wantList);
    zv_resolve();
}

/* ───────────── команда ───────────── */
static int zv_tokens(void* arr, char tok[][32], int maxn) {
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
            if (!q || w[0] == '/' || seq(w, "zavod")) continue;     /* сама команда может входить в массив (как у /tpl, /shaxta) */
            if (n < maxn) { int z; for (z = 0; z <= q; z++) tok[n][z] = w[z]; n++; }
        }
    }
    return n;
}
static void zv_usage(void) {
    chat_usage("/zavod — тестовый автоцикл завода: меню на экране.  /zavod on|off — автоцикл;  /zavod mode 0|1 — 0 завод, 1 цех тюрьмы (токарные станки)");
    chat_usage("/zavod bind take|put|mach|clear — запомнить текущее место как «взять» / «сдать» / станок / сбросить;  /zavod list — ближайшие 3D-подписи;  /zavod status");
    chat_usage("/zavod fz [on|off] — ручная заморозка персонажа (автоцикл замораживает сам)");
}
static const char* zv_src_name(int s) { return s == 1 ? "bind" : s == 2 ? "подпись сервера" : s == 3 ? "встроено из Pawn" : "нет"; }
static void zv_status(void) {
    char m[300]; Buf b; int i, fr = 0, bz = 0;
    for (i = 0; i < g_zv_nm; i++) { if (g_zv_m[i].st == 1) fr++; else if (g_zv_m[i].st == 2) bz++; }
    b_init(&b, m, sizeof m);
    b_s(&b, "/zavod: режим "); b_s(&b, g_zv_mode ? "1 (цех тюрьмы)" : "0 (завод)");
    b_s(&b, ", «взять»: "); b_s(&b, zv_src_name(g_zv_srcTk)); b_s(&b, ", «сдать»: "); b_s(&b, zv_src_name(g_zv_srcPt));
    chat_say(m);
    b_init(&b, m, sizeof m);
    b_s(&b, "  станков "); b_int(&b, g_zv_nm); b_s(&b, " ("); b_s(&b, zv_src_name(g_zv_srcM)); b_s(&b, "), свободно "); b_int(&b, fr); b_s(&b, ", занято "); b_int(&b, bz);
    b_s(&b, ", подписей станков видно "); b_int(&b, g_zv_nlm); b_s(&b, ", рядом с вами: "); b_s(&b, g_zv_nb == 1 ? "свободен" : g_zv_nb == 2 ? "занят" : "нет подписи");
    chat_say(m);
    b_init(&b, m, sizeof m);
    b_s(&b, "  подписей в пуле: "); b_int(&b, g_zv_poolN); b_s(&b, " (-1 — не прочитался), маркер сервера: "); b_s(&b, g_zv_mark ? "есть" : "нет");
    b_s(&b, ", заморозка: "); b_s(&b, g_zv_fz ? "наша ВКЛ" : "выкл"); b_s(&b, ", запомнено станков: "); b_int(&b, g_zv_nbM);
    chat_say(m);
}
static void zv_list(void) {
    int i; char m[200]; Buf b;
    zv_sync(1);
    if (g_zv_poolN < 0) { chat_say("/zavod: подписи не прочитались (вы не в игре?) — подробности в konstt.log"); return; }
    if (g_zv_nl == 0) { chat_say("/zavod: рядом нет 3D-подписей (сервер стримит их только вблизи: подойдите к складу или станку)"); return; }
    chat_say("/zavod: ближайшие 3D-подписи (если названия не те — поправьте ZV_KW_* в hassle_zavod.c):");
    for (i = 0; i < g_zv_nl; i++) {
        b_init(&b, m, sizeof m);
        b_s(&b, "  "); b_fix(&b, g_zv_ld[i], 1); b_s(&b, " м: "); b_s(&b, g_zv_ltxt[i]);
        chat_say(m);
    }
}
static void zv_bind(const char* what) {
    float p[3];
    if (!g_safe || !il2cpp_light()) { chat_say("/zavod: игра ещё грузится, подождите"); return; }
    ensure_attached();
    if (seq(what, "clear") || seq(what, "сброс")) {
        g_zv_hasBTk = 0; g_zv_hasBPt = 0; g_zv_nbM = 0;
        zv_say("/zavod: всё запомненное сброшено, точки снова берутся из подписей сервера / встроенных координат");
        return;
    }
    if (!zv_myref(p)) { chat_say("/zavod: ваша позиция не прочиталась — подробности в konstt.log"); return; }
    if (seq(what, "take") || seq(what, "взять")) { zv_cp3(g_zv_bTk, p); g_zv_hasBTk = 1; zv_say("/zavod: точка «взять» запомнена"); }
    else if (seq(what, "put") || seq(what, "сдать")) { zv_cp3(g_zv_bPt, p); g_zv_hasBPt = 1; zv_say("/zavod: точка «сдать» запомнена"); }
    else if (seq(what, "mach") || seq(what, "станок")) {
        char m[120]; Buf b;
        if (g_zv_nbM >= ZV_MAXB) { chat_say("/zavod: запомнено уже максимум станков (8) — /zavod bind clear"); return; }
        zv_cp3(g_zv_bM[g_zv_nbM], p); g_zv_nbM++;
        b_init(&b, m, sizeof m); b_s(&b, "/zavod: станок запомнен, всего "); b_int(&b, g_zv_nbM); zv_say(m);
    } else { chat_usage("/zavod bind take|put|mach|clear"); return; }
}
static void zv_cmd_run(char tok[][32], int nt) {
    if (nt == 0 || seq(tok[0], "menu") || seq(tok[0], "меню")) {
        zv_ui_inject(); zv_sync(0); zv_ui_push(1);
        zv_js_run("window.__zv&&window.__zv.o(1)");
        if (!g_zv_uiOk) chat_say("/zavod: меню не внедрилось — подробности в konstt.log");
        return;
    }
    if (seq(tok[0], "help") || seq(tok[0], "помощь") || seq(tok[0], "?")) { zv_usage(); return; }
    if (seq(tok[0], "sync")) { zv_sync(0); return; }
    if (seq(tok[0], "on") || seq(tok[0], "вкл") || seq(tok[0], "1") || seq(tok[0], "off") || seq(tok[0], "выкл") || seq(tok[0], "0")) {
        int on = seq(tok[0], "on") || seq(tok[0], "вкл") || seq(tok[0], "1");
        if (!g_zv_uiOk) zv_ui_inject();
        zv_sync(0); zv_ui_push(1);
        zv_js_run(on ? "window.__zv&&window.__zv.c(1)" : "window.__zv&&window.__zv.c(0)");
        zv_say(on ? "/zavod: автоцикл включён (выключить: /zavod off или тронуть джойстик)" : "/zavod: автоцикл выключен");
        return;
    }
    if (seq(tok[0], "mode") || seq(tok[0], "режим") || seq(tok[0], "завод") || seq(tok[0], "цех") || seq(tok[0], "тюрьма") || seq(tok[0], "prison")) {
        int v = -1;
        const char* w = (seq(tok[0], "mode") || seq(tok[0], "режим")) ? (nt >= 2 ? tok[1] : "") : tok[0];
        if (seq(w, "0") || seq(w, "завод") || seq(w, "factory")) v = 0;
        else if (seq(w, "1") || seq(w, "цех") || seq(w, "тюрьма") || seq(w, "prison")) v = 1;
        if (v < 0) { chat_usage("/zavod mode 0|1 — 0 завод (металл → рабочее место → склад), 1 цех тюрьмы (бревно → токарный станок → склад фигурок)"); return; }
        g_zv_mode = v;
        zv_say(v ? "/zavod: режим 1 — цех тюрьмы (бревно → токарный станок → склад фигурок)" : "/zavod: режим 0 — завод (металл → рабочее место → склад)");
        zv_sync(0);
        return;
    }
    if (seq(tok[0], "bind") || seq(tok[0], "запомнить")) { if (nt < 2) { chat_usage("/zavod bind take|put|mach|clear"); return; } zv_bind(tok[1]); zv_sync(0); return; }
    if (seq(tok[0], "list") || seq(tok[0], "список")) { zv_list(); return; }
    if (seq(tok[0], "status") || seq(tok[0], "статус")) { zv_sync(0); zv_status(); return; }
    if (seq(tok[0], "log")) { if (nt >= 2) dlog2("zavod: js: ", tok[1]); return; }
    if (seq(tok[0], "fz") || seq(tok[0], "freeze") || seq(tok[0], "фриз") || seq(tok[0], "заморозка")) {
        int want = nt >= 2 ? (seq(tok[1], "on") || seq(tok[1], "вкл") || seq(tok[1], "1")) ? 1 : (seq(tok[1], "off") || seq(tok[1], "выкл") || seq(tok[1], "0")) ? 0 : -1 : !g_zv_fz;
        char m[160]; Buf b;
        if (want < 0) { chat_usage("/zavod fz [on|off] — заморозить / отпустить персонажа (без аргумента — переключить)"); return; }
        if (!zv_freeze(want)) { chat_say("/zavod fz: не вышло (вы не в игре или ToggleFreeze не найден) — подробности в konstt.log"); return; }
        b_init(&b, m, sizeof m); b_s(&b, "/zavod: заморозка "); b_s(&b, want ? "ВКЛ" : "выкл");
        b_s(&b, "; игра считает персонажа замороженным: "); b_s(&b, zv_is_frozen() ? "да" : "нет");
        zv_say(m);
        return;
    }
    zv_usage();
}
static void native_zv_cmd(void* self, void* args, void* mi) {
    char tok[6][32]; int nt, i, z;
    (void)self; (void)mi;
    nt = zv_tokens(args, tok, 6);
    g_zv_q = 0;
    if (nt >= 1 && seq(tok[0], "q")) {                               /* «/zavod q …» — из меню: тихо */
        g_zv_q = 1;
        for (i = 0; i + 1 < nt; i++) for (z = 0; z < 32; z++) tok[i][z] = tok[i + 1][z];
        nt--;
    }
    if (!g_zv_q) dlog_i("zavod: команда вызвана, слов ", nt);
    if (!g_zv_q && !g_zv_uiOk) zv_ui_inject();                       /* первая команда игрока — заодно ставим кнопку меню */
    zv_cmd_run(tok, nt);
    g_zv_q = 0;
    zv_ui_push(0);
}
static void* zv_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("zavod", native_zv_cmd)) return NULL;       /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /zavod */
    hm_mod_ready(g_zv_modId);
    dlog("zavod: мод загружен, команда /zavod работает");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void zv_ctor(void) {
    unsigned long t = 0;
    g_zv_modId = hm_mod_register("zavod");
    pthread_create(&t, NULL, zv_main, NULL);
}
#endif
