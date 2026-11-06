/*
 * hassle_shaxta.c — необязательный мод «/shaxta»: показывает издалека, какие руды сейчас лежат в шахте.
 *
 * ТЕСТОВЫЙ ИНСТРУМЕНТ. Нужен только для настройки шахты на тестовых NPC / тестовом сервере (подобрать шансы руд в
 * g_mining_resources_info, miner_data.pwn). Это не игровая функция: ничего не делает за игрока, ничего не отправляет на сервер.
 * Только на вашем клиенте, сервер ничего не знает. Нужен, чтобы глазами увидеть, много ли золота / серебра / железа / угля
 * спавнится, и по этому подправить шансы на сервере (g_mining_resources_info в miner_data.pwn).
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_shaxta.c и пересоберите бандл — больше ничего править не нужно.
 * С /ids не связан (свой диапазон ID подписей 64100…64147, свой поток) — можно включать оба сразу.
 *
 * Команды в чате:
 *   /shaxta              включить ↔ выключить подписи над рудой
 *   /shaxta on | off     включить / выключить
 *   /shaxta r 400        радиус в метрах (10…1000), можно просто «/shaxta 400»
 *   /shaxta list         сводка в чат: сколько месторождений каждого вида сейчас видно + сколько появлений руды мод насчитал
 *   /shaxta reset        обнулить счётчик появлений
 *   /shaxta test         три цветные подписи рядом с вами (красная, зелёная, синяя) — проверка формата цвета
 *   /shaxta fmt 0|1|2|3  формат цвета: 0 — RGBA, 1 — ARGB, 2 — <color=#RRGGBB>, 3 — {RRGGBB}; выберите тот, где /shaxta test показывает
 *                        красный-зелёный-синий правильно, и впишите его в SHX_FMT_DEFAULT ниже
 *   /shaxta gold|silver|iron|coal|ground   показать/скрыть один вид (по-русски: золото серебро железо уголь земля);
 *                        несколько слов подряд («/shaxta gold silver») — показывать ТОЛЬКО их;  /shaxta all — показывать все виды
 *   /shaxta menu         МЕНЮ на экране (кнопка-ромб + панель): переключатели видов руды, радиус, счётчики. Работает на мобилке
 *   /shaxta fz [on|off]  ЗАМОРОЗКА персонажа (штатная заморозка игры, см. «Заморозка» ниже): без аргумента — переключить; «on» — заморозить где стоите, «off» — отпустить
 *                        (персонаж сам встанет на землю под точкой). Синонимы: freeze, фриз, заморозка
 *   /shaxta status       состояние;  /shaxta help — подсказка
 *
 * Кнопка «ТП» в меню (золото / серебро / железо / уголь): телепорт к руде через обычный «/tpl to X Y Z» (hassle_tpl.c должен быть в бандле; сам мод от него
 *   не зависит — меню просто шлёт чат-команду, как если бы вы набрали её руками). Каждое нажатие — следующая руда этого вида из тех, что клиент сейчас видит
 *   (по порядку ID, в пределах зоны стрима ~300 м); на кнопке видно «ТП 2/5». Встаёте у руды на SHX_TP_BACK м в вашу сторону, Transform на SHX_TP_DY ниже точки
 *   объекта (так сервер видит игрока ровно в точке руды). Если оказались в стене — поправьте SHX_TP_BACK или вернитесь «/tpl pos».
 *
 * Переключатель «Автоцикл шахты» (JS меню; C только сообщает, показан ли маркер сервера — поле k в состоянии): шаг проверки 120 мс; клик по меню взаимодействий —
 *   сразу как оно появилось, но не чаще раза в 1.05 с (серверный интервал, interactions.pwn OnInteractionsClick, считает в целых секундах gettime). Порядок: идёт кружок — ждём (автонажатие включается вместе с циклом); есть меню
 *   взаимодействий — кликаем первую строку (не больше 2 раз подряд, чтобы не застрять у места, где делать нечего); иначе через 0.5 с покоя (после ТП ещё +0.45 с, пока сервер засчитает вход в зону; сразу после кружка — принудительный sync, чтобы маркер был свежим) — «/tpl gps go» к маркеру
 *   сервера, а если маркера нет (после перфоратора сервер его не ставит) — «ТП» к руде того вида, который вы нажимали последним. Маркер ведёт сам: стойка после
 *   каждого этапа (перфоратор → кирка → тележка → лопата), потом месторождение, потом вагонетка/склад — отдельно проверять, какой инструмент в руках, не нужно:
 *   стойка выдаёт нужный сама, а сервер показывает пункт «взять инструмент» только когда рук свободны (Interactions:IsAccess, MINER_TOOLS_INTERACT).
 *
 * Заморозка в АВТОЦИКЛЕ (только в нём; ручные кнопки «ТП» не трогаем): раньше после «/tpl to» к руде персонаж иногда проваливался под пол, и проверять шахту было нельзя. Причина — точка ТП:
 *   Transform ставится на SHX_TP_DY (1 м) НИЖЕ точки объекта, чтобы сервер видел игрока ровно в точке руды (в пакет уходит Transform.y + 1), а точка руды лежит на уровне пола;
 *   капсула оказывается в полу, физика её выталкивает / роняет, и сервер (miner.pwn: IsPlayerInRangeOfPoint(MINER_RADIUS_SPOT * 2.0), область взаимодействия 1.5 м — оба в 3D)
 *   уже не видит игрока у руды. Теперь перед каждым ТП автоцикла мод замораживает персонажа штатным Entity.ToggleFreeze("shaxta", true, accumulated=false) — тем же механизмом, которым игра
 *   замораживает персонажей (Actor.ToggleFreeze → CharacterMovement.IsFreezed; пока заморожен, Character.Move только обнуляет скорость и физику/гравитацию не считает).
 *   Позиция остаётся ровно там, куда поставил SetPosition, и уходит на сервер как обычная. Серверная заморозка (TogglePlayerControllable на этапах работы) — отдельный тип в
 *   списке Entity.m_FreezeTypes, они не мешают друг другу. Отпускает заморозку: остановка автоцикла (выключатель или ручное управление — джойстик / кнопка / клавиша),
 *   «/shaxta fz off». Перед отпусканием мод ставит персонажа на землю под точкой (Player.SetPosition(pos + SHX_TP_DY, findHeight=true), как «/air land»),
 *   иначе после разморозки он бы всё равно провалился. Не проверено на устройстве: если «/shaxta fz» пишет «ToggleFreeze не найден» — смотрите konstt.log.
 *
 * Автоцикл сам выключается, если вы взяли управление: тронули джойстик, нажали любую экранную кнопку (кроме вращения камеры) или клавишу — тогда выключается и автонажатие, если его включил цикл (хук window.onScreenControlTouchStart/Move в JS меню, C не участвует).
 * Переключатель «Автонажатие кружка» (только JS меню, C не участвует): опрос раз в 0.1 с: первый клик — сразу как кружок появился, дальше раз в 0.76 с, шлёт серверу OnProgressBarClick (то же событие, что тап по кружку на телефоне)
 *   для кружков с подписью «Нажимайте Y …» — это этап кирки. Бурение и лопата сервер заполняет сам по таймеру, их нажимать не нужно. Быстрее 0.7 с нельзя:
 *   сервер (progress_bar.pwn, OnKeyPress) считает нажатие чаще 700 мс ошибкой — шкала откатывается, а после 5 ошибок этап срывается.
 *
 * Меню (как оно устроено): мод сам внедряет JS в веб-интерфейс игры через WebViewSystem.ExecuteJavaScript (APK пересобирать не нужно).
 *   Нажатия меню уходят как «/shaxta q …» через window.sendChatInput (q — «тихо», без сообщений в чат), актуальное состояние мод
 *   присылает обратно тем же ExecuteJavaScript. Текст меню — между метками SHX_MENU_BEGIN/END; править shaxta_menu.js и запускать
 *   embed_shaxta_menu.py. Кнопка появляется после первой команды /shaxta и лежит поверх HUD (тянется пальцем).
 *
 * Модели руд (из серверного miner_data.pwn): 17106 уголь/пустое месторождение, 17166 золото, 17164 серебро, 17165 железо,
 * 17107 выбитая руда на земле. Если на клиенте номера другие — встаньте рядом с рудой, «/ids obj» покажет номер модели в [скобках],
 * и поправьте таблицу g_shx_types ниже.
 *
 * Как это работает (по дампу IL2CPP от 20.10.2026; приём тот же, что в hassle_ids.c)
 *   • Объекты берём из Pools.Objects (MapObjectPlace: ID, Model, get_Position()). Игра знает только объекты, которые сервер
 *     сейчас «стримит» клиенту (на сервере stream_distance 300), поэтому дальше ~300 м руду не видно в принципе.
 *   • Подписи рисует сама игра: Pools.Text3DLables.Create(CreateText3DParams), TestLOS=0 (видно сквозь стены).
 *   • Счётчик появлений: для каждой точки (позиция с точностью 1 м) помним последнюю модель; смена модели или появление объекта
 *     после его исчезновения = «новая руда». Точка считается пропавшей, только если вы рядом (SHX_GONE_R), а объекта нет.
 *     Считать можно только то, что вы видели: стойте в шахте, и счётчик копится сам, пока подписи включены.
 *   • Всё Unity — только в главном потоке: фоновый поток шлёт Post в UnitySynchronizationContext (как /ids и /air).
 */
#pragma once
#include "hassle_core.c"

#define SHX_FMT_DEFAULT 0                /* формат цвета подписей: 0 RGBA, 1 ARGB, 2 <color=#>, 3 {RRGGBB} — подберите по /shaxta test */
#define SHX_MAXL 44                      /* максимум подписей руды одновременно */
#define SHX_TESTN 3                      /* тестовые подписи */
#define SHX_BASE 64100                   /* первый ID подписи мода (у /ids — 64000…64047) */
#define SHX_RANGE (SHX_MAXL + SHX_TESTN) /* размер диапазона ID */
#define SHX_TICK_US 1000000              /* период обновления */
#define SHX_NEWPERTICK 16                /* не больше стольких новых подписей за тик */
#define SHX_MOVE 1.5f                    /* на сколько метров сдвинулся объект — переставить подпись */
#define SHX_GONE_R 80.0f                 /* «точка пропала» засчитывается, только если вы ближе этого радиуса */
#define SHX_PMAX 128                     /* сколько точек отслеживаем для счётчика */
#define SHX_NT 5

