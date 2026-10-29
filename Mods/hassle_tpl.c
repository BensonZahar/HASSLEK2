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
 *   /tpl save дом        запомнить ТЕКУЩЕЕ место под именем «дом» (то же имя ещё раз — перезапись)
 *   /tpl go дом          телепорт в сохранённую точку; короче: /tpl дом (если такая точка есть)
 *   /tpl list [стр]      список точек с расстоянием от вас (по 10 на странице);  /tpl del дом — удалить точку
 *   /tpl alt             переключить способ телепорта (запасной — через движение персонажа); если /tpl «не двигает», попробуйте его.
 *                        «/tpl alt 3 назад» — переключить И сразу прыгнуть
 *   /tpl status          какой способ сейчас выбран;  /tpl help — подсказка
 *   /tpl ghost [on|off|back]   «призрак»: игра перестаёт слать серверу вашу позицию (и прицел), пока режим включён. Сервер и его NPC
 *                        считают, что вы стоите там, где включили режим, а вы свободно ходите и телепортируетесь (/tpl …) в другом месте.
 *                        Без аргумента — переключить. «off» — выключить (сервер получит настоящую позицию: вы «появитесь» на новом месте),
 *                        «back» — вернуться на точку включения и выключить. «/tpl ghost unhook» — вернуть код игры в исходный вид.
 *                        «/tpl ghost tap [сек]» — на несколько секунд (по умолчанию 4, до 60) ПУСТИТЬ пакеты: чтобы наступить на метку/маркер
 *                        (переодеться, взять работу и т.п.). Потом призрак включается сам. Пока окно открыто, сервер видит вас там, где вы стоите.
 *   /tpl int N           сменить интерьер на номер N (0..255, 0 — улица). Только номер, координаты не нужны и тело не двигается.
 *                        «/tpl int» без номера — показать текущий (он же в строке «/tpl pos»). ВАЖНО: номер — это ТОЛЬКО метка (освещение
 *                        «в помещении» + сообщение серверу), а не место: сам интерьер — обычный участок карты со своими координатами.
 *                        Номер никуда не переносит; чтобы оказаться внутри, нужно оказаться в его координатах. Поэтому «/tpl save» запоминает
 *                        точку ВМЕСТЕ с номером интерьера, а «/tpl go» переносит и включает тот же интерьер. Если застряла тёмная
 *                        «комнатная» картинка на улице — «/tpl int 0».
 *   /tpl gps             телепорт на метку, поставленную на карте (/gps → точка/маршрут). Высота — по земле. Метки нет, а сервер показал маркер (чекпоинт) — на маркер.
 *                        Синонимы: /tpl метка, /tpl гпс, /tpl mark, /tpl wp
 *   /tpl pick            пикапы и маркеры РЯДОМ с вами (их присылает сервер, и клиент держит только тех, что в зоне стрима) — список по расстоянию, у каждого
 *                        свой номер 1, 2, 3 … (ID пикапа, расстояние, название — если рядом есть 3D-подпись, и координаты). Синонимы: /tpl пикап, /tpl маркеры, /tpl pk
 *                        В строке: номер в списке (для «pick N»), ID пикапа, [модель N] («?» — не прочиталась, причина в konstt.log), расстояние, 3D-подпись рядом и координаты.
 *   /tpl pick N          телепорт к номеру N из ПОСЛЕДНЕГО списка (номера остаются, пока не вызовете «/tpl pick» заново — можно ходить по ним подряд).
 *                        Встаёте ровно в точку пикапа, поэтому он срабатывает как при касании (раздевалка, вход и т.п.).
 *   /tpl pick list [стр] список по 10 на странице; «list 2» листает прежний снимок, не пересобирая
 *   В машине (вы за рулём) все команды телепорта (/tpl 5 вперёд, /tpl to …, /tpl go …) переносят МАШИНУ вместе с вами.
 *   Расстояние — любое от 0.3 м (меньше смысла нет); верхнего лимита нет, только страховка TPL_MAX = 100000 м от переполнения float.
 *
 * Как это работает
 *   • Координаты — это Transform.position игрока в мире Unity (та же система, в которой работает SetPosition). Если координаты из пакета
 *     синхронизации (то, что уходит на сервер) заметно отличаются от них, «/tpl pos» покажет и их отдельной строкой.
 *   • Точки лежат в файле HLOG/tpl_points.txt (рядом с konstt.log, как route.txt), по строке «имя x y z», и переживают перезапуск игры.
 *     Файл можно править руками. До 64 точек; имя — одно слово до 31 байта (≈15 русских букв), регистр не важен. Сохранение пишет
 *     временный файл и переименовывает его, поэтому вылет посреди записи старый список не портит.
 *     Точка — это те же координаты, что показывает «/tpl pos», а «/tpl go» = «/tpl to» с этими координатами. Если в строке есть 5-е число —
 *     это номер интерьера (его пишет «/tpl save»; в старых файлах его нет — тогда интерьер не трогаем): «имя x y z N».
 *   • Имя можно писать до слова-команды: «/tpl дом save». Если имя совпадает со словом-командой или числом («влево», «pos», «5»),
 *     короткая форма «/tpl имя» не сработает — тогда «/tpl go имя» (мод сам предупредит при сохранении).
 *   • Стороны — относительно КАМЕРЫ (как вы видите экран): берём Transform.right у MainCamera, обнуляем высоту, нормализуем.
 *     «Вперёд» получаем поворотом «вправо» на 90° вокруг вертикали (камера может смотреть вниз — Transform.forward тогда вырожден).
 *     Если камеры нет — берём Transform.right самого игрока.
 *   • Позиция — Transform.position (у игрока — Entity.Transform; у запасного способа — transform компонента движения), + сдвиг по
 *     горизонтали, высота не меняется.
 *   • Основной способ — Player.SetPosition(Vector3, findHeight=false): тот же метод, которым игра сама переставляет игрока (по приказу сервера).
 *     Запасной — ICharacterMovement.SetPosition(Vector3, updateGround=true): прямая постановка капсулы. Если основной бросил исключение — пробуем запасной сами.
 *   • Обработчик команды вызывается игрой в ГЛАВНОМ потоке (как у /rec и /nocol), поэтому Post/SynchronizationContext не нужны.
 *   • В машине (за рулём) двигается сама МАШИНА: у машины тот же Entity.SetPosition(Vector3, findHeight), который игра зовёт по приказу
 *     сервера SetVehiclePos (RPC читает id, три float, берёт машину из VehiclePool и вызывает SetPosition(...).Forget()); после него
 *     обнуляем скорость Rigidbody, чтобы машина не улетела дальше. Пассажиру отказ: машину двигает водитель. Координаты в машине —
 *     это Transform машины. Лимита по дальности нет: большие прыжки сервер может откатить, а на далёкой точке карта может быть
 *     не загружена (можно провалиться) — это на ваше усмотрение. На сервере есть античит с проверкой телепорта (AC_TELEPORT) —
 *     и пешком, и на машине дальний прыжок может быть засчитан как телепорт: риск на вас.
 *   • Прыжок МАШИНЫ асинхронный. Entity.SetPosition у машины (тот же вызов, что в RPC SetVehiclePos) сперва замораживает её (Rigidbody
 *     kinematic), затем СОЗДАЁТ точку подгрузки мира в месте назначения (IsNeedLoadCollision у BaseVehicle всегда «да», радиус 300 м, если вы
 *     внутри), ЖДЁТ загрузки коллизии (WorldStream.WaitForCollisionLoaded, с тайм-аутом) и только после этого ставит Rigidbody/Transform.position
 *     и размораживает. Рядом коллизия уже загружена — всё проходит в одном вызове; на большое расстояние ожидание занимает время, и всё это
 *     время машина стоит на старом месте. Поэтому мод НЕ пишет «готово» сразу, а запускает наблюдателя (tplw_*): раз в ~60 мс главному потоку
 *     уходит «тик» (тот же приём, что у /route), наблюдатель дожидается прибытия, ТОГДА обнуляет скорость машины (до прибытия она заморожена и
 *     сброс ничего не давал), сообщает в чат, сколько ждали карту, и ещё 3 с следит: если машину вернуло на прежнее место, он так и скажет
 *     (значит, откат пришёл не из кода /tpl, мод к тому моменту уже поставил машину на точку). Подробности — в konstt.log.
 *   • Это не «клиентская» штука, как /nocol: позиция уходит на сервер как обычная, и другие игроки вас видят на новом месте
 *     (если не включён /tpl ghost — тогда позиция на сервер не уходит вовсе).
 *
 * «Метка GPS» и «пикапы» (/tpl gps, /tpl pick) — по дампу IL2CPP и дизассемблированию libil2cpp.so (RVA из dump.cs)
 *   • Метка с карты: JS зовёт getNavigationPath(x, y, z) в системе SAMP (x, y — плоскость, z — высота), а MiniMap.<GetNavigationPath>d__41.MoveNext кладёт в
 *     MiniMap.m_NavigationPoint (Vector3 @0x84) значение (x, z, y), т.е. уже мировые координаты Unity — те же, что у Transform.position и /tpl to. Это поле и читаем
 *     (MiniMap берём через статический UI.get_MiniMap). Высоту метки сервер знает не всегда (у метки по тапу на карте она может быть 0), поэтому на метку встаём как
 *     «/tpl to X Z»: Player.SetPosition(…, findHeight=true), а высота метки — лишь точка, откуда искать землю вниз. m_IsNavigationActive читаем для сообщений.
 *   • Пикапы: Pools.Pickups (PickupPool : BasePool<Pickup>, словарь m_Items, как у NPC в /air); у Pickup есть только m_ID и Transform — тип и модель клиент не хранит.
 *     Название берём из 3D-подписей сервера: Pools.Text3DLables (sic) → Text3DLabel.m_CreateParams.Text / Position (Position — мировая точка: LateUpdate отдаёт её в
 *     Camera.WorldToViewportPoint); ближайшая подпись не дальше 4 м от пикапа становится его названием.
 *   • Маркер (чекпоинт) — статические Checkpoint.Simple / Checkpoint.Race (SetCheckpoint / SetRaceCheckpoint от сервера); он у игрока один, виден в списке, пока активен.
 *   • ВАЖНО: клиент знает только тех, кого сервер подгрузил рядом (зона стрима), весь список сервера из клиента не достать. Код не запускался на устройстве —
 *     собран и проверен компилятором и тестом разбора команд; если «/tpl gps» не находит метку или «/tpl pick» пуст — смотрите HLOG/konstt.log (строки «tpl:»).
 *
 * «Призрак» (/tpl ghost)
 *   • Позицию на сервер шлёт LocalPlayer.SendSyncData → SendOnFootFullSyncData (пакет «пешком», в нём Position), а камеру/прицел —
 *     SendAimSyncData. В НАЧАЛО этих двух методов ставится перехват (тот же приём, что в hassle_weapon.c: 16 байт пролога заменяются на
 *     абсолютный прыжок в заглушку; заглушка зовёт gh_hook и, если он вернул 1, сразу возвращается из метода — пакет не уходит;
 *     если 0 — выполняет затёртые инструкции и продолжает метод как обычно). Пока режим выключен, хук всегда отвечает 0.
 *   • Место патча не зашито адресом: методы находим по имени. Перед патчем проверяем, что пролог начинается с «sub sp, sp, #…» и
 *     4 затираемые инструкции не зависят от адреса; иначе ничего не трогаем. Только arm64.
 *   • Сервер при этом просто не получает пакеты: он помнит последнюю позицию (ту, где вы включили режим) и показывает вас там
 *     всем (и NPC, и игрокам). Это РАССИНХРОН, а не невидимость: игроки видят вас стоящим на старом месте.
 *   • Почему в призраке нельзя наступить на метку: метки/чекпоинты/зоны на этом сервере проверяет СЕРВЕР по позиции, которую он знает
 *     (в его коде это SetPlayerCheckpoint, CreateDynamicCP/Sphere, IsPlayerInRangeOfPoint…), а в призраке он знает старую точку.
 *     Значит, чтобы метка сработала, серверу нужно показать позицию у метки — это и делает «tap». Всё, что видит сервер, видят и его NPC,
 *     поэтому на время окна вас видно: лучше открывать его, пока рядом нет NPC. Окно закрывается само: заглушка сравнивает время при
 *     каждой отправке (g_gh_on = 2 — окно, 1 — призрак, 0 — выкл.).
 *   • Чего НЕ перехватываем: пакеты машины (Drive/Passenger) и выстрела (SendBulletSyncData). В машине и при стрельбе сервер
 *     узнает настоящую позицию — в призраке не садитесь в машину и не стреляйте. Любая серверная проверка рассинхрона позиций
 *     может кикнуть или забанить: режим на ваш риск.
 *
 * «Интерьер» (/tpl int N)
 *   • Зовёт LocalPlayer.SetInterior(byte) — тот самый метод, которым игра сама меняет интерьер по приказу сервера. По коду игры он
 *     запоминает номер, САМ отправляет серверу RPC об смене интерьера (NetworkClient.SendRPC) и вызывает Lighting.SetInterior
 *     (освещение). Позиция не меняется: попасть внутрь — /tpl to … / /tpl go … (или стоять там, где уже находитесь).
 */
#pragma once
#include "hassle_core.c"

#define TPL_DEF 1.5f
#define TPL_MIN 0.3f
#define TPL_MAX 100000.0f                                /* не «лимит», а страховка: больше — бесконечность/мусор для Unity (его собственный предел координат ~100000) */

