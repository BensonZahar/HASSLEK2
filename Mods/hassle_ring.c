/*
 * hassle_ring.c — /ring: рисует в игре кольцо вокруг раздевалки (пикап «clothesp»), чтобы было видно, где стоять для /dressu и «Переодевание ночь».
 *
 * Почему кольцо, а не круг (по серверу, new.pwn и include/core/pickup.pwn):
 *   • вход в пикап сервер принимает, если игрок не дальше 5 м (IsPlayerInPickup(…, 5.0, …)), НО только когда флаг «уже стою на пикапе» (P_IN_PICKUP) сброшен;
 *   • флаг сбрасывается таймером в 250 мс, как только игрок дальше 1,5 м от пикапа, — пока вы ближе 1,5 м, повторный вход молча игнорируется (отсюда «вплотную /dressu не работает»);
 *   • ответы на диалоги раздевалки (DIALOG_TEAM_SKIN …) сервер принимает только до 4,0 м (IsPlayerInPickupEx(…, 4.0, …)).
 *   Значит стоять надо в кольце: от 1,5 до 4,0 м от точки пикапа (расстояние трёхмерное, от позиции игрока в пакете синхронизации = Transform.y + 1).
 *   Рисуем три окружности: КРАСНАЯ 1,7 м (ближе — вход не сработает), ЗЕЛЁНАЯ посередине (стойте на ней), ЖЁЛТАЯ 3,8 м (дальше диалог не примет).
 *   Средняя окружность меняет цвет по вашему реальному расстоянию: зелёная — вы в рабочей зоне, красная — слишком близко, жёлтая — слишком далеко.
 *
 * Как рисуем (по дизассемблированию EditObjectAxis..ctor, RVA 0x123F2B4): игра сама делает линии так — LineRenderer на новом GameObject, материал
 *   new Material(Shader.Find("GUI/Text Shader")) (встроенный шейдер, он точно есть в билде), renderQueue = 3010, цвет — startColor/endColor.
 *   Мы делаем так же, только useWorldSpace = true и 73 точки по кругу. Шейдер рисует поверх всего (ZTest Always) — кольцо видно и сквозь стены.
 *
 * Команды:
 *   /ring            включить/выключить      /ring on | off
 *   /ring info       где центр, расстояние до него, зона (ок / слишком близко / слишком далеко)
 *   /ring r 1.7 3.8  радиусы красного и жёлтого кольца (зелёное — посередине)     /ring reset — вернуть 1.7 / 3.8
 *   /ring any        центр — ближайший пикап любой модели (если раздевалка не находится по имени «cloth»);  /ring cloth — обратно только раздевалка
 *   /ring dz 0.5     вертикальная поправка, м: на сколько точка пикапа выше центра игрока (по умолчанию 0; радиусы кольца на земле — sqrt(R² − dz²))
 *   /ring scan       искать раздевалку заново сейчас
 *
 * Всё рисование — в главном потоке (Post в UnitySynchronizationContext, как в /nocol); фоновый поток раз в 0,5 с только ставит задачу.
 */
#pragma once
#include "hassle_core.c"

#define RG_SEG 72                       /* сегментов окружности (точек SEG+1: последняя замыкает) */
#define RG_N 3                          /* красная, средняя, жёлтая */
#define RG_SCAN_M 60.0f                 /* раздевалку ищем не дальше этого от игрока, м */
#define RG_RESCAN_S 3                   /* как часто перечитывать пикапы, с */
#define RG_SRV_IN 1.5f                  /* ближе — флаг «стою на пикапе» не сбрасывается */
#define RG_SRV_OUT 4.0f                 /* дальше — ответы диалогов раздевалки не принимаются */
#define RG_QUEUE 3010                   /* renderQueue, как у линий игры */

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
static volatile int g_rg_on;            /* кольца включены */
static volatile int g_rg_pending;       /* в главный поток уже поставлен тик */
static volatile long g_rg_postT;
static int g_rg_any;                    /* 1 — центр любой пикап (/ring any), 0 — только с «cloth» в имени модели */
static float g_rg_rIn = 1.7f, g_rg_rOut = 3.8f, g_rg_dz = 0.0f;
static const float g_rg_width[RG_N] = { 0.10f, 0.06f, 0.10f };
static const float g_rg_col[RG_N][4] = { { 1.0f, 0.15f, 0.15f, 1.0f }, { 0.2f, 1.0f, 0.3f, 1.0f }, { 1.0f, 0.85f, 0.1f, 1.0f } };

