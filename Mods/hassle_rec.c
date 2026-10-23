/*
 * hassle_rec.c — команда /rec [секунд 0–600]: реконнект. Зависит только от ядра (hassle_core.c): мод Near, /nocol и /route ему
 * не нужны. КАК УБРАТЬ: удалите hassle_rec.c и пересоберите .so — остальные моды работают как раньше.
 *
 * Как это работает
 *   • Реконнект — только нативная чат-команда /rec [секунд 0–600] (HTTP-эндпоинта /rec больше нет), как в оригинальном libHassleExtended:
 *     Game.Network.OnApplicationQuit() (шлёт серверу ID_DISCONNECTION_NOTIFICATION, если State == Connected), затем
 *     Game.Network.DelayedConnect(m_Host, m_Port, m_Password, m_NickName, N). Game.Reset() сознательно не вызывается.
 *   • /rec работает и без мода Near: команда сама проверяет готовность IL2CPP (набрать её можно только когда игра уже идёт).
 *   • Нативная чат-команда: мод регистрирует «/rec» прямо в реестре команд игры (WebViewSystem.m_ChatCommandEvents), так что
 *     ни rec.js, ни HTTP-порты для /rec не нужны. Делегат клонируется с уже зарегистрированной игрой команды, подменяется method_ptr
 *     (всё это делает ядро: hm_cmd_install). Свой поток-ждун, как у /nocol и /route.
 *   • /rec diag — состояние в чат.
 */
#pragma once
#include "hassle_core.c"


/* ───────────── реконнект (нативная команда /rec) ─────────────
   Повторяет Hassle::Commands::Reconnect оригинального libHassleExtended (/rec): берёт Game.Network (static), читает
   m_Host/m_Port/m_Password/m_NickName, вызывает OnApplicationQuit() (посылает ID_DISCONNECTION_NOTIFICATION, если State == 2)
   и DelayedConnect(host, port, password, nick, секунды) — внутри UniTask.Delay(N) и затем NetworkClient.Connect(...). */
#define REC_MIN_S 0                     /* можно и совсем быстрый /rec (0–2 с), в оригинале было от 5 */
#define REC_MAX_S 600

typedef struct { int ok; int t; int state; int port; int quit; char why[200]; char exc[256]; } RecRes;
static void rec_set(RecRes* r, const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof r->why - 1; i++) r->why[i] = why[i];
    r->why[i] = 0; r->ok = 0;
    dlog2("реконнект: отказ: ", r->why);
}

/* ── Немедленный разрыв соединения через RakNet (как делала сборка от 21.10 10:31) ──
   OnApplicationQuit() сам шлёт серверу уведомление об отключении только при State == Connected. Чтобы сервер и игра сразу
   увидели разрыв («Вы были отключены от сервера»), закрываем все соединения RakPeer штатным CloseConnection с уведомлением:
   Game.Network.m_RakNet._nativeClient = RakPeer*; libRakNet.so: RakPeer::GetConnectionList + RakPeer::CloseConnection. */
typedef struct { unsigned long long g; u16 si; } RakGuid;                       /* RakNetGUID: 16 байт */
typedef struct { u8 addr[16]; u16 dport; u16 sidx; } RakSysAddr;                /* SystemAddress без IPv6: 20 байт */
typedef struct { RakGuid guid; RakSysAddr addr; } RakAOG;                       /* AddressOrGUID: 40 байт, передаётся по значению */
typedef int  (*rak_getlist_t)(void* peer, RakSysAddr* out, u16* num);
typedef void (*rak_close_t)(void* peer, RakAOG target, u8 notify, u8 channel, int prio);

