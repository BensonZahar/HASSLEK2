/*
 * _template.c — заготовка нового мода. Файлы, имя которых начинается с «_», в бандл НЕ попадают.
 * Скопируйте в Mods/hassle_имя.c, переименуйте всё с префиксом имени, запушьте в main — GitHub Actions сам соберёт и опубликует
 * бандл, игра докачает его при следующем запуске (APK пересобирать не нужно).
 *
 * Правила:
 *   • Все .c из папки Mods собираются ОДНОЙ единицей трансляции (bundle_gen.c делает #include каждого) — поэтому у всех static-функций
 *     и переменных должен быть свой уникальный префикс (hello_ …), иначе будет конфликт имён с другими модулями.
 *   • Первая строка после комментария — #pragma once, затем #include "hassle_core.c" (ядро: IL2CPP-доступ, dlog, реестр команд…).
 *     Другие моды не включайте: мод должен собираться с одним только ядром. `python build_konstt.py --check` проверяет это для каждого
 *     мода по отдельности (и для самого шаблона).
 *   • Свой запуск — через __attribute__((constructor)) + поток-ждун (как в hassle_rec.c / hassle_nocol.c): конструктор срабатывает при
 *     dlopen бандла, а в IL2CPP поток лезет только после готовности игры. hm_cmd_install("имя", функция) ждёт ядро, игру и образец
 *     делегата и регистрирует «/имя»; если команда не нужна, достаточно hm_wait_ready().
 *   • Нужна позиция игрока — local_player() + read_pos(); нужны тики в главном потоке — mt_capture_ctx() из обработчика команды
 *     и Post (образцы: hassle_route.c, hassle_nocol.c).
 */
#pragma once
#include "hassle_core.c"

/* Вызывается игрой в главном потоке при вводе /hello [аргументы] */
static void native_hello_cmd(void* self, void* args, void* mi) {
    (void)self; (void)args; (void)mi;
    chat_say("hello: мод работает");
}
static void* hello_main(void* arg) {
    (void)arg;
    if (!hm_cmd_install("hello", native_hello_cmd)) return NULL;     /* konstt_off — мод выключен целиком; иначе ждём игру и регистрируем /hello */
    dlog("hello: новый мод загружен и работает");
    return NULL;
}
#ifndef KONSTT_TEST
__attribute__((constructor)) static void hello_ctor(void) {
    unsigned long t = 0;
    pthread_create(&t, NULL, hello_main, NULL);
}
#endif
