/*
 * hassle_near.c — мод «игроки рядом» для Hassle Online. Зависит только от ядра (hassle_core.c): /rec, /nocol и /route ему не нужны,
 * а он им — тоже. КАК УБРАТЬ: удалите hassle_near.c и пересоберите .so — остальные моды работают как раньше (HUD без /near просто
 * перестанет получать список игроков).
 *
 * Как это работает
 *   • Фоновый поток присоединяется к IL2CPP (il2cpp_thread_attach), находит Hassle.Global.Pooling.PlayerPool и раз в 300 мс
 *     читает PlayersInStream (игроки, которых клиент «видит» рядом): ID, ник из подписи над головой, позицию из
 *     последнего пакета синхронизации, HP/броню, пинг и счёт.
 *   • Второй поток — крошечный HTTP-сервер на 127.0.0.1:17771 (порты 17771..17780). HUD (Code3.js) опрашивает
 *     http://127.0.0.1:17771/near  и получает JSON. Ничего из IL2CPP на главный поток не навешивается — игру это не трогает.
 *   • Оба потока ждут ядро (hm_wait_started) и, пока игра грузится, отдают в JSON её стадию; опрос игроков начинается, когда ядро
 *     выставило g_ready. Пул игроков (static-поле с PlayerPool) ищет ядро, здесь он только читается.
 *   • /nwatch — постоянное слежение за игроками по ID/нику (зона стрима + весь пул + PlayerPool.Get), события в чат, в HLOG/near_watch.log и в
 *     HTTP GET /watch (см. блок «/nwatch» ниже). Работает само в потоке-сборщике, HUD для этого не нужен.
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
extern int clock_gettime(int, void*);
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
static char g_near_id[40];          /* метка окна игры (команда /nearid kt…): по ней скрипт находит СВОЙ порт среди 17771..17780 — GET /id */
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

/* ───────────── чтение игроков ───────────── */
typedef struct {
    int id; int hasPos; float x, y, z; double hp, ar, ping, score; int veh; int src; char label[96]; char nick[64]; int labelId;
    int hasVel; float vx, vy, vz;                  /* скорость м/с — по смещению позиции между опросами */
} PInfo;
#define MAXP 256
static PInfo g_pl[MAXP];
static int g_np;
static void* g_local;
static int g_meKind;                           /* какой пакет синхронизации у вас самих: 1 пешком, 2 водитель, 3 пассажир */

/* ───────────── время и скорость ─────────────
   Скорость считаем сами: разность позиций между двумя опросами / время между ними (единицы MoveSpeed из пакета заранее неизвестны, а так — всегда м/с).
   Near.js по ней двигает позицию цели на возраст данных + RTT (прогноз на момент прихода команды/сообщения на сервер).
   Позиция не менялась дольше VEL_HOLD_MS — игрок стоит, скорость 0. Запись истории живёт по ID; ключ -2 — это вы сами. */
static long mono_ms(void) {
    struct { long s; long ns; } ts;
    if (clock_gettime(1, &ts) != 0) return 0;
    return ts.s * 1000L + ts.ns / 1000000L;
}
#define VEL_HOLD_MS 700
#define VEL_MAX 60.0f                                  /* больше — это телепорт/скачок, а не скорость */
typedef struct { int id; float x, y, z, vx, vy, vz; long tSeen, tMove; } VelRec;
static VelRec g_vel[MAXP + 1];
static int g_nvel;
static void vel_track(int id, const float* pos, long now, float* v) {
    int i, k = -1; VelRec* r;
    v[0] = v[1] = v[2] = 0;
    for (i = 0; i < g_nvel; i++) if (g_vel[i].id == id) { k = i; break; }
    if (k < 0) {                                        /* новая запись (при переполнении — вытесняем самую давнюю) */
        if (g_nvel < MAXP + 1) k = g_nvel++;
        else { long old = now; k = 0; for (i = 0; i < g_nvel; i++) if (g_vel[i].tSeen <= old) { old = g_vel[i].tSeen; k = i; } }
        r = &g_vel[k]; r->id = id; r->x = pos[0]; r->y = pos[1]; r->z = pos[2]; r->vx = r->vy = r->vz = 0; r->tSeen = r->tMove = now;
        return;
    }
    r = &g_vel[k];
    if (now - r->tSeen > 3000) {                        /* игрока долго не видели — старая позиция не годится для разности */
        r->x = pos[0]; r->y = pos[1]; r->z = pos[2]; r->vx = r->vy = r->vz = 0; r->tSeen = r->tMove = now;
        return;
    }
    if (pos[0] != r->x || pos[1] != r->y || pos[2] != r->z) {
        float dt = (float)(now - r->tMove) / 1000.0f, dx = pos[0] - r->x, dy = pos[1] - r->y, dz = pos[2] - r->z, sp;
        if (dt < 0.05f) dt = 0.05f;
        r->vx = dx / dt; r->vy = dy / dt; r->vz = dz / dt;
        sp = __builtin_sqrtf(r->vx * r->vx + r->vy * r->vy + r->vz * r->vz);
        if (sp > VEL_MAX) r->vx = r->vy = r->vz = 0;    /* скачок/телепорт */
        r->x = pos[0]; r->y = pos[1]; r->z = pos[2]; r->tMove = now;
    } else if (now - r->tMove > VEL_HOLD_MS) {
        r->vx = r->vy = r->vz = 0;
    }
    r->tSeen = now;
    v[0] = r->vx; v[1] = r->vy; v[2] = r->vz;
}
static long g_tickMs;                                   /* момент чтения текущего снимка */
static double g_mePing;

static int bool_field(void* obj, const char* name, int* has) {   /* bool-поле объекта (read_num bool не знает); has=0 — поля нет */
    void* f; if (has) *has = 0;
    if (!obj) return 0;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return 0;
    if (has) *has = 1;
    return *((u8*)obj + A.field_get_offset(f)) ? 1 : 0;
}

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
    PInfo* p; float pos[3]; int ok;
    (void)ctx;
    if (g_np >= MAXP || !item) return;
    p = &g_pl[g_np];
    p->id = int_field(item, "m_ID", NULL, NULL, -1);
    if (item == g_local) return;
    p->hasPos = read_pos_k(item, pos, &p->src);
    if (p->hasPos) {                                    /* пакета позиции ещё не было — в структуре нули: это не координаты, а «неизвестно» */
        int has = 0, got = bool_field(item, "m_IsPositionReceived", &has);
        if (has && !got && pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f) p->hasPos = 0;
    }
    p->x = p->hasPos ? pos[0] : 0; p->y = p->hasPos ? pos[1] : 0; p->z = p->hasPos ? pos[2] : 0;
    p->hasVel = 0; p->vx = p->vy = p->vz = 0;
    if (p->hasPos && p->id >= 0) { float v[3]; vel_track(p->id, pos, g_tickMs, v); p->vx = v[0]; p->vy = v[1]; p->vz = v[2]; p->hasVel = 1; }
    p->hp = num_field(item, "m_Health", &ok);
    p->ar = num_field(item, "m_Armour", &ok);
    p->ping = num_field(item, "Ping", &ok);
    p->score = num_field(item, "Score", &ok);
    p->veh = ref_field(item, "Vehicle") ? 1 : 0;
    read_label(item, p);
    if (p->id < 0 && p->labelId >= 0) p->id = p->labelId;
    g_np++;
}