#define TPL_NAME 32                                      /* имя точки: до 31 байта + ноль */
#define TPL_MAXP 64                                      /* точек всего */
#define TPL_PAGE 10                                      /* строк на странице /tpl list */
#define TPL_PTS_FILE "tpl_points.txt"

enum { TPL_L = 0, TPL_R = 1, TPL_F = 2, TPL_B = 3 };
static const char* const g_tpl_sideName[4] = { "влево", "вправо", "вперёд", "назад" };

static volatile int g_tpl_alt;                           /* 1 — двигать через движение персонажа (запасной способ) */
static int g_tpl_modId = -1;                             /* номер в реестре модов (экран загрузки) */

/* ───────────── мелкие помощники ───────────── */
static int tpl_finite(const float* v) {
    return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && v[0] < 1e7f && v[0] > -1e7f && v[1] < 1e7f && v[1] > -1e7f && v[2] < 1e7f && v[2] > -1e7f;
}
/* копия токена в имя точки (до cap-1 байт). Обрезка не рвёт русскую букву пополам, управляющие символы (в т.ч. табуляция, ломающая файл) → «_» */
static void tpl_copy_name(char* dst, const char* src, int cap) {
    int i = 0, last = -1;
    for (; src[i] && i < cap - 1; i++) dst[i] = ((u8)src[i] < 0x20 || (u8)src[i] == 0x7F) ? '_' : src[i];
    dst[i] = 0;
    for (i = 0; dst[i]; i++) if (((u8)dst[i] & 0xC0) != 0x80) last = i;      /* начало последнего символа */
    if (last >= 0) {
        u8 c = (u8)dst[last]; int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1, have = 0;
        while (dst[last + have]) have++;
        if (have < need) dst[last] = 0;
    }
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
typedef struct { void *me, *mv, *etr, *mtr, *veh, *tgt; } TplCtx;   /* veh — машина, в которой вы за рулём (иначе NULL); tgt — что двигаем: вас или машину */
/* mode: 0 — ничего не требуем; 1 — только пешком (в машине отказ); 2 — пешком или за рулём (тогда двигается МАШИНА вместе с вами, пассажиру отказ).
   Сама пишет в чат, почему не вышло. 1 — можно работать. */
static int tpl_ctx_q(TplCtx* c, int mode, int quiet) {          /* quiet=1 — ничего не писать в чат (для списка точек) */
    void* veh;
    c->me = c->mv = c->etr = c->mtr = c->veh = c->tgt = NULL;
    if (!g_safe || !il2cpp_light()) { if (!quiet) chat_say("Телепорт: игра ещё грузится, подождите"); return 0; }
    ensure_attached();
    c->me = local_player();
    if (!c->me || !hm_obj_ok(c->me)) { if (!quiet) chat_say("Телепорт: вы не в игре (персонаж не создан)"); return 0; }
    c->tgt = c->me;
    veh = ref_field_s(c->me, "Vehicle");                             /* Player.Vehicle: машина, в которой вы сидите */
    if (veh && !hm_obj_ok(veh)) veh = NULL;
    if (veh && mode == 1) { if (!quiet) chat_say("Телепорт: в машине тут не работает — сначала выйдите"); return 0; }
    if (veh && mode == 2) {
        void* drv = ref_field_s(veh, "m_Driver");                    /* BaseVehicle.m_Driver; не прочиталось — считаем водителем */
        if (drv && drv != c->me) { if (!quiet) chat_say("Телепорт: вы пассажир — машину двигает водитель. Выйдите или сядьте за руль"); return 0; }
        c->veh = veh; c->tgt = veh;
    }
    c->etr = tpl_obj(c->tgt, "get_Transform");                        /* Entity.Transform (у игрока или у машины) */
    if (!c->etr) c->etr = tpl_obj(c->tgt, "get_transform");           /* запасной вариант: Component.transform */
    if (!c->veh) {                                                    /* компонент движения — только у пешего игрока */
        c->mv = tpl_movement(c->me);
        if (c->mv && hm_obj_ok(c->mv)) c->mtr = tpl_obj(c->mv, "get_transform");
        else c->mv = NULL;
    }
    return 1;
}
static int tpl_ctx(TplCtx* c, int mode) { return tpl_ctx_q(c, mode, 0); }
/* Где я сейчас: 1 — Transform.position (основное), 2 — из пакета синхронизации (запасное), 0 — не прочиталось */
static int tpl_cur_pos(const TplCtx* c, float* p) {
    if (c->etr && tpl_vec(c->etr, "get_position", p)) return 1;
    if (c->mtr && tpl_vec(c->mtr, "get_position", p)) return 1;
    return read_pos(c->me, p) ? 2 : 0;
}
/* Обнулить скорость машины после переноса (чтобы не улетела дальше): Rigidbody.velocity / angularVelocity = 0. Если Rigidbody не нашёлся — молча пропускаем. */
static void tpl_stop_vehicle(void* veh) {
    void *rb, *m, *exc = NULL, *args[1]; float z[3];
    z[0] = z[1] = z[2] = 0.0f;
    rb = ref_field_s(veh, "m_Rigidbody");                            /* BaseVehicle.m_Rigidbody */
    if (!rb || !hm_obj_ok(rb)) rb = ref_field_s(veh, "vehicleRigidbody");   /* Vehicle.vehicleRigidbody */
    if (!rb || !hm_obj_ok(rb)) { dlog("/tpl: Rigidbody машины не найден — скорость не сбрасывал"); return; }
    args[0] = z;
    m = find_method(A.object_get_class(rb), "set_velocity", 1);
    if (m) A.runtime_invoke(m, rb, args, &exc);
    exc = NULL;
    m = find_method(A.object_get_class(rb), "set_angularVelocity", 1);
    if (m) A.runtime_invoke(m, rb, args, &exc);
}
/* Поставить игрока (или его машину) в точку. npE — позиция в системе Entity.Transform (для SetPosition игрока/машины), npM — в системе transform
   движения (для запасного способа, только пешком); любой может быть NULL. findH — искать высоту по земле (только основной способ).
   Возврат: 1 основной, 2 запасной, 3 — перенесена машина, 0 не вышло. */
static void tplf_guard(const TplCtx* c);                        /* «как на ровном» после телепорта пешком (ниже, рядом с наблюдателем) */
static int tpl_place(const TplCtx* c, const float* npE, const float* npM, u8 findH) {
    if (c->veh) {                                                    /* за рулём: двигаем машину; запасной способ (/tpl alt) тут не нужен */
        if (npE && tpl_call_set(c->veh, npE, findH, "машине")) { tpl_stop_vehicle(c->veh); return 3; }
        return 0;
    }
    if (!g_tpl_alt && npE) {
        if (tpl_call_set(c->me, npE, findH, "Player")) { tplf_guard(c); return 1; }
        dlog("/tpl: основной способ не вышел, пробую запасной (через движение)");
    }
    if (npM && c->mv && tpl_call_set(c->mv, npM, 1, "движению")) { tplf_guard(c); return 2; }
    return 0;
}
static void tpl_fail(void) { chat_say("Телепорт: не вышло — подробности в konstt.log. Попробуйте /tpl alt"); }
static void native_tpl_cmd(void* self, void* args, void* mi);
static int tplw_begin(const TplCtx* c, const float* from, const float* to);   /* наблюдатель за прыжком машины; 1 — машина ещё не на точке (ждёт карту) */

/* ───────────── сдвиг в сторону ───────────── */
static void tpl_go(int side, float dist, int clamped) {
    TplCtx c; float R[3], dir[3], eb[3], mb[3], ne[3], nm[3]; int src = 0, haveE, haveM, via; Buf b; char msg[240];
    if (!tpl_ctx(&c, 2)) return;
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
    if (via == 3) {
        b_s(&b, " (вместе с машиной)");
        if (haveE && tplw_begin(&c, eb, ne)) b_s(&b, " — машина двинется, когда подгрузится карта в точке");
    }
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
    TplCtx c; float p[3], sp[3]; int have, haveS; Buf b; char msg[260];
    if (!tpl_ctx(&c, 0)) return;
    have = tpl_cur_pos(&c, p);
    haveS = (have == 1) && read_pos(c.me, sp);                    /* пакет синхронизации — для сравнения, только если основное прочиталось */
    if (!have) { chat_say("Координаты: не удалось прочитать (подробности в konstt.log)"); dlog("/tpl pos: ни Transform.position, ни пакет синхронизации не прочитались"); return; }
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Координаты: "); tpl_b_xyz(&b, p); b_s(&b, "  — вернуться сюда: /tpl to "); tpl_b_xyz(&b, p);
    chat_say(msg);
    dlog2("/tpl pos: ", msg);
    {   /* интерьер: LocalPlayer.m_Interior — его ставит LocalPlayer.SetInterior (и приказ сервера SetPlayerInterior, и наш /tpl int) */
        int in = int_field(c.me, "m_Interior", "m_Interior", NULL, -1);
        if (in >= 0) {
            b_init(&b, msg, sizeof msg);
            b_s(&b, "Интерьер: "); b_int(&b, in); b_s(&b, in == 0 ? " (улица)" : "  — сменить: /tpl int N");
            chat_say(msg);
        }
    }
    if (haveS && (tpl_fabs(sp[0] - p[0]) > 0.5f || tpl_fabs(sp[1] - p[1]) > 0.5f || tpl_fabs(sp[2] - p[2]) > 0.5f)) {
        b_init(&b, msg, sizeof msg);
        b_s(&b, "По пакету синхронизации (как видит сервер): "); tpl_b_xyz(&b, sp);
        chat_say(msg);
        dlog2("/tpl pos: ", msg);
    }
}
/* «/tpl to X Y Z» (nc = 3) или «/tpl to X Z» (nc = 2, высота по земле).  findH — искать высоту по земле (для nc = 2 всегда; для nc = 3 — когда Y лишь
   «откуда искать вниз»: так /tpl gps ставит на метку, у которой точной высоты нет) */
static int tpl_goto_ex(const float* v, int nc, const char* label, int findH) {
    TplCtx c; float eb[3], mb[3], ne[3], nm[3]; int haveE, haveM, via, i; Buf b; char msg[240];
    for (i = 0; i < nc; i++) if (!(v[i] == v[i]) || v[i] > TPL_MAX || v[i] < -TPL_MAX) { chat_say("Телепорт: координаты вне допустимого диапазона (±100000)"); return 0; }
    if (!tpl_ctx(&c, 2)) return 0;
    haveE = c.etr && tpl_vec(c.etr, "get_position", eb);
    haveM = c.mtr && tpl_vec(c.mtr, "get_position", mb);
    if (nc == 3) { ne[0] = v[0]; ne[1] = v[1]; ne[2] = v[2]; }
    else {                                                         /* X Z: высоту берём текущую, а Player.SetPosition ищет землю */
        if (!haveE && !haveM) { chat_say("Телепорт: не удалось прочитать вашу высоту — укажите все три числа: /tpl to X Y Z"); return 0; }
        ne[0] = v[0]; ne[1] = haveE ? eb[1] : mb[1]; ne[2] = v[1];
    }
    nm[0] = ne[0]; nm[1] = ne[1]; nm[2] = ne[2];
    if (haveE && haveM) { nm[0] += mb[0] - eb[0]; nm[1] += mb[1] - eb[1]; nm[2] += mb[2] - eb[2]; }   /* перевод в систему transform движения */
    via = tpl_place(&c, ne, nm, findH ? 1 : 0);
    if (!via) { tpl_fail(); return 0; }
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Телепорт на ");
    if (label) { b_s(&b, "точку «"); b_s(&b, label); b_s(&b, "»: "); }
    tpl_b_xyz(&b, ne);
    if (findH) b_s(&b, " (высота по земле)");
    if (haveE) {
        double dx = ne[0] - eb[0], dy = ne[1] - eb[1], dz = ne[2] - eb[2];
        b_s(&b, ", это в "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 1); b_s(&b, " м от вас");
    }
    if (via == 2) b_s(&b, " [через движение]");
    if (via == 3) {
        b_s(&b, " (вместе с машиной)");
        if (haveE && tplw_begin(&c, eb, ne)) b_s(&b, " — машина двинется, когда подгрузится карта в точке");
    }
    chat_say(msg);
    dlog2("/tpl: ", msg);
    return 1;
}

static int tpl_goto(const float* v, int nc, const char* label) { return tpl_goto_ex(v, nc, label, nc == 2); }

/* ───────────── призрак (/tpl ghost) и интерьер (/tpl int) ───────────── */
#if defined(__aarch64__)
#define GH_ARM64 1
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int mprotect(void*, unsigned long, int);
extern long pwrite(int, const void*, unsigned long, long);
#endif

#define GH_SITES 2
static const char* const g_gh_siteName[GH_SITES] = { "SendOnFootFullSyncData", "SendAimSyncData" };
static const int g_gh_siteArgc[GH_SITES] = { 1, 0 };     /* SendOnFootFullSyncData(bool force), SendAimSyncData() */

extern int clock_gettime(int, void*);                    /* как в mem/route: struct timespec = два long */
static volatile int g_gh_on;                             /* 0 — выкл.; 1 — призрак (пакеты позиции и прицела не уходят); 2 — окно «tap»: пакеты идут до g_gh_until */
static volatile long long g_gh_until;                    /* конец окна, мс по CLOCK_MONOTONIC */
static volatile unsigned g_gh_hits;                      /* сколько пакетов хук проглотил — по нему видно, что перехват живой */
static float g_gh_pos[3];                                /* где вы были в момент включения (где сервер вас «помнит») */
static int g_gh_havePos;
static int g_gh_state;                                   /* 1 — перехваты стоят */
static char g_gh_err[160];

/* Зовётся из заглушки в начале SendOnFootFullSyncData / SendAimSyncData (главный поток). 1 — метод пропустить. Только атомики, без вызовов IL2CPP. */
static long long gh_now_ms(void) { long ts[2]; ts[0] = 0; ts[1] = 0; clock_gettime(1, ts); return (long long)ts[0] * 1000 + ts[1] / 1000000; }
__attribute__((noinline, used))
static int gh_hook(void* player, void* a1) {
    int s = __atomic_load_n(&g_gh_on, __ATOMIC_ACQUIRE);
    (void)player; (void)a1;
    if (s == 0) return 0;
    if (s == 2) {
        if (gh_now_ms() < __atomic_load_n(&g_gh_until, __ATOMIC_ACQUIRE)) return 0;       /* окно открыто — пакет уходит как обычно */
        {
            int exp = 2;
            __atomic_compare_exchange_n(&g_gh_on, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);   /* окно кончилось — снова призрак */
        }
    }
    __atomic_fetch_add(&g_gh_hits, 1u, __ATOMIC_RELAXED);
    return 1;
}

/* ───────────── наблюдатель за прыжком машины ─────────────
   Зачем: Entity.SetPosition у машины возвращает управление сразу, а сам перенос делает позже (заморозка → подгрузка карты в точке → ожидание →
   Rigidbody/Transform.position → разморозка). Раньше мод в тот же миг писал «готово» и обнулял скорость — на дальнем прыжке и то и другое было
   неправдой (машина ещё стояла на старом месте, замороженная). Теперь: worker-поток раз в TPLW_TICK_US шлёт главному потоку «тик»
   (UnitySynchronizationContext.Post, как у /route), а tplw_tick() на главном потоке смотрит, где машина. Фазы: 1 — ждём прибытия, 2 — следим
   TPLW_WATCH мс после прибытия (не вернули ли назад). Всё только читает позицию (и один раз обнуляет скорость), движением не управляет. */
#define TPLW_ARRIVE  12.0f                               /* м по горизонтали: «машина на точке» */
#define TPLW_FAR     40.0f                               /* м: дальше этого от точки — машину увезло/она уехала сама */
#define TPLW_BIG     100.0f                              /* м: на меньших прыжках откат не разбираем (не отличить от обычной езды) */
#define TPLW_TIMEOUT 30000LL                             /* мс ожидания прибытия */
#define TPLW_WATCH   3000LL                              /* мс наблюдения после прибытия */
#define TPLW_TICK_US 60000

static volatile int g_tplf_on;                           /* 1 — способность Fall выключена нами после телепорта и ждёт возврата */
static volatile int g_tplw_on;                           /* 0 — нет, 1 — ждём прибытия, 2 — наблюдаем после прибытия */
static volatile int g_tplw_pending;                      /* тик отправлен и ещё не обработан */
static volatile long long g_tplw_pendT;
static int g_tplw_thread;
static long long g_tplw_t0, g_tplw_tArr;
static float g_tplw_from[3], g_tplw_to[3], g_tplw_total;

static float tplw_dxz(const float* a, const float* b) {
    double dx = (double)a[0] - (double)b[0], dz = (double)a[2] - (double)b[2];
    return (float)__builtin_sqrt(dx * dx + dz * dz);
}
static void tplw_end(const char* why) {
    __atomic_store_n(&g_tplw_on, 0, __ATOMIC_RELEASE);
    dlog2("/tpl: наблюдение за машиной закончено: ", why);
}
/* worker-поток: положить в очередь главного потока вызов native_tpl_cmd со строкой-меткой вместо string[] */
static void tplw_post(void) {
    void *cb, *st, *a[2], *exc = NULL; char et[200];
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { g_tplw_pending = 0; return; }
    ensure_attached();
    cb = clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)native_tpl_cmd);
    st = A.string_new("tpl-tick");
    if (!cb || !st) { g_tplw_pending = 0; dlog("/tpl: не создался делегат для Post"); return; }
    a[0] = cb; a[1] = st;
    A.runtime_invoke(g_mt_postM, g_mt_ctx, a, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("/tpl: Post бросил исключение: ", et); g_tplw_pending = 0; }
}
static void* tplw_worker(void* arg) {
    (void)arg;
    for (;;) {
        if (!__atomic_load_n(&g_tplw_on, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_tplf_on, __ATOMIC_ACQUIRE)) { usleep(100000); continue; }
        if (!g_tplw_pending || gh_now_ms() - g_tplw_pendT > 1000) {      /* прошлый тик не дошёл (игра на паузе/грузится) — шлём новый */
            g_tplw_pending = 1; g_tplw_pendT = gh_now_ms();
            tplw_post();
        }
        usleep(TPLW_TICK_US);
    }
    return NULL;
}
/* ───────────── «как на ровном» после телепорта (так же, как /air stand) ─────────────
   После телепорта персонаж на миг в воздухе (высота точки выше пола) — включается способность Fall: клип падения, руки в стороны.
   Как в /air: на время падения ставим у Fall Enabled = false (Character.TryStartAbility пропускает выключенные), если уже активна — StopAbility(force);
   вернуть Enabled — когда персонаж встал на землю (get_isOnGround у движения; не прочиталось — через 2.5 с), но не позже TPLF_MAX_MS.
   Если Fall уже выключена до нас (например, работает /air) — ничего не трогаем, чтобы потом не оставить её выключенной насовсем. */