typedef struct { int model; const char* name; unsigned rgb; } ShxType;
static const ShxType g_shx_types[SHX_NT] = {
    { 17166, "ЗОЛОТО",         0xFFD700 },
    { 17164, "СЕРЕБРО",        0xD8E2F0 },
    { 17165, "ЖЕЛЕЗО",         0xFF8030 },
    { 17106, "УГОЛЬ",          0x9A9A9A },
    { 17107, "РУДА НА ЗЕМЛЕ",  0x66FF66 }
};

typedef struct { int tid, id, model; float d; float p[3]; } ShxC;
typedef struct { int used, id, model; float p[3]; } ShxSlot;
typedef struct { float p[3]; int tid, seen; } ShxPos;

static ShxC g_shx_c[SHX_MAXL]; static int g_shx_nc; static float g_shx_ref[3];
static ShxSlot g_shx_slot[SHX_MAXL];
/* точки для кнопки «ТП»: все видимые клиенту руды видов 0…3 (золото, серебро, железо, уголь), порядок по ID объекта — стабильный (по расстоянию он
   менялся бы после каждого прыжка, и кнопка ходила бы туда-сюда). Радиус и скрытые в меню виды не учитываются. */
#define SHX_TPK 12                       /* точек на вид */
#define SHX_TP_DY 1.0f                   /* Transform игрока на столько НИЖЕ точки объекта (сервер видит игрока на Transform.y + 1, как у /tpl pick) */
#define SHX_TP_BACK 0.8f                 /* встать на столько метров от руды в сторону, где вы стоите (руда может быть в стене); 0 — прямо в точку */
#define SHX_TP_NEAR 8.0f                 /* сдвиг в вашу сторону — только если вы ближе (по горизонтали); дальше направление «к вам» может вести сквозь скалу */
typedef struct { int id; float p[3]; } ShxTp;
static ShxTp g_shx_tp[4][SHX_TPK]; static int g_shx_ntp[4];
static int g_shx_mark;                   /* 1 — сервер сейчас показывает маркер (см. shx_marker_active) */
#define SHX_RACKS 6                      /* стоек в списке (ближайшие) */
static float g_shx_rack[SHX_RACKS][3]; static int g_shx_nrack;
static void shx_tp_target(const float* p, float* q);    /* точка, куда ставить игрока у объекта p (ниже) */
static int g_shx_present[SHX_RANGE];
static int g_shx_total[SHX_NT];          /* сколько объектов каждого вида в пуле при последнем скане (весь стрим, не только радиус) */
static int g_shx_inrad[SHX_NT];          /* то же в радиусе */
static int g_shx_poolN = -1;             /* всего объектов в пуле Objects (-1 — не прочитался) */
static ShxPos g_shx_pos[SHX_PMAX]; static int g_shx_np; static int g_shx_spawn[SHX_NT];
static volatile int g_shx_on, g_shx_pending; static long long g_shx_pendT;
static int g_shx_thread, g_shx_modId = -1, g_shx_errs, g_shx_logged, g_shx_testOn;
static int g_shx_fmt = SHX_FMT_DEFAULT;
static float g_shx_radius = 300.0f;
static int g_shx_show = 31;              /* маска видов, которые рисуем: бит i = g_shx_types[i] (счётчик считает все виды всегда) */
static int g_shx_q;                      /* 1 — команда пришла из меню («/shaxta q …»): в чат не пишем */
static int g_shx_uiOk, g_shx_uiTick;     /* JS меню внедрён / счётчик тиков для периодического повторного внедрения */
static char g_shx_lastJs[2000];           /* последнее отправленное в меню состояние — одинаковое повторно не шлём */
static int g_shx_layoutOk, g_shx_oID, g_shx_oCol, g_shx_oPos, g_shx_oDist, g_shx_oLos, g_shx_oPl, g_shx_oVeh, g_shx_oTxt, g_shx_ssize;

extern int clock_gettime(int, void*);        /* как в mem/route/tpl/air: struct timespec = два long */
static long long shx_now_ms(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return (long long)ts[0] * 1000LL + ts[1] / 1000000LL; }
static int shx_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
static float shx_dist(const float* a, const float* b) { double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2]; return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz); }
static int shx_tid(int model) { int i; for (i = 0; i < SHX_NT; i++) if (g_shx_types[i].model == model) return i; return -1; }
static int shx_num(const char* t, float* out) {          /* «12», «12.5», «12,5» (без знака) */
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
    *out = v; return 1;
}
static void shx_err(const char* what, void* exc) {
    char et[200];
    if (g_shx_errs >= 6) return;
    g_shx_errs++;
    exc_text(exc, et, sizeof et);
    dlog2(what, et);
}


static int shx_myref(float* out);
static void shx_collect(void);
static void shx_say(const char* s) { if (!g_shx_q) chat_say(s); }    /* информационные сообщения; ошибки всегда идут в чат через chat_say */

