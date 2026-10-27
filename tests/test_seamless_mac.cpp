// SPDX-License-Identifier: GPL-2.0-or-later
#include "sys/epoll.h"
#include "mac_memory.h"
#include "touch.h"
#include "invasion_visual.h"
#include "common/float_chars.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <sys/mman.h>
#include <unistd.h>
int main() {
    // A cosmetic row may replace only a known blank or its own prior result; preserve mods.
    std::array<uint8_t,BbInvasionVisual::RowSize> blank{},visual{},source{};
    uint16_t state=276;float rate=.7f,duration=-1.f;
    std::memcpy(source.data()+0x156,&state,2);std::memcpy(source.data()+0x10,&rate,4);std::memcpy(source.data()+8,&duration,4);
    const auto original_source=source;
    assert(BbInvasionVisual::Prepare(visual,blank,source));
    std::memcpy(&rate,visual.data()+0x10,4);assert(rate==1.f && source==original_source);
    assert(BbInvasionVisual::Prepare(visual,blank,source));
    visual[0x20]=99;const auto modified=visual;
    assert(!BbInvasionVisual::Prepare(visual,blank,source) && visual==modified);
    visual=blank;source[0x156]=0;
    assert(!BbInvasionVisual::Prepare(visual,blank,source) && visual==blank);
    assert(!BbInvasionVisual::Prepare(std::span(visual).first(3),blank,original_source));
    int a[2],b[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,a)==0);assert(socketpair(AF_UNIX,SOCK_STREAM,0,b)==0);
    const int queue=epoll_create1(EPOLL_CLOEXEC);assert(queue>=0);
    epoll_event input{EPOLLIN,{.u64=7}},out[8];
    assert(epoll_ctl(queue,EPOLL_CTL_ADD,a[0],&input)==0);
    assert(epoll_ctl(queue,EPOLL_CTL_ADD,a[0],&input)==-1 && errno==EEXIST);
    assert(epoll_wait(queue,out,8,0)==0);
    assert(write(a[1],"abc",3)==3);
    assert(epoll_wait(queue,out,8,50)==1 && out[0].data.u64==7 && (out[0].events&EPOLLIN));
    assert(epoll_wait(queue,out,8,0)==1); // level-triggered until drained
    input.data.u64=9;assert(epoll_ctl(queue,EPOLL_CTL_MOD,a[0],&input)==0);
    assert(epoll_wait(queue,out,8,0)==1 && out[0].data.u64==9);
    input.events=1u<<20;assert(epoll_ctl(queue,EPOLL_CTL_MOD,a[0],&input)==-1 && errno==EINVAL);
    assert(epoll_wait(queue,out,8,0)==1 && out[0].data.u64==9); // failed MOD retains interest
    input={EPOLLIN,{.u64=9}};assert(epoll_ctl(queue,EPOLL_CTL_ADD,b[0],&input)==0);
    assert(write(b[1],"z",1)==1);
    assert(epoll_wait(queue,out,8,50)==2); // equal user data does not merge distinct descriptors
    char bytes[4];assert(read(a[0],bytes,4)==3);assert(read(b[0],bytes,4)==1);
    assert(epoll_wait(queue,out,8,0)==0);
    close(a[1]);assert(epoll_wait(queue,out,8,50)==1 && (out[0].events&EPOLLHUP));
    assert(epoll_ctl(queue,EPOLL_CTL_DEL,a[0],nullptr)==0);assert(epoll_close(queue)==0);
    close(a[0]);close(b[0]);close(b[1]);
    const size_t page=getpagesize();void* memory=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);assert(memory!=MAP_FAILED);
    uintptr_t first=0,last=0;uint32_t prot=0;
    assert(BbSeamless::HostRegion(reinterpret_cast<uintptr_t>(memory),first,last,prot) && (prot&3)==3);
    assert(mprotect(memory,page,PROT_READ)==0);
    assert(BbSeamless::HostRegion(reinterpret_cast<uintptr_t>(memory),first,last,prot) && (prot&3)==1);
    munmap(memory,page);assert(!BbSeamless::HostRegion(reinterpret_cast<uintptr_t>(memory),first,last,prot));
    float value=17;const char* good="-12.5 rest";auto result=BbSeamless::FloatChars(good,good+std::strlen(good),value);
    assert(result.ec==std::errc{} && value==-12.5f && *result.ptr==' ');
    const char* bad="+1";assert(BbSeamless::FloatChars(bad,bad+2,value).ec==std::errc::invalid_argument && value==-12.5f);
    BbSeamless::TouchHold touch;int side=-1;
    assert(touch.Step(true,1,1000,side)==0 && side==-1);
    assert(touch.Step(false,0,51000,side)==0 && side==-1);
    assert(touch.Step(false,0,302000,side)==0 && side==1); // delayed original right-side gesture
    touch={};assert(touch.Step(true,0,1000,side)==0);
    assert(touch.Step(true,0,451000,side)==1 && side==-1);
    assert(touch.Step(true,0,600000,side)==0); // one map request per hold
    assert(touch.Step(false,0,610000,side)==0 && side==-1);
    touch={};touch.Step(true,0,1000,side);touch.Step(false,0,51000,side);
    assert(touch.Step(true,0,100000,side)==2 && side==-1); // second tap pings
    assert(touch.Step(false,0,120000,side)==0 && side==-1);
    puts("PASS: guarded invasion visual rows, Mac sockets, EOF, memory protection, float parsing, map hold and ping taps");
}
