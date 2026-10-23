/* Макет libil2cpp.so: ровно столько IL2CPP, сколько нужно ядру и модам, чтобы пройти запуск, найти реестр чат-команд,
   зарегистрировать команды и выполнить /rec. Не игра — только проверка логики потоков и регистрации. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef unsigned short u16;
typedef struct FType FType; typedef struct FClass FClass; typedef struct FField FField; typedef struct FMethod FMethod;
struct FType { int code; const char* name; FClass* cls; };
struct FField { const char* name; int offset; int flags; FType* type; void* sval; };
struct FMethod { const char* name; int argc; void* (*impl)(void* self, void** args); };
struct FClass { const char* ns; const char* name; FClass* parent; FField* fields; int nf; FMethod* methods; int nm; int isvalue; int isize; FClass* elem; int esz; };
typedef struct { FClass* k; void* mon; } Obj;
typedef struct { FClass* k; void* mon; int len; u16 ch[400]; } FStr;

static FType T_ref = { 0x12, "Object", 0 }, T_int = { 8, "Int32", 0 }, T_u16 = { 7, "UInt16", 0 }, T_ptr = { 0x19, "UIntPtr", 0 };

/* ── классы ── */
static FClass C_String = { "System", "String", 0, 0, 0, 0, 0, 0, 32, 0, 0 };
static FField F_deleg[] = { { "m_target", 16, 0, &T_ref, 0 }, { "delegates", 24, 0, &T_ref, 0 }, { "method_ptr", 32, 0, &T_ptr, 0 }, { "invoke_impl", 40, 0, &T_ptr, 0 } };
static FClass C_Deleg = { "Hassle", "ChatCommandEvent", 0, F_deleg, 4, 0, 0, 0, 0x60, 0, 0 };
static FField F_entry[] = { { "hashCode", 0, 0, &T_int, 0 }, { "next", 4, 0, &T_int, 0 }, { "key", 8, 0, &T_ref, 0 }, { "value", 16, 0, &T_ref, 0 } };
static FClass C_Entry = { "System.Collections.Generic", "Entry", 0, F_entry, 4, 0, 0, 1, 24, 0, 0 };
static FClass C_EntryArr = { "", "Entry[]", 0, 0, 0, 0, 0, 0, 32, &C_Entry, 24 };
static FField F_dict[] = { { "_entries", 16, 0, &T_ref, 0 }, { "_count", 24, 0, &T_int, 0 } };
static void* m_set_item(void* self, void** args);
static FMethod M_dict[] = { { "set_Item", 2, m_set_item } };
static FClass C_Dict = { "System.Collections.Generic", "Dictionary`2", 0, F_dict, 2, M_dict, 1, 0, 40, 0, 0 };
static FField F_net[] = { { "State", 16, 0, &T_int, 0 }, { "m_Port", 20, 0, &T_u16, 0 }, { "m_Host", 24, 0, &T_ref, 0 }, { "m_Password", 32, 0, &T_ref, 0 }, { "m_NickName", 40, 0, &T_ref, 0 } };
static void* m_quit(void* self, void** args);
static void* m_delayed(void* self, void** args);
static FMethod M_net[] = { { "OnApplicationQuit", 0, m_quit }, { "DelayedConnect", 5, m_delayed } };
static FClass C_Net = { "Game", "NetworkClient", 0, F_net, 5, M_net, 2, 0, 56, 0, 0 };
static FField F_game[] = { { "Instance", 0, 0x10, &T_ref, 0 }, { "Network", 0, 0x10, &T_ref, 0 }, { "MainCamera", 0, 0x10, &T_ref, 0 } };
static FClass C_Game = { "Hassle", "Game", 0, F_game, 3, 0, 0, 0, 32, 0, 0 };
static FField F_wv[] = { { "m_ChatCommandEvents", 0, 0x10, &T_ref, 0 } };
static void* m_chat(void* self, void** args);
static FMethod M_wv[] = { { "AddChatMessage", 2, m_chat } };
static FClass C_WV = { "Hassle.WebView", "WebViewSystem", 0, F_wv, 1, M_wv, 1, 0, 32, 0, 0 };
static FField F_pp[] = { { "LocalPlayer", 16, 0, &T_ref, 0 }, { "PlayersInStream", 24, 0, &T_ref, 0 } };
static FClass C_PP = { "Hassle.Global.Pooling", "PlayerPool", 0, F_pp, 2, 0, 0, 0, 40, 0, 0 };
static FField F_pools[] = { { "Players", 0, 0x10, &T_ref, 0 } };
static FClass C_Pools = { "Hassle.Global.Pooling", "Pools", 0, F_pools, 1, 0, 0, 0, 32, 0, 0 };
static FField F_list[] = { { "_items", 16, 0, &T_ref, 0 }, { "_size", 24, 0, &T_int, 0 } };
static FClass C_List = { "System.Collections.Generic", "List`1", 0, F_list, 2, 0, 0, 0, 40, 0, 0 };
static FClass C_ObjArr = { "", "Object[]", 0, 0, 0, 0, 0, 0, 32, 0, 8 };
static FClass C_StrArr = { "", "String[]", 0, 0, 0, 0, 0, 0, 32, &C_String, 8 };
static FClass* ALL[] = { &C_Deleg, &C_Dict, &C_Net, &C_Game, &C_WV, &C_PP, &C_Pools, &C_List, &C_String };

