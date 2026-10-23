/*
 * hassle_core.c — ядро клиента konstt для Hassle Online (IL2CPP, Unity). ЕДИНСТВЕННЫЙ обязательный файл в Mods/.
 * Клиент называется konstt; «near» — только имя одного из его модов (/near, игроки рядом).
 *
 * Остальные файлы — это отдельные моды, каждый подключает только ядро (#include "hassle_core.c") и о других модах ничего не знает:
 *   hassle_near.c   игроки рядом: чтение PlayersInStream + HTTP-сервер 127.0.0.1:17771 для HUD (Code3.js) + команда /nearlog
 *   hassle_rec.c    команда /rec — реконнект
 *   hassle_nocol.c  команда /nocol — проходить сквозь игроков и NPC
 *   hassle_route.c  команда /route — запись и повтор маршрута пешком
 * Любой из них можно добавить или убрать, ничего больше не трогая: положите файл в Mods/ или удалите его. Новый мод — Mods/_template.c.
 *
 * Что делает ядро
 *   • Загрузка. Два режима (patch_apk.py выбирает сам):
 *     1) Старый клиент: библиотека называется libHassleExtended.so — её уже грузит игра (System.loadLibrary("HassleExtended")).
 *        Оригинальный мод переименован в libHassleExtend2.so; из нашего JNI_OnLoad он загружается и получает тот же вызов.
 *     2) Чистый APK (Unity 2021.3, libHassleExtended.so в нём нет): мы подменяем libmain.so — её игра грузит первой
 *        (System.loadLibrary("main")). Оригинальный libmain.so переименован в libHassleMain2.so; наш JNI_OnLoad грузит его
 *        и передаёт ему тот же вызов — он, как и раньше, регистрирует NativeLoader и сам поднимает libunity/libil2cpp.
 *   • Готовность игры. Поток ядра (core_main) НЕ ждёт вслепую: libil2cpp загружена → короткая стабилизация (KONSTT_SETTLE_S или файл
 *     HLOG/konstt_delay.txt) → виден Assembly-CSharp → создан Hassle.Game.Instance. Тогда выставляются g_safe и g_ready. Дальше ядро находит
 *     static-поле с PlayerPool (g_poolSrcField) и образец делегата в реестре чат-команд (g_tplState). Пока игра не готова,
 *     IL2CPP не трогается дальше лёгкого опроса.
 *   • Общие помощники для модов: журнал (dlog → HLOG/konstt.log), доступ к IL2CPP (A, find_field, ref_field, ...), безопасное чтение памяти
 *     (hm_readable, hm_obj_ok, ref_field_s), пул игроков (current_pool, local_player, read_pos), реестр чат-команд (cmd_register,
 *     chat_say), главный поток по заказу (mt_capture_ctx).
 *   • Выключатель: файл konstt_off (см. off_switch) отключает ВСЕ моды целиком.
 *
 * API для модов (всё static: бандл — одна единица трансляции, у каждого мода свой префикс имён)
 *   hm_wait_started()          ждёт старт ядра; 0 — мод выключен файлом konstt_off
 *   hm_wait_ready()            то же + ждёт, пока игра готова (Game.Instance создан)
 *   hm_cmd_install(имя, fn)    ждёт готовность и образец делегата, регистрирует «/имя» и «имя» в реестре команд; 1 — готово
 *   Как правило, мод создаёт свой поток в __attribute__((constructor)) и первым делом зовёт одну из этих функций.
 *
 * Только IL2CPP C API (экспортируется libil2cpp.so) — исходники игры и BNM не нужны, классы ищутся по имени.
 * Код не использует libc-заголовки: прототипы объявлены вручную, поэтому собирается обычным clang без NDK.
 */
#pragma once

#ifndef HM_BUILD_ID
#define HM_BUILD_ID "dev"
#endif

typedef unsigned long uptr;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define EXPORT __attribute__((visibility("default")))

/* ───────────── libc / libdl (резолвятся загрузчиком; DT_NEEDED даёт заглушка при линковке) ───────────── */
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int close(int);
extern int pthread_create(unsigned long*, const void*, void* (*)(void*), void*);
extern int usleep(unsigned);
extern int open(const char*, int, ...);
extern long read(int, void*, unsigned long);
extern long time(long*);
extern void* memcpy(void*, const void*, unsigned long);
extern int mkdir(const char*, unsigned);
extern long write(int, const void*, unsigned long);
extern int rename(const char*, const char*);
extern char* dlerror(void);
extern int pipe(int*);
extern int fcntl(int, int, ...);

#define RTLD_NOW_ 2
#define TICK_US 300000
#ifndef KONSTT_EXTRA_DELAY_S
#define KONSTT_EXTRA_DELAY_S 0  /* запасной рычаг: доп. пауза после готовности игры. Если вдруг вернутся вылеты на старте — собрать с -DKONSTT_EXTRA_DELAY_S=10 */
#endif
#ifndef KONSTT_SETTLE_S
#define KONSTT_SETTLE_S 12      /* (пробовали 1 и 3 — иногда вылетало на старте, вернули 12) */
/* сколько секунд после появления libil2cpp.so мод НЕ вызывает ни одной её функции (идёт il2cpp_init). Можно менять файлом HLOG/konstt_delay.txt (число секунд) без пересборки */
#endif
#define POLL_US 250000        /* период лёгкого опроса готовности */

/* ───────────── мелкие строковые помощники (без libc) ───────────── */
static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int sprefix(const char* s, const char* p) { while (*p) { if (*s != *p) return 0; s++; p++; } return 1; }
static int ends_with(const char* s, const char* suf) {
    unsigned a = slen(s), b = slen(suf);
    return a >= b && seq(s + a - b, suf);
}

typedef struct { char* p; unsigned n, cap; } Buf;
static void b_init(Buf* b, char* mem, unsigned cap) { b->p = mem; b->n = 0; b->cap = cap; if (cap) mem[0] = 0; }
static void b_ch(Buf* b, char c) { if (b->n + 1 < b->cap) { b->p[b->n++] = c; b->p[b->n] = 0; } }
static void b_s(Buf* b, const char* s) { while (s && *s) b_ch(b, *s++); }
static void b_int(Buf* b, long v) {
    char t[24]; int i = 0; unsigned long u;
    if (v < 0) { b_ch(b, '-'); u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + (u % 10)); u /= 10; }
    while (i) b_ch(b, t[--i]);
}
static void b_fix(Buf* b, double v, int dec) {          /* число с dec знаками после запятой */
    int i; double m = 1;
    if (!(v == v) || v > 1e12 || v < -1e12) { b_s(b, "0"); return; }
    if (v < 0) { b_ch(b, '-'); v = -v; }
    for (i = 0; i < dec; i++) m *= 10;
    {
        unsigned long long all = (unsigned long long)(v * m + 0.5);
        unsigned long long ip = all / (unsigned long long)m, fp = all % (unsigned long long)m;
        char t[24]; int k = 0;
        if (!ip) t[k++] = '0';
        while (ip) { t[k++] = (char)('0' + (ip % 10)); ip /= 10; }
        while (k) b_ch(b, t[--k]);
        if (dec) {
            b_ch(b, '.');
            for (i = dec - 1; i >= 0; i--) {
                unsigned long long d = fp, q = 1; int j;
                for (j = 0; j < i; j++) q *= 10;
                d = (d / q) % 10;
                b_ch(b, (char)('0' + d));
            }
        }
    }
}

