/*
 * hassle_route.c — необязательный модуль «/route»: записать свой маршрут пешком и повторить его так же.
 *
 * Зависит только от ядра (hassle_core.c): включает его (#include "hassle_core.c"), сам запускает свой поток и сам регистрирует команду.
 * Мод Near, /rec и /nocol для /route не нужны: пул игроков, реестр чат-команд и «главный поток по заказу» (mt_capture_ctx) — в ядре.
 * КАК УБРАТЬ: удалите hassle_route.c и пересоберите .so — больше ничего править не нужно.
 *
 * Команда в чате (нативная, как /rec и /nocol):
 *   /route rec            начать запись; запись стартует, когда вы возьмётесь за левый стик
 *   /route stop           остановить запись (или остановить повтор); запись сохраняется в HLOG/route.txt
 *   /route play           повторить записанный маршрут (без записи в памяти — берёт HLOG/route.txt)
 *   /route play force     то же, но без проверки «вы стоите там же, где начинали запись»
 *   /route play nocam     повтор без выставления камеры (камеру держите сами)
 *   /route play 80        то же, но опережение задать вручную, мс (0 — как раньше, без опережения; по умолчанию авто = длина кадра)
 *   /route status         состояние
 *   /route dress          РАСКЛАДКА «раздевалка» (отдельная запись HLOG/route_dress.txt, обычный маршрут не трогает):
 *                           1) /route dress  — начать запись (пойдёт, когда вы возьмётесь за левый стик), идёте к метке раздевалки;
 *                           2) у метки /route dress (или /route stop) — путь сохранён; переодеваетесь;
 *                           3) /route dress  — персонаж идёт ОБРАТНО по тому же пути и встаёт там, где вы начинали запись.
 *                         /route dress rec    всегда новая запись (если что-то пошло не так);
 *                         /route dress back   вернуться по записи из файла (после перезапуска игры или если /route dress не вернул);
 *                         /route dress back force   то же, но без проверки «вы стоите у метки»;
 *                         Дожим: когда возврат по записи кончился, мод ждёт остановки персонажа (инерция) и сам подтягивает его к точке старта
 *                         записи (до 5 с, не дальше 4 м; взялись за стик — уступает). Ход пишется в konstt.log («раздевалка: дожим …»), итог — в чат.
 *                         то же по-русски: /route раздевалка
 *   /route probe          найти экранный стик и показать его значение (диагностика)
 *
 * Как это устроено (по дампу игры и по Hud.js):
 *   • Экранный стик рисует Vue-компонент в HUD. Он НЕ связан с VirtualControlsManager (тот, что нашёл прошлый вариант мода: ключи
 *     Horizontal/Vertical/Joystick, m_Pressed всегда 0). Компонент зовёт window.onScreenControlTouchStart/Move/End("<Gamepad>/leftStick", x, y),
 *     это Hassle.WebView.Controls.OnScreenControls → OnWebViewScreenStick (OnScreenControl из Unity Input System), который пишет
 *     значение в виртуальный <Gamepad>/leftStick. Игра читает движение уже оттуда.
 *   • Объект стика лежит в статическом словаре OnScreenControls.m_Controls (ключ "<Gamepad>/leftStick") и существует, пока HUD на экране
 *     (плавающий круг рисуется только при касании, а сам стик есть всегда).
 *   • Запись: КАЖДЫЙ КАДР (в главном потоке) читаем значение стика InputControl<Vector2>.ReadUnprocessedValue() и пишем, когда оно изменилось.
 *   • Повтор: каждый кадр зовём OnWebViewScreenStick.OnTouchStart/OnTouchMove(Vector2)/OnTouchEnd — ровно то, что делает палец через JS.
 *   • Все вызовы IL2CPP идут ТОЛЬКО из главного потока. Фоновый поток лишь раз в ~12 мс ставит в очередь SynchronizationContext.Post
 *     «тик» (механизм из /nocol), а время считает по монотонным часам, так что пропущенные кадры ничего не ломают.
 *   • Камера: Game.MainCamera.m_Aim (CinemachinePOV) → m_HorizontalAxis.Value (поворот, градусы) и m_VerticalAxis.Value (наклон).
 *     Пишется вместе со стиком; при повторе камера выставляется сначала (пауза ~0,25 с), потом повторяются и её повороты.
 *   • Спринт: Hud.js включает его САМ, когда стик оттянут вверх дальше 1.3 (значения стика доходят до ~3.6): зовёт onScreenControlTouchStart("<Keyboard>/Space").
 *     Эту кнопку нужно тоже повторять, иначе при повторе персонаж идёт шагом и приходит не туда. Поэтому мод пишет и повторяет ещё и
 *     состояние кнопок <Keyboard>/Space, <Keyboard>/leftShift, <Keyboard>/c (читает их значение так же, как стик). Старая запись без кнопок
 *     (в файле нет строки «B 1») повторяется со спринтом по правилу «стик вверх > 1.3 → Space».
 *   • Если во время повтора вы возьмётесь за стик сами, повтор прерывается (значение стика не совпадает с тем, что писал мод).
 *
 * Ограничения: повторяются стик, камера и кнопки Space/leftShift/c. Положение игрока перед /route play проверяется
 * по точке начала записи (не дальше 2 м). Во время повтора камера занята модом (/route play nocam — чтобы камеру держали вы).
 * Точность: игра идёт по кадрам, на длинном маршруте возможен небольшой снос; по окончании мод пишет, на сколько метров вы не дошли до конца записи.
 */
#pragma once
#include "hassle_core.c"

struct rt_ts { long sec; long nsec; };
extern int clock_gettime(int, void*);

#define RT_MAX 30000                 /* точек записи (на ~8 минут непрерывной ходьбы) */
#define RT_MAXJ 8
#define RT_FBUF (3 << 20)             /* 3 МБ: хватает на заполненную запись вместе с дорожкой позиций */
#define RT_MAXP 3000                 /* точек дорожки позиций (каждые 250 мс, 12 минут) */
#define RT_PSTEP 250                 /* шаг дорожки позиций, мс */
#define RT_MAXMS 600000              /* запись/повтор не длиннее 10 минут */

/* ───────────── дополнительные функции IL2CPP ───────────── */
static struct { void* (*class_get_type)(void*); void* (*type_get_object)(void*); } RA;
static int g_rt_apiTried;
static int rt_api(void) {
    if (!g_rt_apiTried) {
        g_rt_apiTried = 1;
        if (g_il2cpp) {
            *(void**)&RA.class_get_type = dlsym(g_il2cpp, "il2cpp_class_get_type");
            *(void**)&RA.type_get_object = dlsym(g_il2cpp, "il2cpp_type_get_object");
        }
    }
    return RA.class_get_type && RA.type_get_object;
}

/* ───────────── данные ───────────── */
typedef struct { unsigned t; float x, y; unsigned p; float c, v; unsigned b; } RtS;   /* t — мс от начала, p — стик задет (x или y не 0), x/y — значение стика, c/v — поворот/наклон камеры, b — маска нажатых кнопок */
static RtS g_rt_s[RT_MAX];
static volatile int g_rt_n;
static volatile int g_rt_mode;       /* 0 — ничего, 1 — запись, 2 — повтор */
static volatile int g_rt_tickPending;
static volatile long long g_rt_tickT;
static int g_rt_idx = -1;
static int g_rt_rev;                                               /* повтор задом наперёд (возврат из раздевалки): запись идёт с конца, камера +180° */
static int g_rt_dress;                                             /* текущая запись/повтор — режим «раздевалка» */
static int g_rt_dst;                                               /* 1 — путь к раздевалке записан и вы стоите «у метки»: следующий /route dress вернёт назад */
static int g_rt_src;                                               /* что лежит в памяти: 0 — ничего, 1 — обычный маршрут (route.txt), 2 — раздевалка (route_dress.txt) */
static long long g_rt_revT0;                                       /* с какого момента записи начинается возврат, мс (конец записи или середина, если возвращаемся с пути) */
static float g_rt_camMin = -180.0f, g_rt_camMax = 180.0f;          /* диапазон поворота камеры (AxisState.m_MinValue/m_MaxValue) */
static int g_rt_camWrap = 1;
static char g_rt_hn[64], g_rt_vn[64];                              /* в файле остались для совместимости формата */
static int g_rt_hasS, g_rt_hasE;
static float g_rt_S[3], g_rt_E[3];

#define RT_STICK "<Gamepad>/leftStick"
static void* g_rt_st;  static void* g_rt_stk;                      /* OnWebViewScreenStick и его заголовок (проверка перед вызовом) */
static void* g_rt_ctrl; static void* g_rt_ctrlk;                   /* InputControl этого стика (читаем значение) */
static void *g_rt_mRead, *g_rt_mTS, *g_rt_mTM, *g_rt_mTE;          /* ReadUnprocessedValue/ReadValue, OnTouchStart, OnTouchMove(Vector2), OnTouchEnd */
static char g_rt_key[64];
#define RT_NB 3
static const char* const g_rt_bpath[RT_NB] = { "<Keyboard>/Space", "<Keyboard>/leftShift", "<Keyboard>/c" };   /* бит 0 — Space (спринт/прыжок), бит 1 — leftShift, бит 2 — c (присесть) */
static void *g_rt_bo[RT_NB], *g_rt_bok[RT_NB];                     /* OnWebViewScreenButton и его заголовок */
static void *g_rt_bc[RT_NB], *g_rt_bck[RT_NB];                     /* InputControl кнопки и его заголовок */
static void *g_rt_mBS[RT_NB], *g_rt_mBE[RT_NB], *g_rt_mBR[RT_NB]; /* OnTouchStart, OnTouchEnd, ReadValue */
static int g_rt_btnRec;                                            /* в записи есть кнопки */
static unsigned g_rt_bdown, g_rt_lb;                               /* какие кнопки сейчас держит мод (повтор) / последние записанные (запись) */
static int g_rt_started;                                           /* в записи: за стик уже взялись */
static long long g_rt_t0;
/* Опережение повтора. Запись читает стик в кадре k и это значение двигало персонажа за интервал ДО этого кадра (значение уже было обработано
   Input System). Повтор пишет значение в кадре k, а в движение оно попадает только в СЛЕДУЮЩЕМ кадре, поэтому повтор всегда отстаёт примерно
   на кадр (в эмуляторе кадр ~50-60 мс, при 4-5 м/с это 0.25 м и больше). Лечится выбором записанного значения не на «сейчас», а на «сейчас + кадр». */
