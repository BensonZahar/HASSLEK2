/*
 * hassle_spec.c — необязательный модуль «/spec»: тест «spectate sync» вместо «onfoot sync» (для тестовых NPC на своём сервере).
 *
 * Зависит только от ядра (hassle_core.c). КАК УБРАТЬ: удалите hassle_spec.c и пересоберите — больше ничего править не нужно.
 *
 * Команда в чате:
 *   /spec on       включить: клиент начинает слать пакет SPECTATE (ID 212) вместо onfoot/aim/drive
 *   /spec off      выключить: клиент снова шлёт обычные пакеты
 *   /spec          (или /spec status) состояние: флаг, последний отправленный spectate-пакет и последняя позиция onfoot
 *
 * Как это устроено (по дампу игры от 20.10.2026 и дизассемблированию sync_dis.txt):
 *   • LocalPlayer.SendSyncData (RVA 0x12F9640) смотрит на поле LocalPlayer.m_IsSpectating (смещение 0x1D0). Если оно true — зовёт ТОЛЬКО
 *     SendSpectateSyncData (RVA 0x12F9AA4, пакет ID_SPECTATE_SYNC = 212: LeftRightAnalog, UpDownAnalog, Keys, Position) и больше
 *     ничего: ни onfoot (207), ни aim (203), ни drive/passenger не уходят. Если false — обычная ветка.
 *   • Поле пишем НАПРЯМУЮ, а не через LocalPlayer.ToggleSpectating(bool): тот при переходе true→false зовёт RequestSpawn() (респавн),
 *     а нам нужен только переключатель выбора пакета. Серверный RPC ToggleSpectating (124) тоже ставит это поле — сервер может выключить режим сам.
 *   • Ничего не патчим и не хукаем: меняется один байт в объекте LocalPlayer. Выключение (/spec off) или перезапуск игры возвращает всё как было.
 *   • Позицию в пакете берёт сама игра (метод SendSpectateSyncData). Откуда именно — из Transform или иначе — по дизассемблированию не проверено:
 *     сравните в «/spec status» строку «onfoot» и строку «spectate» на одном и том же месте. Если высота разная — это и есть смещение.
 *   • В машине SendSyncData тоже сначала смотрит на m_IsSpectating, так что с включённым режимом пакеты водителя/пассажира не уходят.
 *   • Журнал — HLOG/konstt.log, строки «spec:».
 */
#pragma once
#include "hassle_core.c"

static int g_spec_modId = -1;

/* LocalPlayer или NULL */
static void* spec_lp(void) {
    void* lp = local_player();
    if (!lp || !hm_obj_ok(lp)) return NULL;
    return lp;
}

/* Указатель на байт LocalPlayer.m_IsSpectating (проверка: поле есть, это bool, смещение разумное, память читается/пишется) или NULL */
static u8* spec_flag(void* lp) {
    void* f; int off;
    f = find_field(A.object_get_class(lp), "m_IsSpectating");
    if (!f) return NULL;
    if (A.type_get_type(A.field_get_type(f)) != 2) return NULL;          /* IL2CPP_TYPE_BOOLEAN */
    off = A.field_get_offset(f);
    if (off < (int)HDR || off > 0x2000 || !hm_readable((char*)lp + off, 1)) return NULL;
    return (u8*)((char*)lp + off);
}

/* Последний spectate-пакет игры: LocalPlayer.m_LastSpectateSyncData (value-тип внутри объекта). 1 — прочитали. */
static int spec_read_last(void* lp, unsigned* lr, unsigned* ud, unsigned* keys, float* pos) {
    void *sf, *sc, *f; char* sp; int shift, o;
    sf = find_field(A.object_get_class(lp), "m_LastSpectateSyncData");
    if (!sf) return 0;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc || !A.class_is_valuetype(sc)) return 0;
    sp = (char*)lp + A.field_get_offset(sf);
    shift = struct_shift(sc);
    if (!hm_readable(sp, 0x30)) return 0;
    f = find_field(sc, "LeftRightAnalog"); if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0; *lr = *(u16*)(sp + o);
    f = find_field(sc, "UpDownAnalog");    if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0; *ud = *(u16*)(sp + o);
    f = find_field(sc, "Keys");            if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0; *keys = *(u16*)(sp + o);
    f = find_field(sc, "Position");        if (!f) return 0; o = A.field_get_offset(f) - shift; if (o < 0 || o > 0x30) return 0;
    {   float* v = (float*)(sp + o); pos[0] = v[0]; pos[1] = v[1]; pos[2] = v[2]; }
    return 1;
}

