/*
 * hassle_ring.c — /ring: рисует на полу вокруг раздевалки (пикап «clothesp») ОДНУ зону, где надо стоять для /dressu и «Переодевание ночь».
 *
 * Что значит зона (по серверу, new.pwn и include/core/pickup.pwn):
 *   • вход в пикап сервер принимает, если игрок не дальше 5 м (IsPlayerInPickup(…, 5.0, …)), НО только когда флаг «уже стою на пикапе» (P_IN_PICKUP) сброшен;
 *   • флаг сбрасывается таймером в 250 мс, как только игрок дальше 1,5 м от пикапа, — пока вы ближе 1,5 м, повторный вход молча игнорируется;
 *   • ответы на диалоги раздевалки (DIALOG_TEAM_SKIN …) сервер принимает только до 4,0 м (IsPlayerInPickupEx(…, 4.0, …)).
 *   Значит стоять надо в «бублике»: от 1,5 до 4,0 м от точки пикапа (расстояние трёхмерное: позиция игрока в пакете синхронизации = Transform.y + 1).
 *   Зона нарисована с небольшим запасом: 1,6 … 3,8 м.
 *
 * Как выглядит: родная метка игры (Hassle.Entities.Checkpoint, префаб Checkpoint.prefab — те же метки, что сервер ставит через SetPlayerCheckpoint), прижатая к земле,
 *   размером с зону. ЗЕЛЁНАЯ — вы в зоне (стойте здесь), КРАСНАЯ — вы слишком близко к пикапу или вышли за зону.
 *   Берём гоночную метку Checkpoint.Race: SetRace(центр, размер) / HideRace(). Размер подгоняем по границам её рендереров, цвет — через материалы метки.
 *   Если игра или сервер сам переставил гоночную метку — мод выключается и её не трогает. Если метку нельзя найти или покрасить — запасной режим flat:
 *   своё тонкое кольцо на полу (LineRenderer в мировых координатах; в этой сборке игры у LineRenderer вырезаны alignment и loop, поэтому широкую полосу не сделать).
 *   Материал кольца: «Sprites/Default» (глубину проверяет — кольцо на полу), запасной «GUI/Text Shader» (рисуется поверх всего); /ring see переключает.
 *   Пол для кольца: Transform.y ваших ног, когда вы стоите на земле у раздевалки (пока не встали — пикап − 1 м); лишний метр между точкой пикапа и полом
 *   учитывается в радиусе: радиус на полу = sqrt(R² − dz²), dz = высота пикапа − (пол + 1).
 *
 * Команды:
 *   /ring            включить/выключить      /ring on | off
 *   /ring info       где центр, расстояние до него, зона
 *   /ring r 1.6 3.8  внутренний и внешний радиус зоны, м     /ring reset — вернуть 1.6 / 3.8, подъём 0, родную метку
 *   /ring any        центр — ближайший пикап любой модели (если раздевалка не находится по имени «cloth»);  /ring cloth — обратно только раздевалка
 *   /ring game       родная метка игры (по умолчанию)      /ring flat — своя плоская зона на полу
 *   /ring y 0.05     поднять/опустить плоскую зону относительно пола, м (−2…2); если она мерцает или уходит под пол — поднять
 *   /ring see        плоская зона: на полу (Sprites/Default) ⇄ поверх всего (GUI/Text Shader)
 *   /ring a 0.15     прозрачность родной метки, 0.02…1 (меньше — прозрачнее; по умолчанию 0.15)
 *   /ring mat 1|2|0  как красить родную метку: 1 — своим полупрозрачным материалом с текстурой метки (по умолчанию), 2 — своим без текстуры (сплошная заливка),
 *                    0 — родным материалом игры (только правка свойств цвета; у шейдера Hassle/Checkpoint это не красит)
 *   /ring scan       искать раздевалку заново сейчас
 *
 * Всё рисование — в главном потоке (Post в UnitySynchronizationContext, как в /nocol); фоновый поток раз в 0,5 с только ставит задачу.
 */
#pragma once
#include "hassle_core.c"

#define RG_SEG 72                       /* сегментов окружности */
#define RG_SCAN_M 60.0f                 /* раздевалку ищем не дальше этого от игрока, м */
#define RG_RESCAN_S 3                   /* как часто перечитывать пикапы, с */
#define RG_QUEUE 3010                   /* renderQueue, как у линий игры */
#define RG_LIFT 0.04f                   /* зона чуть выше пола, м (чтобы не мерцала с полом) */
#define RG_GROUND_R 8.0f                /* высоту земли берём по игроку, только если он не дальше этого от центра по горизонтали, м */

typedef struct {
    void* (*cls_methods)(void*, void**);
    const char* (*m_name)(void*);
    int (*m_pcount)(void*);
    void* (*m_param)(void*, int);
    void* (*cls_type)(void*);
    void* (*type_object)(void*);
    unsigned (*gc_new)(void*, int);
    void* (*gc_target)(unsigned);
    void (*gc_free)(unsigned);
} RgApi;
static RgApi RGA;
static int g_rg_apiState;               /* 0 — не искали, 1 — есть, -1 — нет */

static int g_ring_modId = -1;
static volatile int g_rg_on;            /* зона включена */
static volatile int g_rg_pending;       /* в главный поток уже поставлен тик */
static volatile long g_rg_postT;
static int g_rg_any;                    /* 1 — центр любой пикап (/ring any), 0 — только с «cloth» в имени модели */
static int g_rg_see;                    /* 0 — Sprites/Default (на полу), 1 — GUI/Text Shader (поверх всего) */
static float g_rg_alpha = 0.15f;        /* прозрачность родной метки: 1 — как есть, меньше — прозрачнее (/ring a) */
static int g_rg_skin = 1;               /* 1 — свой материал с текстурой метки, 2 — свой без текстуры, 0 — родной материал (/ring mat) */
static float g_rg_rIn = 1.6f, g_rg_rOut = 3.8f, g_rg_lift = 0.0f;
static const float g_rg_colOk[4]  = { 0.15f, 0.95f, 0.30f, 0.42f };     /* в зоне */
static const float g_rg_colBad[4] = { 1.00f, 0.12f, 0.12f, 0.42f };     /* слишком близко или за зоной */

/* найденный центр (пикап раздевалки) */
static int g_rg_have, g_rg_id;
static float g_rg_c[3];
static long g_rg_scanT;
static int g_rg_dirty;                  /* центр, пол или радиусы изменились — перерисовать */
static int g_rg_zone = -1;              /* 0 — слишком близко, 1 — ок, 2 — слишком далеко; -1 — неизвестно (цвет ещё не выставлен) */
static float g_rg_dist = -1.0f;
static int g_rg_notFoundSaid;
static float g_rg_gy; static int g_rg_gyOk;     /* высота пола у раздевалки (Transform.y ног игрока на земле) */
static float g_rg_drawGy = 1e9f;                /* пол, при котором нарисовано */

/* ресурсы Unity */
static struct {
    int state;                          /* 0 — не искали, 1 — всё есть, -1 — чего-то нет */
    void *kGo, *kMat, *kShader, *kLr, *kObj;
    void *mGoCtor, *mAdd, *mFind, *mMatCtor, *mSetQ, *mSetMat, *mWidth, *mWorld, *mCount, *mPos, *mCol0, *mCol1, *mEnable, *mDestroy;
    void* typeLr;
} RG;
static unsigned g_rg_hMat;
static unsigned g_rg_hGo, g_rg_hLr;
static int g_rg_fails;

/* ───────────── мелочи ───────────── */
static int rg_api(void) {
    if (g_rg_apiState) return g_rg_apiState > 0;
    *(void**)&RGA.cls_methods = dlsym(g_il2cpp, "il2cpp_class_get_methods");
    *(void**)&RGA.m_name      = dlsym(g_il2cpp, "il2cpp_method_get_name");
    *(void**)&RGA.m_pcount    = dlsym(g_il2cpp, "il2cpp_method_get_param_count");
    *(void**)&RGA.m_param     = dlsym(g_il2cpp, "il2cpp_method_get_param");
    *(void**)&RGA.cls_type    = dlsym(g_il2cpp, "il2cpp_class_get_type");
    *(void**)&RGA.type_object = dlsym(g_il2cpp, "il2cpp_type_get_object");
    *(void**)&RGA.gc_new      = dlsym(g_il2cpp, "il2cpp_gchandle_new");
    *(void**)&RGA.gc_target   = dlsym(g_il2cpp, "il2cpp_gchandle_get_target");
    *(void**)&RGA.gc_free     = dlsym(g_il2cpp, "il2cpp_gchandle_free");
    g_rg_apiState = (RGA.cls_methods && RGA.m_name && RGA.m_pcount && RGA.m_param && RGA.cls_type && RGA.type_object && RGA.gc_new && RGA.gc_target && RGA.gc_free) ? 1 : -1;
    if (g_rg_apiState < 0) dlog("ring: нет il2cpp_class_get_methods / type_get_object / gchandle_* — кольца не будет");
    return g_rg_apiState > 0;
}

