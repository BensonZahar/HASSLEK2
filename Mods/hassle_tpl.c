/*
 * hassle_tpl.c — необязательный модуль «/tpl»: маленький телепорт в любую сторону от камеры (выбраться, когда NPC/игрок заглючил
 * и /nocol не помог).
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_tpl.c и пересоберите — больше ничего править не нужно.
 *
 * Команда в чате:  /tpl [сторона] [метры]   (порядок любой, всё необязательно)
 *   /tpl                 влево на 1.5 м
 *   /tpl 3               влево на 3 м («2.5» и «2,5» тоже можно)
 *   /tpl вперёд          вперёд на 1.5 м
 *   /tpl назад 2         назад на 2 м
 *   /tpl right 4         вправо на 4 м;  слитно тоже можно: /tpl вправо4, /tpl f3
 *   стороны:  влево | left | l   вправо | right | r   вперёд (вперед) | forward | fwd | f   назад | back | b
 *   /tpl pos             показать ваши координаты (и готовую команду «/tpl to …», чтобы вернуться сюда)
 *   /tpl to X Y Z        телепорт в точку по координатам, например «/tpl to 120.5 8 -340» (числа с минусом можно)
 *   /tpl to X Z          то же, но высота — по земле (игра сама найдёт поверхность в этой точке)
 *   /tpl alt             переключить способ телепорта (запасной — через движение персонажа); если /tpl «не двигает», попробуйте его.
 *                        «/tpl alt 3 назад» — переключить И сразу прыгнуть
 *   /tpl status          какой способ сейчас выбран;  /tpl help — подсказка
 *   Расстояние — любое от 0.3 м (меньше смысла нет); верхнего лимита нет, только страховка TPL_MAX = 100000 м от переполнения float.
 *
 * Как это работает
 *   • Координаты — это Transform.position игрока в мире Unity (та же система, в которой работает SetPosition). Если координаты из пакета
 *     синхронизации (то, что уходит на сервер) заметно отличаются от них, «/tpl pos» покажет и их отдельной строкой.
 *   • Стороны — относительно КАМЕРЫ (как вы видите экран): берём Transform.right у MainCamera, обнуляем высоту, нормализуем.
 *     «Вперёд» получаем поворотом «вправо» на 90° вокруг вертикали (камера может смотреть вниз — Transform.forward тогда вырожден).
 *     Если камеры нет — берём Transform.right самого игрока.
 *   • Позиция — Transform.position (у игрока — Entity.Transform; у запасного способа — transform компонента движения), + сдвиг по
 *     горизонтали, высота не меняется.
 *   • Основной способ — Player.SetPosition(Vector3, findHeight=false): тот же метод, которым игра сама переставляет игрока (по приказу сервера).
 *     Запасной — ICharacterMovement.SetPosition(Vector3, updateGround=true): прямая постановка капсулы. Если основной бросил исключение — пробуем запасной сами.
 *   • Обработчик команды вызывается игрой в ГЛАВНОМ потоке (как у /rec и /nocol), поэтому Post/SynchronizationContext не нужны.
 *   • В машине не работает (телепорт водителя/пассажира сломал бы синхронизацию). Лимита по дальности нет: большие прыжки сервер может откатить,
 *     а на далёкой точке карта может быть не загружена (можно провалиться) — это на ваше усмотрение.
 *   • Это не «клиентская» штука, как /nocol: позиция уходит на сервер как обычная, и другие игроки вас видят на новом месте.
 */
#pragma once
#include "hassle_core.c"

#define TPL_DEF 1.5f
#define TPL_MIN 0.3f
#define TPL_MAX 100000.0f                                /* не «лимит», а страховка: больше — бесконечность/мусор для Unity (его собственный предел координат ~100000) */

enum { TPL_L = 0, TPL_R = 1, TPL_F = 2, TPL_B = 3 };
static const char* const g_tpl_sideName[4] = { "влево", "вправо", "вперёд", "назад" };

static volatile int g_tpl_alt;                           /* 1 — двигать через движение персонажа (запасной способ) */
static int g_tpl_modId = -1;                             /* номер в реестре модов (экран загрузки) */

