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
 *   • Встроенный автореконнект игры (PacketReceive → DelayedConnect(1 с) после «Потеряно соединение с сервером») на время ожидания
 *     подавляется заменой одной инструкции в PacketReceive (arm64); после подключения + 15 с возвращается. См. rec_ac_* ниже.
 *   • Таймер под чатом: после успешного запуска мод один раз вызывает WebViewSystem.ExecuteJavaScript(string) (static, 1 аргумент —
 *     из метаданных клиента) и внедряет в WebView маленький оверлей. Дальше считает JS сам: «Реконнект через N с» → «Подключение… N с»;
 *     прячется, когда открылось окно Authorization, при новом /rec или через (N + 120) с. Ничего в фоне и ничего в главный поток не постится.
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

/* ── Таймер реконнекта под чатом ──
   Оверлей — один div в WebView, ставится под блок чата (.radmir-chat, у старого чата .chat), при отсутствии чата — слева на 40vh.
   Скрипт идемпотентный: новый /rec сносит старый оверлей и таймер. Внутри только одинарные кавычки, чтобы не экранировать их в C. */
static void rec_js(const char* code) {
    void *cw, *m, *exc = NULL, *args[1]; char et[200];
    if (!A.string_new || !g_img) return;
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    if (!cw) { dlog("реконнект: таймер: класс WebViewSystem не найден"); return; }
    m = A.class_get_method_from_name(cw, "ExecuteJavaScript", 1);
    if (!m) { dlog("реконнект: таймер: метод ExecuteJavaScript(1) не найден"); return; }
    args[0] = A.string_new(code);
    if (!args[0]) return;
    A.runtime_invoke(m, NULL, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реконнект: таймер: ExecuteJavaScript бросил: ", et); }
}
static void rec_timer_show(int t) {
    char js[2600]; Buf b;
    b_init(&b, js, sizeof js);
    b_s(&b,
        "(function(t){try{var w=window,id='hm-rec-timer',old=document.getElementById(id);"
        "if(w.__hmRecT){clearInterval(w.__hmRecT.iv);w.__hmRecT=null;}"
        "if(old&&old.parentNode)old.parentNode.removeChild(old);"
        "var e=document.createElement('div');e.id=id;"
        "e.style.cssText='position:fixed;z-index:2147483000;pointer-events:none;padding:.5vh 1.1vh;border-radius:.7vh;"
        "background:rgba(12,10,6,.62);color:#f4f1e1;font:600 1.7vh Open Sans,sans-serif;"
        "text-shadow:0 .1vh .2vh rgba(0,0,0,.8);white-space:nowrap;left:1.66vw;top:40vh';"
        "(document.body||document.documentElement).appendChild(e);"
        "var t0=Date.now(),end=t0+t*1000,st={iv:0};w.__hmRecT=st;"
        "function stop(){clearInterval(st.iv);if(e.parentNode)e.parentNode.removeChild(e);if(w.__hmRecT===st)w.__hmRecT=null;}"
        "function auth(){try{return !!(w.getInterfaceStatus&&w.getInterfaceStatus('Authorization'));}catch(x){return false;}}"
        "function place(){var c=document.querySelector('.radmir-chat')||document.querySelector('.chat-container .chat');"
        "if(c){var r=c.getBoundingClientRect();if(r.width>0&&r.height>0){e.style.left=r.left+'px';e.style.top=(r.bottom+6)+'px';return;}}"
        "e.style.left='1.66vw';e.style.top='40vh';}"
        "function tick(){var now=Date.now(),left=Math.ceil((end-now)/1000);"
        "if(now-t0>(t+120)*1000||(left<=0&&auth())){stop();return;}"
        "if(left>0){e.textContent='Реконнект через '+left+' с';e.style.color='#f4f1e1';}"
        "else{e.textContent='Подключение... '+Math.floor((now-end)/1000)+' с';e.style.color='#ffd166';}"
        "place();}"
        "st.iv=setInterval(tick,250);tick();}catch(x){}})(");
    b_int(&b, t);
    b_s(&b, ");");
    rec_js(js);
    dlog_i("реконнект: таймер под чатом запущен, секунд ", t);
}

/* ── Подавление встроенного автореконнекта игры на время ожидания /rec ──
   ПРИЧИНА «подключается раньше таймера» (разбор libil2cpp, arm64). OnApplicationQuit() только шлёт серверу ID_DISCONNECTION_NOTIFICATION;
   сервер рвёт связь, и через несколько секунд в NetworkClient.PacketReceive приходит ID_CONNECTION_LOST (22). Игра в этой ветке:
   пишет в чат «Потеряно соединение с сервером», вызывает Game.Reset() и СРАЗУ (без задержки) NetworkClient.Connect(host, port,
   password, nick) — отсюда «Подключение к …» через ~1 с. Тот же хвост с Connect у ID_CONNECTION_ATTEMPT_FAILED (17). Наш
   DelayedConnect(N) при этом остаётся в очереди и через N с подключает ВТОРОЙ раз. (Ветка NO_FREE_INCOMING (20) — отдельная,
   с DelayedConnect(1 с); её не трогаем.)
   Лечение: пока /rec ждёт, в хвосте PacketReceive единственную инструкцию `B Connect` (перед ней стек уже восстановлен:
   `ldr x22,[sp],#0x30`, PacketReceive возвращает void) заменяем на `RET`, потом возвращаем как было. Сообщение «Потеряно
   соединение…» и Game.Reset() остаются как есть — не остаётся только немедленного подключения. Адрес берём из MethodInfo
   (методы Connect и PacketReceive), не из RVA; если поиск не сошёлся (другая сборка игры, не arm64) — ничего не трогаем. */
#if defined(__aarch64__)
#define REC_AC_ARM64 1
extern int mprotect(void*, unsigned long, int);
extern long pwrite(int, const void*, unsigned long, long);
#endif
#define REC_HOLD_AFTER_S 15             /* сколько держать подавление после подключения: запоздалый «lost» от старой связи */
#define REC_HOLD_MAX_S   30             /* предел удержания сверх N секунд, если подключения так и нет (дальше игра снова реконнектится сама) */
#define REC_RET_INSN     0xD65F03C0u    /* ret */

#ifdef REC_AC_ARM64
static uptr g_rec_acSite;               /* адрес инструкции «B Connect» в хвосте PacketReceive (0 — не искали/не нашли) */
static u32  g_rec_acOrig;               /* её исходное слово */
#endif
static int  g_rec_acTried;              /* 1 — нашли, -1 — искали и не нашли */
static int  g_rec_acOn;                 /* 1 — сейчас на месте стоит RET */
static int  g_rec_gen;                  /* номер последнего /rec (поток-наблюдатель снимает подавление только за свой) */
static int  g_rec_acLock;
static int  g_rec_wT;                   /* секунды последнего /rec — для потока-наблюдателя */

#ifdef REC_AC_ARM64
static void rec_ac_flush(void* p, unsigned n) {          /* синхронизация кэшей после записи кода */
    uptr ctr, dl, il, a, e;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    dl = 4u << ((ctr >> 16) & 15u); il = 4u << (ctr & 15u);
    e = (uptr)p + n;
    for (a = (uptr)p & ~(dl - 1); a < e; a += dl) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uptr)p & ~(il - 1); a < e; a += il) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