/* метод name с одним параметром типа tn (у конструкторов Material и AddComponent есть другие перегрузки с тем же числом аргументов) */
static void* rg_find_m1(void* k, const char* name, const char* tn) {
    for (; k; k = A.class_get_parent(k)) {
        void* it = NULL; void* m;
        while ((m = RGA.cls_methods(k, &it)) != NULL) {
            const char* nm = RGA.m_name(m); const char* t;
            if (!nm || !seq(nm, name) || RGA.m_pcount(m) != 1) continue;
            t = A.type_get_name(RGA.m_param(m, 0));
            if (t && seq(t, tn)) return m;
        }
    }
    return NULL;
}

/* вызов; 1 — без исключения. При исключении пишет в журнал класс (первые несколько раз) */
static int rg_call(const char* what, void* m, void* obj, void** args, void** ret) {
    void* exc = NULL; void* r; static int said;
    r = A.runtime_invoke(m, obj, args, &exc);
    if (ret) *ret = r;
    if (exc) {
        if (said < 8) {
            said++;
            dlog2("ring: исключение в ", what);
            if (hm_obj_ok(exc)) dlog2("ring:   класс исключения ", A.class_get_name(A.object_get_class(exc)));
        }
        return 0;
    }
    return 1;
}

static int rg_finite3(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
static void* rg_obj(void* obj, const char* name) {          /* метод без аргументов, вернувший объект */
    void *m, *r = NULL;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m || !rg_call(name, m, obj, NULL, &r)) return NULL;
    return (r && hm_obj_ok(r)) ? r : NULL;
}
static int rg_vec(void* obj, const char* name, float* out) {   /* метод без аргументов, вернувший Vector3 */
    void *m, *r = NULL; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m || !rg_call(name, m, obj, NULL, &r) || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!rg_finite3(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
static float rg_d3(const float* a, const float* b) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz);
}
static int rg_num(const char* s, float* out) {                 /* «1.7», «1,7», «-0.5» */
    double v = 0, f = 0.1; int neg = 0, dig = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; dig++; }
    if (*s == '.' || *s == ',') { s++; while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f /= 10; s++; dig++; } }
    if (!dig || *s) return 0;
    *out = (float)(neg ? -v : v);
    return 1;
}

/* ───────────── ресурсы Unity: классы и методы ───────────── */
/* база: нужны только функции IL2CPP API (для родной метки больше ничего не нужно) */
static int rg_resolve(void) { return rg_api(); }

/* ресурсы плоского режима (LineRenderer): тонкое кольцо на полу. alignment/loop в этой сборке игры вырезаны, поэтому только мировые координаты и ширина */
static int rg_flat_resolve(void) {
    void* img;
    if (RG.state) return RG.state > 0;
    if (!rg_api()) { RG.state = -1; return 0; }
    img = find_image_pre("UnityEngine.CoreModule");
    if (!img) { dlog("ring: нет образа UnityEngine.CoreModule"); RG.state = -1; return 0; }
    RG.kGo     = A.class_from_name(img, "UnityEngine", "GameObject");
    RG.kMat    = A.class_from_name(img, "UnityEngine", "Material");
    RG.kShader = A.class_from_name(img, "UnityEngine", "Shader");
    RG.kLr     = A.class_from_name(img, "UnityEngine", "LineRenderer");
    RG.kObj    = A.class_from_name(img, "UnityEngine", "Object");
    if (!RG.kGo || !RG.kMat || !RG.kShader || !RG.kLr || !RG.kObj) { dlog("ring: не нашлись классы UnityEngine (GameObject/Material/Shader/LineRenderer/Object)"); RG.state = -1; return 0; }
    RG.mGoCtor  = rg_find_m1(RG.kGo, ".ctor", "System.String");
    RG.mAdd     = rg_find_m1(RG.kGo, "AddComponent", "System.Type");
    RG.mFind    = find_method(RG.kShader, "Find", 1);
    RG.mMatCtor = rg_find_m1(RG.kMat, ".ctor", "UnityEngine.Shader");
    RG.mSetQ    = find_method(RG.kMat, "set_renderQueue", 1);
    RG.mSetMat  = find_method(RG.kLr, "set_material", 1);
    RG.mWidth   = find_method(RG.kLr, "set_widthMultiplier", 1);
    RG.mWorld   = find_method(RG.kLr, "set_useWorldSpace", 1);
    RG.mCount   = find_method(RG.kLr, "set_positionCount", 1);
    RG.mPos     = find_method(RG.kLr, "SetPosition", 2);
    RG.mCol0    = find_method(RG.kLr, "set_startColor", 1);
    RG.mCol1    = find_method(RG.kLr, "set_endColor", 1);
    RG.mEnable  = find_method(RG.kLr, "set_enabled", 1);
    RG.mDestroy = find_method(RG.kObj, "Destroy", 1);
    RG.typeLr   = RGA.type_object(RGA.cls_type(RG.kLr));
    {
        const char* miss = !RG.mGoCtor ? "GameObject..ctor(string)" : !RG.mAdd ? "GameObject.AddComponent(Type)" :
            !RG.mFind ? "Shader.Find" : !RG.mMatCtor ? "Material..ctor(Shader)" : !RG.mSetQ ? "Material.set_renderQueue" : !RG.mSetMat ? "Renderer.set_material" :
            !RG.mWidth ? "LineRenderer.set_widthMultiplier" : !RG.mWorld ? "LineRenderer.set_useWorldSpace" : !RG.mCount ? "LineRenderer.set_positionCount" :
            !RG.mPos ? "LineRenderer.SetPosition" : !RG.mCol0 ? "LineRenderer.set_startColor" : !RG.mCol1 ? "LineRenderer.set_endColor" :
            !RG.mEnable ? "Renderer.set_enabled" : !RG.mDestroy ? "Object.Destroy" : !RG.typeLr ? "typeof(LineRenderer)" : NULL;
        if (miss) { dlog2("ring: нет метода ", miss); RG.state = -1; return 0; }
    }
    RG.state = 1;
    dlog("ring: методы плоского режима найдены");
    return 1;
}

/* общий материал (создаётся один раз, пересоздаётся при смене /ring see) */
static void* rg_material(void) {
    void *mat, *shader = NULL, *args[1], *r = NULL;
    static const char* const namesFloor[] = { "Sprites/Default", "GUI/Text Shader", "Hidden/Internal-Colored", NULL };
    static const char* const namesTop[]   = { "GUI/Text Shader", "Sprites/Default", "Hidden/Internal-Colored", NULL };
    const char* const* names = g_rg_see ? namesTop : namesFloor;
    int i, q = RG_QUEUE;
    if (g_rg_hMat) { mat = RGA.gc_target(g_rg_hMat); if (mat && hm_obj_ok(mat)) return mat; RGA.gc_free(g_rg_hMat); g_rg_hMat = 0; }
    for (i = 0; names[i] && !shader; i++) {
        args[0] = A.string_new(names[i]); r = NULL;
        if (rg_call("Shader.Find", RG.mFind, NULL, args, &r) && r && hm_obj_ok(r)) { shader = r; dlog2("ring: шейдер ", names[i]); }
    }
    if (!shader) { dlog("ring: ни один шейдер не нашёлся (Shader.Find вернул null)"); return NULL; }
    mat = A.object_new(RG.kMat);
    if (!mat) return NULL;
    args[0] = shader;
    if (!rg_call("Material..ctor", RG.mMatCtor, mat, args, NULL)) return NULL;
    args[0] = &q;
    rg_call("Material.set_renderQueue", RG.mSetQ, mat, args, NULL);
    g_rg_hMat = RGA.gc_new(mat, 0);
    return mat;
}

static void* rg_lr(void) {
    void* lr = g_rg_hLr ? RGA.gc_target(g_rg_hLr) : NULL;
    return (lr && hm_obj_ok(lr)) ? lr : NULL;
}

static void rg_set_color(const float* c) {
    void *lr, *args[1]; float col[4];
    lr = rg_lr();
    if (!lr) return;
    col[0] = c[0]; col[1] = c[1]; col[2] = c[2]; col[3] = c[3];
    args[0] = col;
    rg_call("set_startColor", RG.mCol0, lr, args, NULL);
    rg_call("set_endColor", RG.mCol1, lr, args, NULL);
}

static void rg_enable(int on) {
    void *lr, *args[1]; u8 v = on ? 1 : 0;
    lr = rg_lr();
    if (!lr) return;
    args[0] = &v;
    rg_call("set_enabled", RG.mEnable, lr, args, NULL);
}