static void rak_close_all(void* nc) {
    void *rk, *peer, *h; rak_getlist_t gl = NULL; rak_close_t cl = NULL; const RakGuid* ug = NULL;
    RakSysAddr list[8]; u16 num = 8; int i, n, closed = 0;
    rk = ref_field(nc, "m_RakNet");
    if (!rk) { dlog("реконнект: Game.Network.m_RakNet не найден или пуст"); return; }
    peer = ref_field(rk, "_nativeClient");
    if (!peer) { dlog("реконнект: m_RakNet._nativeClient пуст"); return; }
    h = dlopen("libRakNet.so", RTLD_NOW_ | 4);                                  /* RTLD_NOLOAD: только если уже загружена */
    if (!h) { dlog("реконнект: libRakNet.so / GetConnectionList недоступны"); return; }
    *(void**)&gl = dlsym(h, "_ZNK6RakNet7RakPeer17GetConnectionListEPNS_13SystemAddressEPt");
    *(void**)&cl = dlsym(h, "_ZN6RakNet7RakPeer15CloseConnectionENS_13AddressOrGUIDEbh14PacketPriority");
    ug = (const RakGuid*)dlsym(h, "_ZN6RakNet22UNASSIGNED_RAKNET_GUIDE");
    if (!gl) { dlog("реконнект: libRakNet.so / GetConnectionList недоступны"); return; }
    if (!cl) { dlog("реконнект: RakPeer::CloseConnection не найден в libRakNet.so"); return; }
    { unsigned k; for (k = 0; k < sizeof list; k++) ((u8*)list)[k] = 0; }
    gl(peer, list, &num);
    n = num > 8 ? 8 : (int)num;
    dlog_i("реконнект: активных соединений RakNet: ", n);
    for (i = 0; i < n; i++) {
        RakAOG a; unsigned k;
        for (k = 0; k < sizeof a; k++) ((u8*)&a)[k] = 0;
        if (ug) a.guid = *ug; else { a.guid.g = ~0ULL; a.guid.si = 0xFFFF; }
        a.addr = list[i];
        cl(peer, a, 1, 0, 0);                                                   /* уведомить сервер, канал 0, IMMEDIATE_PRIORITY */
        closed++;
    }
    dlog_i("реконнект: закрыто соединений: ", closed);
}

/* ── Камера → «в воздух», как при заходе в игру, БЕЗ загрузки чего-либо ──
   При входе камера висит в воздухе (мир не рендерится). Повторяем это на /rec в три лёгких шага (всё в главном потоке, исключения
   гасим, при любой неудаче просто выходим):
     1) MainCamera.SetMode(None = 0) — как Game.OnLoadingFinished / RequestSpawn: ResetTarget(), камера отцеплена от персонажа/машины;
     2) get_Position() — где камера сейчас; SetPosition(она + REC_CAM_UP по Y) — поднимаем простую камеру высоко вверх;
     3) SetLookPosition(точка на той же высоте впереди) — смотрим горизонтально, в небо.
   После реконнекта сервер ставит свою камеру (auth-сцена: InterpolateCameraPos), при спавне SetTarget(Ped) вернёт слежение.
   Game.Reset() не вызываем — он зовётся только из PacketReceive и сбрасывает/грузит лишнее. */