/* ───────────── /nwatch — постоянное слежение за игроками («всегда следит, пишет, когда появился») ─────────────
   Цели: до NW_MAX игроков по ID или по нику (часть ника тоже годится, «_» и пробел одно и то же, регистр не важен), плюс режим «any» —
   сообщать о ЛЮБОМ новом игроке в зоне. Слежение идёт в потоке-сборщике каждые 300 мс само по себе: HUD/Near.js для этого не нужны.
   Список целей хранится в HLOG/near_watch.txt и поднимается при следующем запуске игры — после реконнекта или рестарта сервера слежение продолжается.

   Как ищем игрока (все способы по очереди, первый нашедший побеждает; в событиях и в /watch это поле how):
     1 — PlayersInStream по ID           (игрок в зоне стрима, позиция из пакета синхронизации, свежая)
     2 — PlayersInStream по нику         (подпись над головой «Ник_Фамилия (ID)»)
     3 — PlayerPool.m_Items по ID        (все игроки, которых клиент знает: онлайн, но вне зоны стрима; позиция — последняя известная)
     4 — PlayerPool.m_Items по нику
     5 — PlayerPool.Get(id) напрямую     (запасной путь, если словарь не нашёл)
   Слежка по ID запоминает ник и дальше следует за НИКОМ: если игрок перезашёл и получил другой ID, цель перепривязывается сама.
   Состояния: 1 в зоне, 2 онлайн но вне зоны, 3 нет в пуле (вышел/реконнект), 0 пока не определено.
   Куда идут события: чат игры, HLOG/near_watch.log, HTTP GET /watch[?since=N] (для скриптов-NPC и Telegram; seq растёт, ничего не теряется). */
#define NW_MAX 8
#define NW_EVN 48
#define NW_MISS 8                      /* тиков подряд «не вижу в зоне» (~2,4 с), прежде чем считать, что вышел из зоны */
#define NW_MISS_POOL 4                 /* то же для «пропал из пула» */
#define NW_POOLMAX 512
#define NW_Q 24
#define NW_LOGMAX 1500000
static volatile unsigned g_ticks;                      /* номер тика сборщика (его читают /nwatch и JSON) */
typedef struct {
    int used, id, st, how, cid, hasPos, veh, miss, idTaken;
    char q[40], qn[40], nick[64];
    float x, y, z, d, d2; double hp;
    long tSince, tSeen;
} NwT;
typedef struct { long seq, ms; int tgt, id; char kind[14], nick[64]; float d; char msg[220]; } NwEv;
typedef struct { int ok, score, id, hasPos, veh, how; char nick[64]; float x, y, z; double hp; } NwC;
typedef struct { int id, hasPos, veh, inStream; char nick[64]; float x, y, z; double hp; } NwP;
static NwT g_nw[NW_MAX];
static NwEv g_nwEv[NW_EVN];
static long g_nwSeq;
static NwP g_nwP[NW_POOLMAX];
static int g_nwNP, g_nwPoolTotal = -2, g_nwPoolNicks;
static volatile int g_nwLock, g_nwAny, g_nwChat = 1, g_nwLoaded;
static char g_nwq[NW_Q][220];
static volatile unsigned g_nwqH, g_nwqT;
static volatile int g_nwPostPend; static long g_nwPostT;
static long g_nwLogBytes;
static char g_nwTj[2][9216]; static volatile int g_nwTjCur;
static int g_nwGetFail, g_nwCtxTries, g_nwNoCtxLogged;
#ifdef KONSTT_TEST
static int g_nwTestTotal = -1;
#endif
static int g_nwAnyId[MAXP], g_nwAnyMiss[MAXP]; static char g_nwAnyNick[MAXP][40]; static int g_nwAnyN, g_nwAnySeeded;
static long g_nwWinT; static int g_nwWinN, g_nwWinSkip;

static void nw_lock(void) { while (__sync_lock_test_and_set(&g_nwLock, 1)) usleep(100); }
static void nw_unlock(void) { __sync_lock_release(&g_nwLock); }