/* убрать объект зоны */
static void rg_destroy_all(void) {
    if (RG.state > 0 && g_rg_hGo) {
        void* go = RGA.gc_target(g_rg_hGo); void* args[1];
        if (go && hm_obj_ok(go)) { args[0] = go; rg_call("Object.Destroy", RG.mDestroy, NULL, args, NULL); }
    }
    if (g_rg_hGo) RGA.gc_free(g_rg_hGo);
    if (g_rg_hLr) RGA.gc_free(g_rg_hLr);
    g_rg_hGo = g_rg_hLr = 0;
    g_rg_zone = -1;
    g_rg_drawGy = 1e9f;
    g_rg_dirty = 1;
}

/* создать объект кольца: GameObject + LineRenderer в мировых координатах (alignment у LineRenderer в этой сборке вырезан — тонкая линия смотрит в камеру, для кольца на полу это нормально). 1 — получилось */
static int rg_make(void) {
    void *mat, *go, *lr = NULL, *args[2], *name; u8 world = 1; int cnt = RG_SEG + 1; float w = 0.16f; char nm[16];
    nm[0] = 'k'; nm[1] = 'o'; nm[2] = 'n'; nm[3] = 's'; nm[4] = 't'; nm[5] = '_'; nm[6] = 'z'; nm[7] = 'o'; nm[8] = 'n'; nm[9] = 'e'; nm[10] = 0;
    mat = rg_material();
    if (!mat) return 0;
    go = A.object_new(RG.kGo);
    if (!go) return 0;
    name = A.string_new(nm);
    args[0] = name;
    if (!rg_call("GameObject..ctor", RG.mGoCtor, go, args, NULL)) return 0;
    args[0] = RG.typeLr;
    if (!rg_call("AddComponent(LineRenderer)", RG.mAdd, go, args, &lr) || !lr || !hm_obj_ok(lr)) return 0;
    g_rg_hGo = RGA.gc_new(go, 0);
    g_rg_hLr = RGA.gc_new(lr, 0);
    args[0] = mat;     rg_call("Renderer.set_material", RG.mSetMat, lr, args, NULL);
    args[0] = &world;  rg_call("LineRenderer.set_useWorldSpace", RG.mWorld, lr, args, NULL);
    args[0] = &w;      rg_call("LineRenderer.set_widthMultiplier", RG.mWidth, lr, args, NULL);
    args[0] = &cnt;    rg_call("LineRenderer.set_positionCount", RG.mCount, lr, args, NULL);
    rg_set_color(g_rg_colOk);
    return 1;
}

/* Высота пола у центра: по ногам игрока, пока не знаем — на метр ниже точки пикапа (так считал сервер: Transform.y + 1 = точка пикапа) */
static float rg_floor_y(void) { return g_rg_gyOk ? g_rg_gy : g_rg_c[1] - 1.0f; }

/* Радиус на полу для трёхмерного расстояния R: sqrt(R² − dz²), dz = высота точки пикапа над «центром тела» игрока (пол + 1 м) */
static float rg_flat_r(float R) {
    double dz = (double)g_rg_c[1] - ((double)rg_floor_y() + 1.0);
    double v = (double)R * R - dz * dz;
    return v > 0.04 ? (float)__builtin_sqrt(v) : 0.2f;
}

/* окружность радиуса R вокруг центра на высоте пола (мировые координаты) */
static int rg_points(float R) {
    void *lr, *args[2]; int k, idx, ok = 1; float p[3], y = rg_floor_y() + RG_LIFT + g_rg_lift;
    double c, s, a = 6.283185307179586 / RG_SEG, x, z, nx;
    lr = rg_lr();
    if (!lr) return 0;
    c = 1.0 - a * a / 2 + a * a * a * a / 24 - a * a * a * a * a * a / 720;              /* cos и sin шага — без libm */
    s = a - a * a * a / 6 + a * a * a * a * a / 120 - a * a * a * a * a * a * a / 5040;
    x = R; z = 0;                                                                         /* поворачиваем вектор радиуса шаг за шагом */
    for (k = 0; k <= RG_SEG && ok; k++) {
        p[0] = g_rg_c[0] + (float)x; p[1] = y; p[2] = g_rg_c[2] + (float)z;
        idx = k; args[0] = &idx; args[1] = p;
        ok = rg_call("LineRenderer.SetPosition", RG.mPos, lr, args, NULL);
        nx = x * c - z * s; z = x * s + z * c; x = nx;
    }
    return ok;
}

/* объект зоны жив? */
static int rg_alive(void) {
    void* go = g_rg_hGo ? RGA.gc_target(g_rg_hGo) : NULL;
    return go && hm_obj_ok(go) && rg_lr();
}

/* ───────────── режим «как в игре»: родная метка Checkpoint.Race ─────────────
   У игры есть своя метка (Hassle.Entities.Checkpoint, префаб Assets/Prefabs/Checkpoint/Checkpoint.prefab; Checkpoint.Simple — «обычная», Checkpoint.Race — «гоночная»,
   SetRace(Vector3 position, float size) / HideRace(), сама прижимается к земле: PlaceOnGround). Берём гоночную (SetPlayerRaceCheckpoint сервер почти не использует):
   вызываем SetRace в центре пикапа, подгоняем размер под зону (размер измеряем по границам рендереров метки и один раз пересчитываем) и красим материалы метки:
   зелёный — вы в зоне, красный — слишком близко или за зоной. Если игра/сервер сам поставил гоночную метку в другом месте — мы её не трогаем и выключаемся.
   Если метку покрасить не получилось (нет свойства цвета у шейдера) или метки нет — рисуем свою плоскую полосу (режим flat). */
static struct {
    int state;                          /* 0 — не искали, 1 — есть, -1 — нет */
    void *kCp, *kRend, *kComp, *kMat, *mSet, *mHide, *mGetMat, *mGetBounds, *mGetComp, *mHas, *mSetCol, *mGetCol;
    void* typeRend;
    /* для своего материала метки (необязательные: нет — красим свойства родного) */
    void *kShader, *mFind, *mMatCtor, *mSetQ, *mGetShared, *mSetShared, *mGetTex, *mSetTex, *mGetTexNames, *mGetTexS;
    int skinOk;
} GM;
static int g_rg_flat;                   /* 1 — рисуем свою плоскую полосу вместо родной метки */
static int g_rg_gmSet;                  /* родную метку выставили мы */
static float g_rg_gmPos[3], g_rg_gmSize, g_rg_cal;      /* cal — диаметр метки в метрах на единицу size (0 — ещё не измерили) */
static int g_rg_calChecked, g_rg_gmTries, g_rg_gmLogged;

/* метод name с argc параметрами, у которого первый параметр типа tn0 (у HasProperty/SetColor/GetColor есть перегрузки со строкой и с int) */
static void* rg_find_t(void* k, const char* name, int argc, const char* tn0) {
    for (; k; k = A.class_get_parent(k)) {
        void* it = NULL; void* m;
        while ((m = RGA.cls_methods(k, &it)) != NULL) {
            const char* nm = RGA.m_name(m); const char* t;
            if (!nm || !seq(nm, name) || RGA.m_pcount(m) != argc) continue;
            t = A.type_get_name(RGA.m_param(m, 0));
            if (t && seq(t, tn0)) return m;
        }
    }
    return NULL;
}