/* найденный центр (пикап раздевалки) */
static int g_rg_have, g_rg_id;
static float g_rg_c[3];
static long g_rg_scanT;
static int g_rg_dirty;                  /* центр или радиусы изменились — перерисовать точки */
static int g_rg_zone = -1;              /* 0 — слишком близко, 1 — ок, 2 — слишком далеко; -1 — неизвестно */
static float g_rg_dist = -1.0f;
static int g_rg_notFoundSaid;

/* ресурсы Unity */
static struct {
    int state;                          /* 0 — не искали, 1 — всё есть, -1 — чего-то нет */
    void *kGo, *kMat, *kShader, *kLr, *kObj;
    void *mGoCtor, *mAdd, *mFind, *mMatCtor, *mSetQ, *mSetMat, *mWidth, *mWorld, *mCount, *mPos, *mCol0, *mCol1, *mEnable, *mDestroy;
    void* typeLr;
} RG;
static unsigned g_rg_hMat;
static unsigned g_rg_hGo[RG_N], g_rg_hLr[RG_N];
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
        const char* miss = !RG.mGoCtor ? "GameObject..ctor(string)" : !RG.mAdd ? "GameObject.AddComponent(Type)" : !RG.mFind ? "Shader.Find" :
            !RG.mMatCtor ? "Material..ctor(Shader)" : !RG.mSetQ ? "Material.set_renderQueue" : !RG.mSetMat ? "Renderer.set_material" :
            !RG.mWidth ? "LineRenderer.set_widthMultiplier" : !RG.mWorld ? "LineRenderer.set_useWorldSpace" : !RG.mCount ? "LineRenderer.set_positionCount" :
            !RG.mPos ? "LineRenderer.SetPosition" : !RG.mCol0 ? "LineRenderer.set_startColor" : !RG.mCol1 ? "LineRenderer.set_endColor" :
            !RG.mEnable ? "Renderer.set_enabled" : !RG.mDestroy ? "Object.Destroy" : !RG.typeLr ? "typeof(LineRenderer)" : NULL;
        if (miss) { dlog2("ring: нет метода ", miss); RG.state = -1; return 0; }
    }
    RG.state = 1;
    dlog("ring: методы Unity найдены");
    return 1;
}