static void nw_norm(const char* s, char* o, int cap) {      /* нижний регистр (латиница и кириллица), «_» → пробел */
    int n = 0;
    for (; s && *s && n + 2 < cap; s++) {
        u8 c = (u8)*s;
        if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
        else if (c == '_') c = ' ';
        else if (c == 0xD0 && (u8)s[1] >= 0x90 && (u8)s[1] <= 0xAF) {
            u8 d = (u8)s[1];
            if (d <= 0x9F) { o[n++] = (char)0xD0; o[n++] = (char)(d + 0x20); }
            else { o[n++] = (char)0xD1; o[n++] = (char)(d - 0x20); }
            s++; continue;
        }
        o[n++] = (char)c;
    }
    o[n] = 0;
}
static int nw_has(const char* hay, const char* nd) {
    int i, L = (int)slen(nd);
    if (!L) return 0;
    for (i = 0; hay[i]; i++) { int k = 0; while (k < L && hay[i + k] == nd[k]) k++; if (k == L) return 1; }
    return 0;
}
static int nw_isnum(const char* s) { int i = 0; if (!s[0]) return 0; for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0; return i <= 5; }
static int nw_atoi(const char* s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static void nw_cpy(char* d, const char* s, int cap) { int i = 0; for (; s && s[i] && i < cap - 1; i++) d[i] = s[i]; d[i] = 0; }

/* путь соседнего файла в HLOG (как у near_snap.log) */
static int nw_path(const char* name, char* out, int cap) {
    unsigned L = slen(g_flog), i; Buf b;
    if (!g_flog[0] || !ends_with(g_flog, HM_LOGNAME)) return 0;
    b_init(&b, out, cap);
    for (i = 0; i + HM_LOGNAME_LEN < L && b.n + 1 < b.cap; i++) b_ch(&b, g_flog[i]);
    b_s(&b, name);
    return 1;
}
static void nw_save(void) {                                   /* список целей → HLOG/near_watch.txt */
    char path[320], t[900]; Buf b; int i;
    if (!nw_path("near_watch.txt", path, sizeof path)) return;
    b_init(&b, t, sizeof t);
    nw_lock();
    for (i = 0; i < NW_MAX; i++) if (g_nw[i].used) { b_s(&b, g_nw[i].id >= 0 ? "id " : "nick "); if (g_nw[i].id >= 0) b_int(&b, g_nw[i].id); else b_s(&b, g_nw[i].q); b_ch(&b, '\n'); }
    if (g_nwAny) b_s(&b, "any 1\n");
    if (!g_nwChat) b_s(&b, "chat 0\n");
    nw_unlock();
    flog_write(path, b.p, b.n, 1);
}
static int nw_add_locked(const char* tok) {                   /* 1 — добавлена, 0 — уже есть, -1 — места нет */
    int i, id = -1; char qn[40];
    if (nw_isnum(tok)) id = nw_atoi(tok); else nw_norm(tok, qn, sizeof qn);
    for (i = 0; i < NW_MAX; i++) if (g_nw[i].used && ((id >= 0 && g_nw[i].id == id) || (id < 0 && g_nw[i].id < 0 && seq(g_nw[i].qn, qn)))) return 0;
    for (i = 0; i < NW_MAX; i++) if (!g_nw[i].used) {
        NwT* t = &g_nw[i];
        { char* z = (char*)t; unsigned k; for (k = 0; k < sizeof *t; k++) z[k] = 0; }
        t->used = 1; t->id = id; t->cid = -1; nw_cpy(t->q, tok, sizeof t->q);
        if (id < 0) nw_cpy(t->qn, qn, sizeof t->qn);
        return 1;
    }
    return -1;
}
static void nw_load(void) {                                   /* один раз, когда путь журнала уже известен */
    char path[320], t[900]; int fd, p = 0; long n;
    if (g_nwLoaded) return;
    if (!nw_path("near_watch.txt", path, sizeof path)) return;
    g_nwLoaded = 1;
    fd = open(path, 0);
    if (fd < 0) return;
    n = read(fd, t, sizeof t - 1); close(fd);
    if (n <= 0) return;
    t[n] = 0;
    nw_lock();
    while (t[p]) {
        char line[64]; int q = 0;
        while (t[p] && t[p] != '\n' && t[p] != '\r') { if (q < 63) line[q++] = t[p]; p++; }
        while (t[p] == '\n' || t[p] == '\r') p++;
        line[q] = 0;
        if (sprefix(line, "id ")) nw_add_locked(line + 3);
        else if (sprefix(line, "nick ")) nw_add_locked(line + 5);
        else if (seq(line, "any 1")) g_nwAny = 1;
        else if (seq(line, "chat 0")) g_nwChat = 0;
    }
    nw_unlock();
    dlog("nwatch: список слежения восстановлен из HLOG/near_watch.txt");
}

/* ── событие (вызывать ТОЛЬКО под nw_lock) ── */
static void nw_ev(int tgt, int id, const char* kind, const char* nick, float d, const char* msg, int chat) {
    NwEv* e = &g_nwEv[g_nwSeq % NW_EVN]; char path[320], line[300]; Buf b;
    e->seq = ++g_nwSeq; e->ms = mono_ms(); e->tgt = tgt; e->id = id; e->d = d;
    nw_cpy(e->kind, kind, sizeof e->kind); nw_cpy(e->nick, nick, sizeof e->nick); nw_cpy(e->msg, msg, sizeof e->msg);
    if (g_nwLogBytes < NW_LOGMAX && nw_path("near_watch.log", path, sizeof path)) {
        b_init(&b, line, sizeof line);
        b_ch(&b, '+'); b_int(&b, time(NULL) - g_flogT0); b_s(&b, "с #"); b_int(&b, e->seq); b_ch(&b, ' '); b_s(&b, msg); b_ch(&b, '\n');
        if (flog_write(path, b.p, b.n, 0)) g_nwLogBytes += b.n;
    }
    if (tgt >= 0) dlog2("nwatch: ", msg);                      /* в konstt.log (лимит 4000 строк) — только события целей, «any» — лишь в near_watch.log */
    if (chat && g_nwChat) {
        if (g_nwqH - g_nwqT >= NW_Q) g_nwqT++;                  /* очередь полна — теряем самое старое */
        nw_cpy(g_nwq[g_nwqH % NW_Q], msg, sizeof g_nwq[0]);
        g_nwqH++;
    }
}

/* ── главный поток: канал для сообщений в чат ── */
static void native_nwatch_cmd(void* self, void* args, void* mi);
static int nw_grab_ctx(void) {                                 /* как mem_grab_ctx: UnitySynchronizationContext главного потока без команды */
    void *img, **th, *kcb, *pm; uptr n = 0, i; void** (*threads_all)(uptr*);
    if (g_mt_ctx && g_mt_postM && g_mt_kCb) return 1;
    if (g_nwCtxTries >= 60 || !g_ready || g_tplState != 1 || !g_tplDelegate || !g_il2cpp) return 0;
    { static long last; long now = mono_ms(); if (now - last < 3000) return 0; last = now; }
    g_nwCtxTries++;
    *(void**)&threads_all = dlsym(g_il2cpp, "il2cpp_thread_get_all_attached_threads");
    if (!threads_all) { g_nwCtxTries = 99; dlog("nwatch: нет il2cpp_thread_get_all_attached_threads — в чат пишет только команда /nwatch"); return 0; }
    ensure_attached();
    th = threads_all(&n);
    if (!th || n == 0 || n > 4096) return 0;
    for (i = 0; i < n; i++) {
        void *t = th[i], *ec, *sc;
        if (!t || !hm_obj_ok(t)) continue;
        ec = ref_field_s(t, "m_ExecutionContext"); if (!ec) continue;
        sc = ref_field_s(ec, "_syncContext"); if (!sc) sc = ref_field_s(ec, "_syncContextNoFlow");
        if (!sc || !hm_obj_ok(sc) || !seq(A.class_get_name(A.object_get_class(sc)), "UnitySynchronizationContext")) continue;
        pm = find_method(A.object_get_class(sc), "Post", 2);
        if (!pm) return 0;
        img = find_image_pre("mscorlib");
        kcb = img ? A.class_from_name(img, "System.Threading", "SendOrPostCallback") : NULL;
        g_mt_kCb = (kcb && A.class_instance_size && A.class_instance_size(kcb) == g_cmdSize) ? kcb : g_cmdKls;
        g_mt_postM = pm; g_mt_ctx = sc;
        dlog("nwatch: главный поток найден сам, сообщения в чат пойдут без команды");
        return 1;
    }
    return 0;
}
static void nw_flush_chat(void) {                              /* в ГЛАВНОМ потоке */
    while (g_nwqT != g_nwqH) { char m[220]; nw_cpy(m, g_nwq[g_nwqT % NW_Q], sizeof m); g_nwqT++; chat_say(m); }
    g_nwPostPend = 0;
}
static void nw_pump(void) {                                    /* из сборщика: если в очереди есть сообщения — просим главный поток вывести */
    void *cb, *st, *a[2], *exc = NULL; char et[200]; long now;
    if (g_nwqT == g_nwqH) return;
    now = mono_ms();
    if (g_nwPostPend && now - g_nwPostT < 3000) return;
    if (!nw_grab_ctx()) {
        if (!g_nwNoCtxLogged && g_nwCtxTries >= 5) { g_nwNoCtxLogged = 1; dlog("nwatch: канала в главный поток нет — события только в near_watch.log и /watch, пока не введёте любую команду /nwatch"); }
        return;
    }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_nwatch_cmd);
    st = A.string_new("nw-flush");
    if (!cb || !st) return;
    g_nwPostPend = 1; g_nwPostT = now;
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("nwatch: Post бросил: ", et); g_nwPostPend = 0; }
}