/* ───────────── меню на экране: внедрение JS и обмен состоянием ───────────── */
/* SHX_MENU_BEGIN */
static const char g_shx_menu_js[] =
"(function(){\n"
"if(window.__shx)return;\n"
"var T=[\n"
"{k:'gold',n:'Золото',c:'#FFD700'},\n"
"{k:'silver',n:'Серебро',c:'#D8E2F0'},\n"
"{k:'iron',n:'Железо',c:'#FF8030'},\n"
"{k:'coal',n:'Уголь',c:'#9A9A9A'},\n"
"{k:'ground',n:'Руда на земле',c:'#66FF66'}];\n"
"var S={o:0,r:300,m:31,c:[0,0,0,0,0],s:[0,0,0,0,0],p:0,k:0,z:0,rk:[],t:[[],[],[],[]]},ix=[0,0,0,0],cycTgt=0;\n"
"var open=false,dragged=false,syncT=null,fzHeld=false;\n"
"function cmd(x){try{window.sendChatInput('/shaxta q '+x);}catch(e){}}\n"
"function frz(v){fzHeld=!!v;cmd('fz '+(v?1:0));}\n"
"function tp(c){if(cyc)frz(1);try{window.sendChatInput(c);}catch(e){}}\n"
"function goPt(i){var a=S.t[i];if(!a||!a.length)return 0;var k=ix[i]%a.length;ix[i]=k+1;var q=a[k];tp('/tpl to '+q[0]+' '+q[1]+' '+q[2]);return 1;}\n"
"function go(i){cycTgt=i;goPt(i);}\n"
"function pickTgt(){var o=[cycTgt,0,1,2,3];for(var j=0;j<o.length;j++){var a=S.t[o[j]];if(a&&a.length)return o[j];}return -1;}\n"
"function el(tag,id,cls,txt){var e=document.createElement(tag);if(id)e.id=id;if(cls)e.className=cls;if(txt!==undefined)e.textContent=txt;return e;}\n"
"var css='#shx-fab{position:fixed;right:1.4vh;top:34vh;width:6.6vh;height:6.6vh;border-radius:50%;background:rgba(12,10,6,.82);border:0.35vh solid #ffb110;z-index:999990;pointer-events:auto;display:flex;align-items:center;justify-content:center}'\n"
"+'#shx-fab.on{background:rgba(255,177,16,.9)}'\n"
"+'#shx-fab svg{width:3.6vh;height:3.6vh}'\n"
"+'#shx-panel{position:fixed;left:50%;top:50%;margin-left:-30vh;margin-top:-37vh;width:60vh;background:rgba(12,10,6,.95);border:0.3vh solid #ffb110;border-radius:1.6vh;z-index:999991;pointer-events:auto;color:#eeeee6;font-family:\"Open Sans\",sans-serif;display:none;padding:1.2vh 1.6vh 1.4vh 1.6vh;box-sizing:border-box}'\n"
"+'#shx-panel.show{display:block}'\n"
"+'.shx-h{display:flex;align-items:center;justify-content:space-between;height:6vh}'\n"
"+'.shx-t{font-size:2.9vh;font-weight:700;color:#ffb110}'\n"
"+'.shx-sub{font-size:1.7vh;color:rgba(238,238,230,.55);margin:-0.4vh 0 0.8vh 0}'\n"
"+'.shx-x{width:5.6vh;height:5.6vh;border-radius:50%;background:rgba(255,255,255,.1);display:flex;align-items:center;justify-content:center;font-size:3.4vh;line-height:3.4vh;color:#eeeee6}'\n"
"+'.shx-row{display:flex;align-items:center;justify-content:space-between;height:7.6vh;border-top:0.2vh solid rgba(255,255,255,.1)}'\n"
"+'.shx-row.off .shx-l{opacity:.38}'\n"
"+'.shx-l{display:flex;flex-direction:column;justify-content:center}'\n"
"+'.shx-n{font-size:2.6vh;font-weight:700}'\n"
"+'.shx-s{font-size:1.9vh;color:rgba(238,238,230,.62);margin-top:0.3vh}'\n"
"+'.shx-sw{width:8.4vh;height:4.6vh;border-radius:2.3vh;background:rgba(255,255,255,.18);position:relative;flex-shrink:0}'\n"
"+'.shx-sw i{position:absolute;top:0.5vh;left:0.5vh;width:3.6vh;height:3.6vh;border-radius:50%;background:#eeeee6}'\n"
"+'.shx-sw.on{background:#ffb110}'\n"
"+'.shx-sw.on i{left:4.3vh;background:#0c0a06}'\n"
"+'.shx-st{display:flex;align-items:center}'\n"
"+'.shx-b{min-width:5.6vh;height:5.2vh;border-radius:1.2vh;background:rgba(255,255,255,.12);display:flex;align-items:center;justify-content:center;font-size:3vh;font-weight:700;padding:0 1vh;box-sizing:border-box}'\n"
"+'.shx-v{min-width:11vh;text-align:center;font-size:2.7vh;font-weight:700}'\n"
"+'.shx-f{display:flex;align-items:center;justify-content:space-between;border-top:0.2vh solid rgba(255,255,255,.1);padding-top:1vh;height:7vh;box-sizing:border-box}'\n"
"+'.shx-hint{font-size:1.7vh;color:rgba(238,238,230,.55);flex:1;margin-right:1.2vh}'\n"
"+'.shx-ctl{display:flex;align-items:center}'\n"
"+'.shx-tp{min-width:6.4vh;height:4.6vh;margin-right:1.2vh;border-radius:1.2vh;background:rgba(255,177,16,.22);border:0.2vh solid #ffb110;color:#ffb110;display:flex;align-items:center;justify-content:center;font-size:2.1vh;font-weight:700;padding:0 1vh;box-sizing:border-box}'\n"
"+'.shx-tp.dis{opacity:.3}'\n"
"+'.shx-rs{background:rgba(226,85,68,.22);color:#ffb3a8;font-size:2.1vh;font-weight:700}';\n"
"var st=el('style');st.textContent=css;document.head.appendChild(st);\n"
"var fab=el('div','shx-fab','');\n"
"fab.innerHTML='<svg viewBox=\"0 0 24 24\"><path d=\"M12 2L3 9l9 13 9-13z\" fill=\"#ffb110\" stroke=\"#0c0a06\" stroke-width=\"1.4\" stroke-linejoin=\"round\"/><path d=\"M3 9h18M8 9l4-7 4 7M8 9l4 13 4-13\" fill=\"none\" stroke=\"#0c0a06\" stroke-width=\"1.1\" stroke-linejoin=\"round\"/></svg>';\n"
"var panel=el('div','shx-panel','');\n"
"var hd=el('div',null,'shx-h');hd.appendChild(el('div',null,'shx-t','Шахта: руды (тест)'));\n"
"var xb=el('div',null,'shx-x','\\u00d7');hd.appendChild(xb);panel.appendChild(hd);\n"
"panel.appendChild(el('div',null,'shx-sub','Тест шахты. «ТП» — телепорт к руде (через /tpl to), каждое нажатие — следующая'));\n"
"function mkSw(){var s=el('div',null,'shx-sw');s.appendChild(el('i'));return s;}\n"
"var mrow=el('div',null,'shx-row');var ml=el('div',null,'shx-l');ml.appendChild(el('div',null,'shx-n','Подписи в мире'));\n"
"var msub=el('div',null,'shx-s','');ml.appendChild(msub);mrow.appendChild(ml);var msw=mkSw();mrow.appendChild(msw);panel.appendChild(mrow);\n"
"var rows=[];\n"
"for(var i=0;i<T.length;i++){(function(i){\n"
"var r=el('div',null,'shx-row');var l=el('div',null,'shx-l');var n=el('div',null,'shx-n',T[i].n);n.style.color=T[i].c;\n"
"var s=el('div',null,'shx-s','');l.appendChild(n);l.appendChild(s);r.appendChild(l);var sw=mkSw();var ctl=el('div',null,'shx-ctl');var tp=null;\n"
"if(i<4){tp=el('div',null,'shx-tp','ТП');tp.addEventListener('click',function(e){e.stopPropagation();go(i);});ctl.appendChild(tp);}\n"
"ctl.appendChild(sw);r.appendChild(ctl);\n"
"r.addEventListener('click',function(){cmd(T[i].k);});\n"
"panel.appendChild(r);rows.push({r:r,s:s,sw:sw,tp:tp});})(i);}\n"
"var rr=el('div',null,'shx-row');rr.appendChild(el('div',null,'shx-n','Радиус'));\n"
"var rc=el('div',null,'shx-st');var bm=el('div',null,'shx-b','\\u2212');var rv=el('div',null,'shx-v','300 м');var bp=el('div',null,'shx-b','+');\n"
"rc.appendChild(bm);rc.appendChild(rv);rc.appendChild(bp);rr.appendChild(rc);panel.appendChild(rr);\n"
"var ar=el('div',null,'shx-row');var al=el('div',null,'shx-l');al.appendChild(el('div',null,'shx-n','Автонажатие кружка'));\n"
"var asub=el('div',null,'shx-s','выкл');al.appendChild(asub);ar.appendChild(al);var asw=mkSw();ar.appendChild(asw);panel.appendChild(ar);\n"
"var cr=el('div',null,'shx-row');var cl=el('div',null,'shx-l');cl.appendChild(el('div',null,'shx-n','Автоцикл шахты'));\n"
"var csub=el('div',null,'shx-s','выкл · цель — последняя нажатая руда');cl.appendChild(csub);cr.appendChild(cl);var csw=mkSw();cr.appendChild(csw);panel.appendChild(cr);\n"
"var ft=el('div',null,'shx-f');var hint=el('div',null,'shx-hint','');var rs=el('div',null,'shx-b shx-rs','Сбросить счётчик');\n"
"ft.appendChild(hint);ft.appendChild(rs);panel.appendChild(ft);\n"
"document.body.appendChild(fab);document.body.appendChild(panel);\n"
"function pct(a,b){return b>0?Math.round(a*1000/b)/10:0;}\n"
"function render(){\n"
"var tot=0,sp=0,i;for(i=0;i<5;i++){tot+=S.c[i];sp+=S.s[i];}\n"
"fab.className=S.o?'on':'';\n"
"msw.className='shx-sw'+(S.o?' on':'');\n"
"msub.textContent=S.o?'включены':'выключены';\n"
"for(i=0;i<5;i++){var on=((S.m>>i)&1)===1;rows[i].r.className='shx-row'+(on?'':' off');rows[i].sw.className='shx-sw'+(on?' on':'');\n"
"if(rows[i].tp){var nt=S.t[i]?S.t[i].length:0;rows[i].tp.className='shx-tp'+(nt?'':' dis');rows[i].tp.textContent=nt?'ТП '+(ix[i]%nt+1)+'/'+nt:'ТП';}\n"
"rows[i].s.textContent='сейчас '+S.c[i]+' \\u00b7 появлений '+S.s[i]+(sp>0?' ('+pct(S.s[i],sp)+'%)':'');}\n"
"rv.textContent=S.r+' м';\n"
"hint.textContent=S.p?(tot>0?'дальше ~300 м сервер руду не отдаёт':'руды рядом нет: зайдите в шахту'):'данные не прочитались (вы не в игре?)';\n"
"}\n"
"function setOpen(v){open=v;panel.className=v?'show':'';\n"
"if(syncT){clearInterval(syncT);syncT=null;}\n"
"if(v){cmd('sync');syncT=setInterval(function(){if(!S.o)cmd('sync');},2000);}}\n"
"fab.addEventListener('click',function(){if(dragged){dragged=false;return;}setOpen(!open);});\n"
"xb.addEventListener('click',function(){setOpen(false);});\n"
"msw.parentNode.addEventListener('click',function(){cmd(S.o?'off':'on');});\n"
"bm.addEventListener('click',function(){cmd('r '+Math.max(50,S.r-50));});\n"
"bp.addEventListener('click',function(){cmd('r '+Math.min(1000,S.r+50));});\n"
"rs.addEventListener('click',function(){cmd('reset');});\n"
"var auto=false,autoT=null,autoN=0,keyLast=0,KEY_MS=760;\n"
"function ev(n,a){try{window.sendClientEventHandle(0,n,a);return 1;}catch(e){}try{window.sendClientEvent(gm.EVENT_EXECUTE_PUBLIC,{ignoreChat:true},n,a);return 1;}catch(e){}return 0;}\n"
"function bars(){try{var n=document.querySelectorAll('.preloader-list').length;if(n)return n;if(window.getInterfaceStatus('ProgressBar')){var v=window.interface('ProgressBar');if(v&&v.list)return v.list.length;}}catch(e){}return 0;}\n"
"function intr(){try{var d=document.querySelectorAll('.interactions-list').length;if(!d&&!window.getInterfaceStatus('Interactions'))return null;var v=window.interface('Interactions');if(v&&v.list&&v.list.length)return v.list;}catch(e){}return null;}\n"
"function autoTick(){try{var n=bars();if(!n)return;var now=Date.now();if(now-keyLast<KEY_MS)return;keyLast=now;for(var i=0;i<n;i++){ev('OnProgressBarClick',i);autoN++;}asub.textContent='вкл · нажатий '+autoN;}catch(e){}}\n"
"function setAuto(v){if(v===auto)return;auto=v;asw.className='shx-sw'+(auto?' on':'');\n"
"if(autoT){clearInterval(autoT);autoT=null;}if(auto){autoN=0;asub.textContent='вкл · нажатий 0';keyLast=0;autoT=setInterval(autoTick,100);}else asub.textContent='выкл';}\n"
"ar.addEventListener('click',function(){setAuto(!auto);});\n"
"var cyc=false,cycT=null,cycLast=0,clk=0,phase='rack',fails=0,rix=0,rkWait=false,clkLast=0,syncLast=0,hadBar=0,autoByCyc=false;\n"
"var UI_CD=1050,IDLE_MS=500,SETTLE_MS=450,SYNC_MS=250,CYC_MS=120;\n"
"function toRack(){var a=S.rk;if(!a||!a.length){csub.textContent='стоек не видно (подойдите к шахте)';return;}\n"
"if(rkWait)rix++;rkWait=true;var q=a[rix%a.length];csub.textContent='к стойке '+(rix%a.length+1)+'/'+a.length;\n"
"tp('/tpl to '+q[0]+' '+q[1]+' '+q[2]);}\n"
"function cycTick(){try{var now=Date.now();if(now-syncLast>=SYNC_MS){cmd('sync');syncLast=now;}\n"
"if(bars()){hadBar=1;cycLast=now;clk=0;fails=0;phase='ore';csub.textContent='кружок идёт';return;}\n"
"if(hadBar){hadBar=0;cmd('sync');syncLast=now;cycLast=now;}\n"
"var L=intr();\n"
"if(L){if(clk<2){if(now-clkLast<UI_CD)return;var tt=L[0].title;ev('OnInteractionsClick',L[0].type);clk++;clkLast=now;cycLast=now;rkWait=false;csub.textContent='взаимодействие: '+String(tt).replace(/<[^>]*>/g,' ');return;}}else clk=0;\n"
"if(now-cycLast<IDLE_MS)return;cycLast=now+SETTLE_MS;clk=0;\n"
"if(S.k){tp('/tpl gps go');csub.textContent='к маркеру сервера';return;}\n"
"if(phase==='ore'&&fails<2){var ti=pickTgt();if(ti<0){csub.textContent='руды рядом нет: подойдите к месторождениям';return;}\n"
"fails++;goPt(ti);csub.textContent='к руде: '+T[ti].n+(ti===cycTgt?'':' (запасная)')+' ('+fails+')';}\n"
"else{phase='ore';fails=0;toRack();}}catch(e){csub.textContent='ошибка: '+e;}}\n"
"function cycSet(v,why){if(v===cyc)return;cyc=v;csw.className='shx-sw'+(cyc?' on':'');\n"
"if(cycT){clearInterval(cycT);cycT=null;}\n"
"if(cyc){autoByCyc=!auto;setAuto(true);clk=0;cycLast=0;clkLast=0;syncLast=0;hadBar=0;phase='rack';fails=0;rkWait=false;csub.textContent='вкл';cycT=setInterval(cycTick,CYC_MS);}\n"
"else{if(fzHeld)frz(0);if(why&&autoByCyc)setAuto(false);autoByCyc=false;csub.textContent=why||'выкл · цель — последняя нажатая руда';}}\n"
"cr.addEventListener('click',function(){cycSet(!cyc);});\n"
"function manual(src){if(cyc)cycSet(false,'стоп: ручное управление ('+src+')');}\n"
"function hookCtl(n,onlyStick){var f=window[n];if(typeof f!=='function'||f.__shx)return;var w=function(p){try{if(cyc&&typeof p==='string'&&p.indexOf('<Mouse>/')!==0&&(!onlyStick||p==='<Gamepad>/leftStick'))manual(p==='<Gamepad>/leftStick'?'джойстик':'кнопка');}catch(e){}return f.apply(this,arguments);};w.__shx=1;window[n]=w;}\n"
"hookCtl('onScreenControlTouchStart',false);hookCtl('onScreenControlTouchMove',true);\n"
"window.addEventListener('keydown',function(e){try{if(!cyc)return;var t=e.target,tg=t&&t.tagName;if(tg==='INPUT'||tg==='TEXTAREA'||(t&&t.isContentEditable))return;manual('клавиша');}catch(x){}},true);\n"
"var dx=0,dy=0,sx=0,sy=0,tch=false;\n"
"fab.addEventListener('touchstart',function(e){var t=e.touches[0];sx=t.clientX;sy=t.clientY;var b=fab.getBoundingClientRect();dx=t.clientX-b.left;dy=t.clientY-b.top;tch=true;dragged=false;});\n"
"fab.addEventListener('touchmove',function(e){if(!tch)return;var t=e.touches[0];if(Math.abs(t.clientX-sx)+Math.abs(t.clientY-sy)<12&&!dragged)return;dragged=true;\n"
"fab.style.right='auto';fab.style.left=Math.max(0,Math.min(window.innerWidth-fab.offsetWidth,t.clientX-dx))+'px';fab.style.top=Math.max(0,Math.min(window.innerHeight-fab.offsetHeight,t.clientY-dy))+'px';});\n"
"fab.addEventListener('touchend',function(){tch=false;});\n"
"window.__shx={u:function(s){for(var k in s){S[k]=s[k];}render();},o:function(w){if(w)setOpen(true);}};\n"
"render();\n"
"})();\n"
;
/* SHX_MENU_END */
static int shx_js_run(const char* js) {                             /* WebViewSystem.ExecuteJavaScript(js) — только из главного потока */
    void *k, *m, *exc = NULL, *args[1];
    if (!g_safe || !il2cpp_light() || !g_img) return 0;
    ensure_attached();
    k = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    m = k ? find_method(k, "ExecuteJavaScript", 1) : NULL;
    if (!m) { if (g_shx_errs < 6) { g_shx_errs++; dlog("shaxta: нет WebViewSystem.ExecuteJavaScript(string)"); } return 0; }
    args[0] = A.string_new(js);
    if (!args[0]) return 0;
    A.runtime_invoke(m, NULL, args, &exc);
    if (exc) { shx_err("shaxta: ExecuteJavaScript бросил исключение: ", exc); return 0; }
    return 1;
}
static void shx_ui_inject(void) {                                   /* идемпотентно: если меню уже есть на странице, JS сразу выходит */
    if (shx_js_run(g_shx_menu_js)) { g_shx_uiOk = 1; g_shx_lastJs[0] = 0; }
}
/* ───────────── заморозка персонажа (см. шапку) ───────────── */
#define SHX_FZ_TYPE "shaxta"             /* имя типа заморозки в Entity.m_FreezeTypes (серверная заморозка — свои типы, не пересекаются) */
#define SHX_FZ_LAND 1                    /* 1 — перед отпусканием поставить персонажа на землю под точкой (иначе после разморозки он провалится) */
static int g_shx_fz;                     /* 1 — мы держим заморозку */
static int g_shx_fzErr;
static int shx_is_frozen(void) {         /* Entity.IsFreezed (список типов не пуст: наш, серверный TogglePlayerControllable и т.д.); 0 — нет / не прочиталось */
    void *me, *m, *exc = NULL, *r;
    if (!g_safe || !il2cpp_light()) return 0;
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) return 0;
    m = find_method(A.object_get_class(me), "get_IsFreezed", 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, me, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 1)) return 0;
    return *((u8*)r + HDR) ? 1 : 0;
}
static int shx_set_pos(void* me, const float* pos, u8 findH) {      /* Player.SetPosition(Vector3, findHeight) — как в /tpl и /air land */
    void *m, *exc = NULL, *args[2]; float v[3]; char et[200];
    v[0] = pos[0]; v[1] = pos[1]; v[2] = pos[2];
    m = find_method(A.object_get_class(me), "SetPosition", 2);
    if (!m) return 0;
    args[0] = v; args[1] = &findH;
    A.runtime_invoke(m, me, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); if (g_shx_fzErr < 6) { g_shx_fzErr++; dlog2("shaxta: SetPosition бросил исключение: ", et); } return 0; }
    return 1;
}
/* on=1 — заморозить (идемпотентно: accumulated=false не даёт добавить тип дважды); on=0 — отпустить (сначала на землю под точкой). 1 — вызов прошёл */
static int shx_freeze(int on) {
    void *me, *m, *exc = NULL, *args[3]; u8 st = on ? 1 : 0, acc = 0; char et[200];
    if (!g_safe || !il2cpp_light()) return 0;
    ensure_attached();
    me = local_player();
    if (!me || !hm_obj_ok(me)) { g_shx_fz = 0; return 0; }          /* не в игре / персонаж пересоздан — заморозки уже нет */
    m = find_method(A.object_get_class(me), "ToggleFreeze", 3);     /* Actor.ToggleFreeze(string type, bool status, bool accumulated) */
    if (!m) { if (g_shx_fzErr < 6) { g_shx_fzErr++; dlog("shaxta: нет Entity.ToggleFreeze(string,bool,bool)"); } return 0; }
    if (!on && g_shx_fz && SHX_FZ_LAND) {
        float p[3];
        if (shx_myref(p)) { p[1] += SHX_TP_DY; shx_set_pos(me, p, 1); }   /* от точки объекта ищем пол вниз; SetPosition асинхронный, наш тип снимаем сразу — он держит свою заморозку сам */
    }
    args[0] = A.string_new(SHX_FZ_TYPE); args[1] = &st; args[2] = &acc;
    if (!args[0]) return 0;
    A.runtime_invoke(m, me, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); if (g_shx_fzErr < 6) { g_shx_fzErr++; dlog2("shaxta: ToggleFreeze бросил исключение: ", et); } return 0; }
    g_shx_fz = on ? 1 : 0;
    return 1;
}