/* ───────────── журнал диагностики (в памяти; пишется ещё в файл HLOG/konstt.log) ───────────── */
static char g_diag_mem[24576];
static Buf g_diag;
static int g_diag_ready;
typedef void (*logfn_t)(int, const char*, const char*);
static logfn_t g_alog;

/* ───────────── файловый журнал: <папка игры>/files/HLOG/konstt.log ─────────────
   Каждая строка дописывается в файл сразу же (open → write → close), поэтому при вылете последняя строка показывает, где всё оборвалось.
   Основной путь: /sdcard/Android/data/<пакет>/files/HLOG/konstt.log  (Android/data/<пакет>/files/HLOG в файловом менеджере/по USB).
   Если туда писать нельзя — запасной: /data/data/<пакет>/files/HLOG/konstt.log. Прошлый запуск сохраняется как konstt_prev.log. */
#define HM_LOGNAME "konstt.log"                   /* имя файла журнала; пути соседних файлов (konstt_delay.txt, near_snap.log) считаются от него */
#define HM_LOGNAME_LEN ((unsigned)(sizeof(HM_LOGNAME) - 1))
static char g_flog[300], g_flog_prev[300];
static int g_flogInit, g_flogLines, g_flogFallback;
static volatile int g_flogLock;
static long g_flogT0;

static int pkg_name(char* out, int cap) {
    char cmd[128]; int fd, i; long n;
    out[0] = 0;
    fd = open("/proc/self/cmdline", 0);
    if (fd < 0) return 0;
    n = read(fd, cmd, sizeof cmd - 1); close(fd);
    if (n <= 0) return 0;
    cmd[n] = 0;
    for (i = 0; cmd[i] && cmd[i] != ':' && i < cap - 1; i++) out[i] = cmd[i];
    out[i] = 0;
    return i > 0;
}
static void flog_paths(const char* root, const char* pkg) {      /* root: "/sdcard/Android/data/" или "/data/data/" */
    char d[300]; Buf b;
    b_init(&b, d, sizeof d); b_s(&b, root); b_s(&b, pkg); mkdir(d, 0777);
    b_s(&b, "/files"); mkdir(d, 0777);
    b_s(&b, "/HLOG"); mkdir(d, 0777);
    b_init(&b, g_flog, sizeof g_flog); b_s(&b, d); b_s(&b, "/" HM_LOGNAME);
    b_init(&b, g_flog_prev, sizeof g_flog_prev); b_s(&b, d); b_s(&b, "/konstt_prev.log");
}
static int flog_write(const char* path, const char* data, unsigned n, int create_new) {
    int fd = open(path, create_new ? 0x241 : 0x441, 0666);        /* O_WRONLY|O_CREAT|(O_TRUNC | O_APPEND) */
    if (fd < 0) return 0;
    write(fd, data, n); close(fd);
    return 1;
}
static void file_log(const char* line) {
    char t[700]; Buf b; long now;
    while (__sync_lock_test_and_set(&g_flogLock, 1)) usleep(200);
    if (g_flogLines > 4000) { __sync_lock_release(&g_flogLock); return; }
    now = time(NULL);
    if (!g_flogInit) {
        char pkg[100];
        g_flogInit = 1; g_flogT0 = now;
        if (pkg_name(pkg, sizeof pkg)) {
            flog_paths("/sdcard/Android/data/", pkg);
            rename(g_flog, g_flog_prev);                          /* прошлый запуск (мог закончиться вылетом) не теряем */
            if (!flog_write(g_flog, "", 0, 1)) {                  /* внешняя папка недоступна — запасная внутренняя */
                flog_paths("/data/data/", pkg); g_flogFallback = 1;
                rename(g_flog, g_flog_prev);
                if (!flog_write(g_flog, "", 0, 1)) g_flog[0] = 0;
            }
        } else g_flog[0] = 0;
        if (g_flog[0]) {
            b_init(&b, t, sizeof t); b_s(&b, "== запуск konstt; файл: "); b_s(&b, g_flog);
            if (g_flogFallback) b_s(&b, "  (ВНУТРЕННЯЯ папка: в /sdcard/Android/data писать нельзя)");
            b_s(&b, "\n"); flog_write(g_flog, t, b.n, 0);
        }
    }
    if (g_flog[0]) {
        b_init(&b, t, sizeof t);
        b_ch(&b, '+'); b_int(&b, now - g_flogT0); b_s(&b, "с  "); b_s(&b, line); b_ch(&b, '\n');
        flog_write(g_flog, t, b.n, 0);
        g_flogLines++;
    }
    __sync_lock_release(&g_flogLock);
}

static void dlog(const char* s) {
    if (!g_diag_ready) { b_init(&g_diag, g_diag_mem, sizeof g_diag_mem); g_diag_ready = 1; }
    if (g_diag.n + 200 > g_diag.cap) {                  /* кольцо: когда заполнилось — начинаем сначала */
        g_diag.n = 0; g_diag.p[0] = 0;
        b_s(&g_diag, "[журнал переполнен, начат заново]\n");
    }
    b_s(&g_diag, s); b_ch(&g_diag, '\n');
    if (g_alog) g_alog(4, "Konstt", s);
    file_log(s);
}
static void dlog2(const char* a, const char* b) {
    char t[512]; Buf x; b_init(&x, t, sizeof t); b_s(&x, a); b_s(&x, b); dlog(t);
}
static void dlog_i(const char* a, long v) {
    char t[512]; Buf x; b_init(&x, t, sizeof t); b_s(&x, a); b_int(&x, v); dlog(t);
}

/* ───────────── IL2CPP API ───────────── */
static struct {
    void* (*domain_get)(void);
    void* (*thread_attach)(void*);
    void** (*domain_get_assemblies)(void*, uptr*);
    void* (*assembly_get_image)(void*);
    const char* (*image_get_name)(void*);
    uptr (*image_get_class_count)(void*);
    void* (*image_get_class)(void*, uptr);
    void* (*class_from_name)(void*, const char*, const char*);
    const char* (*class_get_name)(void*);
    const char* (*class_get_namespace)(void*);
    void* (*class_get_parent)(void*);
    void* (*class_get_fields)(void*, void**);
    void* (*class_get_field_from_name)(void*, const char*);
    const char* (*field_get_name)(void*);
    int (*field_get_flags)(void*);
    void* (*field_get_type)(void*);
    int (*field_get_offset)(void*);
    void (*field_static_get_value)(void*, void*);
    void* (*object_get_class)(void*);
    int (*type_get_type)(void*);
    void* (*class_from_type)(void*);
    int (*class_is_valuetype)(void*);
    void* (*class_get_method_from_name)(void*, const char*, int);
    void* (*runtime_invoke)(void*, void*, void**, void**);
    u16* (*string_chars)(void*);
    int (*string_length)(void*);
    int (*class_array_element_size)(void*);
    void* (*class_get_element_class)(void*);
    const char* (*type_get_name)(void*);
    /* необязательные (нужны для нативных чат-команд) */
    void* (*string_new)(const char*);
    void* (*object_new)(void*);
    int (*class_instance_size)(void*);
} A;

static void* g_il2cpp;
static void* g_domain;
static volatile int g_safe;          /* 1 — период ожидания после загрузки libil2cpp прошёл, функции IL2CPP звать можно */

#define LOAD(n) do { *(void**)&A.n = dlsym(g_il2cpp, "il2cpp_" #n); if (!A.n) { missing = #n; } } while (0)