static int rg_gm_resolve(void) {
    void* img;
    if (GM.state) return GM.state > 0;
    if (!g_safe || !g_img || !rg_api()) return 0;                       /* игра ещё не готова — не кэшируем отказ */
    GM.state = -1;
    img = find_image_pre("UnityEngine.CoreModule");
    GM.kCp = A.class_from_name(g_img, "Hassle.Entities", "Checkpoint");
    GM.kRend = img ? A.class_from_name(img, "UnityEngine", "Renderer") : NULL;
    GM.kComp = img ? A.class_from_name(img, "UnityEngine", "Component") : NULL;
    GM.kMat = img ? A.class_from_name(img, "UnityEngine", "Material") : NULL;
    if (!GM.kCp || !GM.kRend || !GM.kComp || !GM.kMat) { dlog("ring: нет класса Hassle.Entities.Checkpoint или UnityEngine.Renderer/Component"); return 0; }
    GM.mSet       = find_method(GM.kCp, "SetRace", 2);
    GM.mHide      = find_method(GM.kCp, "HideRace", 0);
    GM.mGetMat    = find_method(GM.kRend, "get_material", 0);
    GM.mGetBounds = find_method(GM.kRend, "get_bounds", 0);
    GM.mGetComp   = rg_find_t(GM.kComp, "GetComponent", 1, "System.Type");      /* GetComponentsInChildren(Type,bool) в этой сборке вырезан — обходим детей сами */
    GM.mHas       = rg_find_t(GM.kMat, "HasProperty", 1, "System.String");
    GM.mSetCol    = rg_find_t(GM.kMat, "SetColor", 2, "System.String");
    GM.mGetCol    = rg_find_t(GM.kMat, "GetColor", 1, "System.String");
    GM.typeRend   = RGA.type_object(RGA.cls_type(GM.kRend));
    {
        const char* miss = !GM.mSet ? "Checkpoint.SetRace" : !GM.mHide ? "Checkpoint.HideRace" : !GM.mGetMat ? "Renderer.get_material" : !GM.mGetBounds ? "Renderer.get_bounds" :
            !GM.mGetComp ? "Component.GetComponent(Type)" : !GM.mHas ? "Material.HasProperty(string)" : !GM.mSetCol ? "Material.SetColor(string,Color)" :
            !GM.mGetCol ? "Material.GetColor(string)" : !GM.typeRend ? "typeof(Renderer)" : NULL;
        if (miss) { dlog2("ring: для метки игры нет метода ", miss); return 0; }
    }
    GM.kShader     = img ? A.class_from_name(img, "UnityEngine", "Shader") : NULL;
    GM.mFind       = GM.kShader ? find_method(GM.kShader, "Find", 1) : NULL;
    GM.mMatCtor    = rg_find_m1(GM.kMat, ".ctor", "UnityEngine.Shader");
    GM.mSetQ       = find_method(GM.kMat, "set_renderQueue", 1);
    GM.mGetShared  = find_method(GM.kRend, "get_sharedMaterial", 0);
    GM.mSetShared  = find_method(GM.kRend, "set_sharedMaterial", 1);
    GM.mGetTex     = find_method(GM.kMat, "get_mainTexture", 0);
    GM.mSetTex     = find_method(GM.kMat, "set_mainTexture", 1);
    GM.mGetTexNames = find_method(GM.kMat, "GetTexturePropertyNames", 0);
    GM.mGetTexS    = rg_find_t(GM.kMat, "GetTexture", 1, "System.String");
    GM.skinOk = GM.mFind && GM.mMatCtor && GM.mSetQ && GM.mGetShared && GM.mSetShared;
    if (!GM.skinOk) dlog("ring: для своего материала метки не хватает методов Unity — буду красить свойства родного материала");
    GM.state = 1;
    dlog("ring: родная метка Checkpoint.Race найдена");
    return 1;
}

static void* rg_gm_cp(void) {                                              /* Checkpoint.Race (static) или NULL */
    void *f, *obj = NULL;
    if (GM.state <= 0) return NULL;
    f = A.class_get_field_from_name(GM.kCp, "Race");
    if (!f) return NULL;
    A.field_static_get_value(f, &obj);
    return (obj && hm_obj_ok(obj)) ? obj : NULL;
}

/* рендереры метки: обходим Transform метки и всех детей (глубина до 5, до 16 детей на узел), у каждого берём GetComponent(Renderer).
   Префаб грузится асинхронно — сразу после SetRace детей может ещё не быть. */
static void rg_gm_walk(void* tr, int depth, void** out, int* n, int max) {
    void *r = NULL, *m, *args[1]; int cnt, i;
    if (!tr || !hm_obj_ok(tr) || *n >= max) return;
    args[0] = GM.typeRend;
    if (rg_call("Component.GetComponent", GM.mGetComp, tr, args, &r) && r && hm_obj_ok(r)) out[(*n)++] = r;
    if (depth >= 5) return;
    m = find_method(A.object_get_class(tr), "get_childCount", 0);
    r = NULL;
    if (!m || !rg_call("get_childCount", m, tr, NULL, &r) || !r || !hm_readable((char*)r + HDR, 4)) return;
    cnt = *(int*)((char*)r + HDR);
    if (cnt < 1 || cnt > 16) return;
    m = find_method(A.object_get_class(tr), "GetChild", 1);
    if (!m) return;
    for (i = 0; i < cnt && *n < max; i++) {
        void* ch = NULL; int idx = i;
        args[0] = &idx;
        if (rg_call("GetChild", m, tr, args, &ch) && ch) rg_gm_walk(ch, depth + 1, out, n, max);
    }
}
static int rg_gm_rends(void* cp, void** out, int max) {
    int n = 0;
    rg_gm_walk(rg_obj(cp, "get_transform"), 0, out, &n, max);
    return n;
}

/* горизонтальный диаметр метки по границам рендереров, м; 0 — не измерилось */
static float rg_gm_width(void) {
    void *cp = rg_gm_cp(), *rs[16]; int n, i; float best = 0.0f;
    if (!cp) return 0.0f;
    n = rg_gm_rends(cp, rs, 16);
    for (i = 0; i < n; i++) {
        void* r = NULL; const float* b; float w;
        if (!rg_call("Renderer.get_bounds", GM.mGetBounds, rs[i], NULL, &r) || !r || !hm_readable((char*)r + HDR, 24)) continue;
        b = (const float*)((char*)r + HDR);                                 /* Bounds: центр xyz, половинные размеры xyz */
        if (!rg_finite3(b + 3)) continue;
        w = (b[3] > b[5] ? b[3] : b[5]) * 2.0f;
        if (w > best) best = w;
    }
    return best;
}

/* один раз пишем в журнал шейдер и свойства материала метки — по ним видно, чем её красить */
static void rg_gm_log_shader(void* mat) {
    void *sh, *nm = NULL, *m, *args[1]; char tmp[96], line[220]; Buf b; int n = 0, i;
    if (g_rg_gmLogged) return;
    g_rg_gmLogged = 1;
    sh = rg_obj(mat, "get_shader");
    if (!sh) return;
    m = find_method(A.object_get_class(sh), "get_name", 0);
    if (m && rg_call("Shader.get_name", m, sh, NULL, &nm) && nm && hm_obj_ok(nm)) { utf16_to_utf8(A.string_chars(nm), A.string_length(nm), tmp, sizeof tmp); dlog2("ring: шейдер метки: ", tmp); }
    m = find_method(A.object_get_class(sh), "GetPropertyCount", 0);
    if (m) { void* r = NULL; if (rg_call("GetPropertyCount", m, sh, NULL, &r) && r && hm_readable((char*)r + HDR, 4)) n = *(int*)((char*)r + HDR); }
    m = find_method(A.object_get_class(sh), "GetPropertyName", 1);
    if (!m || n <= 0) return;
    b_init(&b, line, sizeof line); b_s(&b, "ring: свойства шейдера:");
    for (i = 0; i < n && i < 14; i++) {
        void* r = NULL; args[0] = &i;
        if (rg_call("GetPropertyName", m, sh, args, &r) && r && hm_obj_ok(r)) { utf16_to_utf8(A.string_chars(r), A.string_length(r), tmp, sizeof tmp); b_ch(&b, ' '); b_s(&b, tmp); }
    }
    dlog(line);
}

/* ЗАПАСНОЙ способ (/ring mat 0): покрасить свойства родных материалов метки: good=1 — зелёный, 0 — красный. rgb меняем, у обычных цветов альфа = /ring a, яркость свечения как в игре.
   У шейдера Hassle/Checkpoint это на экране ничего не меняло (по журналу свойства «красились», а метка оставалась прежней), поэтому по умолчанию работает rg_gm_skin ниже. Возвращает, сколько свойств покрашено */
static int rg_gm_tint(int good) {
    static const char* const names[] = { "_Color", "_TintColor", "_BaseColor", "_MainColor", "_EmissionColor", "_RimColor", "_FresnelColor", NULL };
    void *cp = rg_gm_cp(), *rs[16]; int n, i, k, cnt = 0;
    if (!cp) return 0;
    n = rg_gm_rends(cp, rs, 16);
    for (i = 0; i < n; i++) {
        void* mat = NULL;
        if (!rg_call("Renderer.get_material", GM.mGetMat, rs[i], NULL, &mat) || !mat || !hm_obj_ok(mat)) continue;
        rg_gm_log_shader(mat);
        for (k = 0; names[k]; k++) {
            void *nm = A.string_new(names[k]), *a[2], *r = NULL; float col[4], mx, mul = 1.0f; int glow = k >= 4;
            a[0] = nm;
            if (!rg_call("Material.HasProperty", GM.mHas, mat, a, &r) || !r || !hm_readable((char*)r + HDR, 1) || !*((u8*)r + HDR)) continue;
            col[0] = col[1] = col[2] = col[3] = 1.0f; r = NULL; a[0] = nm;
            if (rg_call("Material.GetColor", GM.mGetCol, mat, a, &r) && r && hm_readable((char*)r + HDR, 16)) { const float* f = (const float*)((char*)r + HDR); col[0] = f[0]; col[1] = f[1]; col[2] = f[2]; col[3] = f[3]; }
            mx = col[0] > col[1] ? col[0] : col[1]; if (col[2] > mx) mx = col[2];
            if (glow) { if (mx < 0.02f) continue; if (mx > 1.0f) mul = mx; }     /* у свечения чёрный = выключено, яркость сохраняем */
            col[0] = (good ? 0.15f : 1.00f) * mul; col[1] = (good ? 0.95f : 0.12f) * mul; col[2] = (good ? 0.30f : 0.12f) * mul;
            if (!glow) col[3] = g_rg_alpha;                                    /* прозрачность по /ring a (у свечения альфу не трогаем) */
            a[0] = nm; a[1] = col;
            if (rg_call("Material.SetColor", GM.mSetCol, mat, a, NULL)) cnt++;
        }
    }
    return cnt;
}