/* общий материал (создаётся один раз) */
static void* rg_material(void) {
    void *mat, *shader = NULL, *args[1], *r = NULL;
    static const char* const names[] = { "GUI/Text Shader", "Sprites/Default", "Hidden/Internal-Colored", NULL };
    int i, q = RG_QUEUE;
    if (g_rg_hMat) { mat = RGA.gc_target(g_rg_hMat); if (mat && hm_obj_ok(mat)) return mat; g_rg_hMat = 0; }
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

static void rg_set_color(int i, const float* c) {
    void *lr, *args[1]; float col[4];
    lr = g_rg_hLr[i] ? RGA.gc_target(g_rg_hLr[i]) : NULL;
    if (!lr) return;
    col[0] = c[0]; col[1] = c[1]; col[2] = c[2]; col[3] = c[3];
    args[0] = col;
    rg_call("set_startColor", RG.mCol0, lr, args, NULL);
    rg_call("set_endColor", RG.mCol1, lr, args, NULL);
}

static void rg_enable(int on) {
    int i;
    for (i = 0; i < RG_N; i++) {
        void *lr, *args[1]; u8 v = on ? 1 : 0;
        lr = g_rg_hLr[i] ? RGA.gc_target(g_rg_hLr[i]) : NULL;
        if (!lr) continue;
        args[0] = &v;
        rg_call("set_enabled", RG.mEnable, lr, args, NULL);
    }
}

/* убрать все объекты колец */
static void rg_destroy_all(void) {
    int i;
    if (RG.state > 0) {
        for (i = 0; i < RG_N; i++) {
            void* go = g_rg_hGo[i] ? RGA.gc_target(g_rg_hGo[i]) : NULL; void* args[1];
            if (go && hm_obj_ok(go)) { args[0] = go; rg_call("Object.Destroy", RG.mDestroy, NULL, args, NULL); }
        }
    }
    for (i = 0; i < RG_N; i++) {
        if (g_rg_hGo[i]) RGA.gc_free(g_rg_hGo[i]);
        if (g_rg_hLr[i]) RGA.gc_free(g_rg_hLr[i]);
        g_rg_hGo[i] = g_rg_hLr[i] = 0;
    }
    g_rg_zone = -1;
    g_rg_dirty = 1;
}

/* создать i-е кольцо (GameObject + LineRenderer). 1 — получилось */
static int rg_make(int i) {
    void *mat, *go, *lr = NULL, *args[2], *name; float w = g_rg_width[i]; int cnt = RG_SEG + 1; u8 world = 1; char nm[24];
    nm[0] = 'k'; nm[1] = 'o'; nm[2] = 'n'; nm[3] = 's'; nm[4] = 't'; nm[5] = '_'; nm[6] = 'r'; nm[7] = 'i'; nm[8] = 'n'; nm[9] = 'g'; nm[10] = (char)('0' + i); nm[11] = 0;
    mat = rg_material();
    if (!mat) return 0;
    go = A.object_new(RG.kGo);
    if (!go) return 0;
    name = A.string_new(nm);
    args[0] = name;
    if (!rg_call("GameObject..ctor", RG.mGoCtor, go, args, NULL)) return 0;
    args[0] = RG.typeLr;
    if (!rg_call("AddComponent(LineRenderer)", RG.mAdd, go, args, &lr) || !lr || !hm_obj_ok(lr)) return 0;
    g_rg_hGo[i] = RGA.gc_new(go, 0);
    g_rg_hLr[i] = RGA.gc_new(lr, 0);
    args[0] = mat;     rg_call("Renderer.set_material", RG.mSetMat, lr, args, NULL);
    args[0] = &world;  rg_call("LineRenderer.set_useWorldSpace", RG.mWorld, lr, args, NULL);
    args[0] = &w;      rg_call("LineRenderer.set_widthMultiplier", RG.mWidth, lr, args, NULL);
    args[0] = &cnt;    rg_call("LineRenderer.set_positionCount", RG.mCount, lr, args, NULL);
    rg_set_color(i, g_rg_col[i]);
    return 1;
}

/* Радиус кольца на земле для трёхмерного расстояния R с вертикальной поправкой dz: sqrt(R² − dz²) */
static float rg_flat_r(float R) {
    double v = (double)R * R - (double)g_rg_dz * g_rg_dz;
    return v > 0.01 ? (float)__builtin_sqrt(v) : 0.0f;
}

/* расставить точки i-го кольца вокруг центра. 1 — без исключений */
static int rg_points(int i, float R) {
    void *lr, *args[2]; int k, idx, ok = 1; float p[3];
    double c, s, a = 6.283185307179586 / RG_SEG, x, z, nx;
    lr = g_rg_hLr[i] ? RGA.gc_target(g_rg_hLr[i]) : NULL;
    if (!lr) return 0;
    c = 1.0 - a * a / 2 + a * a * a * a / 24 - a * a * a * a * a * a / 720;              /* cos и sin шага — без libm */
    s = a - a * a * a / 6 + a * a * a * a * a / 120 - a * a * a * a * a * a * a / 5040;
    x = R; z = 0;                                                                         /* поворачиваем вектор радиуса шаг за шагом */
    for (k = 0; k <= RG_SEG && ok; k++) {
        p[0] = g_rg_c[0] + (float)x; p[1] = g_rg_c[1] - 1.0f + 0.05f; p[2] = g_rg_c[2] + (float)z;   /* на уровне ног: Transform.y игрока = y пикапа − 1 */
        idx = k; args[0] = &idx; args[1] = p;
        ok = rg_call("LineRenderer.SetPosition", RG.mPos, lr, args, NULL);
        nx = x * c - z * s; z = x * s + z * c; x = nx;
    }
    return ok;
}

/* все три кольца живы? */
static int rg_alive(void) {
    int i;
    for (i = 0; i < RG_N; i++) {
        void* lr = g_rg_hLr[i] ? RGA.gc_target(g_rg_hLr[i]) : NULL;
        if (!lr || !hm_obj_ok(lr)) return 0;
    }
    return 1;
}

/* перерисовать все кольца (после смены центра/радиусов). 1 — получилось. Любое исключение (например, объект уничтожен при смене локации) —
   всё убираем и в следующий тик создаём заново; 5 неудач подряд — выключаемся. */
static int rg_draw(void) {
    float R[RG_N]; int i, ok = 1;
    R[0] = rg_flat_r(g_rg_rIn); R[2] = rg_flat_r(g_rg_rOut); R[1] = (R[0] + R[2]) * 0.5f;
    if (!rg_alive()) {
        rg_destroy_all();
        for (i = 0; i < RG_N && ok; i++) ok = rg_make(i);
    }
    for (i = 0; i < RG_N && ok; i++) ok = rg_points(i, R[i]);
    if (ok) { rg_enable(1); g_rg_dirty = 0; g_rg_fails = 0; }
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
        if (had) { rg_enable(0); dlog("ring: раздевалка пропала из зоны стрима — кольца скрыты"); }
        return;
    }
    g_rg_have = 1; g_rg_id = RS.id;
    g_rg_c[0] = RS.p[0]; g_rg_c[1] = RS.p[1]; g_rg_c[2] = RS.p[2];
    g_rg_notFoundSaid = 0;
    if (!had || rg_d3(old, g_rg_c) > 0.05f) {
        Buf b; char msg[200];
        g_rg_dirty = 1;
        b_init(&b, msg, sizeof msg);
        b_s(&b, g_rg_any ? "Кольцо: центр — пикап ID " : "Кольцо у раздевалки: пикап ID "); b_int(&b, g_rg_id);
        b_s(&b, ", красное — "); b_fix(&b, g_rg_rIn, 1); b_s(&b, " м (ближе не сработает), жёлтое — "); b_fix(&b, g_rg_rOut, 1); b_s(&b, " м (дальше диалог не примет)");
        chat_say(msg);
        dlog2("ring: ", msg);
    }
}