/* ── чтение игроков из пула ── */
static void nw_fill_p(void* it, NwP* q) {
    float pos[3]; int ok, has = 0, got; PInfo pi;
    q->id = int_field(it, "m_ID", NULL, NULL, -1);
    read_label(it, &pi);
    if (q->id < 0 && pi.labelId >= 0) q->id = pi.labelId;
    nw_cpy(q->nick, pi.nick, sizeof q->nick);
    q->hasPos = read_pos_k(it, pos, NULL);
    got = bool_field(it, "m_IsPositionReceived", &has);
    if (q->hasPos && has && !got && pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f) q->hasPos = 0;
    q->x = q->hasPos ? pos[0] : 0; q->y = q->hasPos ? pos[1] : 0; q->z = q->hasPos ? pos[2] : 0;
    q->hp = num_field(it, "m_Health", &ok);
    q->veh = ref_field(it, "Vehicle") ? 1 : 0;
    q->inStream = 0;
}
static void nw_pool_cb(void* it, void* ctx) {
    (void)ctx;
    if (g_nwNP >= NW_POOLMAX || !it || it == g_local || !hm_obj_ok(it)) return;
    nw_fill_p(it, &g_nwP[g_nwNP]);
    if (g_nwP[g_nwNP].nick[0]) g_nwPoolNicks++;
    g_nwNP++;
}
static int nw_scan_pool(void* pool) {                          /* все игроки пула (не только зона стрима); -1 — словарь не прочитался */
    void *dict, *arr; int total, i, k;
    g_nwNP = 0; g_nwPoolNicks = 0;
    dict = ref_field_s(pool, "m_Items");
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (!arr) return -1;
    total = int_field(dict, "_count", "count", NULL, -1);
    walk_struct_array(arr, total, "value", nw_pool_cb, NULL);
    for (i = 0; i < g_nwNP; i++) for (k = 0; k < g_np; k++) if (g_pl[k].id == g_nwP[i].id) { g_nwP[i].inStream = 1; break; }
    return total;
}
static int nw_get_direct(void* pool, int id, NwP* q) {         /* PlayerPool.Get(id) */
    void *m, *args[1], *exc = NULL, *r; int v = id;
#ifdef KONSTT_TEST
    (void)pool; (void)q; (void)v; (void)m; (void)args; (void)exc; (void)r; return 0;
#else
    if (g_nwGetFail >= 3) return 0;
    m = find_method(A.object_get_class(pool), "Get", 1);
    if (!m) { g_nwGetFail = 99; return 0; }
    args[0] = &v;
    r = A.runtime_invoke(m, pool, args, &exc);
    if (exc) { g_nwGetFail++; return 0; }
    if (!r || !hm_obj_ok(r) || r == g_local) return 0;
    nw_fill_p(r, q);
    if (q->id < 0) q->id = id;
    return 1;
#endif
}

/* ── сопоставление цели и игрока: 0 нет, 1 частично, 2 точно, -1 это тот же ID, но другой игрок ── */
static int nw_match(const NwT* t, int pid, const char* nick) {
    char a[64], n[64];
    if (nick[0]) nw_norm(nick, n, sizeof n); else n[0] = 0;
    if (t->id >= 0) {
        if (t->nick[0] && n[0]) {
            nw_norm(t->nick, a, sizeof a);
            if (seq(a, n)) return 2;                           /* тот же ник — следуем и при смене ID */
            return pid == t->id ? -1 : 0;
        }
        return pid == t->id ? 2 : 0;
    }
    if (!n[0]) return 0;
    if (seq(n, t->qn)) return 2;
    if (slen(t->qn) >= 3 && nw_has(n, t->qn)) return 1;
    return 0;
}
static void nw_consider(NwT* t, int pid, const char* nick, int hasPos, float x, float y, float z, int veh, double hp, int how, NwC* best) {
    int sc = nw_match(t, pid, nick);
    if (sc < 0) { t->idTaken = 1; return; }
    if (sc == 0) return;
    if (best->ok && (sc < best->score || (sc == best->score && !(pid == t->cid && best->id != t->cid)))) return;
    best->ok = 1; best->score = sc; best->id = pid; best->hasPos = hasPos; best->x = x; best->y = y; best->z = z; best->veh = veh; best->hp = hp; best->how = how;
    nw_cpy(best->nick, nick, sizeof best->nick);
}
static void nw_distfmt(Buf* b, const NwT* t, int havePos) {
    if (t->hasPos && havePos) { b_fix(b, t->d, 1); b_s(b, " м"); } else b_s(b, "расстояние неизвестно");
}
static void nw_name(Buf* b, const NwT* t) {
    b_s(b, t->nick[0] ? t->nick : t->q); b_s(b, " ["); if (t->cid >= 0) b_int(b, t->cid); else b_ch(b, '?'); b_ch(b, ']');
}
static const char* g_nwHow[] = { "?", "зона/ID", "зона/ник", "пул/ID", "пул/ник", "Get(id)" };

static void nw_transition(NwT* t, int idx, int ns, int havePos) {
    char m[220]; Buf b; const char* kind;
    b_init(&b, m, sizeof m);
    b_s(&b, "Near: "); nw_name(&b, t);
    if (ns == 1) {
        kind = t->st == 2 ? "enter" : "appear";
        b_s(&b, t->st == 2 ? " вошёл в зону: " : t->st == 3 ? " появился в игре и сразу в зоне: " : " в зоне: ");
        nw_distfmt(&b, t, havePos); if (t->veh) b_s(&b, ", в машине");
        if (t->hasPos && havePos) { b_s(&b, " (по горизонтали "); b_fix(&b, t->d2, 1); b_s(&b, ")"); }
    } else if (ns == 2) {
        kind = t->st == 1 ? "leave" : "online";
        b_s(&b, t->st == 1 ? (g_nwPoolTotal >= 0 ? " вышел из зоны, он онлайн" : " вышел из зоны (пул игроков не читается — онлайн ли, не знаю)") : " онлайн, но вне зоны стрима");
        if (t->hasPos && havePos) { b_s(&b, " (последняя позиция "); b_fix(&b, t->d, 0); b_s(&b, " м)"); }
    } else {
        kind = "gone";
        b_s(&b, t->st == 0 ? " не найден: ни в зоне, ни в пуле игроков" : " пропал из пула игроков (вышел из игры или реконнект)");
        if (t->idTaken) b_s(&b, "; его ID теперь у другого игрока");
    }
    nw_ev(idx, t->cid, kind, t->nick[0] ? t->nick : t->q, t->hasPos && havePos ? t->d : -1.0f, m, 1);
}
static void nw_any_flush(void) {                               /* «ещё N событий» — по таймеру, чтобы сводка не ждала следующего события */
    long now = mono_ms();
    if (g_nwWinSkip > 0 && now - g_nwWinT > 3000) {
        char m[220]; Buf b; b_init(&b, m, sizeof m);
        b_s(&b, "Near: ещё "); b_int(&b, g_nwWinSkip); b_s(&b, " событий «любой игрок» не показано (много людей) — всё в near_watch.log");
        nw_ev(-1, -1, "any_more", "", -1, m, 1);
        g_nwWinT = now; g_nwWinN = 0; g_nwWinSkip = 0;
    }
}
static void nw_say_any(int appear, const char* what, int id, const char* nick, float d, int havePos) {   /* режим any: не больше 4 сообщений в чат за 3 с */
    char m[220]; Buf b; long now = mono_ms(); int chat = 1;
    if (now - g_nwWinT > 3000) {
        if (g_nwWinSkip > 0) { b_init(&b, m, sizeof m); b_s(&b, "Near: ещё "); b_int(&b, g_nwWinSkip); b_s(&b, " событий «любой игрок» не показано (много людей) — всё в near_watch.log"); nw_ev(-1, -1, "any_more", "", -1, m, 1); }
        g_nwWinT = now; g_nwWinN = 0; g_nwWinSkip = 0;
    }
    if (g_nwWinN >= 4) { chat = 0; g_nwWinSkip++; } else g_nwWinN++;
    b_init(&b, m, sizeof m);
    b_s(&b, "Near: "); b_s(&b, nick[0] ? nick : "игрок"); b_s(&b, " ["); b_int(&b, id); b_s(&b, "] "); b_s(&b, what);
    if (d >= 0 && havePos) { b_s(&b, ": "); b_fix(&b, d, 1); b_s(&b, " м"); }
    nw_ev(-1, id, appear ? "any_appear" : "any_leave", nick, havePos ? d : -1.0f, m, chat);
}

