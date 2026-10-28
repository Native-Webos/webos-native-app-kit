/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A host stand-in for LG's libNDL_directmedia.so.1 (v2 entry points NdlBackend looks up). Behaviour
 * from the environment, read per call: WNK_FAKE_NDL_UNLOAD_MS=N makes NDL_DirectMediaUnload return
 * after N ms. wnk_fake_ndl_counts() reports Init, Load, Unload (returned) and Quit calls. */
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef void (*load_cb)(int type, long long num, const char *str);
static atomic_int g_init, g_load, g_unload, g_quit;  /* read by the test while the worker runs */
static load_cb g_cb;

static long env_long(const char *name) {
    const char *v = getenv(name);
    return v != NULL ? atol(v) : 0;
}

void wnk_fake_ndl_counts(int *init, int *load, int *unload, int *quit) {
    *init = atomic_load(&g_init);
    *load = atomic_load(&g_load);
    *unload = atomic_load(&g_unload);
    *quit = atomic_load(&g_quit);
}

int NDL_DirectMediaInit(const char *app_id, void *released) {
    (void)app_id;
    (void)released;
    ++g_init;
    return 0;
}
int NDL_DirectMediaQuit(void) {
    ++g_quit;
    return 0;
}
int NDL_DirectMediaSetAppState(int state) {
    (void)state;
    return 0;
}
int NDL_DirectMediaLoad(void *info, load_cb cb) {
    (void)info;
    ++g_load;
    g_cb = cb;
    if (cb != NULL) cb(0x16, 0, "true");
    return 0;
}
int NDL_DirectMediaUnload(void) {
    const long ms = env_long("WNK_FAKE_NDL_UNLOAD_MS");
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    ++g_unload;
    return 0;
}
int NDL_DirectVideoPlay(void *data, unsigned int size, long long pts) {
    (void)data;
    (void)size;
    (void)pts;
    return 0;
}
int NDL_DirectAudioPlay(void *data, unsigned int size, long long pts) {
    (void)data;
    (void)size;
    (void)pts;
    return 0;
}
int NDL_DirectVideoSetArea(int x, int y, int w, int h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    return 0;
}
const char *NDL_DirectMediaGetError(void) { return ""; }