static int rec_ac_write(uptr addr, u32 w) {              /* одно слово кода; права страницы возвращаются в r-x */
    uptr p0 = addr & ~(uptr)4095;
    if (mprotect((void*)p0, 4096, 7) == 0) {
        *(volatile u32*)addr = w;
        rec_ac_flush((void*)addr, 4);
        mprotect((void*)p0, 4096, 5);
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, &w, 4, (long)addr); close(fd); }
        if (r == 4) { rec_ac_flush((void*)addr, 4); return 1; }
    }
    return 0;
}
/* Ищем в теле PacketReceive единственный хвостовой «B <Connect>», перед которым стек уже восстановлен (ldr/ldp с пост-инкрементом от sp). */
static int rec_ac_find(void* nk) {
    void *pr, *cm; uptr f, cn, a, site = 0; int found = 0; u32 prev;
    if (g_rec_acTried) return g_rec_acTried > 0;
    pr = find_method(nk, "PacketReceive", 2);
    cm = find_method(nk, "Connect", 4);
    if (!pr || !cm || !hm_readable(pr, sizeof(void*)) || !hm_readable(cm, sizeof(void*))) { dlog("реконнект: автореконнект: PacketReceive/Connect не найдены"); return 0; }
    f = *(uptr*)pr; cn = *(uptr*)cm;
    if (!f || !cn || (f & 3) || (cn & 3) || !hm_range_ok((void*)f, 0x400)) { dlog("реконнект: автореконнект: адреса методов нечитаемы"); return 0; }
    g_rec_acTried = -1;
    for (a = f; a < f + 0x400; a += 4) {
        u32 w = *(const u32*)a;
        if ((w >> 26) == 5u) {                                           /* B imm26 */
            long imm = (long)(w & 0x3FFFFFFu); uptr tgt;
            if (imm & 0x2000000) imm -= 0x4000000;
            tgt = a + (uptr)(imm * 4);
            if (tgt == cn) { if (!found) site = a; found++; }
        }
    }
    if (found != 1) { dlog_i("реконнект: автореконнект: переходов на Connect в PacketReceive: ", found); return 0; }
    prev = *(const u32*)(site - 4);
    /* ldr xN,[sp],#imm (пост-инкремент) или ldp xN,xM,[sp],#imm — стек уже снят, значит это настоящий хвостовой вызов */
    if (!(((prev & 0xFFE00C00u) == 0xF8400400u || (prev & 0xFFC00000u) == 0xA8C00000u) && ((prev >> 5) & 31u) == 31u)) {
        dlog("реконнект: автореконнект: перед переходом нет снятия стека — другая сборка, не трогаем"); return 0; }
    g_rec_acSite = site; g_rec_acOrig = *(const u32*)site; g_rec_acTried = 1;
    dlog_i("реконнект: автореконнект (B Connect) найден, смещение в PacketReceive ", (long)(site - f));
    return 1;
}
#endif