#define TPLF_MIN_MS 700LL                                /* не возвращаем раньше: даём падению начаться */
#define TPLF_BLIND_MS 2500LL                             /* земля не читается — возвращаем по времени */
#define TPLF_MAX_MS 8000LL                               /* предел: дольше Fall выключенной не держим */
static int g_tplf_nosnap, g_tplf_snapOk;                          /* nosnap — этот телепорт на пикап: на землю не подтягивать */
static long long g_tplf_t0; static int g_tplf_snaps;                /* сколько раз за это окно подтягивали к земле */
#define TPLF_SNAP_MS 2000LL                              /* подтягиваем к земле только первые 2 с после телепорта */
#define TPLF_SNAP_MAX 40
static int tplf_getb(void* obj, const char* getter, int dflt) {
    void *m, *exc = NULL, *r;
    if (!obj) return dflt;
    m = find_method(A.object_get_class(obj), getter, 0);
    if (!m) return dflt;
    r = A.runtime_invoke(m, obj, NULL, &exc);
    if (exc || !r || !hm_readable((char*)r + HDR, 1)) return dflt;
    return *((u8*)r + HDR) ? 1 : 0;
}
static int tplf_setb(void* obj, const char* setter, int v) {
    void *m, *exc = NULL, *args[1]; u8 b = v ? 1 : 0;
    if (!obj) return 0;
    m = find_method(A.object_get_class(obj), setter, 1);
    if (!m) return 0;
    args[0] = &b;
    A.runtime_invoke(m, obj, args, &exc);
    return exc ? 0 : 1;
}
static int tplf_fieldb(void* obj, const char* name, int v) {            /* запасной путь: поле Ability.m_Enabled */
    void* f = find_field(A.object_get_class(obj), name); u8* q;
    if (!f || (A.field_get_flags(f) & 0x10)) return 0;
    q = (u8*)obj + A.field_get_offset(f);
    if (!hm_readable(q, 1)) return 0;
    *q = v ? 1 : 0;
    return 1;
}
static void* tplf_fall(void* me) {                                      /* способность Fall из Character.m_Abilities (List<Ability>) */
    void *ctrl, *list, *items; int n, i; long len; char* data;
    ctrl = me ? ref_field_s(me, "m_Controller") : NULL;                 /* Actor.m_Controller */
    if (!ctrl || !hm_obj_ok(ctrl)) return NULL;
    list = ref_field_s(ctrl, "m_Abilities");
    if (!list || !hm_obj_ok(list)) return NULL;
    items = ref_field_s(list, "_items");
    if (!items || !hm_obj_ok(items)) return NULL;
    n = int_field(list, "_size", "size", NULL, -1);
    len = *(long*)((char*)items + 3 * sizeof(void*));
    if (len <= 0 || len > 64) return NULL;
    if (n < 0 || n > len) n = (int)len;
    data = (char*)items + 4 * sizeof(void*);
    for (i = 0; i < n; i++) {
        void* a = *(void**)(data + (uptr)i * sizeof(void*));
        if (!a || !hm_obj_ok(a)) continue;
        if (seq(A.class_get_name(A.object_get_class(a)), "Fall")) return a;
    }
    return NULL;
}
static void tplf_stop(void* ab) {                                       /* Ability.StopAbility(bool force) */
    void *m, *exc = NULL, *args[1]; u8 force = 1;
    m = find_method(A.object_get_class(ab), "StopAbility", 1);
    if (!m) return;
    args[0] = &force;
    A.runtime_invoke(m, ab, args, &exc);
}
/* Главный поток, сразу после успешного телепорта ПЕШКОМ (из tpl_place) */
static void tplf_guard(const TplCtx* c) {
    void* fall;
    if (!c || c->veh || !c->me) return;
    mt_capture_ctx();
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { dlog("/tpl: нет канала в главный поток — Fall после телепорта не трогаю"); return; }
    fall = tplf_fall(c->me);
    if (!fall) { dlog("/tpl: у персонажа не найдена способность Fall — руки в падении не убрать"); return; }
    if (!__atomic_load_n(&g_tplf_on, __ATOMIC_ACQUIRE)) {
        if (!tplf_getb(fall, "get_Enabled", 1)) return;                  /* уже выключена не нами (например /air) — пусть так и остаётся */
        if (!tplf_setb(fall, "set_Enabled", 0) && !tplf_fieldb(fall, "m_Enabled", 0)) { dlog("/tpl: Fall.Enabled не записался"); return; }
    }
    if (tplf_getb(fall, "get_IsActive", 0)) tplf_stop(fall);
    g_tplf_t0 = gh_now_ms(); g_tplf_snaps = 0; g_tplf_snapOk = !g_tplf_nosnap;
    if (!g_tplw_thread) {
        unsigned long t = 0;
        if (pthread_create(&t, NULL, tplw_worker, NULL) != 0) {
            if (!tplf_setb(fall, "set_Enabled", 1)) tplf_fieldb(fall, "m_Enabled", 1);
            dlog("/tpl: не создался поток — Fall возвращена сразу"); return;
        }
        g_tplw_thread = 1;
    }
    if (!__atomic_load_n(&g_tplf_on, __ATOMIC_ACQUIRE)) dlog("/tpl: Fall выключена на время падения после телепорта");
    __atomic_store_n(&g_tplf_on, 1, __ATOMIC_RELEASE);
}
/* Главный поток, по тику: пора ли вернуть Fall */
static void tplf_tick(long long now) {
    TplCtx c; long long dt = now - g_tplf_t0; int gr, back = 0; void* fall;
    if (!__atomic_load_n(&g_tplf_on, __ATOMIC_ACQUIRE)) return;
    if (!tpl_ctx_q(&c, 0, 1)) { if (dt >= TPLF_MAX_MS) { __atomic_store_n(&g_tplf_on, 0, __ATOMIC_RELEASE); dlog("/tpl: персонажа нет — Fall вернуть не удалось"); } return; }
    gr = c.mv ? tplf_getb(c.mv, "get_isOnGround", -1) : -1;
    if (g_tplf_snapOk && gr == 0 && c.mv && c.mtr && dt < TPLF_SNAP_MS && g_tplf_snaps < TPLF_SNAP_MAX) {   /* всё ещё в воздухе: движение само ставит на пол (SetPosition с updateGround = FindGround + AdjustGroundHeight) */
        float mp[3];
        if (tpl_vec(c.mtr, "get_position", mp) && tpl_call_set(c.mv, mp, 1, "движению (на землю)")) g_tplf_snaps++;
    }
    if (dt < TPLF_MIN_MS) return;
    if (gr == 1 || dt >= TPLF_MAX_MS || (gr < 0 && dt >= TPLF_BLIND_MS)) back = 1;
    if (!back) return;
    fall = tplf_fall(c.me);
    if (fall && !tplf_setb(fall, "set_Enabled", 1) && !tplf_fieldb(fall, "m_Enabled", 1)) dlog("/tpl: Fall.Enabled не вернулся");
    __atomic_store_n(&g_tplf_on, 0, __ATOMIC_RELEASE);
    { char lg[120]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "/tpl: Fall возвращена через мс: "); b_int(&x, (int)dt); b_s(&x, ", подтягиваний к земле: "); b_int(&x, g_tplf_snaps); b_s(&x, gr == 1 ? " (встали на землю)" : gr == 0 ? " (по пределу)" : " (земля не читается)"); dlog(lg); }
}
/* Главный поток, сразу после успешного Entity.SetPosition у машины. from — где была, to — куда ставим. 1 — машина ещё не на точке */
static int tplw_begin(const TplCtx* c, const float* from, const float* to) {
    float cur[3]; int pending = 1, i; long long now = gh_now_ms();
    mt_capture_ctx();                                                    /* нужен главный поток — мы как раз в нём (обработчик команды) */
    if (!g_mt_ctx || !g_mt_postM || !g_mt_kCb) { dlog("/tpl: нет канала в главный поток — за машиной не слежу (сообщение «готово» может быть раньше переноса)"); return 0; }
    if (!g_tplw_thread) {
        unsigned long t = 0;
        if (pthread_create(&t, NULL, tplw_worker, NULL) != 0) { dlog("/tpl: не создался поток наблюдателя"); return 0; }
        g_tplw_thread = 1;
    }
    for (i = 0; i < 3; i++) { g_tplw_from[i] = from[i]; g_tplw_to[i] = to[i]; }
    g_tplw_total = tplw_dxz(from, to);
    g_tplw_t0 = now; g_tplw_tArr = now;
    if (tpl_cur_pos(c, cur) && tplw_dxz(cur, to) <= TPLW_ARRIVE) pending = 0;      /* близкий прыжок: коллизия была загружена, перенос уже случился */
    g_tplw_pending = 0;
    __atomic_store_n(&g_tplw_on, pending ? 1 : 2, __ATOMIC_RELEASE);
    {
        char lg[200]; Buf x; b_init(&x, lg, sizeof lg);
        b_s(&x, "/tpl: слежу за машиной, горизонтально "); b_fix(&x, g_tplw_total, 0); b_s(&x, " м, "); b_s(&x, pending ? "ещё не на точке (ждём подгрузку карты)" : "уже на точке");
        dlog(lg);
    }
    return pending;
}
/* Главный поток, по тику */
static void tplw_tick(void) {
    TplCtx c; float p[3]; long long now = gh_now_ms(); int st; Buf b; char msg[300];
    g_tplw_pending = 0;
    if (__atomic_load_n(&g_tplf_on, __ATOMIC_ACQUIRE)) tplf_tick(now);
    st = __atomic_load_n(&g_tplw_on, __ATOMIC_ACQUIRE);
    if (!st) return;
    if (!tpl_ctx_q(&c, 2, 1) || !c.veh) { tplw_end("вы уже не за рулём"); return; }
    if (!tpl_cur_pos(&c, p)) { if (now - g_tplw_t0 > TPLW_TIMEOUT) tplw_end("позицию машины прочитать не удалось"); return; }
    if (st == 1) {
        float dTo = tplw_dxz(p, g_tplw_to);
        if (dTo <= TPLW_ARRIVE) {
            long long dt = now - g_tplw_t0;
            tpl_stop_vehicle(c.veh);                                     /* машина уже разморожена и стоит на точке: теперь сброс скорости имеет смысл */
            g_tplw_tArr = now;
            __atomic_store_n(&g_tplw_on, 2, __ATOMIC_RELEASE);
            { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "/tpl: машина на точке, ждали мс: "); b_int(&x, (int)dt); dlog(lg); }
            if (dt > 500) {
                b_init(&b, msg, sizeof msg);
                b_s(&b, "Машина на точке (подгрузка карты заняла "); b_fix(&b, (double)dt / 1000.0, 1); b_s(&b, " с)");
                chat_say(msg);
            }
        } else if (now - g_tplw_t0 > TPLW_TIMEOUT) {
            b_init(&b, msg, sizeof msg);
            b_s(&b, "Телепорт машины: за 30 с машина не дошла до точки (осталось "); b_fix(&b, dTo, 0); b_s(&b, " м) — карта в точке не подгрузилась? Подробности в konstt.log");
            chat_say(msg);
            tplw_end("не дошла до точки за тайм-аут");
        }
        return;
    }
    {   /* st == 2: наблюдаем */
        float dFrom = tplw_dxz(p, g_tplw_from), dTo = tplw_dxz(p, g_tplw_to); long long dt = now - g_tplw_tArr;
        if (g_tplw_total > TPLW_BIG && dFrom <= TPLW_ARRIVE) {
            b_init(&b, msg, sizeof msg);
            b_s(&b, "Машину вернуло на прежнее место через "); b_int(&b, (int)dt);
            b_s(&b, " мс после прибытия. К этому моменту /tpl уже поставил её на точку, так что откат пришёл не из кода мода — похоже на серверную проверку телепорта у машин");
            chat_say(msg);
            { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "/tpl: ОТКАТ машины на старое место через мс: "); b_int(&x, (int)dt); dlog(lg); }
            tplw_end("машину вернуло назад");
            return;
        }
        if (dTo > TPLW_FAR) {                                            /* уехала (вы за рулём) или увезло в другое место — в чат не пишем */
            { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "/tpl: машина ушла от точки, м: "); b_int(&x, (int)dTo); dlog(lg); }
            tplw_end("машина ушла от точки");
            return;
        }
        if (dt >= TPLW_WATCH) {
            if (g_tplw_total > TPLW_BIG) chat_say("Машина удержалась на точке (3 с)");
            tplw_end("удержалась");
        }
    }
}

