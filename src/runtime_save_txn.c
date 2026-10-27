// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
/* Native runtime/save foundation: Mark Hall. Whole-directory copy-on-write saves.
 * A native character image and its reward receipt
 * become visible together; there is no receipt-only or partially overwritten save state.
 * Native temporary-close save work by lKaniel (bloodborne-server@8d5e54f,
 * bbport/src/runtime_file.c) informed the boundary; this implementation also covers the
 * game's preceding userdata -> backup rename by exchanging the whole directory. */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "runtime_save_txn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/file.h>
#ifdef __APPLE__
#include <copyfile.h>
#include <sys/stdio.h>
#else
#include <sys/syscall.h>
#endif

enum { TXNS=16, WRITERS=128, RANGES=128, SCOPE_MAX=512, PAYLOAD_MAX=4096,
       CHARACTER_BYTES=0x140000, TREE_ENTRIES_MAX=8192, RECEIPTS_MAX=4096 };
static const char receipt_dir[]=".bb-reward-receipts";
typedef struct { uint64_t first,last; } Range;
typedef struct {
    int used, writable, failed, writers, expected_started, expected_closed, range_count, traced_writes;
    char canonical[1024], stage[1024];
    Range ranges[RANGES];
} Transaction;
typedef struct { int used, fd, transaction, expected; } Writer;
typedef struct {
    int present, armed, slot;
    uint64_t id;
    size_t size;
    char scope[SCOPE_MAX+1];
    unsigned char payload[PAYLOAD_MAX];
} Pending;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static Transaction transactions[TXNS];
static Writer writers[WRITERS];
static Pending pending;
static int enabled, root_lock=-1;
static char save_root[1024], title_id[32], canonical_root[1024];
static int trace_on(void) { const char *e=getenv("BB_SAVE_TRACE"); return e && e[0]=='1'; }
#ifdef BB_SAVE_TXN_TESTING
static int (*fault_callback)(const char *);
void runtime_save_txn_test_fault(int (*callback)(const char *)) { fault_callback=callback; }
static int fault(const char *where) { return fault_callback ? fault_callback(where) : 0; }
#else
static int fault(const char *where) { (void)where; return 0; }
#endif

static int path_join(char *out,size_t cap,const char *a,const char *b) {
    int n=snprintf(out,cap,"%s/%s",a,b);
    return n<0 || (size_t)n>=cap ? -ENAMETOOLONG : 0;
}
static int regular(const char *path,struct stat *out) {
    struct stat st;
    if(lstat(path,&st)) return -errno;
    if(!S_ISREG(st.st_mode)) return -EINVAL;
    if(out) *out=st;
    return 0;
}
static int sync_fd(int fd) {
    return fsync(fd) ? -errno : 0;
}
static int full_sync_fd(int fd) {
    int result=sync_fd(fd);
    if(result) return result;
#ifdef __APPLE__
    /* fsync alone need not flush a drive's volatile write cache on macOS. */
    if(fcntl(fd,F_FULLFSYNC) && errno!=EINVAL && errno!=ENOTSUP) return -errno;
#endif
    return 0;
}
static int sync_directory(const char *path) {
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -errno;
    int result=fsync(fd) ? -errno : 0;
    close(fd);
    return result;
}
static int parent_path(const char *path,char *out,size_t cap) {
    if(strlen(path)>=cap) return -ENAMETOOLONG;
    strcpy(out,path);
    char *slash=strrchr(out,'/');
    if(!slash || slash==out) return -EINVAL;
    *slash=0;
    return 0;
}
/* readdir distinguishes end-of-directory from an I/O error only through errno.
 * Recursive work may change errno, so clear it immediately before every read. */
