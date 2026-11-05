/*
 * hassle_spec.c — необязательный модуль «/spec»: тест «spectate sync» вместо «onfoot sync» (для тестовых NPC на своём сервере).
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_spec.c и пересоберите — больше ничего править не нужно.
 *
 * Команда в чате:
 *   /spec on       включить: клиент начинает слать пакет SPECTATE (ID 212) вместо onfoot/aim/drive
 *   /spec off      выключить: клиент снова шлёт обычные пакеты
 *   /spec          (или /spec status) состояние: флаг, последний отправленный spectate-пакет и последняя позиция onfoot
 *
 * Как это устроено (по дампу игры от 20.10.2026 и дизассемблированию libil2cpp.so):
 *   • LocalPlayer.SendSyncData (RVA 0x12F9640) смотрит на поле LocalPlayer.m_IsSpectating (смещение 0x1D0). Если оно true — зовёт ТОЛЬКО
 *     SendSpectateSyncData (RVA 0x12F9AA4, пакет ID_SPECTATE_SYNC = 212: LeftRightAnalog, UpDownAnalog, Keys, Position) и больше
 *     ничего: ни onfoot (207), ни aim (203), ни drive/passenger не уходят. Если false — обычная ветка.
 *   • ВАЖНО (причина «NPC стоит на месте, чекпоинты не срабатывают»): SendSpectateSyncData НЕ шлёт пакет, пока
 *     Game.MainCamera.gameObject.GetComponent<Spectate>().m_LoadingStatus (Spectate+0x28) != 2 (Loaded) — при другом значении функция сразу
 *     выходит (RVA 0x12F9B68..0x12F9B70: ldr w8,[x0,#0x28]; cmp w8,#2; b.ne ret). Статус 2 игра ставит сама только тогда, когда сервер
 *     реально включил наблюдение за целью (RPC ToggleSpectating + SetTarget). Если менять только m_IsSpectating, onfoot уже не уходит,
 *     а spectate ещё не уходит — на сервер не приходит НИЧЕГО, позиция NPC замерзает. Поэтому /spec on теперь пишет и m_LoadingStatus = 2.
 *     Компонента Spectate на камере может не быть вовсе: игра создаёт его лениво (F.GetOrAddComponent<Spectate>) при первом серверном наблюдении.
 *     Тогда /spec on добавляет его сам (GameObject.AddComponent). Spectate.Update при этом ничего не ломает: пока m_TargetID == 0xFFFF (значение из конструктора) он сразу выходит.
 *   • ПОЗИЦИЯ В ПАКЕТЕ — не позиция игрока, а MainCamera.TargetPosition + (0, 1, 0) (RVA 0x12F9C10..0x12F9C28: x, y+1.0, z).
 *     TargetPosition = transform.position цели камеры (MainCamera.m_Target/m_TargetTransform). Пока камера следит за своим игроком — это
 *     позиция игрока, но сервер увидит её на 1 м выше, чем в onfoot (учтите на сервере или вычтите 1.0 по Y). Если камера на чужой цели
 *     (настоящее наблюдение) — придёт позиция ЦЕЛИ.
 *   • LeftRightAnalog/UpDownAnalog берутся из InputAction (MainControls.Main.MoveX / OnFoot.MoveY), а не из скорости персонажа: если NPC
 *     двигается скриптом/телепортом, а не стиком, оси будут 0 (позиция при этом идёт). Keys — Character.GetKeys() с последующим ResetKeys().
 *   • Поле пишем НАПРЯМУЮ, а не через LocalPlayer.ToggleSpectating(bool): тот при переходе true→false зовёт RequestSpawn() (респавн),
 *     а нам нужен только переключатель выбора пакета. Серверный RPC ToggleSpectating (124) тоже ставит это поле — сервер может выключить режим сам.
 *   • Ничего не патчим и не хукаем: меняются два поля в объектах (LocalPlayer.m_IsSpectating и Spectate.m_LoadingStatus).
 *     Выключение (/spec off) или перезапуск игры возвращает всё как было.
 *   • В машине SendSyncData тоже сначала смотрит на m_IsSpectating, так что с включённым режимом пакеты водителя/пассажира не уходят.
 *   • «/spec status» показывает m_LoadingStatus, позицию цели камеры, последний ушедший spectate-пакет и позицию onfoot. Если после
 *     /spec on и шагов «spectate pos» остаётся 0 0 0 — пакеты не уходят (статус не 2 или NetworkUpdate=false).
 *   • Серверная сторона (проверьте у себя): сервер, получивший 212 вместо 207, может переводить игрока в состояние SPECTATING, а в этом
 *     состоянии чекпоинты обычно не проверяются — это уже не клиентская проблема.
 *   • Журнал — HLOG/konstt.log, строки «spec:».
 */
#pragma once
#include "hassle_core.c"

static int g_spec_modId = -1;

/* LocalPlayer или NULL */
static void* spec_lp(void) {
    void* lp = local_player();
    if (!lp || !hm_obj_ok(lp)) return NULL;
    return lp;
}

/* Указатель на байт LocalPlayer.m_IsSpectating (проверка: поле есть, это bool, смещение разумное, память читается/пишется) или NULL */
static u8* spec_flag(void* lp) {
    void* f; int off;
    f = find_field(A.object_get_class(lp), "m_IsSpectating");
    if (!f) return NULL;
    if (A.type_get_type(A.field_get_type(f)) != 2) return NULL;          /* IL2CPP_TYPE_BOOLEAN */
    off = A.field_get_offset(f);
    if (off < (int)HDR || off > 0x2000 || !hm_readable((char*)lp + off, 1)) return NULL;
    return (u8*)((char*)lp + off);
}

/* ───── Spectate (компонент на объекте Game.MainCamera): поле m_LoadingStatus: None 0, Loading 1, Loaded 2 ───── */
typedef struct {
    void* (*cls_methods)(void*, void**);
    const char* (*m_name)(void*);
    int (*m_pcount)(void*);
    void* (*m_param)(void*, int);
    void* (*cls_type)(void*);
    void* (*type_object)(void*);
} SpecApi;
static SpecApi SP;
static int g_sp_api;                 /* 0 — ещё не искали, 1 — есть, -1 — нет */