static int gh_fail(const char* why) {
    unsigned i = 0;
    for (; why[i] && i < sizeof g_gh_err - 1; i++) g_gh_err[i] = why[i];
    g_gh_err[i] = 0;
    dlog2("/tpl ghost: перехват не поставлен: ", why);
    return 0;
}

/* Инструкция зависит от своего адреса (при переносе в заглушку сломается)? */
static int gh_pc_rel(u32 w) {
    if ((w & 0x1F000000u) == 0x10000000u) return 1;              /* ADR, ADRP */
    if ((w & 0x3B000000u) == 0x18000000u) return 1;              /* LDR / LDRSW / PRFM (literal) */
    if ((w & 0x7C000000u) == 0x14000000u) return 1;              /* B, BL */
    if ((w & 0xFF000010u) == 0x54000000u) return 1;              /* B.cond */
    if ((w & 0x7E000000u) == 0x34000000u) return 1;              /* CBZ, CBNZ */
    if ((w & 0x7E000000u) == 0x36000000u) return 1;              /* TBZ, TBNZ */
    return 0;
}

#define GH_TRAMP_WORDS 32                                /* место под одну заглушку (занято 22 слова) */
/* Заглушка (arm64). Вход — абсолютным прыжком из начала метода, регистры как при входе (x0 — this, x1 — аргумент, x2 — MethodInfo).
   Сохраняем x0–x3 и x30, зовём gh_hook; результат != 0 → восстанавливаем x30/sp и возвращаемся из метода (ret), иначе восстанавливаем
   x0–x3, выполняем 4 затёртые инструкции и прыгаем на 5-ю инструкцию метода.
     0  stp x0, x1, [sp, #-48]!      1  stp x2, x3, [sp, #16]       2  str x30, [sp, #32]
     3  ldr x16, =gh_hook (слово 18) 4  blr x16                     5  cbnz w0, слово 15
     6  ldr x30, [sp, #32]           7  ldp x2, x3, [sp, #16]       8  ldp x0, x1, [sp], #48
     9-12 четыре затёртые инструкции (позиционно независимые — проверено gh_pc_rel)
    13  ldr x16, =метод+16 (слово 20) 14 br x16
    15  ldr x30, [sp, #32]           16 add sp, sp, #48             17 ret
    18-19 адрес gh_hook              20-21 адрес метода+16 */
static void gh_make_tramp(u32* t, const u32* orig4, uptr hook, uptr resume) {
    int i;
    t[0] = 0xA9BD07E0u;
    t[1] = 0xA9010FE2u;
    t[2] = 0xF90013FEu;
    t[3] = 0x580001F0u;
    t[4] = 0xD63F0200u;
    t[5] = 0x35000140u;
    t[6] = 0xF94013FEu;
    t[7] = 0xA9410FE2u;
    t[8] = 0xA8C307E0u;
    for (i = 0; i < 4; i++) t[9 + i] = orig4[i];
    t[13] = 0x580000F0u;
    t[14] = 0xD61F0200u;
    t[15] = 0xF94013FEu;
    t[16] = 0x9100C3FFu;
    t[17] = 0xD65F03C0u;
    t[18] = (u32)(hook & 0xFFFFFFFFu);
    t[19] = (u32)(((unsigned long long)hook) >> 32);
    t[20] = (u32)(resume & 0xFFFFFFFFu);
    t[21] = (u32)(((unsigned long long)resume) >> 32);
}
/* 16 байт, которыми заменяется начало метода: ldr x16,[pc,#8]; br x16; .quad адрес заглушки */
static void gh_make_entry(u32* e, uptr tramp) {
    e[0] = 0x58000050u;
    e[1] = 0xD61F0200u;
    e[2] = (u32)(tramp & 0xFFFFFFFFu);
    e[3] = (u32)(((unsigned long long)tramp) >> 32);
}

#ifdef GH_ARM64
static void* g_gh_veneer;                                /* страница с заглушками (после снятия остаётся — вдруг поток ещё внутри) */
static uptr g_gh_site[GH_SITES];                         /* адреса методов */
static u32 g_gh_orig[GH_SITES][4];                       /* их исходные 4 слова */
static int g_gh_patched[GH_SITES];

static void gh_flush(void* p, unsigned n) {              /* синхронизация кэшей после записи кода */
    uptr ctr, dl, il, a, e;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    dl = 4u << ((ctr >> 16) & 15u); il = 4u << (ctr & 15u);
    e = (uptr)p + n;
    for (a = (uptr)p & ~(dl - 1); a < e; a += dl) __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = (uptr)p & ~(il - 1); a < e; a += il) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}
/* Записать n слов в код игры; права на страницы возвращаются как были (r-x). Слова пишутся с конца к началу: первым ложится адрес,
   последним — первая инструкция. Запасной путь — /proc/self/mem. */
static int gh_write_code(uptr addr, const u32* w, int n) {
    uptr p0 = addr & ~(uptr)4095, p1 = (addr + (uptr)n * 4 - 1) & ~(uptr)4095;
    unsigned long len = (unsigned long)(p1 - p0) + 4096;
    int i;
    if (mprotect((void*)p0, len, 7) == 0) {
        for (i = n - 1; i >= 0; i--) ((volatile u32*)addr)[i] = w[i];
        gh_flush((void*)addr, (unsigned)n * 4);
        mprotect((void*)p0, len, 5);
        return 1;
    }
    {
        int fd = open("/proc/self/mem", 2); long r = -1;
        if (fd >= 0) { r = pwrite(fd, w, (unsigned long)n * 4, (long)addr); close(fd); }
        if (r == (long)n * 4) { gh_flush((void*)addr, (unsigned)n * 4); return 1; }
    }
    return 0;
}
static int gh_install(void) {
    void* kls; uptr fn[GH_SITES]; u32 code[GH_SITES][4]; int s, i;
    if (g_gh_state == 1) return 1;
    if (!g_img) return gh_fail("игра ещё не готова");
    kls = A.class_from_name(g_img, "Hassle.Players", "LocalPlayer");
    if (!kls) return gh_fail("нет класса LocalPlayer");
    for (s = 0; s < GH_SITES; s++) {
        void* m = A.class_get_method_from_name(kls, g_gh_siteName[s], g_gh_siteArgc[s]);
        if (!m || !hm_readable(m, sizeof(void*))) return gh_fail("нет метода отправки синхронизации");
        fn[s] = *(uptr*)m;
        if (!fn[s] || (fn[s] & 3) || !hm_range_ok((void*)fn[s], 64)) return gh_fail("адрес метода отправки нечитаем");
        for (i = 0; i < 4; i++) code[s][i] = ((const u32*)fn[s])[i];
        if (g_gh_veneer && g_gh_patched[s] && g_gh_site[s] == fn[s]) for (i = 0; i < 4; i++) code[s][i] = g_gh_orig[s][i];   /* уже патчен нами — берём сохранённое */
        if ((code[s][0] & 0xFF0003FFu) != 0xD10003FFu) return gh_fail("пролог метода не начинается с «sub sp, sp, #…» (другая сборка игры?)");
        for (i = 0; i < 4; i++) if (gh_pc_rel(code[s][i])) return gh_fail("пролог метода зависит от адреса — перенести нельзя");
        if (g_gh_veneer && g_gh_site[s] && g_gh_site[s] != fn[s]) return gh_fail("адрес метода изменился после первой установки");
    }
    if (!g_gh_veneer) {
        u32 v[GH_TRAMP_WORDS];
        void* page = mmap(NULL, 4096, 3, 0x22, -1, 0);           /* R|W, MAP_PRIVATE|MAP_ANONYMOUS; адрес любой — прыжок абсолютный */
        if (page == (void*)-1 || !page) return gh_fail("не удалось выделить страницу под заглушки");
        for (s = 0; s < GH_SITES; s++) {
            for (i = 0; i < GH_TRAMP_WORDS; i++) v[i] = 0xD503201Fu;     /* nop */
            gh_make_tramp(v, code[s], (uptr)gh_hook, fn[s] + 16);
            for (i = 0; i < GH_TRAMP_WORDS; i++) ((u32*)page)[s * GH_TRAMP_WORDS + i] = v[i];
        }
        gh_flush(page, (unsigned)(GH_SITES * GH_TRAMP_WORDS * 4));
        if (mprotect(page, 4096, 5) != 0) { munmap(page, 4096); return gh_fail("не удалось сделать страницу заглушек исполняемой"); }
        g_gh_veneer = page;
        for (s = 0; s < GH_SITES; s++) { g_gh_site[s] = fn[s]; for (i = 0; i < 4; i++) g_gh_orig[s][i] = code[s][i]; }
    }
    for (s = 0; s < GH_SITES; s++) {
        u32 e[4];
        if (g_gh_patched[s]) continue;
        gh_make_entry(e, (uptr)((u32*)g_gh_veneer + s * GH_TRAMP_WORDS));
        if (!gh_write_code(fn[s], e, 4)) {
            int k;
            for (k = 0; k < s; k++) if (g_gh_patched[k] && gh_write_code(g_gh_site[k], g_gh_orig[k], 4)) g_gh_patched[k] = 0;   /* откат первого */
            return gh_fail("запись в код игры запрещена системой");
        }
        g_gh_patched[s] = 1;
    }
    g_gh_state = 1;
    dlog("/tpl ghost: перехваты SendOnFootFullSyncData и SendAimSyncData установлены");
    return 1;
}
static void gh_unhook(void) {
    int s, ok = 1;
    __atomic_store_n(&g_gh_on, 0, __ATOMIC_RELEASE);
    for (s = 0; s < GH_SITES; s++)
        if (g_gh_patched[s]) { if (gh_write_code(g_gh_site[s], g_gh_orig[s], 4)) g_gh_patched[s] = 0; else ok = 0; }
    if (ok) { g_gh_state = 0; dlog("/tpl ghost: перехваты сняты, код игры восстановлен"); }
    else dlog("/tpl ghost: не удалось вернуть исходные инструкции");
}
#else
static int gh_install(void) { return gh_fail("поддерживается только arm64"); }
static void gh_unhook(void) { __atomic_store_n(&g_gh_on, 0, __ATOMIC_RELEASE); }
#endif

