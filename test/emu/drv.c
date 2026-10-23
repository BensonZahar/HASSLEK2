#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
static void* H;
#define F(t,n,a) static t (*n)a
int main(void) {
    void* il = dlopen("./libil2cpp.so", RTLD_NOW | RTLD_GLOBAL);
    if (!il) { puts(dlerror()); return 1; }
    void (*fake_init)(void) = dlsym(il, "fake_init");
    void (*fake_set_ready)(void) = dlsym(il, "fake_set_ready");
    void (*fake_add)(const char*) = dlsym(il, "fake_add_game_cmd");
    int (*has)(const char*) = dlsym(il, "fake_has_key");
    int (*call)(const char*, const char*, const char*) = dlsym(il, "fake_call_cmd");
    int (*nchat)(void) = dlsym(il, "fake_chat_count");
    const char* (*chat)(int) = dlsym(il, "fake_chat_get");
    const char* (*netlog)(void) = dlsym(il, "fake_netlog");
    void (*clr)(void) = dlsym(il, "fake_clear");
    fake_init(); fake_add("help"); fake_set_ready();
    H = dlopen("./bundle_test.so", RTLD_NOW);
    if (!H) { puts(dlerror()); return 1; }
    const char* (*diag)(void) = dlsym(H, "hm_test_diag");
    const char* cmds[] = {"rec", "nearlog", "nocol", "route"};
    for (int w = 0; w < 8; w++) {
        int all = 1; for (int i = 0; i < 4; i++) if (!has(cmds[i])) all = 0;
        if (all) break; sleep(1);
    }
    for (int i = 0; i < 4; i++) printf("registered %-8s %d\n", cmds[i], has(cmds[i]));
    clr();
    printf("call rec 5 -> %d\n", call("rec", "5", NULL));
    sleep(1);
    printf("netlog: %s\n", netlog());
    for (int i = 0; i < nchat(); i++) printf("chat[%d]: %s\n", i, chat(i));
    call("nearlog", "2", NULL); sleep(3);
    clr();
    call("rec", "diag", NULL); call("nearlog", "diag", NULL);
    for (int i = 0; i < nchat(); i++) printf("chat[%d]: %s\n", i, chat(i));
    puts("---- diag log ----"); puts(diag());
    return 0;
}
