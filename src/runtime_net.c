/* Online play (BB_ONLINE=1): the game's network and PSN imports go to the online module
 * (gpu/bbnet: libbbnet.dylib beside libbbgpu.dylib; libbbnet.so on Linux), loaded here on the
 * first such import. Offline,
 * or when the module is missing or fails to start, they stay with the runtime's offline
 * versions (runtime_services.c). Trophies, voice and the store/profile dialogs always stay here. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbnet/bbnet.h"
#include "gpu/bbseamless/client.h"
#include "gpu/bbgpu.h"
#include <sys/mman.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#define BBNET_MODULE_FILE "libbbnet.dylib"
#else
#define BBNET_MODULE_FILE "libbbnet.so"
#endif

static char game_folder[PATH_MAX], game_serial[16], game_title[128], game_version[16],game_user[PATH_MAX];

void runtime_net_configure(const char *app0, const char *serial, const char *title, const char *version, const char *user) {
    snprintf(game_user,sizeof(game_user),"%s",user?user:"");
    snprintf(game_folder,sizeof(game_folder),"%s",app0 ? app0 : "");
    snprintf(game_serial,sizeof(game_serial),"%s",serial ? serial : "");
    snprintf(game_title,sizeof(game_title),"%s",title ? title : "");
    snprintf(game_version,sizeof(game_version),"%s",version ? version : "");
}

int runtime_net_enabled(void) {
    static int enabled=-1;
    if (enabled<0) { const char *v=getenv("BB_ONLINE"); enabled=v && v[0]=='1'; }
    return enabled;
}

/* Imports the module answers: network, HTTP/SSL and NP, except what stays in the runtime. */
int runtime_net_symbol(const char *symbol) {
    static const char *const online[]={"sceNet","sceNp","sceHttp","sceSsl"};
    static const char *const local[]={"sceNpTrophy","sceNpCommerce","sceNpProfileDialog"};
    if (!symbol) return 0;
    int match=0;
    for (size_t i=0;i<sizeof(online)/sizeof(*online);++i)
        if (!strncmp(symbol,online[i],strlen(online[i]))) match=1;
    for (size_t i=0;i<sizeof(local)/sizeof(*local);++i)
        if (!strncmp(symbol,local[i],strlen(local[i]))) match=0;
    return match;
}

static BbNetSetImageFn module_set_image;
static BbNetStartFn module_start;
static void *online_module;
int runtime_coop_enabled(void) {
    const char *backend=getenv("BB_NET_BACKEND");
    return runtime_net_enabled() && backend && !strcmp(backend,"seamless");
}
static void coop_unavailable(const char *reason) {
    fprintf(stderr,"Co-op: %s\n",reason);
    exit(1);
}
static int32_t party_trophy(int32_t id) {
    return runtime_coop_record_trophy(id);
}

/* The module: BB_NET_MODULE, else gpu/libbbnet.* beside bb-probe (where the GPU library is). */
static BbNetResolveFn module_resolve(void) {
    static int tried;
    static BbNetResolveFn resolve;
    if (tried) return resolve;
    tried=1;
    char path[PATH_MAX];
    const char *module_file=runtime_coop_enabled()?"libbbseamless.dylib":BBNET_MODULE_FILE;
    const char *forced=getenv("BB_NET_MODULE");
    if (forced && forced[0]) {
        snprintf(path,sizeof(path),"%s",forced);
    } else {
        char exe[PATH_MAX-32];
#ifdef __APPLE__
        uint32_t exe_size=sizeof(exe);
        if (_NSGetExecutablePath(exe,&exe_size)!=0) return NULL;
#else
        const ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
        if (n<=0) return NULL;
        exe[n]=0;
#endif
        char *slash=strrchr(exe,'/');
        if (slash) *slash=0;
        snprintf(path,sizeof(path),"%s/gpu/%s",exe,module_file);
#ifdef __APPLE__
        /* Release apps keep libraries in Contents/Frameworks, next to MacOS/bb-probe. */
        if (access(path,R_OK)!=0)
            snprintf(path,sizeof(path),"%s/../Frameworks/%s",exe,module_file);
#endif
    }
    /* RTLD_LOCAL: its C++ (network and NP libraries) stays out of the process's symbol scope. */
    void *module=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!module) {
        if (runtime_coop_enabled()) coop_unavailable(dlerror());
        printf("Online: the online module is not available (%s): offline play\n",dlerror());
        return NULL;
    }
    const BbNetInitFn init=(BbNetInitFn)dlsym(module,"bbnet_init");
    const BbNetResolveFn found=(BbNetResolveFn)dlsym(module,"bbnet_resolve");
    const BbNetHost host={
        .abi=BBNET_ABI_VERSION, .app0=game_folder, .serial=game_serial, .title=game_title,
        .app_ver=game_version, .thread_spawn=runtime_thread_spawn,
        .thread_join=runtime_thread_join_spawned, .thread_exit=runtime_thread_exit_spawned,
        .memory_is_mapped=runtime_memory_is_mapped,
    };
    int initialized=-1;
    if (runtime_coop_enabled()) {
        const BbSeamlessInitFn seamless_init=(BbSeamlessInitFn)dlsym(module,"bbseamless_init");
        const BbSeamlessHost seamless_host={.abi=BBSEAMLESS_ABI_VERSION,.net=host,
            .thread_attach=runtime_thread_attach_host,.memory_protection=runtime_memory_protection,
            .trophy_unlock=party_trophy,.publish_ui=bbgpu_coop_publish,
            .next_command=bbgpu_coop_next_command,.take_portrait=bbgpu_coop_take_portrait,.user_dir=game_user};
        if (seamless_init) initialized=seamless_init(&seamless_host);
        if (!found || initialized) coop_unavailable("client module failed its ABI check; rebuild the client");
    } else if (init) initialized=init(&host);
    if (!found || initialized) {
        printf("Online: %s did not start: offline play\n",path);
        return NULL;
    }
    printf("Online: module %s\n",path);
    resolve=found;
    online_module=module;
    module_set_image=(BbNetSetImageFn)dlsym(module,"bbnet_set_image");
    module_start=(BbNetStartFn)dlsym(module,"bbnet_start");
    return resolve;
}