static void rec_ac_lock(void)   { while (__sync_lock_test_and_set(&g_rec_acLock, 1)) usleep(200); }
static void rec_ac_unlock(void) { __sync_lock_release(&g_rec_acLock); }

/* Включить подавление (и объявить новый /rec). 1 — подавление стоит; 0 — не получилось (тогда как раньше). */
static int rec_ac_hold(void* nk, int t) {
    int r = 0;
    rec_ac_lock();
    g_rec_gen++; g_rec_wT = t;
#ifdef REC_AC_ARM64
    if (rec_ac_find(nk)) {
        if (g_rec_acOn) r = 1;
        else if (rec_ac_write(g_rec_acSite, REC_RET_INSN)) { g_rec_acOn = 1; r = 1; dlog("реконнект: мгновенный автореконнект игры (B Connect) подавлен на время ожидания"); }
        else dlog("реконнект: автореконнект: не удалось записать в код игры");
    }
#else
    (void)nk; dlog("реконнект: подавление автореконнекта есть только в arm64-сборке");
#endif
    rec_ac_unlock();
    return r;
}
/* Снять подавление. gen = 0 — безусловно (ошибка запуска), иначе только если за это время не начался новый /rec. */
static void rec_ac_release(int gen) {
    rec_ac_lock();
#ifdef REC_AC_ARM64
    if (g_rec_acOn && (gen == 0 || gen == g_rec_gen)) {
        if (rec_ac_write(g_rec_acSite, g_rec_acOrig)) { g_rec_acOn = 0; dlog("реконнект: встроенный автореконнект игры возвращён"); }
        else dlog("реконнект: автореконнект: не удалось вернуть код — останется подавленным до перезапуска игры");
    }
#else
    (void)gen;
#endif
    rec_ac_unlock();
}
/* NetworkState — enum (тип поля VALUETYPE), num_field() для него всегда даёт 0, поэтому читаем int по смещению поля. подтверждено только 2 = Connected (OnApplicationQuit делает cmp State,#2); -1 — не прочитать. */
static int rec_state_of(void* nc) {
    void* f; int off;
    if (!nc || !hm_obj_ok(nc)) return -1;
    f = find_field(A.object_get_class(nc), "State");
    if (!f) return -1;
    off = A.field_get_offset(f);
    if (off < 0 || off > 0x200) return -1;
    return *(volatile int*)((char*)nc + off);
}
static int rec_net_state(void) {
    void *fn, *nc = NULL;
    if (!g_cGame) return -1;
    fn = A.class_get_field_from_name(g_cGame, "Network");
    if (!fn) return -1;
    A.field_static_get_value(fn, &nc);
    return rec_state_of(nc);
}
/* Поток-наблюдатель: ждёт N с, потом подключения и ещё REC_HOLD_AFTER_S с, затем возвращает код игры.
   Каждое изменение State пишет в журнал со временем от старта /rec — по нему видно, когда игра «потеряла» связь. */