static int spec_api(void) {
    if (g_sp_api) return g_sp_api > 0;
    *(void**)&SP.cls_methods = dlsym(g_il2cpp, "il2cpp_class_get_methods");
    *(void**)&SP.m_name      = dlsym(g_il2cpp, "il2cpp_method_get_name");
    *(void**)&SP.m_pcount    = dlsym(g_il2cpp, "il2cpp_method_get_param_count");
    *(void**)&SP.m_param     = dlsym(g_il2cpp, "il2cpp_method_get_param");
    *(void**)&SP.cls_type    = dlsym(g_il2cpp, "il2cpp_class_get_type");
    *(void**)&SP.type_object = dlsym(g_il2cpp, "il2cpp_type_get_object");
    g_sp_api = (SP.cls_methods && SP.m_name && SP.m_pcount && SP.m_param && SP.cls_type && SP.type_object) ? 1 : -1;
    if (g_sp_api < 0) dlog("spec: нет il2cpp_class_get_methods / method_get_param / type_get_object");
    return g_sp_api > 0;
}

/* Game.MainCamera (static) или NULL */
static void* spec_camera(void) {
    void *fn, *cam = NULL;
    if (!g_cGame || !g_img) return NULL;
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) return NULL;
    A.field_static_get_value(fn, &cam);
    return (cam && hm_obj_ok(cam)) ? cam : NULL;
}

/* Метод name(System.Type) с одним параметром: у GetComponent/AddComponent есть и другие перегрузки с тем же числом аргументов
   (string и т.п.), поэтому перебираем методы класса и его предков и сверяем тип параметра */
static void* spec_find_type_method(void* k, const char* name) {
    for (; k; k = A.class_get_parent(k)) {
        void* it = NULL; void* m;
        while ((m = SP.cls_methods(k, &it)) != NULL) {
            const char* nm = SP.m_name(m); const char* tn;
            if (!nm || !seq(nm, name) || SP.m_pcount(m) != 1) continue;
            tn = A.type_get_name(SP.m_param(m, 0));
            if (tn && seq(tn, "System.Type")) return m;
        }
    }
    return NULL;
}

/* Вызов с проверкой исключения; при исключении пишет в журнал класс исключения */
static void* spec_invoke(const char* what, void* m, void* obj, void** args) {
    void* exc = NULL; void* r = A.runtime_invoke(m, obj, args, &exc);
    if (exc) {
        dlog2("spec: исключение в ", what);
        if (hm_obj_ok(exc)) dlog2("spec:   класс исключения ", A.class_get_name(A.object_get_class(exc)));
        return NULL;
    }
    return r;
}

/* Компонент Hassle.Graphics.Spectate на объекте MainCamera или NULL. Сама игра создаёт его лениво (F.GetOrAddComponent<Spectate>) при
   первом серверном «наблюдении», поэтому у игрока, за которым ещё не наблюдали, компонента нет. create=1 — добавить, если его нет.
   Только главный поток (вызывается из обработчика команды). */
static void* spec_component(void** outKlass, int create) {
    void *cam, *kc, *m, *typ, *args[1], *r, *go;
    if (!spec_api()) return NULL;
    cam = spec_camera();
    if (!cam) { dlog("spec: Game.MainCamera = null"); return NULL; }
    kc = A.class_from_name(g_img, "Hassle.Graphics", "Spectate");
    if (!kc) { dlog("spec: класс Hassle.Graphics.Spectate не найден"); return NULL; }
    typ = SP.type_object(SP.cls_type(kc));
    if (!typ) { dlog("spec: не получился System.Type для Spectate"); return NULL; }
    args[0] = typ;
    if (outKlass) *outKlass = kc;
    m = spec_find_type_method(A.object_get_class(cam), "GetComponent");
    if (!m) { dlog("spec: Component.GetComponent(Type) не найден"); return NULL; }
    r = spec_invoke("GetComponent(Spectate)", m, cam, args);
    if (r && hm_obj_ok(r)) return r;
    if (!create) { dlog("spec: на MainCamera пока нет компонента Spectate (игра создаёт его при первом наблюдении)"); return NULL; }
    /* компонента нет — добавляем сами: MainCamera.gameObject.AddComponent(typeof(Spectate)); конструктор ставит m_TargetID = 0xFFFF,
       так что Spectate.Update ничего не делает */
    m = find_method(A.object_get_class(cam), "get_gameObject", 0);
    if (!m) { dlog("spec: Component.get_gameObject не найден"); return NULL; }
    go = spec_invoke("get_gameObject", m, cam, NULL);
    if (!go || !hm_obj_ok(go)) { dlog("spec: get_gameObject вернул null"); return NULL; }
    m = spec_find_type_method(A.object_get_class(go), "AddComponent");
    if (!m) { dlog("spec: GameObject.AddComponent(Type) не найден"); return NULL; }
    r = spec_invoke("AddComponent(Spectate)", m, go, args);
    if (!r || !hm_obj_ok(r)) { dlog("spec: AddComponent(Spectate) вернул null"); return NULL; }
    dlog("spec: компонент Spectate добавлен на MainCamera");
    return r;
}

/* Указатель на int Spectate.m_LoadingStatus или NULL */
static int* spec_status_ptr(void* sp) {
    void* f; int off;
    f = find_field(A.object_get_class(sp), "m_LoadingStatus");
    if (!f) return NULL;
    off = A.field_get_offset(f);
    if (off < (int)HDR || off > 0x200 || !hm_readable((char*)sp + off, 4)) return NULL;
    return (int*)((char*)sp + off);
}

/* MainCamera.TargetPosition (то, что игра кладёт в spectate-пакет, +1 по Y). 1 — прочитали. */
static int spec_cam_target(float* out) {
    void *cam = spec_camera(), *m, *exc = NULL, *r;
    if (!cam) return 0;
    m = find_method(A.object_get_class(cam), "get_TargetPosition", 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, cam, NULL, &exc);
    if (exc || !r) return 0;
    out[0] = ((float*)((char*)r + HDR))[0]; out[1] = ((float*)((char*)r + HDR))[1]; out[2] = ((float*)((char*)r + HDR))[2];
    return 1;
}