static long long g_rt_lastTk;                                      /* когда был предыдущий тик повтора */
static float g_rt_dtEma = 55.0f;                                   /* средний интервал между тиками (≈ длительность кадра), мс */
static float g_rt_lead;                                            /* опережение, применённое в последнем тике, мс */
static int g_rt_leadArg = -1;                                      /* /route play <мс> — задать опережение вручную; -1 — авто (= длина кадра) */
static int g_rt_pi;
static int g_rt_wrote, g_rt_bad, g_rt_down, g_rt_whi;
static float g_rt_wh[4][2];                                        /* последние записанные значения (стик обновляется с задержкой в кадр) */
static float g_rt_lx, g_rt_ly;
static unsigned g_rt_lp;
static char g_rt_fbuf[RT_FBUF];
/* дорожка позиций: где вы реально были в записи (читается так же, как «ваша позиция» у /near). Нужна, чтобы повтор мог сам сказать, где и насколько ушёл от записи */
typedef struct { unsigned t; float x, y, z; } RtP;
static RtP g_rt_pp[RT_MAXP];
static volatile int g_rt_pn;
static int g_rt_ppi;                                               /* индекс для интерполяции при повторе (время растёт — идём вперёд) */
static long long g_rt_lastP;                                       /* когда писали последнюю точку дорожки (запись) */
static long long g_rt_lastDev, g_rt_lastDevLog;                    /* когда считали/писали в журнал отклонение (повтор) */
static float g_rt_maxDev; static long long g_rt_maxDevT;           /* наибольшее отклонение от записи за повтор, и когда */
static float g_rt_startDev[3];                                     /* на сколько вы стояли не там, где начинали запись (повтор) */
/* дожим после возврата (раздевалка): запись кончилась — мод ждёт, пока персонаж остановится (инерция), и сам подтягивает его к точке старта записи */
#define RT_FIX_MAXD 4.0f             /* дальше этого от точки старта — не дожимаем (что-то пошло не так, не уводим персонажа) */
#define RT_FIX_TOL  0.20f            /* считаем, что дошли, если ближе (м) */
#define RT_FIX_MS   5000             /* весь дожим не дольше (мс) */
#define RT_FIX_LEGS 3                /* не больше столько заходов «пошёл–остановился» */
static int g_rt_fix;                                               /* 1 — идёт дожим */
static long long g_rt_fixT0, g_rt_fixLT, g_rt_fixStall, g_rt_fixLog;   /* старт дожима, предыдущий тик, сколько мс стик держим, а персонаж не движется, когда писали в журнал */
static float g_rt_fixTgt[3], g_rt_fixPrev[3], g_rt_fixPD, g_rt_fixSp, g_rt_fixCl, g_rt_fixD0;   /* цель, прошлая позиция, прошлое расстояние, скорость и сближение (м/с), расстояние после остановки */
static int g_rt_fixHave, g_rt_fixLegs;                            /* сколько замеров есть, сколько заходов «пошёл–остановился» было */
static void* g_rt_mGT; static void* g_rt_mEA;                      /* Component.get_transform, Transform.get_eulerAngles (реальная камера) */
static int rt_my_pos(float* me);
static void b_pos(Buf* b, const float* p);
/* камера */
static void* g_rt_aim; static void* g_rt_aimk;                    /* CinemachinePOV и его заголовок (проверка перед записью) */
static int g_rt_offCH = -1, g_rt_offCV = -1;                      /* абсолютные смещения m_HorizontalAxis.Value / m_VerticalAxis.Value в объекте POV */
static int g_rt_camRec;                                           /* в записи есть камера */
static int g_rt_camOn;                                            /* при повторе выставляем камеру */
static float g_rt_lc, g_rt_lv;                                    /* последние записанные поворот/наклон */

static long long rt_now(void) {
    struct rt_ts t;
    if (clock_gettime(1, &t)) return 0;                          /* CLOCK_MONOTONIC */
    return (long long)t.sec * 1000 + t.nsec / 1000000;
}

/* ───────────── поиск экранного стика (ТОЛЬКО главный поток) ───────────── */
static void rt_str(void* s, char* out, int cap) {
    int n, i; const u16* ch;
    out[0] = 0;
    if (!s || !hm_readable(s, 24)) return;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 60 || !ch) return;
    utf16_to_utf8(ch, n, out, cap);
    for (i = 0; out[i]; i++) if (out[i] == ' ') out[i] = '_';
}

static int rt_stick_ok(void) { return g_rt_st && hm_obj_ok(g_rt_st) && *(void**)g_rt_st == g_rt_stk; }
static int rt_ctrl_ok(void) { return g_rt_ctrl && hm_obj_ok(g_rt_ctrl) && *(void**)g_rt_ctrl == g_rt_ctrlk; }

/* значение стика (главный поток). Результат — упакованный Vector2: два float сразу после заголовка объекта */
static int rt_stick_read(float* x, float* y) {
    void *r, *exc = NULL; const float* f;
    if (!g_rt_mRead || !rt_ctrl_ok()) return 0;
    r = A.runtime_invoke(g_rt_mRead, g_rt_ctrl, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 8)) return 0;
    f = (const float*)((char*)r + HDR);
    *x = f[0]; *y = f[1];
    return 1;
}
/* what: 1 — OnTouchStart, 2 — OnTouchMove(x, y), 3 — OnTouchEnd */
static int rt_stick_send(int what, float x, float y) {
    void *a[1], *exc = NULL; float v[2]; void* m = what == 1 ? g_rt_mTS : what == 2 ? g_rt_mTM : g_rt_mTE;
    if (!m || !rt_stick_ok()) return 0;
    v[0] = x; v[1] = y; a[0] = v;                                /* Vector2 передаётся указателем на два float */
    A.runtime_invoke(m, g_rt_st, what == 2 ? a : NULL, &exc);
    return exc ? 0 : 1;
}

/* кнопка i (главный поток): down = нажата ли; 0 — не читается */
static int rt_btn_read(int i, int* down) {
    void *r, *exc = NULL;
    if (!g_rt_mBR[i] || !g_rt_bc[i] || !hm_obj_ok(g_rt_bc[i]) || *(void**)g_rt_bc[i] != g_rt_bck[i]) return 0;
    r = A.runtime_invoke(g_rt_mBR[i], g_rt_bc[i], NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 4)) return 0;
    *down = *(const float*)((char*)r + HDR) > 0.5f;
    return 1;
}
static unsigned rt_btn_mask(void) {
    unsigned m = 0; int i, d;
    for (i = 0; i < RT_NB; i++) if (rt_btn_read(i, &d) && d) m |= 1u << i;
    return m;
}
static int rt_btn_send(int i, int start) {
    void *exc = NULL; void* m = start ? g_rt_mBS[i] : g_rt_mBE[i];
    if (!m || !g_rt_bo[i] || !hm_obj_ok(g_rt_bo[i]) || *(void**)g_rt_bo[i] != g_rt_bok[i]) return 0;
    A.runtime_invoke(m, g_rt_bo[i], NULL, &exc);
    return exc ? 0 : 1;
}
static void rt_btn_apply(unsigned want) {                          /* привести кнопки, которые держит мод, к маске want */
    int i;
    for (i = 0; i < RT_NB; i++) {
        unsigned bit = 1u << i;
        if (((want ^ g_rt_bdown) & bit) == 0) continue;
        if (rt_btn_send(i, (want & bit) != 0)) g_rt_bdown = (want & bit) ? (g_rt_bdown | bit) : (g_rt_bdown & ~bit);
    }
}

static int rt_find(void) {
    void *kc, *fd, *dict = NULL, *dk, *fe, *fc, *ent, *best = NULL; long cnt, i; int offE, offC, stride;
    char bestKey[64]; const char* bestCls = NULL;
    g_rt_st = g_rt_ctrl = g_rt_mRead = g_rt_mTS = g_rt_mTM = g_rt_mTE = NULL; g_rt_key[0] = 0; bestKey[0] = 0;
    { int q; for (q = 0; q < RT_NB; q++) { g_rt_bo[q] = g_rt_bc[q] = g_rt_mBS[q] = g_rt_mBE[q] = g_rt_mBR[q] = NULL; } }
    if (!g_img) { dlog("маршрут: нет образа игры"); return 0; }
    kc = A.class_from_name(g_img, "Hassle.WebView.Controls", "OnScreenControls");
    if (!kc) { dlog("маршрут: класс OnScreenControls не найден"); return 0; }
    fd = A.class_get_field_from_name(kc, "m_Controls");
    if (!fd) { dlog("маршрут: у OnScreenControls нет поля m_Controls"); return 0; }
    A.field_static_get_value(fd, &dict);
    if (!dict || !hm_obj_ok(dict)) { dlog("маршрут: OnScreenControls.m_Controls = null (HUD с экранным управлением не создан?)"); return 0; }
    dk = A.object_get_class(dict);
    fe = find_field(dk, "_entries"); fc = find_field(dk, "_count");
    if (!fe || !fc) { dlog("маршрут: у словаря нет полей _entries/_count"); return 0; }
    offE = A.field_get_offset(fe); offC = A.field_get_offset(fc);
    if (offE < 0x10 || offE > 0x100 || offC < 0x10 || offC > 0x100 || !hm_readable((char*)dict + offE, sizeof(void*)) || !hm_readable((char*)dict + offC, 4)) { dlog("маршрут: странные смещения словаря"); return 0; }
    ent = *(void**)((char*)dict + offE); cnt = *(int*)((char*)dict + offC);
    stride = 8 + 2 * (int)sizeof(void*);                           /* Entry<string, ref>: int hashCode; int next; key; value */
    if (!ent || cnt < 0 || cnt > 256) { dlog_i("маршрут: словарь экранных элементов странный, count=", cnt); return 0; }
    if (cnt == 0 || !hm_range_ok(ent, 4 * (long)sizeof(void*) + cnt * stride)) { dlog("маршрут: экранных элементов нет (HUD ещё не создал стик)"); return 0; }
    for (i = 0; i < cnt; i++) {
        char* e = (char*)ent + 4 * sizeof(void*) + (uptr)i * (uptr)stride;
        void* key = *(void**)(e + 8);
        void* val = *(void**)(e + 8 + sizeof(void*));
        char k[64]; const char* cn;
        if (!val || !hm_obj_ok(val)) continue;
        rt_str(key, k, sizeof k);
        cn = A.class_get_name(A.object_get_class(val));
        dlog2("маршрут: экранный элемент: ", k);
        dlog2("маршрут:    класс: ", cn ? cn : "?");
        if (cn && seq(cn, "OnWebViewScreenButton")) {
            int q;
            for (q = 0; q < RT_NB; q++) if (seq(k, g_rt_bpath[q])) { g_rt_bo[q] = val; g_rt_bok[q] = *(void**)val; }
        }
        if (cn && seq(cn, "OnWebViewScreenStick") && (!best || seq(k, RT_STICK))) {
            best = val; bestCls = cn;
            { int q; for (q = 0; q < 63 && k[q]; q++) bestKey[q] = k[q]; bestKey[q] = 0; }
        }
    }
    if (!best) { dlog("маршрут: среди экранных элементов нет OnWebViewScreenStick"); return 0; }
    (void)bestCls;
    {
        void* kcls = A.object_get_class(best); void *ctrl, *m1, *m2; float tx, ty;
        g_rt_mTS = find_method(kcls, "OnTouchStart", 0);
        g_rt_mTM = find_method(kcls, "OnTouchMove", 1);
        g_rt_mTE = find_method(kcls, "OnTouchEnd", 0);
        if (!g_rt_mTS || !g_rt_mTM || !g_rt_mTE) { dlog("маршрут: у стика нет OnTouchStart/OnTouchMove/OnTouchEnd"); return 0; }
        ctrl = ref_field_s(best, "m_Control");
        if (!ctrl || !hm_obj_ok(ctrl)) {                           /* поле ещё пусто — спросим свойством control */
            void *gm = find_method(kcls, "get_control", 0), *exc = NULL;
            ctrl = gm ? A.runtime_invoke(gm, best, NULL, &exc) : NULL;
            if (exc) ctrl = NULL;
        }
        if (!ctrl || !hm_obj_ok(ctrl)) { dlog("маршрут: у стика нет InputControl (виртуальный геймпад ещё не создан?)"); return 0; }
        g_rt_st = best; g_rt_stk = *(void**)best; g_rt_ctrl = ctrl; g_rt_ctrlk = *(void**)ctrl;
        m1 = find_method(A.object_get_class(ctrl), "ReadUnprocessedValue", 0);
        m2 = find_method(A.object_get_class(ctrl), "ReadValue", 0);
        g_rt_mRead = m1;
        if (!m1 || !rt_stick_read(&tx, &ty)) { g_rt_mRead = m2; if (!m2 || !rt_stick_read(&tx, &ty)) { g_rt_mRead = NULL; g_rt_st = g_rt_ctrl = NULL; dlog("маршрут: значение стика не читается (ReadUnprocessedValue/ReadValue)"); return 0; } }
        { int q; for (q = 0; q < 63 && bestKey[q]; q++) g_rt_key[q] = bestKey[q]; g_rt_key[q] = 0; }
    }
    {   /* кнопки Space/leftShift/c: не обязательны — не нашли, повторяем только стик */
        int q, okn = 0;
        for (q = 0; q < RT_NB; q++) {
            void *bo = g_rt_bo[q], *bk, *ctrl; int dn;
            if (!bo) { dlog2("маршрут: кнопка не найдена: ", g_rt_bpath[q]); continue; }
            bk = A.object_get_class(bo);
            g_rt_mBS[q] = find_method(bk, "OnTouchStart", 0);
            g_rt_mBE[q] = find_method(bk, "OnTouchEnd", 0);
            ctrl = ref_field_s(bo, "m_Control");
            if (!ctrl || !hm_obj_ok(ctrl)) {
                void *gm = find_method(bk, "get_control", 0), *exc = NULL;
                ctrl = gm ? A.runtime_invoke(gm, bo, NULL, &exc) : NULL;
                if (exc) ctrl = NULL;
            }
            if (ctrl && hm_obj_ok(ctrl)) {
                g_rt_bc[q] = ctrl; g_rt_bck[q] = *(void**)ctrl;
                g_rt_mBR[q] = find_method(A.object_get_class(ctrl), "ReadValue", 0);
                if (!rt_btn_read(q, &dn)) { g_rt_mBR[q] = NULL; dlog2("маршрут: значение кнопки не читается: ", g_rt_bpath[q]); }
            }
            if (g_rt_mBS[q] && g_rt_mBE[q]) { okn++; dlog2("маршрут: кнопка найдена: ", g_rt_bpath[q]); }
        }
        dlog_i("маршрут: кнопок найдено ", okn);
    }
    dlog2("маршрут: стик найден, ключ ", g_rt_key);
    return 1;
}

