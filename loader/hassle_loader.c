/*
 * hassle_loader.c — ЕДИНСТВЕННАЯ нативная часть, которая вшивается в APK (patch_apk.py кладёт её как libmain.so / libHassleExtended.so).
 *
 * Зачем
 *   Раньше весь мод (Mods/hassle_near.c + nocol + route) был зашит в APK: любой новый .c = пересборка .so и новый APK.
 *   Теперь в APK лежит только этот маленький загрузчик. Всё остальное (все .c из папки Mods) GitHub Actions собирает в один бандл
 *   libkonstt_mods_<abi>.so и публикует в ветку dist. Загрузчик при КАЖДОМ запуске игры:
 *     1) грузит оригинал (libHassleExtend2.so / libHassleMain2.so) и отдаёт ему JNI_OnLoad — как делал hassle_near.c;
 *     2) в фоновом потоке по HTTPS (через Java HttpURLConnection, своего TLS в C нет) скачивает manifest.txt;
 *     3) если в manifest другой sha256, чем у закешированного бандла — докачивает новый бандл, проверяет размер и sha256;
 *     4) делает dlopen бандла; его конструкторы сами запускают Near / nocol / route / ваши новые модули.
 *   Нет сети — берётся последний проверенный бандл из кеша. Нет ни сети, ни кеша — игра просто работает без мода.
 *
 * Файлы
 *   /data/data/<пакет>/files/hm/bundle.so      закешированный бандл (после записи — только чтение, этого требует Android 14+)
 *   /data/data/<пакет>/files/hm/manifest.txt   manifest, с которым бандл был проверен
 *   /data/data/<пакет>/files/hm/strikes.txt    счётчик «бандл не дожил» (защита от вылетов, см. ниже)
 *   <HLOG>/loader.log                          журнал загрузчика (рядом с konstt.log, папка та же, что у мода)
 *   Выключатель: файл konstt_off (как раньше) — тогда загрузчик ничего не качает и не грузит.
 *
 * Формат manifest.txt (его пишет build_konstt.py в CI):
 *   loader_min 1
 *   build 20261023-134200-ab12cd3
 *   arm64 <sha256 hex> <размер> libkonstt_mods_arm64.so
 *   arm32 <sha256 hex> <размер> libkonstt_mods_arm32.so
 *
 * Безопасность: скачанный код выполняется внутри игры, поэтому sha256 проверяется ВСЕГДА, а адрес жёстко вшит в загрузчик
 * (HM_BASE_URL, только https). sha256 из того же репозитория защищает от битой/обрезанной загрузки, но не от тех, кто
 * получит запись в ваш репозиторий: включите 2FA на GitHub-аккаунте. Для настоящей защиты нужна подпись (Ed25519) — см. README.
 */

#ifndef HM_BASE_URL
#define HM_BASE_URL "https://raw.githubusercontent.com/CHANGE_ME/CHANGE_ME/dist/"   /* build_konstt.py подставляет из GITHUB_REPOSITORY */
#endif
#ifndef HM_ABI_KEY
#if defined(__aarch64__)
#define HM_ABI_KEY "arm64"
#else
#define HM_ABI_KEY "arm32"
#endif
#endif
#define HM_LOADER_VER 1
#define HM_MAX_BUNDLE (24u << 20)     /* бандл больше 24 МБ не принимаем */
#define HM_MAX_MANIFEST 4096
#define HM_NET_TIMEOUT_MS 10000

typedef unsigned long uptr;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
#ifndef NULL
#define NULL ((void*)0)
#endif
#define EXPORT __attribute__((visibility("default")))

extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern char* dlerror(void);
extern int open(const char*, int, ...);
extern long read(int, void*, unsigned long);
extern long write(int, const void*, unsigned long);
extern int close(int);
extern int mkdir(const char*, unsigned);
extern int rename(const char*, const char*);
extern int unlink(const char*);
extern int chmod(const char*, unsigned);
extern long time(long*);
extern void* malloc(unsigned long);
extern void* realloc(void*, unsigned long);
extern void free(void*);
extern void* memcpy(void*, const void*, unsigned long);
extern int pthread_create(unsigned long*, const void*, void* (*)(void*), void*);
extern int usleep(unsigned);
#define RTLD_NOW_ 2