/* Последний spectate-пакет игры: LocalPlayer.m_LastSpectateSyncData (value-тип внутри объекта). 1 — прочитали. */
static int spec_read_last(void* lp, unsigned* lr, unsigned* ud, unsigned* keys, float* pos) {
    void *sf, *sc, *f; char* sp; int shift, o;
    sf = find_field(A.object_get_class(lp), "m_LastSpectateSyncData");
    if (!sf) return 0;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return 0;
    sp = (char*)lp + A.field_get_offset(sf);
    shift = struct_shift(sc);
    if (!hm_readable(sp, 0x30)) return 0;
    f = find_field(sc, "LeftRightAnalog"); if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0; *lr = *(u16*)(sp + o);
    f = find_field(sc, "UpDownAnalog");    if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0; *ud = *(u16*)(sp + o);
    f = find_field(sc, "Keys");            if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0; *keys = *(u16*)(sp + o);
    f = find_field(sc, "Position");        if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0;
    {   float* v = (float*)(sp + o); pos[0] = v[0]; pos[1] = v[1]; pos[2] = v[2]; }
    return 1;
}

/* Разбор аргументов (string[]): 1 — on, 2 — off, 3 — sync, 4 — dump, 5 — full (переключить полную трассировку), 0 — статус/ничего.
   Команда сама может входить в массив — не важно. */
static int spec_args(void* arr) {
    long len; int i, cmd = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[48]; int n; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 20 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        if (seq(tmp, "on") || seq(tmp, "1") || seq(tmp, "вкл")) cmd = 1;
        else if (seq(tmp, "off") || seq(tmp, "0") || seq(tmp, "выкл")) cmd = 2;
        else if (seq(tmp, "sync") || seq(tmp, "force") || seq(tmp, "синк")) cmd = 3;
        else if (seq(tmp, "dump") || seq(tmp, "дамп")) cmd = 4;
        else if (seq(tmp, "full") || seq(tmp, "trace") || seq(tmp, "лог")) cmd = 5;
        else if (seq(tmp, "status") || seq(tmp, "статус")) cmd = 0;
    }
    return cmd;
}

static void spec_say_xyz(const char* title, const float* p) {
    char mem[160]; Buf b; b_init(&b, mem, sizeof mem);
    b_s(&b, title); b_fix(&b, p[0], 2); b_s(&b, "  "); b_fix(&b, p[1], 2); b_s(&b, "  "); b_fix(&b, p[2], 2);
    chat_say(b.p);
}

/* ═════════════════════════ ТРАССИРОВКА («максимальное логирование») ═════════════════════════
 * Отдельный файл HLOG/spec_trace.log (прошлый запуск → spec_trace_prev.log, лимит 40000 строк). Не konstt.log: тот ограничен ~4000 строк,
 * и поток опроса выбил бы из него всё остальное. Самые важные события дублируются и в konstt.log (строки «spec:», не больше 150).
 * Поток-трассировщик не вызывает методы игры (никаких runtime_invoke): только читает поля объектов по именам — поэтому безопасен.
 * Что пишется:
 *   START / Δ    полный снимок при запуске и каждое изменение: m_IsSpectating, NetworkUpdate, m_IsCharacterStarted, m_HasSpawnInfo, m_Controller,
 *                Vehicle, интерьер, здоровье, Spectate.{m_LoadingStatus,m_Type,m_TargetID,m_Target}, MainCamera.{m_Mode,m_Target,m_TargetTransform},
 *                Game.Network.State
 *   SEND #n      пакет 212 реально ушёл (изменился LocalPlayer.m_LastSendSyncTick — его пишет только SendSpectateSyncData после отправки):
 *                интервал между отправками, keys/lr/ud и позиция из m_LastSpectateSyncData
 *   ONFOOT       позиция m_LastOnFootSyncData менялась при включённом /spec (должна замёрзнуть — onfoot больше не шлётся)
 *   HB           пульс (раз в 1.5 c при /spec on, раз в 10 c при выкл): весь снимок + число отправок
 *   WATCHDOG     /spec on, а пакетов нет дольше 1.5 c: список конкретных причин (каждое условие взято из дизассемблирования SendSyncData/SendSpectateSyncData)
 *   !!!          режим сбросили не мы: сервер (RPC ToggleSpectating 124 → m_IsSpectating=0 + RequestSpawn), смена Spectate.m_LoadingStatus и т.п.
 *   DUMP / SYNC  по командам /spec dump и /spec sync (см. ниже)
 */
struct sp_ts { long sec, nsec; };
extern int clock_gettime(int, void*);
static long long sp_now_ms(void) {
    struct sp_ts t;
    if (clock_gettime(1, &t)) return 0;                                     /* CLOCK_MONOTONIC */
    return (long long)t.sec * 1000 + t.nsec / 1000000;
}

#define SP_MAX_LINES 40000
static char g_st_path[320];
static int g_st_on, g_st_lines, g_st_mirror;
static volatile int g_st_lock;
static long long g_st_t0;
static volatile int g_st_full;           /* /spec full: строка на каждый опрос */
static volatile int g_sp_wantOn;         /* режим включили мы (/spec on) и он должен держаться */
static void* g_sp_comp;                  /* последний найденный компонент Spectate (его находит только главный поток) */
static int g_sp_sends;                   /* сколько отправок spectate-пакета увидели */
static volatile long long g_sp_lastSend; /* когда увидели последнюю (мс, монотонные) */