/* камера (ТОЛЬКО главный поток): Game.MainCamera (static) → m_Aim (CinemachinePOV) → AxisState.Value горизонтали и вертикали */
static int rt_cam_find(void) {
    void *fn, *cam = NULL, *aim, *ak, *fh, *fv, *sc, *fval; int sh, vo;
    g_rt_aim = NULL; g_rt_offCH = g_rt_offCV = -1;
    if (!g_cGame || !g_img) { dlog("маршрут: камера: класс Game не найден"); return 0; }
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) { dlog("маршрут: камера: Game.MainCamera не найдено"); return 0; }
    A.field_static_get_value(fn, &cam);
    if (!cam || !hm_obj_ok(cam)) { dlog("маршрут: камера: Game.MainCamera = null"); return 0; }
    aim = ref_field_s(cam, "m_Aim");
    if (!aim || !hm_obj_ok(aim)) { dlog("маршрут: камера: MainCamera.m_Aim пуст (камера не в режиме слежения?)"); return 0; }
    ak = A.object_get_class(aim);
    fh = find_field(ak, "m_HorizontalAxis"); fv = find_field(ak, "m_VerticalAxis");
    if (!fh || !fv) { dlog2("маршрут: камера: нет m_HorizontalAxis/m_VerticalAxis в классе ", A.class_get_name(ak)); return 0; }
    sc = A.class_from_type(A.field_get_type(fh));
    fval = sc ? find_field(sc, "Value") : NULL;
    if (!fval) { dlog("маршрут: камера: у AxisState нет поля Value"); return 0; }
    sh = struct_shift(sc);
    vo = A.field_get_offset(fval) - sh;
    g_rt_offCH = A.field_get_offset(fh) + vo; g_rt_offCV = A.field_get_offset(fv) + vo;
    if (g_rt_offCH < 0x10 || g_rt_offCH > 0x400 || g_rt_offCV < 0x10 || g_rt_offCV > 0x400 || !hm_readable((char*)aim + g_rt_offCH, 4) || !hm_readable((char*)aim + g_rt_offCV, 4)) {
        dlog_i("маршрут: камера: странные смещения ", g_rt_offCH); g_rt_offCH = g_rt_offCV = -1; return 0;
    }
    g_rt_aim = aim; g_rt_aimk = *(void**)aim;
    g_rt_camMin = -180.0f; g_rt_camMax = 180.0f; g_rt_camWrap = 1;   /* по умолчанию — как у стандартного POV Cinemachine; ниже уточняем из игры */
    {
        void *fmn = find_field(sc, "m_MinValue"), *fmx = find_field(sc, "m_MaxValue"), *fwr = find_field(sc, "m_Wrap");
        int base = A.field_get_offset(fh) - sh;                    /* смещение начала AxisState внутри объекта POV */
        if (fmn && fmx) {
            int om = base + A.field_get_offset(fmn), ox = base + A.field_get_offset(fmx);
            if (om > 0x10 && om < 0x400 && ox > 0x10 && ox < 0x400 && hm_readable((char*)aim + om, 4) && hm_readable((char*)aim + ox, 4)) {
                float lo = *(volatile float*)((char*)aim + om), hi = *(volatile float*)((char*)aim + ox);
                if (hi - lo >= 1.0f && lo > -1000.0f && hi < 1000.0f) { g_rt_camMin = lo; g_rt_camMax = hi; }
            }
        }
        if (fwr) {
            int ow = base + A.field_get_offset(fwr);
            if (ow > 0x10 && ow < 0x400 && hm_readable((char*)aim + ow, 1)) g_rt_camWrap = *(volatile unsigned char*)((char*)aim + ow) ? 1 : 0;
        }
    }
    dlog_i("маршрут: камера найдена, смещение поворота ", g_rt_offCH);
    return 1;
}
/* поворот камеры «в противоположную сторону» (для возврата задом наперёд): +180° и обратно в диапазон оси */
static float rt_flip_yaw(float c) {
    float lo = g_rt_camMin, hi = g_rt_camMax, r = hi - lo; int k;
    c += 180.0f;
    if (r < 1.0f) return c;
    for (k = 0; k < 8 && c >= hi; k++) c -= r;
    for (k = 0; k < 8 && c < lo; k++) c += r;
    return c;
}
static int rt_cam_read(float* c, float* v) {
    char* o = (char*)g_rt_aim;
    if (!o || g_rt_offCH < 0 || !hm_readable(o, 16) || *(void**)o != g_rt_aimk || !hm_readable(o + g_rt_offCH, 4) || !hm_readable(o + g_rt_offCV, 4)) return 0;
    *c = *(volatile float*)(o + g_rt_offCH);
    *v = *(volatile float*)(o + g_rt_offCV);
    return 1;
}
static int rt_cam_write(float c, float v) {
    char* o = (char*)g_rt_aim;
    if (!o || g_rt_offCH < 0 || !hm_readable(o, 16) || *(void**)o != g_rt_aimk || !hm_readable(o + g_rt_offCH, 4) || !hm_readable(o + g_rt_offCV, 4)) return 0;
    *(volatile float*)(o + g_rt_offCH) = c;
    *(volatile float*)(o + g_rt_offCV) = v;
    return 1;
}

/* Реальный поворот камеры по её Transform (независимо от CinemachinePOV): по ним проверяем, что значение из m_Aim — настоящее.
   У камеры Cinemachine вращение = Euler(наклон, поворот, 0), поэтому eulerAngles.y должен совпадать с m_HorizontalAxis.Value, а .x — с m_VerticalAxis.Value (по модулю 360). */
