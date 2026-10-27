// SPDX-License-Identifier: GPL-2.0-or-later
#include "gpu/bbseamless/client.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
static int32_t Spawn(void**,void* __attribute__((sysv_abi))(*)(void*),void*,uint64_t,const char*) {return ENOSYS;}
static int32_t Join(void*,void**) {return ENOSYS;}
static void Exit(void*) {std::_Exit(7);}
static int Mapped(uintptr_t,uint64_t) {return 0;}
static void Attach(const char*) {}
static int Protection(uintptr_t,uintptr_t*,uintptr_t*,uint32_t*) {return 0;}
static void Publish(const BbCoopUi*) {}
static int Command(BbCoopCommand*) {return 0;}
static int Model(uint8_t*,uint32_t) {return 0;}
int main(int argc,char** argv) {
    assert(argc>=3);
    unsetenv("BB_SHADNET_WEBAPI");unsetenv("BB_SHADNET_SERVER_KEY");
    const bool custom=!std::strcmp(argv[2],"custom");
    if(custom) setenv("BB_SHADNET_SERVER","different.example:32000",1);else unsetenv("BB_SHADNET_SERVER");
    auto module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!module) {std::fprintf(stderr,"%s\n",dlerror());return 1;}
    auto init=reinterpret_cast<BbSeamlessInitFn>(dlsym(module,"bbseamless_init"));
    auto resolve=reinterpret_cast<BbNetResolveFn>(dlsym(module,"bbnet_resolve"));
    assert(init && resolve);
    const auto temp=std::filesystem::temp_directory_path()/ ("bb-seamless-guard-"+std::to_string(getpid()));
    const auto user=temp.string();
    BbSeamlessHost host{};host.abi=BBSEAMLESS_ABI_VERSION;host.net.abi=BBNET_ABI_VERSION;
    host.net.app0="/nonexistent";host.net.serial="CUSA03173";host.net.title="Bloodborne";host.net.app_ver="01.09";
    host.net.thread_spawn=Spawn;host.net.thread_join=Join;host.net.thread_exit=Exit;host.net.memory_is_mapped=Mapped;
    host.thread_attach=Attach;host.memory_protection=Protection;host.publish_ui=Publish;host.next_command=Command;
    host.take_portrait=Model;host.user_dir=user.c_str();
    host.reward_lookup=[](const char*,uint64_t,const void*,size_t,int){return 0;};
    host.reward_begin=host.reward_lookup;
    host.reward_arm=[](uint64_t,int){return 0;};
    host.reward_disarm=[](uint64_t){return 0;};host.reward_cancel=host.reward_disarm;
    assert(init(nullptr)==-1);host.abi=0;assert(init(&host)==-1);host.abi=BBSEAMLESS_ABI_VERSION;
    assert(init(&host)==0);assert(resolve("Nlev7Lg8k3A#libSceNet#libSceNet")!=0);
    assert(std::strcmp(std::getenv("BB_SHADNET_WEBAPI"),custom?"http://different.example:31315":BBSEAMLESS_DEFAULT_WEBAPI)==0);
    if(custom) assert(!std::getenv("BB_SHADNET_SERVER_KEY"));
    else assert(std::strcmp(std::getenv("BB_SHADNET_SERVER_KEY"),BBSEAMLESS_DEFAULT_SERVER_KEY)==0);
    {
        auto fixture=temp/"game";std::filesystem::create_directories(fixture/"sce_sys");
        std::vector<unsigned char> sfo(104);
        const auto put32=[&](size_t at,uint32_t value) {std::memcpy(sfo.data()+at,&value,4);};
        put32(0,0x46535000);put32(4,0x101);put32(8,52);put32(12,72);put32(16,2);
        put32(20,0x02040000);put32(24,10);put32(28,16);put32(32,0);
        put32(36,0x02040009);put32(40,6);put32(44,16);put32(48,16);
        std::memcpy(sfo.data()+52,"TITLE_ID\0APP_VER\0",17);
        std::memcpy(sfo.data()+72,"CUSA03173",10);std::memcpy(sfo.data()+88,"01.09",6);
        std::ofstream out(fixture/"sce_sys/param.sfo",std::ios::binary);out.write(reinterpret_cast<const char*>(sfo.data()),sfo.size());out.close();
        auto install=reinterpret_cast<int(*)(unsigned char*,uint64_t,void*,uint64_t,const char*)>(dlsym(module,"bbseamless_install"));assert(install);
        setenv("SHADPS4_BLOODBORNE_SEAMLESS_COOP","1",1);
        constexpr size_t Size=2*1024*1024;
        auto image=static_cast<unsigned char*>(mmap(nullptr,Size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_ANON|MAP_PRIVATE,-1,0));assert(image!=MAP_FAILED);
        std::memset(image,0xCC,Size);
        const auto app0=argc==4?std::string{argv[3]}:fixture.string();
        assert(install(image,65536,image+1024*1024,1024*1024,app0.c_str())==-1);
        for(size_t i=0;i<65536;++i) assert(image[i]==0xCC);
        munmap(image,Size);
    }
    std::filesystem::remove_all(temp);
    puts("PASS: seamless module ABI, imports, server defaults/overrides and rejected image stays unchanged");
}