static struct dirent *read_entry(DIR *dir,int *result) {
    errno=0;
    struct dirent *entry=readdir(dir);
    if(!entry && errno) *result=-errno;
    return entry;
}
/* Only our own sibling stages are removed. Symlinks are unlinked, never traversed. */
static int remove_at(int parent,const char *name,unsigned *entries) {
    if(++*entries>TREE_ENTRIES_MAX) return -EFBIG;
    struct stat st;
    if(fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW)) return errno==ENOENT ? 0 : -errno;
    if(!S_ISDIR(st.st_mode)) return unlinkat(parent,name,0) ? -errno : 0;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -errno;
    DIR *dir=fdopendir(fd);
    if(!dir) { int result=-errno; close(fd); return result; }
    int result=0;
    for(struct dirent *e;(e=read_entry(dir,&result));) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if((result=remove_at(dirfd(dir),e->d_name,entries))) break;
    }
    closedir(dir);
    if(!result && unlinkat(parent,name,AT_REMOVEDIR)) result=-errno;
    return result;
}
static int remove_tree(const char *path) {
    char parent[1200]; int result=parent_path(path,parent,sizeof(parent));
    if(result) return result;
    const char *name=strrchr(path,'/')+1;
    if(strncmp(name,".bb-save-txn-SPRJ0005-",22) || strlen(name)!=28) return -EINVAL;
    for(size_t i=22;i<28;++i) if(!isalnum((unsigned char)name[i])) return -EINVAL;
    int fd=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -errno;
    unsigned entries=0; result=remove_at(fd,name,&entries); close(fd); return result;
}
static int clone_tree(const char *from,const char *to,unsigned *entries,uint64_t *bytes) {
    if(++*entries>TREE_ENTRIES_MAX) return -EFBIG;
    struct stat st;
    if(lstat(from,&st)) return -errno;
    if(S_ISDIR(st.st_mode)) {
        if(mkdir(to,0700) && errno!=EEXIST) return -errno;
        DIR *dir=opendir(from);
        if(!dir) return -errno;
        int result=0;
        for(struct dirent *e;(e=read_entry(dir,&result));) {
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
            char a[1200],b[1200];
            if((result=path_join(a,sizeof(a),from,e->d_name)) ||
               (result=path_join(b,sizeof(b),to,e->d_name)) ||
               (result=clone_tree(a,b,entries,bytes))) break;
        }
        closedir(dir);
#ifdef __APPLE__
        if(!result && copyfile(from,to,NULL,COPYFILE_METADATA|COPYFILE_NOFOLLOW_SRC|COPYFILE_NOFOLLOW_DST)) result=-errno;
#else
        if(!result && chmod(to,st.st_mode&0777)) result=-errno;
#endif
        return result;
    }
    if(S_ISREG(st.st_mode)) {
        if(st.st_size<0 || (*bytes+=(uint64_t)st.st_size)>UINT64_C(512)*1024*1024) return -EFBIG;
#ifdef __APPLE__
        if(copyfile(from,to,NULL,COPYFILE_ALL|COPYFILE_CLONE|COPYFILE_NOFOLLOW_SRC|COPYFILE_NOFOLLOW_DST)) return -errno;
        return 0;
#else
        int in=open(from,O_RDONLY|O_CLOEXEC|O_NOFOLLOW),out=-1,result=0;
        if(in<0) return -errno;
        out=open(to,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,st.st_mode&0777);
        if(out<0) result=-errno;
        unsigned char buffer[65536]; ssize_t n;
        while(!result && (n=read(in,buffer,sizeof(buffer)))>0) {
            ssize_t at=0;
            while(at<n) { ssize_t written=write(out,buffer+at,(size_t)(n-at)); if(written<=0) { result=-errno; break; } at+=written; }
        }
        if(!result && n<0) result=-errno;
        if(out>=0 && close(out) && !result) result=-errno;
        close(in); return result;
#endif
    }
    if(S_ISLNK(st.st_mode)) {
#ifdef __APPLE__
        return copyfile(from,to,NULL,COPYFILE_ALL|COPYFILE_NOFOLLOW_SRC|COPYFILE_NOFOLLOW_DST) ? -errno : 0;
#else
        char link[1200]; ssize_t n=readlink(from,link,sizeof(link)-1);
        if(n<0) return -errno;
        if((size_t)n>=sizeof(link)-1) return -ENAMETOOLONG;
        link[n]=0; return symlink(link,to) ? -errno : 0;
#endif
    }
    return -EINVAL;
}
static int sync_tree(const char *path,unsigned *entries) {
    if(++*entries>TREE_ENTRIES_MAX) return -EFBIG;
    struct stat st;
    if(lstat(path,&st)) return -errno;
    if(S_ISLNK(st.st_mode)) return 0;
    if(S_ISREG(st.st_mode)) {
        int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0) return -errno;
        int result=sync_fd(fd); close(fd); return result;
    }
    if(!S_ISDIR(st.st_mode)) return -EINVAL;
    DIR *dir=opendir(path);
    if(!dir) return -errno;
    int result=0;
    for(struct dirent *e;(e=read_entry(dir,&result));) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1200];
        if((result=path_join(child,sizeof(child),path,e->d_name)) || (result=sync_tree(child,entries))) break;
    }
    closedir(dir);
    return result ? result : sync_directory(path);
}
static uint64_t digest(const void *bytes,size_t size,uint64_t hash) {
    const unsigned char *p=bytes;
    for(size_t i=0;i<size;++i) hash=(hash^p[i])*UINT64_C(1099511628211);
    return hash;
}
static uint64_t scope_digest(const char *scope) { return digest(scope,strlen(scope),UINT64_C(14695981039346656037)); }
static void put32(unsigned char *p,uint32_t v) { for(int i=0;i<4;++i) p[i]=(unsigned char)(v>>(8*i)); }
static void put64(unsigned char *p,uint64_t v) { for(int i=0;i<8;++i) p[i]=(unsigned char)(v>>(8*i)); }
static uint32_t get32(const unsigned char *p) { uint32_t v=0; for(int i=0;i<4;++i) v|=(uint32_t)p[i]<<(8*i); return v; }
static uint64_t get64(const unsigned char *p) { uint64_t v=0; for(int i=0;i<8;++i) v|=(uint64_t)p[i]<<(8*i); return v; }
static int valid_request(const char *scope,uint64_t id,const void *payload,size_t size,int slot) {
    if(!scope || !*scope || strnlen(scope,SCOPE_MAX+1)>SCOPE_MAX || !id || !payload || !size || size>PAYLOAD_MAX || slot<0 || slot>9) return -EINVAL;
    return 0;
}
static int receipt_path(const char *root,const char *scope,uint64_t id,char *out,size_t cap) {
    char leaf[100];
    snprintf(leaf,sizeof(leaf),"%s/%016llx-%016llx.receipt",receipt_dir,
             (unsigned long long)scope_digest(scope),(unsigned long long)id);
    return path_join(out,cap,root,leaf);
}
static int parent_sync(void) {
    char parent[1024]; int result=parent_path(canonical_root,parent,sizeof(parent));
    if(!result) result=sync_directory(parent);
    /* Finish with the platform's full drive-cache barrier after publishing directory
     * metadata, rather than relying on a pre-rename file flush alone. */
    if(!result && root_lock>=0) result=full_sync_fd(root_lock);
    return result;
}
/* Receipts contain the complete bounded request, not a collision-prone digest alone. */
static int lookup_locked(const char *scope,uint64_t id,const void *payload,size_t size) {
    if(!enabled || !canonical_root[0] || root_lock<0) return -ENODEV;
    char receipt_root[1100];
    int result=path_join(receipt_root,sizeof(receipt_root),canonical_root,receipt_dir);
    if(result) return result;
    struct stat receipt_stat;
    if(lstat(receipt_root,&receipt_stat)) return errno==ENOENT ? 0 : -errno;
    if(!S_ISDIR(receipt_stat.st_mode)) return -EBADMSG;
    char path[1200]; result=receipt_path(canonical_root,scope,id,path,sizeof(path));
    if(result) return result;
    struct stat st;
    if(lstat(path,&st)) return errno==ENOENT ? 0 : -errno;
    if(!S_ISREG(st.st_mode) || st.st_nlink!=1 || st.st_size<40 || st.st_size>40+SCOPE_MAX+PAYLOAD_MAX) return -EBADMSG;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0) return -errno;
    unsigned char bytes[40+SCOPE_MAX+PAYLOAD_MAX]; size_t got=0;
    while(got<(size_t)st.st_size) { ssize_t n=read(fd,bytes+got,(size_t)st.st_size-got); if(n<=0) { result=n<0 ? -errno : -EBADMSG; break; } got+=(size_t)n; }
    if(!result && (memcmp(bytes,"BBRWD01\0",8) || get32(bytes+8)>SCOPE_MAX || get32(bytes+12)>PAYLOAD_MAX ||
       (uint64_t)40+get32(bytes+8)+get32(bytes+12)!=got || get64(bytes+16)!=id || get32(bytes+24)>9 || get32(bytes+28))) result=-EBADMSG;
    if(!result) {
        uint64_t check=digest(bytes,32,UINT64_C(14695981039346656037));
        check=digest(bytes+40,got-40,check);
        if(check!=get64(bytes+32)) result=-EBADMSG;
        else if(get32(bytes+8)!=strlen(scope) || get32(bytes+12)!=size || memcmp(bytes+40,scope,strlen(scope)) || memcmp(bytes+40+strlen(scope),payload,size)) result=-EEXIST;
    }
    if(!result) result=sync_fd(fd);
    close(fd);
    if(!result) { char dir[1100]; result=path_join(dir,sizeof(dir),canonical_root,receipt_dir); if(!result) result=sync_directory(dir); }
    if(!result) result=sync_directory(canonical_root);
    if(!result) result=parent_sync();
    if(!result && pending.present && pending.id==id && !strcmp(pending.scope,scope)) pending=(Pending){0};
    return result ? result : 1;
}
static int receipt_capacity(const char *root) {
    char dir[1100];
    int result=path_join(dir,sizeof(dir),root,receipt_dir);
    if(result) return result;
    struct stat st;
    if(lstat(dir,&st)) return errno==ENOENT ? 0 : -errno;
    if(!S_ISDIR(st.st_mode)) return -EBADMSG;
    DIR *listing=opendir(dir); if(!listing) return -errno;
    unsigned count=0;
    for(struct dirent *e;(e=read_entry(listing,&result));) if(strcmp(e->d_name,".") && strcmp(e->d_name,"..") && ++count>=RECEIPTS_MAX) {
        result=-ENOSPC; break;
    }
    closedir(listing);
    return result;
}
static int write_receipt(const char *root) {
    char dir[1100],path[1200];
    int result=receipt_capacity(root);
    if(result || (result=path_join(dir,sizeof(dir),root,receipt_dir))) return result;
    if(mkdir(dir,0700) && errno!=EEXIST) return -errno;
    struct stat st;
    if(lstat(dir,&st) || !S_ISDIR(st.st_mode)) return -EINVAL;
    if((result=receipt_path(root,pending.scope,pending.id,path,sizeof(path)))) return result;
    unsigned char bytes[40+SCOPE_MAX+PAYLOAD_MAX]={0};
    size_t scope_size=strlen(pending.scope),size=40+scope_size+pending.size;
    memcpy(bytes,"BBRWD01\0",8); put32(bytes+8,(uint32_t)scope_size); put32(bytes+12,(uint32_t)pending.size);
    put64(bytes+16,pending.id); put32(bytes+24,(uint32_t)pending.slot);
    memcpy(bytes+40,pending.scope,scope_size); memcpy(bytes+40+scope_size,pending.payload,pending.size);
    uint64_t check=digest(bytes,32,UINT64_C(14695981039346656037)); check=digest(bytes+40,size-40,check); put64(bytes+32,check);
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return -errno;
    size_t at=0;
    while(at<size) { ssize_t n=write(fd,bytes+at,size-at); if(n<=0) { result=n<0 ? -errno : -EIO; break; } at+=(size_t)n; }
    if(!result) result=sync_fd(fd);
    if(close(fd) && !result) result=-errno;
    return result;
}
static int establish_root(const char *canonical) {
    char expected[1200],base[1024],actual[1024];
    if(!realpath(save_root,base) || !realpath(canonical,actual)) return -errno;
    int n=snprintf(expected,sizeof(expected),"%s/1/%s/SPRJ0005",base,title_id);
    if(n<0 || (size_t)n>=sizeof(expected) || strcmp(expected,actual)) return -EINVAL;
    if(canonical_root[0]) return strcmp(canonical_root,actual) ? -EINVAL : 0;
    char parent[1024],lock_path[1200]; int result=parent_path(actual,parent,sizeof(parent));
    if(result || (result=path_join(lock_path,sizeof(lock_path),parent,".bb-save-lock-SPRJ0005"))) return result;
    int fd=open(lock_path,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) return -errno;
    struct stat st;
    if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_nlink!=1) { close(fd); return -EINVAL; }
    if(flock(fd,LOCK_EX|LOCK_NB)) { result=-errno; close(fd); return result; }
    /* Holding the profile's lock proves no other cooperating process owns these stages. */
    DIR *dir=opendir(parent);
    if(!dir) { result=-errno; close(fd); return result; }
    for(struct dirent *e;(e=read_entry(dir,&result));) if(!strncmp(e->d_name,".bb-save-txn-SPRJ0005-",22) && strlen(e->d_name)==28) {
        char stale[1200];
        if(!path_join(stale,sizeof(stale),parent,e->d_name)) remove_tree(stale);
    }
    closedir(dir);
    if(result) { close(fd); return result; }
    root_lock=fd; strcpy(canonical_root,actual);
    return 0;
}
void runtime_save_txn_configure(int on,const char *root,const char *title) {
    pthread_mutex_lock(&mutex);
    enabled=on;
    snprintf(save_root,sizeof(save_root),"%s",root ? root : "");
    snprintf(title_id,sizeof(title_id),"%s",title ? title : "");
    pthread_mutex_unlock(&mutex);
}
int runtime_save_txn_mount(const char *canonical,int writable,char *mounted,size_t capacity) {
    if(!canonical || !mounted || strlen(canonical)>=capacity) return -EINVAL;
    strcpy(mounted,canonical);
    pthread_mutex_lock(&mutex);
    int result=0;
    if(!enabled) goto done;
    const char *leaf=strrchr(canonical,'/');
    if(!leaf || strcmp(leaf+1,"SPRJ0005")) goto done;
    if((result=establish_root(canonical))) goto done;
    for(int i=0;i<TXNS;++i) if(transactions[i].used && !strcmp(transactions[i].canonical,canonical_root)) { result=-EBUSY; goto done; }
    if(!writable) goto done;
    int slot=-1;
    for(int i=0;i<TXNS;++i) if(!transactions[i].used) { slot=i; break; }
    if(slot<0) { result=-EMFILE; goto done; }
    Transaction *t=&transactions[slot];
    t->used=1; t->writable=1; strcpy(t->canonical,canonical_root);
    char parent[1024];
    if((result=parent_path(canonical_root,parent,sizeof(parent)))) goto failed;
    if(snprintf(t->stage,sizeof(t->stage),"%s/.bb-save-txn-SPRJ0005-XXXXXX",parent)>=(int)sizeof(t->stage)) { result=-ENAMETOOLONG; goto failed; }
    if(!mkdtemp(t->stage)) { result=-errno; goto failed; }
    unsigned entries=0; uint64_t bytes=0;
    if((result=clone_tree(canonical_root,t->stage,&entries,&bytes))) goto failed;
    if(strlen(t->stage)>=capacity) { result=-ENAMETOOLONG; goto failed; }
    strcpy(mounted,t->stage);
    goto done;
failed:
    if(t->stage[0]) remove_tree(t->stage);
    *t=(Transaction){0};
done:
    if(trace_on()) fprintf(stderr,"Save transaction: mount writable=%d pending=%d armed=%d stage=%s result=%d\n",
                          writable,pending.present,pending.armed,strcmp(mounted,canonical) ? "yes" : "no",result);
    pthread_mutex_unlock(&mutex);
    return result;
}
static int exchange_dirs(const char *a,const char *b) {
#ifdef __APPLE__
    return renameatx_np(AT_FDCWD,a,AT_FDCWD,b,RENAME_SWAP) ? -errno : 0;
#elif defined(SYS_renameat2)
    return syscall(SYS_renameat2,AT_FDCWD,a,AT_FDCWD,b,2) ? -errno : 0;
#else
    (void)a; (void)b; return -ENOTSUP;
#endif
}
int runtime_save_txn_umount(const char *mounted,int successful) {
    pthread_mutex_lock(&mutex);
    Transaction *t=NULL;
    for(int i=0;i<TXNS;++i) if(transactions[i].used && !strcmp(transactions[i].stage,mounted)) { t=&transactions[i]; break; }
    int result=0;
    if(!t) goto done;
    if(t->writers) { result=-EBUSY; goto done; }
    if(!successful || t->failed) { result=-EIO; goto discard; }
    if(pending.present && !pending.armed) {
        if(trace_on()) fprintf(stderr,"Save transaction: unarmed preimage pinned; native snapshot discarded\n");
        goto discard;
    }
    if(pending.present) {
        if(!t->expected_closed) {
            if(trace_on()) fprintf(stderr,"Save transaction: no proved userdata%04d close (started=%d ranges=%d); snapshot discarded\n",pending.slot,t->expected_started,t->range_count);
            goto discard; /* profile-only native transaction */
        }
        if(t->range_count!=1 || t->ranges[0].first || t->ranges[0].last!=CHARACTER_BYTES) { result=-EBADMSG; goto discard; }
        char leaf[32],path[1200];
        snprintf(leaf,sizeof(leaf),"userdata%04d",pending.slot);
        if((result=path_join(path,sizeof(path),t->stage,leaf))) goto discard;
        struct stat st;
        if((result=regular(path,&st))) goto discard;
        if(st.st_size!=CHARACTER_BYTES) {
            if(trace_on()) fprintf(stderr,"Save transaction: userdata%04d has size=%lld, expected=%d\n",pending.slot,(long long)st.st_size,CHARACTER_BYTES);
            result=-EBADMSG; goto discard;
        }
        int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW); unsigned char magic[4];
        if(fd<0) { result=-errno; goto discard; }
        ssize_t n=read(fd,magic,sizeof(magic)); close(fd);
        if(n!=4 || get32(magic)!=0x41) {
            if(trace_on()) fprintf(stderr,"Save transaction: userdata%04d header read=%lld version=0x%08x, expected=0x41\n",pending.slot,(long long)n,n==4 ? get32(magic) : 0);
            result=-EBADMSG; goto discard;
        }
        /* The native loader may fall back to backup000N after a failed load. Both choices
         * must include the credited award when the shared receipt becomes visible. */
        char backup[1200]; snprintf(leaf,sizeof(leaf),"backup%04d",pending.slot);
        if((result=path_join(backup,sizeof(backup),t->stage,leaf))) goto discard;
        if(unlink(backup) && errno!=ENOENT) { result=-errno; goto discard; }
        { unsigned entries=0; uint64_t bytes=0; if((result=clone_tree(path,backup,&entries,&bytes))) goto discard; }
        if((result=write_receipt(t->stage))) goto discard;
    }
    if((result=fault("before_flush"))) goto discard;
    { unsigned entries=0; if((result=sync_tree(t->stage,&entries))) goto discard; }
    if((result=parent_sync())) goto discard;
    if((result=fault("before_swap"))) goto discard;
    if((result=exchange_dirs(t->stage,t->canonical))) goto discard;
    result=fault("after_swap");
    /* Even when this fails the exchange is never reversed: canonical+receipt agree.
     * Lookup retries the flush and withholds proof/ACK until it succeeds. */
    if(!result) result=parent_sync();
    if(!result && pending.present) pending=(Pending){0};