/* ───────── свой полупрозрачный материал на рендерерах метки ─────────
   Шейдер метки (Hassle/Checkpoint) на SetColor по известным именам свойств не реагирует, а свойства шейдера в этой сборке прочитать нельзя (GetPropertyCount вырезан).
   Поэтому надёжный путь: на каждый рендерер метки вешаем СВОЙ материал (Sprites/Default: альфа-смешивание, без записи глубины, глубину проверяет — стены метку перекрывают),
   берём у родного материала текстуру (если есть) и красим _Color = (r, g, b, /ring a). Сама геометрия метки (меш), размер и положение остаются игровыми (SetRace/PlaceOnGround).
   Родной материал каждого рендерера запоминаем — /ring mat 0 его возвращает. */
#define RG_MAXR 16
static unsigned g_rg_skinH[RG_MAXR];            /* наши материалы по номеру рендерера */
static unsigned g_rg_origH[RG_MAXR];            /* родные материалы (чтобы вернуть) */
static int g_rg_skinLogged, g_rg_skinSaid;

/* один раз по каждому рендереру пишем в журнал: класс, какие свойства цвета есть у материала, какие текстуры — по этому видно, чем красить, если что-то не так */
static void rg_gm_log_props(void* mat, void* rend, int idx) {
    static const char* const names[] = { "_Color", "_TintColor", "_BaseColor", "_MainColor", "_EmissionColor", "_RimColor", "_FresnelColor", "_Tint", "_ColorA", "_ColorB",
                                         "_InnerColor", "_OuterColor", "_GlowColor", "_EdgeColor", "_FillColor", "_Alpha", "_Opacity", "_Intensity", "_Cutoff", NULL };
    char line[360], tmp[96]; Buf b; int k; void* arr = NULL;
    if (idx < 0 || idx >= 8 || (g_rg_skinLogged & (1 << idx)) || !mat || !hm_obj_ok(mat)) return;
    g_rg_skinLogged |= 1 << idx;
    rg_gm_log_shader(mat);
    b_init(&b, line, sizeof line);
    b_s(&b, "ring: рендерер "); b_int(&b, idx); b_s(&b, " ("); b_s(&b, A.class_get_name(A.object_get_class(rend))); b_s(&b, "), свойства:");
    for (k = 0; names[k]; k++) {
        void *a[1], *r = NULL;
        a[0] = A.string_new(names[k]);
        if (rg_call("Material.HasProperty", GM.mHas, mat, a, &r) && r && hm_readable((char*)r + HDR, 1) && *((u8*)r + HDR)) { b_ch(&b, ' '); b_s(&b, names[k]); }
    }
    b_s(&b, "; текстуры:");
    if (GM.mGetTexNames && rg_call("Material.GetTexturePropertyNames", GM.mGetTexNames, mat, NULL, &arr) && arr && hm_obj_ok(arr)) {
        long len = *(long*)((char*)arr + 3 * sizeof(void*)); long i;
        if (len > 0 && len <= 8) {
            for (i = 0; i < len; i++) {
                void* s = *(void**)((char*)arr + 4 * sizeof(void*) + (uptr)i * sizeof(void*)); int l2;
                if (!s) continue;
                l2 = A.string_length(s);
                if (l2 <= 0 || l2 > 60) continue;
                utf16_to_utf8(A.string_chars(s), l2, tmp, sizeof tmp);
                b_ch(&b, ' '); b_s(&b, tmp);
            }
        } else b_s(&b, " нет");
    } else b_s(&b, " нет");
    dlog(line);
}

/* текстура родного материала: сначала _MainTex, иначе первая непустая из GetTexturePropertyNames */
static void* rg_gm_find_tex(void* src) {
    void *r = NULL, *arr = NULL; long len, i;
    if (!src || !hm_obj_ok(src)) return NULL;
    if (GM.mGetTex && rg_call("Material.get_mainTexture", GM.mGetTex, src, NULL, &r) && r && hm_obj_ok(r)) return r;
    if (!GM.mGetTexNames || !GM.mGetTexS) return NULL;
    if (!rg_call("Material.GetTexturePropertyNames", GM.mGetTexNames, src, NULL, &arr) || !arr || !hm_obj_ok(arr)) return NULL;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 1 || len > 8) return NULL;
    for (i = 0; i < len; i++) {
        void* s = *(void**)((char*)arr + 4 * sizeof(void*) + (uptr)i * sizeof(void*)); void *a[1], *t = NULL;
        if (!s) continue;
        a[0] = s;
        if (rg_call("Material.GetTexture", GM.mGetTexS, src, a, &t) && t && hm_obj_ok(t)) return t;
    }
    return NULL;
}

/* новый материал: Sprites/Default (запасные UI/Default и GUI/Text Shader), очередь отрисовки 3005, текстура — у родного материала src (режим /ring mat 1) */
static void* rg_skin_make(void* src) {
    static const char* const names[] = { "Sprites/Default", "UI/Default", "GUI/Text Shader", NULL };
    void *shader = NULL, *mat, *a[1], *r; int i, q = 3005;
    for (i = 0; names[i] && !shader; i++) {
        a[0] = A.string_new(names[i]); r = NULL;
        if (rg_call("Shader.Find", GM.mFind, NULL, a, &r) && r && hm_obj_ok(r)) {
            shader = r;
            if (!g_rg_skinSaid) { g_rg_skinSaid = 1; dlog2("ring: свой материал метки, шейдер ", names[i]); }
        }
    }
    if (!shader) { if (!g_rg_skinSaid) { g_rg_skinSaid = 1; dlog("ring: ни один шейдер для своего материала метки не нашёлся (Shader.Find вернул null)"); } return NULL; }
    mat = A.object_new(GM.kMat);
    if (!mat) return NULL;
    a[0] = shader;
    if (!rg_call("Material..ctor", GM.mMatCtor, mat, a, NULL)) return NULL;
    a[0] = &q; rg_call("Material.set_renderQueue", GM.mSetQ, mat, a, NULL);
    if (g_rg_skin == 1 && GM.mSetTex) {
        void* tex = rg_gm_find_tex(src);
        if (tex) { a[0] = tex; rg_call("Material.set_mainTexture", GM.mSetTex, mat, a, NULL); }
    }
    return mat;
}

/* повесить свой материал на все рендереры метки и покрасить: good=1 — зелёный, 0 — красный, прозрачность g_rg_alpha. Возвращает, сколько рендереров покрашено */
static int rg_gm_skin(int good) {
    void *cp = rg_gm_cp(), *rs[RG_MAXR]; int n, i, cnt = 0; float col[4];
    if (!cp || !GM.skinOk) return 0;
    n = rg_gm_rends(cp, rs, RG_MAXR);
    col[0] = good ? 0.15f : 1.00f; col[1] = good ? 0.95f : 0.12f; col[2] = good ? 0.30f : 0.12f; col[3] = g_rg_alpha;
    for (i = 0; i < n; i++) {
        void *cur = NULL, *mine = g_rg_skinH[i] ? RGA.gc_target(g_rg_skinH[i]) : NULL, *a[2], *nm;
        if (!rg_call("Renderer.get_sharedMaterial", GM.mGetShared, rs[i], NULL, &cur)) continue;
        if (cur && !hm_obj_ok(cur)) cur = NULL;
        if (!(mine && hm_obj_ok(mine) && cur == mine)) {                    /* на рендерере ещё не наш материал (или игра подменила) */
            void* orig = g_rg_origH[i] ? RGA.gc_target(g_rg_origH[i]) : NULL;
            if (cur && !(mine && cur == mine)) {                            /* запомнить родной, если это не наш прежний */
                if (g_rg_origH[i]) RGA.gc_free(g_rg_origH[i]);
                g_rg_origH[i] = RGA.gc_new(cur, 0); orig = cur;
            }
            rg_gm_log_props(orig ? orig : cur, rs[i], i);
            if (g_rg_skinH[i]) { RGA.gc_free(g_rg_skinH[i]); g_rg_skinH[i] = 0; }
            mine = rg_skin_make(orig ? orig : cur);
            if (!mine) continue;
            g_rg_skinH[i] = RGA.gc_new(mine, 0);
            a[0] = mine;
            if (!rg_call("Renderer.set_sharedMaterial", GM.mSetShared, rs[i], a, NULL)) continue;
        }
        nm = A.string_new("_Color");
        a[0] = nm; a[1] = col;
        if (rg_call("Material.SetColor", GM.mSetCol, mine, a, NULL)) cnt++;
    }
    return cnt;
}