static void st_init(void) {
    char dir[300], prev[330]; Buf b; unsigned L, i;
    if (g_st_on) return;
    dlog("spec: трассировка пишется в HLOG/spec_trace.log");                 /* заодно инициализирует путь журнала */
    L = slen(g_flog);
    if (L <= HM_LOGNAME_LEN + 1 || L >= 280) { dlog("spec: путь журнала неизвестен — отдельный файл трассировки не создан"); return; }
    for (i = 0; i < L - HM_LOGNAME_LEN; i++) dir[i] = g_flog[i];
    dir[i] = 0;
    b_init(&b, prev, sizeof prev); b_s(&b, dir); b_s(&b, "spec_trace_prev.log");
    b_init(&b, g_st_path, sizeof g_st_path); b_s(&b, dir); b_s(&b, "spec_trace.log");
    rename(g_st_path, prev);
    if (!flog_write(g_st_path, "", 0, 1)) { dlog("spec: не удалось создать spec_trace.log"); return; }
    g_st_t0 = sp_now_ms();
    g_st_on = 1;
}
static void st_write(const char* line) {
    char t[1100]; Buf b; long long ms; int f;
    if (!g_st_on) return;
    while (__sync_lock_test_and_set(&g_st_lock, 1)) usleep(200);
    if (g_st_lines >= SP_MAX_LINES) { __sync_lock_release(&g_st_lock); return; }
    ms = sp_now_ms() - g_st_t0; f = (int)(ms % 1000);
    b_init(&b, t, sizeof t);
    b_ch(&b, '+'); b_int(&b, (long)(ms / 1000)); b_ch(&b, '.');
    b_ch(&b, (char)('0' + f / 100)); b_ch(&b, (char)('0' + (f / 10) % 10)); b_ch(&b, (char)('0' + f % 10));
    b_s(&b, "  "); b_s(&b, line); b_ch(&b, '\n');
    flog_write(g_st_path, b.p, b.n, 0);
    g_st_lines++;
    if (g_st_lines == SP_MAX_LINES) flog_write(g_st_path, "[лимит строк трассировки достигнут]\n", slen("[лимит строк трассировки достигнут]\n"), 0);
    __sync_lock_release(&g_st_lock);
}
static void sp_log(const char* s) { st_write(s); }
static void sp_key(const char* s) {                    /* в трассировку и (с лимитом) в konstt.log */
    st_write(s);
    if (g_st_mirror < 150) { g_st_mirror++; dlog2("spec: ", s); }
}

/* ───── чтение полей по именам (без вызовов методов); obj уже проверен на lim байт ───── */
static char* sp_fp(void* obj, const char* name, int n, int lim) {
    void* f; int off;
    if (!obj) return NULL;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return NULL;
    off = A.field_get_offset(f);
    if (off < (int)HDR || off + n > lim) return NULL;
    return (char*)obj + off;
}
/* поле fname внутри value-структуры, лежащей в поле sname объекта */
static char* sp_sfield(void* obj, const char* sname, const char* fname, int n, int lim) {
    void *sf, *sc, *f; char* base; int shift, o, so;
    if (!obj) return NULL;
    sf = find_field(A.object_get_class(obj), sname);
    if (!sf) return NULL;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return NULL;
    so = A.field_get_offset(sf);
    shift = struct_shift(sc);
    f = find_field(sc, fname);
    if (!f) return NULL;
    o = A.field_get_offset(f) - shift;
    if (so < (int)HDR || o < 0 || so + o + n > lim) return NULL;
    base = (char*)obj + so;
    return base + o;
}
static int sp_comp_ok(void* c) {
    return c && hm_obj_ok(c) && seq(A.class_get_name(A.object_get_class(c)), "Spectate");
}

typedef struct {
    int lp, isSpec, netUpd, started, spawn, interior, veh, ctrl;
    int hpOk, tickOk, pingOk; float hp, tick; unsigned ping;
    int lsOk; unsigned lsLR, lsUD, lsKeys; float lsPos[3];
    int ofOk; float ofPos[3];
    int comp, status, type, tid, tgtObj;
    int cam, camMode, camTarget, camTransform;
    int netState;
} SpSnap;

#define SP_LP_LIM 0x230
static void sp_snap(SpSnap* s) {
    void *lp, *cam, *net = NULL, *c; char* p;
    __builtin_memset(s, 0, sizeof *s);
    s->isSpec = s->netUpd = s->started = s->spawn = s->interior = s->veh = s->ctrl = -1;
    s->status = s->type = s->tid = s->tgtObj = -1;
    s->camMode = s->camTarget = s->camTransform = s->netState = -1;
    lp = spec_lp();
    if (lp && hm_range_ok(lp, SP_LP_LIM)) {
        s->lp = 1;
        if ((p = sp_fp(lp, "m_IsSpectating", 1, SP_LP_LIM)))       s->isSpec = *(u8*)p;
        if ((p = sp_fp(lp, "NetworkUpdate", 1, SP_LP_LIM)))        s->netUpd = *(u8*)p;
        if ((p = sp_fp(lp, "m_IsCharacterStarted", 1, SP_LP_LIM))) s->started = *(u8*)p;
        if ((p = sp_fp(lp, "m_HasSpawnInfo", 1, SP_LP_LIM)))       s->spawn = *(u8*)p;
        if ((p = sp_fp(lp, "m_Interior", 4, SP_LP_LIM)))           s->interior = *(int*)p;
        if ((p = sp_fp(lp, "Vehicle", (int)sizeof(void*), SP_LP_LIM)))      s->veh = *(void**)p != NULL;
        if ((p = sp_fp(lp, "m_Controller", (int)sizeof(void*), SP_LP_LIM))) s->ctrl = *(void**)p != NULL;
        if ((p = sp_fp(lp, "m_Health", 4, SP_LP_LIM)))             { s->hp = *(float*)p; s->hpOk = 1; }
        if ((p = sp_fp(lp, "Ping", 4, SP_LP_LIM)))                 { s->ping = *(u32*)p; s->pingOk = 1; }
        if ((p = sp_fp(lp, "m_LastSendSyncTick", 4, SP_LP_LIM)))   { s->tick = *(float*)p; s->tickOk = 1; }
        if ((p = sp_sfield(lp, "m_LastSpectateSyncData", "Position", 12, SP_LP_LIM))) { float* v = (float*)p; s->lsPos[0] = v[0]; s->lsPos[1] = v[1]; s->lsPos[2] = v[2]; s->lsOk = 1; }
        if ((p = sp_sfield(lp, "m_LastSpectateSyncData", "LeftRightAnalog", 2, SP_LP_LIM))) s->lsLR = *(u16*)p;
        if ((p = sp_sfield(lp, "m_LastSpectateSyncData", "UpDownAnalog", 2, SP_LP_LIM)))    s->lsUD = *(u16*)p;
        if ((p = sp_sfield(lp, "m_LastSpectateSyncData", "Keys", 2, SP_LP_LIM)))            s->lsKeys = *(u16*)p;
        if ((p = sp_sfield(lp, "m_LastOnFootSyncData", "Position", 12, SP_LP_LIM))) { float* v = (float*)p; s->ofPos[0] = v[0]; s->ofPos[1] = v[1]; s->ofPos[2] = v[2]; s->ofOk = 1; }
    }
    c = g_sp_comp;
    if (sp_comp_ok(c) && hm_range_ok(c, 0x30)) {
        s->comp = 1;
        if ((p = sp_fp(c, "m_LoadingStatus", 4, 0x30))) s->status = *(int*)p;
        if ((p = sp_fp(c, "m_Type", 4, 0x30)))          s->type = *(int*)p;
        if ((p = sp_fp(c, "m_TargetID", 2, 0x30)))      s->tid = *(u16*)p;
        if ((p = sp_fp(c, "m_Target", (int)sizeof(void*), 0x30))) s->tgtObj = *(void**)p != NULL;
    }
    cam = spec_camera();
    if (cam && hm_range_ok(cam, 0x160)) {
        s->cam = 1;
        if ((p = sp_fp(cam, "m_Mode", 4, 0x160)))                                    s->camMode = *(int*)p;
        if ((p = sp_fp(cam, "m_Target", (int)sizeof(void*), 0x160)))                 s->camTarget = *(void**)p != NULL;
        if ((p = sp_fp(cam, "m_TargetTransform", (int)sizeof(void*), 0x160)))        s->camTransform = *(void**)p != NULL;
    }
    if (g_cGame && g_img) {
        void* fn = A.class_get_field_from_name(g_cGame, "Network");
        if (fn) A.field_static_get_value(fn, &net);
        if (net && hm_range_ok(net, 0x40) && (p = sp_fp(net, "State", 4, 0x40))) s->netState = *(int*)p;
    }
}