void runtime_coop_note_segment(uint64_t address,uint64_t size,unsigned flags) {
    if (!runtime_coop_enabled() || !module_resolve()) return;
    void (*note)(uint64_t,uint64_t,unsigned)=dlsym(online_module,"bbcoop_note_segment");
    if (!note) coop_unavailable("segment hook is missing");
    note(address,size,flags);
}
int runtime_coop_install(unsigned char *image,uint64_t size) {
    if (!runtime_coop_enabled()) return 0;
    if (!module_resolve()) return -1;
    int (*install)(unsigned char*,uint64_t,void*,uint64_t,const char*)=
        dlsym(online_module,"bbseamless_install");
    enum { CODE_BYTES=1024*1024 };
    void *code=runtime_low_map(CODE_BYTES,PROT_READ|PROT_WRITE|PROT_EXEC);
    if (!install || !code || (uintptr_t)code>(uintptr_t)image+INT32_MAX ||
        (uintptr_t)image>(uintptr_t)code+INT32_MAX) return -1;
    return install(image,size,code,CODE_BYTES,game_folder);
}
void runtime_coop_frame(uint32_t *buttons,uint8_t sticks[4],uint8_t *l2,uint8_t *r2,int32_t *touch_side) {
    if (!runtime_coop_enabled() || !online_module) return;
    static BbSeamlessFrameFn frame;
    if (!frame) frame=dlsym(online_module,"bbseamless_frame");
    if (frame) frame(buttons,sticks,l2,r2,touch_side);
}
void runtime_coop_file_opened(const char *path) {
    if (!runtime_coop_enabled() || !online_module) return;
    void (*opened)(const char*)=dlsym(online_module,"bbcoop_file_opened");
    if (opened) opened(path);
}
void runtime_coop_trophy_unlocked(int32_t id) {
    if (!runtime_coop_enabled() || !online_module) return;
    void (*unlocked)(int32_t)=dlsym(online_module,"bbcoop_trophy_unlocked");
    if (unlocked) unlocked(id);
}
int runtime_coop_sound_held(void) {
    if (!runtime_coop_enabled() || !online_module) return 0;
    int (*held)(void)=dlsym(online_module,"bbcoop_sound_held");
    return held?held():0;
}
int runtime_coop_camera_mouse(float dx,float dy) {
    if (!runtime_coop_enabled() || !online_module) return 0;
    int (*mouse)(float,float)=dlsym(online_module,"bbcoop_camera_mouse");
    return mouse?mouse(dx,dy):0;
}

/* The loaded image before the patches (probe.c): loads the module now, so it can read it. */
void runtime_net_set_image(unsigned char *image, uint64_t size) {
    if (!runtime_net_enabled() || !module_resolve() || !module_set_image) return;
    module_set_image(image,size);
}

/* The game is about to start (probe.c): the module's online session may begin. */
void runtime_net_start(void) {
    if (!runtime_net_enabled() || !module_resolve() || !module_start) return;
    module_start();
}

uintptr_t runtime_net_resolve(const char *name) {
    if (!runtime_net_enabled()) return 0;
    const char *symbol=runtime_symbol(name);
    if (!runtime_net_symbol(symbol)) return 0;
    const BbNetResolveFn resolve=module_resolve();
    const uintptr_t address=resolve ? resolve(name) : 0;
    if (resolve && !address) printf("Online: %s has no online version; the offline one is used\n",symbol);
    return address;
}