static int rt_cam_true(float* yaw, float* pitch) {
    void *fn, *cam = NULL, *co, *tr, *exc = NULL, *r; const float* f;
    if (!g_cGame) return 0;
    fn = A.class_get_field_from_name(g_cGame, "MainCamera");
    if (!fn) return 0;
    A.field_static_get_value(fn, &cam);
    if (!cam || !hm_obj_ok(cam)) return 0;
    co = ref_field_s(cam, "m_Camera");
    if (!co || !hm_obj_ok(co)) return 0;
    if (!g_rt_mGT) g_rt_mGT = find_method(A.object_get_class(co), "get_transform", 0);
    if (!g_rt_mGT) return 0;
    tr = A.runtime_invoke(g_rt_mGT, co, NULL, &exc);
    if (exc || !tr || !hm_obj_ok(tr)) return 0;
    if (!g_rt_mEA) g_rt_mEA = find_method(A.object_get_class(tr), "get_eulerAngles", 0);
    if (!g_rt_mEA) return 0;
    r = A.runtime_invoke(g_rt_mEA, tr, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    *pitch = f[0]; *yaw = f[1];
    return 1;
}
static float rt_adiff(float a, float b) {                          /* разница углов, градусы, 0..180 */
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d < 0 ? -d : d;
}
/* 1 — значение из m_Aim совпадает с реальной камерой; 0 — не совпадает (m_Aim НЕ настоящий поворот); -1 — проверить не удалось */
static int rt_cam_check(void) {
    float c = 0, v = 0, ty = 0, tx = 0; char t[200]; Buf b;
    if (!rt_cam_read(&c, &v)) return -1;
    if (!rt_cam_true(&ty, &tx)) { dlog("маршрут: камера: реальный поворот (Transform) прочитать не удалось, проверка пропущена"); return -1; }
    b_init(&b, t, sizeof t);
    b_s(&b, "маршрут: камера: POV "); b_fix(&b, c, 1); b_ch(&b, '/'); b_fix(&b, v, 1);
    b_s(&b, ", реальная "); b_fix(&b, ty, 1); b_ch(&b, '/'); b_fix(&b, tx, 1);
    dlog(t);
    return (rt_adiff(c, ty) <= 5.0f && rt_adiff(v, tx) <= 5.0f) ? 1 : 0;
}

/* строка в konstt.log о состоянии: время, камера (POV и реальная), позиция */
static void rt_log_state(const char* tag, long long ms) {
    char t[260]; Buf b; float c = 0, v = 0, ty = 0, tx = 0, me[3];
    int hc = g_rt_aim ? rt_cam_read(&c, &v) : 0, ht = rt_cam_true(&ty, &tx);
    b_init(&b, t, sizeof t);
    b_s(&b, "маршрут: "); b_s(&b, tag); b_s(&b, " t="); b_fix(&b, (double)ms / 1000.0, 1); b_s(&b, " с");
    if (hc) { b_s(&b, ", POV "); b_fix(&b, c, 1); b_ch(&b, '/'); b_fix(&b, v, 1); }
    if (ht) { b_s(&b, ", камера "); b_fix(&b, ty, 1); b_ch(&b, '/'); b_fix(&b, tx, 1); }
    if (rt_my_pos(me)) { b_s(&b, ", поз "); b_pos(&b, me); }
    dlog(t);
}
/* дорожка позиций: запись */
static void rt_pos_push(long long ms) {
    float me[3]; int n = g_rt_pn;
    if (n >= RT_MAXP || !rt_my_pos(me)) return;
    g_rt_pp[n].t = (unsigned)ms; g_rt_pp[n].x = me[0]; g_rt_pp[n].y = me[1]; g_rt_pp[n].z = me[2];
    __atomic_store_n(&g_rt_pn, n + 1, __ATOMIC_RELEASE);
    g_rt_lastP = ms;
}
/* позиция по записи в момент ms (линейная интерполяция между точками дорожки) */
static int rt_pos_at(long long ms, float* out) {
    int n = g_rt_pn, i = g_rt_ppi, k;
    if (n < 2) return 0;
    if (i < 0 || i >= n) i = 0;
    while (i > 0 && (long long)g_rt_pp[i].t > ms) i--;                 /* при возврате время идёт назад */
    while (i + 1 < n && (long long)g_rt_pp[i + 1].t <= ms) i++;
    g_rt_ppi = i;
    if (i + 1 >= n || ms <= (long long)g_rt_pp[i].t) { out[0] = g_rt_pp[i].x; out[1] = g_rt_pp[i].y; out[2] = g_rt_pp[i].z; return 1; }
    {
        float f = (float)(ms - (long long)g_rt_pp[i].t) / (float)((long long)g_rt_pp[i + 1].t - (long long)g_rt_pp[i].t);
        out[0] = g_rt_pp[i].x + (g_rt_pp[i + 1].x - g_rt_pp[i].x) * f;
        out[1] = g_rt_pp[i].y + (g_rt_pp[i + 1].y - g_rt_pp[i].y) * f;
        out[2] = g_rt_pp[i].z + (g_rt_pp[i + 1].z - g_rt_pp[i].z) * f;
    }
    (void)k;
    return 1;
}

/* ───────────── тик: всё происходит в главном потоке ───────────── */
static void native_route_cmd(void* self, void* args, void* mi);
static void rt_report(int code);
static void rt_event(int code) { dlog_i("маршрут: событие ", code); rt_report(code); }

static void rt_post(void) {                                        /* фоновый поток: поставить тик в очередь главного потока */
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { g_rt_tickPending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_route_cmd);
    st = A.string_new("route-tick");
    if (!cb || !st) { g_rt_tickPending = 0; dlog("маршрут: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("маршрут: Post бросил: ", et); g_rt_tickPending = 0; }
}

static void rt_push(unsigned t, unsigned p, float x, float y, float c, float v, unsigned b) {
    int n = g_rt_n;
    if (n >= RT_MAX) return;
    g_rt_s[n].t = t; g_rt_s[n].p = p; g_rt_s[n].x = x; g_rt_s[n].y = y; g_rt_s[n].c = c; g_rt_s[n].v = v; g_rt_s[n].b = b;
    __atomic_store_n(&g_rt_n, n + 1, __ATOMIC_RELEASE);
}
static float rt_abs(float v) { return v < 0 ? -v : v; }

static void rt_stop_rec(Buf* b);
static void rt_release_stick(void) {                               /* отпустить стик и кнопки, если мы их держали */
    if (g_rt_down) { rt_stick_send(3, 0, 0); g_rt_down = 0; }
    rt_btn_apply(0);
}

static void rt_rec_step(void) {
    float x, y, c = 0, v = 0; unsigned p, b = 0; long long now;
    if (g_rt_camRec && !rt_cam_read(&c, &v)) { c = g_rt_lc; v = g_rt_lv; }
    if (g_rt_btnRec) b = rt_btn_mask();
    if (!rt_stick_read(&x, &y)) {
        if (g_rt_dress && g_rt_started && g_rt_n >= 2) {           /* раздевалка: HUD мог исчезнуть у самой метки — то, что записали, сохраняем */
            char t[520]; Buf b; b_init(&b, t, sizeof t);
            b_s(&b, "Раздевалка: стик пропал (HUD закрылся?) — запись завершена автоматически. ");
            rt_stop_rec(&b); chat_say(t);
            return;
        }
        __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(4); return;
    }
    p = (x != 0 || y != 0) ? 1u : 0u;
    if (!g_rt_started) {                                           /* ждём, когда вы возьмётесь за стик */
        if (p) {
            float me0[3];
            g_rt_started = 1; g_rt_t0 = rt_now();
            if (g_rt_dress && rt_my_pos(me0)) { g_rt_S[0] = me0[0]; g_rt_S[1] = me0[1]; g_rt_S[2] = me0[2]; g_rt_hasS = 1; }   /* раздевалка: точка возврата — где вы начали идти */
            g_rt_lp = p; g_rt_lx = x; g_rt_ly = y; g_rt_lc = c; g_rt_lv = v; g_rt_lb = b;
            g_rt_n = 0;
            rt_push(0, p, x, y, c, v, b);
            g_rt_pn = 0; g_rt_lastP = -(long long)RT_PSTEP; g_rt_lastDevLog = -1000;
            rt_pos_push(0);
            dlog("маршрут: запись началась");
            rt_log_state("запись", 0);
        }
        return;
    }
    now = rt_now() - g_rt_t0;
    if (p != g_rt_lp || b != g_rt_lb || rt_abs(x - g_rt_lx) > 0.004f || rt_abs(y - g_rt_ly) > 0.004f || (g_rt_camRec && (rt_abs(c - g_rt_lc) > 0.15f || rt_abs(v - g_rt_lv) > 0.15f))) {
        rt_push((unsigned)now, p, x, y, c, v, b);
        g_rt_lp = p; g_rt_lx = x; g_rt_ly = y; g_rt_lc = c; g_rt_lv = v; g_rt_lb = b;
    }
    if (now - g_rt_lastP >= RT_PSTEP) rt_pos_push(now);
    if (now - g_rt_lastDevLog >= 1000) { g_rt_lastDevLog = now; rt_log_state("запись", now); }
    if (g_rt_n >= RT_MAX - 2) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(3); return; }
    if (now > RT_MAXMS) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); rt_event(5); return; }
}

static void rt_play_stop(int code) {
    __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE);
    g_rt_fix = 0;
    rt_release_stick();
    if (code == 1 && g_rt_rev && g_rt_dress) g_rt_dst = 0;          /* вернулись — раздевалка пройдена, следующий /route dress начнёт новую запись */
    if (code) rt_event(code);
}

/* Возврат: запись «на момент a» — последняя точка с t <= a. Время a только убывает, поэтому индекс идёт назад (g_rt_pi). */
static RtS* rt_rev_sample(long long a) {
    int n = g_rt_n;
    if (g_rt_pi >= n) g_rt_pi = n - 1;
    if (g_rt_pi < 0) g_rt_pi = 0;
    while (g_rt_pi > 0 && (long long)g_rt_s[g_rt_pi].t > a) g_rt_pi--;
    return &g_rt_s[g_rt_pi];
}

/* ───────────── дожим после возврата (раздевалка) ─────────────
   Возврат подаёт ровно тот стик, что вы подавали по дороге туда, но персонаж инерционный: в конце записи он бежит на полной скорости и после
   отпускания стика катится ещё около метра. Поэтому по окончании записи мод стик отпускает, ждёт остановки и, если до точки старта записи
   (плюс смещение, с которым вы начинали возврат) ещё больше RT_FIX_TOL, короткими заходами подтягивает персонажа к ней: направление считается
   из позиции и реальной камеры, скорость мала и падает по мере приближения, отпускание — с упреждением на накат. Не больше RT_FIX_MS и RT_FIX_LEGS
   заходов; дальше RT_FIX_MAXD от точки не трогаем. Взялись за стик — уступаем (общая проверка в rt_play_step). */
static float rt_sind(float d) {                                    /* sin в градусах без libm: приводим к [-90°, 90°] и считаем ряд Тейлора */
    float x, x2;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    if (d > 90.0f) d = 180.0f - d; else if (d < -90.0f) d = -180.0f - d;
    x = d * 0.017453293f; x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
}
static int rt_fix_put(int on, float x, float y) {                  /* on=1 — вести стиком (x, y), 0 — отпустить; запоминаем, что писали (проверка «взялись за стик») */
    float wx = 0, wy = 0;
    if (on) {
        if (!g_rt_down) { if (!rt_stick_send(1, 0, 0)) return 0; g_rt_down = 1; }
        if (!rt_stick_send(2, x, y)) return 0;
        wx = x; wy = y;
    } else if (g_rt_down) {
        if (!rt_stick_send(3, 0, 0)) return 0;
        g_rt_down = 0;
    }
    rt_btn_apply(0);
    g_rt_wh[g_rt_whi & 3][0] = wx; g_rt_wh[g_rt_whi & 3][1] = wy; g_rt_whi++;
    g_rt_wrote = 1;
    return 1;
}
static void rt_fix_log(const char* tag, const float* me) {          /* журнал: позиция, до цели, скорость */
    char t[300]; Buf b; double dx = me[0] - g_rt_fixTgt[0], dz = me[2] - g_rt_fixTgt[2];
    b_init(&b, t, sizeof t);
    b_s(&b, "раздевалка: "); b_s(&b, tag);
    b_s(&b, ", поз "); b_pos(&b, me);
    b_s(&b, ", до цели "); b_fix(&b, __builtin_sqrt(dx * dx + dz * dz), 2); b_s(&b, " м (Δ "); b_fix(&b, dx, 2); b_s(&b, ", "); b_fix(&b, dz, 2); b_ch(&b, ')');
    if (g_rt_fixHave >= 2) { b_s(&b, ", скорость "); b_fix(&b, g_rt_fixSp, 2); b_s(&b, " м/с"); }
    b_s(&b, g_rt_down ? ", стик держу" : ", стик отпущен");
    dlog(t);
}
static void rt_fix_done(const char* why) {                         /* конец дожима: журнал и обычное завершение возврата (стик отпускается, в чат — итог) */
    float me[3];
    if (rt_my_pos(me)) rt_fix_log(why, me); else dlog2("раздевалка: дожим закончен: ", why);
    rt_play_stop(1);
}
/* 1 — дожим начат (стик отпущен, дальше тики идут в rt_fix_step); 0 — дожимать нечего/нельзя, возврат можно завершать */
static int rt_fix_begin(long long tnow) {
    float me[3], dx, dz, d;
    g_rt_fixLegs = 0; g_rt_fix = 0;
    if (!g_rt_hasS || !rt_my_pos(me)) { dlog("раздевалка: возврат по записи закончен, позиция или точка старта неизвестны — дожима нет"); return 0; }
    g_rt_fixTgt[0] = g_rt_S[0] + g_rt_startDev[0]; g_rt_fixTgt[1] = g_rt_S[1]; g_rt_fixTgt[2] = g_rt_S[2] + g_rt_startDev[2];
    dx = g_rt_fixTgt[0] - me[0]; dz = g_rt_fixTgt[2] - me[2]; d = (float)__builtin_sqrt((double)dx * dx + (double)dz * dz);
    g_rt_fixHave = 0; g_rt_fixSp = 0; g_rt_fixCl = 0;
    rt_fix_log("возврат по записи закончен", me);
    if (d > RT_FIX_MAXD) { dlog("раздевалка: дальше 4 м от точки старта — дожим не делаю"); return 0; }
    g_rt_fixT0 = g_rt_fixLT = tnow; g_rt_fixLog = tnow; g_rt_fixStall = 0; g_rt_fixD0 = d;
    g_rt_fixPrev[0] = me[0]; g_rt_fixPrev[1] = me[1]; g_rt_fixPrev[2] = me[2]; g_rt_fixPD = d;
    if (!rt_fix_put(0, 0, 0)) return 0;
    g_rt_fix = 1;
    return 1;
}
static void rt_fix_step(long long tnow) {
    float me[3], dx, dz, dist, dt, sp, cl, yaw = 0, pv = 0, s, c, ux, uz, r, f, mag; long long ldt = tnow - g_rt_fixLT; int have = g_rt_fixHave, drive = 0, moving;
    if (!rt_my_pos(me)) { dlog("раздевалка: дожим: позиция не читается, останавливаюсь"); rt_play_stop(1); return; }
    dx = g_rt_fixTgt[0] - me[0]; dz = g_rt_fixTgt[2] - me[2];
    dist = (float)__builtin_sqrt((double)dx * dx + (double)dz * dz);
    if (have >= 1 && ldt > 0) {                                    /* скорость и сближение по двум последним замерам */
        float mx = me[0] - g_rt_fixPrev[0], mz = me[2] - g_rt_fixPrev[2];
        dt = (float)ldt / 1000.0f;
        sp = (float)__builtin_sqrt((double)mx * mx + (double)mz * mz) / dt;
        cl = (g_rt_fixPD - dist) / dt;
        if (have == 1) { g_rt_fixSp = sp; g_rt_fixCl = cl; }
        else { g_rt_fixSp += (sp - g_rt_fixSp) * 0.5f; g_rt_fixCl += (cl - g_rt_fixCl) * 0.5f; }
    }
    g_rt_fixPrev[0] = me[0]; g_rt_fixPrev[1] = me[1]; g_rt_fixPrev[2] = me[2]; g_rt_fixPD = dist; g_rt_fixLT = tnow;
    if (g_rt_fixHave < 1000) g_rt_fixHave++;
    if (have < 1) return;                                          /* первый замер: только запомнили позицию (стик уже отпущен) */
    if (tnow - g_rt_fixT0 > RT_FIX_MS) { rt_fix_done("дожим: время вышло"); return; }
    moving = g_rt_fixSp > 0.3f;
    if (dist > RT_FIX_TOL) {
        if (g_rt_down) drive = !(g_rt_fixCl > 0.2f && dist <= g_rt_fixCl * 0.30f + 0.10f);   /* ведём; пора отпускать — докатится сам (накат ≈ 0.3 с скорости) */
        else if (!moving) {                                        /* стоим и не дошли — новый заход */
            if (g_rt_fixLegs >= RT_FIX_LEGS) { rt_fix_done("дожим: заходы кончились"); return; }
            g_rt_fixLegs++; g_rt_fixStall = 0; drive = 1;
            if (g_rt_fixLegs == 1) g_rt_fixD0 = dist;              /* сколько было после остановки — в итоговое сообщение */
        }
    } else if (!moving) { rt_fix_done("дожим: дошёл"); return; }
    if (drive && g_rt_fixSp < 0.1f) {                              /* стик подан, а персонаж не движется: упёрся или мёртвая зона стика */
        g_rt_fixStall += ldt;
        if (g_rt_fixStall > 700) { rt_fix_done("дожим: стик подан, а персонаж не двигается — останавливаюсь"); return; }
    } else g_rt_fixStall = 0;
    if (drive) {
        float ty = 0, tx = 0;
        if (rt_cam_true(&ty, &tx)) yaw = ty;                       /* направление считаем от реальной камеры: стик «вперёд» идёт туда, куда она смотрит */
        else if (!rt_cam_read(&yaw, &pv)) { rt_fix_done("дожим: камера не читается"); return; }
        ux = dx / dist; uz = dz / dist;
        s = rt_sind(yaw); c = rt_sind(90.0f - yaw);                /* мир (x, z) = вправо·(cos, −sin) + вперёд·(sin, cos) → обратно */
        r = ux * c - uz * s; f = ux * s + uz * c;
        mag = dist * 1.5f; if (mag < 0.8f) mag = 0.8f; if (mag > 1.5f) mag = 1.5f;
        if (!rt_fix_put(1, r * mag, f * mag)) { rt_play_stop(4); return; }
    } else if (!rt_fix_put(0, 0, 0)) { rt_play_stop(4); return; }
    if (tnow - g_rt_fixLog >= 250) { g_rt_fixLog = tnow; rt_fix_log(drive ? "дожим: иду к точке" : "дожим: жду остановки", me); }
}