#define F_I(nm, v) do { b_s(o, " " nm "="); b_int(o, (long)(v)); } while (0)
#define F_F(nm, v, d) do { b_s(o, " " nm "="); b_fix(o, (double)(v), d); } while (0)
static void sp_fmt(const SpSnap* s, Buf* o) {
    F_I("lp", s->lp); F_I("m_IsSpectating", s->isSpec); F_I("NetworkUpdate", s->netUpd); F_I("m_IsCharacterStarted", s->started);
    F_I("m_HasSpawnInfo", s->spawn); F_I("m_Controller", s->ctrl); F_I("Vehicle", s->veh); F_I("interior", s->interior);
    if (s->hpOk) F_F("hp", s->hp, 1);
    if (s->pingOk) F_I("ping", s->ping);
    b_s(o, " | Spectate:"); F_I("comp", s->comp); F_I("m_LoadingStatus", s->status); F_I("m_Type", s->type); F_I("m_TargetID", s->tid); F_I("m_Target", s->tgtObj);
    b_s(o, " | MainCamera:"); F_I("есть", s->cam); F_I("m_Mode", s->camMode); F_I("m_Target", s->camTarget); F_I("m_TargetTransform", s->camTransform);
    b_s(o, " | Game.Network.State="); b_int(o, (long)s->netState);
    if (s->tickOk) { b_s(o, " | last212: tick="); b_fix(o, s->tick, 3); }
    if (s->lsOk) { b_s(o, " keys="); b_int(o, (long)s->lsKeys); b_s(o, " lr="); b_int(o, (long)s->lsLR); b_s(o, " ud="); b_int(o, (long)s->lsUD);
                   b_s(o, " pos="); b_fix(o, s->lsPos[0], 2); b_s(o, " "); b_fix(o, s->lsPos[1], 2); b_s(o, " "); b_fix(o, s->lsPos[2], 2); }
    if (s->ofOk) { b_s(o, " | onfoot pos="); b_fix(o, s->ofPos[0], 2); b_s(o, " "); b_fix(o, s->ofPos[1], 2); b_s(o, " "); b_fix(o, s->ofPos[2], 2); }
}

static int sp_feq(float a, float b) { return a == b || (a != a && b != b); }
#define D_I(fld, nm) do { if (a->fld != b->fld) { b_s(o, " " nm ":"); b_int(o, (long)a->fld); b_s(o, "->"); b_int(o, (long)b->fld); n++; } } while (0)
/* изменения без тика/позиций/пинга (их показывают SEND и HB) */
static int sp_diff(const SpSnap* a, const SpSnap* b, Buf* o) {
    int n = 0;
    D_I(lp, "lp"); D_I(isSpec, "m_IsSpectating"); D_I(netUpd, "NetworkUpdate"); D_I(started, "m_IsCharacterStarted"); D_I(spawn, "m_HasSpawnInfo");
    D_I(ctrl, "m_Controller"); D_I(veh, "Vehicle"); D_I(interior, "interior");
    if (!sp_feq(a->hp, b->hp)) { b_s(o, " hp:"); b_fix(o, a->hp, 1); b_s(o, "->"); b_fix(o, b->hp, 1); n++; }
    D_I(comp, "Spectate.comp"); D_I(status, "Spectate.m_LoadingStatus"); D_I(type, "Spectate.m_Type"); D_I(tid, "Spectate.m_TargetID"); D_I(tgtObj, "Spectate.m_Target");
    D_I(cam, "камера"); D_I(camMode, "cam.m_Mode"); D_I(camTarget, "cam.m_Target"); D_I(camTransform, "cam.m_TargetTransform"); D_I(netState, "Network.State");
    return n;
}

/* Почему при включённом /spec пакет 212 не уходит. Каждое условие — из дизассемблирования (RVA в скобках):
   FixedUpdate (0x12F962C): если Actor.m_Controller (+0x50) == null — ничего не шлёт;
   SendSyncData (0x12F9640): выходит, если m_IsCharacterStarted == false; при m_IsSpectating зовёт SendSpectateSyncData;
   SendSpectateSyncData (0x12F9AA4): выход, если NetworkUpdate == false и force == false; NRE, если нет Game.MainCamera / компонента Spectate / m_Controller;
   тихий выход при Spectate.m_LoadingStatus != 2; MainCamera.get_TargetPosition (0x123C188): NRE, если m_Target != null, а m_TargetTransform == null.
   Возвращает маску причин; текст дописывает в why. */
