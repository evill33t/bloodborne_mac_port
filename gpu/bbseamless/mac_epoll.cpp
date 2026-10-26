// SPDX-License-Identifier: GPL-2.0-or-later
// The imported client uses level-triggered epoll readiness; kqueue preserves that contract.
#include "sys/epoll.h"
#include <sys/event.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <map>
#include <mutex>
#include <vector>
namespace {
std::mutex mutex;
std::map<int,std::map<int,epoll_event>> queues;
int change(int queue,int fd,int16_t filter,uint16_t flags,uintptr_t data) {
    struct kevent event;
    EV_SET(&event,fd,filter,flags,0,0,reinterpret_cast<void*>(data));
    return kevent(queue,&event,1,nullptr,0,nullptr);
}
}
int epoll_create1(int flags) {
    if (flags & ~EPOLL_CLOEXEC) { errno=EINVAL;return -1; }
    const int fd=kqueue();
    if (fd>=0) { std::scoped_lock lock{mutex};queues[fd]={}; }
    return fd;
}
int epoll_close(int queue) {
    std::scoped_lock lock{mutex};
    if (!queues.erase(queue)) { errno=EBADF;return -1; }
    return close(queue);
}
int epoll_ctl(int queue,int operation,int fd,epoll_event* event) {
    std::scoped_lock lock{mutex};
    auto found=queues.find(queue);
    if (found==queues.end()) { errno=EBADF;return -1; }
    auto& watched=found->second;
    if (operation!=EPOLL_CTL_ADD && operation!=EPOLL_CTL_MOD && operation!=EPOLL_CTL_DEL) {
        errno=EINVAL;return -1;
    }
    if (operation!=EPOLL_CTL_DEL && !event) { errno=EFAULT;return -1; }
    if (operation==EPOLL_CTL_ADD && watched.contains(fd)) { errno=EEXIST;return -1; }
    if (operation!=EPOLL_CTL_ADD && !watched.contains(fd)) { errno=ENOENT;return -1; }
    if (fd==queue) { errno=EINVAL;return -1; }
    if (fcntl(fd,F_GETFD)<0) return -1;
    const auto remove=[&](const epoll_event& item) {
        if(item.events & (EPOLLIN|EPOLLRDHUP)) change(queue,fd,EVFILT_READ,EV_DELETE,0);
        if(item.events & EPOLLOUT) change(queue,fd,EVFILT_WRITE,EV_DELETE,0);
    };
    const epoll_event old=operation==EPOLL_CTL_ADD?epoll_event{}:watched.at(fd);
    if(operation==EPOLL_CTL_DEL) { remove(old);watched.erase(fd);return 0; }
    if(event->events & ~(EPOLLIN|EPOLLOUT|EPOLLERR|EPOLLHUP|EPOLLRDHUP|EPOLLET)) { errno=EINVAL;return -1; }
    const auto apply=[&](const epoll_event& item) {
        const uint16_t flags=EV_ADD|EV_ENABLE|((item.events & EPOLLET)?EV_CLEAR:0);
        if((item.events & (EPOLLIN|EPOLLRDHUP)) && change(queue,fd,EVFILT_READ,flags,item.data.u64)<0) return -1;
        if((item.events & EPOLLOUT) && change(queue,fd,EVFILT_WRITE,flags,item.data.u64)<0) return -1;
        return 0;
    };
    if(apply(*event)<0) {
        const int error=errno;remove(*event);apply(old);errno=error;return -1;
    }
    if((old.events & (EPOLLIN|EPOLLRDHUP)) && !(event->events & (EPOLLIN|EPOLLRDHUP))) change(queue,fd,EVFILT_READ,EV_DELETE,0);
    if((old.events & EPOLLOUT) && !(event->events & EPOLLOUT)) change(queue,fd,EVFILT_WRITE,EV_DELETE,0);
    watched[fd]=*event;return 0;
}
int epoll_wait(int queue,epoll_event* out,int count,int milliseconds) {
    if (count<=0 || count>65536 || !out) { errno=EINVAL;return -1; }
    std::vector<struct kevent> ready(static_cast<size_t>(count));
    const timespec timeout{milliseconds<0?0:milliseconds/1000,
                           milliseconds<0?0:(milliseconds%1000)*1000000L};
    int n=kevent(queue,nullptr,0,ready.data(),count,milliseconds<0?nullptr:&timeout);
    if (n<0) return n;
    std::vector<uintptr_t> descriptors;
    int used=0;
    for (int i=0;i<n;++i) {
        const auto data=reinterpret_cast<uintptr_t>(ready[i].udata);
        int index=0;while (index<used && descriptors[index]!=ready[i].ident) ++index;
        if (index==used) { descriptors.push_back(ready[i].ident);out[used]={};out[used++].data.u64=data; }
        if (ready[i].filter==EVFILT_READ) out[index].events|=EPOLLIN;
        if (ready[i].filter==EVFILT_WRITE) out[index].events|=EPOLLOUT;
        if (ready[i].flags & EV_EOF) out[index].events|=EPOLLHUP|EPOLLRDHUP;
        if (ready[i].flags & EV_ERROR) out[index].events|=EPOLLERR;
    }
    return used;
}
