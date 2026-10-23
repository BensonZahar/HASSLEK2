/*
 * _template.c — заготовка нового мода. Файлы, имя которых начинается с «_», в бандл НЕ попадают.
 * Скопируйте в Mods/hassle_имя.c, переименуйте всё с префиксом имени, запушьте в main — GitHub Actions сам соберёт и опубликует
 * бандл, игра докачает его при следующем запуске (APK пересобирать не нужно).
 *
 * Правила:
 *   • Все .c из папки Mods собираются ОДНОЙ единицей трансляции (bundle_gen.c делает #include каждого) — поэтому у всех static-функций
 *     и переменных должен быть свой уникальный префикс (hello_ …), иначе будет конфликт имён с другими модулями.
 *   • Первая строка после комментария — #pragma once, затем #include "hassle_near.c" (ядро: IL2CPP-доступ, dlog, реестр команд…).
 *   • Свой запуск — через __attribute__((constructor)) + поток-ждун (как в hassle_nocol.c / hassle_route.c): конструктор срабатывает
 *     при dlopen бандла, а в IL2CPP поток лезет только после того, как Near отметил игру готовой.
 *   • Образец регистрации чат-команды: hassle_nocol.c (nocol_boot_once / nocol_register).
 */
#pragma once
#include "hassle_near.c"

static void* hello_main(void* arg) {
    int w;
    (void)arg;
    for (w = 0; !__atomic_load_n(&g_started, __ATOMIC_ACQUIRE); w++) usleep(500000);     /* ждём, пока ядро Near стартует */
    if (off_switch()) return NULL;                                                      /* near_off — мод выключен целиком */
    for (w = 0; !__atomic_load_n(&g_cmdDone, __ATOMIC_ACQUIRE); w++) usleep(500000);     /* игра готова, образец делегата найден */
    dlog("hello: новый мод загружен и работает");
    return NULL;
}
#ifndef NEAR_TEST
__attribute__((constructor)) static void hello_ctor(void) {
    unsigned long t = 0;
    pthread_create(&t, NULL, hello_main, NULL);
}
#endif
