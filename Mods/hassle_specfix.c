/*
 * hassle_specfix.c — «заморозка» тестового NPC: видим только серверу, другие игроки
 * его не видят и не натыкаются, но PayDay и таймерные системы работают нормально.
 *
 * ПРИНЦИП
 * Сервер зовёт TogglePlayerSpectating(npcid, 1) → клиент получает RPC → ставит
 * m_IsSpectating = 1. ПРОБЛЕМА: SendSpectateSyncData проверяет Spectate.m_LoadingStatus;
 * если оно не равно 2 (Loaded) — метод молча выходит, ни один пакет не уходит,
 * сервер через ~15 с ставит AFK. Мод чинит это: как только m_IsSpectating = 1,
 * ставим m_LoadingStatus = 2. Теперь пакет 212 уходит с позицией камеры (= позиция NPC),
 * OnPlayerUpdate сбрасывает таймер AFK, PayDay работает. NPC невидим для других.
 *
 * ИСПОЛЬЗОВАНИЕ
 *   1. Запустить NPC, дождаться загрузки.
 *   2. /specfix on  — один раз из главного потока: кэшируем Spectate-компонент
 *                     (только отсюда можно безопасно GetComponent/AddComponent).
 *   3. Сервер ставит TogglePlayerSpectating(npcid, 1) — мод автоматически держит
 *      m_LoadingStatus = 2 каждые 500 мс.
 *   4. /specfix off — выключить мониторинг (NPC снова сможет замёрзнуть).
 *
 * Зависит только от hassle_core.c. КАК УБРАТЬ: удалите файл, пересоберите.
 * Лог — konstt.log, строки «sfx:».
 * Серверная часть — hassle_pawn_npc.pwn (вставить в new.pwn).
 *
 * По дампу libil2cpp.so от 20.10.2026.
 */
#pragma once
#include "hassle_core.c"

static int   g_sfx_modId = -1;
static volatile int  g_sfx_on = 0;          /* 0 = мониторинг выкл., 1 = вкл. */
static int* volatile g_sfx_st  = NULL;      /* указатель на Spectate.m_LoadingStatus (кэш) */
static u8*  volatile g_sfx_fl  = NULL;      /* указатель на LocalPlayer.m_IsSpectating (кэш) */

/* ─── GetComponent / AddComponent через IL2CPP (только главный поток) ─── */

typedef struct {
    void* (*cls_methods)(void*, void**);
    const char* (*m_name)(void*);
    int   (*m_pcount)(void*);
    void* (*m_param)(void*, int);
    void* (*cls_type)(void*);
    void* (*type_object)(void*);
} SfxApi;
static SfxApi SF;
static int g_sfx_api_ok; /* 0=не инициализировали, 1=ok, -1=нет символов */

static int sfx_api(void) {
    if (g_sfx_api_ok) return g_sfx_api_ok > 0;
    *(void**)&SF.cls_methods = dlsym(g_il2cpp, "il2cpp_class_get_methods");
    *(void**)&SF.m_name      = dlsym(g_il2cpp, "il2cpp_method_get_name");
    *(void**)&SF.m_pcount    = dlsym(g_il2cpp, "il2cpp_method_get_param_count");
    *(void**)&SF.m_param     = dlsym(g_il2cpp, "il2cpp_method_get_param");
    *(void**)&SF.cls_type    = dlsym(g_il2cpp, "il2cpp_class_get_type");
    *(void**)&SF.type_object = dlsym(g_il2cpp, "il2cpp_type_get_object");
    g_sfx_api_ok = (SF.cls_methods && SF.m_name && SF.m_pcount &&
                    SF.m_param && SF.cls_type && SF.type_object) ? 1 : -1;
    if (g_sfx_api_ok < 0) dlog("sfx: не нашли il2cpp_class_get_methods/method_get_param");
    return g_sfx_api_ok > 0;
}

static void* sfx_type_method(void* k, const char* name) {
    for (; k; k = A.class_get_parent(k)) {
        void* it = NULL; void* m;
        while ((m = SF.cls_methods(k, &it)) != NULL) {
            const char* nm = SF.m_name(m);
            if (!nm || !seq(nm, name) || SF.m_pcount(m) != 1) continue;
            const char* tn = A.type_get_name(SF.m_param(m, 0));
            if (tn && seq(tn, "System.Type")) return m;
        }
    }
    return NULL;
}

static void* sfx_camera(void) {
    void *fn, *cam = NULL;
    if (!g_cGame || !g_img) return NULL;
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) return NULL;
    A.field_static_get_value(fn, &cam);
    return (cam && hm_obj_ok(cam)) ? cam : NULL;
}

