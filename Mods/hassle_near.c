/*
 * hassle_near.c — мод «игроки рядом» для Hassle Online. Зависит только от ядра (hassle_core.c): /rec, /nocol и /route ему не нужны,
 * а он им — тоже. КАК УБРАТЬ: удалите hassle_near.c и пересоберите .so — остальные моды работают как раньше (HUD без /near просто
 * перестанет получать список игроков).
 *
 * Как это работает
 *   • Фоновый поток присоединяется к IL2CPP (il2cpp_thread_attach), находит Hassle.Global.Pooling.PlayerPool и ~50 раз в секунду
 *     (NEAR_TICK_US) читает PlayersInStream (игроки, которых клиент «видит» рядом): ID, позицию из последнего пакета синхронизации
 *     (это дёшево — просто поля), а «тяжёлое» — ник из подписи над головой (раз в 1,5 с) и HP/броню/пинг/счёт (раз в 300 мс) — берёт из кэша.
 *     v2 JSON: у каждого игрока и у вас — скорость vx,vy,vz (м/с, по смене позиции за последние ≤0,6 с) и age (мс с последней смены позиции),
 *     в корне — ago (мс от тика до ответа, подставляется в момент запроса), у вас — ping. Координаты и дистанции — с двумя знаками.
 *   • Второй поток — крошечный HTTP-сервер на 127.0.0.1:17771 (порты 17771..17780). HUD (Code3.js) опрашивает
 *     http://127.0.0.1:17771/near  и получает JSON. Ничего из IL2CPP на главный поток не навешивается — игру это не трогает.
 *   • Оба потока ждут ядро (hm_wait_started) и, пока игра грузится, отдают в JSON её стадию; опрос игроков начинается, когда ядро
 *     выставило g_ready. Пул игроков (static-поле с PlayerPool) ищет ядро, здесь он только читается.
 *   • Команда /nearlog [N] пишет N снимков в HLOG/near_snap.log (см. ниже), /nearlog diag — стадия и порт HTTP-сервера в чат.
 */
#pragma once
#include "hassle_core.c"


static void b_jstr(Buf* b, const char* s) {            /* JSON-строка (s — UTF-8) */
    b_ch(b, '"');
    for (; s && *s; s++) {
        u8 c = (u8)*s;
        if (c == '"' || c == '\\') { b_ch(b, '\\'); b_ch(b, (char)c); }
        else if (c < 0x20) { b_s(b, "\\u00"); b_ch(b, "0123456789abcdef"[c >> 4]); b_ch(b, "0123456789abcdef"[c & 15]); }
        else b_ch(b, (char)c);
    }
    b_ch(b, '"');
}

/* ───────────── сокеты (нужны только HTTP-серверу) ───────────── */
extern int socket(int, int, int);
extern int setsockopt(int, int, int, const void*, unsigned);
extern int bind(int, const void*, unsigned);
extern int listen(int, int);
extern int accept(int, void*, void*);
extern long recv(int, void*, unsigned long, int);
extern long send(int, const void*, unsigned long, int);
#define NEAR_PORT_FIRST 17771
#define NEAR_PORT_COUNT 10
static int g_port;                  /* порт, на котором слушает HTTP-сервер */
static int g_fieldsDumped;

/* ───────────── перебор коллекции PlayersInStream ───────────── */
static char g_collKind[96];
static int walk_collection(void* coll, item_cb cb, void* ctx) {
    void* k; const char* nm; void* arr;
    if (!coll) return 0;
    k = A.object_get_class(coll);
    nm = A.class_get_name(k);
    if (!seq(g_collKind, nm)) { unsigned i; for (i = 0; i < 95 && nm[i]; i++) g_collKind[i] = nm[i]; g_collKind[i] = 0; dlog2("PlayersInStream: тип ", nm); }
    if (ends_with(nm, "[]")) { walk_ref_array(coll, -1, 1, cb, ctx); return 1; }
    if (sprefix(nm, "List`1")) {
        arr = ref_field(coll, "_items"); if (!arr) arr = ref_field(coll, "items");
        walk_ref_array(arr, int_field(coll, "_size", "size", NULL, -1), 1, cb, ctx);
        return 1;
    }
    if (sprefix(nm, "Dictionary`2")) {
        arr = ref_field(coll, "_entries"); if (!arr) arr = ref_field(coll, "entries");
        if (arr) { walk_struct_array(arr, int_field(coll, "_count", "count", NULL, -1), "value", cb, ctx); return 1; }
        arr = ref_field(coll, "valueSlots");                         /* старый Mono */
        if (arr) { walk_ref_array(arr, int_field(coll, "touchedSlots", NULL, NULL, -1), 1, cb, ctx); return 1; }
    }
    if (sprefix(nm, "HashSet`1")) {
        arr = ref_field(coll, "_entries"); if (!arr) arr = ref_field(coll, "m_slots"); if (!arr) arr = ref_field(coll, "_slots");
        if (arr) { walk_struct_array(arr, int_field(coll, "_count", "m_lastIndex", "_lastIndex", -1), "value", cb, ctx); return 1; }
    }
    if (!g_fieldsDumped) dump_class(k, "PlayersInStream (неизвестный тип коллекции)");
    return 0;
}