static int il2cpp_open(void) {
    const char* missing = NULL;
    if (g_il2cpp) return 1;
#ifdef KONSTT_TEST
    g_il2cpp = dlopen(NULL, RTLD_NOW_);
#else
    g_il2cpp = dlopen("libil2cpp.so", RTLD_NOW_ | 4);   /* RTLD_NOLOAD: только если игра уже загрузила, сами не грузим */
#endif
    if (!g_il2cpp) return 0;
    LOAD(domain_get); LOAD(thread_attach); LOAD(domain_get_assemblies); LOAD(assembly_get_image);
    LOAD(image_get_name); LOAD(image_get_class_count); LOAD(image_get_class); LOAD(class_from_name);
    LOAD(class_get_name); LOAD(class_get_namespace); LOAD(class_get_parent); LOAD(class_get_fields);
    LOAD(class_get_field_from_name); LOAD(field_get_name); LOAD(field_get_flags); LOAD(field_get_type);
    LOAD(field_get_offset); LOAD(field_static_get_value); LOAD(object_get_class); LOAD(type_get_type);
    LOAD(class_from_type); LOAD(class_is_valuetype); LOAD(class_get_method_from_name); LOAD(runtime_invoke);
    LOAD(string_chars); LOAD(string_length); LOAD(class_array_element_size); LOAD(class_get_element_class);
    LOAD(type_get_name);
    *(void**)&A.string_new = dlsym(g_il2cpp, "il2cpp_string_new");
    *(void**)&A.object_new = dlsym(g_il2cpp, "il2cpp_object_new");
    *(void**)&A.class_instance_size = dlsym(g_il2cpp, "il2cpp_class_instance_size");
    if (missing) { dlog2("нет функции il2cpp_", missing); g_il2cpp = NULL; return 0; }
    return 1;
}

/* Состояние ядра (читают моды через __atomic_load_n):
   g_started — start_threads уже вызван (дальше надо проверить off_switch());
   g_ready   — игра готова: Game.Instance создан, g_img/g_domain заполнены, можно работать с IL2CPP из своего (присоединённого) потока;
   g_tplState — реестр чат-команд: 0 образец делегата ещё ищется, 1 найден (команды можно регистрировать), -1 не нашли, команд не будет. */
static int g_started;
static volatile int g_ready;
static volatile int g_tplState;

/* ───────────── доступ к полям ───────────── */
#define HDR (2 * sizeof(void*))                   /* Il2CppObject: klass + monitor */

static void* find_field(void* k, const char* name) {
    for (; k; k = A.class_get_parent(k)) {
        void* f = A.class_get_field_from_name(k, name);
        if (f) return f;
    }
    return NULL;
}
static void* find_method(void* k, const char* name, int argc) {
    for (; k; k = A.class_get_parent(k)) {
        void* m = A.class_get_method_from_name(k, name, argc);
        if (m) return m;
    }
    return NULL;
}
static void* ref_field(void* obj, const char* name) {   /* значение ссылочного поля объекта */
    void* f; void* v = NULL;
    if (!obj) return NULL;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return NULL;
    v = *(void**)((char*)obj + A.field_get_offset(f));
    return v;
}
static int field_is_valuetype(void* f) {
    void* t = A.field_get_type(f);
    void* c = t ? A.class_from_type(t) : NULL;
    return c ? A.class_is_valuetype(c) : 0;
}
static double read_num(void* addr, int t) {
    switch (t) {
        case 4: return *(signed char*)addr;
        case 5: return *(u8*)addr;
        case 6: return *(short*)addr;
        case 7: return *(u16*)addr;
        case 8: return *(int*)addr;
        case 9: return *(u32*)addr;
        case 10: return (double)*(long long*)addr;
        case 11: return (double)*(unsigned long long*)addr;
        case 12: return *(float*)addr;
        case 13: return *(double*)addr;
        default: return 0;
    }
}
/* число из поля объекта; ok=0 если поля нет */
static double num_field(void* obj, const char* name, int* ok) {
    void* f;
    if (ok) *ok = 0;
    if (!obj) return 0;
    f = find_field(A.object_get_class(obj), name);
    if (!f) return 0;
    if (ok) *ok = 1;
    return read_num((char*)obj + A.field_get_offset(f), A.type_get_type(A.field_get_type(f)));
}
/* смещение первого поля структуры: у value-типов в метаданных оно бывает с учётом заголовка объекта */
static int struct_shift(void* sclass) {
    void* it = NULL; void* f; int minoff = 0x7fffffff;
    while ((f = A.class_get_fields(sclass, &it))) {
        if (A.field_get_flags(f) & 0x10) continue;
        if (A.field_get_offset(f) < minoff) minoff = A.field_get_offset(f);
    }
    return (minoff != 0x7fffffff && minoff >= (int)HDR) ? (int)HDR : 0;
}
static int int_field(void* o, const char* a, const char* b, const char* c, int dflt) {
    int ok = 0; double v;
    v = num_field(o, a, &ok); if (ok) return (int)v;
    if (b) { v = num_field(o, b, &ok); if (ok) return (int)v; }
    if (c) { v = num_field(o, c, &ok); if (ok) return (int)v; }
    return dflt;
}

/* ───────────── безопасное чтение памяти ───────────── */
/* Можно ли безопасно читать n байт по адресу p. Проверка через write() в неблокирующий pipe: ядро копирует память пользователя
   и при недоступной странице вернёт короткую запись / EFAULT вместо падения игры. Нужна, потому что в списках игроков и NPC
   бывают устаревшие записи, и разыменовать «мусорный» указатель = вылет или зависание. */
static int g_hm_pipe[2] = { -1, -1 }; static volatile int g_hm_pipeLock;
static int hm_readable(const void* p, unsigned n) {
    char buf[64]; long w; int ok;
    if (!p || (uptr)p < 0x10000 || n == 0 || n > sizeof buf) return 0;
    while (__sync_lock_test_and_set(&g_hm_pipeLock, 1)) usleep(50);
    if (g_hm_pipe[0] == -1) {
        if (pipe(g_hm_pipe)) g_hm_pipe[0] = -2;
        else { fcntl(g_hm_pipe[0], 4, 0x800); fcntl(g_hm_pipe[1], 4, 0x800); }       /* F_SETFL, O_NONBLOCK */
    }
    if (g_hm_pipe[0] < 0) { __sync_lock_release(&g_hm_pipeLock); return 1; }            /* pipe не создался — проверить не можем */
    w = write(g_hm_pipe[1], p, n);
    ok = (w == (long)n);
    if (w > 0) while (read(g_hm_pipe[0], buf, sizeof buf) > 0) {}
    __sync_lock_release(&g_hm_pipeLock);
    return ok;
}
/* объект живой на вид: заголовок читается и указатель на класс ведёт в читаемую память */
static int hm_obj_ok(const void* o) { return o && hm_readable(o, 16) && hm_readable(*(void* const*)o, 16); }
/* вся область [p, p+bytes) читается: по одному байту на страницу и последний байт */
static int hm_range_ok(const void* p, long bytes) {
    long o;
    if (!p || bytes <= 0 || bytes > (1L << 22)) return 0;
    for (o = 0; o < bytes; o += 4096) if (!hm_readable((const char*)p + o, 1)) return 0;
    return hm_readable((const char*)p + bytes - 1, 1);
}
/* Ссылочное поле С ПРОВЕРКОЙ ТИПА и памяти: bool/число/структуру как указатель не читаем, «мусорный» объект не трогаем. */
static void* ref_field_s(void* obj, const char* name) {
    void *k, *f; int tk, off;
    if (!obj || !hm_readable(obj, 16)) return NULL;
    k = *(void**)obj;
    if (!hm_readable(k, 16)) return NULL;                       /* указатель на класс должен вести в живую память */
    k = A.object_get_class(obj);
    f = find_field(k, name);
    if (!f) return NULL;
    tk = A.type_get_type(A.field_get_type(f));
    if (!(tk == 0x12 || tk == 0x15 || tk == 0x1c || tk == 0x0e || tk == 0x1d || tk == 0x14)) return NULL;   /* CLASS, GENERICINST, OBJECT, STRING, SZARRAY, ARRAY */
    off = A.field_get_offset(f);
    if (off < 0 || off > 0x2000 || !hm_readable((char*)obj + off, sizeof(void*))) return NULL;
    return *(void**)((char*)obj + off);
}