#define REC_CAM_UP 3000.0f
static void rec_camera_stock(void) {
    void *fn, *cam = NULL, *k, *m, *exc = NULL, *ret; int mode = 0; float p[3], l[3]; char et[256];
    if (!g_cGame) return;
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) { dlog("реконнект: Game.MainCamera не найдено — камеру не трогаем"); return; }
    A.field_static_get_value(fn, &cam);
    if (!cam) { dlog("реконнект: Game.MainCamera = null — камеру не трогаем"); return; }
    k = A.object_get_class(cam);

    m = find_method(k, "SetMode", 1);
    if (!m) { dlog("реконнект: MainCamera.SetMode не найден"); return; }
    { void* a[1]; a[0] = &mode; A.runtime_invoke(m, cam, a, &exc); }
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: MainCamera.SetMode(None) бросил: ", et); return; }

    m = find_method(k, "get_Position", 0);
    if (!m) { dlog("реконнект: MainCamera.get_Position не найден — камера только отцеплена"); return; }
    exc = NULL; ret = A.runtime_invoke(m, cam, NULL, &exc);
    if (exc || !ret) { dlog("реконнект: get_Position не удался — камера только отцеплена"); return; }
    { const float* v = (const float*)((char*)ret + 2 * sizeof(void*));          /* упакованный Vector3: данные сразу за заголовком объекта */
      p[0] = v[0]; p[1] = v[1] + REC_CAM_UP; p[2] = v[2];
      if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2])) { dlog("реконнект: позиция камеры не число — камера только отцеплена"); return; } }
    l[0] = p[0]; l[1] = p[1]; l[2] = p[2] + 100.0f;

    m = find_method(k, "SetPosition", 1);
    if (m) { void* a[1]; a[0] = p; exc = NULL; A.runtime_invoke(m, cam, a, &exc);
             if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: MainCamera.SetPosition бросил: ", et); return; } }
    else { dlog("реконнект: MainCamera.SetPosition не найден"); return; }
    m = find_method(k, "SetLookPosition", 1);
    if (m) { void* a[1]; a[0] = l; exc = NULL; A.runtime_invoke(m, cam, a, &exc);
             if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: MainCamera.SetLookPosition бросил: ", et); return; } }
    dlog("реконнект: камера поднята в воздух (SetMode None + SetPosition/SetLookPosition)");
}

/* Общая часть: и для HTTP /rec, и для нативной чат-команды. IL2CPP при необходимости поднимается ЛЕНИВО — набрать /rec можно только
   тогда, когда игра уже идёт, поэтому ждать сборщика (бывшие 60 секунд) не нужно. */
static void do_reconnect(int t, RecRes* r) {
    int state = -1, port = 0, ok = 0;
    void *fn, *nc = NULL, *nk, *mq, *md, *exc = NULL, *host, *pw, *nick;
    unsigned short port16; float tf; char et[256];

    r->ok = 0; r->state = -1; r->port = 0; r->quit = 0; r->why[0] = 0; r->exc[0] = 0;
    if (t < REC_MIN_S) t = REC_MIN_S;                   /* диапазон 0–600 */
    if (t > REC_MAX_S) t = REC_MAX_S;
    r->t = t;
    if (!g_safe) { rec_set(r, "мод ещё стартует (ждёт конца загрузки IL2CPP), подождите"); return; }
    if (!il2cpp_light()) { rec_set(r, "IL2CPP ещё не готов (игра грузится, подождите)"); return; }
    ensure_attached();

    if (!g_cGame) g_cGame = A.class_from_name(g_img, "Hassle", "Game");
    if (!g_cGame) { rec_set(r, "класс Hassle.Game не найден"); return; }
    fn = A.class_get_field_from_name(g_cGame, "Network");
    if (!fn) { rec_set(r, "поле Game.Network не найдено"); return; }
    A.field_static_get_value(fn, &nc);
    if (!nc) { rec_set(r, "Game.Network = null (вы не на сервере?)"); return; }
    nk = A.object_get_class(nc);

    state = (int)num_field(nc, "State", &ok);
    host = ref_field(nc, "m_Host");
    pw = ref_field(nc, "m_Password");
    nick = ref_field(nc, "m_NickName");
    port = (int)num_field(nc, "m_Port", &ok);
    if (!host || !nick) { rec_set(r, "нет данных подключения (m_Host/m_NickName пусты)"); return; }
    mq = find_method(nk, "OnApplicationQuit", 0);
    md = find_method(nk, "DelayedConnect", 5);
    if (!mq || !md) { rec_set(r, !md ? "метод DelayedConnect не найден" : "метод OnApplicationQuit не найден"); return; }

    dlog_i("реконнект: старт, секунд ", t);
    dlog_i("реконнект: State ", state);
    dlog_i("реконнект: порт ", port);

    /* 1) корректный выход с сервера (внутри сам проверяет State == Connected).
       Как в оригинальном libHassleExtended (HassleRec): OnApplicationQuit → DelayedConnect.
       Game.Reset() НЕ вызываем (нагружает, на прошлом клиенте игра вылетала спустя время); кулдауна между /rec нет, как в оригинале;
       прямой разрыв через RakNet (rak_close_all) из /rec убран; камера (rec_camera_stock) возвращена. */
    exc = NULL;
    A.runtime_invoke(mq, nc, NULL, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: OnApplicationQuit бросил: ", et); }
    else r->quit = 1;

    /* 1б) камера — в стоковое (лёгкие сеттеры, ничего не грузим) */
    rec_camera_stock();

    /* 2) подключение через N секунд (внутри UniTask.Delay(N), затем NetworkClient.Connect) */
    port16 = (unsigned short)port; tf = (float)t;
    {
        void* args[5];
        args[0] = host; args[1] = &port16; args[2] = pw; args[3] = nick; args[4] = &tf;
        exc = NULL;
        A.runtime_invoke(md, nc, args, &exc);
        if (exc && t == 0) {   /* на случай, если нулевая задержка не принимается: повторяем с минимальной (0.1 с) */
            exc_text(exc, et, sizeof et);
            dlog2("реконнект: DelayedConnect(0) бросил, повтор с 0.1 с: ", et);
            tf = 0.1f; exc = NULL;
            A.runtime_invoke(md, nc, args, &exc);
        }
    }
    if (exc) {
        exc_text(exc, et, sizeof et);
        dlog2("реконнект: DelayedConnect бросил: ", et);
        rec_set(r, "DelayedConnect: исключение");
        { unsigned i = 0; for (; et[i] && i < sizeof r->exc - 1; i++) r->exc[i] = et[i]; r->exc[i] = 0; }
        return;
    }
    r->ok = 1; r->state = state; r->port = port;
    dlog("реконнект: DelayedConnect запущен");
}

static int g_recReg;                             /* 1 — /rec зарегистрирована, -1 — нет */
/* Вызывается игрой в главном потоке: (this-цель делегата, string[] args, MethodInfo*) */
static void native_rec_cmd(void* self, void* args, void* mi) {
    RecRes r; int diag = 0, t; Buf b; char msg[300];
    (void)self; (void)mi;
    t = parse_cmd_args(args, &diag);
    dlog_i("нативная /rec: вызвана, секунд ", t);
    b_init(&b, msg, sizeof msg);
    if (diag) {
        b_s(&b, "Rec: стадия «"); b_s(&b, g_stage); b_s(&b, "», игра готова: "); b_s(&b, g_ready ? "да" : "нет");
        b_s(&b, ", /rec нативно: "); b_s(&b, g_recReg == 1 ? "да" : "нет");
        chat_say(msg);
        return;
    }
    if (t < 0) t = 5;                                   /* без аргумента — 5 секунд */
    if (t < REC_MIN_S || t > REC_MAX_S) {               /* допустимо 0–600 */
        chat_say("Используйте: /rec [секунды (0-600]");
        return;
    }
    do_reconnect(t, &r);
    if (r.ok) return;                                   /* успех — в чат ничего не пишем */
    b_s(&b, "Реконнект: "); b_s(&b, r.why);             /* ошибку оставляем, чтобы было видно причину */
    chat_say(msg);
}


/* ───────────── самозапуск ───────────── */
static void* rec_main(void* arg) {
    (void)arg;
    g_recReg = hm_cmd_install("rec", native_rec_cmd) ? 1 : -1;     /* ждёт ядро, готовность игры и реестр команд; регистрирует /rec */
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void rec_ctor(void) {          /* только создаёт поток-ждуна; до готовности игры он ничего в IL2CPP не трогает */
    unsigned long t = 0;
    pthread_create(&t, NULL, rec_main, NULL);
}
#endif