/* вернуть родные материалы и забыть свои (/ring mat 0 и смена 1 ↔ 2) */
static void rg_gm_unskin(int restore) {
    int i;
    if (restore && GM.state > 0 && GM.skinOk) {
        void *cp = rg_gm_cp(), *rs[RG_MAXR];
        if (cp) {
            int n = rg_gm_rends(cp, rs, RG_MAXR);
            for (i = 0; i < n; i++) {
                void *orig = g_rg_origH[i] ? RGA.gc_target(g_rg_origH[i]) : NULL, *a[1];
                if (!orig || !hm_obj_ok(orig)) continue;
                a[0] = orig; rg_call("Renderer.set_sharedMaterial", GM.mSetShared, rs[i], a, NULL);
            }
        }
    }
    for (i = 0; i < RG_MAXR; i++) if (g_rg_skinH[i]) { RGA.gc_free(g_rg_skinH[i]); g_rg_skinH[i] = 0; }
}

static void rg_gm_hide(void) {
    if (g_rg_gmSet && GM.state > 0) rg_call("Checkpoint.HideRace", GM.mHide, NULL, NULL, NULL);
    g_rg_gmSet = 0;
}

/* переключиться на свою плоскую полосу */
static void rg_use_flat(const char* why) {
    rg_gm_hide();
    g_rg_flat = 1; g_rg_dirty = 1; g_rg_zone = -1; g_rg_fails = 0;
    dlog2("ring: режим flat: ", why);
    chat_say("Зона: родная метка игры не подошла — рисую своё кольцо на полу");
}

/* выставить родную метку в центре, размер — под зону */
static int rg_draw_game(void) {
    float D, size, p[3]; void* args[2];
    if (!rg_gm_resolve()) { if (GM.state < 0) rg_use_flat("нет методов Checkpoint"); return 0; }
    D = 2.0f * rg_flat_r(g_rg_rOut);
    size = g_rg_cal > 0.0f ? D / g_rg_cal : D * 0.5f;                       /* пока не измерили — как в SA-MP: size — радиус */
    p[0] = g_rg_c[0]; p[1] = g_rg_c[1]; p[2] = g_rg_c[2];
    args[0] = p; args[1] = &size;
    if (!rg_call("Checkpoint.SetRace", GM.mSet, NULL, args, NULL)) return 0;
    g_rg_gmSet = 1; g_rg_gmSize = size; g_rg_gmPos[0] = p[0]; g_rg_gmPos[1] = p[1]; g_rg_gmPos[2] = p[2];
    g_rg_zone = -1; g_rg_dirty = 0; g_rg_fails = 0; g_rg_drawGy = rg_floor_y();
    { Buf b; char lg[120]; b_init(&b, lg, sizeof lg); b_s(&b, "ring: SetRace размер "); b_fix(&b, size, 2); b_s(&b, ", нужный диаметр "); b_fix(&b, D, 2); b_s(&b, " м"); dlog(lg); }
    return 1;
}

/* после рисования: метка на месте? подгоняем размер; следим, чтобы её не перехватили */
static void rg_gm_service(void) {
    void *cp, *tr; float p[3], dx, dz;
    if (g_rg_flat || !g_rg_gmSet) return;
    cp = rg_gm_cp();
    if (!cp) { if (++g_rg_gmTries > 24) rg_use_flat("метка Checkpoint.Race не появилась"); return; }
    tr = rg_obj(cp, "get_transform");
    if (tr && rg_vec(tr, "get_position", p)) {
        dx = p[0] - g_rg_gmPos[0]; dz = p[2] - g_rg_gmPos[2];
        if (dx * dx + dz * dz > 0.25f) {                                    /* гоночную метку переставил сервер — не воюем */
            g_rg_on = 0; g_rg_gmSet = 0;
            chat_say("Зона: гоночную метку заняла игра или сервер — зона выключена. /ring on вернёт её, /ring flat — своя зона на полу");
            dlog("ring: Checkpoint.Race передвинули не мы — выключаюсь");
            return;
        }
    }
    if (g_rg_cal <= 0.0f) {
        float w = rg_gm_width();
        if (w > 0.05f && w < 500.0f && g_rg_gmSize > 0.01f) { g_rg_cal = w / g_rg_gmSize; g_rg_dirty = 1; { Buf b; char lg[100]; b_init(&b, lg, sizeof lg); b_s(&b, "ring: метка: при size "); b_fix(&b, g_rg_gmSize, 2); b_s(&b, " диаметр "); b_fix(&b, w, 2); b_s(&b, " м"); dlog(lg); } }
    } else if (!g_rg_calChecked && !g_rg_dirty) {
        float w = rg_gm_width();
        if (w > 0.05f) { Buf b; char lg[100]; g_rg_calChecked = 1; b_init(&b, lg, sizeof lg); b_s(&b, "ring: метка после подгонки: диаметр "); b_fix(&b, w, 2); b_s(&b, " м"); dlog(lg); }
    }
}

/* перерисовать зону (после смены центра/радиусов/пола). 1 — получилось. Любое исключение (например, объект уничтожен при смене локации) —
   всё убираем и в следующий тик создаём заново; 5 неудач подряд — выключаемся. */
static int rg_draw(void) {
    int ok = 1;
    if (!g_rg_flat) {
        ok = rg_draw_game();
        if (!ok && !g_rg_flat && ++g_rg_fails >= 3) rg_use_flat("SetRace не сработал 3 раза подряд");
        return ok;
    }
    if (!rg_flat_resolve()) { g_rg_on = 0; chat_say("Зона: не нашлись методы Unity для плоской зоны, подробности в HLOG/konstt.log"); return 0; }
    if (!rg_alive()) {
        rg_destroy_all();
        ok = rg_make();
    }
    if (ok) ok = rg_points(rg_flat_r(g_rg_rOut));
    if (ok) { rg_enable(1); g_rg_dirty = 0; g_rg_fails = 0; g_rg_drawGy = rg_floor_y(); }
    else { g_rg_fails++; rg_destroy_all(); }
    return ok;
}

/* ───────────── поиск раздевалки среди пикапов ───────────── */
static struct { float me[3]; int meOk; int found; int id; float p[3]; float d; } RS;

/* в имени модели (дочерний объект пикапа) есть «cloth» без учёта регистра? */
static int rg_is_cloth(void* pk) {
    void *tr, *m, *r = NULL, *ch, *nm; int n = 0, i, len, j; const u16* chs; char tmp[96];
    tr = rg_obj(pk, "get_transform");
    if (!tr) return 0;
    m = find_method(A.object_get_class(tr), "get_childCount", 0);
    if (m && rg_call("get_childCount", m, tr, NULL, &r) && r && hm_readable((char*)r + HDR, 4)) n = *(int*)((char*)r + HDR);
    if (n < 1 || n > 8) return 0;
    m = find_method(A.object_get_class(tr), "GetChild", 1);
    if (!m) return 0;
    for (i = 0; i < n; i++) {
        void* args[1]; int idx = i; void* gm; void* rr = NULL;
        args[0] = &idx;
        if (!rg_call("GetChild", m, tr, args, &rr) || !rr || !hm_obj_ok(rr)) continue;
        ch = rr;
        gm = find_method(A.object_get_class(ch), "get_name", 0);
        if (!gm) continue;
        nm = NULL;
        if (!rg_call("get_name", gm, ch, NULL, &nm) || !nm || !hm_obj_ok(nm)) continue;
        len = A.string_length(nm); chs = A.string_chars(nm);
        if (len <= 0 || len > 80 || !chs) continue;
        utf16_to_utf8(chs, len, tmp, sizeof tmp);
        for (j = 0; tmp[j]; j++) {
            if ((tmp[j] | 32) == 'c' && (tmp[j + 1] | 32) == 'l' && (tmp[j + 2] | 32) == 'o' && (tmp[j + 3] | 32) == 't' && (tmp[j + 4] | 32) == 'h') return 1;
        }
    }
    return 0;
}

static void rg_pk_cb(void* it, void* ctx) {
    float p[3]; void* tr; float d = 0;
    (void)ctx;
    if (!hm_obj_ok(it)) return;
    tr = rg_obj(it, "get_transform");
    if (!tr || !rg_vec(tr, "get_position", p)) return;
    if (RS.meOk) { d = rg_d3(p, RS.me); if (d > RG_SCAN_M) return; }
    if (RS.found && d >= RS.d) return;                         /* имя читаем только у тех, кто ближе уже найденного */
    if (!g_rg_any && !rg_is_cloth(it)) return;
    RS.found = 1; RS.d = d; RS.id = (int)num_field(it, "m_ID", NULL);
    RS.p[0] = p[0]; RS.p[1] = p[1]; RS.p[2] = p[2];
}

static void* rg_pool(const char* field) {
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}