discard:
    if(trace_on() || result) fprintf(stderr,"Save transaction: umount result=%d successful=%d failed=%d pending=%d armed=%d expected-started=%d expected-closed=%d ranges=%d first=%llu last=%llu\n",
                    result,successful,t->failed,pending.present,pending.armed,t->expected_started,t->expected_closed,t->range_count,
                    (unsigned long long)(t->range_count ? t->ranges[0].first : 0),
                    (unsigned long long)(t->range_count ? t->ranges[0].last : 0));
    { int cleaned=remove_tree(t->stage); if(!result && cleaned) result=cleaned; }
    *t=(Transaction){0};
done:
    pthread_mutex_unlock(&mutex);
    return result;
}
int runtime_save_txn_write_allowed(const char *path) {
    if(!path) return -EINVAL;
    pthread_mutex_lock(&mutex);
    int result=0;
    /* A read-only canonical mount must not bypass staging through an accidental write open. */
    const size_t canonical_size=strlen(canonical_root);
    if(enabled && canonical_size && !strncmp(path,canonical_root,canonical_size) &&
       (path[canonical_size]=='/' || !path[canonical_size])) {
        pthread_mutex_unlock(&mutex); return -EROFS;
    }
    for(int i=0;i<TXNS;++i) {
        Transaction *t=&transactions[i]; size_t prefix=strlen(t->stage);
        if(!t->used || strncmp(path,t->stage,prefix) || path[prefix]!='/') continue;
        /* Descriptor-relative traversal refuses symlink ancestors as well as the leaf.
         * The profile lock excludes concurrent cooperating writers. */
        int fd=open(t->stage,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0) { result=-errno; break; }
        char relative[1200]; snprintf(relative,sizeof(relative),"%s",path+prefix+1);
        char *save=NULL,*component=strtok_r(relative,"/",&save);
        while(component) {
            char *next=strtok_r(NULL,"/",&save);
            if(!strcmp(component,".") || !strcmp(component,"..")) { result=-EACCES; break; }
            struct stat st;
            if(fstatat(fd,component,&st,AT_SYMLINK_NOFOLLOW)) {
                result=errno==ENOENT && !next ? 0 : -errno; break;
            }
            if(S_ISLNK(st.st_mode)) { result=-ELOOP; break; }
            if(next) {
                int child=openat(fd,component,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
                if(child<0) { result=-errno; break; }
                close(fd); fd=child;
            }
            component=next;
        }
        close(fd); break;
    }
    pthread_mutex_unlock(&mutex); return result;
}
void runtime_save_txn_file_opened(int fd,const char *path,int writable) {
    if(!writable || !path) return;
    pthread_mutex_lock(&mutex);
    for(int i=0;i<TXNS;++i) {
        Transaction *t=&transactions[i]; size_t prefix=strlen(t->stage);
        if(!t->used || strncmp(path,t->stage,prefix) || path[prefix]!='/') continue;
        int slot=-1; for(int j=0;j<WRITERS;++j) if(!writers[j].used) { slot=j; break; }
        if(slot<0) { t->failed=1; break; }
        char expected[32]; snprintf(expected,sizeof(expected),"userdata%04d",pending.slot);
        const int is_expected=pending.present && pending.armed && !strcmp(path+prefix+1,expected);
        if(is_expected) {
            if(t->expected_started) t->failed=1; /* a replaced/reopened image loses provenance */
            t->expected_started=1; t->expected_closed=0; t->range_count=0;
        }
        writers[slot]=(Writer){.used=1,.fd=fd,.transaction=i,.expected=is_expected};
        ++t->writers; break;
    }
    pthread_mutex_unlock(&mutex);
}
static void cover(Transaction *t,uint64_t first,uint64_t last) {
    if(first>=last || last>CHARACTER_BYTES) { t->failed=1; return; }
    int at=0;
    while(at<t->range_count && t->ranges[at].last<first) ++at;
    int end=at;
    while(end<t->range_count && t->ranges[end].first<=last) {
        if(t->ranges[end].first<first) first=t->ranges[end].first;
        if(t->ranges[end].last>last) last=t->ranges[end].last;
        ++end;
    }
    int count=t->range_count-(end-at)+1;
    if(count>RANGES) { t->failed=1; return; }
    memmove(t->ranges+at+1,t->ranges+end,(size_t)(t->range_count-end)*sizeof(Range));
    t->ranges[at]=(Range){first,last}; t->range_count=count;
}
void runtime_save_txn_file_written(int fd,int64_t offset,uint64_t requested,int64_t result) {
    pthread_mutex_lock(&mutex);
    for(int i=0;i<WRITERS;++i) if(writers[i].used && writers[i].fd==fd) {
        Transaction *t=&transactions[writers[i].transaction];
        if(trace_on() && writers[i].expected && t->traced_writes++<8)
            fprintf(stderr,"Save transaction: expected userdata write offset=%lld requested=%llu result=%lld\n",
                    (long long)offset,(unsigned long long)requested,(long long)result);
        if(result<0 || (uint64_t)result!=requested || offset<0) t->failed=1;
        else if(writers[i].expected && result>0) cover(t,(uint64_t)offset,(uint64_t)offset+(uint64_t)result);
        break;
    }
    pthread_mutex_unlock(&mutex);
}
void runtime_save_txn_file_closed(int fd,int error) {
    pthread_mutex_lock(&mutex);
    for(int i=0;i<WRITERS;++i) if(writers[i].used && writers[i].fd==fd) {
        Transaction *t=&transactions[writers[i].transaction];
        if(error) t->failed=1;
        if(writers[i].expected && !error) t->expected_closed=1;
        --t->writers; writers[i]=(Writer){0}; break;
    }
    pthread_mutex_unlock(&mutex);
}
void runtime_save_txn_file_truncated(int fd,int64_t length,int result) {
    pthread_mutex_lock(&mutex);
    for(int i=0;i<WRITERS;++i) if(writers[i].used && writers[i].fd==fd) {
        Transaction *t=&transactions[writers[i].transaction];
        if(result || length<0) t->failed=1;
        else if(writers[i].expected) {
            /* ftruncate preserves written bytes below the new EOF. In particular, the
             * native stream may set its final size after its full write. An extension
             * contributes no coverage; shrinking removes proof only for discarded bytes. */
            int kept=0;
            for(int j=0;j<t->range_count;++j) {
                Range range=t->ranges[j];
                if(range.first>=(uint64_t)length) break;
                if(range.last>(uint64_t)length) range.last=(uint64_t)length;
                t->ranges[kept++]=range;
            }
            t->range_count=kept;
            if(trace_on()) fprintf(stderr,"Save transaction: expected userdata ftruncate length=%lld preserves ranges=%d\n",(long long)length,kept);
        }
        break;
    }
    pthread_mutex_unlock(&mutex);
}
void runtime_save_txn_path_mutated(const char *path) {
    if(!path) return;
    pthread_mutex_lock(&mutex);
    if(pending.present) for(int i=0;i<TXNS;++i) {
        Transaction *t=&transactions[i]; size_t prefix=strlen(t->stage);
        if(!t->used || !t->expected_started || strncmp(path,t->stage,prefix) || path[prefix]!='/') continue;
        char expected[32]; snprintf(expected,sizeof(expected),"userdata%04d",pending.slot);
        if(!strcmp(path+prefix+1,expected)) t->failed=1;
    }
    pthread_mutex_unlock(&mutex);
}
static int writable_active(void) { for(int i=0;i<TXNS;++i) if(transactions[i].used && transactions[i].writable) return 1; return 0; }
int runtime_reward_lookup(const char *scope,uint64_t id,const void *payload,size_t size,int slot) {
    int result=valid_request(scope,id,payload,size,slot); if(result) return result;
    pthread_mutex_lock(&mutex); result=lookup_locked(scope,id,payload,size); pthread_mutex_unlock(&mutex); return result;
}
int runtime_reward_begin(const char *scope,uint64_t id,const void *payload,size_t size,int slot) {
    int result=valid_request(scope,id,payload,size,slot); if(result) return result;
    pthread_mutex_lock(&mutex);
    result=lookup_locked(scope,id,payload,size);
    if(result>0) result=-EALREADY;
    else if(!result && (pending.present || writable_active())) result=-EBUSY;
    else if(!result) result=receipt_capacity(canonical_root);
    if(!result) {
        pending=(Pending){.present=1,.slot=slot,.id=id,.size=size};
        strcpy(pending.scope,scope); memcpy(pending.payload,payload,size);
    }
    pthread_mutex_unlock(&mutex); return result;
}
int runtime_reward_arm(uint64_t id,int slot) {
    pthread_mutex_lock(&mutex);
    int result=!pending.present || pending.id!=id || pending.slot!=slot ? -EINVAL : writable_active() ? -EBUSY : 0;
    if(!result) pending.armed=1;
    pthread_mutex_unlock(&mutex); return result;
}
int runtime_reward_disarm(uint64_t id) {
    pthread_mutex_lock(&mutex);
    int result=!pending.present || pending.id!=id ? -EINVAL : writable_active() ? -EBUSY : 0;
    if(!result) pending.armed=0;
    pthread_mutex_unlock(&mutex); return result;
}
int runtime_reward_cancel_before_apply(uint64_t id) {
    pthread_mutex_lock(&mutex);
    int result=!pending.present || pending.id!=id || pending.armed ? -EINVAL : writable_active() ? -EBUSY : 0;
    if(!result) pending=(Pending){0};
    pthread_mutex_unlock(&mutex); return result;
}
#ifdef BB_SAVE_TXN_TESTING
void runtime_save_txn_test_reset(void) {
    pthread_mutex_lock(&mutex);
    for(int i=0;i<TXNS;++i) if(transactions[i].used) remove_tree(transactions[i].stage);
    memset(transactions,0,sizeof(transactions)); memset(writers,0,sizeof(writers)); pending=(Pending){0};
    if(root_lock>=0) close(root_lock);
    root_lock=-1; canonical_root[0]=0; enabled=0; fault_callback=NULL;
    pthread_mutex_unlock(&mutex);
}
#endif