static void shx_ui_push(int force) {                                /* состояние -> меню: window.__shx.u({...}) */
    char js[2000]; Buf b; int i, k, n;
    if (!g_shx_uiOk) return;
    b_init(&b, js, sizeof js);
    b_s(&b, "window.__shx&&window.__shx.u({o:"); b_int(&b, g_shx_on ? 1 : 0);
    b_s(&b, ",r:"); b_fix(&b, g_shx_radius, 0);
    b_s(&b, ",m:"); b_int(&b, g_shx_show);
    b_s(&b, ",p:"); b_int(&b, g_shx_poolN >= 0 ? 1 : 0);
    b_s(&b, ",c:[");
    for (i = 0; i < SHX_NT; i++) { if (i) b_ch(&b, ','); b_int(&b, g_shx_total[i]); }
    b_s(&b, "],s:[");
    for (i = 0; i < SHX_NT; i++) { if (i) b_ch(&b, ','); b_int(&b, g_shx_spawn[i]); }
    b_s(&b, "],k:"); b_int(&b, g_shx_mark); b_s(&b, ",z:"); b_int(&b, shx_is_frozen()); b_s(&b, ",t:[");
    for (i = 0; i < 4; i++) {                                       /* куда ставить игрока: точка руды, Transform на SHX_TP_DY ниже, чуть в вашу сторону */
        if (i) b_ch(&b, ',');
        b_ch(&b, '[');
        for (n = 0; n < g_shx_ntp[i]; n++) {
            float q[3]; shx_tp_target(g_shx_tp[i][n].p, q);
            if (n) b_ch(&b, ',');
            b_ch(&b, '['); b_fix(&b, q[0], 1); b_ch(&b, ','); b_fix(&b, q[1], 1); b_ch(&b, ','); b_fix(&b, q[2], 1); b_ch(&b, ']');
        }
        b_ch(&b, ']');
    }
    b_s(&b, "],rk:[");
    for (i = 0; i < g_shx_nrack; i++) {
        float q[3]; shx_tp_target(g_shx_rack[i], q);
        if (i) b_ch(&b, ',');
        b_ch(&b, '['); b_fix(&b, q[0], 1); b_ch(&b, ','); b_fix(&b, q[1], 1); b_ch(&b, ','); b_fix(&b, q[2], 1); b_ch(&b, ']');
    }
    b_s(&b, "]})");
    if (!force && seq(js, g_shx_lastJs)) return;
    for (k = 0; js[k] && k < (int)sizeof g_shx_lastJs - 1; k++) g_shx_lastJs[k] = js[k];
    g_shx_lastJs[k] = 0;
    shx_js_run(js);
}
static void shx_sync(void) {                                        /* свежий скан для меню, даже когда подписи выключены */
    if (!g_safe || !il2cpp_light()) return;
    ensure_attached();
    if (shx_myref(g_shx_ref)) shx_collect();
}

