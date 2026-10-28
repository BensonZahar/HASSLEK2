/*
 * hassle_tpl.c — необязательный модуль «/tpl»: маленький телепорт ВЛЕВО (выбраться, когда NPC/игрок заглючил и /nocol не помог).
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_tpl.c и пересоберите — больше ничего править не нужно.
 *
 * Команда в чате:
 *   /tpl            сдвиг влево на 1.5 м
 *   /tpl 3          сдвиг влево на 3 м (можно «2.5» или «2,5»; допустимо от 0.3 до 5 м)
 *   /tpl alt        переключить способ телепорта (запасной — через движение персонажа); если /tpl «не двигает», попробуйте его
 *   /tpl status     какой способ сейчас выбран
 *
 * Как это работает
 *   • «Влево» — относительно КАМЕРЫ (как вы видите экран): вектор Transform.right у MainCamera, обнулённая высота, нормализация, знак минус.
 *     Если камеры нет — берём Transform.right самого игрока.
 *   • Позиция — Transform.position (у игрока — Entity.Transform; у запасного способа — transform компонента движения), + сдвиг по горизонтали, высота не меняется.
 *   • Основной способ — Player.SetPosition(Vector3, findHeight=false): тот же метод, которым игра сама переставляет игрока (по приказу сервера).
 *     Запасной — ICharacterMovement.SetPosition(Vector3, updateGround=true): прямая постановка капсулы. Если основной бросил исключение — пробуем запасной сами.
 *   • Обработчик команды вызывается игрой в ГЛАВНОМ потоке (как у /rec и /nocol), поэтому Post/SynchronizationContext не нужны.
 *   • В машине не работает (телепорт водителя/пассажира сломал бы синхронизацию). Максимум 5 м за раз — сервер может не любить большие прыжки.
 *   • Это не «клиентская» штука, как /nocol: позиция уходит на сервер как обычная, и другие игроки вас видят на новом месте.
 */
#pragma once
#include "hassle_core.c"

#define TPL_DEF 1.5f
#define TPL_MIN 0.3f
#define TPL_MAX 5.0f

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
/* «Влево» на плоскости XZ: единичный вектор. 1 — получилось; *src: 1 — по камере, 2 — по самому игроку */
static int tpl_left_dir(void* entityTr, float* dir, int* src) {
    float r[3]; float len; void* ct = tpl_cam_transform();
    *src = 1;
    if (!ct || !tpl_vec(ct, "get_right", r)) {
        *src = 2;
        if (!entityTr || !tpl_vec(entityTr, "get_right", r)) return 0;
    }
    len = (float)__builtin_sqrt((double)r[0] * r[0] + (double)r[2] * r[2]);
    if (len < 1e-3f) return 0;                           /* камера смотрит ровно вверх/вниз — «право» вырождено */
    dir[0] = -r[0] / len; dir[1] = 0.0f; dir[2] = -r[2] / len;
    return 1;
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

/* ───────────── сам телепорт (главный поток) ───────────── */
static void tpl_go(float dist) {
    void *me, *mv = NULL, *etr, *mtr = NULL; float dir[3], base[3], np[3]; int src = 0, ok = 0, via = 0; Buf b; char msg[200];
    if (!g_safe || !il2cpp_light()) { chat_say("Телепорт: игра ещё грузится, подождите"); return; }
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) { chat_say("Телепорт: вы не в игре (персонаж не создан)"); return; }
    if (ref_field_s(me, "Vehicle")) { chat_say("Телепорт: в машине не работает — сначала выйдите"); return; }
    etr = tpl_obj(me, "get_Transform");                  /* Entity.Transform */
    if (!etr) etr = tpl_obj(me, "get_transform");        /* запасной вариант: Component.transform */
    mv = tpl_movement(me);
    if (mv && hm_obj_ok(mv)) mtr = tpl_obj(mv, "get_transform");
    if (!tpl_left_dir(etr ? etr : mtr, dir, &src)) { chat_say("Телепорт: не удалось определить, где «влево» (нет камеры и трансформа игрока)"); return; }

    /* способ 1 (основной): Player.SetPosition, позиция — Entity.Transform */
    if (!g_tpl_alt && etr && tpl_vec(etr, "get_position", base)) {
        np[0] = base[0] + dir[0] * dist; np[1] = base[1]; np[2] = base[2] + dir[2] * dist;
        if (tpl_call_set(me, np, 0, "Player")) { ok = 1; via = 1; }
        else dlog("/tpl: основной способ не вышел, пробую запасной (через движение)");
    }
    /* способ 2 (запасной): ICharacterMovement.SetPosition, позиция — transform компонента движения */
    if (!ok && mv && mtr && tpl_vec(mtr, "get_position", base)) {
        np[0] = base[0] + dir[0] * dist; np[1] = base[1]; np[2] = base[2] + dir[2] * dist;
        if (tpl_call_set(mv, np, 1, "движению")) { ok = 1; via = 2; }
    }
    b_init(&b, msg, sizeof msg);
    if (!ok) {
        b_s(&b, "Телепорт: не вышло — подробности в konstt.log. Попробуйте /tpl alt");
        chat_say(msg); return;
    }
    b_s(&b, "Телепорт влево на "); b_fix(&b, dist, 1); b_s(&b, " м");
    b_s(&b, src == 1 ? " (по камере)" : " (по повороту персонажа, камера не найдена)");
    if (via == 2) b_s(&b, " [через движение]");
    chat_say(msg);
    {
        char lg[200]; Buf x; b_init(&x, lg, sizeof lg);
        b_s(&x, "/tpl: готово, метров "); b_fix(&x, dist, 1); b_s(&x, ", способ "); b_int(&x, via); b_s(&x, ", направление ");
        b_fix(&x, dir[0], 2); b_s(&x, ", "); b_fix(&x, dir[2], 2);
        dlog(lg);
    }
}