/* ───────────── строки ───────────── */
static unsigned slen(const char* s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static int seq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int sprefix(const char* s, const char* p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }
static void sapp(char* d, unsigned cap, const char* s) { unsigned n = slen(d); while (s && *s && n + 1 < cap) d[n++] = *s++; d[n] = 0; }
static void sapp_i(char* d, unsigned cap, long v) {
    char t[24]; int i = 0; unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) sapp(d, cap, "-");
    if (!u) t[i++] = '0';
    while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    { char o[24]; int k = 0; while (i) o[k++] = t[--i]; o[k] = 0; sapp(d, cap, o); }
}

/* ───────────── журнал loader.log ───────────── */
static char g_lp[320];
static int g_lpInit;
static char g_pkg[100];
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
static int try_log_dir(const char* root) {
    char d[300]; int fd;
    d[0] = 0; sapp(d, sizeof d, root); sapp(d, sizeof d, g_pkg); mkdir(d, 0777);
    sapp(d, sizeof d, "/files"); mkdir(d, 0777);
    sapp(d, sizeof d, "/HLOG"); mkdir(d, 0777);
    g_lp[0] = 0; sapp(g_lp, sizeof g_lp, d); sapp(g_lp, sizeof g_lp, "/loader.log");
    fd = open(g_lp, 0x241, 0666);                      /* O_WRONLY|O_CREAT|O_TRUNC */
    if (fd < 0) return 0;
    close(fd);
    return 1;
}
static void lg(const char* a, const char* b) {
    char t[700]; int fd;
    if (!g_lpInit) {
        g_lpInit = 1;
        if (!pkg_name(g_pkg, sizeof g_pkg) || !(try_log_dir("/sdcard/Android/data/") || try_log_dir("/data/data/"))) g_lp[0] = 0;
    }
    t[0] = 0; sapp(t, sizeof t, "loader: "); sapp(t, sizeof t, a); sapp(t, sizeof t, b ? b : ""); sapp(t, sizeof t, "\n");
#ifdef HM_TEST
    write(2, t, slen(t));
#endif
    if (!g_lp[0]) return;
    fd = open(g_lp, 0x441, 0666);                      /* O_WRONLY|O_CREAT|O_APPEND */
    if (fd < 0) return;
    write(fd, t, slen(t)); close(fd);
}
static void lgi(const char* a, long v) { char t[40]; t[0] = 0; sapp_i(t, sizeof t, v); lg(a, t); }