static void rt_play_step(void) {
    long long tnow = rt_now(), now = tnow - g_rt_t0, look, tl; int n = g_rt_n, i, ok; float cx, cy, wx, wy; RtS* s;
    if (n < 2) { __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE); return; }
    if (g_rt_lastTk) {                                           /* длина кадра = интервал между тиками (тик идёт раз за кадр) */
        float d = (float)(tnow - g_rt_lastTk);
        if (d < 5.0f) d = 5.0f;
        if (d > 200.0f) d = 200.0f;
        g_rt_dtEma += (d - g_rt_dtEma) * 0.1f;
    }
    g_rt_lastTk = tnow;
    g_rt_lead = g_rt_leadArg >= 0 ? (float)g_rt_leadArg : g_rt_dtEma;
    if (g_rt_lead > 250.0f) g_rt_lead = 250.0f;
    look = now + (long long)(g_rt_lead + 0.5f);                  /* по этому времени выбираем записанный стик/кнопки/камеру */
    /* Возврат (раздевалка): идём по записи с конца к началу. Запись в момент a описывает смещение за кадр ДО a, а значение, поданное сейчас,
       сработает в СЛЕДУЮЩЕМ кадре — поэтому при обратном ходе опережение не нужно: берём ровно «конец минус прошедшее время».
       Мир при этом идёт назад, если камеру повернуть на 180° при том же стике (стик «вперёд» относительно камеры). */
    if (g_rt_rev) { look = g_rt_revT0 - now; g_rt_lead = 0; }
    tl = g_rt_rev ? g_rt_revT0 - now : now;                      /* момент ЗАПИСИ, который сейчас «повторяется» (для сравнения с дорожкой позиций) */
    if (now < 0) {                                               /* предстарт: сначала выставляем камеру в положение начала записи, стик ещё не трогаем */
        RtS* f = g_rt_rev ? rt_rev_sample(g_rt_revT0) : &g_rt_s[0];
        if (g_rt_camOn && !rt_cam_write(g_rt_rev ? rt_flip_yaw(f->c) : f->c, f->v)) {
            if (g_rt_rev) { rt_play_stop(6); return; }           /* без камеры возврат идёт не туда — не начинаем */
            g_rt_camOn = 0; dlog("маршрут: камеру выставить не удалось, дальше только стик");
        }
        return;
    }
    if (g_rt_wrote) {                                            /* кто-то (вы) изменил стик не так, как мы писали — уступаем */
        if (!rt_stick_read(&cx, &cy)) { rt_play_stop(4); return; }
        ok = 0;                                                  /* значение доходит до стика с задержкой в кадр: сверяем с несколькими последними записанными */
        for (i = 0; i < 4; i++) if (rt_abs(cx - g_rt_wh[i][0]) <= 0.02f && rt_abs(cy - g_rt_wh[i][1]) <= 0.02f) ok = 1;
        if (!ok) {
            /* значение не наше: НЕ перезаписываем (иначе будем бороться с пальцем); 6 тиков подряд — уступаем */
            if (++g_rt_bad >= 6) {
                char t[200]; Buf b; b_init(&b, t, sizeof t);
                b_s(&b, "маршрут: повтор прерван, прочитали стик "); b_fix(&b, cx, 3); b_ch(&b, '/'); b_fix(&b, cy, 3);
                dlog(t);
                rt_play_stop(2);
            }
            return;
        }
        g_rt_bad = 0;
    }
    if (g_rt_pn >= 2 && !g_rt_fix && now - g_rt_lastDev >= 100) {              /* отклонение от записи: где вы сейчас против того, где были в записи в этот же момент */
        float me[3], rp[3];
        g_rt_lastDev = now;
        if (rt_my_pos(me) && rt_pos_at(tl, rp)) {
            double dx = me[0] - rp[0], dy = me[1] - rp[1], dz = me[2] - rp[2];
            float dev = (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz);
            if (dev > g_rt_maxDev) { g_rt_maxDev = dev; g_rt_maxDevT = now; }
            if (now - g_rt_lastDevLog >= (g_rt_rev ? 250 : 1000)) {   /* возврат короткий — пишем чаще */
                char t[200]; Buf b; b_init(&b, t, sizeof t);
                g_rt_lastDevLog = now;
                b_s(&b, "маршрут: повтор t="); b_fix(&b, (double)now / 1000.0, 1); b_s(&b, " с, отклонение от записи "); b_fix(&b, dev, 2);
                b_s(&b, " м (Δ "); b_fix(&b, dx, 2); b_s(&b, ", "); b_fix(&b, dy, 2); b_s(&b, ", "); b_fix(&b, dz, 2); b_ch(&b, ')');
                b_s(&b, ", кадр "); b_fix(&b, g_rt_dtEma, 0); b_s(&b, " мс, опережение "); b_fix(&b, g_rt_lead, 0); b_s(&b, " мс");
                dlog(t);
                rt_log_state("повтор", now);
            }
        }
    }
    if (g_rt_rev) {
        if (g_rt_fix) { rt_fix_step(tnow); return; }             /* запись кончилась, идёт дожим до точки старта */
        if (look <= (long long)(g_rt_dtEma * 0.5f)) {            /* дошли до начала записи (с точностью до полкадра): отпускаем стик, дальше — дожим */
            if (rt_fix_begin(tnow)) return;
            rt_play_stop(1);
            return;
        }
        s = rt_rev_sample(look);
    } else {
        while (g_rt_pi + 1 < n && (long long)g_rt_s[g_rt_pi + 1].t <= look) g_rt_pi++;
        s = &g_rt_s[g_rt_pi];
        if (g_rt_pi >= n - 1 && look >= (long long)g_rt_s[n - 1].t) {            /* дошли до конца записи: отпускаем стик */
            rt_play_stop(1);
            return;
        }
    }
    if (g_rt_camOn && !rt_cam_write(g_rt_rev ? rt_flip_yaw(s->c) : s->c, s->v)) {
        if (g_rt_rev) { rt_play_stop(6); return; }               /* возврат без камеры пошёл бы в другую сторону — останавливаемся */
        g_rt_camOn = 0; dlog("маршрут: камера пропала (режим камеры сменился?), дальше только стик");
    }
    if (s->p) {
        if (!g_rt_down) { if (!rt_stick_send(1, 0, 0)) { rt_play_stop(4); return; } g_rt_down = 1; }
        if (!rt_stick_send(2, s->x, s->y)) { rt_play_stop(4); return; }
        wx = s->x; wy = s->y;
    } else {
        if (g_rt_down) { if (!rt_stick_send(3, 0, 0)) { rt_play_stop(4); return; } g_rt_down = 0; }
        wx = 0; wy = 0;
    }
    if (g_rt_btnRec) rt_btn_apply(s->b & ((1u << RT_NB) - 1));
    else rt_btn_apply((s->p && s->y > 1.3f) ? 1u : 0u);              /* запись без кнопок: спринт по правилу Hud.js (стик вверх > 1.3 → Space) */
    g_rt_wh[g_rt_whi & 3][0] = wx; g_rt_wh[g_rt_whi & 3][1] = wy; g_rt_whi++;
    g_rt_wrote = 1;
}

static void rt_tick(void) {                                        /* главный поток */
    int mode = __atomic_load_n(&g_rt_mode, __ATOMIC_ACQUIRE);
    g_rt_tickPending = 0;
    if (mode == 1) rt_rec_step(); else if (mode == 2) rt_play_step();
}

static void* rt_worker(void* arg) {
    (void)arg;
    for (;;) {
        int mode = __atomic_load_n(&g_rt_mode, __ATOMIC_ACQUIRE);
        if (mode == 0) { usleep(40000); continue; }
        if (!g_rt_tickPending || rt_now() - g_rt_tickT > 1000) {   /* предыдущий тик ещё не выполнен — не копим очередь (но если потерялся — ставим заново) */
            g_rt_tickPending = 1; g_rt_tickT = rt_now();
            rt_post();
        }
        usleep(12000);
    }
    return NULL;
}

