/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BBSEAMLESS_CLIENT_H
#define BBSEAMLESS_CLIENT_H
#include "../bbnet/bbnet.h"
#include "ui.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BBSEAMLESS_ABI_VERSION 4
#define BBSEAMLESS_DEFAULT_HOST "bloodborne.phobos.services"
#define BBSEAMLESS_DEFAULT_LINK BBSEAMLESS_DEFAULT_HOST ":31314"
#define BBSEAMLESS_DEFAULT_WEBAPI "http://" BBSEAMLESS_DEFAULT_HOST ":31315"
#define BBSEAMLESS_DEFAULT_SERVICES "http://" BBSEAMLESS_DEFAULT_HOST ":20443"
#define BBSEAMLESS_DEFAULT_SERVER_KEY \
    "1562e07491a27d6c7aea2ed37364b8839a4a924886afae1ff6eae5c483ea3424"
typedef struct BbSeamlessHost {
    uint32_t abi;
    BbNetHost net;
    void (*thread_attach)(const char *name);
    int (*memory_protection)(uintptr_t address, uintptr_t *start, uintptr_t *end, uint32_t *prot);
    int32_t (*trophy_unlock)(int32_t trophy);
    void (*publish_ui)(const BbCoopUi *view);
    int (*next_command)(BbCoopCommand *command);
    int (*take_portrait)(uint8_t *rgba, uint32_t capacity);
    const char *user_dir;
    int (*reward_lookup)(const char*,uint64_t,const void*,size_t,int);
    int (*reward_begin)(const char*,uint64_t,const void*,size_t,int);
    int (*reward_arm)(uint64_t,int);
    int (*reward_disarm)(uint64_t);
    int (*reward_cancel)(uint64_t);
} BbSeamlessHost;
typedef void (*BbSeamlessFrameFn)(uint32_t*,uint8_t[4],uint8_t*,uint8_t*,int32_t*);
typedef int (*BbSeamlessInitFn)(const BbSeamlessHost *host);
int bbseamless_init(const BbSeamlessHost *host);
#ifdef __cplusplus
}
#endif
#endif