static void* rec_ac_watch(void* arg) {
    int gen = (int)(long)arg, t = g_rec_wT, i, conn = -1, st, last = -99, steps = (t + REC_HOLD_MAX_S) * 4;
    char m[96]; Buf b;
    ensure_attached();
    for (i = 0; i < steps; i++) {
        usleep(250000);
        if (g_rec_gen != gen) return NULL;               /* пошёл новый /rec — он сам снимет подавление */
        if (i & 1) continue;                             /* State читаем раз в полсекунды */
        st = rec_net_state();
        if (st != last) {
            b_init(&b, m, sizeof m); b_s(&b, "реконнект: State "); b_int(&b, st); b_s(&b, " через "); b_int(&b, i / 4); b_s(&b, " с");
            dlog(m); last = st;
        }
        if (i >= t * 4 && st == 2) {
            if (conn < 0) conn = i;
            if (i - conn >= REC_HOLD_AFTER_S * 4) break;
        }
    }
    rec_ac_release(gen);
    return NULL;
}

/* Общая часть: и для HTTP /rec, и для нативной чат-команды. IL2CPP при необходимости поднимается ЛЕНИВО — набрать /rec можно только
   тогда, когда игра уже идёт, поэтому ждать сборщика (бывшие 60 секунд) не нужно. */
static void do_reconnect(int t, RecRes* r) {
    int state = -1, port = 0, ok = 0, held = 0;
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

    state = rec_state_of(nc);
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

    /* 0) глушим встроенный автореконнект игры (PacketReceive → DelayedConnect(1 с)), иначе она подключится сама через ~1 с после
       «потеряно соединение», не дожидаясь N секунд. Если не вышло — работаем как раньше. */
    held = rec_ac_hold(nk, t);

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
        if (held) rec_ac_release(0);
        { unsigned i = 0; for (; et[i] && i < sizeof r->exc - 1; i++) r->exc[i] = et[i]; r->exc[i] = 0; }
        return;
    }
    r->ok = 1; r->state = state; r->port = port;
    dlog("реконнект: DelayedConnect запущен");
    if (held) {
        unsigned long th = 0;
        if (pthread_create(&th, NULL, rec_ac_watch, (void*)(long)g_rec_gen) != 0) rec_ac_release(0);   /* без наблюдателя подавление не оставляем */
    }
    rec_timer_show(t);                                  /* таймер под чатом (только при успехе — при ошибке в чате и так причина) */
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
        b_s(&b, ", автореконнект игры: "); b_s(&b, g_rec_acTried > 0 ? (g_rec_acOn ? "подавлен" : "найден") : (g_rec_acTried < 0 ? "не найден" : "ещё не искали"));
        chat_say(msg);
        return;
    }
    if (t < 0) t = 5;                                   /* без аргумента — 5 секунд */
    if (t < REC_MIN_S || t > REC_MAX_S) {               /* допустимо 0–600 */
        chat_usage("Используйте: /rec [секунды (0-600]");
        return;
    }
    do_reconnect(t, &r);
    if (r.ok) return;                                   /* успех — в чат ничего не пишем */
    b_s(&b, "Реконнект: "); b_s(&b, r.why);             /* ошибку оставляем, чтобы было видно причину */
    chat_say(msg);
}


/* ───────────── самозапуск ───────────── */
static int g_rec_modId = -1;                             /* номер в реестре модов (экран загрузки) */
static void* rec_main(void* arg) {
    (void)arg;
    g_recReg = hm_cmd_install("rec", native_rec_cmd) ? 1 : -1;     /* ждёт ядро, готовность игры и реестр команд; регистрирует /rec */
    if (g_recReg == 1) hm_mod_ready(g_rec_modId);
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void rec_ctor(void) {          /* только создаёт поток-ждуна; до готовности игры он ничего в IL2CPP не трогает */
    unsigned long t = 0;
    g_rec_modId = hm_mod_register("rec");
    pthread_create(&t, NULL, rec_main, NULL);
}
#endif
