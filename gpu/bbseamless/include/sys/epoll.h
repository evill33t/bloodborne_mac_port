// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
union epoll_data { void* ptr; int fd; uint32_t u32; uint64_t u64; };
struct epoll_event { uint32_t events; epoll_data data; };
enum : uint32_t { EPOLLIN=1, EPOLLOUT=4, EPOLLERR=8, EPOLLHUP=16,
                  EPOLLRDHUP=0x2000, EPOLLET=1u<<31 };
enum { EPOLL_CTL_ADD=1, EPOLL_CTL_DEL=2, EPOLL_CTL_MOD=3, EPOLL_CLOEXEC=0x80000 };
int epoll_create1(int flags);
int epoll_ctl(int queue,int operation,int fd,epoll_event* event);
int epoll_wait(int queue,epoll_event* events,int count,int milliseconds);
int epoll_close(int queue);