static void nw_reset(void) {                                   /* игра сбросила пул (реконнект, загрузка): состояния «неизвестно», события вспыхнут заново */
    int i;
    nw_lock();
    for (i = 0; i < NW_MAX; i++) { g_nw[i].st = 0; g_nw[i].miss = 0; }
    g_nwAnySeeded = 0; g_nwAnyN = 0; g_nwPoolTotal = -2; g_nwNP = 0;
    nw_unlock();
}

static void nw_publish(const float* me, int havePos, int meId) {   /* JSON целей для /watch (под nw_lock) */
    Buf b; int next = g_nwTjCur ^ 1, i; long now = mono_ms();
    b_init(&b, g_nwTj[next], sizeof g_nwTj[next]);
    b_s(&b, "\"watch\":{\"any\":"); b_int(&b, g_nwAny); b_s(&b, ",\"chat\":"); b_int(&b, g_nwChat);
    b_s(&b, ",\"pool\":"); b_int(&b, g_nwPoolTotal); b_s(&b, ",\"poolNicks\":"); b_int(&b, g_nwPoolNicks); b_s(&b, ",\"stream\":"); b_int(&b, g_np);
    b_s(&b, ",\"me\":{\"id\":"); b_int(&b, meId);
    if (havePos) { b_s(&b, ",\"x\":"); b_fix(&b, me[0], 1); b_s(&b, ",\"y\":"); b_fix(&b, me[1], 1); b_s(&b, ",\"z\":"); b_fix(&b, me[2], 1); }
    b_s(&b, "},\"t\":[");
    for (i = 0; i < NW_MAX; i++) {
        NwT* t = &g_nw[i]; int first = (b.p[b.n - 1] == '[');
        if (!t->used) continue;
        if (!first) b_ch(&b, ',');
        b_s(&b, "{\"i\":"); b_int(&b, i); b_s(&b, ",\"q\":"); b_jstr(&b, t->q);
        b_s(&b, ",\"id\":"); b_int(&b, t->cid); b_s(&b, ",\"nick\":"); b_jstr(&b, t->nick);
        b_s(&b, ",\"st\":"); b_int(&b, t->st); b_s(&b, ",\"how\":"); b_int(&b, t->how);
        b_s(&b, ",\"veh\":"); b_int(&b, t->veh); b_s(&b, ",\"hp\":"); b_fix(&b, t->hp, 0);
        if (t->hasPos) {
            b_s(&b, ",\"x\":"); b_fix(&b, t->x, 1); b_s(&b, ",\"y\":"); b_fix(&b, t->y, 1); b_s(&b, ",\"z\":"); b_fix(&b, t->z, 1);
            if (havePos) { b_s(&b, ",\"d\":"); b_fix(&b, t->d, 1); b_s(&b, ",\"d2\":"); b_fix(&b, t->d2, 1); }
        }
        b_s(&b, ",\"since\":"); b_int(&b, t->tSince ? now - t->tSince : 0);
        b_s(&b, ",\"seenAgo\":"); b_int(&b, t->tSeen ? now - t->tSeen : -1);
        b_s(&b, ",\"idTaken\":"); b_int(&b, t->idTaken);
        b_ch(&b, '}');
    }
    b_s(&b, "]}");
    g_nwTjCur = next;
}