/* ───────────── файл HLOG/route.txt ─────────────
   H <имя_гориз> <имя_верт> <индекс>     (пустое имя — «-»)
   S x y z    позиция в начале записи   E x y z    позиция в конце     K 1 — в записи есть камера
   затем строки: <мс> <нажат 0/1> <x> <y> [<поворот камеры> <наклон камеры>] */
#define RT_FILE "route.txt"
#define RT_FILE_DRESS "route_dress.txt"
static int rt_path(char* out, int cap, const char* name) {
    int i, last = -1; Buf b;
    if (!g_flog[0]) return 0;
    for (i = 0; g_flog[i]; i++) if (g_flog[i] == '/') last = i;
    if (last < 0 || last + 24 > cap) return 0;
    b_init(&b, out, cap);
    for (i = 0; i <= last; i++) b_ch(&b, g_flog[i]);
    b_s(&b, name);
    return 1;
}
static int rt_save(const char* name) {
    char path[320]; static char buf[8192]; Buf b; int i, n = g_rt_n, first = 1;
    if (!rt_path(path, sizeof path, name)) return 0;
    b_init(&b, buf, sizeof buf);
    b_s(&b, "H "); b_s(&b, g_rt_hn[0] ? g_rt_hn : "-"); b_ch(&b, ' '); b_s(&b, g_rt_vn[0] ? g_rt_vn : "-"); b_ch(&b, ' '); b_int(&b, g_rt_idx); b_ch(&b, '\n');
    if (g_rt_hasS) { b_s(&b, "S "); b_fix(&b, g_rt_S[0], 3); b_ch(&b, ' '); b_fix(&b, g_rt_S[1], 3); b_ch(&b, ' '); b_fix(&b, g_rt_S[2], 3); b_ch(&b, '\n'); }
    if (g_rt_hasE) { b_s(&b, "E "); b_fix(&b, g_rt_E[0], 3); b_ch(&b, ' '); b_fix(&b, g_rt_E[1], 3); b_ch(&b, ' '); b_fix(&b, g_rt_E[2], 3); b_ch(&b, '\n'); }
    if (g_rt_camRec) b_s(&b, "K 1\n");
    if (g_rt_btnRec) b_s(&b, "B 1\n");
    for (i = 0; i < g_rt_pn; i++) {                                 /* P <мс> x y z — дорожка позиций (для отчёта об отклонении при повторе) */
        b_s(&b, "P "); b_int(&b, (long)g_rt_pp[i].t); b_ch(&b, ' '); b_fix(&b, g_rt_pp[i].x, 3); b_ch(&b, ' '); b_fix(&b, g_rt_pp[i].y, 3); b_ch(&b, ' '); b_fix(&b, g_rt_pp[i].z, 3); b_ch(&b, '\n');
        if (b.n > sizeof buf - 100) {
            if (!flog_write(path, b.p, b.n, first)) return 0;
            first = 0; b_init(&b, buf, sizeof buf);
        }
    }
    for (i = 0; i < n; i++) {
        b_int(&b, (long)g_rt_s[i].t); b_ch(&b, ' '); b_int(&b, (long)g_rt_s[i].p); b_ch(&b, ' ');
        b_fix(&b, g_rt_s[i].x, 3); b_ch(&b, ' '); b_fix(&b, g_rt_s[i].y, 3);
        if (g_rt_camRec || g_rt_btnRec) { b_ch(&b, ' '); b_fix(&b, g_rt_s[i].c, 2); b_ch(&b, ' '); b_fix(&b, g_rt_s[i].v, 2); }
        if (g_rt_btnRec) { b_ch(&b, ' '); b_int(&b, (long)g_rt_s[i].b); }
        b_ch(&b, '\n');
        if (b.n > sizeof buf - 100) {
            if (!flog_write(path, b.p, b.n, first)) return 0;
            first = 0; b_init(&b, buf, sizeof buf);
        }
    }
    if (b.n || first) if (!flog_write(path, b.p, b.n, first)) return 0;
    return 1;
}
static const char* rt_num(const char* s, double* out) {
    double v = 0, f = 1; int neg = 0, any = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { f /= 10; v += (*s - '0') * f; s++; any = 1; } }
    if (!any) return NULL;
    *out = neg ? -v : v;
    return s;
}
static const char* rt_word(const char* s, char* out, int cap) {
    int k = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && *s != '\n' && *s != '\r') { if (k < cap - 1) out[k++] = *s; s++; }
    out[k] = 0;
    return s;
}
static int rt_load(const char* name) {
    char path[320]; int fd, cnt = 0; long tot = 0; const char* p; double v[4];
    if (!rt_path(path, sizeof path, name)) return 0;
    fd = open(path, 0);
    if (fd < 0) return 0;
    for (;;) { long r = read(fd, g_rt_fbuf + tot, RT_FBUF - 1 - tot); if (r <= 0) break; tot += r; if (tot >= RT_FBUF - 1) break; }
    close(fd);
    g_rt_fbuf[tot] = 0;
    g_rt_hasS = g_rt_hasE = 0; g_rt_hn[0] = g_rt_vn[0] = 0; g_rt_idx = -1; g_rt_camRec = 0; g_rt_btnRec = 0; g_rt_pn = 0;
    for (p = g_rt_fbuf; *p; ) {
        const char* q = p;
        if (*q == 'H') {
            q = rt_word(q + 1, g_rt_hn, sizeof g_rt_hn); q = rt_word(q, g_rt_vn, sizeof g_rt_vn);
            if (seq(g_rt_hn, "-")) g_rt_hn[0] = 0;
            if (seq(g_rt_vn, "-")) g_rt_vn[0] = 0;
            if ((q = rt_num(q, &v[0]))) g_rt_idx = (int)v[0];
        } else if (*q == 'S' || *q == 'E') {
            int isS = (*q == 'S'); float* dst = isS ? g_rt_S : g_rt_E;
            q++;
            if ((q = rt_num(q, &v[0])) && (q = rt_num(q, &v[1])) && (q = rt_num(q, &v[2]))) {
                dst[0] = (float)v[0]; dst[1] = (float)v[1]; dst[2] = (float)v[2];
                if (isS) g_rt_hasS = 1; else g_rt_hasE = 1;
            }
        } else if (*q == 'P') {
            q++;
            if ((q = rt_num(q, &v[0])) && (q = rt_num(q, &v[1])) && (q = rt_num(q, &v[2])) && (q = rt_num(q, &v[3])) && g_rt_pn < RT_MAXP) {
                g_rt_pp[g_rt_pn].t = (unsigned)v[0]; g_rt_pp[g_rt_pn].x = (float)v[1]; g_rt_pp[g_rt_pn].y = (float)v[2]; g_rt_pp[g_rt_pn].z = (float)v[3];
                g_rt_pn++;
            }
        } else if (*q == 'K') {
            g_rt_camRec = 1;
        } else if (*q == 'B') {
            g_rt_btnRec = 1;
        } else if (*q >= '0' && *q <= '9') {
            if ((q = rt_num(q, &v[0])) && (q = rt_num(q, &v[1])) && (q = rt_num(q, &v[2])) && (q = rt_num(q, &v[3])) && cnt < RT_MAX) {
                double cc = 0, vv = 0, bb = 0; const char* q2 = rt_num(q, &cc); const char* q3 = NULL;
                if (q2) q3 = rt_num(q2, &vv);
                if (q3) rt_num(q3, &bb);
                g_rt_s[cnt].t = (unsigned)v[0]; g_rt_s[cnt].p = v[1] > 0.5 ? 1u : 0u; g_rt_s[cnt].x = (float)v[2]; g_rt_s[cnt].y = (float)v[3];
                g_rt_s[cnt].c = (float)cc; g_rt_s[cnt].v = (float)vv; g_rt_s[cnt].b = (unsigned)bb;
                cnt++;
            }
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    g_rt_n = cnt;
    g_rt_src = (cnt >= 2) ? (seq(name, RT_FILE_DRESS) ? 2 : 1) : 0;
    return cnt;
}

/* ───────────── команда ───────────── */
/* слова: 1 rec/record, 2 stop/cancel, 4 play, 16 status, 32 probe, 64 force, 128 nocam, 256 dress/раздевалка, 512 back/назад */
static int rt_parse(void* arr) {
    long len; int i, w = 0; char* data;
    g_rt_leadArg = -1;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[96]; int n, p = 0; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 60 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        while (tmp[p]) {
            char tok[24]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 23) { char c = tmp[p]; tok[q++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; } p++; }
            tok[q] = 0;
            if (seq(tok, "rec") || seq(tok, "record")) w |= 1;
            else if (seq(tok, "stop") || seq(tok, "cancel")) w |= 2;
            else if (seq(tok, "play")) w |= 4;
            else if (seq(tok, "status") || seq(tok, "diag")) w |= 16;
            else if (seq(tok, "probe")) w |= 32;
            else if (seq(tok, "force")) w |= 64;
            else if (seq(tok, "nocam")) w |= 128;
            else if (seq(tok, "dress") || seq(tok, "раздевалка")) w |= 256;
            else if (seq(tok, "back") || seq(tok, "назад")) w |= 512;
            else if (tok[0] >= '0' && tok[0] <= '9') { int v = 0, k; for (k = 0; tok[k] >= '0' && tok[k] <= '9' && k < 6; k++) v = v * 10 + (tok[k] - '0'); g_rt_leadArg = v > 250 ? 250 : v; }
        }
    }
    return w;
}

static int rt_my_pos(float* me) {
    void* lp = local_player();                                       /* из пула игроков, который находит ядро (мод Near не нужен) */
    if (!lp || !hm_obj_ok(lp)) return 0;
    return read_pos(lp, me);
}
static void b_pos(Buf* b, const float* p) { b_fix(b, p[0], 1); b_s(b, ", "); b_fix(b, p[1], 1); b_s(b, ", "); b_fix(b, p[2], 1); }
static double rt_dist2(const float* a, const float* c) { double dx = a[0] - c[0], dz = a[2] - c[2]; return __builtin_sqrt(dx * dx + dz * dz); }   /* по горизонтали: x и z (индекс 1 — высота, в записях раздевалки он почти не меняется) */