/* ───────────── время, скорость и кэш «тяжёлых» полей ───────────── */
extern int clock_gettime(int, void*);          /* struct timespec = два long (так же объявлен в hassle_mem.c / hassle_route.c) */
static long long near_now_ms(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return (long long)ts[0] * 1000 + ts[1] / 1000000; }   /* CLOCK_MONOTONIC */
#define NEAR_TICK_US 20000                     /* период сборщика: ~50 Гц — позицию ловим почти сразу после прихода пакета */
#define NEAR_SLOW_MS 300                       /* HP / броня / пинг / счёт / «в машине» — как раньше, раз в 300 мс */
#define NEAR_NICK_MS 1500                      /* подпись над головой (вызов метода IL2CPP) — раз в 1,5 с и у нового игрока */
#define NEAR_VWIN_MS 600                       /* окно, по которому считается скорость */
#define NEAR_JUMP2   900.0                     /* скачок позиции > 30 м — телепорт, а не скорость: история начинается заново */
static long long g_tickNow;                    /* момент начала текущего тика (мс, монотонное время) */
static volatile unsigned g_ticks;              /* номер тика */
static unsigned g_ticks_u(void) { return g_ticks; }

typedef struct { float x, y, z; long long t; } TPt;
typedef struct { TPt h[6]; int n, head; } Trk;   /* кольцо последних СМЕН позиции: когда позиция новая, ловим момент (с точностью до тика) */
static void trk_push(Trk* k, const float* p, long long now) {
    int idx;
    if (k->n) {
        TPt* l = &k->h[(k->head + k->n - 1) % 6];
        if (l->x == p[0] && l->y == p[1] && l->z == p[2]) return;                   /* новый пакет синхронизации ещё не пришёл */
        { double dx = p[0] - l->x, dy = p[1] - l->y, dz = p[2] - l->z; if (dx * dx + dy * dy + dz * dz > NEAR_JUMP2) k->n = 0; }
    }
    if (k->n < 6) { idx = (k->head + k->n) % 6; k->n++; }
    else { idx = k->head; k->head = (k->head + 1) % 6; }
    k->h[idx].x = p[0]; k->h[idx].y = p[1]; k->h[idx].z = p[2]; k->h[idx].t = now;
}
/* скорость (м/с) и возраст позиции (мс с её последней смены) на момент now */
static void trk_vel(const Trk* k, long long now, float* vx, float* vy, float* vz, int* age) {
    const TPt* l; const TPt* o; int i; long long dt;
    *vx = 0; *vy = 0; *vz = 0; *age = 0;
    if (!k->n) return;
    l = &k->h[(k->head + k->n - 1) % 6];
    dt = now - l->t; *age = dt < 0 ? 0 : (dt > 600000 ? 600000 : (int)dt);
    if (k->n < 2 || dt > 500) return;                                                /* стоит или пакеты давно не приходили */
    o = NULL;
    for (i = k->n - 2; i >= 0; i--) {                                                /* самая старая точка в окне — меньше шум от дискретности тиков */
        const TPt* c = &k->h[(k->head + i) % 6];
        if (l->t - c->t > NEAR_VWIN_MS) break;
        o = c;
    }
    if (!o || l->t - o->t < 40) return;
    {
        double s = 1000.0 / (double)(l->t - o->t);
        double ax = (l->x - o->x) * s, ay = (l->y - o->y) * s, az = (l->z - o->z) * s;
        if (ax * ax + ay * ay + az * az > 120.0 * 120.0) return;                     /* мусор */
        *vx = (float)ax; *vy = (float)ay; *vz = (float)az;
    }
}