/* ───────────── доступ к игре ───────────── */
static void* shx_pool(const char* field) {                          /* Pools.<field> */
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}
static void* shx_obj(void* obj, const char* name) {                 /* метод без аргументов, вернувший ссылочный объект */
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int shx_vec(void* obj, const char* name, float* out) {       /* метод без аргументов, вернувший Vector3 (в боксе: 3 float после заголовка) */
    void *m, *exc = NULL, *r; const float* f;
    if (!obj || !hm_obj_ok(obj)) return 0;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return 0;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    f = (const float*)((char*)r + HDR);
    if (!shx_finite(f)) return 0;
    out[0] = f[0]; out[1] = f[1]; out[2] = f[2];
    return 1;
}
static int shx_myref(float* out) {                                  /* ваша позиция */
    void* me = local_player();
    void* tr;
    if (!me || !hm_obj_ok(me)) return 0;
    tr = shx_obj(me, "get_Transform");
    if (!tr) tr = shx_obj(me, "get_transform");
    if (tr && shx_vec(tr, "get_position", out)) return 1;
    return read_pos(me, out);
}
/* значения словаря пула (BasePool<T>.m_Items). -1 — не прочиталось, иначе число записей */
static int shx_scan_pool(const char* field, item_cb cb) {
    void *pool = shx_pool(field), *dict, *arr; int total;
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (!arr) return -1;
    total = int_field(dict, "_count", "count", NULL, -1);
    walk_struct_array(arr, total, "value", cb, NULL);
    return total;
}

/* ───────────── сбор руды и счётчик появлений ───────────── */
static void shx_track(int tid, const float* p) {                     /* учесть увиденный объект руды в таблице точек */
    int i;
    for (i = 0; i < g_shx_np; i++) {
        if (shx_dist(g_shx_pos[i].p, p) < 1.0f) {
            if (g_shx_pos[i].tid != tid) g_shx_spawn[tid]++;          /* там была другая руда или пусто — новое появление */
            g_shx_pos[i].tid = tid; g_shx_pos[i].seen = 1;
            return;
        }
    }
    if (g_shx_np < SHX_PMAX) {
        g_shx_pos[g_shx_np].p[0] = p[0]; g_shx_pos[g_shx_np].p[1] = p[1]; g_shx_pos[g_shx_np].p[2] = p[2];
        g_shx_pos[g_shx_np].tid = tid; g_shx_pos[g_shx_np].seen = 1;
        g_shx_np++; g_shx_spawn[tid]++;
    }
}
static void shx_add(int tid, int id, const float* p, int model) {
    float d; int i, j;
    d = shx_dist(p, g_shx_ref);
    g_shx_inrad[tid]++;
    if (!((g_shx_show >> tid) & 1)) return;                         /* вид скрыт в меню — только считаем, подпись не рисуем */
    for (i = 0; i < g_shx_nc && g_shx_c[i].d <= d; i++) {}
    if (i >= SHX_MAXL) return;
    for (j = (g_shx_nc < SHX_MAXL ? g_shx_nc : SHX_MAXL - 1); j > i; j--) g_shx_c[j] = g_shx_c[j - 1];
    g_shx_c[i].tid = tid; g_shx_c[i].id = id; g_shx_c[i].model = model; g_shx_c[i].d = d;
    g_shx_c[i].p[0] = p[0]; g_shx_c[i].p[1] = p[1]; g_shx_c[i].p[2] = p[2];
    if (g_shx_nc < SHX_MAXL) g_shx_nc++;
}
static void shx_tp_target(const float* p, float* q) {             /* Transform на SHX_TP_DY ниже точки; чуть в вашу сторону, если вы рядом */
    float dx, dz, hd;
    q[0] = p[0]; q[1] = p[1] - SHX_TP_DY; q[2] = p[2];
    dx = g_shx_ref[0] - p[0]; dz = g_shx_ref[2] - p[2]; hd = (float)__builtin_sqrt((double)(dx * dx + dz * dz));
    if (hd > 0.05f && hd < SHX_TP_NEAR) { q[0] += dx / hd * SHX_TP_BACK; q[2] += dz / hd * SHX_TP_BACK; }
}
static void shx_tp_add(int tid, int id, const float* p) {         /* вставка с сортировкой по ID, не больше SHX_TPK */
    int i, j, n = g_shx_ntp[tid];
    for (i = 0; i < n && g_shx_tp[tid][i].id <= id; i++) {}
    if (i >= SHX_TPK) return;
    for (j = (n < SHX_TPK ? n : SHX_TPK - 1); j > i; j--) g_shx_tp[tid][j] = g_shx_tp[tid][j - 1];
    g_shx_tp[tid][i].id = id; g_shx_tp[tid][i].p[0] = p[0]; g_shx_tp[tid][i].p[1] = p[1]; g_shx_tp[tid][i].p[2] = p[2];
    if (n < SHX_TPK) g_shx_ntp[tid] = n + 1;
}
static void shx_obj_cb(void* it, void* ctx) {
    float p[3]; int model, tid; (void)ctx;
    if (!hm_obj_ok(it)) return;
    model = (int)num_field(it, "Model", NULL);
    tid = shx_tid(model);
    if (tid < 0) return;                                             /* не руда — пропускаем, позицию даже не читаем */
    if (!shx_vec(it, "get_Position", p)) return;
    g_shx_total[tid]++;
    shx_track(tid, p);
    if (tid < 4) shx_tp_add(tid, (int)num_field(it, "ID", NULL), p);
    if (shx_dist(p, g_shx_ref) <= g_shx_radius) shx_add(tid, (int)num_field(it, "ID", NULL), p, model);
}
/* Стойки с инструментами: сервер вешает на каждую 3D-подпись «Стойка с инструментами» (miner.pwn, Miner:CreateToolRacks); клиент держит подписи в Pools.Text3DLables
   (CreateText3DParams: Position — мировая точка Unity, Text). Ищем по тексту, поэтому правка расстановки стоек на сервере ничего не ломает. Ближайшие — первыми. */
static int shx_lb_layout(void);
static int shx_has(const char* h, const char* n) {
    int i, k;
    for (i = 0; h[i]; i++) { for (k = 0; n[k] && h[i + k] == n[k]; k++) {} if (!n[k]) return 1; }
    return 0;
}
static void shx_rack_cb(void* it, void* ctx) {
    void *sf, *sc, *s; char* sp; int n, i, j, cnt; const u16* ch; char tmp[220]; float p[3], d; (void)ctx;
    if (!hm_obj_ok(it)) return;
    sf = find_field(A.object_get_class(it), "m_CreateParams");
    if (!sf) return;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return;
    sp = (char*)it + A.field_get_offset(sf);
    if (!hm_readable(sp + g_shx_oPos, 12) || !hm_readable(sp + g_shx_oTxt, 8)) return;
    s = *(void**)(sp + g_shx_oTxt);
    if (!s || !hm_obj_ok(s)) return;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 200 || !ch) return;
    utf16_to_utf8(ch, n, tmp, sizeof tmp);
    if (!shx_has(tmp, "Стойка с инструментами")) return;
    p[0] = ((float*)(sp + g_shx_oPos))[0]; p[1] = ((float*)(sp + g_shx_oPos))[1]; p[2] = ((float*)(sp + g_shx_oPos))[2];
    if (!(p[0] == p[0] && p[1] == p[1] && p[2] == p[2])) return;
    d = shx_dist(p, g_shx_ref); cnt = g_shx_nrack;
    for (i = 0; i < cnt && shx_dist(g_shx_rack[i], g_shx_ref) <= d; i++) {}
    if (i >= SHX_RACKS) return;
    for (j = (cnt < SHX_RACKS ? cnt : SHX_RACKS - 1); j > i; j--) { g_shx_rack[j][0] = g_shx_rack[j - 1][0]; g_shx_rack[j][1] = g_shx_rack[j - 1][1]; g_shx_rack[j][2] = g_shx_rack[j - 1][2]; }
    g_shx_rack[i][0] = p[0]; g_shx_rack[i][1] = p[1]; g_shx_rack[i][2] = p[2];
    if (cnt < SHX_RACKS) g_shx_nrack = cnt + 1;
}
/* Маркер сервера: Checkpoint.Simple / Checkpoint.Race (статические, по одному на игрока). Сервер шахты сам ставит маркер на следующую цель:
   стойка с инструментом после каждого этапа, месторождение после взятия кирки/тележки/лопаты, вагонетка/склад при сдаче. Спрятанный маркер уезжает в (0,0,0). */
static void* shx_call0(void* obj, const char* name) {
    void *m, *exc = NULL, *r;
    if (!obj || !hm_obj_ok(obj)) return NULL;
    m = find_method(A.object_get_class(obj), name, 0);
    if (!m) return NULL;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    return (exc || !r || !hm_obj_ok(r)) ? NULL : r;
}
static int shx_marker_one(const char* fld) {
    void *cls, *f, *obj = NULL, *tr, *go, *m, *exc = NULL, *r; const float* v;
    if (!g_safe || !il2cpp_light() || !g_img) return 0;
    cls = A.class_from_name(g_img, "Hassle.Entities", "Checkpoint");
    f = cls ? A.class_get_field_from_name(cls, fld) : NULL;
    if (!f) return 0;
    A.field_static_get_value(f, &obj);
    if (!obj || !hm_obj_ok(obj)) return 0;
    go = shx_call0(obj, "get_gameObject");
    if (go) {                                                       /* GameObject.activeInHierarchy: выключенный маркер — это «маркера нет» */
        m = find_method(A.object_get_class(go), "get_activeInHierarchy", 0);
        if (m) { r = A.runtime_invoke(m, go, NULL, &exc); if (!exc && r && hm_readable((char*)r + HDR, 1) && *((u8*)r + HDR) == 0) return 0; }
    }
    tr = shx_call0(obj, "get_transform");
    if (!tr) return 0;
    m = find_method(A.object_get_class(tr), "get_position", 0);
    if (!m) return 0;
    exc = NULL; r = A.runtime_invoke(m, tr, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 12)) return 0;
    v = (const float*)((char*)r + HDR);
    if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return 0;
    return !(v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f);
}
static int shx_marker_active(void) { return shx_marker_one("Simple") || shx_marker_one("Race"); }
static void shx_collect(void) {                                     /* заполняет g_shx_c ближайшими рудами к g_shx_ref */
    int i;
    g_shx_nc = 0;
    for (i = 0; i < SHX_NT; i++) { g_shx_total[i] = 0; g_shx_inrad[i] = 0; }
    for (i = 0; i < 4; i++) g_shx_ntp[i] = 0;
    g_shx_mark = shx_marker_active();
    g_shx_nrack = 0;
    if (shx_lb_layout()) shx_scan_pool("Text3DLables", shx_rack_cb);
    for (i = 0; i < g_shx_np; i++) g_shx_pos[i].seen = 0;
    g_shx_poolN = shx_scan_pool("Objects", shx_obj_cb);
    if (g_shx_poolN < 0) return;
    for (i = 0; i < g_shx_np; i++)                                   /* объекта нет, а вы рядом — точка опустела (руду выбили / ждёт респавна) */
        if (!g_shx_pos[i].seen && g_shx_pos[i].tid >= 0 && shx_dist(g_shx_pos[i].p, g_shx_ref) < SHX_GONE_R) g_shx_pos[i].tid = -1;
}