/* ── главная проверка: каждый тик сборщика (после чтения PlayersInStream в g_pl) ── */
static void nw_tick(const float* me, int havePos, int meId, void* pool) {
    int i, k, any = 0, doPool;
    for (i = 0; i < NW_MAX; i++) if (g_nw[i].used) any = 1;
    if (!any && !g_nwAny) { nw_lock(); nw_publish(me, havePos, meId); nw_unlock(); return; }
    doPool = (g_ticks % 5 == 1) || g_nwPoolTotal == -2;       /* весь пул — раз в ~1,5 с (ники читаются через IL2CPP, это дороже) */
    if (doPool) {
#ifdef KONSTT_TEST
        int tot = g_nwTestTotal;                              /* юнит-тест подставляет g_nwP сам */
#else
        int tot = nw_scan_pool(pool);
#endif
        if (tot != g_nwPoolTotal) { if (g_nwPoolTotal == -2 || tot < 0) dlog_i("nwatch: пул игроков, записей (-1 — не прочитался) ", tot); }
        g_nwPoolTotal = tot;
        if (tot >= 0) { static int once; if (!once) { once = 1; dlog_i("nwatch: игроков в пуле с читаемым ником ", g_nwPoolNicks); } }
    } else {                                                  /* между сканами пула — только обновить флаг «в зоне» */
        for (i = 0; i < g_nwNP; i++) { g_nwP[i].inStream = 0; for (k = 0; k < g_np; k++) if (g_pl[k].id == g_nwP[i].id) { g_nwP[i].inStream = 1; break; } }
    }
    nw_lock();
    for (i = 0; i < NW_MAX; i++) {
        NwT* t = &g_nw[i]; NwC best; int ns, oldId;
        if (!t->used) continue;
        { char* z = (char*)&best; unsigned q; for (q = 0; q < sizeof best; q++) z[q] = 0; }
        t->idTaken = 0;
        for (k = 0; k < g_np; k++) { PInfo* p = &g_pl[k]; nw_consider(t, p->id, p->nick, p->hasPos, p->x, p->y, p->z, p->veh, p->hp, 1, &best); }
        if (best.ok) { best.how = (t->id >= 0 || nw_isnum(t->q)) ? 1 : 2; ns = 1; }
        else {
            for (k = 0; k < g_nwNP; k++) { NwP* p = &g_nwP[k]; if (!p->inStream) nw_consider(t, p->id, p->nick, p->hasPos, p->x, p->y, p->z, p->veh, p->hp, (t->id >= 0 || nw_isnum(t->q)) ? 3 : 4, &best); }
            if (!best.ok && doPool && t->id >= 0 && g_nwPoolTotal >= 0) {      /* словарь не нашёл — спрашиваем пул напрямую */
                NwP q;
                if (nw_get_direct(pool, t->id, &q) && nw_match(t, q.id, q.nick) > 0) {
                    best.ok = 1; best.score = 2; best.id = q.id; best.hasPos = q.hasPos; best.x = q.x; best.y = q.y; best.z = q.z; best.veh = q.veh; best.hp = q.hp; best.how = 5;
                    nw_cpy(best.nick, q.nick, sizeof best.nick);
                }
            }
            if (best.ok) ns = 2;
            else ns = (g_nwPoolTotal >= 0) ? 3 : (t->st == 1 ? 2 : 0);   /* пул не читается — «не знаю», а не «пропал» */
        }
        if (best.ok) {
            oldId = t->cid;
            if (best.nick[0]) nw_cpy(t->nick, best.nick, sizeof t->nick);
            t->cid = best.id; t->how = best.how; t->hasPos = best.hasPos; t->veh = best.veh; t->hp = best.hp;
            if (best.hasPos) { t->x = best.x; t->y = best.y; t->z = best.z; }
            if (t->hasPos && havePos) {
                float dx = t->x - me[0], dy = t->y - me[1], dz = t->z - me[2];
                t->d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz); t->d2 = __builtin_sqrtf(dx * dx + dy * dy);
            }
            t->tSeen = mono_ms();
            if (t->id >= 0 && best.id != t->id && t->st != 0) {   /* слежка по ID: тот же ник у другого ID — перезаход */
                char m[200]; Buf b; b_init(&b, m, sizeof m);
                b_s(&b, "Near: "); b_s(&b, t->nick); b_s(&b, " сменил ID: "); b_int(&b, t->id); b_s(&b, " → "); b_int(&b, best.id); b_s(&b, " (слежу дальше по нику)");
                nw_ev(i, best.id, "newid", t->nick, -1, m, 1);
            }
            if (t->id >= 0) t->id = best.id;
            (void)oldId;
        }
        /* смена состояния с задержкой на уход (мерцание на границе зоны не должно сыпать сообщениями) */
        if (ns == t->st || (ns == 0 && t->st != 0)) { t->miss = 0; }
        else if (t->st == 0 || ns == 1 || (t->st == 3 && ns == 2)) {
            nw_transition(t, i, ns, havePos); t->st = ns; t->miss = 0; t->tSince = mono_ms();
        } else {
            int lim = (t->st == 1) ? NW_MISS : NW_MISS_POOL;
            if (++t->miss >= lim) { nw_transition(t, i, ns, havePos); t->st = ns; t->miss = 0; t->tSince = mono_ms(); }
        }
        if (ns == 3 && t->st == 3) t->hasPos = 0;
    }
    if (g_nwAny) {                                             /* «любой игрок»: новые в зоне и ушедшие из зоны */
        int j;
        for (k = 0; k < g_np; k++) {
            PInfo* p = &g_pl[k]; int found = -1, tracked = 0;
            if (p->id < 0) continue;
            for (j = 0; j < g_nwAnyN; j++) if (g_nwAnyId[j] == p->id) { found = j; break; }
            for (j = 0; j < NW_MAX; j++) if (g_nw[j].used && g_nw[j].cid == p->id && g_nw[j].st == 1) tracked = 1;
            if (found >= 0) { g_nwAnyMiss[found] = 0; nw_cpy(g_nwAnyNick[found], p->nick, sizeof g_nwAnyNick[0]); continue; }
            if (g_nwAnyN >= MAXP) continue;
            g_nwAnyId[g_nwAnyN] = p->id; g_nwAnyMiss[g_nwAnyN] = 0; nw_cpy(g_nwAnyNick[g_nwAnyN], p->nick, sizeof g_nwAnyNick[0]); g_nwAnyN++;
            if (g_nwAnySeeded && !tracked) {
                float d = -1;
                if (p->hasPos && havePos) { float dx = p->x - me[0], dy = p->y - me[1], dz = p->z - me[2]; d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz); }
                nw_say_any(1, "появился в зоне", p->id, p->nick, d, havePos);
            }
        }
        for (j = 0; j < g_nwAnyN; ) {
            int present = 0;
            for (k = 0; k < g_np; k++) if (g_pl[k].id == g_nwAnyId[j]) { present = 1; break; }
            if (present) { j++; continue; }
            if (++g_nwAnyMiss[j] < NW_MISS) { j++; continue; }
            if (g_nwAnySeeded) { int tr = 0, q; for (q = 0; q < NW_MAX; q++) if (g_nw[q].used && g_nw[q].cid == g_nwAnyId[j]) tr = 1; if (!tr) nw_say_any(0, "ушёл из зоны", g_nwAnyId[j], g_nwAnyNick[j], -1, 0); }
            g_nwAnyId[j] = g_nwAnyId[g_nwAnyN - 1]; g_nwAnyMiss[j] = g_nwAnyMiss[g_nwAnyN - 1]; nw_cpy(g_nwAnyNick[j], g_nwAnyNick[g_nwAnyN - 1], sizeof g_nwAnyNick[0]); g_nwAnyN--;
        }
        g_nwAnySeeded = 1;
        nw_any_flush();
    } else { g_nwAnySeeded = 0; g_nwAnyN = 0; }
    nw_publish(me, havePos, meId);
    nw_unlock();
}

/* ── JSON событий для GET /watch?since=N (под nw_lock) ── */
static void nw_events_json(Buf* b, long since) {
    long s; int first = 1;
    b_s(b, ",\"seq\":"); b_int(b, g_nwSeq); b_s(b, ",\"ev\":[");
    for (s = g_nwSeq > NW_EVN ? g_nwSeq - NW_EVN + 1 : 1; s <= g_nwSeq; s++) {
        NwEv* e = &g_nwEv[(s - 1) % NW_EVN];
        if (e->seq != s || s <= since) continue;
        if (!first) b_ch(b, ',');
        first = 0;
        b_s(b, "{\"seq\":"); b_int(b, e->seq); b_s(b, ",\"ms\":"); b_int(b, e->ms); b_s(b, ",\"k\":"); b_jstr(b, e->kind);
        b_s(b, ",\"t\":"); b_int(b, e->tgt); b_s(b, ",\"id\":"); b_int(b, e->id); b_s(b, ",\"nick\":"); b_jstr(b, e->nick);
        if (e->d >= 0) { b_s(b, ",\"d\":"); b_fix(b, e->d, 1); }
        b_s(b, ",\"msg\":"); b_jstr(b, e->msg); b_ch(b, '}');
    }
    b_s(b, "]");
}