/* ───────────── поиск классов ───────────── */
static void* g_img;
static void* g_cPools, *g_cPlayerPool;
static void* g_poolSrcField;       /* static-поле, в котором лежит PlayerPool (или Pools) */
static int g_poolSrcIsPools;

static void* find_image(void) {
    uptr n = 0, i; void** as;
    if (!g_domain) return NULL;
    as = A.domain_get_assemblies(g_domain, &n);
    for (i = 0; as && i < n; i++) {
        void* img = A.assembly_get_image(as[i]);
        const char* nm = img ? A.image_get_name(img) : NULL;
        if (nm && sprefix(nm, "Assembly-CSharp") && !sprefix(nm, "Assembly-CSharp-")) return img;
    }
    return NULL;
}

static void* find_image_pre(const char* pre) {
    uptr n = 0, i; void** as;
    if (!g_domain) return NULL;
    as = A.domain_get_assemblies(g_domain, &n);
    for (i = 0; as && i < n; i++) {
        void* img = A.assembly_get_image(as[i]);
        const char* nm = img ? A.image_get_name(img) : NULL;
        if (nm && sprefix(nm, pre)) return img;
    }
    return NULL;
}
static __thread int tl_attached;
static void ensure_attached(void) {
    if (!tl_attached && g_domain) { A.thread_attach(g_domain); tl_attached = 1; }
}
/* libil2cpp.so уже загружена игрой? Смотрим /proc/self/maps — это обычное чтение файла, с динамическим загрузчиком не связано.
   (dlopen(..., RTLD_NOLOAD) из фонового потока, пока главный поток грузит библиотеки, ронял игру — журнал оборвался на этом цикле.)
   Возврат: 1 — есть, 0 — нет, -1 — maps прочитать нельзя. */
static int lib_mapped(void) {
    static char buf[8192];
    int fd, found = 0, keep = 0, i; long n; const char* pat = "libil2cpp.so";
    fd = open("/proc/self/maps", 0);
    if (fd < 0) return -1;
    for (;;) {
        n = read(fd, buf + keep, sizeof buf - 1 - keep);
        if (n <= 0) break;
        n += keep; buf[n] = 0;
        for (i = 0; i < n; i++) if (buf[i] == 'l' && sprefix(buf + i, pat)) { found = 1; break; }
        if (found) break;
        keep = 11 < n ? 11 : (int)n;                      /* хвост, чтобы не потерять совпадение на границе блоков */
        for (i = 0; i < keep; i++) buf[i] = buf[n - keep + i];
    }
    close(fd);
    return found;
}
/* Лёгкая проверка: libil2cpp загружена, домен создан, Assembly-CSharp виден. Классов не трогает, поток не присоединяет.
   Вызывать только после g_safe: ранние вызовы il2cpp_domain_get/…_get_assemblies (до конца il2cpp_init) роняют игру.
   Шаги пишутся в журнал ДО вызова — если игра всё же упадёт, последняя строка покажет, на каком вызове. */
static int il2cpp_light(void) {
    static int l1, l2, l3;
    if (!g_safe) return 0;
    if (!g_il2cpp && !l1) { l1 = 1; dlog("probe: il2cpp_open (dlsym функций API)"); }
    if (!il2cpp_open()) return 0;
    if (!g_domain) {
        if (!l2) { l2 = 1; dlog("probe: il2cpp_domain_get"); }
        g_domain = A.domain_get();
        if (g_domain) dlog("probe: домен получен");
    }
    if (!g_domain) return 0;
    if (!g_img) {
        if (!l3) { l3 = 1; dlog("probe: il2cpp_domain_get_assemblies + поиск Assembly-CSharp"); }
        g_img = find_image();
        if (g_img) dlog("probe: Assembly-CSharp найден");
    }
    return g_img != NULL;
}
static void* g_cGame, *g_fGameInstance;
/* Игра запущена: Hassle.Game.Instance создан (Game.Awake отработал). Нужен присоединённый поток. */
static int game_ready(void) {
    void* inst = NULL;
    if (!il2cpp_light()) return 0;
    if (!g_cGame) g_cGame = A.class_from_name(g_img, "Hassle", "Game");
    if (!g_cGame) return 0;
    if (!g_fGameInstance) g_fGameInstance = A.class_get_field_from_name(g_cGame, "Instance");
    if (!g_fGameInstance) return 0;
    ensure_attached();
    A.field_static_get_value(g_fGameInstance, &inst);
    return inst != NULL;
}

static void dump_class(void* k, const char* title) {
    void* it = NULL; void* f; int guard = 0;
    char t[600]; Buf b;
    b_init(&b, t, sizeof t); b_s(&b, "== "); b_s(&b, title); b_s(&b, " ("); b_s(&b, A.class_get_name(k)); b_s(&b, ")");
    dlog(t);
    for (; k && guard < 6; k = A.class_get_parent(k), guard++) {
        it = NULL;
        while ((f = A.class_get_fields(k, &it))) {
            void* ty = A.field_get_type(f);
            b_init(&b, t, sizeof t);
            b_s(&b, "   "); b_s(&b, A.class_get_name(k)); b_s(&b, "."); b_s(&b, A.field_get_name(f));
            b_s(&b, " : "); b_s(&b, ty ? A.type_get_name(ty) : "?");
            b_s(&b, " @"); b_int(&b, A.field_get_offset(f));
            if (A.field_get_flags(f) & 0x10) b_s(&b, " static");
            dlog(t);
        }
    }
}

/* PlayerPool может лежать в статическом поле Pools.Players, либо в статическом поле любого класса;
   запоминаем найденное static-поле и каждый тик читаем из него свежее значение. */