static unsigned sp_gate(const SpSnap* s, Buf* why) {
    unsigned m = 0;
    if (!s->lp)               { m |= 1;   b_s(why, " [LocalPlayer не найден]"); return m; }
    if (s->ctrl == 0)         { m |= 2;   b_s(why, " [Actor.m_Controller=null: LocalPlayer.FixedUpdate ничего не шлёт / NRE в SendSpectateSyncData]"); }
    if (s->started == 0)      { m |= 4;   b_s(why, " [m_IsCharacterStarted=false: SendSyncData выходит сразу]"); }
    if (s->netUpd == 0)       { m |= 8;   b_s(why, " [NetworkUpdate=false: FixedUpdate зовёт SendSyncData(force=false) — выход (обычно сервер/загрузка выключили сетевое обновление)]"); }
    if (!s->comp)             { m |= 16;  b_s(why, " [компонента Spectate нет или он уничтожен: GetComponent=null → NRE]"); }
    else if (s->status != 2)  { m |= 32;  b_s(why, " [Spectate.m_LoadingStatus!=2: SendSpectateSyncData молча выходит]"); }
    if (!s->cam)              { m |= 64;  b_s(why, " [Game.MainCamera=null → NRE]"); }
    else if (s->camTarget == 1 && s->camTransform == 0) { m |= 128; b_s(why, " [у камеры m_Target есть, а m_TargetTransform=null → NRE в get_TargetPosition]"); }
    if (s->netState != -1 && s->netState != 2) { m |= 256; b_s(why, " [Game.Network.State!=Connected(2)]"); }
    return m;
}

static void sp_send_line(const SpSnap* s, long long dt, int n) {
    char mem[400]; Buf b; b_init(&b, mem, sizeof mem);
    b_s(&b, "SEND #"); b_int(&b, n);
    b_s(&b, " tick="); b_fix(&b, s->tick, 3);
    b_s(&b, " dt="); b_int(&b, (long)dt); b_s(&b, "мс keys="); b_int(&b, (long)s->lsKeys); b_s(&b, " lr="); b_int(&b, (long)s->lsLR); b_s(&b, " ud="); b_int(&b, (long)s->lsUD);
    b_s(&b, " pos="); b_fix(&b, s->lsPos[0], 2); b_s(&b, " "); b_fix(&b, s->lsPos[1], 2); b_s(&b, " "); b_fix(&b, s->lsPos[2], 2);
    sp_log(b.p);
}

/* Полный снимок по запросу (главный поток: можно звать методы). why — подпись в журнале. Возвращает маску причин. */
static unsigned sp_dump(void* lp, const char* why) {
    SpSnap s; char mem[1100]; Buf b; float cp[3]; unsigned m;
    (void)lp;
    { void* c = spec_component(NULL, 0); if (c) g_sp_comp = c; }
    sp_snap(&s);
    b_init(&b, mem, sizeof mem); b_s(&b, "DUMP ["); b_s(&b, why); b_s(&b, "]"); sp_fmt(&s, &b); sp_key(b.p);
    if (spec_cam_target(cp)) {
        b_init(&b, mem, sizeof mem); b_s(&b, "DUMP камера: get_TargetPosition = "); b_fix(&b, cp[0], 2); b_s(&b, " "); b_fix(&b, cp[1], 2); b_s(&b, " "); b_fix(&b, cp[2], 2);
        b_s(&b, "  (в пакет 212 уйдёт Y+1)"); sp_log(b.p);
    } else sp_log("DUMP камера: get_TargetPosition прочитать не удалось (исключение или нет камеры)");
    b_init(&b, mem, sizeof mem); b_s(&b, "DUMP условия отправки:");
    m = sp_gate(&s, &b);
    if (!m) b_s(&b, " все условия выполнены");
    sp_key(b.p);
    return m;
}

/* /spec sync: принудительно зовём LocalPlayer.SendSyncData(force=true) и смотрим, ушёл ли пакет (изменился ли m_LastSendSyncTick).
   Это самый прямой ответ на вопрос «работает ли клиентская часть»: если тик сдвинулся — игра отправила пакет, и дальше смотреть надо сервер. */
static void spec_do_sync(void* lp) {
    SpSnap a, b2; void *m, *exc = NULL, *args[1]; u8 force = 1; char mem[600]; Buf b; unsigned g;
    { void* c = spec_component(NULL, 0); if (c) g_sp_comp = c; }
    sp_snap(&a);
    m = find_method(A.object_get_class(lp), "SendSyncData", 1);
    if (!m) { chat_say("spec: метод LocalPlayer.SendSyncData(bool) не найден"); sp_key("SYNC: метод SendSyncData(bool) не найден"); return; }
    b_init(&b, mem, sizeof mem); b_s(&b, "SYNC: вызываю LocalPlayer.SendSyncData(force=true), до вызова:"); sp_fmt(&a, &b); sp_log(b.p);
    args[0] = &force;
    A.runtime_invoke(m, lp, args, &exc);
    if (exc) {
        const char* cn = hm_obj_ok(exc) ? A.class_get_name(A.object_get_class(exc)) : "?";
        dlog2("spec: SendSyncData бросил исключение ", cn);
        b_init(&b, mem, sizeof mem); b_s(&b, "SYNC: ИСКЛЮЧЕНИЕ внутри SendSyncData: "); b_s(&b, cn); sp_key(b.p);
        chat_say("spec: sync — внутри игры исключение (класс в konstt.log / spec_trace.log)");
    }
    sp_snap(&b2);
    b_init(&b, mem, sizeof mem); b_s(&b, "SYNC: после вызова:"); sp_fmt(&b2, &b); sp_log(b.p);
    if (a.tickOk && b2.tickOk && a.tick != b2.tick) {
        b_init(&b, mem, sizeof mem); b_s(&b, "SYNC: ПАКЕТ УШЁЛ (m_LastSendSyncTick "); b_fix(&b, a.tick, 3); b_s(&b, " -> "); b_fix(&b, b2.tick, 3); b_s(&b, ") — клиент свою часть сделал, дальше смотрите сервер");
        sp_key(b.p); chat_say("spec: sync — пакет ушёл (тик сдвинулся). Клиент работает; если на сервере ничего — причина на стороне сервера");
        if (b2.lsOk) spec_say_xyz("spec: ушло pos: ", b2.lsPos);
    } else {
        b_init(&b, mem, sizeof mem); b_s(&b, "SYNC: тик НЕ изменился — пакет не ушёл. Причины:");
        g = sp_gate(&b2, &b); if (!g) b_s(&b, " по полям всё в порядке (проверьте, что /spec on включён: режим выкл шлёт onfoot, а не 212, и тик не трогает)");
        if (b2.isSpec == 0) b_s(&b, " [m_IsSpectating=0: сейчас идёт обычный onfoot, spectate-тик не меняется]");
        sp_key(b.p); chat_say("spec: sync — пакет НЕ ушёл, причины в HLOG/spec_trace.log (строка SYNC)");
    }
}