/* цвет средней окружности по реальному расстоянию до центра: зелёная — рабочая зона */
static void rg_zone_update(void) {
    float me[3]; void* lp; int z; static const float red[4] = { 1.0f, 0.15f, 0.15f, 1.0f }, yel[4] = { 1.0f, 0.85f, 0.1f, 1.0f };
    lp = local_player();
    if (!g_rg_have || !lp || !hm_obj_ok(lp) || !read_pos(lp, me)) return;
    g_rg_dist = rg_d3(me, g_rg_c);
    z = g_rg_dist < RG_SRV_IN ? 0 : g_rg_dist > RG_SRV_OUT - 0.1f ? 2 : 1;
    if (z == g_rg_zone) return;
    g_rg_zone = z;
    rg_set_color(1, z == 0 ? red : z == 2 ? yel : g_rg_col[1]);
    dlog2("ring: зона: ", z == 0 ? "слишком близко (<1.5 м)" : z == 2 ? "слишком далеко (>3.9 м)" : "рабочая (1.5–3.9 м)");
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
    if (g_rg_dirty && !rg_draw()) {
        if (g_rg_fails >= 5) { g_rg_on = 0; chat_say("Кольцо: не получилось нарисовать 5 раз подряд — выключено. Подробности в HLOG/konstt.log"); dlog("ring: 5 неудач подряд, выключено"); }
        return;
    }
    rg_zone_update();
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
    if (!g_rg_on) { chat_say("Кольцо выключено. /ring on — включить"); return; }
    if (!g_rg_have) { chat_say("Кольцо: центр не найден (раздевалка не в зоне стрима). /ring scan — искать заново, /ring any — любой пикап"); return; }
    b_s(&b, "Кольцо: пикап ID "); b_int(&b, g_rg_id); b_s(&b, ", вы в "); b_fix(&b, g_rg_dist >= 0 ? g_rg_dist : 0, 1); b_s(&b, " м — ");
    b_s(&b, g_rg_zone == 0 ? "СЛИШКОМ БЛИЗКО (ближе 1,5 м вход не сработает)" : g_rg_zone == 2 ? "СЛИШКОМ ДАЛЕКО (дальше 4 м диалог не примет)" : g_rg_zone == 1 ? "рабочая зона" : "расстояние не определилось");
    b_s(&b, ". Радиусы: "); b_fix(&b, g_rg_rIn, 1); b_s(&b, " / "); b_fix(&b, g_rg_rOut, 1); b_s(&b, " м, dz "); b_fix(&b, g_rg_dz, 1); b_s(&b, g_rg_any ? ", центр: любой пикап" : ", центр: раздевалка");
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
            g_rg_on = 1; g_rg_notFoundSaid = 0; g_rg_have = 0; g_rg_dirty = 1; g_rg_scanT = 0; g_rg_fails = 0;
            if (!g_mt_ctx) chat_say("Кольцо: нет главного потока для автообновления — рисую один раз, повторный /ring on обновит");
            rg_tick();
            if (g_rg_have) chat_say("Красное — не стойте внутри, жёлтое — не выходите за него, зелёное — стойте на нём. /ring info — расстояние");
        } else {
            g_rg_on = 0;
            rg_destroy_all();
            chat_say("Кольцо выключено");
        }
        return;
    }
    if (seq(tok[0], "info") || seq(tok[0], "status")) { rg_info(); return; }
    if (seq(tok[0], "scan")) { g_rg_scanT = 0; g_rg_have = 0; g_rg_notFoundSaid = 0; if (g_rg_on) rg_tick(); else chat_say("Кольцо выключено. /ring on — включить"); return; }
    if (seq(tok[0], "any")) { g_rg_any = 1; g_rg_have = 0; g_rg_notFoundSaid = 0; chat_say("Кольцо: центр — ближайший пикап любой модели"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "cloth")) { g_rg_any = 0; g_rg_have = 0; g_rg_notFoundSaid = 0; chat_say("Кольцо: центр — только раздевалка (модель «cloth»)"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "reset")) { g_rg_rIn = 1.7f; g_rg_rOut = 3.8f; g_rg_dz = 0.0f; g_rg_dirty = 1; chat_say("Кольцо: радиусы 1.7 / 3.8 м, dz 0"); if (g_rg_on) rg_tick(); return; }
    if (seq(tok[0], "r") && n >= 3) {
        float a, c;
        if (!rg_num(tok[1], &a) || !rg_num(tok[2], &c) || a < 0.5f || c > 8.0f || a >= c) { chat_say("Кольцо: /ring r 1.7 3.8 — внутренний и внешний радиус, м (от 0.5 до 8, внутренний меньше внешнего)"); return; }
        g_rg_rIn = a; g_rg_rOut = c; g_rg_dirty = 1;
        chat_say("Кольцо: радиусы изменены"); if (g_rg_on) rg_tick();
        return;
    }
    if (seq(tok[0], "dz") && n >= 2) {
        float v;
        if (!rg_num(tok[1], &v) || v < -3.0f || v > 3.0f) { chat_say("Кольцо: /ring dz 0.5 — на сколько точка пикапа выше центра игрока, м (от −3 до 3)"); return; }
        g_rg_dz = v; g_rg_dirty = 1; chat_say("Кольцо: вертикальная поправка изменена"); if (g_rg_on) rg_tick();
        return;
    }
    chat_say("Кольцо: /ring on|off, /ring info, /ring r 1.7 3.8, /ring dz 0.5, /ring any|cloth, /ring scan, /ring reset");
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
