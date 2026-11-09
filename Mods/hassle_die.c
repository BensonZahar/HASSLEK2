/*
 * hassle_die.c — необязательный модуль «/die» и «/hp»: умереть самому или выставить себе здоровье одной командой в чате.
 * Нужен, когда тестовый NPC (аккаунт на нашем клиенте) завис и не хочется идти в админку выдавать ему HP 0.
 *
 * Зависит только от ядра (hassle_core.c): включает его, сам запускает свой поток и сам регистрирует команду.
 * КАК УБРАТЬ: удалите hassle_die.c и пересоберите бандл — больше ничего править не нужно.
 *
 * Команда в чате (нативная, как /nocol):
 *   /die            убить своего персонажа (здоровье → 0)
 *   /hp 0..100      выставить здоровье (0 — смерть, как /die); /hp без числа показывает текущее
 *
 * Как это работает (по dump.cs и разбору libil2cpp.so, RVA из дампа этого APK)
 *   • LocalPlayer.SetHealth(float) (RVA 0x12FC5FC, override Actor.SetHealth) зовёт Actor.SetHealth: тот пишет m_Health (+0x5C)
 *     и вызывает виртуальный OnHealthChanged. У LocalPlayer.OnHealthChanged (RVA 0x12FC83C) проверка «здоровье < 1.0» —
 *     если да, зовётся LocalPlayer.SendDeathNotification (RVA 0x12FC8B0): пакет RPC Death (53) с байтом и ushort на сервер.
 *     Тот же путь игра проходит, когда HP обнуляет сервер или вас убили, поэтому сервер видит обычную смерть и сам делает респавн.
 *   • Команда вызывается игрой в главном потоке — отдельно ничего постить не нужно.
 *   • /hp N (1..100) — тот же SetHealth(N), но смерти нет. ВАЖНО: это здоровье на вашей стороне (поле m_Health и полоска в интерфейсе);
 *     сервер хранит своё и может в любой момент прислать своё значение поверх. Смерть (0) сервер видит через RPC, поэтому она настоящая.
 *   • Если SetHealth вдруг не нашёлся или бросил исключение — запасной путь: напрямую SendDeathNotification.
 */
#pragma once
#include "hassle_core.c"

static int g_die_modId = -1;

/* LocalPlayer.SetHealth(hp). 1 — вызвали без исключения */
static int die_set_health(void* lp, float hp) {
    void *m, *exc = NULL, *args[1]; char et[240];
    m = find_method(A.object_get_class(lp), "SetHealth", 1);
    if (!m) { dlog("die: SetHealth не найден"); return 0; }
    args[0] = &hp;
    A.runtime_invoke(m, lp, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("die: SetHealth бросил исключение: ", et); return 0; }
    return 1;
}
/* запасной путь: сразу уведомить сервер о смерти (приватный LocalPlayer.SendDeathNotification) */
static int die_send_notification(void* lp) {
    void *m, *exc = NULL; char et[240];
    m = find_method(A.object_get_class(lp), "SendDeathNotification", 0);
    if (!m) { dlog("die: SendDeathNotification не найден"); return 0; }
    A.runtime_invoke(m, lp, NULL, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("die: SendDeathNotification бросил исключение: ", et); return 0; }
    return 1;
}

/* Вызывается игрой в главном потоке при вводе /die */
static void native_die_cmd(void* self, void* args, void* mi) {
    void* lp; int okf = 0; double hp; char msg[120]; Buf b;
    (void)self; (void)args; (void)mi;
    ensure_attached();
    lp = local_player();
    if (!lp || !hm_obj_ok(lp)) { chat_say("/die: персонаж недоступен (ещё не в игре?)"); dlog("die: local_player не найден"); return; }
    if (!die_set_health(lp, 0.0f)) {
        if (!die_send_notification(lp)) { chat_say("/die: не получилось, подробности в konstt.log"); return; }
        chat_say("/die: SetHealth не сработал, отправил сервер уведомление о смерти напрямую");
        return;
    }
    hp = num_field(lp, "m_Health", &okf);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "die: SetHealth(0) вызван");
    if (okf) { b_s(&b, ", m_Health теперь "); b_fix(&b, hp, 1); }
    dlog(msg);
    chat_say("/die: умираем");
}

/* Вызывается игрой в главном потоке при вводе /hp [0..100] */
static void native_hp_cmd(void* self, void* args, void* mi) {
    void* lp; int okf = 0, diag = 0, n; double hp; char msg[120]; Buf b;
    (void)self; (void)mi;
    ensure_attached();
    lp = local_player();
    if (!lp || !hm_obj_ok(lp)) { chat_say("/hp: персонаж недоступен (ещё не в игре?)"); dlog("hp: local_player не найден"); return; }
    n = parse_cmd_args(args, &diag);
    if (n < 0) {                                              /* без числа — показываем текущее здоровье */
        hp = num_field(lp, "m_Health", &okf);
        b_init(&b, msg, sizeof msg);
        b_s(&b, "/hp: сейчас "); if (okf) b_fix(&b, hp, 0); else b_s(&b, "?");
        b_s(&b, ". Использование: /hp 0..100");
        chat_usage(msg);
        return;
    }
    if (n > 100) { chat_usage("/hp: число от 0 до 100"); return; }
    if (!die_set_health(lp, (float)n)) {
        if (n == 0 && die_send_notification(lp)) { chat_say("/hp: SetHealth не сработал, отправил сервер уведомление о смерти напрямую"); return; }
        chat_say("/hp: не получилось, подробности в konstt.log");
        return;
    }
    hp = num_field(lp, "m_Health", &okf);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "hp: SetHealth("); b_int(&b, n); b_s(&b, ") вызван");
    if (okf) { b_s(&b, ", m_Health теперь "); b_fix(&b, hp, 1); }
    dlog(msg);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "/hp: здоровье "); b_int(&b, n);
    chat_say(msg);
}

static void* die_main(void* arg) {
    int i, r = 0;
    (void)arg;
    if (!hm_cmd_install("die", native_die_cmd)) return NULL;        /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /die */
    for (i = 0; i < 20 && r <= 0; i++) { r = cmd_register("hp", native_hp_cmd); if (r <= 0) usleep(500000); }
    if (r <= 0) dlog("die: команда /hp не зарегистрирована, /die работает");
    hm_mod_ready(g_die_modId);
    dlog("die: мод загружен, команды /die и /hp работают");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void die_ctor(void) {
    unsigned long t = 0;
    g_die_modId = hm_mod_register("die");
    pthread_create(&t, NULL, die_main, NULL);
}
#endif