/* ───────────── мелкие помощники ───────────── */
static int tpl_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
/* метод без аргументов, вернувший ссылочный объект (Transform, GameObject…) */
static void* tpl_obj(void* obj, const char* name) {
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
/* метод без аргументов, вернувший Vector3 (в боксе: 3 float после заголовка объекта) */
static int tpl_vec(void* obj, const char* name, float* out) {
    void *m, *exc = NULL, *r; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!tpl_finite(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
/* Transform камеры (Game.MainCamera → m_Camera → transform), как в /route */
static void* tpl_cam_transform(void) {
    void *fn, *cam = NULL, *co;
    if (!g_cGame) return NULL;
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) return NULL;
    A.field_static_get_value(fn, &cam);
    if (!cam || !hm_obj_ok(cam)) return NULL;
    co = ref_field_s(cam, "m_Camera");
    if (!co || !hm_obj_ok(co)) return NULL;
    return tpl_obj(co, "get_transform");
}
/* компонент движения: Actor.m_Controller → Character.Movement */
static void* tpl_movement(void* actor) {
    void* ctrl = ref_field_s(actor, "m_Controller");
    return ctrl ? ref_field_s(ctrl, "Movement") : NULL;
}
/* Единичный вектор «вправо» на плоскости XZ. 1 — получилось; *src: 1 — по камере, 2 — по самому игроку */
static int tpl_right_dir(void* entityTr, float* r2, int* src) {
    float r[3]; float len; void* ct = tpl_cam_transform();
    *src = 1;
    if (!ct || !tpl_vec(ct, "get_right", r)) {
        *src = 2;
        if (!entityTr || !tpl_vec(entityTr, "get_right", r)) return 0;
    }
    len = (float)__builtin_sqrt((double)r[0] * r[0] + (double)r[2] * r[2]);
    if (len < 1e-3f) return 0;                           /* «право» вырождено (поворот на 90° по тангажу) */
    r2[0] = r[0] / len; r2[1] = 0.0f; r2[2] = r[2] / len;
    return 1;
}
/* Единичный вектор нужной стороны из «вправо» (в Unity: вперёд = (-right.z, 0, right.x), т.к. right=(cos,0,-sin), forward=(sin,0,cos)) */
static void tpl_side_dir(const float* R, int side, float* d) {
    d[1] = 0.0f;
    switch (side) {
        case TPL_R: d[0] = R[0];  d[2] = R[2];  break;
        case TPL_F: d[0] = -R[2]; d[2] = R[0];  break;
        case TPL_B: d[0] = R[2];  d[2] = -R[0]; break;
        default:    d[0] = -R[0]; d[2] = -R[2]; break;   /* TPL_L */
    }
}

/* вызвать SetPosition(Vector3, bool) у объекта obj. 1 — прошло без исключения */
static int tpl_call_set(void* obj, const float* pos, u8 flag, const char* what) {
    void *m, *exc = NULL, *args[2]; float v[3]; char et[240];
    v[0] = pos[0]; v[1] = pos[1]; v[2] = pos[2];
    m = find_method(A.object_get_class(obj), "SetPosition", 2);
    if (!m) { dlog2("/tpl: SetPosition не найден у ", what); return 0; }
    args[0] = v; args[1] = &flag;
    A.runtime_invoke(m, obj, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("/tpl: SetPosition бросил исключение: ", et); return 0; }
    return 1;
}

/* ───────────── общая часть: найти игрока и его компоненты (главный поток) ───────────── */
typedef struct { void *me, *mv, *etr, *mtr; } TplCtx;
/* needOnFoot=1 — для телепорта: в машине нельзя. Сама пишет в чат, почему не вышло. 1 — можно работать. */
static int tpl_ctx(TplCtx* c, int needOnFoot) {
    c->me = c->mv = c->etr = c->mtr = NULL;
    if (!g_safe || !il2cpp_light()) { chat_say("Телепорт: игра ещё грузится, подождите"); return 0; }
    ensure_attached();
    c->me = local_player();
    if (!c->me || !hm_obj_ok(c->me)) { chat_say("Телепорт: вы не в игре (персонаж не создан)"); return 0; }
    if (needOnFoot && ref_field_s(c->me, "Vehicle")) { chat_say("Телепорт: в машине не работает — сначала выйдите"); return 0; }
    c->etr = tpl_obj(c->me, "get_Transform");                    /* Entity.Transform */
    if (!c->etr) c->etr = tpl_obj(c->me, "get_transform");       /* запасной вариант: Component.transform */
    c->mv = tpl_movement(c->me);
    if (c->mv && hm_obj_ok(c->mv)) c->mtr = tpl_obj(c->mv, "get_transform");
    else c->mv = NULL;
    return 1;
}
/* Поставить игрока в точку. npE — позиция в системе Entity.Transform (для Player.SetPosition), npM — в системе transform движения
   (для запасного способа); любой может быть NULL. findH — искать высоту по земле (только основной способ). Возврат: 1 основной, 2 запасной, 0 не вышло. */
static int tpl_place(const TplCtx* c, const float* npE, const float* npM, u8 findH) {
    if (!g_tpl_alt && npE) {
        if (tpl_call_set(c->me, npE, findH, "Player")) return 1;
        dlog("/tpl: основной способ не вышел, пробую запасной (через движение)");
    }
    if (npM && c->mv && tpl_call_set(c->mv, npM, 1, "движению")) return 2;
    return 0;
}
static void tpl_fail(void) { chat_say("Телепорт: не вышло — подробности в konstt.log. Попробуйте /tpl alt"); }

/* ───────────── сдвиг в сторону ───────────── */
static void tpl_go(int side, float dist, int clamped) {
    TplCtx c; float R[3], dir[3], eb[3], mb[3], ne[3], nm[3]; int src = 0, haveE, haveM, via; Buf b; char msg[240];
    if (!tpl_ctx(&c, 1)) return;
    if (!tpl_right_dir(c.etr ? c.etr : c.mtr, R, &src)) { chat_say("Телепорт: не удалось определить стороны (нет камеры и трансформа игрока)"); return; }
    tpl_side_dir(R, side, dir);
    haveE = c.etr && tpl_vec(c.etr, "get_position", eb);          /* Player.SetPosition: позиция — Entity.Transform */
    haveM = c.mtr && tpl_vec(c.mtr, "get_position", mb);          /* запасной способ: позиция — transform компонента движения */
    if (haveE) { ne[0] = eb[0] + dir[0] * dist; ne[1] = eb[1]; ne[2] = eb[2] + dir[2] * dist; }
    if (haveM) { nm[0] = mb[0] + dir[0] * dist; nm[1] = mb[1]; nm[2] = mb[2] + dir[2] * dist; }
    via = tpl_place(&c, haveE ? ne : NULL, haveM ? nm : NULL, 0);
    if (!via) { tpl_fail(); return; }
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Телепорт "); b_s(&b, g_tpl_sideName[side]); b_s(&b, " на "); b_fix(&b, dist, 1); b_s(&b, " м");
    if (clamped) b_s(&b, " (максимум)");
    b_s(&b, src == 1 ? " (по камере)" : " (по повороту персонажа, камера не найдена)");
    if (via == 2) b_s(&b, " [через движение]");
    chat_say(msg);
    {
        char lg[220]; Buf x; b_init(&x, lg, sizeof lg);
        b_s(&x, "/tpl: готово, сторона "); b_s(&x, g_tpl_sideName[side]); b_s(&x, ", метров "); b_fix(&x, dist, 1); b_s(&x, ", способ "); b_int(&x, via);
        b_s(&x, ", направление "); b_fix(&x, dir[0], 2); b_s(&x, ", "); b_fix(&x, dir[2], 2);
        dlog(lg);
    }
}

/* ───────────── координаты ───────────── */
static void tpl_b_xyz(Buf* b, const float* p) { b_fix(b, p[0], 1); b_ch(b, ' '); b_fix(b, p[1], 1); b_ch(b, ' '); b_fix(b, p[2], 1); }
static float tpl_fabs(float v) { return v < 0 ? -v : v; }

/* «/tpl pos»: где я. Основное — Transform.position; из пакета синхронизации (read_pos ядра) — если заметно отличается */
static void tpl_show(void) {
    TplCtx c; float p[3], sp[3]; int have = 0, haveS; Buf b; char msg[260];
    if (!tpl_ctx(&c, 0)) return;
    if (c.etr && tpl_vec(c.etr, "get_position", p)) have = 1;
    else if (c.mtr && tpl_vec(c.mtr, "get_position", p)) have = 1;
    haveS = read_pos(c.me, sp);
    if (!have && haveS) { p[0] = sp[0]; p[1] = sp[1]; p[2] = sp[2]; have = 1; haveS = 0; }
    if (!have) { chat_say("Координаты: не удалось прочитать (подробности в konstt.log)"); dlog("/tpl pos: ни Transform.position, ни пакет синхронизации не прочитались"); return; }
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Координаты: "); tpl_b_xyz(&b, p); b_s(&b, "  — вернуться сюда: /tpl to "); tpl_b_xyz(&b, p);
    chat_say(msg);
    dlog2("/tpl pos: ", msg);
    if (haveS && (tpl_fabs(sp[0] - p[0]) > 0.5f || tpl_fabs(sp[1] - p[1]) > 0.5f || tpl_fabs(sp[2] - p[2]) > 0.5f)) {
        b_init(&b, msg, sizeof msg);
        b_s(&b, "По пакету синхронизации (как видит сервер): "); tpl_b_xyz(&b, sp);
        chat_say(msg);
        dlog2("/tpl pos: ", msg);
    }
}
/* «/tpl to X Y Z» (nc = 3) или «/tpl to X Z» (nc = 2, высота по земле) */
static void tpl_goto(const float* v, int nc) {
    TplCtx c; float eb[3], mb[3], ne[3], nm[3]; int haveE, haveM, via, i; Buf b; char msg[240];
    for (i = 0; i < nc; i++) if (!(v[i] == v[i]) || v[i] > TPL_MAX || v[i] < -TPL_MAX) { chat_say("Телепорт: координаты вне допустимого диапазона (±100000)"); return; }
    if (!tpl_ctx(&c, 1)) return;
    haveE = c.etr && tpl_vec(c.etr, "get_position", eb);
    haveM = c.mtr && tpl_vec(c.mtr, "get_position", mb);
    if (nc == 3) { ne[0] = v[0]; ne[1] = v[1]; ne[2] = v[2]; }
    else {                                                         /* X Z: высоту берём текущую, а Player.SetPosition ищет землю */
        if (!haveE && !haveM) { chat_say("Телепорт: не удалось прочитать вашу высоту — укажите все три числа: /tpl to X Y Z"); return; }
        ne[0] = v[0]; ne[1] = haveE ? eb[1] : mb[1]; ne[2] = v[1];
    }
    nm[0] = ne[0]; nm[1] = ne[1]; nm[2] = ne[2];
    if (haveE && haveM) { nm[0] += mb[0] - eb[0]; nm[1] += mb[1] - eb[1]; nm[2] += mb[2] - eb[2]; }   /* перевод в систему transform движения */
    via = tpl_place(&c, ne, nm, nc == 2 ? 1 : 0);
    if (!via) { tpl_fail(); return; }
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Телепорт на "); tpl_b_xyz(&b, ne);
    if (nc == 2) b_s(&b, " (высота по земле)");
    if (haveE) {
        double dx = ne[0] - eb[0], dy = ne[1] - eb[1], dz = ne[2] - eb[2];
        b_s(&b, ", это в "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 1); b_s(&b, " м от вас");
    }
    if (via == 2) b_s(&b, " [через движение]");
    chat_say(msg);
    dlog2("/tpl: ", msg);
}

/* ───────────── разбор аргументов ───────────── */
typedef struct { int side, sideSet, alt, status, help, numSet, bad, clamped, mode, nc; float dist; float c[3]; } TplArgs;   /* mode: 0 — сдвиг, 1 — pos, 2 — to X Y Z */

/* UTF-8 в нижний регистр для кириллицы (ASCII свёртывает вызывающий): А–П → а–п, Р–Я → р–я, Ё → ё */
static void tpl_fold(char* s) {
    for (; *s; s++) {
        u8 c = (u8)*s;
        if (c == 0xD0 && s[1]) {
            u8 d = (u8)s[1];
            if (d >= 0x90 && d <= 0x9F) s[1] = (char)(d + 0x20);
            else if (d >= 0xA0 && d <= 0xAF) { s[0] = (char)0xD1; s[1] = (char)(d - 0x20); }
            else if (d == 0x81) { s[0] = (char)0xD1; s[1] = (char)0x91; }
            s++;
        } else if (c >= 'A' && c <= 'Z') *s = (char)(c + 32);
        else if (c >= 0x80 && s[1]) s++;                 /* прочие двухбайтные символы пропускаем целиком */
    }
}
/* слово → сторона 0..3; 10 alt; 11 status; 12 help; 13 pos; 14 to; -1 не знаем */
static int tpl_word(const char* t) {
    if (seq(t, "l") || seq(t, "left") || seq(t, "влево") || seq(t, "лево") || seq(t, "л")) return TPL_L;
    if (seq(t, "r") || seq(t, "right") || seq(t, "вправо") || seq(t, "право") || seq(t, "п")) return TPL_R;
    if (seq(t, "f") || seq(t, "fw") || seq(t, "fwd") || seq(t, "forward") || seq(t, "front") ||
        seq(t, "вперёд") || seq(t, "вперед") || seq(t, "впер") || seq(t, "в")) return TPL_F;
    if (seq(t, "b") || seq(t, "back") || seq(t, "backward") || seq(t, "назад") || seq(t, "н")) return TPL_B;
    if (seq(t, "alt")) return 10;
    if (seq(t, "status") || seq(t, "diag")) return 11;
    if (seq(t, "help") || seq(t, "?")) return 12;
    if (seq(t, "pos") || seq(t, "coords") || seq(t, "coord") || seq(t, "где") || seq(t, "коорд") || seq(t, "координаты")) return 13;
    if (seq(t, "to") || seq(t, "goto") || seq(t, "на") || seq(t, "xyz")) return 14;
    return -1;
}
/* вся строка — число («2», «2.5», «2,5»)? */
static int tpl_num(const char* t, float* out) {
    float v = 0.0f, f = 0.1f; int k = 0, dot = 0, digits = 0;
    for (; t[k]; k++) {
        if (t[k] >= '0' && t[k] <= '9') {
            digits++;
            if (!dot) v = v * 10.0f + (float)(t[k] - '0');
            else { v += f * (float)(t[k] - '0'); f *= 0.1f; }
        } else if ((t[k] == '.' || t[k] == ',') && !dot) dot = 1;
        else return 0;
    }
    if (!digits) return 0;
    *out = v;
    return 1;
}
/* число со знаком («-340», «+5», «12.5») — для координат */
static int tpl_snum(const char* t, float* out) {
    int neg = 0; float v;
    if (*t == '-') { neg = 1; t++; } else if (*t == '+') t++;
    if (!tpl_num(t, &v)) return 0;
    *out = neg ? -v : v;
    return 1;
}
static void tpl_apply_word(int w, TplArgs* a) {
    if (w >= 0 && w <= 3) { a->side = w; a->sideSet = 1; }
    else if (w == 10) a->alt = 1;
    else if (w == 11) a->status = 1;
    else if (w == 12) a->help = 1;
    else if (w == 13) a->mode = 1;
    else if (w == 14) a->mode = 2;
}
static void tpl_token(const char* tok, TplArgs* a) {
    float v; int w, k;
    if (!tok[0] || tok[0] == '/' || seq(tok, "tpl")) return;       /* сама команда может входить в массив */
    if (a->mode == 2 && tpl_snum(tok, &v)) {                       /* после «to» числа — это координаты (с минусом можно) */
        if (a->nc < 3) a->c[a->nc++] = v; else a->bad = 1;
        return;
    }
    if (tpl_num(tok, &v)) { if (!a->numSet) { a->dist = v; a->numSet = 1; } return; }
    w = tpl_word(tok);
    if (w >= 0) { tpl_apply_word(w, a); return; }
    for (k = 1; tok[k]; k++) {                                     /* слитно: «вправо4», «f3», «left2.5» */
        if (tok[k] >= '0' && tok[k] <= '9') {
            char pre[24];
            if (k >= (int)sizeof pre) break;
            { int i; for (i = 0; i < k; i++) pre[i] = tok[i]; pre[k] = 0; }
            w = tpl_word(pre);
            if (w >= 0 && w <= 3 && tpl_num(tok + k, &v)) {
                tpl_apply_word(w, a);
                if (!a->numSet) { a->dist = v; a->numSet = 1; }
                return;
            }
            break;
        }
    }
    a->bad = 1;
}
/* аргументы: string[] */
static void tpl_parse(void* arr, TplArgs* a) {
    long len; int i; char* data;
    a->side = TPL_L; a->sideSet = a->alt = a->status = a->help = a->numSet = a->bad = a->clamped = a->mode = a->nc = 0; a->dist = TPL_DEF;
    a->c[0] = a->c[1] = a->c[2] = 0.0f;
    if (!arr) return;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[96]; int n, p = 0; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 40 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        tpl_fold(tmp);
        while (tmp[p]) {
            char tok[32]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 31) tok[q++] = tmp[p]; p++; }
            tok[q] = 0;
            tpl_token(tok, a);
        }
    }
    if (a->dist < TPL_MIN) a->dist = TPL_MIN;
    if (a->dist > TPL_MAX) { a->dist = TPL_MAX; a->clamped = 1; }
}