/*
 * Возвращает компонент Hassle.Graphics.Spectate на MainCamera.
 * create=1: добавить, если ещё нет (игра создаёт лениво при первом серверном наблюдении).
 * ТОЛЬКО ГЛАВНЫЙ ПОТОК — GetComponent и AddComponent требуют Unity main thread.
 */
static void* sfx_component(int create) {
    void *cam, *kc, *m, *typ, *args[1], *r, *go, *exc = NULL;
    if (!sfx_api()) return NULL;
    cam = sfx_camera();
    if (!cam) { dlog("sfx: sfx_component: MainCamera = null"); return NULL; }
    kc = A.class_from_name(g_img, "Hassle.Graphics", "Spectate");
    if (!kc) { dlog("sfx: класс Hassle.Graphics.Spectate не найден"); return NULL; }
    typ = SF.type_object(SF.cls_type(kc));
    if (!typ) { dlog("sfx: не получился System.Type для Spectate"); return NULL; }
    args[0] = typ;
    m = sfx_type_method(A.object_get_class(cam), "GetComponent");
    if (!m) { dlog("sfx: GetComponent(Type) не найден"); return NULL; }
    r = A.runtime_invoke(m, cam, args, &exc);
    if (r && hm_obj_ok(r)) return r;
    if (!create) return NULL;
    exc = NULL;
    m = find_method(A.object_get_class(cam), "get_gameObject", 0);
    if (!m) { dlog("sfx: get_gameObject не найден"); return NULL; }
    go = A.runtime_invoke(m, cam, NULL, &exc);
    if (!go || !hm_obj_ok(go)) { dlog("sfx: get_gameObject → null"); return NULL; }
    m = sfx_type_method(A.object_get_class(go), "AddComponent");
    if (!m) { dlog("sfx: AddComponent(Type) не найден"); return NULL; }
    exc = NULL;
    r = A.runtime_invoke(m, go, args, &exc);
    if (!r || !hm_obj_ok(r)) { dlog("sfx: AddComponent(Spectate) не вышло"); return NULL; }
    dlog("sfx: Spectate-компонент добавлен на MainCamera");
    return r;
}

/*
 * Кэшируем g_sfx_st и g_sfx_fl из главного потока.
 * Возврат: 1 — всё ок, 0 — ошибка.
 */
static int sfx_cache(void) {
    void* lp = local_player();
    if (!lp || !hm_obj_ok(lp)) { dlog("sfx: sfx_cache: LocalPlayer = null"); return 0; }

    /* m_IsSpectating */
    {
        void* f = find_field(A.object_get_class(lp), "m_IsSpectating");
        int off;
        if (!f) { dlog("sfx: поле m_IsSpectating не найдено"); return 0; }
        if (A.type_get_type(A.field_get_type(f)) != 2) { dlog("sfx: m_IsSpectating не bool"); return 0; }
        off = A.field_get_offset(f);
        if (off < (int)HDR || off > 0x2000 || !hm_readable((char*)lp + off, 1))
            { dlog("sfx: m_IsSpectating нечитаем"); return 0; }
        __atomic_store_n(&g_sfx_fl, (u8*)((char*)lp + off), __ATOMIC_RELEASE);
    }

    /* Spectate.m_LoadingStatus */
    {
        void* sp = sfx_component(1);  /* главный поток → безопасно */
        int off;
        if (!sp) { dlog("sfx: компонент Spectate не получен"); return 0; }
        void* f = find_field(A.object_get_class(sp), "m_LoadingStatus");
        if (!f) { dlog("sfx: поле m_LoadingStatus не найдено"); return 0; }
        off = A.field_get_offset(f);
        if (off < (int)HDR || off > 0x200 || !hm_readable((char*)sp + off, 4))
            { dlog("sfx: m_LoadingStatus нечитаем"); return 0; }
        __atomic_store_n(&g_sfx_st, (int*)((char*)sp + off), __ATOMIC_RELEASE);
    }
    dlog("sfx: кэш обновлён (fl и st готовы)");
    return 1;
}

/* ─── Фоновый поток мониторинга ─── */
/*
 * Раз в 500 мс: если g_sfx_on && g_sfx_fl && *g_sfx_fl == 1 (spectating)
 * → убеждаемся, что *g_sfx_st == 2.
 * Используем только raw-указатели (кэшированные из главного потока).
 * Гонка с main-thread исключена: оба пишут одно значение (2).
 */