static void rt_report(int code) {                                  /* главный поток */
    Buf b; char msg[520]; float me[3];
    b_init(&b, msg, sizeof msg);
    if (code == 1 && g_rt_rev) {
        b_s(&b, "Раздевалка: вернулся назад по записанному пути");
        if (g_rt_hasS && rt_my_pos(me)) {
            double dx = me[0] - g_rt_S[0], dy = me[1] - g_rt_S[1], dz = me[2] - g_rt_S[2];
            b_s(&b, ". До точки, где вы начали запись: "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 2); b_s(&b, " м (Δ "); b_fix(&b, dx, 2); b_s(&b, ", "); b_fix(&b, dy, 2); b_s(&b, ", "); b_fix(&b, dz, 2); b_ch(&b, ')');
        }
        if (g_rt_fixLegs > 0) { b_s(&b, ". Дожим: после остановки было "); b_fix(&b, g_rt_fixD0, 2); b_s(&b, " м до точки, заходов "); b_int(&b, g_rt_fixLegs); }
        if (g_rt_pn >= 2) { b_s(&b, ". Макс. отклонение от записи "); b_fix(&b, g_rt_maxDev, 2); b_s(&b, " м"); }
    } else if (code == 1) {
        b_s(&b, "Маршрут пройден");
        if (g_rt_hasE && rt_my_pos(me)) {
            double dx = me[0] - g_rt_E[0], dy = me[1] - g_rt_E[1], dz = me[2] - g_rt_E[2];
            b_s(&b, ". До точки конца записи: "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 2); b_s(&b, " м (Δ "); b_fix(&b, dx, 2); b_s(&b, ", "); b_fix(&b, dy, 2); b_s(&b, ", "); b_fix(&b, dz, 2); b_ch(&b, ')');
        }
        if (g_rt_pn >= 2) { b_s(&b, ". Макс. отклонение от записи "); b_fix(&b, g_rt_maxDev, 2); b_s(&b, " м на "); b_fix(&b, (double)g_rt_maxDevT / 1000.0, 1); b_s(&b, " с"); }
    } else if (code == 2 && g_rt_rev) b_s(&b, "Раздевалка: возврат прерван — вы взялись за стик. Снова /route dress — продолжу с того места пути, где вы стоите (если вы рядом с записанным путём)");
    else if (code == 2) b_s(&b, "Маршрут: повтор прерван — вы взялись за стик");
    else if (code == 6) b_s(&b, "Раздевалка: камера недоступна (режим камеры сменился?) — возврат остановлен, без неё путь не повторить. Повторите /route dress");
    else if (code == 3) { b_s(&b, "Маршрут: запись заполнена. "); rt_stop_rec(&b); }
    else if (code == 4 && g_rt_rev) b_s(&b, "Раздевалка: стик пропал (HUD пересоздан?), возврат остановлен. Снова /route dress — продолжу с места, где вы стоите");
    else if (code == 4) b_s(&b, "Маршрут: стик пропал (HUD пересоздан?), остановлено. Повторите /route probe");
    else if (code == 5) { b_s(&b, "Маршрут: лимит 10 минут. "); rt_stop_rec(&b); }
    else return;
    chat_say(msg);
}

/* раздевалка: срезать «стояние» в конце записи — время между последним отпусканием стика и вашей командой /route dress (его возвращать не нужно) */
static void rt_trim_tail(void) {
    int n = g_rt_n, i; long long T;
    for (i = n - 1; i >= 0 && !g_rt_s[i].p; i--) ;
    if (i < 0 || i + 2 >= n) return;                               /* стика не было, или последняя точка уже и есть отпускание */
    g_rt_n = i + 2;                                                /* i — последняя точка со стиком, i+1 — его отпускание (оставляем) */
    T = (long long)g_rt_s[g_rt_n - 1].t;
    while (g_rt_pn > 0 && (long long)g_rt_pp[g_rt_pn - 1].t > T) g_rt_pn--;   /* дорожка позиций тоже до момента отпускания */
}

static void rt_stop_rec(Buf* b) {                                  /* главный поток; режим записи уже выключен или выключается здесь */
    int n; float me[3];
    __atomic_store_n(&g_rt_mode, 0, __ATOMIC_RELEASE);
    n = g_rt_n;
    if (!g_rt_started || n < 1) { b_s(b, "Запись остановлена: вы так и не брались за стик, ничего не записано"); return; }
    rt_push((unsigned)(rt_now() - g_rt_t0), 0, 0, 0, g_rt_lc, g_rt_lv, 0);   /* отпускание стика — чтобы повтор знал длительность */
    if (g_rt_dress) rt_trim_tail();
    n = g_rt_n;
    g_rt_idx = 0; g_rt_hn[0] = '-'; g_rt_hn[1] = 0; g_rt_vn[0] = '-'; g_rt_vn[1] = 0;
    g_rt_hasE = rt_my_pos(me); if (g_rt_hasE) { g_rt_E[0] = me[0]; g_rt_E[1] = me[1]; g_rt_E[2] = me[2]; }
    rt_pos_push((long long)g_rt_s[n - 1].t);
    b_s(b, "Записано: "); b_int(b, n); b_s(b, " точек, "); b_fix(b, (double)g_rt_s[n - 1].t / 1000.0, 1); b_s(b, " с");
    if (g_rt_hasS && g_rt_hasE) { b_s(b, ", от старта до финиша по прямой "); b_fix(b, rt_dist2(g_rt_S, g_rt_E), 1); b_s(b, " м"); }
    if (g_rt_dress) {
        g_rt_src = 2; g_rt_dst = 1;                                 /* в памяти — раздевалка, вы «у метки»: следующий /route dress вернёт назад */
        b_s(b, rt_save(RT_FILE_DRESS) ? ". Сохранено в HLOG/route_dress.txt. " : ". Файл сохранить не удалось (только в памяти). ");
        b_s(b, "Переоденьтесь и снова /route dress — вернусь по этому же пути");
    } else {
        g_rt_src = 1;
        b_s(b, rt_save(RT_FILE) ? ". Сохранено в HLOG/route.txt. Повтор: /route play" : ". Файл сохранить не удалось (только в памяти). Повтор: /route play");
    }
}

/* Где на записанном пути вы сейчас: ближайшая точка дорожки позиций (с проекцией на отрезок). Нужна, чтобы продолжить возврат,
   если он был прерван на полпути. tOut — момент записи в этой точке (мс), dOut — расстояние до неё (м). */
static int rt_track_snap(const float* me, long long* tOut, double* dOut) {
    int n = g_rt_pn, i; double best = 1e30; long long bt = 0;
    if (n < 2) return 0;
    for (i = 0; i + 1 < n; i++) {
        const RtP* p = &g_rt_pp[i]; const RtP* q = &g_rt_pp[i + 1];
        double ex = q->x - p->x, ey = q->y - p->y, ez = q->z - p->z, wx = me[0] - p->x, wy = me[1] - p->y, wz = me[2] - p->z;
        double l2 = ex * ex + ey * ey + ez * ez, f = l2 > 1e-6 ? (wx * ex + wy * ey + wz * ez) / l2 : 0, dx, dy, dz, d;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        dx = wx - ex * f; dy = wy - ey * f; dz = wz - ez * f;
        d = __builtin_sqrt(dx * dx + dy * dy + dz * dz);
        if (d < best) { best = d; bt = (long long)p->t + (long long)((double)((long long)q->t - (long long)p->t) * f); }
    }
    *tOut = bt; *dOut = best;
    return 1;
}