/* ── команда /nwatch ── */
static int nw_tokens(void* arr, char tok[][48], int maxn) {
    long len; int i, n = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[160]; int cnt, p = 0; const u16* ch;
        if (!s) continue;
        cnt = A.string_length(s); ch = A.string_chars(s);
        if (cnt <= 0 || cnt > 40 || !ch) continue;
        utf16_to_utf8(ch, cnt, tmp, sizeof tmp);
        while (tmp[p]) {
            char w[48]; int q = 0, z;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 47) w[q++] = tmp[p]; p++; }
            w[q] = 0;
            if (!q || w[0] == '/' || seq(w, "nwatch")) continue;
            if (n < maxn) { for (z = 0; z <= q; z++) tok[n][z] = w[z]; n++; }
        }
    }
    return n;
}
static void nw_status(void) {
    char m[220]; Buf b; int i, n = 0;
    nw_lock();
    for (i = 0; i < NW_MAX; i++) {
        NwT* t = &g_nw[i]; if (!t->used) continue; n++;
        b_init(&b, m, sizeof m);
        b_s(&b, "Near слежу "); b_int(&b, i + 1); b_s(&b, ") «"); b_s(&b, t->q); b_s(&b, "» → "); nw_name(&b, t); b_s(&b, ": ");
        b_s(&b, t->st == 1 ? "в зоне " : t->st == 2 ? "онлайн, вне зоны " : t->st == 3 ? "нет в пуле" : "ещё не определено");
        if (t->st == 1 || t->st == 2) { if (t->hasPos) { b_fix(&b, t->d, 1); b_s(&b, " м"); } else b_s(&b, "позиция неизвестна"); b_s(&b, ", способ: "); b_s(&b, g_nwHow[t->how < 6 ? t->how : 0]); }
        if (t->idTaken) b_s(&b, "; этот ID теперь у другого игрока");
        chat_say(m);
    }
    nw_unlock();
    b_init(&b, m, sizeof m);
    b_s(&b, n ? "Near: режим «любой игрок» " : "Near: целей нет; режим «любой игрок» "); b_s(&b, g_nwAny ? "ВКЛ" : "выкл"); b_s(&b, ", сообщения в чат "); b_s(&b, g_nwChat ? "ВКЛ" : "выкл");
    chat_say(m);
}
static void nw_usage(void) {
    chat_usage("/nwatch ID или ник — следить (до 8 целей; ник можно частью). Пишу в чат и в HLOG/near_watch.log, когда появился/вышел из зоны/пропал");
    chat_usage("/nwatch off ID|ник|all — снять;  /nwatch any — вкл/выкл «любой новый игрок в зоне»;  /nwatch list — состояние");
    chat_usage("/nwatch chat — вкл/выкл сообщения в чат (файл и http://127.0.0.1:17771/watch остаются);  /nwatch diag — диагностика");
}
static void native_nwatch_cmd(void* self, void* args, void* mi) {
    char tok[6][48]; int nt, i;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { nw_flush_chat(); return; }   /* тик из сборщика, а не команда */
    mt_capture_ctx();                                            /* мы в главном потоке: запомним канал для сообщений */
    nt = nw_tokens(args, tok, 6);
    if (nt == 0 || seq(tok[0], "list") || seq(tok[0], "status") || seq(tok[0], "список")) { nw_status(); return; }
    if (seq(tok[0], "help") || seq(tok[0], "?") || seq(tok[0], "помощь")) { nw_usage(); return; }
    if (seq(tok[0], "diag")) {
        char m[220]; Buf b; b_init(&b, m, sizeof m);
        b_s(&b, "Near diag: пул "); b_int(&b, g_nwPoolTotal); b_s(&b, " (с ником "); b_int(&b, g_nwPoolNicks); b_s(&b, "), в зоне "); b_int(&b, g_np);
        b_s(&b, ", главный поток "); b_s(&b, (g_mt_ctx && g_mt_postM) ? "есть" : "нет"); b_s(&b, ", Get(id) сбоев "); b_int(&b, g_nwGetFail);
        chat_say(m); return;
    }
    if (seq(tok[0], "any") || seq(tok[0], "все") || seq(tok[0], "всех")) {
        g_nwAny = !g_nwAny; nw_save();
        chat_say(g_nwAny ? "Near: режим «любой игрок» ВКЛ — напишу о каждом, кто появился в зоне или ушёл (4 сообщения в 3 с, остальное в near_watch.log)" : "Near: режим «любой игрок» выкл");
        return;
    }
    if (seq(tok[0], "chat") || seq(tok[0], "чат")) { g_nwChat = !g_nwChat; nw_save(); chat_say(g_nwChat ? "Near: сообщения слежения в чат ВКЛ" : "Near: сообщения слежения в чат выкл (файл near_watch.log и /watch работают)"); return; }
    if (seq(tok[0], "off") || seq(tok[0], "del") || seq(tok[0], "stop") || seq(tok[0], "выкл") || seq(tok[0], "стоп")) {
        int removed = 0;
        if (nt < 2 || seq(tok[1], "all") || seq(tok[1], "все")) { nw_lock(); for (i = 0; i < NW_MAX; i++) { removed += g_nw[i].used; g_nw[i].used = 0; } nw_unlock(); }
        else {
            char qn[40]; int id = nw_isnum(tok[1]) ? nw_atoi(tok[1]) : -1;
            if (id < 0) nw_norm(tok[1], qn, sizeof qn);
            nw_lock();
            for (i = 0; i < NW_MAX; i++) {
                NwT* t = &g_nw[i]; char nn[64];
                if (!t->used) continue;
                nw_norm(t->nick, nn, sizeof nn);
                if ((id >= 0 && (t->id == id || t->cid == id)) || (id < 0 && (seq(t->qn, qn) || seq(nn, qn)))) { t->used = 0; removed++; }
            }
            nw_unlock();
        }
        nw_save();
        { char m[120]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "Near: снято целей: "); b_int(&b, removed); chat_say(m); }
        return;
    }
    {                                                            /* /nwatch <ID|ник> [ещё...] */
        int added = 0, dup = 0, full = 0;
        for (i = 0; i < nt; i++) {
            int r;
            if (seq(tok[i], "on") || seq(tok[i], "add") || seq(tok[i], "вкл")) continue;
            nw_lock(); r = nw_add_locked(tok[i]); nw_unlock();
            if (r > 0) added++; else if (r == 0) dup++; else full++;
        }
        nw_save();
        { char m[200]; Buf b; b_init(&b, m, sizeof m);
          if (added) { b_s(&b, "Near: слежу, добавлено целей: "); b_int(&b, added); b_s(&b, " — напишу, как только найду (способы: зона, пул игроков, Get(id), ник)"); }
          else if (dup) b_s(&b, "Near: эта цель уже на слежении (/nwatch list)");
          else b_s(&b, "Near: нет места, максимум 8 целей (/nwatch off ID)");
          if (added && full) b_s(&b, "; часть не влезла (максимум 8)");
          chat_say(m); }
    }
}