static void* current_pool(void) {
    void* v = NULL;
    if (!g_poolSrcField) return NULL;
    A.field_static_get_value(g_poolSrcField, &v);
    if (!v) return NULL;
    if (g_poolSrcIsPools) return ref_field(v, "Players");
    return v;
}
static void scan_for_pool_run(void) {
    uptr n, i;
    void* it; void* f;
    /* 1) Pools.Players как статическое поле */
    if (g_cPools) {
        f = A.class_get_field_from_name(g_cPools, "Players");
        if (f && (A.field_get_flags(f) & 0x10)) { g_poolSrcField = f; g_poolSrcIsPools = 0; dlog("PlayerPool: Pools.Players (static)"); return; }
    }
    /* 2) любое static-поле типа PlayerPool или Pools */
    n = A.image_get_class_count(g_img);
    for (i = 0; i < n; i++) {
        void* k = A.image_get_class(g_img, i);
        if (!k) continue;
        it = NULL;
        while ((f = A.class_get_fields(k, &it))) {
            void* ty; void* c;
            if (!(A.field_get_flags(f) & 0x10) || (A.field_get_flags(f) & 0x40)) continue;
            ty = A.field_get_type(f);
            if (!ty || A.type_get_type(ty) != 0x12) continue;      /* CLASS */
            c = A.class_from_type(ty);
            if (c == g_cPlayerPool) {
                g_poolSrcField = f; g_poolSrcIsPools = 0;
                dlog2("PlayerPool: static-поле ", A.field_get_name(f)); dlog2("   в классе ", A.class_get_name(k));
                return;
            }
            if (c == g_cPools) {
                g_poolSrcField = f; g_poolSrcIsPools = 1;
                dlog2("Pools: static-поле ", A.field_get_name(f)); dlog2("   в классе ", A.class_get_name(k));
                return;
            }
        }
    }
}
static volatile int g_poolBusy;
static void scan_for_pool(void) {                        /* поиск static-поля с PlayerPool; зовут ядро и моды, второй поток не дублирует скан */
    if (g_poolSrcField) return;
    if (__sync_lock_test_and_set(&g_poolBusy, 1)) return;
    scan_for_pool_run();
    __sync_lock_release(&g_poolBusy);
}
static int pool_classes(void) {                          /* классы PlayerPool и Pools найдены */
    if (!g_cPlayerPool) {
        g_cPlayerPool = A.class_from_name(g_img, "Hassle.Global.Pooling", "PlayerPool");
        g_cPools = A.class_from_name(g_img, "Hassle.Global.Pooling", "Pools");
    }
    return g_cPlayerPool != NULL;
}
static int pool_ensure(void) {                           /* 1 — static-поле с пулом игроков найдено */
    if (g_poolSrcField) return 1;
    if (!g_img || !pool_classes()) return 0;
    ensure_attached();
    scan_for_pool();
    return g_poolSrcField != NULL;
}
/* LocalPlayer текущего пула игроков или NULL (пул ещё не создан / игрок не появился). Поле читается с проверкой памяти. */
static void* local_player(void) {
    void* pool = current_pool();
    return pool ? ref_field_s(pool, "LocalPlayer") : NULL;
}

/* ───────────── перебор коллекции ───────────── */
typedef void (*item_cb)(void* item, void* ctx);

static void walk_ref_array(void* arr, int count, int stride_is_ptr, item_cb cb, void* ctx) {
    int i; char* data;
    if (!arr) return;
    {
        long len = *(long*)((char*)arr + 3 * sizeof(void*));
        if (len < 0 || len > 100000) return;
        if (count < 0 || count > len) count = (int)len;
    }
    data = (char*)arr + 4 * sizeof(void*);
    (void)stride_is_ptr;
    for (i = 0; i < count; i++) {
        void* it = *(void**)(data + (uptr)i * sizeof(void*));
        if (it) cb(it, ctx);
    }
}
/* массив структур (записи Dictionary/HashSet): берём ссылочное поле fld из каждой записи */
static void walk_struct_array(void* arr, int count, const char* fld, item_cb cb, void* ctx) {
    void* ac; void* ec; void* f; int esz, i, off, shift; char* data; long len;
    if (!arr) return;
    ac = A.object_get_class(arr);
    ec = A.class_get_element_class(ac);
    esz = A.class_array_element_size(ac);
    if (!ec || esz <= 0) return;
    f = find_field(ec, fld);
    if (!f) return;
    shift = A.class_is_valuetype(ec) ? struct_shift(ec) : 0;
    off = A.field_get_offset(f) - shift;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 100000) return;
    if (count < 0 || count > len) count = (int)len;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < count; i++) {
        void* it = *(void**)(data + (uptr)i * (uptr)esz + off);
        if (it) cb(it, ctx);
    }
}

/* Какой из трёх «последних пакетов синхронизации» актуален: 1 — пешком, 2 — водитель, 3 — пассажир.
   БЫЛА ОШИБКА: раньше тут читалось поле «NetworkUpdate», но у Player это bool («обновление по сети включено»), а read_num bool не знает
   и возвращал 0 при ok=1 — до «m_NetworkUpdateType» дело не доходило, и ВСЕГДА брался пакет «пешком». У сидящих в машине (и у вас самих
   за рулём/пассажиром) позиция застревала в точке, где они сели, — отсюда «ближе тот, кто стоит, хотя я рядом с другим».
   Теперь: у RemotePlayer — его собственный m_NetworkUpdateType (enum int32: None 0, OnFoot 1, Driver 2, Passenger 3);
   у LocalPlayer такого поля нет — смотрим Vehicle и get_IsDriver(). */
static int sync_kind(void* player) {
    void* k = A.object_get_class(player); void* f; void* m;
    f = find_field(k, "m_NetworkUpdateType");
    if (f) {
        int v = *(int*)((char*)player + A.field_get_offset(f));
        if (v >= 1 && v <= 3) return v;
    }
    if (!ref_field(player, "Vehicle")) return 1;
    m = find_method(k, "get_IsDriver", 0);
    if (m) {
        void* exc = NULL; void* r = A.runtime_invoke(m, player, NULL, &exc);
        if (!exc && r) return *((u8*)r + HDR) ? 2 : 3;
    }
    return 2;
}

/* Позиция игрока из его последнего пакета синхронизации. kind (можно NULL) — какой это пакет: 1 пешком, 2 водитель, 3 пассажир. */
static int read_pos_k(void* player, float* out, int* kind) {
    int nu, ok = 0; const char* sname; void* sf; void* sc; char* sp; int shift, poff; void* pf;
    nu = sync_kind(player); if (kind) *kind = nu;
    sname = nu == 2 ? "m_LastDriveSyncData" : nu == 3 ? "m_LastPassengerSyncData" : "m_LastOnFootSyncData";
    sf = find_field(A.object_get_class(player), sname);
    if (!sf) return 0;
    sc = A.class_from_type(A.field_get_type(sf));
    if (!sc) return 0;
    if (A.class_is_valuetype(sc)) sp = (char*)player + A.field_get_offset(sf);
    else { sp = *(char**)((char*)player + A.field_get_offset(sf)); if (!sp) return 0; shift = 0; goto have; }
    shift = struct_shift(sc);
have:
    pf = find_field(sc, "Position");
    if (!pf) return 0;
    poff = A.field_get_offset(pf) - (A.class_is_valuetype(sc) ? shift : 0);
    if (!A.class_is_valuetype(sc)) poff = A.field_get_offset(pf);
    {
        float* v = (float*)(sp + poff);
        if (!(v[0] == v[0] && v[1] == v[1] && v[2] == v[2])) return 0;
        if (v[0] > 1e6f || v[0] < -1e6f || v[1] > 1e6f || v[1] < -1e6f) return 0;
        out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; ok = 1;
    }
    return ok;
}

static int read_pos(void* player, float* out) { return read_pos_k(player, out, NULL); }

/* UTF-16 → UTF-8 */
static void utf16_to_utf8(const u16* s, int n, char* out, int cap) {
    int i, o = 0;
    for (i = 0; i < n && o + 4 < cap; i++) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xE000) { out[o++] = '?'; i++; continue; }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}

/* Текущая стадия запуска: её читают моды (Near отдаёт в JSON, /rec — в diag); ядро обновляет её по ходу ожидания игры. */
static char g_stage[160] = "старт";

static void set_stage(const char* s) {
    unsigned i = 0;
    if (seq(g_stage, s)) return;
    for (; s[i] && i < sizeof g_stage - 1; i++) g_stage[i] = s[i];
    g_stage[i] = 0;
    dlog2("стадия: ", s);
}

static void init_log(void) {
    void* lg;
    if (g_alog) return;
    lg = dlopen("liblog.so", RTLD_NOW_);
    if (lg) *(void**)&g_alog = dlsym(lg, "__android_log_write");
}

