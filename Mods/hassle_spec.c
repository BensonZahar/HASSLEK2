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

/* Разбор аргументов (string[]): 1 — on, 2 — off, 0 — статус/ничего. Команда сама может входить в массив — не важно. */
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
        else if (seq(tmp, "status") || seq(tmp, "статус")) cmd = 0;
    }
    return cmd;
}

static void spec_say_xyz(const char* title, const float* p) {
    char mem[160]; Buf b; b_init(&b, mem, sizeof mem);
    b_s(&b, title); b_fix(&b, p[0], 2); b_s(&b, "  "); b_fix(&b, p[1], 2); b_s(&b, "  "); b_fix(&b, p[2], 2);
    chat_say(b.p);
}

static void spec_status(void* lp) {
    u8* fl = spec_flag(lp);
    unsigned lr = 0, ud = 0, keys = 0; float sp[3] = {0, 0, 0}, op[3] = {0, 0, 0};
    chat_say(!fl ? "spec: поле m_IsSpectating не найдено" : *fl ? "spec: ВКЛ (шлётся spectate, ID 212)" : "spec: выкл (обычные пакеты)");
    {   void* sc = spec_component(NULL, 0); int* st = sc ? spec_status_ptr(sc) : NULL;
        char mem[96]; Buf b; b_init(&b, mem, sizeof mem);
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
}

/* Вызывается игрой в главном потоке при вводе /spec [on|off|status] */
static void native_spec_cmd(void* self, void* args, void* mi) {
    void* lp; u8* fl; int cmd;
    (void)self; (void)mi;
    cmd = spec_args(args);
    lp = spec_lp();
    if (!lp) { chat_say("spec: игрок ещё не появился"); return; }
    if (cmd == 0) { spec_status(lp); chat_usage("Используйте: /spec on | /spec off | /spec status"); return; }
    fl = spec_flag(lp);
    if (!fl) { chat_say("spec: поле m_IsSpectating не найдено (смотрите konstt.log)"); dlog("spec: нет поля LocalPlayer.m_IsSpectating или оно не bool"); return; }
    if (cmd == 1) {
        void* sc = spec_component(NULL, 1); int* st = sc ? spec_status_ptr(sc) : NULL;
        if (!st) { chat_say("spec: не нашёл Spectate.m_LoadingStatus — без него пакет не уйдёт, режим НЕ включён (см. konstt.log)"); return; }
        *st = 2;                                            /* Loaded: иначе SendSpectateSyncData молча выходит и на сервер не уходит ничего */
        *fl = 1;                                            /* НЕ ToggleSpectating: тот при выключении делает RequestSpawn */
        chat_say("spec: ВКЛ — вместо onfoot/aim/drive уходит spectate (ID 212)");
        dlog("spec: Spectate.m_LoadingStatus = 2, m_IsSpectating = 1");
    } else {
        void* sc = spec_component(NULL, 0); int* st = sc ? spec_status_ptr(sc) : NULL;
        *fl = 0;
        if (st && *st == 2) *st = 0;                        /* вернуть как было (None) */
        chat_say("spec: выкл — снова обычные пакеты");
        dlog("spec: m_IsSpectating = 0, Spectate.m_LoadingStatus = 0");
    }
}

static void* spec_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("spec", native_spec_cmd)) return NULL;
    hm_mod_ready(g_spec_modId);
    dlog("spec: мод загружен, команда /spec зарегистрирована");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void spec_ctor(void) {
    unsigned long t = 0;
    g_spec_modId = hm_mod_register("spec");
    pthread_create(&t, NULL, spec_main, NULL);
}
#endif