/* ───────────── чтение игроков ───────────── */
typedef struct {
    int id; int hasPos; float x, y, z; double hp, ar, ping, score; int veh; int src; char label[96]; char nick[64]; int labelId;
    float vx, vy, vz; int age;
} PInfo;
#define MAXP 256
static PInfo g_pl[MAXP];
static int g_np;
static void* g_local;
static int g_meKind;                           /* какой пакет синхронизации у вас самих: 1 пешком, 2 водитель, 3 пассажир */

typedef struct {
    void* item; int id; unsigned seen;         /* seen — номер тика, когда игрок был в списке последний раз */
    long long tSlow, tNick;
    double hp, ar, ping, score; int veh;
    char label[96]; char nick[64]; int labelId;
    Trk trk;
} PCache;
static PCache g_pc[MAXP];
static void pc_reset(PCache* c) { unsigned char* b = (unsigned char*)c; unsigned i; for (i = 0; i < sizeof *c; i++) b[i] = 0; c->labelId = -1; }
static PCache* pc_get(void* item, int id) {
    int i, freeI = -1, oldI = 0;
    for (i = 0; i < MAXP; i++) {
        if (g_pc[i].item == item) {
            if (g_pc[i].id != id) { pc_reset(&g_pc[i]); g_pc[i].item = item; g_pc[i].id = id; }   /* на том же адресе теперь другой игрок */
            g_pc[i].seen = g_ticks_u();
            return &g_pc[i];
        }
        if (freeI < 0 && (!g_pc[i].item || g_ticks_u() - g_pc[i].seen > 5)) freeI = i;
        if (g_pc[i].seen < g_pc[oldI].seen) oldI = i;
    }
    i = freeI >= 0 ? freeI : oldI;
    pc_reset(&g_pc[i]); g_pc[i].item = item; g_pc[i].id = id; g_pc[i].seen = g_ticks_u();
    return &g_pc[i];
}
static Trk g_meTrk;                            /* ваша скорость — по тем же правилам */
static float g_meV[3]; static int g_meAge, g_mePingOk; static double g_mePing; static long long g_meSlowT;

static void* g_mGetText;
static void read_label(void* player, PInfo* p) {
    void* tags = ref_field(player, "Tags");
    void* lab; void* exc = NULL; void* s;
    p->label[0] = 0; p->nick[0] = 0; p->labelId = -1;
    if (!tags) return;
    lab = ref_field(tags, "m_NickName");
    if (!lab) return;
    if (!g_mGetText) g_mGetText = find_method(A.object_get_class(lab), "get_text", 0);
    if (!g_mGetText) return;
    s = A.runtime_invoke(g_mGetText, lab, NULL, &exc);
    if (exc || !s) return;
    {
        char raw[160]; int n = A.string_length(s), i, o = 0, depth = 0;
        const u16* ch = A.string_chars(s);
        if (n <= 0 || n > 80 || !ch) return;
        utf16_to_utf8(ch, n, raw, sizeof raw);
        for (i = 0; raw[i] && o < (int)sizeof(p->label) - 1; i++) {      /* убираем rich-text теги <...> */
            if (raw[i] == '<') { depth++; continue; }
            if (raw[i] == '>' && depth) { depth--; continue; }
            if (!depth) p->label[o++] = raw[i];
        }
        p->label[o] = 0;
    }
    /* «Ник_Фамилия (123)» → ник и ID */
    {
        int len = (int)slen(p->label), i, nn = 0;
        int paren = -1;
        for (i = len - 1; i >= 0; i--) if (p->label[i] == '(') { paren = i; break; }
        if (paren > 0) {
            int j = paren + 1, v = 0, digits = 0;
            while (p->label[j] >= '0' && p->label[j] <= '9') { v = v * 10 + (p->label[j] - '0'); j++; digits++; }
            if (digits && p->label[j] == ')') p->labelId = v;
            for (i = 0; i < paren && nn < (int)sizeof(p->nick) - 1; i++) p->nick[nn++] = p->label[i];
            while (nn > 0 && p->nick[nn - 1] == ' ') nn--;
            p->nick[nn] = 0;
        } else {
            for (i = 0; i < len && nn < (int)sizeof(p->nick) - 1; i++) p->nick[nn++] = p->label[i];
            p->nick[nn] = 0;
        }
    }
}