/* перечитать пикапы и выбрать центр. Главный поток. */
static void rg_scan(long now) {
    void *pool, *dict, *arr, *lp; int total, had = g_rg_have; float old[3];
    old[0] = g_rg_c[0]; old[1] = g_rg_c[1]; old[2] = g_rg_c[2];
    g_rg_scanT = now;
    RS.found = 0; RS.d = 0; RS.meOk = 0;
    lp = local_player();
    if (lp && hm_obj_ok(lp) && read_pos(lp, RS.me)) RS.meOk = 1;
    pool = rg_pool("Pickups");
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (arr) { total = int_field(dict, "_count", "count", NULL, -1); walk_struct_array(arr, total, "value", rg_pk_cb, NULL); }
    if (!RS.found) {
        g_rg_have = 0;
        if (had) { rg_enable(0); rg_gm_hide(); g_rg_dirty = 1; dlog("ring: раздевалка пропала из зоны стрима — зона скрыта"); }
        return;
    }
    g_rg_have = 1; g_rg_id = RS.id;
    g_rg_c[0] = RS.p[0]; g_rg_c[1] = RS.p[1]; g_rg_c[2] = RS.p[2];
    g_rg_notFoundSaid = 0;
    if (!had || rg_d3(old, g_rg_c) > 0.05f) {
        Buf b; char msg[200];
        g_rg_dirty = 1; g_rg_gyOk = 0; g_rg_zone = -1;
        b_init(&b, msg, sizeof msg);
        b_s(&b, g_rg_any ? "Зона: центр — пикап ID " : "Зона у раздевалки: пикап ID "); b_int(&b, g_rg_id);
        b_s(&b, ". Стойте в ЗЕЛЁНОЙ зоне (");  b_fix(&b, g_rg_rIn, 1); b_s(&b, "–"); b_fix(&b, g_rg_rOut, 1); b_s(&b, " м от пикапа), красная — слишком близко или далеко");
        chat_say(msg);
        dlog2("ring: ", msg);
    }
}

/* цвет средней окружности по реальному расстоянию до центра: зелёная — рабочая зона */

/* ───────────── пол и зона ───────────── */
static float rg_abs(float v) { return v < 0 ? -v : v; }

/* 1 — на земле, 0 — в воздухе, -1 — не прочиталось (LocalPlayer.m_Controller.Movement.isOnGround, как в /tpl) */
static int rg_on_ground(void* lp) {
    void *ctrl = ref_field_s(lp, "m_Controller"), *mv = ctrl ? ref_field_s(ctrl, "Movement") : NULL, *m, *r = NULL;
    if (!mv || !hm_obj_ok(mv)) return -1;
    m = find_method(A.object_get_class(mv), "get_isOnGround", 0);
    if (!m || !rg_call("get_isOnGround", m, mv, NULL, &r) || !r || !hm_readable((char*)r + HDR, 1)) return -1;
    return *((u8*)r + HDR) ? 1 : 0;
}

/* Пол у раздевалки = Transform.y ног игрока (в пакете Transform.y + 1), пока он стоит на земле недалеко от центра. В прыжке и на другом этаже не обновляем. */
static void rg_ground_update(void* lp, const float* me) {
    float f[3], y, hx, hz; void* tr; int gr;
    tr = rg_obj(lp, "get_transform");
    if (tr && rg_vec(tr, "get_position", f)) { y = f[1]; hx = f[0] - g_rg_c[0]; hz = f[2] - g_rg_c[2]; }
    else { y = me[1] - 1.0f; hx = me[0] - g_rg_c[0]; hz = me[2] - g_rg_c[2]; }
    if (hx * hx + hz * hz > RG_GROUND_R * RG_GROUND_R) return;
    if (rg_abs(y - (g_rg_c[1] - 1.0f)) > 4.0f) return;                      /* другой этаж / телепорт */
    gr = rg_on_ground(lp);
    if (gr == 0) return;                                                    /* прыжок или падение */
    if (g_rg_gyOk && rg_abs(y - g_rg_gy) > (gr == 1 ? 2.5f : 0.12f)) return;
    if (!g_rg_gyOk) {
        Buf b; char msg[140];
        b_init(&b, msg, sizeof msg); b_s(&b, "ring: пол по ногам игрока y="); b_fix(&b, y, 2); b_s(&b, ", пикап y="); b_fix(&b, g_rg_c[1], 2); b_s(&b, " (над полом на "); b_fix(&b, g_rg_c[1] - y, 2); b_s(&b, " м)");
        dlog(msg);
    }
    g_rg_gy = y; g_rg_gyOk = 1;
    if (rg_abs(rg_floor_y() - g_rg_drawGy) > 0.04f) g_rg_dirty = 1;
}

/* замер: расстояние до центра (трёхмерное, по позиции из пакета синхронизации — как считает сервер) и пол */
static void rg_sample(void) {
    float me[3]; void* lp;
    lp = local_player();
    if (!g_rg_have || !lp || !hm_obj_ok(lp) || !read_pos(lp, me)) return;
    g_rg_dist = rg_d3(me, g_rg_c);
    rg_ground_update(lp, me);
}

/* цвет зоны: зелёная — вы в зоне, красная — слишком близко или за зоной */
static void rg_zone_color(void) {
    int z;
    if (!g_rg_have || g_rg_dist < 0.0f) return;
    z = g_rg_dist < g_rg_rIn ? 0 : g_rg_dist > g_rg_rOut ? 2 : 1;
    if (z == g_rg_zone) return;
    if (!g_rg_flat) {
        int painted = 0;
        if (!g_rg_gmSet) return;
        if (g_rg_skin) painted = rg_gm_skin(z == 1) > 0;                    /* своя полупрозрачная заливка */
        if (!painted) painted = rg_gm_tint(z == 1) > 0;                     /* запасной путь: свойства родного материала */
        if (painted) { g_rg_zone = z; g_rg_gmTries = 0; }
        else {
            if (++g_rg_gmTries > 24) rg_use_flat("цвет метки не поменять: у её шейдера нет свойства цвета (см. «шейдер метки» в логе)");
            return;
        }
    } else {
        if (!rg_alive()) return;
        g_rg_zone = z;
        rg_set_color(z == 1 ? g_rg_colOk : g_rg_colBad);
    }
    dlog2("ring: зона: ", z == 0 ? "слишком близко (красная)" : z == 2 ? "за зоной (красная)" : "в зоне (зелёная)");
}

/* ───────────── тик в главном потоке ───────────── */
static void rg_tick(void) {
    long now;
    if (!g_rg_on || !rg_resolve()) return;
    now = time(NULL);
    if (g_rg_have ? (now - g_rg_scanT >= RG_RESCAN_S) : (now - g_rg_scanT >= 1)) rg_scan(now);       /* нашли — перечитываем раз в 3 с; не нашли — раз в секунду */
    if (!g_rg_have) {
        if (!g_rg_notFoundSaid) {
            g_rg_notFoundSaid = 1;
            chat_say(g_rg_any ? "Кольцо: пикапов рядом нет (дальше 60 м). Подойдите ближе." : "Кольцо: раздевалка (модель «cloth») рядом не найдена. Подойдите ближе или /ring any — центром станет ближайший пикап любой модели.");
        }
        return;
    }
    rg_sample();
    if (g_rg_dirty && !rg_draw()) {
        if (g_rg_fails >= 5) { g_rg_on = 0; chat_say("Кольцо: не получилось нарисовать 5 раз подряд — выключено. Подробности в HLOG/konstt.log"); dlog("ring: 5 неудач подряд, выключено"); }
        return;
    }
    rg_gm_service();
    if (!g_rg_on) return;
    rg_zone_color();
}

/* ───────────── команда ───────────── */
static int rg_tokens(void* arr, char tok[][24], int maxn) {
    long len; int i, n = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len && n < maxn; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[64]; int len2, k; const u16* ch;
        if (!s) continue;
        len2 = A.string_length(s); ch = A.string_chars(s);
        if (len2 <= 0 || len2 > 40 || !ch) continue;
        utf16_to_utf8(ch, len2, tmp, sizeof tmp);
        for (k = 0; tmp[k] && k < 23; k++) tok[n][k] = (tmp[k] >= 'A' && tmp[k] <= 'Z') ? (char)(tmp[k] + 32) : tmp[k];
        tok[n][k] = 0;
        if (seq(tok[n], "ring") || seq(tok[n], "/ring")) continue;                    /* сама команда в массиве — пропускаем */
        n++;
    }
    return n;
}


