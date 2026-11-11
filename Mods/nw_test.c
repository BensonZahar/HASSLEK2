#define KONSTT_BUNDLE 1
#define KONSTT_TEST 1
#include "hassle_core.c"
#include "hassle_near.c"
#include <stdio.h>
#include <string.h>

static int T;
static void setp(int i, int id, const char* nick, float x, float y, float z) {
    PInfo* p = &g_pl[i]; memset(p, 0, sizeof *p);
    p->id = id; p->hasPos = 1; p->x = x; p->y = y; p->z = z; p->hp = 100; strcpy(p->nick, nick); strcpy(p->label, nick);
}
static void pool_set(int i, int id, const char* nick, float x, float y, float z) {
    NwP* q = &g_nwP[i]; memset(q, 0, sizeof *q);
    q->id = id; q->hasPos = 1; q->x = x; q->y = y; q->z = z; q->hp = 100; strcpy(q->nick, nick);
}
static float me[3] = {0, 0, 0};
static void tick(void) { g_ticks = 1 + 5 * (++T); nw_tick(me, 1, 7, NULL); }
static void dump_chat(const char* title) {
    printf("--- %s\n", title);
    while (g_nwqT != g_nwqH) { printf("  CHAT: %s\n", g_nwq[g_nwqT % NW_Q]); g_nwqT++; }
}
int main(void) {
    int i;
    /* A: слежка по нику */
    nw_lock(); nw_add_locked("ivan_petrov"); nw_unlock();
    g_np = 0; g_nwNP = 0; g_nwTestTotal = 0; tick(); dump_chat("A1 не найден");
    g_nwTestTotal = 1; pool_set(0, 12, "Ivan_Petrov", 100, 0, 0); g_nwNP = 1; tick(); dump_chat("A2 онлайн вне зоны (пул)");
    g_np = 1; setp(0, 12, "Ivan_Petrov", 30, 0, 40); tick(); dump_chat("A3 в зоне, 50 м");
    g_np = 0; for (i = 0; i < 5; i++) tick(); g_np = 1; tick(); dump_chat("A4 мерцание 5 тиков — тихо");
    g_np = 0; pool_set(0, 12, "Ivan_Petrov", 100, 0, 0); for (i = 0; i < 9; i++) tick(); dump_chat("A5 ушёл из зоны, онлайн");
    g_nwTestTotal = 0; g_nwNP = 0; for (i = 0; i < 5; i++) tick(); dump_chat("A6 вышел из игры");
    /* B: слежка по ID и смена ID после перезахода */
    nw_lock(); for (i = 0; i < NW_MAX; i++) g_nw[i].used = 0; nw_add_locked("12"); nw_unlock();
    for (i = 0; i < NW_MAX; i++) g_nw[i].st = 0;
    g_np = 1; setp(0, 12, "Ivan_Petrov", 3, 4, 0); g_nwTestTotal = 1; pool_set(0, 12, "Ivan_Petrov", 3, 4, 0); g_nwNP = 1; g_nwP[0].inStream = 1; tick(); dump_chat("B1 по ID 12 в зоне 5 м");
    g_np = 1; setp(0, 40, "ivan_PETROV", 6, 8, 0); pool_set(0, 40, "ivan_PETROV", 6, 8, 0); tick(); dump_chat("B2 тот же ник, ID 40 → перепривязка");
    /* C: чужой ник на старом ID */
    g_np = 1; setp(0, 40, "Other_Guy", 6, 8, 0); g_nwNP = 1; pool_set(0, 40, "Other_Guy", 6, 8, 0); for (i = 0; i < 9; i++) tick(); dump_chat("C ID 40 теперь у другого");
    printf("idTaken=%d st=%d\n", g_nw[0].idTaken, g_nw[0].st);
    /* D: any */
    nw_lock(); for (i = 0; i < NW_MAX; i++) g_nw[i].used = 0; nw_unlock();
    g_nwAny = 1; g_np = 2; setp(0, 1, "Aaa_Bbb", 10, 0, 0); setp(1, 2, "Ccc_Ddd", 20, 0, 0); g_nwNP = 0; tick(); dump_chat("D1 any: засев, тихо");
    g_np = 8; for (i = 2; i < 8; i++) { char n[20]; sprintf(n, "Px_%d", i); setp(i, 10 + i, n, 5.0f * i, 0, 0); } tick(); dump_chat("D2 any: 6 новых сразу — в чат не больше 4");
    g_np = 0; for (i = 0; i < 9; i++) tick(); dump_chat("D3 any: все ушли");
    /* E: JSON */
    { static char out[16384]; Buf b; b_init(&b, out, sizeof out); b_s(&b, "{"); b_s(&b, g_nwTj[g_nwTjCur]); nw_events_json(&b, 0); b_s(&b, "}"); puts("--- JSON"); puts(out); }
    { static char out[16384]; Buf b; b_init(&b, out, sizeof out); b_s(&b, "{"); nw_lock(); nw_add_locked("Ivan\"x\\y"); nw_unlock(); tick(); b_s(&b, g_nwTj[g_nwTjCur]); nw_events_json(&b, g_nwSeq - 2); b_s(&b, "}"); puts("--- JSON2 (since)"); puts(out); }
    return 0;
}