static void* sfx_monitor(void* arg) {
    (void)arg;
    ensure_attached();
    dlog("sfx: поток мониторинга запущен");
    for (;;) {
        usleep(500000);
        if (!__atomic_load_n(&g_sfx_on, __ATOMIC_ACQUIRE)) continue;
        u8*  fl = __atomic_load_n(&g_sfx_fl, __ATOMIC_ACQUIRE);
        int* st = __atomic_load_n(&g_sfx_st, __ATOMIC_ACQUIRE);
        if (!fl || !st) continue;
        if (!hm_readable(fl, 1) || !hm_readable(st, 4)) {
            /* указатели устарели (переподключение?) — сбрасываем кэш */
            __atomic_store_n(&g_sfx_fl, NULL, __ATOMIC_RELEASE);
            __atomic_store_n(&g_sfx_st, NULL, __ATOMIC_RELEASE);
            dlog("sfx: кэш сброшен (нечитаемые указатели); вызовите /specfix on снова");
            continue;
        }
        if (*fl && *st != 2) {
            *st = 2;
            dlog("sfx: auto m_LoadingStatus → 2");
        }
    }
    return NULL;
}

/* ─── Команда /specfix ─── */

static void sfx_say_state(void) {
    u8*  fl = __atomic_load_n(&g_sfx_fl, __ATOMIC_ACQUIRE);
    int* st = __atomic_load_n(&g_sfx_st, __ATOMIC_ACQUIRE);
    int mon = __atomic_load_n(&g_sfx_on, __ATOMIC_ACQUIRE);
    char mem[200]; Buf b; b_init(&b, mem, sizeof mem);
    b_s(&b, "specfix: мониторинг ");
    b_s(&b, mon ? "ВКЛ" : "выкл");
    b_s(&b, "  кэш ");
    b_s(&b, (fl && st) ? "готов" : "НЕ ГОТОВ (вызовите /specfix on)");
    if (fl) { b_s(&b, "  IsSpec="); b_int(&b, (long)*fl); }
    if (st) {
        b_s(&b, "  LoadSt="); b_int(&b, (long)*st);
        b_s(&b, *st == 2 ? " (OK)" : " (не 2!)");
    }
    chat_say(b.p);
}

static int sfx_parse_arg(void* arr) {
    long len; char buf[48];
    if (!arr) return -1;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return -1;
    char* p = (char*)arr + 4 * sizeof(void*);
    for (int i = 0; i < (int)len; i++) {
        void* s = *(void**)(p + (uptr)i * sizeof(void*));
        int n; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 20 || !ch) continue;
        utf16_to_utf8(ch, n, buf, sizeof buf);
        if (seq(buf, "on")  || seq(buf, "вкл")  || seq(buf, "1")) return 1;
        if (seq(buf, "off") || seq(buf, "выкл") || seq(buf, "0")) return 0;
    }
    return -1;   /* status / no arg */
}

static void native_sfx_cmd(void* self, void* args, void* mi) {
    int cmd; (void)self; (void)mi;
    cmd = sfx_parse_arg(args);

    if (cmd == 1) {
        /* /specfix on — кэшировать, включить, сразу зафиксировать */
        if (!sfx_cache()) {
            chat_say("specfix: не удалось закэшировать компоненты (см. konstt.log)");
            return;
        }
        __atomic_store_n(&g_sfx_on, 1, __ATOMIC_RELEASE);
        int* st = __atomic_load_n(&g_sfx_st, __ATOMIC_ACQUIRE);
        u8*  fl = __atomic_load_n(&g_sfx_fl, __ATOMIC_ACQUIRE);
        if (fl && *fl && st && *st != 2) { *st = 2; dlog("sfx: on: m_LoadingStatus → 2 сразу"); }
        chat_say("specfix: ВКЛ — NPC невидим для других, пакет 212 уходит, PayDay работает");
        dlog("sfx: мониторинг включён");

    } else if (cmd == 0) {
        __atomic_store_n(&g_sfx_on, 0, __ATOMIC_RELEASE);
        chat_say("specfix: выкл");
        dlog("sfx: мониторинг выключён");

    } else {
        sfx_say_state();
        chat_usage("/specfix on  — включить «заморозку»\n/specfix off — выключить");
    }
}

static void* sfx_main(void* arg) {
    unsigned long t2 = 0; (void)arg;
    if (!hm_cmd_install("specfix", native_sfx_cmd)) return NULL;
    pthread_create(&t2, NULL, sfx_monitor, NULL);
    hm_mod_ready(g_sfx_modId);
    dlog("sfx: мод загружен (/specfix зарегистрирован)");
    return NULL;
}

#ifndef KONSTT_TEST
__attribute__((constructor)) static void sfx_ctor(void) {
    unsigned long t = 0;
    g_sfx_modId = hm_mod_register("specfix");
    pthread_create(&t, NULL, sfx_main, NULL);
}
#endif