/* ───────────── подписи ───────────── */
static int shx_lb_layout(void) {
    void *sc, *f; int shift;
    if (g_shx_layoutOk) return 1;
    if (!g_img) return 0;
    sc = A.class_from_name(g_img, "Hassle.Global.Pooling", "CreateText3DParams");
    if (!sc || !A.class_is_valuetype(sc)) { dlog("shaxta: нет структуры Hassle.Global.Pooling.CreateText3DParams"); return 0; }
    shift = struct_shift(sc);
#define SHX_OFF(var, nm) do { f = find_field(sc, nm); if (!f) { dlog2("shaxta: нет поля CreateText3DParams.", nm); return 0; } var = A.field_get_offset(f) - shift; } while (0)
    SHX_OFF(g_shx_oID, "ID"); SHX_OFF(g_shx_oCol, "Color"); SHX_OFF(g_shx_oPos, "Position"); SHX_OFF(g_shx_oDist, "Distance");
    SHX_OFF(g_shx_oLos, "TestLOS"); SHX_OFF(g_shx_oPl, "PlayerID"); SHX_OFF(g_shx_oVeh, "VehicleID"); SHX_OFF(g_shx_oTxt, "Text");
#undef SHX_OFF
    g_shx_ssize = A.class_instance_size ? A.class_instance_size(sc) - shift : 0x28;
    if (g_shx_ssize < 0x28) g_shx_ssize = 0x28;
    if (g_shx_ssize > 120 || g_shx_oTxt + 8 > 120 || g_shx_oTxt < 0 || g_shx_oPos < 0 || g_shx_oID < 0) { dlog_i("shaxta: странная раскладка CreateText3DParams, размер ", g_shx_ssize); return 0; }
    g_shx_layoutOk = 1;
    dlog_i("shaxta: CreateText3DParams размер ", g_shx_ssize);
    return 1;
}
static int shx_lb_remove(int idnum) {                               /* Pools.Text3DLables.Remove(ID) */
    void *pool = shx_pool("Text3DLables"), *m, *exc = NULL, *args[1]; int id = SHX_BASE + idnum;
    if (!pool) return 0;
    m = find_method(A.object_get_class(pool), "Remove", 1);
    if (!m) { if (g_shx_errs < 6) { g_shx_errs++; dlog("shaxta: у пула подписей нет Remove(int)"); } return 0; }
    args[0] = &id;
    A.runtime_invoke(m, pool, args, &exc);
    if (exc) { shx_err("shaxta: Remove бросил исключение: ", exc); return 0; }
    return 1;
}
static void shx_hex6(Buf* b, unsigned rgb) {
    static const char* H = "0123456789ABCDEF"; int k;
    for (k = 20; k >= 0; k -= 4) b_ch(b, H[(rgb >> k) & 15]);
}
/* idnum — номер в диапазоне 0…SHX_RANGE-1; цвет задаётся форматом g_shx_fmt */
static int shx_lb_make(int idnum, const float* p, const char* name, unsigned rgb) {
    void *pool = shx_pool("Text3DLables"), *m, *exc = NULL, *args[1]; unsigned long long raw[16]; char* buf = (char*)raw; char txt[96]; Buf b; int k, col = -1;
    if (!pool || !shx_lb_layout()) return 0;
    m = find_method(A.object_get_class(pool), "Create", 1);
    if (!m) { if (g_shx_errs < 6) { g_shx_errs++; dlog("shaxta: у пула подписей нет Create(1 аргумент)"); } return 0; }
    for (k = 0; k < (int)sizeof raw; k++) buf[k] = 0;
    b_init(&b, txt, sizeof txt);
    if (g_shx_fmt == 0) col = (int)((rgb << 8) | 0xFFu);             /* 0xRRGGBBAA */
    else if (g_shx_fmt == 1) col = (int)(0xFF000000u | rgb);         /* 0xAARRGGBB */
    if (g_shx_fmt == 2) { b_s(&b, "<color=#"); shx_hex6(&b, rgb); b_ch(&b, '>'); b_s(&b, name); b_s(&b, "</color>"); }
    else if (g_shx_fmt == 3) { b_ch(&b, '{'); shx_hex6(&b, rgb); b_ch(&b, '}'); b_s(&b, name); }
    else b_s(&b, name);
    *(unsigned short*)(buf + g_shx_oID) = (unsigned short)(SHX_BASE + idnum);
    *(int*)(buf + g_shx_oCol) = col;
    ((float*)(buf + g_shx_oPos))[0] = p[0]; ((float*)(buf + g_shx_oPos))[1] = p[1] + 0.6f; ((float*)(buf + g_shx_oPos))[2] = p[2];
    *(float*)(buf + g_shx_oDist) = g_shx_radius + 10.0f;
    *(unsigned char*)(buf + g_shx_oLos) = 0;                         /* без проверки видимости: видно и сквозь стены */
    *(unsigned short*)(buf + g_shx_oPl) = 0xFFFF; *(unsigned short*)(buf + g_shx_oVeh) = 0xFFFF;
    *(void**)(buf + g_shx_oTxt) = A.string_new(txt);
    args[0] = buf;
    A.runtime_invoke(m, pool, args, &exc);
    if (exc) { shx_err("shaxta: Create бросил исключение: ", exc); return 0; }
    if (g_shx_logged < 3) { g_shx_logged++; dlog2("shaxta: создана подпись ", txt); }
    return 1;
}
static void shx_lb_cb(void* it, void* ctx) {                        /* какие из НАШИХ ID сейчас есть в пуле подписей */
    void *sf, *sc, *idf; char* sp; int shift, ioff, id; (void)ctx;
    if (!hm_obj_ok(it)) return;
    sf = find_field(A.object_get_class(it), "m_CreateParams");
    if (!sf) return;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return;
    idf = find_field(sc, "ID");
    if (!idf) return;
    sp = (char*)it + A.field_get_offset(sf);
    shift = struct_shift(sc);
    ioff = A.field_get_offset(idf) - shift;
    if (!hm_readable(sp + ioff, 2)) return;
    id = *(unsigned short*)(sp + ioff);
    if (id >= SHX_BASE && id < SHX_BASE + SHX_RANGE) g_shx_present[id - SHX_BASE] = 1;
}
static int shx_scan_present(void) {                                 /* 1 — пул подписей прочитан */
    int i;
    for (i = 0; i < SHX_RANGE; i++) g_shx_present[i] = 0;
    return shx_scan_pool("Text3DLables", shx_lb_cb) >= 0;
}
static int shx_any(void) { int i; for (i = 0; i < SHX_MAXL; i++) if (g_shx_slot[i].used) return 1; return 0; }
static void shx_clear_all(void) {                                   /* убрать все свои подписи (и осиротевшие, и тестовые) */
    int i;
    if (shx_scan_present()) for (i = 0; i < SHX_RANGE; i++) if (g_shx_present[i]) shx_lb_remove(i);
    for (i = 0; i < SHX_MAXL; i++) g_shx_slot[i].used = 0;
    g_shx_testOn = 0;
}

/* ───────────── тик (главный поток) ───────────── */
static void shx_tick(void) {
    int i, c, created = 0; int matched[SHX_MAXL];
    if (!g_safe || !il2cpp_light()) return;
    ensure_attached();
    if (!shx_pool("Text3DLables") || !shx_myref(g_shx_ref)) return;  /* не в игре / пулы сброшены — ждём */
    if (!shx_lb_layout()) { g_shx_on = 0; return; }
    if (!shx_scan_present()) return;
    for (i = 0; i < SHX_MAXL; i++) {                                 /* сверка с реальным пулом */
        if (g_shx_slot[i].used && !g_shx_present[i]) g_shx_slot[i].used = 0;       /* игра её уже убрала (реконнект) */
        else if (!g_shx_slot[i].used && g_shx_present[i]) shx_lb_remove(i);        /* осиротевшая */
    }
    shx_collect();
    if (g_shx_poolN < 0) { shx_ui_push(0); return; }
    if (g_shx_uiOk && ++g_shx_uiTick >= 30) { g_shx_uiTick = 0; shx_ui_inject(); }   /* страница могла перезагрузиться (реконнект) — меню вернётся само */
    for (c = 0; c < g_shx_nc; c++) matched[c] = 0;
    for (i = 0; i < SHX_MAXL; i++) {
        ShxSlot* s = &g_shx_slot[i]; int hit = -1;
        if (!s->used) continue;
        for (c = 0; c < g_shx_nc; c++) if (g_shx_c[c].id == s->id) { hit = c; break; }
        if (hit < 0) { shx_lb_remove(i); s->used = 0; continue; }       /* объект пропал (руду выбили) или ушёл за радиус */
        matched[hit] = 1;
        if (g_shx_c[hit].model != s->model || shx_dist(s->p, g_shx_c[hit].p) > SHX_MOVE) {   /* сменилась руда / сдвинулся — переделать */
            shx_lb_remove(i);
            if (shx_lb_make(i, g_shx_c[hit].p, g_shx_types[g_shx_c[hit].tid].name, g_shx_types[g_shx_c[hit].tid].rgb)) {
                s->model = g_shx_c[hit].model; s->p[0] = g_shx_c[hit].p[0]; s->p[1] = g_shx_c[hit].p[1]; s->p[2] = g_shx_c[hit].p[2];
            } else s->used = 0;
        }
    }
    for (c = 0; c < g_shx_nc && created < SHX_NEWPERTICK; c++) {
        int fs = -1;
        if (matched[c]) continue;
        for (i = 0; i < SHX_MAXL; i++) if (!g_shx_slot[i].used) { fs = i; break; }
        if (fs < 0) break;
        if (shx_lb_make(fs, g_shx_c[c].p, g_shx_types[g_shx_c[c].tid].name, g_shx_types[g_shx_c[c].tid].rgb)) {
            g_shx_slot[fs].used = 1; g_shx_slot[fs].id = g_shx_c[c].id; g_shx_slot[fs].model = g_shx_c[c].model;
            g_shx_slot[fs].p[0] = g_shx_c[c].p[0]; g_shx_slot[fs].p[1] = g_shx_c[c].p[1]; g_shx_slot[fs].p[2] = g_shx_c[c].p[2];
            created++;
        }
    }
    shx_ui_push(0);
}