static void on_player(void* item, void* ctx) {
    PInfo* p; PCache* c; float pos[3]; int ok, id;
    (void)ctx;
    if (g_np >= MAXP || !item) return;
    if (item == g_local) return;
    id = int_field(item, "m_ID", NULL, NULL, -1);
    c = pc_get(item, id);
    p = &g_pl[g_np];
    p->id = id;
    p->hasPos = read_pos_k(item, pos, &p->src);                          /* каждый тик: это просто чтение полей */
    p->x = p->hasPos ? pos[0] : 0; p->y = p->hasPos ? pos[1] : 0; p->z = p->hasPos ? pos[2] : 0;
    p->vx = 0; p->vy = 0; p->vz = 0; p->age = 0;
    if (p->hasPos) { trk_push(&c->trk, pos, g_tickNow); trk_vel(&c->trk, g_tickNow, &p->vx, &p->vy, &p->vz, &p->age); }
    if (!c->tSlow || g_tickNow - c->tSlow >= NEAR_SLOW_MS) {             /* HP, броня, пинг, счёт, «в машине» — раз в 300 мс */
        c->hp = num_field(item, "m_Health", &ok);
        c->ar = num_field(item, "m_Armour", &ok);
        c->ping = num_field(item, "Ping", &ok);
        c->score = num_field(item, "Score", &ok);
        c->veh = ref_field(item, "Vehicle") ? 1 : 0;
        c->tSlow = g_tickNow;
    }
    p->hp = c->hp; p->ar = c->ar; p->ping = c->ping; p->score = c->score; p->veh = c->veh;
    if (!c->tNick || g_tickNow - c->tNick >= NEAR_NICK_MS) {              /* подпись над головой — раз в 1,5 с (пустая — повтор через 0,2 с) */
        read_label(item, p);
        { int k; for (k = 0; k < (int)sizeof c->label; k++) c->label[k] = p->label[k]; for (k = 0; k < (int)sizeof c->nick; k++) c->nick[k] = p->nick[k]; }
        c->labelId = p->labelId;
        c->tNick = p->label[0] ? g_tickNow : g_tickNow - NEAR_NICK_MS + 200;
        if (!c->tNick) c->tNick = 1;
    } else {
        int k; for (k = 0; k < (int)sizeof p->label; k++) p->label[k] = c->label[k]; for (k = 0; k < (int)sizeof p->nick; k++) p->nick[k] = c->nick[k];
        p->labelId = c->labelId;
    }
    if (p->id < 0 && p->labelId >= 0) p->id = p->labelId;
    g_np++;
}