/* Выключатель для диагностики: если существует файл konstt_off, мод ничего не запускает (работает только подмена оригинала).
   /sdcard/Android/data/<пакет>/files/konstt_off  или  /data/local/tmp/konstt_off */
static int off_switch(void) {
    char cmd[128], path[256]; Buf b; int fd; long n, i;
    fd = open("/data/local/tmp/konstt_off", 0);
    if (fd >= 0) { close(fd); return 1; }
    fd = open("/proc/self/cmdline", 0);
    if (fd < 0) return 0;
    n = read(fd, cmd, sizeof cmd - 1); close(fd);
    if (n <= 0) return 0;
    cmd[n] = 0;
    for (i = 0; cmd[i]; i++) if (cmd[i] == ':') { cmd[i] = 0; break; }
    b_init(&b, path, sizeof path);
    b_s(&b, "/sdcard/Android/data/"); b_s(&b, cmd); b_s(&b, "/files/konstt_off");
    fd = open(path, 0);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

/* Сколько секунд ждать после появления libil2cpp: по умолчанию KONSTT_SETTLE_S, либо число из файла HLOG/konstt_delay.txt */
static int settle_seconds(void) {
    char p[330], tmp[16]; unsigned L = slen(g_flog); int i, fd, v = -1, k = 0; long r;
    if (L > HM_LOGNAME_LEN + 1 && L < 300) {
        for (i = 0; i < (int)(L - HM_LOGNAME_LEN); i++) p[i] = g_flog[i];   /* папка со слэшем (без «konstt.log») */
        p[i] = 0;
        { const char* nm = "konstt_delay.txt"; for (k = 0; nm[k]; k++) p[i + k] = nm[k]; p[i + k] = 0; }
        fd = open(p, 0);
        if (fd >= 0) {
            r = read(fd, tmp, sizeof tmp - 1); close(fd);
            if (r > 0) {
                tmp[r] = 0; v = 0;
                for (k = 0; k < r && tmp[k] >= '0' && tmp[k] <= '9' && v < 100000; k++) v = v * 10 + (tmp[k] - '0');
                if (k == 0) v = -1;
            }
        }
    }
    if (v < 0) return KONSTT_SETTLE_S;
    if (v > 600) v = 600;
    dlog_i("collector: задержка взята из konstt_delay.txt, секунд ", v);
    return v;
}

/* ───────────── реестр чат-команд игры ─────────────
   В игре есть реестр чат-команд: WebViewSystem.m_ChatCommandEvents (Dictionary<string, ChatCommandEvent>), его заполняет сама игра через
   RegisterChatCommandEvent; WebViewSystem.SendChatInput(текст) ищет команду там. Мы берём любой уже зарегистрированный игрой делегат
   (с целью-объектом), клонируем его и подменяем method_ptr на нашу функцию — IL2CPP вызовет её как (this, string[] args, MethodInfo*).
   Команда выполняется в главном потоке Unity. Ключи кладём и «/имя», и «имя» (какой формат ждёт реестр — видно по журналу).
   Образец делегата ищет поток ядра (cmd_find_template), команды регистрируют сами моды (hm_cmd_install). Записи в словарь идут
   под замком g_cmdLock: словарь не потокобезопасен, а модов-«писателей» несколько. */
static int g_cmdTries, g_keyLogged;
static void* g_tplDelegate;
static int g_dictCount, g_keyHasSlash;
static void* g_cmdKls;                                   /* класс делегата чат-команды */
static int g_cmdSize, g_cmdOffMp, g_cmdOffIi;            /* размер делегата, смещения method_ptr и invoke_impl (0 — нет) */
static volatile int g_cmdLock;
typedef void (*native_cmd_fn)(void* self, void* args, void* mi);


static void exc_text(void* exc, char* out, int cap) {
    void* m; void* e2 = NULL; void* s;
    out[0] = 0;
    if (!exc) return;
    m = find_method(A.object_get_class(exc), "ToString", 0);
    if (!m) return;
    s = A.runtime_invoke(m, exc, NULL, &e2);
    if (e2 || !s) return;
    { int n = A.string_length(s); if (n > 240) n = 240; utf16_to_utf8(A.string_chars(s), n, out, cap); }
}

typedef void (*kv_cb)(void* key, void* val, void* ctx);
static void dict_each(void* dict, kv_cb cb, void* ctx) {
    void *arr, *ac, *ec, *fk, *fv, *fh; int esz, i, shift, cnt, offk, offv, offh = 0; char* data; long len;
    if (!dict) return;
    arr = ref_field(dict, "_entries"); if (!arr) arr = ref_field(dict, "entries");
    if (!arr) return;
    ac = A.object_get_class(arr); ec = A.class_get_element_class(ac); esz = A.class_array_element_size(ac);
    if (!ec || esz <= 0) return;
    fk = find_field(ec, "key"); fv = find_field(ec, "value"); fh = find_field(ec, "hashCode");
    if (!fk || !fv) return;
    shift = A.class_is_valuetype(ec) ? struct_shift(ec) : 0;
    offk = A.field_get_offset(fk) - shift; offv = A.field_get_offset(fv) - shift;
    if (fh) offh = A.field_get_offset(fh) - shift;
    cnt = int_field(dict, "_count", "count", NULL, -1);
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 100000) return;
    if (cnt < 0 || cnt > len) cnt = (int)len;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < cnt; i++) {
        char* e = data + (uptr)i * (uptr)esz;
        if (fh && *(int*)(e + offh) < 0) continue;        /* свободная запись словаря */
        cb(*(void**)(e + offk), *(void**)(e + offv), ctx);
    }
}
static void tpl_cb(void* key, void* val, void* ctx) {
    (void)ctx;
    g_dictCount++;
    if (key) {
        int n = A.string_length(key); const u16* ch = A.string_chars(key);
        if (n > 0 && ch && ch[0] == '/') g_keyHasSlash = 1;
        if (g_keyLogged < 40 && n > 0 && n < 60 && ch) {
            char tmp[160]; utf16_to_utf8(ch, n, tmp, sizeof tmp);
            dlog2("реестр команд: ключ ", tmp); g_keyLogged++;
        }
    }
    if (val && !g_tplDelegate) {
        void* tgt = ref_field(val, "m_target"); void* dels = ref_field(val, "delegates");
        if (tgt && !dels) g_tplDelegate = val;           /* обычный (не multicast) делегат с целью — годится как образец */
    }
}


static void chat_say(const char* msg) {
    void *cw, *m, *exc = NULL, *args[2];
    if (!A.string_new || !g_img) return;
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    if (!cw) return;
    m = A.class_get_method_from_name(cw, "AddChatMessage", 2);
    if (!m) return;
    args[0] = A.string_new(msg); args[1] = A.string_new("white");
    A.runtime_invoke(m, NULL, args, &exc);
}

/* аргументы команды: string[]; число секунд — первый числовой элемент (не важно, входит ли сама команда в массив); «diag» — диагностика */
static int parse_cmd_args(void* arr, int* diag) {
    long len; int i, t = -1; char* data;
    *diag = 0;
    if (!arr) return -1;
    len = *(long*)((char*)arr + 3 * sizeof(void*));
    if (len < 0 || len > 16) return -1;
    data = (char*)arr + 4 * sizeof(void*);
    for (i = 0; i < len; i++) {
        void* s = *(void**)(data + (uptr)i * sizeof(void*));
        char tmp[64]; int n; const u16* ch;
        if (!s) continue;
        n = A.string_length(s); ch = A.string_chars(s);
        if (n <= 0 || n > 40 || !ch) continue;
        utf16_to_utf8(ch, n, tmp, sizeof tmp);
        if (seq(tmp, "diag")) { *diag = 1; continue; }
        if (t < 0 && tmp[0] >= '0' && tmp[0] <= '9') {
            int v = 0, k = 0;
            while (tmp[k] >= '0' && tmp[k] <= '9' && v < 100000) { v = v * 10 + (tmp[k] - '0'); k++; }
            t = v;
        }
    }
    return t;
}