static void tpl_cmd_ghost(int sub, float secs) {            /* secs — для «tap»: -1 — не указано */
    TplCtx c; Buf b; char msg[300]; int st = __atomic_load_n(&g_gh_on, __ATOMIC_ACQUIRE);
    if (sub == 4) {
        gh_unhook();
        chat_say(g_gh_state ? "Призрак: выключен, но вернуть код игры не удалось (см. konstt.log)" : "Призрак: выключен, перехваты сняты, код игры в исходном виде");
        return;
    }
    if (sub == 5) {                                                  /* tap: окно синхронизации — чтобы наступить на метку */
        long long ms; float cur[3];
        if (!st) { chat_say("Призрак выключен — окно не нужно, сервер и так видит вас. Включить: /tpl ghost on"); return; }
        if (secs < 0) secs = 4.0f;
        if (secs < 1.0f) secs = 1.0f;
        if (secs > 60.0f) secs = 60.0f;
        ms = (long long)(secs * 1000.0f + 0.5f);
        if (tpl_ctx(&c, 0) && tpl_cur_pos(&c, cur)) { g_gh_pos[0] = cur[0]; g_gh_pos[1] = cur[1]; g_gh_pos[2] = cur[2]; g_gh_havePos = 1; }   /* сервер запомнит это место */
        __atomic_store_n(&g_gh_until, gh_now_ms() + ms, __ATOMIC_RELEASE);
        __atomic_store_n(&g_gh_on, 2, __ATOMIC_RELEASE);
        b_init(&b, msg, sizeof msg);
        b_s(&b, "Окно синхронизации "); b_fix(&b, secs, 1); b_s(&b, " с: сервер и NPC видят вас там, где вы стоите. Наступите на метку и не уходите с неё");
        chat_say(msg);
        chat_usage("Потом призрак включится сам (сервер запомнит это место). Закрыть раньше: /tpl ghost on");
        dlog2("/tpl ghost: ", msg);
        return;
    }
    if (sub == 0) sub = st ? 2 : 1;
    if (sub == 1) {
        if (st == 2) {                                               /* «on» во время окна — закрыть его досрочно */
            __atomic_store_n(&g_gh_on, 1, __ATOMIC_RELEASE);
            chat_say("Окно синхронизации закрыто, призрак снова включён");
            return;
        }
        if (st) { chat_say("Призрак уже включён (выключить: /tpl ghost off, вернуться: /tpl ghost back, наступить на метку: /tpl ghost tap)"); return; }
        if (!tpl_ctx(&c, 1)) return;                                 /* пешком: пакеты машины мы не перехватываем */
        if (!tpl_cur_pos(&c, g_gh_pos)) { chat_say("Призрак: не удалось прочитать вашу позицию (подробности в konstt.log)"); return; }
        g_gh_havePos = 1;
        if (!gh_install()) { chat_say("Призрак: не удалось поставить перехват — подробности в konstt.log"); return; }
        __atomic_store_n(&g_gh_on, 1, __ATOMIC_RELEASE);
        b_init(&b, msg, sizeof msg);
        b_s(&b, "Призрак ВКЛЮЧЁН: сервер и NPC считают, что вы стоите здесь: "); tpl_b_xyz(&b, g_gh_pos);
        chat_say(msg);
        chat_usage("Теперь двигайтесь или телепортируйтесь (/tpl …). Не садитесь в машину и не стреляйте — это выдаст позицию. Метки в призраке не срабатывают: /tpl ghost tap. Выключить: /tpl ghost off, вернуться: /tpl ghost back");
        dlog2("/tpl ghost: ", msg);
        return;
    }
    if (!st) { chat_say("Призрак и так выключен"); return; }
    if (sub == 3) {
        if (!g_gh_havePos) { chat_say("Призрак: точка включения неизвестна — выключаю без возврата"); }
        else {
            if (!tpl_ctx(&c, 1)) return;                             /* в машине вернуться нельзя — режим остаётся включённым */
            tpl_goto(g_gh_pos, 3, "включения призрака");
        }
    }
    __atomic_store_n(&g_gh_on, 0, __ATOMIC_RELEASE);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Призрак выключен (не отправлено пакетов: "); b_int(&b, (long)g_gh_hits); b_s(&b, ")");
    if (sub == 2) b_s(&b, ". Сервер получит вашу настоящую позицию — для NPC и игроков вы появитесь на новом месте");
    chat_say(msg);
    dlog2("/tpl ghost: ", msg);
}
static void tpl_status_extra(void) {
    char m[200]; Buf b; b_init(&b, m, sizeof m);
    b_s(&b, "Призрак: "); b_s(&b, g_gh_on == 2 ? "окно синхронизации (tap)" : (g_gh_on ? "ВКЛЮЧЁН" : "выключен"));
    b_s(&b, g_gh_state ? ", перехват стоит" : ", перехват не поставлен");
    b_s(&b, ", пакетов не отправлено: "); b_int(&b, (long)g_gh_hits);
    chat_say(m);
}

/* «/tpl int N»: сменить интерьер на N (только номер). have=0 — номер не указан, показать текущий */
/* Вызвать LocalPlayer.SetInterior(byte). 1 — вызвалось без исключения. Он сам сообщает серверу и включает/выключает «комнатное» освещение. */
static int tpl_do_interior(void* me, int id) {
    void *m, *exc = NULL, *args[1]; u8 v; char et[240];
    m = find_method(A.object_get_class(me), "SetInterior", 1);
    if (!m) { dlog("/tpl int: LocalPlayer.SetInterior не найден"); return 0; }
    v = (u8)id; args[0] = &v;
    A.runtime_invoke(m, me, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("/tpl int: SetInterior бросил исключение: ", et); return 0; }
    return 1;
}
/* «/tpl int N»: сменить интерьер на N (только номер). have=0 — номер не указан, показать текущий */
static void tpl_cmd_int(int id, int have) {
    TplCtx c; Buf b; char msg[300];
    if (!tpl_ctx(&c, 0)) return;
    if (!have) {
        int cur = int_field(c.me, "m_Interior", "m_Interior", NULL, -1);
        b_init(&b, msg, sizeof msg);
        if (cur < 0) b_s(&b, "Интерьер: текущий номер прочитать не удалось. Сменить: /tpl int N");
        else { b_s(&b, "Интерьер сейчас: "); b_int(&b, cur); b_s(&b, ". Сменить: /tpl int N (0 — улица)"); }
        chat_say(msg);
        return;
    }
    if (id < 0 || id > 255) { chat_say("Интерьер: номер от 0 до 255 (0 — улица)"); return; }
    if (!tpl_do_interior(c.me, id)) { chat_say("Интерьер: не вышло — подробности в konstt.log"); return; }
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Интерьер: "); b_int(&b, id);
    if (id == 0) b_s(&b, " (улица): дневной свет, серверу сообщено");
    else b_s(&b, ": включено освещение «в помещении», серверу сообщено. Сам мир не менялся — номер это метка, а не место. Вернуть улицу: /tpl int 0");
    chat_say(msg);
    dlog2("/tpl int: ", msg);
}

/* ───────────── разбор аргументов ───────────── */
typedef struct { int side, sideSet, alt, status, help, numSet, bad, clamped, mode, nc, nameSet, unkN, gsub, iid, iidSet, pkList; float dist, rawNum; float c[3]; char name[TPL_NAME], unk[TPL_NAME]; } TplArgs;
/* mode: 0 — сдвиг, 1 — pos, 2 — to X Y Z, 3 — save имя, 4 — go имя, 5 — list, 6 — del имя, 7 — ghost, 8 — int, 9 — gps, 10 — pick (pkList — слово list после pick).
   gsub (для ghost): 0 — переключить, 1 on, 2 off, 3 back, 4 unhook;  iid/iidSet — номер интерьера;  rawNum — число до подрезки по TPL_MIN (для «/tpl 0 int»).  name — имя точки (слово после save/go/del);
   unk/unkN — первое неизвестное слово и сколько их было: это может быть имя точки («/tpl дом», «/tpl дом save») */

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
/* слово → сторона 0..3; 10 alt; 11 status; 12 help; 13 pos; 14 to; 15 save; 16 go; 17 list; 18 del; 19 ghost; 20 int; 21 gps; 22 pick; -1 не знаем */
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
    if (seq(t, "save") || seq(t, "set") || seq(t, "add") || seq(t, "сохр") || seq(t, "сохранить") || seq(t, "запомни") || seq(t, "запомнить")) return 15;
    if (seq(t, "go") || seq(t, "иди") || seq(t, "вернуться")) return 16;
    if (seq(t, "list") || seq(t, "ls") || seq(t, "points") || seq(t, "список") || seq(t, "точки")) return 17;
    if (seq(t, "del") || seq(t, "delete") || seq(t, "rm") || seq(t, "remove") || seq(t, "удали") || seq(t, "удалить")) return 18;
    if (seq(t, "ghost") || seq(t, "gh") || seq(t, "призрак") || seq(t, "невидимка") || seq(t, "hide")) return 19;
    if (seq(t, "int") || seq(t, "interior") || seq(t, "инт") || seq(t, "интерьер")) return 20;
    if (seq(t, "gps") || seq(t, "гпс") || seq(t, "метка") || seq(t, "mark") || seq(t, "waypoint") || seq(t, "wp")) return 21;
    if (seq(t, "pick") || seq(t, "pickup") || seq(t, "pickups") || seq(t, "pk") || seq(t, "пикап") || seq(t, "пикапы") ||
        seq(t, "маркер") || seq(t, "маркеры") || seq(t, "marker") || seq(t, "markers") || seq(t, "mk")) return 22;
    return -1;
}
/* подслово после «ghost»: 1 on, 2 off, 3 back, 4 unhook, 5 tap, 0 — не оно */
static int tpl_gword(const char* t) {
    if (seq(t, "on") || seq(t, "вкл") || seq(t, "включить")) return 1;
    if (seq(t, "off") || seq(t, "выкл") || seq(t, "выключить")) return 2;
    if (seq(t, "back") || seq(t, "назад") || seq(t, "вернись") || seq(t, "возврат") || seq(t, "return")) return 3;
    if (seq(t, "unhook") || seq(t, "снять")) return 4;
    if (seq(t, "tap") || seq(t, "sync") || seq(t, "окно") || seq(t, "метка") || seq(t, "маркер")) return 5;
    return 0;
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
    if (a->mode == 10 && (w == 14 || w == 16 || w == 17)) { if (w == 17) a->pkList = 1; return; }   /* «pick list», «pick go N» — не смена команды */
    if (w >= 0 && w <= 3) { a->side = w; a->sideSet = 1; }
    else if (w == 10) a->alt = 1;
    else if (w == 11) a->status = 1;
    else if (w == 12) a->help = 1;
    else if (w == 13) a->mode = 1;
    else if (w == 14) a->mode = 2;
    else if (w == 15) a->mode = 3;
    else if (w == 16) a->mode = 4;
    else if (w == 17) a->mode = 5;
    else if (w == 18) a->mode = 6;
    else if (w == 19) a->mode = 7;
    else if (w == 20) a->mode = 8;
    else if (w == 21) a->mode = 9;
    else if (w == 22) { if (a->mode == 5) a->pkList = 1; a->mode = 10; }
}
static void tpl_token(const char* tok, TplArgs* a) {
    float v; int w, k;
    if (!tok[0] || tok[0] == '/' || seq(tok, "tpl")) return;       /* сама команда может входить в массив */
    if ((a->mode == 3 || a->mode == 4 || a->mode == 6) && !a->nameSet) {   /* слово сразу после save/go/del — имя точки, что бы оно ни значило */
        tpl_copy_name(a->name, tok, TPL_NAME); a->nameSet = a->name[0] ? 1 : 0;
        return;
    }
    if (a->mode == 7) { int g = tpl_gword(tok); if (g) { a->gsub = g; return; } }     /* после «ghost»: on / off / back / unhook / tap */
    if (a->mode == 8 && tpl_num(tok, &v)) { a->iid = (int)(v + 0.5f); a->iidSet = 1; return; }   /* после «int»: номер (0 — валидный, не подрезаем) */
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
    if (!a->unkN) tpl_copy_name(a->unk, tok, TPL_NAME);            /* неизвестное слово: возможно, имя точки */
    a->unkN++;
    a->bad = 1;
}
static void tpl_args_init(TplArgs* a) {
    a->side = TPL_L; a->sideSet = a->alt = a->status = a->help = a->numSet = a->bad = a->clamped = a->mode = a->nc = a->nameSet = a->unkN = a->gsub = a->iid = a->iidSet = a->pkList = 0; a->dist = TPL_DEF; a->rawNum = -1.0f;
    a->c[0] = a->c[1] = a->c[2] = 0.0f; a->name[0] = a->unk[0] = 0;
}
/* аргументы: string[] */
static void tpl_parse(void* arr, TplArgs* a) {
    long len; int i; char* data;
    tpl_args_init(a);
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
    a->rawNum = a->numSet ? a->dist : -1.0f;
    if (a->dist < TPL_MIN) a->dist = TPL_MIN;
    if (a->dist > TPL_MAX) { a->dist = TPL_MAX; a->clamped = 1; }
}