/* ───────────── SHA-256 ───────────── */
typedef struct { u32 h[8]; u8 buf[64]; u64 len; u32 n; } Sha;
static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(Sha* s, const u8* p) {
    u32 w[64], a, b, c, d, e, f, g, h, t1, t2; int i;
    for (i = 0; i < 16; i++) w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(Sha* s) {
    static const u32 iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    int i; for (i = 0; i < 8; i++) s->h[i] = iv[i];
    s->len = 0; s->n = 0;
}
static void sha_upd(Sha* s, const u8* p, unsigned long n) {
    s->len += n;
    while (n) {
        unsigned take = 64 - s->n; if (take > n) take = (unsigned)n;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; n -= take;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}
static void sha_hex(Sha* s, char* out /* 65 байт */) {
    u64 bits = s->len * 8; u8 pad = 0x80, z = 0, lenb[8]; int i;
    sha_upd(s, &pad, 1);
    while (s->n != 56) sha_upd(s, &z, 1);
    for (i = 0; i < 8; i++) lenb[i] = (u8)(bits >> (56 - 8 * i));
    sha_upd(s, lenb, 8);
    for (i = 0; i < 32; i++) { u8 v = (u8)(s->h[i / 4] >> (24 - 8 * (i % 4))); out[2 * i] = "0123456789abcdef"[v >> 4]; out[2 * i + 1] = "0123456789abcdef"[v & 15]; }
    out[64] = 0;
}
static void sha256_hex(const u8* p, unsigned long n, char* out) { Sha s; sha_init(&s); sha_upd(&s, p, n); sha_hex(&s, out); }

/* ───────────── файлы ───────────── */
static u8* read_file(const char* path, unsigned long max, unsigned long* outn) {
    int fd = open(path, 0); u8* b; unsigned long cap = 65536, n = 0; long r;
    if (fd < 0) return NULL;
    b = (u8*)malloc(cap);
    if (!b) { close(fd); return NULL; }
    for (;;) {
        if (n == cap) {
            u8* nb; if (cap >= max) { free(b); close(fd); return NULL; }
            cap *= 2; nb = (u8*)realloc(b, cap); if (!nb) { free(b); close(fd); return NULL; } b = nb;
        }
        r = read(fd, b + n, cap - n);
        if (r < 0) { free(b); close(fd); return NULL; }
        if (r == 0) break;
        n += (unsigned long)r;
    }
    close(fd);
    *outn = n;
    return b;
}
/* пишет во временный файл, делает его read-only (Android 14+ не грузит код из записываемых файлов), затем атомарно подменяет */
static int write_atomic(const char* path, const u8* data, unsigned long n) {
    char tmp[340]; int fd; unsigned long done = 0;
    tmp[0] = 0; sapp(tmp, sizeof tmp, path); sapp(tmp, sizeof tmp, ".tmp");
    unlink(tmp);
    fd = open(tmp, 0x241, 0600);
    if (fd < 0) return 0;
    while (done < n) { long w = write(fd, data + done, n - done); if (w <= 0) { close(fd); unlink(tmp); return 0; } done += (unsigned long)w; }
    close(fd);
    chmod(tmp, 0400);
    if (rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

/* ───────────── manifest ───────────── */
typedef struct { int ok; int loader_min; char build[80]; char sha[65]; unsigned long size; char file[100]; } Manifest;
static int is_hex64(const char* s) { int i; for (i = 0; i < 64; i++) { char c = s[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; } return s[64] == 0 || s[64] == ' '; }
static int safe_name(const char* s) {
    int i; if (!s[0]) return 0;
    for (i = 0; s[i]; i++) { char c = s[i]; if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0; }
    return 1;
}
static void parse_manifest(const u8* d, unsigned long n, Manifest* m) {
    char line[256]; unsigned long i = 0; int k;
    for (k = 0; k < (int)sizeof *m; k++) ((char*)m)[k] = 0;
    while (i < n) {
        unsigned L = 0; char* p; char* sp; char* sp2;
        while (i < n && d[i] != '\n') { if (L + 1 < sizeof line && d[i] != '\r') line[L++] = (char)d[i]; i++; }
        i++; line[L] = 0;
        if (sprefix(line, "loader_min ")) {
            m->loader_min = 0; for (p = line + 11; *p >= '0' && *p <= '9'; p++) m->loader_min = m->loader_min * 10 + (*p - '0');
        } else if (sprefix(line, "build ")) {
            unsigned j = 0; for (p = line + 6; *p && j + 1 < sizeof m->build; p++) m->build[j++] = *p; m->build[j] = 0;
        } else {
            const char* key = HM_ABI_KEY; unsigned kl = slen(key);
            unsigned q; int match = 1;
            for (q = 0; q < kl; q++) if (line[q] != key[q]) match = 0;
            if (!match || line[kl] != ' ') continue;
            p = line + kl + 1;
            if (!is_hex64(p)) continue;
            sp = p + 65; if (p[64] != ' ') continue;
            { unsigned j; for (j = 0; j < 64; j++) m->sha[j] = p[j]; m->sha[64] = 0; }
            m->size = 0; for (; *sp >= '0' && *sp <= '9'; sp++) m->size = m->size * 10 + (unsigned long)(*sp - '0');
            if (*sp != ' ') continue;
            sp2 = sp + 1;
            if (!safe_name(sp2) || slen(sp2) >= sizeof m->file) continue;
            { unsigned j; for (j = 0; sp2[j]; j++) m->file[j] = sp2[j]; m->file[j] = 0; }
            m->ok = (m->size > 0 && m->size <= HM_MAX_BUNDLE);
        }
    }
}

/* ───────────── JNI (минимальный набор, индексы — из jni.h) ───────────── */
typedef void* jobject;
typedef void* jmethodID;
typedef int (*AttachT)(void*, void**, void*);
typedef int (*DetachT)(void*);
typedef jobject (*FindClassT)(void*, const char*);                          /*   6 */
typedef void (*ExcClearT)(void*);                                           /*  17 */
typedef void (*DelRefT)(void*, jobject);                                    /*  23 */
typedef jobject (*NewObjectT)(void*, jobject, jmethodID, ...);              /*  28 */
typedef jmethodID (*GetMidT)(void*, jobject, const char*, const char*);     /*  33 */
typedef jobject (*CallObjT)(void*, jobject, jmethodID, ...);                /*  34 */
typedef int (*CallIntT)(void*, jobject, jmethodID, ...);                    /*  49 */
typedef void (*CallVoidT)(void*, jobject, jmethodID, ...);                  /*  61 */
typedef jobject (*NewUtfT)(void*, const char*);                             /* 167 */
typedef jobject (*NewBytesT)(void*, int);                                   /* 176 */
typedef void (*GetBytesRegT)(void*, jobject, int, int, signed char*);       /* 200 */
typedef unsigned char (*ExcCheckT)(void*);                                  /* 228; jboolean = 1 байт: старшие биты результата не определены, читать как int нельзя */
#define JF(env, idx, T) ((T)((*(void***)(env))[idx]))
#define J_EXC(env) ((int)(JF(env, 228, ExcCheckT)(env) & 0xff) != 0)
#define J_FAIL(env, why) do { if (J_EXC(env)) JF(env, 17, ExcClearT)(env); lg("http: ", why); goto fail; } while (0)

static void* g_vm;

/* GET по http(s) через java.net.HttpURLConnection. Возвращает malloc-буфер (или NULL) и длину. */
static u8* http_get(void* env, const char* url, unsigned long max, unsigned long* outn) {
    jobject cURL = NULL, cConn = NULL, cHttp = NULL, cIS = NULL, jurl = NULL, jstr = NULL, conn = NULL, in = NULL, arr = NULL;
    jmethodID mNew = NULL, mOpen = NULL, mCT = NULL, mRT = NULL, mCode = NULL, mGetIn = NULL, mRead = NULL, mClose = NULL, mDisc = NULL;
    u8* buf = NULL; unsigned long len = 0, cap = 0; int code;
    FindClassT FindClass = JF(env, 6, FindClassT); GetMidT GetMid = JF(env, 33, GetMidT);
    CallObjT CallObj = JF(env, 34, CallObjT); CallIntT CallInt = JF(env, 49, CallIntT); CallVoidT CallVoid = JF(env, 61, CallVoidT);
    *outn = 0;
    cURL = FindClass(env, "java/net/URL"); if (!cURL) J_FAIL(env, "нет класса java.net.URL");
    cConn = FindClass(env, "java/net/URLConnection"); if (!cConn) J_FAIL(env, "нет URLConnection");
    cHttp = FindClass(env, "java/net/HttpURLConnection"); if (!cHttp) J_FAIL(env, "нет HttpURLConnection");
    cIS = FindClass(env, "java/io/InputStream"); if (!cIS) J_FAIL(env, "нет InputStream");
    mNew = GetMid(env, cURL, "<init>", "(Ljava/lang/String;)V");
    mOpen = GetMid(env, cURL, "openConnection", "()Ljava/net/URLConnection;");
    mCT = GetMid(env, cConn, "setConnectTimeout", "(I)V");
    mRT = GetMid(env, cConn, "setReadTimeout", "(I)V");
    mGetIn = GetMid(env, cConn, "getInputStream", "()Ljava/io/InputStream;");
    mCode = GetMid(env, cHttp, "getResponseCode", "()I");
    mDisc = GetMid(env, cHttp, "disconnect", "()V");
    mRead = GetMid(env, cIS, "read", "([B)I");
    mClose = GetMid(env, cIS, "close", "()V");
    if (!mNew || !mOpen || !mCT || !mRT || !mGetIn || !mCode || !mDisc || !mRead || !mClose) J_FAIL(env, "не нашлись методы Java");
    jstr = JF(env, 167, NewUtfT)(env, url); if (!jstr) J_FAIL(env, "NewStringUTF");
    jurl = JF(env, 28, NewObjectT)(env, cURL, mNew, jstr); if (!jurl || J_EXC(env)) J_FAIL(env, "new URL() не удался");
    conn = CallObj(env, jurl, mOpen); if (!conn || J_EXC(env)) J_FAIL(env, "openConnection не удался");
    CallVoid(env, conn, mCT, HM_NET_TIMEOUT_MS); if (J_EXC(env)) J_FAIL(env, "setConnectTimeout");
    CallVoid(env, conn, mRT, HM_NET_TIMEOUT_MS); if (J_EXC(env)) J_FAIL(env, "setReadTimeout");
    code = CallInt(env, conn, mCode); if (J_EXC(env)) J_FAIL(env, "нет соединения (getResponseCode)");
    if (code != 200) { lgi("http: код ответа ", code); goto fail; }
    in = CallObj(env, conn, mGetIn); if (!in || J_EXC(env)) J_FAIL(env, "getInputStream не удался");
    arr = JF(env, 176, NewBytesT)(env, 65536); if (!arr) J_FAIL(env, "NewByteArray");
    for (;;) {
        int r = CallInt(env, in, mRead, arr);
        if (J_EXC(env)) J_FAIL(env, "ошибка чтения");
        if (r < 0) break;
        if (r == 0) continue;
        if (len + (unsigned long)r > max) { lg("http: ответ больше допустимого", ""); goto fail; }
        if (len + (unsigned long)r > cap) {
            unsigned long nc = cap ? cap * 2 : 131072; u8* nb;
            while (nc < len + (unsigned long)r) nc *= 2;
            nb = (u8*)realloc(buf, nc); if (!nb) { lg("http: нет памяти", ""); goto fail; } buf = nb; cap = nc;
        }
        JF(env, 200, GetBytesRegT)(env, arr, 0, r, (signed char*)(buf + len));
        len += (unsigned long)r;
    }
    CallVoid(env, in, mClose); CallVoid(env, conn, mDisc);
    if (J_EXC(env)) JF(env, 17, ExcClearT)(env);
    if (!buf) buf = (u8*)malloc(1);
    *outn = len;
    return buf;
fail:
    if (conn) { CallVoid(env, conn, mDisc); if (J_EXC(env)) JF(env, 17, ExcClearT)(env); }
    if (buf) free(buf);
    return NULL;
}

/* ───────────── основная работа ───────────── */
static char g_dir[300];
#ifdef HM_TEST
static const char* g_dirOverride;
EXPORT void hm_test_set_dir(const char* d) { g_dirOverride = d; }
#endif

static int setup_dir(void) {
    char d[300];
#ifdef HM_TEST
    if (g_dirOverride) { g_dir[0] = 0; sapp(g_dir, sizeof g_dir, g_dirOverride); mkdir(g_dir, 0700); return 1; }
#endif
    if (!g_pkg[0] && !pkg_name(g_pkg, sizeof g_pkg)) return 0;
    d[0] = 0; sapp(d, sizeof d, "/data/data/"); sapp(d, sizeof d, g_pkg); mkdir(d, 0700);
    sapp(d, sizeof d, "/files"); mkdir(d, 0700);
    sapp(d, sizeof d, "/hm"); mkdir(d, 0700);
    g_dir[0] = 0; sapp(g_dir, sizeof g_dir, d);
    return 1;
}
static void path_of(char* out, unsigned cap, const char* name) { out[0] = 0; sapp(out, cap, g_dir); sapp(out, cap, "/"); sapp(out, cap, name); }

static int off_switch(void) {
    char path[260]; int fd;
#ifdef HM_TEST
    if (g_dirOverride) return 0;
#endif
    fd = open("/data/local/tmp/konstt_off", 0);
    if (fd >= 0) { close(fd); return 1; }
    if (!g_pkg[0] && !pkg_name(g_pkg, sizeof g_pkg)) return 0;
    path[0] = 0; sapp(path, sizeof path, "/sdcard/Android/data/"); sapp(path, sizeof path, g_pkg); sapp(path, sizeof path, "/files/konstt_off");
    fd = open(path, 0);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

/* лежит ли в кеше бандл, подходящий под manifest m (размер + sha256) */
static int cache_matches(const Manifest* m) {
    char p[340], hex[65]; unsigned long n = 0; u8* b; int ok;
    path_of(p, sizeof p, "bundle.so");
    b = read_file(p, HM_MAX_BUNDLE, &n);
    if (!b) return 0;
    ok = (n == m->size);
    if (ok) { sha256_hex(b, n, hex); ok = seq(hex, m->sha); }
    free(b);
    return ok;
}
static int download_bundle(void* env, const Manifest* m) {
    char url[400], hex[65], p[340]; u8* b; unsigned long n = 0; int ok;
    url[0] = 0; sapp(url, sizeof url, HM_BASE_URL); sapp(url, sizeof url, m->file); sapp(url, sizeof url, "?t="); sapp_i(url, sizeof url, (long)time(NULL));
    lg("качаю бандл: ", m->file);
    b = http_get(env, url, HM_MAX_BUNDLE, &n);
    if (!b) return 0;
    if (n != m->size) { lgi("размер не совпал с manifest: получено ", (long)n); free(b); return 0; }
    sha256_hex(b, n, hex);
    if (!seq(hex, m->sha)) { lg("sha256 не совпал — бандл отброшен: ", hex); free(b); return 0; }
    path_of(p, sizeof p, "bundle.so");
    ok = write_atomic(p, b, n);
    free(b);
    if (!ok) lg("не удалось записать бандл в кеш", "");
    return ok;
}

/* ───────────── защита от «падающего» бандла ─────────────
   Перед каждой загрузкой бандла в strikes.txt пишется «<sha> <попыток> <время>». Если игра прожила HM_ALIVE_S секунд — счётчик
   сбрасывается. Если подряд HM_MAX_STRIKES запусков не дожили (вылет, либо вы закрыли игру сразу) — этот бандл на HM_QUARANTINE_S секунд
   не грузится (игра работает без мода). Новая сборка (другой sha) начинает с нуля. */
#ifndef HM_ALIVE_S
#define HM_ALIVE_S 25
#endif
#define HM_MAX_STRIKES 3
#define HM_QUARANTINE_S 3600
static char g_loadedSha[65];
static void strikes_get(const char* sha, int* n, long* t) {
    char p[340]; unsigned long len = 0; u8* b; unsigned long i; int k;
    *n = 0; *t = 0;
    path_of(p, sizeof p, "strikes.txt");
    b = read_file(p, 256, &len);
    if (!b) return;
    if (len > 66) {
        for (k = 0; k < 64 && b[k] == (u8)sha[k]; k++) {}
        if (k == 64 && b[64] == ' ') {
            i = 65; while (i < len && b[i] >= '0' && b[i] <= '9' && *n < 100000) *n = *n * 10 + (b[i++] - '0');
            if (i < len && b[i] == ' ') { i++; while (i < len && b[i] >= '0' && b[i] <= '9') *t = *t * 10 + (long)(b[i++] - '0'); }
        }
    }
    free(b);
}
static void strikes_set(const char* sha, int n, long t) {
    char p[340], txt[120]; int fd;
    txt[0] = 0; sapp(txt, sizeof txt, sha); sapp(txt, sizeof txt, " "); sapp_i(txt, sizeof txt, n); sapp(txt, sizeof txt, " "); sapp_i(txt, sizeof txt, t); sapp(txt, sizeof txt, "\n");
    path_of(p, sizeof p, "strikes.txt");
    unlink(p);
    fd = open(p, 0x241, 0600);
    if (fd >= 0) { write(fd, txt, slen(txt)); close(fd); }
}

typedef const char* (*idfn_t)(void);

/* одна попытка: проверить manifest, при необходимости скачать, загрузить. Возвращает 1, если бандл загружен */
static int try_use(void* env, const Manifest* m, int from_net) {
    char p[340]; void* h; idfn_t idf;
    if (!m->ok) return 0;
    if (m->loader_min > HM_LOADER_VER) { lgi("бандлу нужен загрузчик версии ", m->loader_min); lg("обновите APK (пересоберите .so загрузчика)", ""); return 0; }
    if (!cache_matches(m)) {
        if (!from_net || !env || !download_bundle(env, m)) return 0;
    } else lg("кеш актуален, качать не нужно", "");
    { int sn; long st; long now = (long)time(NULL);
      strikes_get(m->sha, &sn, &st);
      if (sn >= HM_MAX_STRIKES && now - st < HM_QUARANTINE_S) {
          lgi("бандл не дожил до конца прошлых запусков (подряд): ", sn);
          lg("пропускаю его на время карантина (до новой сборки или ~часа)", "");
          return 0;
      }
      strikes_set(m->sha, sn + 1, now); }
    path_of(p, sizeof p, "bundle.so");
    h = dlopen(p, RTLD_NOW_);
    if (!h) { const char* de = dlerror(); lg("dlopen бандла не удался: ", de ? de : "(нет)"); return 0; }
    { int k; for (k = 0; k < 65; k++) g_loadedSha[k] = m->sha[k]; }
    *(void**)&idf = dlsym(h, "hm_bundle_id");
    lg("бандл загружен, сборка: ", idf ? idf() : "?");
    return 1;
}

/* ───────────── переустановка мода по проверке файлов игры ─────────────
   Игра при «проверке файлов» (и при ручном удалении resources_version.txt) заново качает ресурсы. Раз файл пропал — сбрасываем и кеш
   мода: стираем bundle.so, manifest.txt, strikes.txt и качаем бандл с нуля (заодно снимается карантин).
   Чтобы не угадывать путь вслепую: запоминаем маркером rv_seen.txt, что файл когда-то был найден. Пока маркера нет (файл ни разу не видели
   по известным путям) — ничего не стираем, только пишем в loader.log, где искали. Стираем кеш только если удалось скачать manifest (иначе
   без сети остались бы совсем без мода) — тогда попробуем при следующем запуске. */
static int file_exists(const char* path) { int fd = open(path, 0); if (fd >= 0) { close(fd); return 1; } return 0; }
static int find_rv(char* out, unsigned cap) {
    static const char* const roots[] = { "/sdcard/Android/data/", "/storage/emulated/0/Android/data/", "/data/data/", 0 };
    int i;
    for (i = 0; roots[i]; i++) {
        out[0] = 0; sapp(out, cap, roots[i]); sapp(out, cap, g_pkg); sapp(out, cap, "/files/resources_version.txt");
        if (file_exists(out)) return 1;
    }
    out[0] = 0;
    return 0;
}
/* 1 — надо переустановить мод (файл был, теперь нет) */
static int rv_wants_reinstall(void) {
    char rv[300], mk[340];
    path_of(mk, sizeof mk, "rv_seen.txt");
    if (find_rv(rv, sizeof rv)) {
        if (!file_exists(mk)) {
            int fd = open(mk, 0x241, 0600);
            if (fd >= 0) { write(fd, "1\n", 2); close(fd); }
            lg("resources_version.txt найден, слежу за ним: ", rv);
        }
        return 0;
    }
    if (file_exists(mk)) { lg("resources_version.txt пропал (проверка файлов игры) — мод будет установлен заново", ""); return 1; }
    lg("resources_version.txt не найден по известным путям — слежение выключено", "");
    return 0;
}
static void wipe_cache(void) {
    char p[340];
    path_of(p, sizeof p, "bundle.so"); unlink(p);
    path_of(p, sizeof p, "manifest.txt"); unlink(p);
    path_of(p, sizeof p, "strikes.txt"); unlink(p);
    path_of(p, sizeof p, "rv_seen.txt"); unlink(p);
    lg("кеш мода стёрт (bundle.so, manifest.txt, strikes.txt)", "");
}

static void run_loader(void* env) {
    char url[400], p[340]; u8* mf = NULL; unsigned long mn = 0; Manifest net, cache; int have_cache = 0, done = 0, force = 0;
    net.ok = 0; cache.ok = 0;
    if (!setup_dir()) { lg("нет папки для кеша (не определён пакет)", ""); return; }
    force = rv_wants_reinstall();
    /* 1) manifest из сети */
    net.ok = 0;
    if (env) {
        url[0] = 0; sapp(url, sizeof url, HM_BASE_URL); sapp(url, sizeof url, "manifest.txt?t="); sapp_i(url, sizeof url, (long)time(NULL));
        lg("качаю manifest: ", HM_BASE_URL);
        mf = http_get(env, url, HM_MAX_MANIFEST, &mn);
        if (mf) { parse_manifest(mf, mn, &net); if (!net.ok) lg("manifest скачан, но в нём нет записи для ", HM_ABI_KEY); free(mf); }
        else lg("manifest не скачался (нет сети?), попробую кеш", "");
    }
    if (force) {
        if (net.ok) wipe_cache();
        else lg("переустановка отложена: manifest не скачался, кеш оставляю до следующего запуска", "");
    }
    /* 2) manifest из кеша */
    path_of(p, sizeof p, "manifest.txt");
    mf = read_file(p, HM_MAX_MANIFEST, &mn);
    if (mf) { parse_manifest(mf, mn, &cache); have_cache = cache.ok; free(mf); }
    /* 3) сеть, потом кеш */
    if (net.ok) {
        lg("сборка на сервере: ", net.build);
        if (try_use(env, &net, 1)) {
            done = 1;
            if (!have_cache || !seq(cache.sha, net.sha)) {         /* запомнить manifest, с которым бандл проверен */
                char mp[340]; char txt[600]; txt[0] = 0;
                sapp(txt, sizeof txt, "loader_min "); sapp_i(txt, sizeof txt, net.loader_min); sapp(txt, sizeof txt, "\nbuild "); sapp(txt, sizeof txt, net.build);
                sapp(txt, sizeof txt, "\n"); sapp(txt, sizeof txt, HM_ABI_KEY); sapp(txt, sizeof txt, " "); sapp(txt, sizeof txt, net.sha); sapp(txt, sizeof txt, " ");
                sapp_i(txt, sizeof txt, (long)net.size); sapp(txt, sizeof txt, " "); sapp(txt, sizeof txt, net.file); sapp(txt, sizeof txt, "\n");
                path_of(mp, sizeof mp, "manifest.txt");
                unlink(mp);
                { int fd = open(mp, 0x241, 0600); if (fd >= 0) { write(fd, txt, slen(txt)); close(fd); } }
            }
        }
    }
    if (!done && have_cache) { lg("использую бандл из кеша, сборка: ", cache.build); done = try_use(env, &cache, 0); }
    if (!done) lg("бандл не загружен — игра работает без мода", "");
}

static void* loader_thread(void* arg) {
    void* env = NULL; int attached = 0; (void)arg;
    if (g_vm) {
        void** tbl = *(void***)g_vm;
        if (((AttachT)tbl[4])(g_vm, &env, NULL) == 0) attached = 1; else env = NULL;
    }
    if (!attached) lg("не удалось присоединить поток к JVM — только кеш", "");
    run_loader(env);
    if (attached) ((DetachT)(*(void***)g_vm)[5])(g_vm);
    if (g_loadedSha[0]) {                                   /* дожили до HM_ALIVE_S секунд — бандл считаем рабочим */
        int w; for (w = 0; w < HM_ALIVE_S; w++) usleep(1000000);
        strikes_set(g_loadedSha, 0, 0);
        lg("игра прожила достаточно: бандл признан рабочим", "");
    }
    return NULL;
}

#ifdef HM_TEST
EXPORT void hm_test_run(void* vm) { g_vm = vm; loader_thread(NULL); }
EXPORT void hm_test_sha(const u8* p, unsigned long n, char* out) { sha256_hex(p, n, out); }
EXPORT int hm_test_manifest(const char* txt, unsigned long n, unsigned long* size, char* sha, char* file, int* lmin) {
    Manifest m; parse_manifest((const u8*)txt, n, &m);
    if (m.ok) { *size = m.size; *lmin = m.loader_min; { int i; for (i = 0; i < 65; i++) sha[i] = m.sha[i]; for (i = 0; i < 100; i++) file[i] = m.file[i]; } }
    return m.ok;
}
#else
/* ───────────── вход: подмена libmain.so / libHassleExtended.so ───────────── */
typedef int (*jni_onload_t)(void*, void*);
typedef void (*jni_onunload_t)(void*, void*);
static void* g_orig;

EXPORT int JNI_OnLoad(void* vm, void* reserved) {
    int ver = 0x00010006;                                  /* JNI_VERSION_1_6 */
    g_vm = vm;
    lg("JNI_OnLoad: вход, версия загрузчика ", "1");
    g_orig = dlopen("libHassleExtend2.so", RTLD_NOW_);     /* старый клиент */
    if (g_orig) lg("режим старого клиента (libHassleExtend2.so)", "");
    else {
        g_orig = dlopen("libHassleMain2.so", RTLD_NOW_);   /* чистый APK: оригинальный libmain.so */
        if (g_orig) lg("режим чистого APK (libHassleMain2.so = оригинальный libmain.so)", "");
    }
    if (g_orig) {
        jni_onload_t f = (jni_onload_t)dlsym(g_orig, "JNI_OnLoad");
        if (f) ver = f(vm, reserved);
        lgi("оригинал вернул версию ", ver);
    } else {
        const char* de = dlerror();
        lg("оригинал НЕ загрузился: ", de ? de : "(нет)");
    }
    if (off_switch()) lg("konstt_off найден: мод отключён, работает только оригинал", "");
    else {
        unsigned long t = 0;
        if (pthread_create(&t, NULL, loader_thread, NULL) != 0) lg("не удалось создать поток загрузчика", "");
    }
    return ver;
}
EXPORT void JNI_OnUnload(void* vm, void* reserved) {
    if (g_orig) {
        jni_onunload_t f = (jni_onunload_t)dlsym(g_orig, "JNI_OnUnload");
        if (f) f(vm, reserved);
    }
}
#endif