/* ───────────── итоговый JSON ───────────── */
#define NJ 4                                   /* кольцо буферов JSON: HTTP-поток успевает скопировать свой, пока сборщик пишет в другой */
#define JCAP 65536
static char g_json[NJ][JCAP];
static volatile int g_jsonCur;
static int g_agoOff[NJ];                       /* где в буфере лежит поле «ago» (8 символов под число — подставляется при запросе) */
static long long g_tickAt[NJ];                 /* момент тика, которому принадлежит буфер */
static void publish(int ok, const char* why, int havePos, float mx, float my, float mz, int meId) {
    Buf b; int next = (g_jsonCur + 1) % NJ, i;
    b_init(&b, g_json[next], sizeof g_json[next]);
    b_s(&b, "{\"v\":2,\"ok\":"); b_int(&b, ok);
    b_s(&b, ",\"stage\":"); b_jstr(&b, why);
    b_s(&b, ",\"tick\":"); b_int(&b, (long)g_ticks);
    b_s(&b, ",\"ago\":"); g_agoOff[next] = (int)b.n; b_s(&b, "       0");   /* 8 символов: при запросе сюда пишется «мс от тика до ответа» (пробелы перед числом в JSON допустимы) */
    g_tickAt[next] = g_tickNow;
    b_s(&b, ",\"me\":{\"id\":"); b_int(&b, meId); b_s(&b, ",\"src\":"); b_int(&b, g_meKind);
    if (havePos) {
        b_s(&b, ",\"x\":"); b_fix(&b, mx, 2); b_s(&b, ",\"y\":"); b_fix(&b, my, 2); b_s(&b, ",\"z\":"); b_fix(&b, mz, 2);
        b_s(&b, ",\"vx\":"); b_fix(&b, g_meV[0], 1); b_s(&b, ",\"vy\":"); b_fix(&b, g_meV[1], 1); b_s(&b, ",\"vz\":"); b_fix(&b, g_meV[2], 1);
        b_s(&b, ",\"age\":"); b_int(&b, g_meAge);
        if (g_mePingOk) { b_s(&b, ",\"ping\":"); b_fix(&b, g_mePing, 0); }
    }
    b_s(&b, "},\"n\":[");
    for (i = 0; i < g_np; i++) {
        PInfo* p = &g_pl[i];
        if (i) b_ch(&b, ',');
        b_s(&b, "{\"id\":"); b_int(&b, p->id);
        b_s(&b, ",\"nick\":"); b_jstr(&b, p->nick);
        b_s(&b, ",\"label\":"); b_jstr(&b, p->label);
        if (p->hasPos) {
            b_s(&b, ",\"x\":"); b_fix(&b, p->x, 2); b_s(&b, ",\"y\":"); b_fix(&b, p->y, 2); b_s(&b, ",\"z\":"); b_fix(&b, p->z, 2);
            b_s(&b, ",\"vx\":"); b_fix(&b, p->vx, 1); b_s(&b, ",\"vy\":"); b_fix(&b, p->vy, 1); b_s(&b, ",\"vz\":"); b_fix(&b, p->vz, 1);
            b_s(&b, ",\"age\":"); b_int(&b, p->age);
            if (havePos) {
                double dx = p->x - mx, dy = p->y - my, dz = p->z - mz;
                b_s(&b, ",\"d\":"); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 2);
                b_s(&b, ",\"d2\":"); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy), 2);
            }
        }
        b_s(&b, ",\"hp\":"); b_fix(&b, p->hp, 0);
        b_s(&b, ",\"ar\":"); b_fix(&b, p->ar, 0);
        b_s(&b, ",\"ping\":"); b_fix(&b, p->ping, 0);
        b_s(&b, ",\"score\":"); b_fix(&b, p->score, 0);
        b_s(&b, ",\"veh\":"); b_int(&b, p->veh); b_s(&b, ",\"src\":"); b_int(&b, p->src);
        b_ch(&b, '}');
    }
    b_s(&b, "]}");
    g_jsonCur = next;
}

/* ───────────── снимки в HLOG/near_snap.log (вместо http://127.0.0.1:17771/near в браузере) ─────────────
   Команда /nearlog [N] в чате игры: мод запишет N снимков (по одному в секунду, 1..120; без числа — один) рядом с konstt.log.
   В каждом: ваша позиция и источник (src: 1 пешком, 2 водитель, 3 пассажир), у каждого игрока — ID, ник, d2 (по горизонтали), d (3D),
   src, veh, HP, пинг и координаты. Нужно, чтобы поймать момент, когда расстояния «врут»: набрать /nearlog 20, походить, потом открыть файл. */