static void* spec_trace_thread(void* arg) {
    SpSnap cur, prev; int have = 0, wasSpec = 0; long long now, lastHb = 0, onSince = 0, lastWd = 0; unsigned prevMask = 0; char mem[1100]; Buf b;
    (void)arg;
    __builtin_memset(&prev, 0, sizeof prev);
    if (!hm_wait_ready()) return NULL;
    ensure_attached();
    st_init();
    for (;;) {
        now = sp_now_ms();
        sp_snap(&cur);
        if (!have) {
            b_init(&b, mem, sizeof mem); b_s(&b, "START"); sp_fmt(&cur, &b); sp_key(b.p);
            prev = cur; have = 1; lastHb = now;
        } else {
            /* 1. изменения полей */
            b_init(&b, mem, sizeof mem); b_s(&b, "Δ");
            if (sp_diff(&prev, &cur, &b)) {
                sp_key(b.p);
                /* кто-то выключил режим / сбросил статус не по нашей команде */
                if (g_sp_wantOn) {
                    if (prev.isSpec == 1 && cur.isSpec == 0) {
                        sp_key("!!! m_IsSpectating сброшен НЕ нами: игра/сервер выключили режим (серверный RPC ToggleSpectating=124 → при false игра делает RequestSpawn). Пакеты 212 прекращены");
                        g_sp_wantOn = 0;
                    } else if (prev.status == 2 && cur.status != 2 && cur.status != -1) {
                        sp_key("!!! Spectate.m_LoadingStatus изменили (был 2): SendSpectateSyncData снова молча выходит");
                    } else if (prev.comp == 1 && cur.comp == 0) {
                        sp_key("!!! компонент Spectate на MainCamera пропал (уничтожен игрой)");
                    }
                    if (prev.started == 1 && cur.started == 0) sp_key("!!! m_IsCharacterStarted стал false: персонаж пересоздаётся (респавн) — sync не идёт");
                }
            }
            /* 2. отправки spectate-пакета */
            if (cur.tickOk && prev.tickOk && cur.tick != prev.tick) {
                g_sp_sends++;
                sp_send_line(&cur, g_sp_lastSend ? now - g_sp_lastSend : 0, g_sp_sends);
                g_sp_lastSend = now;
            } else if (cur.lsOk && prev.lsOk && (!sp_feq(cur.lsPos[0], prev.lsPos[0]) || !sp_feq(cur.lsPos[1], prev.lsPos[1]) || !sp_feq(cur.lsPos[2], prev.lsPos[2]))) {
                b_init(&b, mem, sizeof mem); b_s(&b, "DATA позиция в m_LastSpectateSyncData изменилась без смены тика:"); sp_fmt(&cur, &b); sp_log(b.p);
            }
            /* 3. onfoot при включённом spec должен замёрзнуть */
            if (cur.isSpec == 1 && cur.ofOk && prev.ofOk && (!sp_feq(cur.ofPos[0], prev.ofPos[0]) || !sp_feq(cur.ofPos[1], prev.ofPos[1]) || !sp_feq(cur.ofPos[2], prev.ofPos[2]))) {
                b_init(&b, mem, sizeof mem); b_s(&b, "ONFOOT позиция m_LastOnFootSyncData всё ещё меняется при включённом /spec: ");
                b_fix(&b, cur.ofPos[0], 2); b_s(&b, " "); b_fix(&b, cur.ofPos[1], 2); b_s(&b, " "); b_fix(&b, cur.ofPos[2], 2); sp_log(b.p);
            }
        }
        /* 4. вход/выход из режима */
        if (cur.isSpec == 1 && !wasSpec) { onSince = now; prevMask = 0; lastWd = 0; sp_key("режим spec ВКЛЮЧЁН (m_IsSpectating=1): с этого момента ждём пакеты 212"); }
        if (cur.isSpec != 1 && wasSpec)  sp_key("режим spec ВЫКЛЮЧЕН (m_IsSpectating!=1)");
        wasSpec = cur.isSpec == 1;
        /* 5. сторож: spec включён, а пакетов нет */
        if (cur.isSpec == 1) {
            long long ref = g_sp_lastSend > onSince ? g_sp_lastSend : onSince;
            if (now - ref > 1500) {
                Buf w; char wm[800]; unsigned mask;
                b_init(&w, wm, sizeof wm); b_s(&w, "WATCHDOG: spec включён, но пакетов 212 нет уже "); b_int(&w, (long)((now - ref) / 1000)); b_s(&w, " c. Причины:");
                mask = sp_gate(&cur, &w);
                if (!mask) b_s(&w, " ни одно условие из дизассемблирования не нарушено → вероятно, исключение внутри SendSpectateSyncData (смотрите logcat Unity) или данные не менялись и тик не обновился");
                if (mask != prevMask || now - lastWd > 5000) { sp_key(w.p); lastWd = now; prevMask = mask; }
            }
        }
        /* 6. пульс */
        if (now - lastHb >= (cur.isSpec == 1 ? 1500 : 10000)) {
            b_init(&b, mem, sizeof mem); b_s(&b, "HB sends="); b_int(&b, g_sp_sends);
            b_s(&b, " последний_SEND_назад="); if (g_sp_lastSend) { b_int(&b, (long)((now - g_sp_lastSend) / 1000)); b_s(&b, "c"); } else b_s(&b, "никогда");
            sp_fmt(&cur, &b); sp_log(b.p); lastHb = now;
        } else if (g_st_full) {
            b_init(&b, mem, sizeof mem); b_s(&b, "POLL"); sp_fmt(&cur, &b); sp_log(b.p);
        }
        prev = cur;
        usleep(g_st_full ? 50000 : (cur.isSpec == 1 ? 100000 : 250000));
    }
    return NULL;
}