/* ───────────── итоговый JSON ───────────── */
static char g_json[2][32768];
static volatile int g_jsonCur;
static volatile long g_snapMs[2];                      /* когда прочитан снимок g_json[i] — по нему при ответе считаем возраст «ago» */
static float g_meV[3]; static int g_meHasV;
static void publish(int ok, const char* why, int havePos, float mx, float my, float mz, int meId) {
    Buf b; int next = g_jsonCur ^ 1, i;
    b_init(&b, g_json[next], sizeof g_json[next]);
    b_s(&b, "{\"v\":1,\"ok\":"); b_int(&b, ok);
    b_s(&b, ",\"stage\":"); b_jstr(&b, why);
    b_s(&b, ",\"tick\":"); b_int(&b, (long)g_ticks);
    b_s(&b, ",\"me\":{\"id\":"); b_int(&b, meId); b_s(&b, ",\"src\":"); b_int(&b, g_meKind);
    if (g_mePing > 0) { b_s(&b, ",\"ping\":"); b_fix(&b, g_mePing, 0); }
    if (havePos) { b_s(&b, ",\"x\":"); b_fix(&b, mx, 1); b_s(&b, ",\"y\":"); b_fix(&b, my, 1); b_s(&b, ",\"z\":"); b_fix(&b, mz, 1); }
    if (havePos && g_meHasV) { b_s(&b, ",\"vx\":"); b_fix(&b, g_meV[0], 2); b_s(&b, ",\"vy\":"); b_fix(&b, g_meV[1], 2); b_s(&b, ",\"vz\":"); b_fix(&b, g_meV[2], 2); }
    b_s(&b, "},\"n\":[");
    for (i = 0; i < g_np; i++) {
        PInfo* p = &g_pl[i];
        if (i) b_ch(&b, ',');
        b_s(&b, "{\"id\":"); b_int(&b, p->id);
        b_s(&b, ",\"nick\":"); b_jstr(&b, p->nick);
        b_s(&b, ",\"label\":"); b_jstr(&b, p->label);
        if (p->hasPos) {
            b_s(&b, ",\"x\":"); b_fix(&b, p->x, 1); b_s(&b, ",\"y\":"); b_fix(&b, p->y, 1); b_s(&b, ",\"z\":"); b_fix(&b, p->z, 1);
            if (p->hasVel) { b_s(&b, ",\"vx\":"); b_fix(&b, p->vx, 2); b_s(&b, ",\"vy\":"); b_fix(&b, p->vy, 2); b_s(&b, ",\"vz\":"); b_fix(&b, p->vz, 2); }
            if (havePos) {
                double dx = p->x - mx, dy = p->y - my, dz = p->z - mz;
                b_s(&b, ",\"d\":"); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 1);
                b_s(&b, ",\"d2\":"); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy), 1);
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
    g_snapMs[next] = g_tickMs ? g_tickMs : mono_ms();
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
    g_tickMs = mono_ms();
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
    if (!pool) { set_stage("PlayerPool ещё не создан (вы не в игре?)"); g_np = 0; nw_reset(); publish(0, g_stage, 0, 0, 0, 0, -1); return; }

    if (!g_fieldsDumped) {
        dump_class(g_cPlayerPool, "PlayerPool");
        g_fieldsDumped = 1;
    }
    g_local = ref_field(pool, "LocalPlayer");
    if (g_local) {
        meId = int_field(g_local, "m_ID", NULL, NULL, -1);
        havePos = read_pos_k(g_local, me, &g_meKind);
        { int pk = 0; double pg = num_field(g_local, "Ping", &pk); g_mePing = pk ? pg : 0; }
        g_meHasV = 0;
        if (havePos) { vel_track(-2, me, g_tickMs, g_meV); g_meHasV = 1; }
    }
    stream = ref_field(pool, "PlayersInStream");
    if (!stream) { set_stage("PlayersInStream = null"); g_np = 0; nw_reset(); publish(0, g_stage, havePos, me[0], me[1], me[2], meId); return; }
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
    nw_tick(me, havePos, meId, pool);                    /* /nwatch: постоянное слежение (если цели есть) */
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

/* /nearid kt… — привязка окна к скрипту: запоминаем метку, которую скрипт этого окна придумал сам (см. hm_take_tag в ядре). Тихо, без сообщений в чат. */
static void native_nearid_cmd(void* self, void* args, void* mi) {
    (void)self; (void)mi;
    if (hm_take_tag(args, g_near_id, (int)sizeof g_near_id)) dlog2("near: привязка к окну, метка ", g_near_id);
}

/* /nearlog регистрируется, как только ядро нашло образец делегата (раньше это делал сборщик вместе с /rec) */
static int g_nearlogReg;
static int g_near_modId = -1;                          /* номер в реестре модов: по нему на экране загрузки считается «Запускаем моды N/M» */
static void near_try_register(void) {
    int st = __atomic_load_n(&g_tplState, __ATOMIC_ACQUIRE);
    if (g_nearlogReg || !st) return;                     /* 0 — образец ещё ищется */
    g_nearlogReg = 1;
    if (st == 1) { cmd_register("nearlog", native_nearlog_cmd); cmd_register("nearid", native_nearid_cmd); cmd_register("nwatch", native_nwatch_cmd); }
    else dlog("нативная /nearlog: образец делегата не найден, команды не будет");
}

/* ───────────── сборщик игроков ───────────── */
static void* near_collector_main(void* arg) {
    (void)arg;
    if (!hm_wait_started()) return NULL;                 /* konstt_off — мод выключен целиком */
    dlog("near: сборщик игроков запущен");
    while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) {       /* пока ядро ждёт игру — отдаём её стадию в JSON для HUD */
        publish(0, g_stage, 0, 0, 0, 0, -1);
        usleep(POLL_US);
    }
    dlog("near: игра готова, начинаю опрос игроков");
    hm_mod_ready(g_near_modId);                          /* мод заработал — на экране загрузки он засчитан */
    nw_load();                                           /* список слежения прошлого запуска (HLOG/near_watch.txt) */
    for (;;) {
        near_try_register();
        nw_load();                                       /* если путь журнала появился позже */
        collect_tick();
        nw_pump();                                       /* сообщения слежения → чат (через главный поток) */
        usleep(TICK_US);
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
                else if (sprefix(rq, "GET /watch")) {   /* слежение: цели, состояние, события с seq (?since=N — только новее N) */
                    static char wout[9216 + NW_EVN * 720 + 128];                 /* цели + все события с запасом на экранирование: JSON не должен обрезаться */ Buf wb; long since = 0; const char* q = rq; int cur;
                    while (*q && *q != '\r' && *q != '\n') { if (sprefix(q, "since=")) { q += 6; while (*q >= '0' && *q <= '9' && since < 1000000000L) since = since * 10 + (*q++ - '0'); break; } q++; }
                    b_init(&wb, wout, sizeof wout);
                    nw_lock();
                    cur = g_nwTjCur;
                    b_s(&wb, "{\"v\":1,\"tick\":"); b_int(&wb, (long)g_ticks); b_ch(&wb, ',');
                    b_s(&wb, g_nwTj[cur][0] ? g_nwTj[cur] : "\"watch\":{\"any\":0,\"t\":[]}");
                    nw_events_json(&wb, since);
                    nw_unlock();
                    b_ch(&wb, '}');
                    respond(c, "200 OK", "application/json", wb.p, wb.n);
                }
                else if (sprefix(rq, "GET /near")) {   /* к снимку дописываем "ago" — сколько мс ему было на момент ответа (Near.js двигает цель на это время) */
                    static char out[32768 + 64]; int cur = g_jsonCur; const char* j = g_json[cur]; long ago = mono_ms() - g_snapMs[cur]; Buf ob;
                    if (ago < 0) ago = 0;
                    if (ago > 60000) ago = 60000;
                    b_init(&ob, out, sizeof out);
                    b_s(&ob, "{\"ago\":"); b_int(&ob, ago); b_ch(&ob, ',');
                    b_s(&ob, j[0] == '{' ? j + 1 : j);
                    respond(c, "200 OK", "application/json", ob.p, ob.n);
                }
                else if (sprefix(rq, "GET /id")) { char id[40]; int k; for (k = 0; k < 39 && g_near_id[k]; k++) id[k] = g_near_id[k]; id[k] = 0; respond(c, "200 OK", "text/plain", id, (unsigned)k); }   /* метка окна («» — ещё не привязано) */
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
    g_near_modId = hm_mod_register("near");              /* учесть мод в счётчике — только в конструкторе */
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
