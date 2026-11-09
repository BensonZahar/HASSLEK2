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
 * Как выглядит: одна полупрозрачная кольцевая полоса на полу. ЗЕЛЁНАЯ — вы внутри зоны (стойте здесь), КРАСНАЯ — вы слишком близко к пикапу или вышли за зону.
 *
 * Как рисуем (по дизассемблированию EditObjectAxis..ctor, RVA 0x123F2B4: игра сама делает линии через LineRenderer): GameObject + LineRenderer, одна окружность
 *   с шириной = ширине зоны и alignment = TransformZ, а сам объект повёрнут на −90° вокруг X — полоса лежит плашмя, как пятно на полу.
 *   Материал: по умолчанию «Sprites/Default» (встроенный, глубину проверяет — зона на полу, персонажи и стены её перекрывают), запасной — «GUI/Text Shader»
 *   (как у линий игры; рисуется поверх всего, видно и сквозь стены). /ring see переключает между ними.
 *   Высота: Transform.y ваших ног, когда вы стоите на земле у раздевалки (пока не встали — пикап − 1 м). Лишний метр между точкой пикапа и полом
 *   (если пикап висит выше) учитывается в радиусе: радиус на полу = sqrt(R² − dz²), dz = высота пикапа − (пол + 1).
 *
 * Команды:
 *   /ring            включить/выключить      /ring on | off
 *   /ring info       где центр, расстояние до него, зона
 *   /ring r 1.6 3.8  внутренний и внешний радиус зоны, м     /ring reset — вернуть 1.6 / 3.8, подъём 0, материал по умолчанию
 *   /ring any        центр — ближайший пикап любой модели (если раздевалка не находится по имени «cloth»);  /ring cloth — обратно только раздевалка
 *   /ring y 0.05     поднять/опустить зону относительно пола, м (−2…2); если зона мерцает или уходит под пол — поднять
 *   /ring see        материал: на полу (Sprites/Default) ⇄ поверх всего (GUI/Text Shader)
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
    void *kGo, *kMat, *kShader, *kLr, *kObj, *kTr;
    void *mGoCtor, *mAdd, *mFind, *mMatCtor, *mSetQ, *mSetMat, *mWidth, *mWorld, *mCount, *mPos, *mCol0, *mCol1, *mEnable, *mDestroy;
    void *mAlign, *mLoop, *mGetTr, *mTrPos, *mTrRot;
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
static int rg_resolve(void) {
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
    RG.kTr     = A.class_from_name(img, "UnityEngine", "Transform");
    if (!RG.kGo || !RG.kMat || !RG.kShader || !RG.kLr || !RG.kObj || !RG.kTr) { dlog("ring: не нашлись классы UnityEngine (GameObject/Material/Shader/LineRenderer/Object/Transform)"); RG.state = -1; return 0; }
    RG.mGoCtor  = rg_find_m1(RG.kGo, ".ctor", "System.String");
    RG.mAdd     = rg_find_m1(RG.kGo, "AddComponent", "System.Type");
    RG.mGetTr   = find_method(RG.kGo, "get_transform", 0);
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
    RG.mAlign   = find_method(RG.kLr, "set_alignment", 1);
    RG.mLoop    = find_method(RG.kLr, "set_loop", 1);            /* необязательный: без него замыкаем сами повтором первой точки */
    RG.mTrPos   = find_method(RG.kTr, "set_position", 1);
    RG.mTrRot   = find_method(RG.kTr, "set_rotation", 1);
    RG.mDestroy = find_method(RG.kObj, "Destroy", 1);
    RG.typeLr   = RGA.type_object(RGA.cls_type(RG.kLr));
    {
        const char* miss = !RG.mGoCtor ? "GameObject..ctor(string)" : !RG.mAdd ? "GameObject.AddComponent(Type)" : !RG.mGetTr ? "GameObject.get_transform" :
            !RG.mFind ? "Shader.Find" : !RG.mMatCtor ? "Material..ctor(Shader)" : !RG.mSetQ ? "Material.set_renderQueue" : !RG.mSetMat ? "Renderer.set_material" :
            !RG.mWidth ? "LineRenderer.set_widthMultiplier" : !RG.mWorld ? "LineRenderer.set_useWorldSpace" : !RG.mCount ? "LineRenderer.set_positionCount" :
            !RG.mPos ? "LineRenderer.SetPosition" : !RG.mCol0 ? "LineRenderer.set_startColor" : !RG.mCol1 ? "LineRenderer.set_endColor" :
            !RG.mEnable ? "Renderer.set_enabled" : !RG.mAlign ? "LineRenderer.set_alignment" : !RG.mTrPos ? "Transform.set_position" :
            !RG.mTrRot ? "Transform.set_rotation" : !RG.mDestroy ? "Object.Destroy" : !RG.typeLr ? "typeof(LineRenderer)" : NULL;
        if (miss) { dlog2("ring: нет метода ", miss); RG.state = -1; return 0; }
    }
    RG.state = 1;
    dlog(RG.mLoop ? "ring: методы Unity найдены (с set_loop)" : "ring: методы Unity найдены (без set_loop)");
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

/* создать объект зоны: GameObject + LineRenderer, плашмя (alignment = TransformZ, поворот −90° вокруг X), локальные координаты. 1 — получилось */
static int rg_make(void) {
    void *mat, *go, *lr = NULL, *tr = NULL, *args[2], *name; u8 world = 0; int align = 1; char nm[16];
    float rot[4];
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
    args[0] = &align;  rg_call("LineRenderer.set_alignment", RG.mAlign, lr, args, NULL);
    if (RG.mLoop) { u8 lp = 1; args[0] = &lp; rg_call("LineRenderer.set_loop", RG.mLoop, lr, args, NULL); }
    if (!rg_call("GameObject.get_transform", RG.mGetTr, go, NULL, &tr) || !tr || !hm_obj_ok(tr)) return 0;
    rot[0] = -0.70710678f; rot[1] = 0.0f; rot[2] = 0.0f; rot[3] = 0.70710678f;          /* Quaternion: −90° вокруг X (локальный Z смотрит вверх) */
    args[0] = rot;     if (!rg_call("Transform.set_rotation", RG.mTrRot, tr, args, NULL)) return 0;
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

/* поставить объект на пол над центром */
static int rg_place(void) {
    void *go, *tr = NULL, *args[1]; float p[3];
    go = g_rg_hGo ? RGA.gc_target(g_rg_hGo) : NULL;
    if (!go || !hm_obj_ok(go)) return 0;
    if (!rg_call("GameObject.get_transform", RG.mGetTr, go, NULL, &tr) || !tr || !hm_obj_ok(tr)) return 0;
    p[0] = g_rg_c[0]; p[1] = rg_floor_y() + RG_LIFT + g_rg_lift; p[2] = g_rg_c[2];
    args[0] = p;
    return rg_call("Transform.set_position", RG.mTrPos, tr, args, NULL);
}

/* окружность по середине полосы: ширина линии = ширина зоны */
static int rg_points(float rIn, float rOut) {
    void *lr, *args[2]; int k, idx, ok = 1, cnt = RG.mLoop ? RG_SEG : RG_SEG + 1; float p[3], w = rOut - rIn, R = (rIn + rOut) * 0.5f;
    double c, s, a = 6.283185307179586 / RG_SEG, x, y, nx;
    lr = rg_lr();
    if (!lr) return 0;
    args[0] = &w;   if (!rg_call("LineRenderer.set_widthMultiplier", RG.mWidth, lr, args, NULL)) return 0;
    args[0] = &cnt; if (!rg_call("LineRenderer.set_positionCount", RG.mCount, lr, args, NULL)) return 0;
    c = 1.0 - a * a / 2 + a * a * a * a / 24 - a * a * a * a * a * a / 720;              /* cos и sin шага — без libm */
    s = a - a * a * a / 6 + a * a * a * a * a / 120 - a * a * a * a * a * a * a / 5040;
    x = R; y = 0;                                                                         /* поворачиваем вектор радиуса шаг за шагом */
    for (k = 0; k < cnt && ok; k++) {
        p[0] = (float)x; p[1] = (float)y; p[2] = 0.0f;                                    /* локальная плоскость XY; поворот объекта кладёт её на пол */
        idx = k; args[0] = &idx; args[1] = p;
        ok = rg_call("LineRenderer.SetPosition", RG.mPos, lr, args, NULL);
        nx = x * c - y * s; y = x * s + y * c; x = nx;
    }
    return ok;
}

/* объект зоны жив? */
static int rg_alive(void) {
    void* go = g_rg_hGo ? RGA.gc_target(g_rg_hGo) : NULL;
    return go && hm_obj_ok(go) && rg_lr();
}

/* перерисовать зону (после смены центра/радиусов/пола). 1 — получилось. Любое исключение (например, объект уничтожен при смене локации) —
   всё убираем и в следующий тик создаём заново; 5 неудач подряд — выключаемся. */
static int rg_draw(void) {
    int ok = 1;
    if (!rg_alive()) {
        rg_destroy_all();
        ok = rg_make();
    }
    if (ok) ok = rg_points(rg_flat_r(g_rg_rIn), rg_flat_r(g_rg_rOut)) && rg_place();
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
        if (had) { rg_enable(0); dlog("ring: раздевалка пропала из зоны стрима — зона скрыта"); }
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
    if (!g_rg_have || g_rg_dist < 0.0f || !rg_alive()) return;
    z = g_rg_dist < g_rg_rIn ? 0 : g_rg_dist > g_rg_rOut ? 2 : 1;
    if (z == g_rg_zone) return;
    g_rg_zone = z;
    rg_set_color(z == 1 ? g_rg_colOk : g_rg_colBad);
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
    Buf b; char msg[300];
    b_init(&b, msg, sizeof msg);
    if (!g_rg_on) { chat_say("Зона выключена. /ring on — включить"); return; }
    if (!g_rg_have) { chat_say("Зона: центр не найден (раздевалка не в зоне стрима). /ring scan — искать заново, /ring any — любой пикап"); return; }
    b_s(&b, "Зона: пикап ID "); b_int(&b, g_rg_id); b_s(&b, ", вы в "); b_fix(&b, g_rg_dist >= 0 ? g_rg_dist : 0, 1); b_s(&b, " м — ");
    b_s(&b, g_rg_zone == 0 ? "СЛИШКОМ БЛИЗКО (ближе 1,5 м вход не сработает)" : g_rg_zone == 2 ? "ЗА ЗОНОЙ (дальше 4 м диалог не примет)" : g_rg_zone == 1 ? "в зоне, всё хорошо" : "расстояние не определилось");
    b_s(&b, ". Зона "); b_fix(&b, g_rg_rIn, 1); b_s(&b, "–"); b_fix(&b, g_rg_rOut, 1); b_s(&b, " м, пол "); b_s(&b, g_rg_gyOk ? "по ногам" : "пока по пикапу"); b_s(&b, ", подъём "); b_fix(&b, g_rg_lift, 2);
    b_s(&b, g_rg_see ? " м, поверх всего" : " м, на полу"); b_s(&b, g_rg_any ? ", центр: любой пикап" : ", центр: раздевалка");
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
            g_rg_on = 1; g_rg_notFoundSaid = 0; g_rg_have = 0; g_rg_dirty = 1; g_rg_scanT = 0; g_rg_fails = 0; g_rg_gyOk = 0;
            if (!g_mt_ctx) chat_say("Кольцо: нет главного потока для автообновления — рисую один раз, повторный /ring on обновит");
            rg_tick();
        } else {
            g_rg_on = 0;
            rg_destroy_all();
            chat_say("Зона выключена");
        }
        return;
    }
    if (seq(tok[0], "info") || seq(tok[0], "status")) { rg_info(); return; }
    if (seq(tok[0], "scan")) { g_rg_scanT = 0; g_rg_have = 0; g_rg_notFoundSaid = 0; if (g_rg_on) rg_tick(); else chat_say("Зона выключена. /ring on — включить"); return; }
    if (seq(tok[0], "any")) { g_rg_any = 1; g_rg_have = 0; g_rg_notFoundSaid = 0; chat_say("Зона: центр — ближайший пикап любой модели"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "cloth")) { g_rg_any = 0; g_rg_have = 0; g_rg_notFoundSaid = 0; chat_say("Зона: центр — только раздевалка (модель «cloth»)"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "see")) {
        g_rg_see = !g_rg_see;
        if (g_rg_hMat) { RGA.gc_free(g_rg_hMat); g_rg_hMat = 0; }
        rg_destroy_all();
        chat_say(g_rg_see ? "Зона: рисуется поверх всего (видно и сквозь стены)" : "Зона: рисуется на полу (персонажи и стены её перекрывают)");
        if (g_rg_on) rg_tick();
        return;
    }
    if (seq(tok[0], "reset")) { g_rg_rIn = 1.6f; g_rg_rOut = 3.8f; g_rg_lift = 0.0f; if (g_rg_see) { g_rg_see = 0; if (g_rg_hMat) { RGA.gc_free(g_rg_hMat); g_rg_hMat = 0; } rg_destroy_all(); } g_rg_dirty = 1; chat_say("Зона: 1.6–3.8 м, подъём 0, на полу"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "r") && n >= 3) {
        float a, c;
        if (!rg_num(tok[1], &a) || !rg_num(tok[2], &c) || a < 0.5f || c > 8.0f || a >= c) { chat_say("Зона: /ring r 1.6 3.8 — внутренний и внешний радиус, м (от 0.5 до 8, внутренний меньше внешнего)"); return; }
        g_rg_rIn = a; g_rg_rOut = c; g_rg_dirty = 1; g_rg_zone = -1;
        chat_say("Зона: радиусы изменены"); if (g_rg_on) rg_tick();
        return;
    }
    if ((seq(tok[0], "y") || seq(tok[0], "dz")) && n >= 2) {
        float v;
        if (!rg_num(tok[1], &v) || v < -2.0f || v > 2.0f) { chat_say("Зона: /ring y 0.05 — поднять зону над полом, м (от −2 до 2)"); return; }
        g_rg_lift = v; g_rg_dirty = 1; chat_say("Зона: высота изменена"); if (g_rg_on) rg_tick();
        return;
    }
    chat_say("Зона: /ring on|off, /ring info, /ring r 1.6 3.8, /ring y 0.05, /ring see, /ring any|cloth, /ring scan, /ring reset");
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