/* ───────────── команда ───────────── */
/* аргументы: string[] (сама команда может входить в массив — её пропускаем). Число метров — первое число; слова alt / status. */
static float tpl_parse(void* arr, int* flags) {
    long len; int i; float dist = TPL_DEF; char* data; int gotNum = 0;
    *flags = 0;
    if (!arr) return dist;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return dist;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[64]; int n, p = 0; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 40 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        while (tmp[p]) {
            char tok[24]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 23) { char c = tmp[p]; tok[q++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } p++; }
            tok[q] = 0;
            if (!q) continue;
            if (seq(tok, "alt")) *flags |= 1;
            else if (seq(tok, "status") || seq(tok, "diag")) *flags |= 2;
            else if (!gotNum && tok[0] >= '0' && tok[0] <= '9') {     /* «2», «2.5», «2,5» */
                float v = 0.0f, f = 0.1f; int k = 0, seenDot = 0;
                for (; tok[k]; k++) {
                    if (tok[k] >= '0' && tok[k] <= '9') {
                        if (!seenDot) v = v * 10.0f + (float)(tok[k] - '0');
                        else { v += f * (float)(tok[k] - '0'); f *= 0.1f; }
                    } else if ((tok[k] == '.' || tok[k] == ',') && !seenDot) seenDot = 1;
                    else break;
                }
                dist = v; gotNum = 1; *flags |= 4;
            }
        }
    }
    if (dist < TPL_MIN) dist = TPL_MIN;
    if (dist > TPL_MAX) dist = TPL_MAX;
    return dist;
}

/* Вызывается игрой в главном потоке при вводе /tpl [аргументы] */
static void native_tpl_cmd(void* self, void* args, void* mi) {
    int fl; float dist;
    (void)self; (void)mi;
    dist = tpl_parse(args, &fl);
    if (fl & 1) {
        g_tpl_alt = !g_tpl_alt;
        chat_say(g_tpl_alt ? "Телепорт: способ — через движение персонажа (запасной)" : "Телепорт: способ — Player.SetPosition (основной)");
        if (!(fl & 2) && !(fl & 4)) return;               /* «/tpl alt» без числа только переключает, без прыжка */
    }
    if (fl & 2) {
        chat_say(g_tpl_alt ? "Телепорт: сейчас способ «через движение» (/tpl alt — вернуть основной)" : "Телепорт: сейчас основной способ Player.SetPosition (/tpl alt — запасной)");
        return;
    }
    dlog_i("/tpl: вызвана, флаги ", fl);
    tpl_go(dist);
}

/* ───────────── самозапуск ───────────── */
static void* tpl_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("tpl", native_tpl_cmd)) return NULL;     /* konstt_off — мод выключен; иначе ждём игру и регистрируем /tpl */
    hm_mod_ready(g_tpl_modId);
    dlog("tpl: мод загружен, команда /tpl [метры] — телепорт влево");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void tpl_ctor(void) {
    unsigned long t = 0;
    g_tpl_modId = hm_mod_register("tpl");
    pthread_create(&t, NULL, tpl_main, NULL);
}
#endif