static void cmd_fail(const char* why) { __atomic_store_n(&g_tplState, -1, __ATOMIC_RELEASE); dlog2("реестр команд: не получилось. Причина: ", why); }
static int cmd_put(void* dict, void* setitem, const char* key, void* del) {
    void* args[2]; void* exc = NULL; char et[256];
    args[0] = A.string_new(key); args[1] = del;
    A.runtime_invoke(setitem, dict, args, &exc);
    if (exc) { exc_text(exc, et, sizeof et); dlog2("реестр команд: set_Item бросил: ", et); return 0; }
    return 1;
}
static void* clone_cmd_delegate(void* kls, int size, int off_mp, int off_ii, uptr fn) {
    void* nd = A.object_new(kls); int i; uptr mp, ii;
    if (!nd) return NULL;
    for (i = 2 * (int)sizeof(void*); i < size; i++) ((char*)nd)[i] = ((char*)g_tplDelegate)[i];
    mp = *(uptr*)((char*)g_tplDelegate + off_mp);
    *(uptr*)((char*)nd + off_mp) = fn;
    if (off_ii) {
        ii = *(uptr*)((char*)g_tplDelegate + off_ii);
        if (ii == mp) *(uptr*)((char*)nd + off_ii) = fn;
    }
    return nd;
}

/* Ищет образец делегата чат-команды (зовёт поток ядра раз в TICK_US, пока g_tplState == 0). Дойдя до конца, выставляет g_tplState = 1. */
static void cmd_find_template(void) {
    void *cw, *fd, *dict = NULL, *kls, *mset, *f_mp, *f_ii; int size, off_mp, off_ii;
    if (g_tplState) return;
    if (++g_cmdTries > 1500) { cmd_fail("реестр команд так и не заполнился (нет образца делегата)"); return; }
    if (!A.string_new || !A.object_new || !A.class_instance_size) { cmd_fail("нет il2cpp_string_new / object_new / class_instance_size"); return; }
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    if (!cw) return;
    fd = A.class_get_field_from_name(cw, "m_ChatCommandEvents");
    if (!fd) { cmd_fail("поле WebViewSystem.m_ChatCommandEvents не найдено"); return; }
    A.field_static_get_value(fd, &dict);
    if (!dict) return;                                   /* реестр ещё не создан */
    g_dictCount = 0; g_tplDelegate = NULL; g_keyHasSlash = 0;
    dict_each(dict, tpl_cb, NULL);
    if (!g_tplDelegate) return;                          /* игра ещё не зарегистрировала ни одной команды — образца нет, ждём */

    kls = A.object_get_class(g_tplDelegate);
    size = A.class_instance_size(kls);
    if (size < 0x40 || size > 0x200) { cmd_fail("странный размер делегата"); return; }
    f_mp = find_field(kls, "method_ptr");
    if (!f_mp) { cmd_fail("в делегате нет поля method_ptr"); return; }
    off_mp = A.field_get_offset(f_mp);
    f_ii = find_field(kls, "invoke_impl");
    off_ii = f_ii ? A.field_get_offset(f_ii) : 0;
    mset = find_method(A.object_get_class(dict), "set_Item", 2);
    if (!mset) { cmd_fail("у словаря нет set_Item"); return; }
    g_cmdKls = kls; g_cmdSize = size; g_cmdOffMp = off_mp; g_cmdOffIi = off_ii;
    dlog_i("реестр команд: записей ", g_dictCount);
    dlog_i("реестр команд: ключи начинаются с «/» ", g_keyHasSlash);
    dlog("реестр команд: образец делегата найден, команды можно регистрировать");
    __atomic_store_n(&g_tplState, 1, __ATOMIC_RELEASE);
}

/* Кладёт нативную команду в реестр под ключами «/имя» и «имя». Из любого присоединённого потока, после g_tplState == 1.
   Возврат: 1 — зарегистрирована, 0 — реестра пока нет (повторить позже), -1 — не вышло. */
static int cmd_register(const char* name, native_cmd_fn fn) {
    void *cw, *fd, *dict = NULL, *mset, *nd; char k1[80], k2[80], msg[160]; Buf b; int ok;
    if (__atomic_load_n(&g_tplState, __ATOMIC_ACQUIRE) != 1 || !g_img) return -1;
    ensure_attached();
    cw = A.class_from_name(g_img, "Hassle.WebView", "WebViewSystem");
    fd = cw ? A.class_get_field_from_name(cw, "m_ChatCommandEvents") : NULL;
    if (!fd) { dlog2("реестр команд: поле WebViewSystem.m_ChatCommandEvents не найдено; команда ", name); return -1; }
    A.field_static_get_value(fd, &dict);
    if (!dict) return 0;
    mset = find_method(A.object_get_class(dict), "set_Item", 2);
    if (!mset) { dlog2("реестр команд: у словаря нет set_Item; команда ", name); return -1; }
    b_init(&b, k1, sizeof k1); b_ch(&b, '/'); b_s(&b, name);
    b_init(&b, k2, sizeof k2); b_s(&b, name);
    while (__sync_lock_test_and_set(&g_cmdLock, 1)) usleep(200);
    nd = clone_cmd_delegate(g_cmdKls, g_cmdSize, g_cmdOffMp, g_cmdOffIi, (uptr)fn);
    ok = nd && (cmd_put(dict, mset, k1, nd) & cmd_put(dict, mset, k2, nd));
    __sync_lock_release(&g_cmdLock);
    b_init(&b, msg, sizeof msg);
    b_s(&b, "нативная /"); b_s(&b, name);
    if (ok) { b_s(&b, ": зарегистрирована («/"); b_s(&b, name); b_s(&b, "» и «"); b_s(&b, name); b_s(&b, "»)"); }
    else b_s(&b, ": НЕ зарегистрирована");
    dlog(msg);
    return ok ? 1 : -1;
}

/* ───────────── главный поток по заказу из фонового (UnitySynchronizationContext.Post) ─────────────
   Общий механизм для модов, которым нужны тики в главном потоке (/nocol в авто-режиме, /route). Захватывать контекст надо из ГЛАВНОГО
   потока (например, из обработчика чат-команды): SynchronizationContext.Current потокозависим. Дальше из любого потока:
   Post(clone_cmd_delegate(g_mt_kCb, g_cmdSize, g_cmdOffMp, g_cmdOffIi, своя_функция), строка) — функция выполнится в главном потоке. */