/* ───────────── команда ───────────── */
static void tpl_usage(void) {
    chat_usage("Используйте: /tpl [влево|вправо|вперёд|назад] [метры] — например «/tpl назад 2», «/tpl вправо 3», «/tpl 4» (по умолчанию влево 1.5 м)");
    chat_usage("/tpl pos — мои координаты;  /tpl to X Y Z — телепорт в точку (или /tpl to X Z — высота по земле);  /tpl alt — другой способ");
}
/* Вызывается игрой в главном потоке при вводе /tpl [аргументы] */
static void native_tpl_cmd(void* self, void* args, void* mi) {
    TplArgs a;
    (void)self; (void)mi;
    tpl_parse(args, &a);
    if (a.bad || a.help) { tpl_usage(); return; }
    if (a.alt) {
        g_tpl_alt = !g_tpl_alt;
        chat_say(g_tpl_alt ? "Телепорт: способ — через движение персонажа (запасной)" : "Телепорт: способ — Player.SetPosition (основной)");
        if (!a.status && !a.numSet && !a.sideSet && !a.mode) return;  /* «/tpl alt» без стороны, числа и pos/to только переключает, без прыжка */
    }
    if (a.status) {
        chat_say(g_tpl_alt ? "Телепорт: сейчас способ «через движение» (/tpl alt — вернуть основной)" : "Телепорт: сейчас основной способ Player.SetPosition (/tpl alt — запасной)");
        return;
    }
    if (a.mode == 1) { tpl_show(); return; }
    if (a.mode == 2) {
        if (a.nc != 2 && a.nc != 3) { chat_usage("Нужно 3 числа: /tpl to X Y Z (или 2 числа X Z — высота по земле). Числа с минусом можно, например /tpl to 120.5 8 -340"); return; }
        tpl_goto(a.c, a.nc);
        return;
    }
    { char lg[80]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "/tpl: вызвана, сторона "); b_s(&x, g_tpl_sideName[a.side]); b_s(&x, ", метров "); b_fix(&x, a.dist, 1); dlog(lg); }
    tpl_go(a.side, a.dist, a.clamped);
}

/* ───────────── самозапуск ───────────── */
static void* tpl_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("tpl", native_tpl_cmd)) return NULL;     /* konstt_off — мод выключен; иначе ждём игру и регистрируем /tpl */
    hm_mod_ready(g_tpl_modId);
    dlog("tpl: мод загружен, команда /tpl [сторона] [метры] | pos | to X Y Z");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void tpl_ctor(void) {
    unsigned long t = 0;
    g_tpl_modId = hm_mod_register("tpl");
    pthread_create(&t, NULL, tpl_main, NULL);
}
#endif