static void spec_status(void* lp) {
    u8* fl = spec_flag(lp);
    unsigned lr = 0, ud = 0, keys = 0; float sp[3] = {0, 0, 0}, op[3] = {0, 0, 0};
    chat_say(!fl ? "spec: поле m_IsSpectating не найдено" : *fl ? "spec: ВКЛ (шлётся spectate, ID 212)" : "spec: выкл (обычные пакеты)");
    {   void* sc = spec_component(NULL, 0); int* st = sc ? spec_status_ptr(sc) : NULL;
        char mem[96]; Buf b; b_init(&b, mem, sizeof mem);
        if (sc) g_sp_comp = sc;
        b_s(&b, "Spectate.m_LoadingStatus = ");
        if (!st) b_s(&b, "? (компонента Spectate ещё нет — появится после /spec on)"); else { b_int(&b, (long)*st); b_s(&b, *st == 2 ? " (Loaded — пакеты идут)" : " (НЕ 2 — spectate-пакет не уходит!)"); }
        chat_say(b.p);
        if (spec_cam_target(sp)) spec_say_xyz("cam target:   ", sp);
    }
    if (spec_read_last(lp, &lr, &ud, &keys, sp)) {
        char mem[160]; Buf b; b_init(&b, mem, sizeof mem);
        b_s(&b, "spectate: keys="); b_int(&b, (long)keys); b_s(&b, " lr="); b_int(&b, (long)lr); b_s(&b, " ud="); b_int(&b, (long)ud);
        chat_say(b.p);
        spec_say_xyz("spectate pos: ", sp);
    } else chat_say("spectate: последнего пакета не прочитать (поле m_LastSpectateSyncData не найдено)");
    if (read_pos(lp, op)) spec_say_xyz("onfoot pos:   ", op);
    {   char mem[160]; Buf b; b_init(&b, mem, sizeof mem);
        b_s(&b, "отправок 212 замечено: "); b_int(&b, g_sp_sends); b_s(&b, g_sp_lastSend ? ", последняя " : ", последней не было");
        if (g_sp_lastSend) { b_int(&b, (long)((sp_now_ms() - g_sp_lastSend) / 1000)); b_s(&b, " c назад"); }
        chat_say(b.p);
    }
    sp_dump(lp, "/spec status");                              /* полный снимок + причины в HLOG/spec_trace.log */
}

/* Вызывается игрой в главном потоке при вводе /spec [on|off|status|sync|dump|full] */
static void native_spec_cmd(void* self, void* args, void* mi) {
    void* lp; u8* fl; int cmd;
    (void)self; (void)mi;
    cmd = spec_args(args);
    lp = spec_lp();
    { char mem[64]; Buf b; b_init(&b, mem, sizeof mem); b_s(&b, "КОМАНДА /spec код="); b_int(&b, cmd); b_s(&b, lp ? "" : " (игрока ещё нет)"); sp_key(b.p); }
    if (!lp) { chat_say("spec: игрок ещё не появился"); return; }
    if (cmd == 0) { spec_status(lp); chat_usage("Используйте: /spec on | off | status | sync | dump | full"); return; }
    if (cmd == 3) { spec_do_sync(lp); return; }
    if (cmd == 4) { sp_dump(lp, "/spec dump"); chat_say("spec: полный снимок записан в HLOG/spec_trace.log (строки DUMP)"); return; }
    if (cmd == 5) {
        g_st_full = !g_st_full;
        chat_say(g_st_full ? "spec: полная трассировка ВКЛ (строка POLL 20 раз в секунду, лимит 40000 строк)" : "spec: полная трассировка выкл (только изменения, SEND и пульс)");
        sp_key(g_st_full ? "полная трассировка ВКЛ" : "полная трассировка выкл");
        return;
    }
    fl = spec_flag(lp);
    if (!fl) { chat_say("spec: поле m_IsSpectating не найдено (смотрите konstt.log)"); dlog("spec: нет поля LocalPlayer.m_IsSpectating или оно не bool"); return; }
    if (cmd == 1) {
        void* sc;
        sp_dump(lp, "перед /spec on");
        sc = spec_component(NULL, 1);
        if (sc) g_sp_comp = sc;
        {   int* st = sc ? spec_status_ptr(sc) : NULL;
            if (!st) { chat_say("spec: не нашёл Spectate.m_LoadingStatus — без него пакет не уйдёт, режим НЕ включён (см. konstt.log)"); sp_key("/spec on: Spectate.m_LoadingStatus не найден — режим не включён"); return; }
            *st = 2;                                        /* Loaded: иначе SendSpectateSyncData молча выходит и на сервер не уходит ничего */
        }
        g_sp_wantOn = 1;
        *fl = 1;                                            /* НЕ ToggleSpectating: тот при выключении делает RequestSpawn */
        chat_say("spec: ВКЛ — вместо onfoot/aim/drive уходит spectate (ID 212)");
        dlog("spec: Spectate.m_LoadingStatus = 2, m_IsSpectating = 1");
        sp_dump(lp, "после /spec on");
    } else {
        void* sc = spec_component(NULL, 0); int* st = sc ? spec_status_ptr(sc) : NULL;
        sp_dump(lp, "перед /spec off");
        g_sp_wantOn = 0;
        *fl = 0;
        if (st && *st == 2) *st = 0;                        /* вернуть как было (None) */
        chat_say("spec: выкл — снова обычные пакеты");
        dlog("spec: m_IsSpectating = 0, Spectate.m_LoadingStatus = 0");
        sp_dump(lp, "после /spec off");
    }
}

static void* spec_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("spec", native_spec_cmd)) return NULL;
    hm_mod_ready(g_spec_modId);
    dlog("spec: мод загружен, команда /spec зарегистрирована");
    return spec_trace_thread(NULL);                         /* этот же поток дальше работает трассировщиком (бесконечный цикл) */
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void spec_ctor(void) {
    unsigned long t = 0;
    g_spec_modId = hm_mod_register("spec");
    pthread_create(&t, NULL, spec_main, NULL);
}
#endif