static void* g_mt_ctx;                             /* UnitySynchronizationContext главного потока */
static void* g_mt_postM;                           /* его Post(SendOrPostCallback, object) */
static void* g_mt_kCb;                             /* класс делегата для колбэка */
static int g_mt_ctxTries;
static void mt_capture_ctx(void) {
    void *img, *k, *m, *ctx, *exc = NULL, *kcb; const char* cn;
    if (g_mt_ctx || g_mt_ctxTries >= 3) return;
    g_mt_ctxTries++;
    img = find_image_pre("mscorlib");
    k = img ? A.class_from_name(img, "System.Threading", "SynchronizationContext") : NULL;
    m = k ? A.class_get_method_from_name(k, "get_Current", 0) : NULL;
    if (!m) { dlog("главный поток: нет SynchronizationContext.get_Current"); return; }
    ctx = A.runtime_invoke(m, NULL, NULL, &exc);
    if (exc || !ctx) { dlog("главный поток: SynchronizationContext.Current недоступен"); return; }
    cn = A.class_get_name(A.object_get_class(ctx));
    if (!seq(cn, "UnitySynchronizationContext")) { dlog2("главный поток: контекст не Unity, не используем: ", cn); return; }   /* иначе Post ушёл бы в пул потоков */
    m = find_method(A.object_get_class(ctx), "Post", 2);
    if (!m) { dlog("главный поток: у контекста нет Post"); return; }
    kcb = A.class_from_name(img, "System.Threading", "SendOrPostCallback");
    g_mt_kCb = (kcb && A.class_instance_size && A.class_instance_size(kcb) == g_cmdSize) ? kcb : g_cmdKls;
    g_mt_postM = m; g_mt_ctx = ctx;
    dlog2("главный поток: захвачен, делегат: ", A.class_get_name(g_mt_kCb));
}

/* ───────────── поток ядра: ожидание игры ───────────── */
static void* core_main(void* arg) {
    int w, sec, polls = 0, m;
    (void)arg;
    dlog("core: старт");
    set_stage("ждём загрузку libil2cpp.so");
    for (;;) {                                           /* только чтение /proc/self/maps, никаких вызовов dlopen/IL2CPP */
        m = lib_mapped();
        if (m > 0) break;
        if (m < 0 && polls > 60) { dlog("core: /proc/self/maps недоступен, продолжаю по таймеру"); break; }
        polls++;
        if (polls % 4 == 0) dlog_i("core: libil2cpp.so ещё не загружена, опросов ", polls);
        usleep(500000);
    }
    sec = settle_seconds();
    dlog_i("core: libil2cpp.so загружена; функции IL2CPP не трогаю, жду секунд ", sec);
    set_stage("IL2CPP инициализируется (мод ждёт)");
    for (w = 0; w < sec; w++) usleep(1000000);
    g_safe = 1;
    dlog("core: пауза прошла, начинаю опрос IL2CPP");
    set_stage("ждём Assembly-CSharp / Game.Instance");
    for (w = 0; !game_ready(); w++) {
        if (w % 20 == 0) dlog_i("core: игра ещё не готова, опросов ", w);
        usleep(POLL_US);
    }
    dlog("core: игра готова (Game.Instance создан)");
    for (w = 0; w < KONSTT_EXTRA_DELAY_S; w++) usleep(1000000);
    set_stage("игра готова");
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    for (;;) {                                           /* пул игроков и образец делегата чат-команды: ждём, пока игра их создаст */
        pool_ensure();
        cmd_find_template();
        if (g_poolSrcField && g_tplState) break;
        usleep(TICK_US);
    }
    dlog("core: пул игроков и реестр команд найдены, поток ядра завершён");
    return NULL;
}

/* ───────────── API для модов: ожидание ядра и регистрация команды ───────────── */
static int hm_wait_started(void) {
    while (!__atomic_load_n(&g_started, __ATOMIC_ACQUIRE)) usleep(100000);
    return !off_switch();                                /* konstt_off — моды выключены целиком */
}
static int hm_wait_ready(void) {
    if (!hm_wait_started()) return 0;
    while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) usleep(200000);
    return 1;
}
/* Блокирует поток мода: ждёт ядро, готовность игры и образец делегата, потом регистрирует команду. 1 — «/имя» работает. */
static int hm_cmd_install(const char* name, native_cmd_fn fn) {
    int i, r;
    if (!hm_wait_ready()) return 0;
    while (!__atomic_load_n(&g_tplState, __ATOMIC_ACQUIRE)) usleep(200000);
    if (g_tplState != 1) { dlog2("реестр команд недоступен, команды не будет: /", name); return 0; }
    for (i = 0; i < 120; i++) {
        r = cmd_register(name, fn);
        if (r) return r > 0;
        usleep(500000);                                  /* реестра пока нет — повторяем */
    }
    dlog2("реестр команд так и не появился, команды не будет: /", name);
    return 0;
}

/* ───────────── запуск ───────────── */
static void start_threads(void) {
    unsigned long t1 = 0;
    if (g_started) return;
    __atomic_store_n(&g_started, 1, __ATOMIC_RELEASE);
    init_log();
    if (off_switch()) { dlog("konstt_off найден: мод отключён, работает только оригинал"); return; }
    dlog("start_threads: konstt_off нет, мод включён");
    dlog("start_threads: запускаю поток ядра");
    pthread_create(&t1, NULL, core_main, NULL);
}

#if defined(KONSTT_BUNDLE)
/* Режим бандла: код скачан и загружен hassle_loader.c через dlopen. JNI_OnLoad и подмену оригинала уже сделал загрузчик.
   Конструкторы всех модулей из папки Mods срабатывают при dlopen; этот запускает ядро. */
__attribute__((constructor)) static void core_ctor(void) {
    init_log();
    dlog("core_ctor: бандл загружен загрузчиком (сборка " HM_BUILD_ID ")");
    start_threads();
}
EXPORT const char* hm_bundle_id(void) { return HM_BUILD_ID; }
#elif !defined(KONSTT_TEST)
__attribute__((constructor)) static void core_ctor(void) {
    init_log();
    dlog("core_ctor: наша .so загружена системой (ещё до JNI_OnLoad)");
}

typedef int (*jni_onload_t)(void*, void*);
typedef void (*jni_onunload_t)(void*, void*);
static void* g_orig;

/* Оригинал лежит рядом: libHassleExtend2.so (старый клиент) или libHassleMain2.so (чистый APK, подмена libmain.so) */
EXPORT int JNI_OnLoad(void* vm, void* reserved) {
    int ver = 0x00010006;                                  /* JNI_VERSION_1_6 */
    init_log();
    dlog_i("JNI_OnLoad: вход, разрядность указателя (бит) ", (long)(sizeof(void*) * 8));
    dlog("JNI_OnLoad: грузим оригинал (libHassleExtend2.so / libHassleMain2.so)");
    g_orig = dlopen("libHassleExtend2.so", RTLD_NOW_);
    if (g_orig) dlog("JNI_OnLoad: режим старого клиента (libHassleExtend2.so)");
    else {
        g_orig = dlopen("libHassleMain2.so", RTLD_NOW_);
        if (g_orig) dlog("JNI_OnLoad: режим чистого APK (libHassleMain2.so = оригинальный libmain.so)");
    }
    if (g_orig) {
        jni_onload_t f = (jni_onload_t)dlsym(g_orig, "JNI_OnLoad");
        dlog("JNI_OnLoad: оригинал загружен, вызываю его JNI_OnLoad");
        if (f) ver = f(vm, reserved);
        dlog_i("JNI_OnLoad: оригинал вернул версию ", ver);
    } else {
        const char* de = dlerror();
        dlog("JNI_OnLoad: оригинал (libHassleExtend2.so / libHassleMain2.so) НЕ загрузился");
        dlog2("JNI_OnLoad: dlerror: ", de ? de : "(нет)");
    }
    start_threads();
    dlog("JNI_OnLoad: выход");
    return ver;
}
EXPORT void JNI_OnUnload(void* vm, void* reserved) {
    if (g_orig) {
        jni_onunload_t f = (jni_onunload_t)dlsym(g_orig, "JNI_OnUnload");
        if (f) f(vm, reserved);
    }
}
#endif