/* ── состояние мира ── */
static Obj *g_dict, *g_net, *g_pool, *g_game_inst;
static char g_chat[64][400]; static int g_nchat;
static char g_netlog[1024];

static void* anew(FClass* k, int bytes) { Obj* o = calloc(1, bytes); o->k = k; return o; }
static void* mkstr(const char* s) {
    FStr* x = calloc(1, sizeof(FStr)); const unsigned char* p = (const unsigned char*)s; int n = 0;
    x->k = &C_String;
    while (*p && n < 399) {
        unsigned c = *p++;
        if (c >= 0xE0) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xC0) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
        x->ch[n++] = (u16)c;
    }
    x->len = n; return x;
}
static void tostr8(FStr* s, char* out, int cap) {
    int i, o = 0;
    for (i = 0; i < s->len && o + 4 < cap; i++) {
        unsigned c = s->ch[i];
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
}
static void* mkarr(FClass* k, int len, int esz) {
    char* a = calloc(1, 32 + (size_t)len * esz + 8);
    *(FClass**)a = k; *(long*)(a + 24) = len; return a;
}
static void dict_add(const char* key, void* val) {
    Obj* d = g_dict; char* arr = *(char**)((char*)d + 16); int* cnt = (int*)((char*)d + 24); char* e;
    e = arr + 32 + (long)(*cnt) * 24;
    *(int*)e = 1; *(int*)(e + 4) = -1; *(void**)(e + 8) = mkstr(key); *(void**)(e + 16) = val;
    (*cnt)++;
}
static int streq(FStr* a, const char* b) { char t[400]; tostr8(a, t, sizeof t); return strcmp(t, b) == 0; }

static void* m_set_item(void* self, void** args) {
    char* arr = *(char**)((char*)self + 16); int* cnt = (int*)((char*)self + 24); int i;
    FStr* key = args[0];
    for (i = 0; i < *cnt; i++) {
        char* e = arr + 32 + (long)i * 24; FStr* k = *(FStr**)(e + 8);
        char t[400]; tostr8(key, t, sizeof t);
        if (k && streq(k, t)) { *(void**)(e + 16) = args[1]; return 0; }
    }
    { char t[400]; char* e = arr + 32 + (long)(*cnt) * 24; tostr8(key, t, sizeof t); (void)t;
      *(int*)e = 1; *(int*)(e + 4) = -1; *(void**)(e + 8) = key; *(void**)(e + 16) = args[1]; (*cnt)++; }
    return 0;
}
static void* m_quit(void* self, void** args) { (void)self; (void)args; strcat(g_netlog, "quit;"); return 0; }
static void* m_delayed(void* self, void** args) {
    char h[100], n[100], b[300]; (void)self;
    tostr8(args[0], h, sizeof h); tostr8(args[3], n, sizeof n);
    snprintf(b, sizeof b, "connect(%s,%u,%s,%.2f);", h, (unsigned)*(u16*)args[1], n, (double)*(float*)args[4]);
    strcat(g_netlog, b); return 0;
}
static void* m_chat(void* self, void** args) {
    (void)self;
    if (g_nchat < 64) { tostr8(args[0], g_chat[g_nchat], 400); g_nchat++; }
    return 0;
}

/* ── управление из тестового драйвера ── */
void fake_init(void) {
    int i;
    for (i = 0; i < 9; i++) if (ALL[i]->fields == 0 && 0) {}
    g_dict = anew(&C_Dict, 40); *(void**)((char*)g_dict + 16) = mkarr(&C_EntryArr, 64, 24); *(int*)((char*)g_dict + 24) = 0;
    C_WV.fields[0].sval = g_dict;
    g_net = anew(&C_Net, 56);
    *(int*)((char*)g_net + 16) = 2; *(u16*)((char*)g_net + 20) = 7777;
    *(void**)((char*)g_net + 24) = mkstr("80.66.71.77"); *(void**)((char*)g_net + 32) = mkstr("pw"); *(void**)((char*)g_net + 40) = mkstr("Nick_Name");
    C_Game.fields[1].sval = g_net;
    {   Obj* list = anew(&C_List, 40);
        *(void**)((char*)list + 16) = mkarr(&C_ObjArr, 0, 8); *(int*)((char*)list + 24) = 0;
        g_pool = anew(&C_PP, 40); *(void**)((char*)g_pool + 24) = list;
        C_Pools.fields[0].sval = g_pool; }
}
void fake_set_ready(void) { g_game_inst = anew(&C_Game, 32); C_Game.fields[0].sval = g_game_inst; }
static void game_cmd_stub(void* self, void* args, void* mi) { (void)self; (void)args; (void)mi; }
void fake_add_game_cmd(const char* key) {
    Obj* d = anew(&C_Deleg, 0x60);
    *(void**)((char*)d + 16) = anew(&C_Game, 32);                     /* m_target */
    *(void**)((char*)d + 32) = (void*)game_cmd_stub; *(void**)((char*)d + 40) = (void*)game_cmd_stub;
    dict_add(key, d);
}
int fake_has_key(const char* key) {
    char* arr = *(char**)((char*)g_dict + 16); int cnt = *(int*)((char*)g_dict + 24), i;
    for (i = 0; i < cnt; i++) { FStr* k = *(FStr**)(arr + 32 + (long)i * 24 + 8); if (k && streq(k, key)) return 1; }
    return 0;
}
int fake_call_cmd(const char* key, const char* a1, const char* a2) {            /* как игра: method_ptr(target, string[] args, NULL) */
    char* arr = *(char**)((char*)g_dict + 16); int cnt = *(int*)((char*)g_dict + 24), i;
    for (i = 0; i < cnt; i++) {
        char* e = arr + 32 + (long)i * 24; FStr* k = *(FStr**)(e + 8);
        if (k && streq(k, key)) {
            char* d = *(char**)(e + 16); void (*mp)(void*, void*, void*) = *(void (**)(void*, void*, void*))(d + 32);
            char* args = mkarr(&C_StrArr, 3, 8); int n = 0;
            *(void**)(args + 32 + 8 * n++) = mkstr(key);
            if (a1) *(void**)(args + 32 + 8 * n++) = mkstr(a1);
            if (a2) *(void**)(args + 32 + 8 * n++) = mkstr(a2);
            *(long*)(args + 24) = n;
            mp(*(void**)(d + 16), args, 0); return 1;
        }
    }
    return 0;
}
int fake_chat_count(void) { return g_nchat; }
const char* fake_chat_get(int i) { return g_chat[i]; }
const char* fake_netlog(void) { return g_netlog; }
void fake_clear(void) { g_nchat = 0; g_netlog[0] = 0; }

/* ── IL2CPP API ── */
static char dom_dummy, asm_dummy, img_dummy, img_mscorlib;
static void* ASM[2];
void* il2cpp_domain_get(void) { return &dom_dummy; }
void* il2cpp_thread_attach(void* d) { return d; }
void** il2cpp_domain_get_assemblies(void* d, unsigned long* n) { (void)d; ASM[0] = &asm_dummy; *n = 1; return ASM; }
void* il2cpp_assembly_get_image(void* a) { (void)a; return &img_dummy; }
const char* il2cpp_image_get_name(void* i) { return i == &img_dummy ? "Assembly-CSharp.dll" : "mscorlib.dll"; }
unsigned long il2cpp_image_get_class_count(void* i) { (void)i; return 0; }
void* il2cpp_image_get_class(void* i, unsigned long k) { (void)i; (void)k; return 0; }
void* il2cpp_class_from_name(void* img, const char* ns, const char* name) {
    int i; (void)img;
    for (i = 0; i < 9; i++) if (!strcmp(ALL[i]->ns, ns) && !strcmp(ALL[i]->name, name)) return ALL[i];
    return 0;
}
const char* il2cpp_class_get_name(void* k) { return ((FClass*)k)->name; }
const char* il2cpp_class_get_namespace(void* k) { return ((FClass*)k)->ns; }
void* il2cpp_class_get_parent(void* k) { return ((FClass*)k)->parent; }
void* il2cpp_class_get_fields(void* k, void** it) {
    FClass* c = k; long i = (long)*it;
    if (i >= c->nf) return 0;
    *it = (void*)(i + 1); return &c->fields[i];
}
void* il2cpp_class_get_field_from_name(void* k, const char* n) {
    FClass* c = k; int i;
    for (i = 0; i < c->nf; i++) if (!strcmp(c->fields[i].name, n)) return &c->fields[i];
    return 0;
}
const char* il2cpp_field_get_name(void* f) { return ((FField*)f)->name; }
int il2cpp_field_get_flags(void* f) { return ((FField*)f)->flags; }
void* il2cpp_field_get_type(void* f) { return ((FField*)f)->type; }
int il2cpp_field_get_offset(void* f) { return ((FField*)f)->offset; }
void il2cpp_field_static_get_value(void* f, void* out) { *(void**)out = ((FField*)f)->sval; }
void* il2cpp_object_get_class(void* o) { return *(void**)o; }
int il2cpp_type_get_type(void* t) { return ((FType*)t)->code; }
void* il2cpp_class_from_type(void* t) { return ((FType*)t)->cls ? ((FType*)t)->cls : &C_ObjArr; }
int il2cpp_class_is_valuetype(void* k) { return ((FClass*)k)->isvalue; }
void* il2cpp_class_get_method_from_name(void* k, const char* n, int argc) {
    FClass* c = k; int i;
    for (i = 0; i < c->nm; i++) if (!strcmp(c->methods[i].name, n) && c->methods[i].argc == argc) return &c->methods[i];
    return 0;
}
void* il2cpp_runtime_invoke(void* m, void* self, void** args, void** exc) { (void)exc; return ((FMethod*)m)->impl(self, args); }
u16* il2cpp_string_chars(void* s) { return ((FStr*)s)->ch; }
int il2cpp_string_length(void* s) { return ((FStr*)s)->len; }
int il2cpp_class_array_element_size(void* k) { return ((FClass*)k)->esz; }
void* il2cpp_class_get_element_class(void* k) { return ((FClass*)k)->elem; }
const char* il2cpp_type_get_name(void* t) { return ((FType*)t)->name; }
void* il2cpp_string_new(const char* s) { return mkstr(s); }
void* il2cpp_object_new(void* k) { return anew(k, ((FClass*)k)->isize); }
int il2cpp_class_instance_size(void* k) { return ((FClass*)k)->isize; }