/* ───────────── сохранённые точки ───────────── */
typedef struct { char name[TPL_NAME]; float p[3]; int in; } TplPt;      /* in — номер интерьера точки (-1 — не известен, интерьер не трогаем) */
static TplPt g_tpl_pts[TPL_MAXP];
static int g_tpl_np, g_tpl_loaded;

/* путь к файлу: папка журнала (HLOG) + имя, как у route.txt. 0 — журнал ещё не открыт/папки нет */
static int tpl_pts_path(char* out, int cap) {
    int i, last = -1; Buf b;
    if (!g_flog[0]) return 0;
    for (i = 0; g_flog[i]; i++) if (g_flog[i] == '/') last = i;
    if (last < 0 || last + (int)sizeof TPL_PTS_FILE + 8 > cap) return 0;
    b_init(&b, out, (unsigned)cap);
    for (i = 0; i <= last; i++) b_ch(&b, g_flog[i]);
    b_s(&b, TPL_PTS_FILE);
    return 1;
}
/* одна строка файла «имя x y z» или «имя x y z N» (N — номер интерьера) → точка; 1 — разобрано (лишнее и мусор пропускаются) */
static int tpl_pts_line(const char* line, TplPt* out) {
    char tok[6][40]; int n = 0, i = 0, in = -1; float v[3], fi;
    while (line[i] && n < 6) {
        int q = 0;
        while (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') i++;
        if (!line[i]) break;
        if (n == 5) return 0;                                       /* шестое слово — не наша строка */
        while (line[i] && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') { if (q < 39) tok[n][q++] = line[i]; i++; }
        tok[n][q] = 0; n++;
    }
    if (n != 4 && n != 5) return 0;
    if (!tpl_snum(tok[1], &v[0]) || !tpl_snum(tok[2], &v[1]) || !tpl_snum(tok[3], &v[2])) return 0;
    if (!tpl_finite(v)) return 0;
    if (n == 5) { if (!tpl_num(tok[4], &fi) || fi < 0.0f || fi > 255.0f) return 0; in = (int)(fi + 0.5f); }
    tpl_copy_name(out->name, tok[0], TPL_NAME);
    tpl_fold(out->name);                                            /* файл правят руками: «Дом» = «дом» */
    out->p[0] = v[0]; out->p[1] = v[1]; out->p[2] = v[2];
    out->in = in;
    return out->name[0] ? 1 : 0;
}
static void tpl_pts_load(void) {
    static char buf[8192]; char path[320]; int fd; long tot = 0; char* p;
    if (g_tpl_loaded) return;
    g_tpl_loaded = 1; g_tpl_np = 0;
    if (!tpl_pts_path(path, sizeof path)) { g_tpl_loaded = 0; return; }      /* папки пока нет — попробуем в следующий раз */
    fd = open(path, 0);
    if (fd < 0) return;                                             /* файла ещё нет — точек нет */
    for (;;) { long r = read(fd, buf + tot, sizeof buf - 1 - (unsigned long)tot); if (r <= 0) break; tot += r; if (tot >= (long)sizeof buf - 1) break; }
    close(fd);
    buf[tot] = 0;
    for (p = buf; *p && g_tpl_np < TPL_MAXP; ) {
        char* e = p; char keep;
        while (*e && *e != '\n') e++;
        keep = *e; *e = 0;
        if (tpl_pts_line(p, &g_tpl_pts[g_tpl_np])) g_tpl_np++;
        if (!keep) break;
        p = e + 1;
    }
    dlog_i("/tpl: точек загружено из файла: ", g_tpl_np);
}
/* записать файл целиком и убедиться, что записалось всё */
static int tpl_write_all(const char* path, const char* data, unsigned n) {
    int fd = open(path, 0x241, 0666);                               /* O_WRONLY|O_CREAT|O_TRUNC */
    long w;
    if (fd < 0) return 0;
    w = n ? write(fd, data, n) : 0;
    close(fd);
    return w == (long)n;
}
/* сохранить список: во временный файл, затем rename — вылет посреди записи старый файл не портит. 0 — не вышло (точки остаются в памяти до перезапуска) */
static int tpl_pts_save(void) {
    static char buf[8192]; char path[320], tmp[336]; Buf b, t; int i;
    if (!tpl_pts_path(path, sizeof path)) return 0;
    b_init(&b, buf, sizeof buf);
    for (i = 0; i < g_tpl_np; i++) {
        b_s(&b, g_tpl_pts[i].name); b_ch(&b, ' ');
        b_fix(&b, g_tpl_pts[i].p[0], 3); b_ch(&b, ' '); b_fix(&b, g_tpl_pts[i].p[1], 3); b_ch(&b, ' '); b_fix(&b, g_tpl_pts[i].p[2], 3);
        if (g_tpl_pts[i].in >= 0) { b_ch(&b, ' '); b_int(&b, g_tpl_pts[i].in); }
        b_ch(&b, '\n');
    }
    b_init(&t, tmp, sizeof tmp); b_s(&t, path); b_s(&t, ".tmp");
    if (tpl_write_all(tmp, b.p, b.n) && rename(tmp, path) == 0) return 1;
    return tpl_write_all(path, b.p, b.n);                          /* rename не получился — пишем прямо */
}
static int tpl_find(const char* name) {
    int i;
    for (i = 0; i < g_tpl_np; i++) if (seq(g_tpl_pts[i].name, name)) return i;
    return -1;
}
/* слово разбирается как команда/число (не как «неизвестное слово») → короткая форма «/tpl имя» для него не сработает */
static int tpl_name_shadowed(const char* name) {
    TplArgs t; tpl_args_init(&t); tpl_token(name, &t);
    return !(t.bad && t.unkN == 1);
}

static void tpl_cmd_save(const char* name) {
    TplCtx c; float p[3]; int i, upd, saved, src; Buf b; char msg[300];
    if (!tpl_ctx(&c, 0)) return;
    src = tpl_cur_pos(&c, p);
    if (!src) { chat_say("Точка: не удалось прочитать ваши координаты (подробности в konstt.log)"); return; }
    tpl_pts_load();
    i = tpl_find(name); upd = i >= 0;
    if (!upd) {
        if (g_tpl_np >= TPL_MAXP) { chat_say("Точка: уже 64 точки — удалите ненужные (/tpl del имя)"); return; }
        i = g_tpl_np++;
        tpl_copy_name(g_tpl_pts[i].name, name, TPL_NAME);
    }
    g_tpl_pts[i].p[0] = p[0]; g_tpl_pts[i].p[1] = p[1]; g_tpl_pts[i].p[2] = p[2];
    g_tpl_pts[i].in = int_field(c.me, "m_Interior", "m_Interior", NULL, -1);      /* интерьер запоминаем вместе с местом */
    saved = tpl_pts_save();
    b_init(&b, msg, sizeof msg);
    b_s(&b, "Точка «"); b_s(&b, g_tpl_pts[i].name); b_s(&b, upd ? "» обновлена: " : "» сохранена: "); tpl_b_xyz(&b, p);
    if (g_tpl_pts[i].in >= 0) { b_s(&b, ", интерьер "); b_int(&b, g_tpl_pts[i].in); }
    b_s(&b, ". Вернуться: /tpl go "); b_s(&b, g_tpl_pts[i].name);
    if (tpl_name_shadowed(g_tpl_pts[i].name)) b_s(&b, " (коротко «/tpl имя» для этого слова не сработает)");
    if (!saved) b_s(&b, " — В ФАЙЛ ЗАПИСАТЬ НЕ УДАЛОСЬ, точка живёт до перезапуска игры");
    chat_say(msg);
    dlog2("/tpl: ", msg);
    if (src == 2) dlog("/tpl save: позиция взята из пакета синхронизации (Transform не прочитался)");
}
static void tpl_cmd_go(const char* name) {
    int i;
    tpl_pts_load();
    i = tpl_find(name);
    if (i < 0) {
        char msg[120]; Buf b; b_init(&b, msg, sizeof msg);
        b_s(&b, "Точки «"); b_s(&b, name); b_s(&b, "» нет. Список: /tpl list");
        chat_say(msg); return;
    }
    if (tpl_goto(g_tpl_pts[i].p, 3, g_tpl_pts[i].name) && g_tpl_pts[i].in >= 0) {      /* у точки известен интерьер — включаем его */
        TplCtx c;
        if (tpl_ctx_q(&c, 0, 1)) {
            int cur = int_field(c.me, "m_Interior", "m_Interior", NULL, -1);
            if (cur != g_tpl_pts[i].in && tpl_do_interior(c.me, g_tpl_pts[i].in)) {
                char m2[120]; Buf b2; b_init(&b2, m2, sizeof m2);
                b_s(&b2, "Интерьер точки: "); b_int(&b2, g_tpl_pts[i].in);
                chat_say(m2);
            }
        }
    }
}
static void tpl_cmd_del(const char* name) {
    int i, saved; char msg[160]; Buf b;
    tpl_pts_load();
    i = tpl_find(name);
    b_init(&b, msg, sizeof msg);
    if (i < 0) { b_s(&b, "Точки «"); b_s(&b, name); b_s(&b, "» нет. Список: /tpl list"); chat_say(msg); return; }
    b_s(&b, "Точка «"); b_s(&b, g_tpl_pts[i].name); b_s(&b, "» удалена");
    for (; i + 1 < g_tpl_np; i++) g_tpl_pts[i] = g_tpl_pts[i + 1];
    g_tpl_np--;
    saved = tpl_pts_save();
    if (!saved) b_s(&b, " (файл обновить не удалось)");
    chat_say(msg);
}
static void tpl_cmd_list(int page) {
    TplCtx c; float me[3]; int i, from, to, pages, haveMe = 0; char msg[220]; Buf b;
    tpl_pts_load();
    if (!g_tpl_np) { chat_say("Точек нет. Сохранить текущее место: /tpl save имя"); return; }
    pages = (g_tpl_np + TPL_PAGE - 1) / TPL_PAGE;
    if (page < 1) page = 1;
    if (page > pages) page = pages;
    from = (page - 1) * TPL_PAGE; to = from + TPL_PAGE; if (to > g_tpl_np) to = g_tpl_np;
    if (tpl_ctx_q(&c, 0, 1)) haveMe = tpl_cur_pos(&c, me) == 1;     /* вне игры список всё равно показываем, только без расстояний */
    b_init(&b, msg, sizeof msg); b_s(&b, "Точки: "); b_int(&b, g_tpl_np);
    if (pages > 1) { b_s(&b, ", страница "); b_int(&b, page); b_s(&b, " из "); b_int(&b, pages); }
    chat_say(msg);
    for (i = from; i < to; i++) {
        b_init(&b, msg, sizeof msg);
        b_s(&b, "«"); b_s(&b, g_tpl_pts[i].name); b_s(&b, "»: "); tpl_b_xyz(&b, g_tpl_pts[i].p);
        if (g_tpl_pts[i].in > 0) { b_s(&b, " [инт "); b_int(&b, g_tpl_pts[i].in); b_ch(&b, ']'); }
        if (haveMe) {
            double dx = g_tpl_pts[i].p[0] - me[0], dy = g_tpl_pts[i].p[1] - me[1], dz = g_tpl_pts[i].p[2] - me[2];
            b_s(&b, " — в "); b_fix(&b, __builtin_sqrt(dx * dx + dy * dy + dz * dz), 0); b_s(&b, " м");
        }
        chat_say(msg);
    }
    if (page < pages) { b_init(&b, msg, sizeof msg); b_s(&b, "Дальше: /tpl list "); b_int(&b, page + 1); chat_usage(msg); }
}

/* ───────────── метка GPS и пикапы/маркеры (/tpl gps, /tpl pick) ───────────── */
#define TPL_PKMAX 200                                    /* пикапов и маркеров в снимке списка */
#define TPL_LBMAX 256                                    /* 3D-подписей, которые читаем, чтобы назвать пикап */
#define TPL_LBNEAR 4.0f                                  /* м: подпись не дальше этого от пикапа считается его названием */
typedef struct { int id, kind, mid; float p[3], d; char lab[56], mdl[32]; } TplPk;   /* kind: 0 — пикап, 1 — маркер (чекпоинт), 2 — гоночный маркер */
typedef struct { float p[3]; char t[56]; } TplLb;
static int g_tpl_why, g_tpl_nch;                                  /* почему модель не прочиталась: 1 нет Transform, 2 нет детей, 3 нет GetChild, 4 имя не прочиталось */
static int g_tpl_log_models = 1;                                  /* писать в konstt.log имя модели каждого пикапа (чтобы сверить формат имён на устройстве) */
static TplPk g_tpl_pk[TPL_PKMAX]; static int g_tpl_npk;       /* снимок списка: номера в «/tpl pick N» — это индексы отсюда (с единицы) */
static TplLb g_tpl_lb[TPL_LBMAX]; static int g_tpl_nlb;
static float g_tpl_pkRef[3];

static float tpl_d3(const float* a, const float* b) {
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return (float)__builtin_sqrt(dx * dx + dy * dy + dz * dz);
}
static void* tpl_pool(const char* field) {                          /* Pools.<field>; NULL — не прочиталось */
    void *cls, *f, *pool = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    f = cls ? A.class_get_field_from_name(cls, field) : NULL;
    if (!f) return NULL;
    A.field_static_get_value(f, &pool);
    return (pool && hm_obj_ok(pool)) ? pool : NULL;
}
/* UI.MiniMap (статический геттер, запасной путь — UI.m_Instance.m_MiniMap) */
static void* tpl_minimap(void) {
    void *cls, *m, *exc = NULL, *r, *f, *inst = NULL;
    if (!g_safe || !il2cpp_light() || !g_img) return NULL;
    ensure_attached();
    cls = A.class_from_name(g_img, "Hassle.Graphics.UserInterfaces", "UI");
    if (!cls) return NULL;
    m = find_method(cls, "get_MiniMap", 0);
    if (m) { r = A.runtime_invoke(m, NULL, NULL, &exc); if (!exc && r && hm_obj_ok(r)) return r; }
    f = A.class_get_field_from_name(cls, "m_Instance");
    if (f) {
        A.field_static_get_value(f, &inst);
        if (inst && hm_obj_ok(inst)) { r = ref_field(inst, "m_MiniMap"); if (r && hm_obj_ok(r)) return r; }
    }
    return NULL;
}
/* MiniMap.m_NavigationPoint — точка GPS в мировых координатах Unity (x, высота, z). Её пишет MiniMap.GetNavigationPath(x, y, z) как (x, z, y):
   JS и сервер говорят в системе SAMP (x, y — плоскость, z — высота), а Unity — x, z плоскость, y высота. active: m_IsNavigationActive (-1 — поля нет). */
static int tpl_nav_point(void* mm, float* p, int* active) {
    void* k = A.object_get_class(mm); void* f = find_field(k, "m_NavigationPoint"); void* fa; const float* v;
    if (active) *active = -1;
    if (!f) return 0;
    v = (const float*)((char*)mm + A.field_get_offset(f));
    if (!hm_readable(v, 12) || !tpl_finite(v)) return 0;
    p[0] = v[0]; p[1] = v[1]; p[2] = v[2];
    fa = find_field(k, "m_IsNavigationActive");
    if (fa && active) { const u8* bp = (const u8*)mm + A.field_get_offset(fa); if (hm_readable(bp, 1)) *active = *bp ? 1 : 0; }
    return 1;
}
/* GameObject.activeInHierarchy у компонента; не прочиталось — считаем активным */
static int tpl_active(void* comp) {
    void *go = tpl_obj(comp, "get_gameObject"), *m, *exc = NULL, *r;
    if (!go) return 0;
    m = find_method(A.object_get_class(go), "get_activeInHierarchy", 0);
    if (!m) return 1;
    r = A.runtime_invoke(m, go, NULL, &exc);
    return (!exc && r && hm_readable((char*)r + HDR, 1)) ? (*((u8*)r + HDR) != 0) : 1;
}
/* Checkpoint.Simple / Checkpoint.Race — маркер, который сервер сейчас показал (SetPlayerCheckpoint / SetPlayerRaceCheckpoint); он один на игрока */
static int tpl_chk_pos(const char* fld, float* out) {
    void *cls, *f, *obj = NULL, *tr;
    if (!g_safe || !il2cpp_light() || !g_img) return 0;
    cls = A.class_from_name(g_img, "Hassle.Entities", "Checkpoint");
    f = cls ? A.class_get_field_from_name(cls, fld) : NULL;
    if (!f) return 0;
    A.field_static_get_value(f, &obj);
    if (!obj || !hm_obj_ok(obj) || !tpl_active(obj)) return 0;
    tr = tpl_obj(obj, "get_transform");
    if (!tr || !tpl_vec(tr, "get_position", out)) return 0;
    return !(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f);   /* спрятанный маркер уезжает в ноль */
}
/* текст подписи: без цветовых кодов {RRGGBB}, переводы строк и табы → пробел, обрезка не рвёт русскую букву */
static void tpl_clean_text(const char* src, char* dst, int cap) {
    char t[200]; int i = 0, k = 0, sp = 1;
    while (src[i] && k < (int)sizeof t - 1) {
        if (src[i] == '{') {
            int j = i + 1;
            while (src[j] && src[j] != '}' && j - i < 10) j++;
            if (src[j] == '}') { i = j + 1; continue; }
        }
        if (src[i] == '\n' || src[i] == '\r' || src[i] == '\t' || src[i] == ' ') { if (!sp) t[k++] = ' '; sp = 1; i++; continue; }
        t[k++] = src[i++]; sp = 0;
    }
    while (k > 0 && t[k - 1] == ' ') k--;
    t[k] = 0;
    tpl_copy_name(dst, t, cap);
}
/* 3D-подпись: Text3DLabel.m_CreateParams (Position — мировая точка: по LateUpdate её сразу отдают в Camera.WorldToViewportPoint; Text — строка) */
static void tpl_lb_cb(void* it, void* ctx) {
    void *sf, *sc, *pf, *tf, *s; char* sp; int shift, poff, toff, n; const u16* ch; char tmp[220]; TplLb* e;
    (void)ctx;
    if (g_tpl_nlb >= TPL_LBMAX || !hm_obj_ok(it)) return;
    sf = find_field(A.object_get_class(it), "m_CreateParams");
    if (!sf) return;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return;
    pf = find_field(sc, "Position"); tf = find_field(sc, "Text");
    if (!pf || !tf) return;
    sp = (char*)it + A.field_get_offset(sf);
    shift = struct_shift(sc);
    poff = A.field_get_offset(pf) - shift; toff = A.field_get_offset(tf) - shift;
    if (!hm_readable(sp, 48)) return;
    s = *(void**)(sp + toff);
    if (!s || !hm_obj_ok(s)) return;
    n = A.string_length(s); ch = A.string_chars(s);
    if (n <= 0 || n > 400 || !ch) return;
    if (n > 160) n = 160;
    utf16_to_utf8(ch, n, tmp, sizeof tmp);
    e = &g_tpl_lb[g_tpl_nlb];
    e->p[0] = ((float*)(sp + poff))[0]; e->p[1] = ((float*)(sp + poff))[1]; e->p[2] = ((float*)(sp + poff))[2];
    if (!tpl_finite(e->p)) return;
    tpl_clean_text(tmp, e->t, sizeof e->t);
    if (e->t[0]) g_tpl_nlb++;
}
/* ───── модель пикапа ─────
   Клиент не знает, что делает пикап: сервер шлёт ему только ID, номер модели, тип и позицию (CreatePickupParams). Показываем ID, номер модели и 3D-подпись рядом (если сервер её повесил).
   Номер модели: PickupPool.Create кладёт в объект пикапа экземпляр префаба модели (Instantiate), его имя читаем у дочернего Transform. */
/* имя модели пикапа (дочерний объект) → raw; номер модели из первой цифровой группы в имени (3…5 цифр), иначе 0 */
static int tpl_pk_model(void* pk, char* raw, int cap) {
    void *tr, *m, *exc = NULL, *r, *ch, *nm; int n = 0, i, mid = 0, k, len; const u16* chs; char tmp[96];
    raw[0] = 0; g_tpl_why = 0; g_tpl_nch = 0;
    tr = tpl_obj(pk, "get_transform");
    if (!tr) { g_tpl_why = 1; return 0; }
    m = find_method(A.object_get_class(tr), "get_childCount", 0);
    if (m) { r = A.runtime_invoke(m, tr, NULL, &exc); if (!exc && r && hm_readable((char*)r + HDR, 4)) n = *(int*)((char*)r + HDR); }
    g_tpl_nch = n;
    if (n < 1 || n > 8) { g_tpl_why = 2; return 0; }
    m = find_method(A.object_get_class(tr), "GetChild", 1);
    if (!m) { g_tpl_why = 3; return 0; }
    for (i = 0; i < n && !raw[0]; i++) {
        void* args[1]; int idx = i; void* gm;
        args[0] = &idx; exc = NULL;
        ch = A.runtime_invoke(m, tr, args, &exc);
        if (exc || !ch || !hm_obj_ok(ch)) continue;
        gm = find_method(A.object_get_class(ch), "get_name", 0);
        if (!gm) continue;
        exc = NULL; nm = A.runtime_invoke(gm, ch, NULL, &exc);
        if (exc || !nm || !hm_obj_ok(nm)) continue;
        len = A.string_length(nm); chs = A.string_chars(nm);
        if (len <= 0 || len > 80 || !chs) continue;
        utf16_to_utf8(chs, len, tmp, sizeof tmp);
        tpl_clean_text(tmp, raw, cap);
        { int L = 0; while (raw[L]) L++;                                  /* «(Clone)» в конце имени — убрать вместе с пробелом перед ним */
          if (L >= 7 && raw[L - 7] == '(' && raw[L - 6] == 'C' && raw[L - 5] == 'l' && raw[L - 4] == 'o' && raw[L - 3] == 'n' && raw[L - 2] == 'e' && raw[L - 1] == ')') {
              L -= 7; while (L > 0 && raw[L - 1] == ' ') L--;
              raw[L] = 0;
          } }
    }
    if (!raw[0]) g_tpl_why = 4;
    for (k = 0; raw[k]; k++) {
        if (raw[k] >= '0' && raw[k] <= '9') {
            int j = k, v = 0; while (raw[j] >= '0' && raw[j] <= '9' && j - k < 6) { v = v * 10 + (raw[j] - '0'); j++; }
            if (j - k >= 3) { mid = v; break; }
            k = j;
        }
    }
    return mid;
}
/* вставить в снимок по расстоянию от g_tpl_pkRef; подпись — ближайшая 3D-подпись не дальше TPL_LBNEAR */
static void tpl_pk_add(int id, int kind, const float* p, const char* mdl, int mid) {
    int i, j, bi = -1; float d, bd = TPL_LBNEAR + 0.001f; TplPk* e;
    d = tpl_d3(p, g_tpl_pkRef);
    for (i = 0; i < g_tpl_npk && g_tpl_pk[i].d <= d; i++) {}
    if (i >= TPL_PKMAX) return;
    for (j = (g_tpl_npk < TPL_PKMAX ? g_tpl_npk : TPL_PKMAX - 1); j > i; j--) g_tpl_pk[j] = g_tpl_pk[j - 1];
    e = &g_tpl_pk[i];
    e->id = id; e->kind = kind; e->d = d; e->p[0] = p[0]; e->p[1] = p[1]; e->p[2] = p[2]; e->lab[0] = 0; e->mid = mid;
    tpl_copy_name(e->mdl, mdl ? mdl : "", (int)sizeof e->mdl);
    for (j = 0; j < g_tpl_nlb; j++) { float dl = tpl_d3(p, g_tpl_lb[j].p); if (dl < bd) { bd = dl; bi = j; } }
    if (bi >= 0) tpl_copy_name(e->lab, g_tpl_lb[bi].t, (int)sizeof e->lab);
    if (g_tpl_npk < TPL_PKMAX) g_tpl_npk++;
}
static void tpl_pk_cb(void* it, void* ctx) {
    float p[3]; void* tr;
    (void)ctx;
    if (!hm_obj_ok(it)) return;
    tr = tpl_obj(it, "get_transform");
    if (!tr || !tpl_vec(tr, "get_position", p)) return;
    { char raw[40]; int mid = tpl_pk_model(it, raw, sizeof raw); int pid = (int)num_field(it, "m_ID", NULL);
      if (g_tpl_log_models) { Buf x; char lg[140]; b_init(&x, lg, sizeof lg); b_s(&x, "tpl: pick: ID "); b_int(&x, pid); b_s(&x, ", детей "); b_int(&x, g_tpl_nch); b_s(&x, ", имя модели «"); b_s(&x, raw); b_s(&x, "», номер "); b_int(&x, mid); b_s(&x, ", причина "); b_int(&x, g_tpl_why); dlog(lg); }
      tpl_pk_add(pid, 0, p, raw[0] ? raw : (g_tpl_why ? "?" : ""), mid); }
}
/* Собрать снимок: пикапы (Pools.Pickups) + текущий маркер сервера. 1 — пул пикапов прочитался */
static int tpl_pk_scan(void) {
    TplCtx c; void *pool, *dict, *arr; int total, ok = 0; float cp[3];
    g_tpl_pkRef[0] = g_tpl_pkRef[1] = g_tpl_pkRef[2] = 0.0f;
    if (tpl_ctx_q(&c, 0, 1)) { float me[3]; if (tpl_cur_pos(&c, me) == 1) { g_tpl_pkRef[0] = me[0]; g_tpl_pkRef[1] = me[1]; g_tpl_pkRef[2] = me[2]; } }
    g_tpl_npk = 0; g_tpl_nlb = 0;
    pool = tpl_pool("Text3DLables");                                /* (sic) так поле называется в игре */
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (arr) { total = int_field(dict, "_count", "count", NULL, -1); walk_struct_array(arr, total, "value", tpl_lb_cb, NULL); }
    pool = tpl_pool("Pickups");
    dict = pool ? ref_field_s(pool, "m_Items") : NULL;
    arr = dict ? ref_field_s(dict, "_entries") : NULL;
    if (arr) { total = int_field(dict, "_count", "count", NULL, -1); walk_struct_array(arr, total, "value", tpl_pk_cb, NULL); ok = 1; }
    if (tpl_chk_pos("Simple", cp)) tpl_pk_add(0, 1, cp, NULL, 0);
    if (tpl_chk_pos("Race", cp)) tpl_pk_add(0, 2, cp, NULL, 0);
    return ok;
}
static void tpl_pk_line(int i) {
    Buf b; char msg[260]; const TplPk* e = &g_tpl_pk[i];
    b_init(&b, msg, sizeof msg);
    b_int(&b, i + 1); b_s(&b, ". ");
    if (e->kind == 0) {
        b_s(&b, "ID "); b_int(&b, e->id);
        b_s(&b, "  [модель "); if (e->mid) b_int(&b, e->mid); else b_s(&b, e->mdl[0] ? e->mdl : "?"); b_ch(&b, ']');
    } else b_s(&b, e->kind == 2 ? "гоночный маркер" : "маркер");
    b_s(&b, " — "); b_fix(&b, e->d, 0); b_s(&b, " м");
    if (e->lab[0]) { b_s(&b, "  «"); b_s(&b, e->lab); b_s(&b, "»"); }
    b_s(&b, "  ("); tpl_b_xyz(&b, e->p); b_ch(&b, ')');
    chat_say(msg);
}
/* «/tpl pick» — обновить и показать список ближайших; «/tpl pick list [стр]» — то же (стр > 1 листает прежний снимок, не пересобирая);
   «/tpl pick N» — телепорт к N-му из последнего списка (если списка ещё не было — покажет его). num < 0 — числа не было. */
static void tpl_cmd_pick(int num, int list) {
    TplCtx c; int i, from, to, pages, page = 1, rescan = 1; Buf b; char msg[220];
    if (!tpl_ctx(&c, 0)) return;
    if (num >= 0 && !list && g_tpl_npk) {
        char lb[100]; Buf x;
        if (num < 1 || num > g_tpl_npk) {
            b_init(&b, msg, sizeof msg); b_s(&b, "Пикап/маркер №"); b_int(&b, num); b_s(&b, " — такого номера нет, в списке 1…"); b_int(&b, g_tpl_npk);
            b_s(&b, " (обновить список: /tpl pick)"); chat_say(msg); return;
        }
        b_init(&x, lb, sizeof lb); b_ch(&x, '#'); b_int(&x, num);
        if (g_tpl_pk[num - 1].lab[0]) { b_ch(&x, ' '); b_s(&x, g_tpl_pk[num - 1].lab); }
        g_tplf_nosnap = 1; tpl_goto_ex(g_tpl_pk[num - 1].p, 3, lb, 0); g_tplf_nosnap = 0;   /* как было: ровно на высоту пикапа, без подтягивания к земле */
        return;
    }
    if (list && num > 1 && g_tpl_npk) { page = num; rescan = 0; }
    if (rescan) {
        int ok = tpl_pk_scan();
        if (!ok && !g_tpl_npk) { chat_say("Пикапы: пул не прочитался — подробности в konstt.log"); dlog("tpl: pick: нет Pools.Pickups.m_Items._entries"); return; }
        if (list && num > 1) page = num;
    }
    if (!g_tpl_npk) { chat_say("Пикапов и маркеров рядом нет: сервер подгружает их только вблизи — подойдите к нужному месту и повторите /tpl pick"); return; }
    pages = (g_tpl_npk + TPL_PAGE - 1) / TPL_PAGE;
    if (page < 1) page = 1;
    if (page > pages) page = pages;
    from = (page - 1) * TPL_PAGE; to = from + TPL_PAGE; if (to > g_tpl_npk) to = g_tpl_npk;
    b_init(&b, msg, sizeof msg); b_s(&b, "Пикапы и маркеры рядом: "); b_int(&b, g_tpl_npk);
    if (pages > 1) { b_s(&b, ", страница "); b_int(&b, page); b_s(&b, " из "); b_int(&b, pages); }
    chat_say(msg);
    for (i = from; i < to; i++) tpl_pk_line(i);
    b_init(&b, msg, sizeof msg); b_s(&b, "Телепорт по номеру: /tpl pick N");
    if (page < pages) { b_s(&b, ";  дальше: /tpl pick list "); b_int(&b, page + 1); }
    chat_usage(msg);
}
/* «/tpl gps» — телепорт на метку, поставленную на карте (/gps). Метка — MiniMap.m_NavigationPoint; если её нет, а сервер показывает маркер (чекпоинт) — на него */
static void tpl_cmd_gps(void) {
    TplCtx c; void* mm; float np[3], cp[3], me[3], v[3]; int act = -1, haveN = 0, nz = 0, haveMe, useChk = 0, staleN = 0;
    if (!tpl_ctx(&c, 0)) return;
    haveMe = tpl_cur_pos(&c, me) == 1;
    mm = tpl_minimap();
    if (mm) haveN = tpl_nav_point(mm, np, &act);
    nz = haveN && (np[0] != 0.0f || np[1] != 0.0f || np[2] != 0.0f);
    if (!(nz && act != 0)) {                                         /* GPS-метки нет или она снята — смотрим маркер сервера */
        if (tpl_chk_pos("Race", cp) || tpl_chk_pos("Simple", cp)) useChk = 1;
        else if (nz) staleN = 1;                                     /* снята, но координаты остались — телепортируем на последнюю */
    }
    if (!useChk && !nz) {
        chat_say(mm ? "Метки нет: откройте карту (/gps), поставьте метку или маршрут и повторите /tpl gps" : "/tpl gps: карта (MiniMap) не нашлась — подробности в konstt.log");
        if (!mm) dlog("tpl: gps: UI.MiniMap не прочитался");
        else if (!haveN) dlog("tpl: gps: у MiniMap нет поля m_NavigationPoint");
        return;
    }
    if (useChk) { tpl_goto_ex(cp, 3, "маркер сервера", 0); return; } /* у маркера высота точная — встаём ровно на него */
    v[0] = np[0]; v[2] = np[2];
    if (np[1] > -50.0f && np[1] < 3000.0f && np[1] != 0.0f) v[1] = np[1] + 1.5f;       /* высота метки (если сервер её знает) — только точка, откуда искать землю вниз */
    else if (haveMe) v[1] = me[1];
    else { chat_say("/tpl gps: не удалось прочитать вашу высоту — используйте /tpl to X Z с координатами метки"); return; }
    tpl_goto_ex(v, 3, staleN ? "последняя метка GPS (уже снята)" : "метка GPS", 1);
    { char lg[160]; Buf x; b_init(&x, lg, sizeof lg); b_s(&x, "tpl: gps: m_NavigationPoint = "); tpl_b_xyz(&x, np); b_s(&x, ", активна "); b_int(&x, act); dlog(lg); }
}

/* ───────────── команда ───────────── */
static void tpl_usage(void) {
    chat_usage("Используйте: /tpl [влево|вправо|вперёд|назад] [метры] — например «/tpl назад 2», «/tpl вправо 3», «/tpl 4» (по умолчанию влево 1.5 м)");
    chat_usage("/tpl pos — мои координаты;  /tpl to X Y Z — телепорт в точку (или /tpl to X Z — высота по земле);  /tpl alt — другой способ");
    chat_usage("/tpl ghost [on|off|back|tap [сек]] — призрак: сервер и NPC не видят, что вы двигаетесь (tap — окно, чтобы наступить на метку);  /tpl int N — метка интерьера/освещение (только номер, 0 — улица; место не меняет — точки запоминают интерьер сами). За рулём телепорт переносит машину");
    chat_usage("Точки: /tpl save имя — запомнить место;  /tpl go имя (или просто /tpl имя) — вернуться;  /tpl list — список;  /tpl del имя — удалить");
    chat_usage("/tpl gps — телепорт на метку, поставленную на карте (/gps);  /tpl pick — список пикапов и маркеров рядом с номерами (с названием, если у них есть 3D-подпись);  /tpl pick N — телепорт к номеру N");
}
/* Вызывается игрой в главном потоке при вводе /tpl [аргументы] */
static void native_tpl_cmd(void* self, void* args, void* mi) {
    TplArgs a;
    (void)self; (void)mi;
    if (args && seq(A.class_get_name(A.object_get_class(args)), "String")) { tplw_tick(); return; }   /* тик наблюдателя из Post (строка), а не команда (string[]) */
    mt_capture_ctx();                                                /* главный поток: запоминаем контекст для тиков наблюдателя */
    tpl_parse(args, &a);
    if (a.bad && a.unkN == 1) {                                     /* одно незнакомое слово — возможно, имя точки */
        if ((a.mode == 3 || a.mode == 4 || a.mode == 6) && !a.nameSet) {            /* «/tpl дом save» — имя стоит перед командой */
            tpl_copy_name(a.name, a.unk, TPL_NAME); a.nameSet = a.name[0] ? 1 : 0; a.bad = !a.nameSet;
        } else if (!a.mode && !a.numSet && !a.sideSet && !a.status && !a.help) {   /* «/tpl дом» — сокращение от «/tpl go дом» */
            tpl_pts_load();
            if (tpl_find(a.unk) >= 0) { a.mode = 4; tpl_copy_name(a.name, a.unk, TPL_NAME); a.nameSet = 1; a.bad = 0; }
            else {
                char m[120]; Buf mb; b_init(&mb, m, sizeof m);
                b_s(&mb, "«"); b_s(&mb, a.unk); b_s(&mb, "» — не команда и не сохранённая точка (список точек: /tpl list)");
                chat_say(m); tpl_usage(); return;
            }
        }
    }
    if (a.bad || a.help) { tpl_usage(); return; }
    if (a.alt) {
        g_tpl_alt = !g_tpl_alt;
        chat_say(g_tpl_alt ? "Телепорт: способ — через движение персонажа (запасной)" : "Телепорт: способ — Player.SetPosition (основной)");
        if (!a.status && !a.numSet && !a.sideSet && !a.mode) return;  /* «/tpl alt» без стороны, числа и pos/to только переключает, без прыжка */
    }
    if (a.status) {
        chat_say(g_tpl_alt ? "Телепорт: сейчас способ «через движение» (/tpl alt — вернуть основной)" : "Телепорт: сейчас основной способ Player.SetPosition (/tpl alt — запасной)");
        tpl_status_extra();
        return;
    }
    if (a.mode == 1) { tpl_show(); return; }
    if (a.mode == 2) {
        if (a.nc != 2 && a.nc != 3) { chat_usage("Нужно 3 числа: /tpl to X Y Z (или 2 числа X Z — высота по земле). Числа с минусом можно, например /tpl to 120.5 8 -340"); return; }
        tpl_goto(a.c, a.nc, NULL);
        return;
    }
    if (a.mode == 7) { tpl_cmd_ghost(a.gsub, a.numSet ? a.rawNum : -1.0f); return; }       /* число после «ghost tap» — секунды окна */
    if (a.mode == 8) {
        int have = a.iidSet || a.numSet;
        int id = a.iidSet ? a.iid : (a.numSet ? (int)(a.rawNum + 0.5f) : 0);        /* «/tpl 5 int» — число стояло до слова int */
        tpl_cmd_int(id, have);
        return;
    }
    if (a.mode == 9) { tpl_cmd_gps(); return; }
    if (a.mode == 10) { tpl_cmd_pick(a.numSet ? (int)(a.rawNum + 0.5f) : -1, a.pkList); return; }
    if (a.mode == 5) { tpl_cmd_list(a.numSet ? (int)(a.dist + 0.5f) : 1); return; }
    if (a.mode == 3 || a.mode == 4 || a.mode == 6) {
        if (!a.nameSet) { chat_usage(a.mode == 3 ? "Укажите имя точки: /tpl save дом" : a.mode == 4 ? "Укажите имя точки: /tpl go дом (список: /tpl list)" : "Укажите имя точки: /tpl del дом (список: /tpl list)"); return; }
        if (a.mode == 3) tpl_cmd_save(a.name); else if (a.mode == 4) tpl_cmd_go(a.name); else tpl_cmd_del(a.name);
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
    dlog("tpl: мод загружен, команда /tpl [сторона] [метры] | pos | to X Y Z | save/go/list/del имя | ghost [on|off|back|tap] | int N | gps | pick [N]");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void tpl_ctor(void) {
    unsigned long t = 0;
    g_tpl_modId = hm_mod_register("tpl");
    pthread_create(&t, NULL, tpl_main, NULL);
}
#endif