static void native_route_cmd(void* self, void* args, void* mi) {
    int w, camBad = 0, dressRec = 0; Buf b; char msg[900]; float me[3];
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { rt_tick(); return; }   /* вызов из Post (тик), не из чата */
    mt_capture_ctx();                                                /* главный поток: берём контекст для тиков */
    w = rt_parse(args);
    dlog_i("нативная /route: вызвана, флаги ", w);
    b_init(&b, msg, sizeof msg);

    if (w & 32) {                                                    /* probe */
        int ok = rt_find();
        if (ok) {
            float x = 0, y = 0; int rd = rt_stick_read(&x, &y);
            b_s(&b, "Стик найден: "); b_s(&b, g_rt_key[0] ? g_rt_key : "-");
            b_s(&b, rd ? ", значение (" : ", значение НЕ читается ("); b_fix(&b, x, 2); b_s(&b, "; "); b_fix(&b, y, 2); b_ch(&b, ')');
        } else b_s(&b, "Стик НЕ найден (причина в konstt.log)");
        if (rt_cam_find()) {
            float c = 0, v = 0, ty = 0, tx = 0; int ck;
            rt_cam_read(&c, &v); ck = rt_cam_check();
            b_s(&b, " | камера: поворот "); b_fix(&b, c, 1); b_s(&b, ", наклон "); b_fix(&b, v, 1);
            if (rt_cam_true(&ty, &tx)) { b_s(&b, " (реальная "); b_fix(&b, ty, 1); b_s(&b, "/"); b_fix(&b, tx, 1); b_s(&b, ck == 1 ? ", совпадает)" : ", НЕ СОВПАДАЕТ)"); }
        }
        else b_s(&b, " | камера: НЕ найдена (см. konstt.log)");
        b_s(&b, g_mt_ctx ? " | тики: да" : " | тики: НЕТ");
        dlog2("маршрут probe: ", msg);
        chat_say(msg);
        return;
    }
    if (w & 16) {                                                    /* status */
        b_s(&b, "Маршрут: "); b_s(&b, g_rt_mode == 1 ? (g_rt_dress ? "идёт запись пути к раздевалке" : "идёт запись") : g_rt_mode == 2 ? (g_rt_rev ? "идёт возврат из раздевалки" : "идёт повтор") : "ничего не идёт");
        b_s(&b, ", точек в памяти "); b_int(&b, g_rt_n);
        if (g_rt_n > 0) { b_s(&b, " ("); b_fix(&b, (double)g_rt_s[g_rt_n - 1].t / 1000.0, 1); b_s(&b, " с)"); }
        b_s(&b, g_mt_ctx ? ", тики: да" : ", тики: нет");
        if (g_rt_dst == 1) b_s(&b, ". Раздевалка: путь записан, /route dress вернёт назад");
        chat_say(msg);
        return;
    }
    if (w & 2) {                                                     /* stop */
        if (g_rt_mode == 1) { rt_stop_rec(&b); chat_say(msg); return; }
        if (g_rt_mode == 2) {
            rt_play_stop(0);
            b_s(&b, "Повтор остановлен"); chat_say(msg); return;
        }
        b_s(&b, "Маршрут: сейчас ничего не идёт"); chat_say(msg); return;
    }
    if (w & 256) {                                                   /* раздевалка: запись пути к метке / возврат по нему назад */
        int back;
        if (g_rt_mode == 1 && g_rt_dress) { rt_stop_rec(&b); chat_say(msg); return; }      /* вы у метки: /route dress = стоп записи */
        if (g_rt_mode == 2 && g_rt_rev) { rt_play_stop(0); b_s(&b, "Возврат остановлен. Снова /route dress — продолжу с места, где вы стоите (рядом с записанным путём)"); chat_say(msg); return; }
        if (g_rt_mode) { b_s(&b, "Маршрут: сначала /route stop"); chat_say(msg); return; }
        back = (w & 512) ? 1 : (w & 1) ? 0 : (g_rt_dst == 1);
        if (back) {
            long long t0; int resumed = 0;
            if (!(g_rt_src == 2 && g_rt_n >= 2) && rt_load(RT_FILE_DRESS) < 2) { b_s(&b, "Раздевалка: записанного пути нет. Сначала /route dress — запись, и идите к метке раздевалки"); chat_say(msg); return; }
            if (!g_mt_ctx) { b_s(&b, "Маршрут: нет главного потока для тиков (см. konstt.log)"); chat_say(msg); return; }
            if (rt_find() < 1) { b_s(&b, "Маршрут: стик не найден (HUD с экранным стиком не создан? см. konstt.log, /route probe)"); chat_say(msg); return; }
            if (!g_rt_camRec) { b_s(&b, "Раздевалка: в записи нет камеры — вернуть нельзя. Запишите путь заново: /route dress rec"); chat_say(msg); return; }
            g_rt_camOn = rt_cam_find();
            if (!g_rt_camOn || rt_cam_check() == 0 || g_rt_camMax - g_rt_camMin < 359.0f) { g_rt_camOn = 0; b_s(&b, "Раздевалка: камера не читается или нестандартная (см. konstt.log, /route probe) — без неё возврат не работает"); chat_say(msg); return; }
            t0 = (long long)g_rt_s[g_rt_n - 1].t;
            g_rt_startDev[0] = g_rt_startDev[1] = g_rt_startDev[2] = 0;
            if (rt_my_pos(me)) {
                if (g_rt_hasE) { g_rt_startDev[0] = me[0] - g_rt_E[0]; g_rt_startDev[1] = me[1] - g_rt_E[1]; g_rt_startDev[2] = me[2] - g_rt_E[2]; }
                if (g_rt_hasE && !(w & 64) && rt_dist2(me, g_rt_E) > 2.0) {       /* не у метки: либо вы на полпути (возврат прервали), либо ушли куда-то ещё */
                    long long ts = 0; double ds = 0;
                    if (rt_track_snap(me, &ts, &ds) && ds <= 3.0) { t0 = ts; resumed = 1; g_rt_startDev[0] = g_rt_startDev[1] = g_rt_startDev[2] = 0; }
                    else {
                        b_s(&b, "Раздевалка: вы в "); b_fix(&b, rt_dist2(me, g_rt_E), 1); b_s(&b, " м от метки ("); b_pos(&b, g_rt_E);
                        b_s(&b, ") и не рядом с записанным путём. Встаньте к метке и повторите, или /route dress back force");
                        chat_say(msg); return;
                    }
                }
            }
            g_rt_dress = 1; g_rt_rev = 1; g_rt_revT0 = t0; g_rt_fix = 0; g_rt_fixLegs = 0;
            g_rt_maxDev = 0; g_rt_maxDevT = 0; g_rt_ppi = 0; g_rt_lastDev = -1000; g_rt_lastDevLog = -1000;
            { int q; for (q = 0; q < 4; q++) { g_rt_wh[q][0] = 0; g_rt_wh[q][1] = 0; } }
            g_rt_whi = 0; g_rt_down = 0; g_rt_bdown = 0; g_rt_tickPending = 0; g_rt_lastTk = 0; g_rt_dtEma = 55.0f; g_rt_lead = 0;
            g_rt_pi = g_rt_n - 1; g_rt_wrote = 0; g_rt_bad = 0; g_rt_t0 = rt_now() + 250;      /* короткий предстарт: сначала выставляется камера */
            __atomic_store_n(&g_rt_mode, 2, __ATOMIC_RELEASE);
            b_s(&b, "Раздевалка: иду обратно по записанному пути ("); b_fix(&b, (double)t0 / 1000.0, 1); b_s(&b, " с). ");
            if (resumed) b_s(&b, "Продолжаю с того места пути, где вы стоите. ");
            {
                double sx = g_rt_startDev[0], sy = g_rt_startDev[1], sz = g_rt_startDev[2], sd = __builtin_sqrt(sx * sx + sy * sy + sz * sz);
                if (!resumed && g_rt_hasE && sd > 0.25) { b_s(&b, "Вы в "); b_fix(&b, sd, 2); b_s(&b, " м от точки, где остановили запись — возврат сместится на столько же. "); }
                dlog_i("раздевалка: старт возврата, смещение от точки конца записи, см: ", (long)(sd * 100.0));
            }
            b_s(&b, "Остановить: /route stop (или возьмитесь за стик)");
            chat_say(msg);
            return;
        }
        dressRec = 1;                                                /* новая запись — дальше общий код записи */
    }
    if ((w & 1) || dressRec) {                                       /* rec */
        if (g_rt_mode) { b_s(&b, "Маршрут: сначала /route stop"); chat_say(msg); return; }
        if (!g_mt_ctx) { b_s(&b, "Маршрут: нет главного потока для тиков (см. konstt.log)"); chat_say(msg); return; }
        if (rt_find() < 1) { b_s(&b, "Маршрут: стик не найден (HUD с экранным стиком не создан? см. konstt.log, /route probe)"); chat_say(msg); return; }
        g_rt_camRec = rt_cam_find(); g_rt_lc = g_rt_lv = 0;
        { int ck = g_rt_camRec ? rt_cam_check() : 1; if (ck == 0) { g_rt_camRec = 0; camBad = 1; } }
        if (dressRec && (!g_rt_camRec || g_rt_camMax - g_rt_camMin < 359.0f)) {   /* возврат задом наперёд держится на камере: нет камеры — нет смысла записывать */
            b_s(&b, "Раздевалка: камера не читается, не совпадает с реальной или имеет нестандартный диапазон (см. konstt.log, /route probe) — возврат без неё не работает, запись не начата");
            chat_say(msg); return;
        }
        g_rt_dress = dressRec; g_rt_rev = 0; g_rt_src = 0;
        g_rt_pn = 0; g_rt_ppi = 0;
        { int q, d, any = 0; for (q = 0; q < RT_NB; q++) if (rt_btn_read(q, &d)) any = 1; g_rt_btnRec = any; g_rt_lb = 0; g_rt_bdown = 0; }
        g_rt_n = 0; g_rt_started = 0; g_rt_hasS = rt_my_pos(me); g_rt_hasE = 0;
        if (g_rt_hasS) { g_rt_S[0] = me[0]; g_rt_S[1] = me[1]; g_rt_S[2] = me[2]; }
        g_rt_tickPending = 0;
        __atomic_store_n(&g_rt_mode, 1, __ATOMIC_RELEASE);
        if (dressRec) {
            b_s(&b, "Раздевалка: запись пути. Возьмитесь за левый стик и идите к метке раздевалки; у метки снова /route dress (или /route stop). ");
            b_s(&b, "Потом переоденьтесь и /route dress — вернусь по этому же пути. Не крутите камеру резко и не заходите в чужие меню по дороге. ");
            b_s(&b, g_rt_btnRec ? "Кнопки Space/Shift/C пишутся (спринт тоже)" : "Кнопки не читаются: спринт при возврате возьмётся по наклону стика");
            chat_say(msg);
            return;
        }
        b_s(&b, "Запись маршрута: возьмитесь за левый стик и идите. Конец: /route stop. ");
        b_s(&b, g_rt_camRec ? "Камера пишется вместе со стиком. " : camBad ? "Камера НЕ пишется: поворот в m_Aim не совпадает с реальной камерой (см. konstt.log), камеру при повторе ставьте сами. " : "Камера НЕ найдена: пишется только стик (при повторе камеру выставляйте сами). ");
        b_s(&b, g_rt_btnRec ? "Кнопки Space/Shift/C пишутся (спринт тоже)" : "Кнопки не читаются: спринт при повторе возьмётся по наклону стика");
        chat_say(msg);
        return;
    }
    if (w & 4) {                                                     /* play */
        int i;
        if (g_rt_mode) { b_s(&b, "Маршрут: сначала /route stop"); chat_say(msg); return; }
        if ((g_rt_n < 2 || g_rt_src == 2) && rt_load(RT_FILE) < 2) { b_s(&b, "Маршрут: записи нет. Сначала /route rec"); chat_say(msg); return; }   /* в памяти раздевалка — обычный маршрут берём из route.txt */
        g_rt_dress = 0; g_rt_rev = 0;
        if (!g_mt_ctx) { b_s(&b, "Маршрут: нет главного потока для тиков (см. konstt.log)"); chat_say(msg); return; }
        if (rt_find() < 1) { b_s(&b, "Маршрут: стик не найден (HUD с экранным стиком не создан? см. konstt.log, /route probe)"); chat_say(msg); return; }
        if (g_rt_hasS && !(w & 64) && rt_my_pos(me)) {
            double d = rt_dist2(me, g_rt_S);
            if (d > 2.0) {
                b_s(&b, "Маршрут: вы в "); b_fix(&b, d, 1); b_s(&b, " м от точки начала записи (");  b_pos(&b, g_rt_S);
                b_s(&b, "). Встаньте туда и повторите, или /route play force");
                chat_say(msg); return;
            }
        }
        g_rt_camOn = 0;
        if (g_rt_camRec && !(w & 128)) g_rt_camOn = rt_cam_find();
        if (g_rt_camOn && rt_cam_check() == 0) { g_rt_camOn = 0; camBad = 1; }       /* m_Aim — не настоящий поворот: писать в него нельзя, камера собьётся */
        g_rt_startDev[0] = g_rt_startDev[1] = g_rt_startDev[2] = 0;
        if (g_rt_hasS && rt_my_pos(me)) { g_rt_startDev[0] = me[0] - g_rt_S[0]; g_rt_startDev[1] = me[1] - g_rt_S[1]; g_rt_startDev[2] = me[2] - g_rt_S[2]; }
        g_rt_maxDev = 0; g_rt_maxDevT = 0; g_rt_ppi = 0; g_rt_lastDev = -1000; g_rt_lastDevLog = -1000;
        for (i = 0; i < 4; i++) { g_rt_wh[i][0] = 0; g_rt_wh[i][1] = 0; }
        g_rt_whi = 0; g_rt_down = 0; g_rt_bdown = 0; g_rt_tickPending = 0; g_rt_lastTk = 0; g_rt_dtEma = 55.0f; g_rt_lead = 0;
        g_rt_pi = 0; g_rt_wrote = 0; g_rt_bad = 0; g_rt_t0 = rt_now() + (g_rt_camOn ? 250 : 0);       /* с камерой — короткий предстарт: сначала выставляется она */
        __atomic_store_n(&g_rt_mode, 2, __ATOMIC_RELEASE);
        b_s(&b, "Повтор маршрута: "); b_fix(&b, (double)g_rt_s[g_rt_n - 1].t / 1000.0, 1); b_s(&b, " с. ");
        b_s(&b, g_rt_camOn ? "Камера выставляется сама. " : (g_rt_camRec ? (w & 128 ? "Камера — ваша (nocam). " : camBad ? "Камера в записи не совпадает с реальной (см. konstt.log): камеру не трогаю, смотрите туда же, куда при записи. " : "Камеру выставить не удалось: смотрите туда же, куда при записи. ") : "В записи нет камеры: смотрите туда же, куда при записи. "));
        {
            double sx = g_rt_startDev[0], sy = g_rt_startDev[1], sz = g_rt_startDev[2], sd = __builtin_sqrt(sx * sx + sy * sy + sz * sz);
            if (g_rt_hasS && sd > 0.25) { b_s(&b, "Вы в "); b_fix(&b, sd, 2); b_s(&b, " м от точки старта (Δ "); b_fix(&b, sx, 2); b_s(&b, ", "); b_fix(&b, sy, 2); b_s(&b, ", "); b_fix(&b, sz, 2); b_s(&b, ") — повтор сместится на столько же. "); }
            dlog_i("маршрут: старт повтора, смещение от точки начала записи, см: ", (long)(sd * 100.0));
        }
        b_s(&b, g_rt_leadArg >= 0 ? "Опережение вручную: " : "Опережение авто (на кадр вперёд). ");
        if (g_rt_leadArg >= 0) { b_int(&b, g_rt_leadArg); b_s(&b, " мс. "); }
        b_s(&b, "Остановить: /route stop (или возьмитесь за стик)");
        chat_say(msg);
        return;
    }
    chat_say("Маршрут: /route rec — запись, /route stop — стоп, /route play — повтор (/route play force — без проверки места), /route status, /route probe. Раздевалка: /route dress — запись пути к метке, у метки ещё раз /route dress, после переодевания /route dress — возврат назад (dress rec — заново, dress back — из файла)");
}

/* ───────────── самозапуск ───────────── */
/* Ждёт ядро, готовность игры и образец делегата, регистрирует /route (hm_cmd_install), потом запускает поток-«тикер». */
static void* rt_main(void* arg) {
    unsigned long t = 0;
    (void)arg;
    if (!hm_cmd_install("route", native_route_cmd)) return NULL;
    pthread_create(&t, NULL, rt_worker, NULL);
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void route_ctor(void) {
    unsigned long t = 0;
    pthread_create(&t, NULL, rt_main, NULL);
}
#endif