/* Разбор аргументов (string[]): 1 — on, 2 — off, 0 — статус/ничего. Команда сама может входить в массив — не важно. */
static int spec_args(void* arr) {
    long len; int i, cmd = 0; char* data;
    if (!arr) return 0;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return 0;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[48]; int n; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 20 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        if (seq(tmp, "on") || seq(tmp, "1") || seq(tmp, "вкл")) cmd = 1;
        else if (seq(tmp, "off") || seq(tmp, "0") || seq(tmp, "выкл")) cmd = 2;
        else if (seq(tmp, "status") || seq(tmp, "статус")) cmd = 0;
    }
    return cmd;
}

static void spec_say_xyz(const char* title, const float* p) {
    char mem[160]; Buf b; b_init(&b, mem, sizeof mem);
    b_s(&b, title); b_fix(&b, p[0], 2); b_s(&b, "  "); b_fix(&b, p[1], 2); b_s(&b, "  "); b_fix(&b, p[2], 2);
    chat_say(b.p);
}

static void spec_status(void* lp) {
    u8* fl = spec_flag(lp);
    unsigned lr = 0, ud = 0, keys = 0; float sp[3] = {0, 0, 0}, op[3] = {0, 0, 0};
    chat_say(!fl ? "spec: поле m_IsSpectating не найдено" : *fl ? "spec: ВКЛ (шлётся spectate, ID 212)" : "spec: выкл (обычные пакеты)");
    if (spec_read_last(lp, &lr, &ud, &keys, sp)) {
        char mem[160]; Buf b; b_init(&b, mem, sizeof mem);
        b_s(&b, "spectate: keys="); b_int(&b, (long)keys); b_s(&b, " lr="); b_int(&b, (long)lr); b_s(&b, " ud="); b_int(&b, (long)ud);
        chat_say(b.p);
        spec_say_xyz("spectate pos: ", sp);
    } else chat_say("spectate: последнего пакета не прочитать (поле m_LastSpectateSyncData не найдено)");
    if (read_pos(lp, op)) spec_say_xyz("onfoot pos:   ", op);
}

/* Вызывается игрой в главном потоке при вводе /spec [on|off|status] */
static void native_spec_cmd(void* self, void* args, void* mi) {
    void* lp; u8* fl; int cmd;
    (void)self; (void)mi;
    cmd = spec_args(args);
    lp = spec_lp();
    if (!lp) { chat_say("spec: игрок ещё не появился"); return; }
    if (cmd == 0) { spec_status(lp); chat_usage("Используйте: /spec on | /spec off | /spec status"); return; }
    fl = spec_flag(lp);
    if (!fl) { chat_say("spec: поле m_IsSpectating не найдено (смотрите konstt.log)"); dlog("spec: нет поля LocalPlayer.m_IsSpectating или оно не bool"); return; }
    if (cmd == 1) {
        *fl = 1;                                            /* НЕ ToggleSpectating: тот при выключении делает RequestSpawn */
        chat_say("spec: ВКЛ — вместо onfoot/aim/drive уходит spectate (ID 212)");
        dlog("spec: m_IsSpectating = 1");
    } else {
        *fl = 0;
        chat_say("spec: выкл — снова обычные пакеты");
        dlog("spec: m_IsSpectating = 0");
    }
}

static void* spec_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("spec", native_spec_cmd)) return NULL;
    hm_mod_ready(g_spec_modId);
    dlog("spec: мод загружен, команда /spec зарегистрирована");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void spec_ctor(void) {
    unsigned long t = 0;
    g_spec_modId = hm_mod_register("spec");
    pthread_create(&t, NULL, spec_main, NULL);
}
#endif