/* ───────────── фоновый поток: Post в главный ───────────── */
static void native_shx_cmd(void* self, void* args, void* mi);
static void shx_post(void) {
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { g_shx_pending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_shx_cmd);
    st = A.string_new("shaxta-tick");
    if (!cb || !st) { g_shx_pending = 0; dlog("shaxta: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("shaxta: Post бросил исключение: ", et); g_shx_pending = 0; }
}
static void* shx_worker(void* arg) {
    (void)arg;
    for (;;) {
        if (!__atomic_load_n(&g_shx_on, __ATOMIC_ACQUIRE)) { usleep(200000); continue; }
        if (!g_shx_pending || shx_now_ms() - g_shx_pendT > 3000) {   /* прошлый тик не дошёл (игра на паузе/грузится) — шлём новый */
            g_shx_pending = 1; g_shx_pendT = shx_now_ms();
            shx_post();
        }
        usleep(SHX_TICK_US);
    }
    return NULL;
}

/* ───────────── команда ───────────── */
static int shx_tokens(void* arr, char tok[][32], int maxn) {
    long len; int i, n = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[96]; int cnt, p = 0, k; const u16* ch;
        if (!s) continue;
        cnt = A.string_length(s); ch = A.string_chars(s);
        if (cnt <= 0 || cnt > 40 || !ch) continue;
        utf16_to_utf8(ch, cnt, tmp, sizeof tmp);
        for (k = 0; tmp[k]; k++) if (tmp[k] >= 'A' && tmp[k] <= 'Z') tmp[k] = (char)(tmp[k] + 32);
        while (tmp[p]) {
            char w[32]; int q = 0;
            while (tmp[p] == ' ') p++;
            while (tmp[p] && tmp[p] != ' ') { if (q < 31) w[q++] = tmp[p]; p++; }
            w[q] = 0;
            if (!q || w[0] == '/' || seq(w, "shaxta")) continue;     /* сама команда может входить в массив (как у /tpl, /ids) */
            if (n < maxn) { int z; for (z = 0; z <= q; z++) tok[n][z] = w[z]; n++; }
        }
    }
    return n;
}
static void shx_usage(void) {
    chat_usage("/shaxta — ТЕСТОВЫЙ инструмент настройки шахты (тестовые NPC): подписи над рудой (вкл/выкл).  /shaxta r 400 — радиус, м;  /shaxta list — сводка по видам и счётчик появлений;  /shaxta reset — обнулить счётчик");
    chat_usage("/shaxta fz [on|off] — заморозка персонажа вручную (автоцикл замораживает сам и отпускает при остановке)");
    chat_usage("/shaxta test — тест цветов;  /shaxta fmt 0|1|2|3 — формат цвета (если цвета неверные);  /shaxta off — убрать подписи;  /shaxta status — состояние");
}
static void shx_b_fmtname(Buf* b) {
    b_s(b, g_shx_fmt == 0 ? "0 (RGBA)" : g_shx_fmt == 1 ? "1 (ARGB)" : g_shx_fmt == 2 ? "2 (<color=#>)" : "3 ({RRGGBB})");
}
static void shx_status(void) {
    char m[300]; Buf b; int n = 0, i;
    for (i = 0; i < SHX_MAXL; i++) if (g_shx_slot[i].used) n++;
    b_init(&b, m, sizeof m);
    b_s(&b, "/shaxta: "); b_s(&b, g_shx_on ? "ВКЛ" : "выкл"); b_s(&b, ", радиус "); b_fix(&b, g_shx_radius, 0);
    b_s(&b, " м, подписей сейчас "); b_int(&b, n); b_s(&b, ", формат цвета "); shx_b_fmtname(&b);
    chat_say(m);
    b_init(&b, m, sizeof m);
    b_s(&b, "  заморозка: "); b_s(&b, g_shx_fz ? "наша ВКЛ" : "наша выкл"); b_s(&b, ", игра считает замороженным: "); b_s(&b, shx_is_frozen() ? "да" : "нет"); b_s(&b, ";");
    b_s(&b, "  объектов в пуле: "); b_int(&b, g_shx_poolN); b_s(&b, " (-1 — пул не прочитался), отслеживаемых точек: "); b_int(&b, g_shx_np);
    chat_say(m);
}
static void shx_list(void) {
    char m[300]; Buf b; int i, tot = 0, tot_in = 0, sp = 0, spots;
    if (!g_safe || !il2cpp_light()) { chat_say("/shaxta: игра ещё грузится, подождите"); return; }
    ensure_attached();
    if (!shx_myref(g_shx_ref)) { chat_say("/shaxta: ваша позиция не прочиталась — подробности в konstt.log"); return; }
    shx_collect();
    if (g_shx_poolN < 0) { chat_say("/shaxta: пул объектов не прочитался (вы не в игре?) — подробности в konstt.log"); return; }
    for (i = 0; i < SHX_NT; i++) { tot += g_shx_total[i]; tot_in += g_shx_inrad[i]; sp += g_shx_spawn[i]; }
    if (tot == 0) {
        chat_say("/shaxta: руды рядом нет (шахта не прогружена или вы далеко). Если вы в шахте — сверьте номера моделей через /ids obj и поправьте g_shx_types");
        return;
    }
    spots = g_shx_total[0] + g_shx_total[1] + g_shx_total[2] + g_shx_total[3];                /* месторождения без «руды на земле» */
    b_init(&b, m, sizeof m);
    b_s(&b, "/shaxta: сейчас видно месторождений "); b_int(&b, spots); b_s(&b, " (+ руды на земле "); b_int(&b, g_shx_total[4]);
    b_s(&b, "), в радиусе "); b_fix(&b, g_shx_radius, 0); b_s(&b, " м: "); b_int(&b, tot_in);
    chat_say(m);
    for (i = 0; i < SHX_NT; i++) {
        b_init(&b, m, sizeof m); b_s(&b, "  "); b_s(&b, g_shx_types[i].name); b_s(&b, ": сейчас "); b_int(&b, g_shx_total[i]);
        if (spots > 0 && i < 4) { b_s(&b, " ("); b_fix(&b, 100.0 * g_shx_total[i] / spots, 0); b_s(&b, "%)"); }
        b_s(&b, ", появлений насчитано "); b_int(&b, g_shx_spawn[i]);
        if (sp > 0) { b_s(&b, " ("); b_fix(&b, 100.0 * g_shx_spawn[i] / sp, 1); b_s(&b, "%)"); }
        chat_say(m);
    }
    chat_say("  счётчик копится, пока подписи включены и вы стоите в шахте; /shaxta reset — обнулить. Дальше ~300 м сервер руду клиенту не отдаёт");
}
static int shx_enable(void) {
    float r[3];
    if (!g_safe || !il2cpp_light()) { chat_say("/shaxta: игра ещё грузится, подождите"); return 0; }
    ensure_attached();
    if (!shx_pool("Text3DLables")) { chat_say("/shaxta: пул 3D-подписей не найден (вы не в игре?) — подробности в konstt.log"); dlog("shaxta: нет Pools.Text3DLables"); return 0; }
    if (!shx_myref(r)) { chat_say("/shaxta: ваша позиция не прочиталась — подробности в konstt.log"); dlog("shaxta: не прочиталась позиция игрока"); return 0; }
    if (!shx_lb_layout()) { chat_say("/shaxta: не нашлась структура CreateText3DParams — подробности в konstt.log"); return 0; }
    mt_capture_ctx();                                                /* нужен главный поток — мы как раз в нём (обработчик команды) */
    shx_tick();                                                      /* первые подписи сразу */
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) {
        chat_say("/shaxta: подписи расставил один раз, но канала в главный поток нет — обновляться они не будут (подробности в konstt.log). /shaxta off уберёт их");
        dlog("shaxta: нет g_mt_ctx — только разовая расстановка");
        return 1;
    }
    g_shx_pending = 0;
    __atomic_store_n(&g_shx_on, 1, __ATOMIC_RELEASE);
    if (!g_shx_thread) {
        unsigned long t = 0;
        if (pthread_create(&t, NULL, shx_worker, NULL) != 0) { __atomic_store_n(&g_shx_on, 0, __ATOMIC_RELEASE); chat_say("/shaxta: не создался рабочий поток"); dlog("shaxta: pthread_create не вышел"); return 0; }
        g_shx_thread = 1;
    }
    return 1;
}
static void shx_disable(void) {
    __atomic_store_n(&g_shx_on, 0, __ATOMIC_RELEASE);
    if (g_safe && il2cpp_light()) { ensure_attached(); shx_clear_all(); }
    else { int i; for (i = 0; i < SHX_MAXL; i++) g_shx_slot[i].used = 0; g_shx_testOn = 0; }
}
static void shx_say_on(void) {
    char m[220]; Buf b; b_init(&b, m, sizeof m);
    b_s(&b, "/shaxta (тест настройки шахты): ВКЛ, радиус "); b_fix(&b, g_shx_radius, 0); b_s(&b, " м. /shaxta list — сводка, /shaxta off — выключить, /shaxta help — все команды");
    shx_say(m);
}
static void shx_test(void) {                                        /* три цветные подписи рядом с вами — проверка формата цвета */
    static const char* nm[SHX_TESTN] = { "ТЕСТ КРАСНЫЙ", "ТЕСТ ЗЕЛЁНЫЙ", "ТЕСТ СИНИЙ" };
    static const unsigned col[SHX_TESTN] = { 0xFF0000, 0x00FF00, 0x0000FF };
    static const float dx[SHX_TESTN] = { 3.0f, 0.0f, -3.0f }, dy[SHX_TESTN] = { 0.0f, 3.0f, 0.0f };
    float r[3], p[3]; int k, ok = 0; char m[200]; Buf b;
    if (!g_safe || !il2cpp_light()) { chat_say("/shaxta: игра ещё грузится, подождите"); return; }
    ensure_attached();
    if (!shx_myref(r)) { chat_say("/shaxta: ваша позиция не прочиталась — подробности в konstt.log"); return; }
    if (shx_scan_present()) for (k = 0; k < SHX_TESTN; k++) if (g_shx_present[SHX_MAXL + k]) shx_lb_remove(SHX_MAXL + k);
    for (k = 0; k < SHX_TESTN; k++) {
        p[0] = r[0] + dx[k]; p[1] = r[1] + dy[k]; p[2] = r[2] + 1.0f;
        if (shx_lb_make(SHX_MAXL + k, p, nm[k], col[k])) ok++;
    }
    g_shx_testOn = ok > 0;
    b_init(&b, m, sizeof m);
    b_s(&b, "/shaxta test: поставил подписей "); b_int(&b, ok); b_s(&b, ", формат "); shx_b_fmtname(&b);
    b_s(&b, ". Должны быть красная, зелёная, синяя. Нет — /shaxta fmt 1, затем 2, 3 и снова /shaxta test. Убрать: /shaxta off");
    chat_say(m);
}
static int shx_type_word(const char* w) {                           /* 0…4 — вид руды, -1 — не вид */
    if (seq(w, "gold") || seq(w, "золото") || seq(w, "au")) return 0;
    if (seq(w, "silver") || seq(w, "серебро") || seq(w, "ag")) return 1;
    if (seq(w, "iron") || seq(w, "железо") || seq(w, "fe")) return 2;
    if (seq(w, "coal") || seq(w, "уголь")) return 3;
    if (seq(w, "ground") || seq(w, "земля") || seq(w, "руда") || seq(w, "ore") || seq(w, "выбитая")) return 4;
    return -1;
}
static void shx_cmd_run(char tok[][32], int nt) {
    float v;
    if (nt == 0) {                                                   /* переключатель */
        if (g_shx_on || shx_any()) { shx_disable(); shx_say("/shaxta: выключен, подписи убраны"); }
        else if (shx_enable()) shx_say_on();
        return;
    }
    if (seq(tok[0], "help") || seq(tok[0], "помощь") || seq(tok[0], "?")) { shx_usage(); return; }
    if (seq(tok[0], "menu") || seq(tok[0], "меню")) {
        shx_ui_inject(); shx_sync();
        shx_js_run("window.__shx&&window.__shx.o(1)");
        if (!g_shx_uiOk) chat_say("/shaxta: меню не внедрилось — подробности в konstt.log");
        return;
    }
    if (seq(tok[0], "fz") || seq(tok[0], "freeze") || seq(tok[0], "фриз") || seq(tok[0], "заморозка")) {
        int want = nt >= 2 ? (seq(tok[1], "on") || seq(tok[1], "вкл") || seq(tok[1], "1")) ? 1 : (seq(tok[1], "off") || seq(tok[1], "выкл") || seq(tok[1], "0")) ? 0 : -1 : !g_shx_fz;
        char m[160]; Buf b;
        if (want < 0) { chat_usage("/shaxta fz [on|off] — заморозить / отпустить персонажа (без аргумента — переключить)"); return; }
        if (!shx_freeze(want)) { chat_say("/shaxta fz: не вышло (вы не в игре или ToggleFreeze не найден) — подробности в konstt.log"); return; }
        b_init(&b, m, sizeof m); b_s(&b, "/shaxta: заморозка "); b_s(&b, want ? "ВКЛ" : "выкл");
        b_s(&b, "; игра считает персонажа замороженным: "); b_s(&b, shx_is_frozen() ? "да" : "нет");
        shx_say(m);
        return;
    }
    if (seq(tok[0], "sync") || seq(tok[0], "обновить")) { shx_sync(); return; }
    if (seq(tok[0], "all") || seq(tok[0], "все") || seq(tok[0], "всё")) {
        g_shx_show = (1 << SHX_NT) - 1;
        if (g_shx_on || shx_any()) shx_tick();
        shx_say("/shaxta: показываю все виды руды"); return;
    }
    {
        int i, k, mask = 0, nw = 0, only = seq(tok[0], "only") || seq(tok[0], "только");
        for (i = only ? 1 : 0; i < nt; i++) { k = shx_type_word(tok[i]); if (k >= 0) { mask |= 1 << k; nw++; } }
        if (nw > 0) {
            char m[160]; Buf b;
            if (nw == 1 && !only) g_shx_show ^= mask;               /* одно слово — переключатель этого вида */
            else g_shx_show = mask;                                 /* несколько слов или «only» — только перечисленные */
            if (g_shx_on || shx_any()) shx_tick(); else shx_sync();
            b_init(&b, m, sizeof m); b_s(&b, "/shaxta: показываю: ");
            for (i = 0, k = 0; i < SHX_NT; i++) if ((g_shx_show >> i) & 1) { if (k++) b_s(&b, ", "); b_s(&b, g_shx_types[i].name); }
            if (!k) b_s(&b, "ничего");
            if (!g_shx_on && !shx_any()) b_s(&b, " (подписи выключены — /shaxta)");
            shx_say(m);
            return;
        }
    }
    if (seq(tok[0], "status") || seq(tok[0], "статус") || seq(tok[0], "diag")) { shx_status(); return; }
    if (seq(tok[0], "list") || seq(tok[0], "список") || seq(tok[0], "stat") || seq(tok[0], "stats")) { shx_list(); return; }
    if (seq(tok[0], "reset") || seq(tok[0], "сброс")) {
        int i; for (i = 0; i < SHX_NT; i++) g_shx_spawn[i] = 0;
        g_shx_np = 0;
        shx_say("/shaxta: счётчик появлений обнулён"); return;
    }
    if (seq(tok[0], "test") || seq(tok[0], "тест")) { shx_test(); return; }
    if (seq(tok[0], "fmt") || seq(tok[0], "format") || seq(tok[0], "формат")) {
        if (nt < 2 || !shx_num(tok[1], &v) || v > 3.5f) { chat_usage("/shaxta fmt 0|1|2|3 — 0 RGBA, 1 ARGB, 2 <color=#RRGGBB>, 3 {RRGGBB}. Проверка: /shaxta test"); return; }
        g_shx_fmt = (int)v;
        {   /* подписи уже стоят в старом формате — пересоздать при ближайшем тике */
            int i; for (i = 0; i < SHX_MAXL; i++) g_shx_slot[i].model = -1;
        }
        { char m[120]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "/shaxta: формат цвета "); shx_b_fmtname(&b); b_s(&b, ". Проверьте: /shaxta test"); shx_say(m); }
        if (g_shx_on) shx_tick();
        return;
    }
    if (seq(tok[0], "off") || seq(tok[0], "выкл") || seq(tok[0], "0")) {
        if (!g_shx_on && !shx_any() && !g_shx_testOn) { shx_say("/shaxta: и так выключен"); return; }
        shx_disable(); shx_say("/shaxta: выключен, подписи убраны"); return;
    }
    if (seq(tok[0], "on") || seq(tok[0], "вкл") || seq(tok[0], "1")) {
        if (nt >= 2 && shx_num(tok[1], &v)) { g_shx_radius = v < 10.0f ? 10.0f : v > 1000.0f ? 1000.0f : v; }
        if (shx_enable()) shx_say_on();
        return;
    }
    if (seq(tok[0], "r") || seq(tok[0], "radius") || seq(tok[0], "радиус") || shx_num(tok[0], &v)) {
        if (!shx_num(tok[0], &v)) { if (nt < 2 || !shx_num(tok[1], &v)) { chat_usage("/shaxta r 400 — радиус в метрах (10…1000)"); return; } }
        g_shx_radius = v < 10.0f ? 10.0f : v > 1000.0f ? 1000.0f : v;
        { int i; for (i = 0; i < SHX_MAXL; i++) g_shx_slot[i].model = -1; }   /* Distance подписи зависит от радиуса — пересоздать */
        if (g_shx_on || shx_any()) { shx_tick(); shx_say_on(); }
        else { char m[120]; Buf b; b_init(&b, m, sizeof m); b_s(&b, "/shaxta: радиус "); b_fix(&b, g_shx_radius, 0); b_s(&b, " м (подписи включит /shaxta)"); shx_say(m); }
        return;
    }
    shx_usage();
}
static void native_shx_cmd(void* self, void* args, void* mi) {
    char tok[6][32]; int nt, i, z;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) {      /* тик worker-а, а не команда */
        if (g_shx_on) shx_tick();
        g_shx_pending = 0;
        return;
    }
    mt_capture_ctx();
    nt = shx_tokens(args, tok, 6);
    dlog_i("shaxta: команда вызвана, слов ", nt);
    g_shx_q = 0;
    if (nt >= 1 && seq(tok[0], "q")) {                               /* «/shaxta q …» — из меню: тихо */
        g_shx_q = 1;
        for (i = 0; i + 1 < nt; i++) for (z = 0; z < 32; z++) tok[i][z] = tok[i + 1][z];
        nt--;
    }
    if (!g_shx_q && !g_shx_uiOk) shx_ui_inject();                    /* первая команда игрока — заодно ставим кнопку меню */
    shx_cmd_run(tok, nt);
    g_shx_q = 0;
    shx_ui_push(1);
}
static void* shx_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("shaxta", native_shx_cmd)) return NULL;     /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /shaxta */
    hm_mod_ready(g_shx_modId);
    dlog("shaxta: мод загружен, команда /shaxta работает");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void shx_ctor(void) {
    unsigned long t = 0;
    g_shx_modId = hm_mod_register("shaxta");
    pthread_create(&t, NULL, shx_main, NULL);
}
#endif