static volatile int g_snapLeft;
static long g_snapLast, g_snapBytes;
static char g_snapBuf[24576];
static void snap_write(float* me, int havePos, int meId) {
    char path[320]; Buf b; unsigned i, L; long now = time(NULL);
    if (g_snapLeft <= 0 || now == g_snapLast) return;
    g_snapLast = now; g_snapLeft--;
    L = slen(g_flog);
    if (!g_flog[0] || !ends_with(g_flog, HM_LOGNAME)) { dlog("снимки: путь к файловому журналу неизвестен — не записано"); g_snapLeft = 0; return; }
    b_init(&b, path, sizeof path);
    for (i = 0; i + HM_LOGNAME_LEN < L && b.n + 1 < b.cap; i++) b_ch(&b, g_flog[i]);
    b_s(&b, "near_snap.log");
    if (g_snapBytes > 2000000) { g_snapLeft = 0; dlog("снимки: near_snap.log достиг 2 МБ — остановлено"); return; }
    b_init(&b, g_snapBuf, sizeof g_snapBuf);
    b_s(&b, "== +"); b_int(&b, now - g_flogT0); b_s(&b, "с tick "); b_int(&b, (long)g_ticks);
    b_s(&b, " | я id "); b_int(&b, meId); b_s(&b, " src "); b_int(&b, g_meKind);
    if (havePos) { b_s(&b, " x "); b_fix(&b, me[0], 1); b_s(&b, " y "); b_fix(&b, me[1], 1); b_s(&b, " z "); b_fix(&b, me[2], 1); }
    else b_s(&b, " позиции нет");
    b_s(&b, " | игроков "); b_int(&b, g_np); b_ch(&b, '\n');
    for (i = 0; i < (unsigned)g_np; i++) {
        PInfo* p = &g_pl[i];
        b_s(&b, "  ["); b_int(&b, p->id); b_s(&b, "] "); b_s(&b, p->nick);
        if (p->hasPos && havePos) {
            double dx = p->x - me[0], dy = p->y - me[1], dz = p->z - me[2];
            b_s(&b, "  d2 "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy), 1);
            b_s(&b, " d "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 1);
        } else b_s(&b, "  d ?");
        b_s(&b, "  src "); b_int(&b, p->src); b_s(&b, " veh "); b_int(&b, p->veh);
        b_s(&b, " hp "); b_fix(&b, p->hp, 0); b_s(&b, " ping "); b_fix(&b, p->ping, 0);
        if (p->hasPos) { b_s(&b, "  xyz "); b_fix(&b, p->x, 1); b_ch(&b, ' '); b_fix(&b, p->y, 1); b_ch(&b, ' '); b_fix(&b, p->z, 1); }
        b_ch(&b, '\n');
    }
    if (flog_write(path, b.p, b.n, 0)) g_snapBytes += b.n;
    else { dlog("снимки: не удалось писать в near_snap.log"); g_snapLeft = 0; }
}
static void collect_tick(void) {
    void* pool; void* stream; float me[3] = {0, 0, 0}; int havePos = 0, meId = -1;
    g_ticks++;
    g_tickNow = near_now_ms();
    if (!il2cpp_open()) { set_stage("ждём libil2cpp"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_domain) g_domain = A.domain_get();
    if (!g_domain) { set_stage("ждём домен IL2CPP"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!g_img) g_img = find_image();
    if (!g_img) { set_stage("ждём Assembly-CSharp"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!pool_classes()) { set_stage("класс PlayerPool не найден"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    if (!tl_attached) { ensure_attached(); dlog("поток присоединён к IL2CPP"); }
    if (!g_poolSrcField) scan_for_pool();
    if (!g_poolSrcField) { set_stage("static-поле с PlayerPool не найдено"); publish(0, g_stage, 0, 0, 0, 0, -1); return; }
    pool = current_pool();
    if (!pool) { set_stage("PlayerPool ещё не создан (вы не в игре?)"); g_np = 0; publish(0, g_stage, 0, 0, 0, 0, -1); return; }

    if (!g_fieldsDumped) {
        dump_class(g_cPlayerPool, "PlayerPool");
        g_fieldsDumped = 1;
    }
    g_local = ref_field(pool, "LocalPlayer");
    if (g_local) {
        meId = int_field(g_local, "m_ID", NULL, NULL, -1);
        havePos = read_pos_k(g_local, me, &g_meKind);
        if (havePos) { trk_push(&g_meTrk, me, g_tickNow); trk_vel(&g_meTrk, g_tickNow, &g_meV[0], &g_meV[1], &g_meV[2], &g_meAge); }
        if (!g_meSlowT || g_tickNow - g_meSlowT >= NEAR_SLOW_MS) {       /* ваш пинг (RTT, мс) — нужен Near.js для прогноза на момент прихода команды */
            int pok = 0; double pv = num_field(g_local, "Ping", &pok);
            g_mePingOk = pok && pv > 0; g_mePing = pv; g_meSlowT = g_tickNow;
        }
    }
    stream = ref_field(pool, "PlayersInStream");
    if (!stream) { set_stage("PlayersInStream = null"); g_np = 0; publish(0, g_stage, havePos, me[0], me[1], me[2], meId); return; }
    g_np = 0;
    if (!walk_collection(stream, on_player, NULL)) { set_stage("неизвестный тип PlayersInStream — см. /diag"); publish(0, g_stage, havePos, me[0], me[1], me[2], meId); return; }
    set_stage("работает");
    {   /* один раз покажем поля первого игрока — нужно, чтобы поправить код без пересборки догадок */
        static int once;
        if (!once && g_np > 0) {
            /* повторный обход, только чтобы получить объект */
            once = 1;
        }
    }
    publish(1, g_stage, havePos, me[0], me[1], me[2], meId);
    snap_write(me, havePos, meId);
}

/* /nearlog [N] — записать N снимков в HLOG/near_snap.log. Выполняется в главном потоке; сам снимок пишет поток-сборщик (g_pl не трогаем отсюда) */
static void native_nearlog_cmd(void* self, void* args, void* mi) {
    int diag = 0, t; Buf b; char msg[200];
    (void)self; (void)mi;
    t = parse_cmd_args(args, &diag);
    if (diag) {                                          /* /nearlog diag — состояние Near (раньше это показывала «/rec diag») */
        b_init(&b, msg, sizeof msg);
        b_s(&b, "Near: стадия «"); b_s(&b, g_stage); b_s(&b, "», HTTP-порт "); b_int(&b, g_port);
        chat_say(msg);
        return;
    }
    if (t < 1) t = 1;
    if (t > 120) t = 120;
    g_snapLast = 0; g_snapLeft = t;
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Near: пишу снимки в HLOG/near_snap.log ("); b_int(&b, t); b_s(&b, " шт., по одному в секунду)");
    chat_say(msg);
    dlog_i("нативная /nearlog: снимков ", t);
}

/* /nearlog регистрируется, как только ядро нашло образец делегата (раньше это делал сборщик вместе с /rec) */
static int g_nearlogReg;
static void near_try_register(void) {
    int st = __atomic_load_n(&g_tplState, __ATOMIC_ACQUIRE);
    if (g_nearlogReg || !st) return;                     /* 0 — образец ещё ищется */
    g_nearlogReg = 1;
    if (st == 1) cmd_register("nearlog", native_nearlog_cmd);
    else dlog("нативная /nearlog: образец делегата не найден, команды не будет");
}

/* ───────────── сборщик игроков ───────────── */
static int g_near_modId = -1;                            /* номер в реестре модов (экран загрузки) */
static void* near_collector_main(void* arg) {
    (void)arg;
    if (!hm_wait_started()) return NULL;                 /* konstt_off — мод выключен целиком */
    dlog("near: сборщик игроков запущен");
    while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) {       /* пока ядро ждёт игру — отдаём её стадию в JSON для HUD */
        g_tickNow = near_now_ms();
        publish(0, g_stage, 0, 0, 0, 0, -1);
        usleep(POLL_US);
    }
    dlog("near: игра готова, начинаю опрос игроков");
    hm_mod_ready(g_near_modId);
    for (;;) {
        near_try_register();
        collect_tick();
        usleep(NEAR_TICK_US);
    }
    return NULL;
}


/* ───────────── HTTP-сервер на loopback ───────────── */
struct sa4 { u16 family; u16 port; u32 addr; u8 zero[8]; };
struct tv { long sec; long usec; };

static int send_all(int fd, const char* p, unsigned n) {
    while (n) {
        long w = send(fd, p, n, 0x4000);               /* MSG_NOSIGNAL */
        if (w <= 0) return 0;
        p += w; n -= (unsigned)w;
    }
    return 1;
}
static void respond(int fd, const char* status, const char* ctype, const char* body, unsigned blen) {
    char h[320]; Buf b;
    b_init(&b, h, sizeof h);
    b_s(&b, "HTTP/1.1 "); b_s(&b, status);
    b_s(&b, "\r\nContent-Type: "); b_s(&b, ctype);
    b_s(&b, "\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Headers: *\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ");
    b_int(&b, (long)blen);
    b_s(&b, "\r\n\r\n");
    if (send_all(fd, h, b.n) && blen) send_all(fd, body, blen);
}

static char g_diag_out[24576 + 1024];
static char g_resp[JCAP];

/* /near: копия готового JSON с подставленным «ago» — сколько мс прошло от тика сборщика до этого ответа (Near.js добавляет это к возрасту данных) */
static void handle_near(int c) {
    int cur = g_jsonCur, off = g_agoOff[cur];
    unsigned L = slen(g_json[cur]), k;
    long long ago = near_now_ms() - g_tickAt[cur];
    if (L >= sizeof g_resp) L = sizeof g_resp - 1;
    for (k = 0; k < L; k++) g_resp[k] = g_json[cur][k];
    g_resp[L] = 0;
    if (off > 0 && (unsigned)off + 8 <= L) {
        char t[8]; int i, n = 0; long long v = ago < 0 ? 0 : (ago > 9999999 ? 9999999 : ago);
        do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 8);
        for (i = 0; i < 8; i++) g_resp[off + i] = ' ';
        for (i = 0; i < n; i++) g_resp[off + 7 - i] = t[i];
    }
    respond(c, "200 OK", "application/json", g_resp, L);
}

static void handle_diag(int c) {
    Buf b;
    b_init(&b, g_diag_out, sizeof g_diag_out);
    if (g_diag_ready) { unsigned i; for (i = 0; i < (unsigned)g_diag.n && b.n + 1 < b.cap; i++) b_ch(&b, g_diag_mem[i]); }
    respond(c, "200 OK", "text/plain; charset=utf-8", g_diag_out, b.n);
}

static void* server_main(void* arg) {
    int fd = -1, i, one = 1;
    (void)arg;
    for (;;) {
        for (i = 0; i < NEAR_PORT_COUNT && fd < 0; i++) {
            struct sa4 sa; int s = socket(2, 1, 0);
            if (s < 0) break;
            setsockopt(s, 1, 2, &one, sizeof one);          /* SOL_SOCKET, SO_REUSEADDR */
            sa.family = 2; sa.port = (u16)(((NEAR_PORT_FIRST + i) >> 8) | (((NEAR_PORT_FIRST + i) & 255) << 8));
            sa.addr = 0x0100007F;                           /* 127.0.0.1 */
            { int z; for (z = 0; z < 8; z++) sa.zero[z] = 0; }
            if (bind(s, &sa, sizeof sa) == 0 && listen(s, 8) == 0) { fd = s; g_port = NEAR_PORT_FIRST + i; }
            else close(s);
        }
        if (fd < 0) { usleep(3000000); continue; }
        dlog_i("HTTP: слушаю порт ", g_port);
        for (;;) {
            int c = accept(fd, NULL, NULL);
            char rq[512]; long n; struct tv t;
            if (c < 0) { usleep(100000); continue; }
            t.sec = 2; t.usec = 0;
            setsockopt(c, 1, 20, &t, sizeof t);             /* SO_RCVTIMEO */
            setsockopt(c, 1, 21, &t, sizeof t);             /* SO_SNDTIMEO */
            n = recv(c, rq, sizeof rq - 1, 0);
            if (n > 0) {
                rq[n] = 0;
                { static int hl; if (hl < 15) { char q[48]; int k; hl++; for (k = 0; k < 47 && rq[k] && rq[k] != '\r' && rq[k] != '\n'; k++) q[k] = rq[k]; q[k] = 0; dlog2("HTTP: запрос ", q); } }
                if (sprefix(rq, "OPTIONS ")) respond(c, "204 No Content", "text/plain", "", 0);
                else if (sprefix(rq, "GET /near")) handle_near(c);
                else if (sprefix(rq, "GET /diag")) handle_diag(c);
                else if (sprefix(rq, "GET /konstt")) { static const char kid[] = "KONSTT-CLIENT " HM_BUILD_ID; respond(c, "200 OK", "text/plain", kid, (unsigned)(sizeof kid - 1)); }   /* константа «это клиент konstt» — по ней Load.js решает, запускать ли скрипты */
                else if (sprefix(rq, "GET /ping")) respond(c, "200 OK", "text/plain", "hassle-near", 11);
                else respond(c, "404 Not Found", "text/plain", "no", 2);
            }
            close(c);
        }
    }
    return NULL;
}

static void* near_server_thread(void* arg) {
    if (!hm_wait_started()) return NULL;                 /* konstt_off — мод выключен целиком */
    return server_main(arg);
}

/* ───────────── запуск ───────────── */
#ifndef KONSTT_TEST
__attribute__((constructor)) static void near_mod_ctor(void) {   /* только создаёт потоки-ждуны; IL2CPP они трогают после готовности игры */
    unsigned long t1 = 0, t2 = 0;
    g_near_modId = hm_mod_register("near");
    pthread_create(&t1, NULL, near_server_thread, NULL);
    pthread_create(&t2, NULL, near_collector_main, NULL);
}
#else
void near_test_start(void) {
    unsigned long t1 = 0, t2 = 0;
    start_threads();
    pthread_create(&t1, NULL, near_server_thread, NULL);
    pthread_create(&t2, NULL, near_collector_main, NULL);
}
void near_test_tick(void) { collect_tick(); }
const char* near_test_json(void) { return g_json[g_jsonCur]; }
const char* near_test_diag(void) { return g_diag_mem; }
int near_test_port(void) { return g_port; }
#endif