static void rg_info(void) {
    Buf b; char msg[360];
    b_init(&b, msg, sizeof msg);
    if (!g_rg_on) { chat_say("Зона выключена. /ring on — включить"); return; }
    if (!g_rg_have) { chat_say("Зона: центр не найден (раздевалка не в зоне стрима). /ring scan — искать заново, /ring any — любой пикап"); return; }
    b_s(&b, "Зона: пикап ID "); b_int(&b, g_rg_id); b_s(&b, ", вы в "); b_fix(&b, g_rg_dist >= 0 ? g_rg_dist : 0, 1); b_s(&b, " м — ");
    b_s(&b, g_rg_zone == 0 ? "СЛИШКОМ БЛИЗКО (ближе 1,5 м вход не сработает)" : g_rg_zone == 2 ? "ЗА ЗОНОЙ (дальше 4 м диалог не примет)" : g_rg_zone == 1 ? "в зоне, всё хорошо" : "расстояние не определилось");
    b_s(&b, ". Зона "); b_fix(&b, g_rg_rIn, 1); b_s(&b, "–"); b_fix(&b, g_rg_rOut, 1); b_s(&b, " м, пол "); b_s(&b, g_rg_gyOk ? "по ногам" : "пока по пикапу"); b_s(&b, ", подъём "); b_fix(&b, g_rg_lift, 2);
    b_s(&b, " м, "); b_s(&b, !g_rg_flat ? "метка игры" : g_rg_see ? "плоская, поверх всего" : "плоская, на полу"); b_s(&b, g_rg_any ? ", центр: любой пикап" : ", центр: раздевалка");
    if (!g_rg_flat) { b_s(&b, ", прозрачность "); b_fix(&b, g_rg_alpha, 2); b_s(&b, g_rg_skin == 1 ? ", свой материал с текстурой" : g_rg_skin == 2 ? ", свой материал без текстуры" : ", родной материал"); }
    chat_say(msg);
}

static void native_ring_cmd(void* self, void* args, void* mi) {
    char tok[6][24]; int n; (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { rg_tick(); g_rg_pending = 0; return; }   /* вызов из Post, не из чата */
    n = rg_tokens(args, tok, 6);
    if (n == 0 || seq(tok[0], "on") || seq(tok[0], "1") || seq(tok[0], "off") || seq(tok[0], "0")) {
        int want = n == 0 ? !g_rg_on : (seq(tok[0], "on") || seq(tok[0], "1"));
        if (want) {
            if (!rg_resolve()) { chat_say("Кольцо: не нашлись методы Unity, подробности в HLOG/konstt.log"); return; }
            mt_capture_ctx();                                   /* главный поток: берём SynchronizationContext для тиков */
            g_rg_on = 1; g_rg_notFoundSaid = 0; g_rg_have = 0; g_rg_dirty = 1; g_rg_scanT = 0; g_rg_fails = 0; g_rg_gyOk = 0; g_rg_gmTries = 0; g_rg_gmSet = 0;
            if (!g_mt_ctx) chat_say("Кольцо: нет главного потока для автообновления — рисую один раз, повторный /ring on обновит");
            rg_tick();
        } else {
            g_rg_on = 0;
            rg_destroy_all();
            rg_gm_hide();
            chat_say("Зона выключена");
        }
        return;
    }
    if (seq(tok[0], "info") || seq(tok[0], "status")) { rg_info(); return; }
    if (seq(tok[0], "scan")) { g_rg_scanT = 0; g_rg_have = 0; g_rg_notFoundSaid = 0; if (g_rg_on) rg_tick(); else chat_say("Зона выключена. /ring on — включить"); return; }
    if (seq(tok[0], "any")) { g_rg_any = 1; g_rg_have = 0; g_rg_notFoundSaid = 0; chat_say("Зона: центр — ближайший пикап любой модели"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "cloth")) { g_rg_any = 0; g_rg_have = 0; g_rg_notFoundSaid = 0; chat_say("Зона: центр — только раздевалка (модель «cloth»)"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "game")) { rg_destroy_all(); g_rg_flat = 0; g_rg_dirty = 1; g_rg_zone = -1; g_rg_gmTries = 0; chat_say("Зона: родная метка игры"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "flat")) { rg_gm_hide(); g_rg_flat = 1; g_rg_dirty = 1; g_rg_zone = -1; chat_say("Зона: своё кольцо на полу"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "see")) {
        rg_gm_hide(); g_rg_flat = 1; g_rg_dirty = 1;
        g_rg_see = !g_rg_see;
        if (g_rg_hMat) { RGA.gc_free(g_rg_hMat); g_rg_hMat = 0; }
        rg_destroy_all();
        chat_say(g_rg_see ? "Зона: рисуется поверх всего (видно и сквозь стены)" : "Зона: рисуется на полу (персонажи и стены её перекрывают)");
        if (g_rg_on) rg_tick();
        return;
    }
    if (seq(tok[0], "reset")) { g_rg_rIn = 1.6f; g_rg_rOut = 3.8f; g_rg_lift = 0.0f; if (g_rg_skin != 1) { rg_gm_unskin(1); g_rg_skin = 1; } g_rg_alpha = 0.15f; g_rg_zone = -1; if (g_rg_see) { g_rg_see = 0; if (g_rg_hMat) { RGA.gc_free(g_rg_hMat); g_rg_hMat = 0; } rg_destroy_all(); } rg_gm_hide(); g_rg_flat = 0; g_rg_cal = 0.0f; g_rg_calChecked = 0; g_rg_dirty = 1; chat_say("Зона: 1.6–3.8 м, подъём 0, прозрачность 0.15, родная метка игры"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "r") && n >= 3) {
        float a, c;
        if (!rg_num(tok[1], &a) || !rg_num(tok[2], &c) || a < 0.5f || c > 8.0f || a >= c) { chat_say("Зона: /ring r 1.6 3.8 — внутренний и внешний радиус, м (от 0.5 до 8, внутренний меньше внешнего)"); return; }
        g_rg_rIn = a; g_rg_rOut = c; g_rg_dirty = 1; g_rg_zone = -1; g_rg_calChecked = 0;
        chat_say("Зона: радиусы изменены"); if (g_rg_on) rg_tick();
        return;
    }
    if (seq(tok[0], "a") && n >= 2) {
        float v;
        if (!rg_num(tok[1], &v) || v < 0.02f || v > 1.0f) { chat_say("Зона: /ring a 0.15 — прозрачность родной метки, от 0.02 (почти невидимая) до 1 (непрозрачная)"); return; }
        g_rg_alpha = v; g_rg_zone = -1;
        chat_say("Зона: прозрачность изменена"); if (g_rg_on) rg_tick();
        return;
    }
    if (seq(tok[0], "mat") && n >= 2) {
        int m = seq(tok[1], "0") ? 0 : seq(tok[1], "1") ? 1 : seq(tok[1], "2") ? 2 : -1;
        if (m < 0) { chat_say("Зона: /ring mat 1 — свой материал с текстурой метки (по умолчанию), 2 — свой без текстуры (сплошная заливка), 0 — родной материал игры"); return; }
        if (m != g_rg_skin) { rg_gm_unskin(1); g_rg_skin = m; g_rg_zone = -1; g_rg_gmTries = 0; }
        chat_say(m == 1 ? "Зона: свой полупрозрачный материал с текстурой метки" : m == 2 ? "Зона: свой полупрозрачный материал без текстуры (сплошная заливка)" : "Зона: родной материал метки (только правка свойств цвета)");
        if (g_rg_on) rg_tick();
        return;
    }
    if ((seq(tok[0], "y") || seq(tok[0], "dz")) && n >= 2) {
        float v;
        if (!rg_num(tok[1], &v) || v < -2.0f || v > 2.0f) { chat_say("Зона: /ring y 0.05 — поднять плоскую зону над полом, м (от −2 до 2)"); return; }
        g_rg_lift = v; g_rg_dirty = 1; chat_say("Зона: высота изменена"); if (g_rg_on) rg_tick();
        return;
    }
    chat_say("Зона: /ring on|off, /ring info, /ring a 0.15 (прозрачность), /ring mat 1|2|0, /ring r 1.6 3.8, /ring game|flat, /ring y 0.05, /ring see, /ring any|cloth, /ring scan, /ring reset");
}

/* ───────────── самозапуск ───────────── */
/* фоновый поток раз в 0,5 с ставит тик в главный поток, пока кольца включены */
static void ring_post(void) {
    void *cb, *st, *args[2], *exc = NULL; long now = time(NULL);
    if (g_rg_pending && now - g_rg_postT < 3) return;           /* прошлый тик ещё не выполнен (или выполнен менее 3 с назад — игра на паузе) */
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_ring_cmd);
    st = A.string_new("ring-tick");
    if (!cb || !st) return;
    args[0] = cb; args[1] = st;
    g_rg_pending = 1; g_rg_postT = now;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, args, &exc);
    if (exc) { char et[200]; exc_text(exc, et, sizeof et); dlog2("ring: Post бросил, кольца выключены: ", et); g_rg_on = 0; g_rg_pending = 0; }
}
static void* ring_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("ring", native_ring_cmd)) return NULL;
    hm_mod_ready(g_ring_modId);
    for (;;) {
        usleep(500000);
        if (g_rg_on && g_mt_ctx && g_mt_postM && g_mt_kCb) { ensure_attached(); ring_post(); }
    }
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void ring_ctor(void) {
    unsigned long t = 0;
    g_ring_modId = hm_mod_register("ring");
    pthread_create(&t, NULL, ring_main, NULL);
}
#endif
