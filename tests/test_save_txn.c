// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
/* Runtime/save foundation: Mark Hall.
 * Synthetic native-size character images, never a player's save. Exercises real copy,
 * descriptor coverage, fsync, directory exchange and fork/SIGKILL recovery boundaries. */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include "../src/runtime_save_txn.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <sys/xattr.h>
#endif

enum { IMAGE_SIZE=0x140000 };
static const uint64_t grant=UINT64_C(0x8000000000000001);
static const char scope[]="bloodborne.phobos.services|test-server-pin|test-account";
static const unsigned char payload[]={1,2,3,4};
static char root[1024],canonical[1200],mounted[1200];
static const char *fault_point;
static int kill_at_fault,fail_at_fault;
static char test_roots[32][1024];
static size_t root_count;
static int remove_fixture_entry(const char *path,const struct stat *st,int type,struct FTW *walk) {
    (void)st; (void)type; (void)walk; return remove(path);
}
static void cleanup(void) {
    runtime_save_txn_test_reset();
    for(size_t i=0;i<root_count;++i) {
        assert(!strncmp(test_roots[i],"/private/tmp/bb-save-txn-test-",30));
        assert(!nftw(test_roots[i],remove_fixture_entry,16,FTW_DEPTH|FTW_PHYS));
    }
}

static void join(char *out,size_t size,const char *a,const char *b) { assert(snprintf(out,size,"%s/%s",a,b)>0 && strlen(out)<size); }
static void write_all(int fd,const void *buffer,size_t size) {
    const unsigned char *p=buffer;
    while(size) { ssize_t n=write(fd,p,size); assert(n>0); p+=n; size-=(size_t)n; }
}
static unsigned char *image(unsigned char marker) {
    unsigned char *p=malloc(IMAGE_SIZE); assert(p); memset(p,marker,IMAGE_SIZE);
    p[0]=0x41; p[1]=p[2]=p[3]=0; return p;
}
static void put_image(const char *directory,const char *name,unsigned char marker) {
    char path[1400]; join(path,sizeof(path),directory,name);
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600); assert(fd>=0);
    unsigned char *bytes=image(marker); write_all(fd,bytes,IMAGE_SIZE); free(bytes); assert(!close(fd));
}
static unsigned char marker(const char *directory,const char *name) {
    char path[1400]; join(path,sizeof(path),directory,name);
    int fd=open(path,O_RDONLY); assert(fd>=0);
    unsigned char byte=0; assert(pread(fd,&byte,1,0x100)==1); close(fd); return byte;
}
static void configure(void) {
    runtime_save_txn_configure(1,root,"CUSA00900");
    char read_path[1200]; assert(!runtime_save_txn_mount(canonical,0,read_path,sizeof(read_path)));
    assert(!strcmp(read_path,canonical));
}
static void setup(void) {
    runtime_save_txn_test_reset();
    strcpy(root,"/private/tmp/bb-save-txn-test-XXXXXX"); assert(mkdtemp(root));
    assert(root_count<sizeof(test_roots)/sizeof(*test_roots)); strcpy(test_roots[root_count++],root);
    char one[1200],title[1200]; join(one,sizeof(one),root,"1"); assert(!mkdir(one,0700));
    join(title,sizeof(title),one,"CUSA00900"); assert(!mkdir(title,0700));
    join(canonical,sizeof(canonical),title,"SPRJ0005"); assert(!mkdir(canonical,0700));
    put_image(canonical,"userdata0000",'O'); put_image(canonical,"backup0000",'B');
    char extra[1400]; join(extra,sizeof(extra),canonical,"keep-this.txt");
    int fd=open(extra,O_WRONLY|O_CREAT|O_EXCL,0640); assert(fd>=0); write_all(fd,"keep",4); close(fd);
#ifdef __APPLE__
    assert(!setxattr(extra,"services.phobos.test","metadata",8,0,0));
#endif
    fault_point=NULL; kill_at_fault=fail_at_fault=0;
    configure();
}
static int fault(const char *point) {
    if(!fault_point || strcmp(point,fault_point)) return 0;
    if(kill_at_fault) { kill(getpid(),SIGKILL); _exit(200); }
    return fail_at_fault ? -EIO : 0;
}
static void begin(void) { assert(!runtime_reward_begin(scope,grant,payload,sizeof(payload),0)); }
static void arm(void) { assert(!runtime_reward_arm(grant,0)); }
static void start_write(void) { assert(!runtime_save_txn_mount(canonical,1,mounted,sizeof(mounted))); assert(strcmp(mounted,canonical)); }
static int open_image(void) {
    char backup[1400],current[1400]; join(backup,sizeof(backup),mounted,"backup0000"); join(current,sizeof(current),mounted,"userdata0000");
    assert(!unlink(backup)); runtime_save_txn_path_mutated(backup);
    assert(!rename(current,backup)); runtime_save_txn_path_mutated(current); runtime_save_txn_path_mutated(backup);
    int fd=open(current,O_WRONLY|O_CREAT|O_TRUNC,0600); assert(fd>=0);
    runtime_save_txn_file_opened(fd,current,1); return fd;
}
static void write_image(int fd,unsigned char value,size_t size) {
    unsigned char *bytes=image(value); off_t at=lseek(fd,0,SEEK_CUR); assert(at>=0);
    ssize_t result=write(fd,bytes,size); runtime_save_txn_file_written(fd,at,size,result);
    assert(result==(ssize_t)size); free(bytes);
}
static void close_image(int fd) { int result=close(fd); runtime_save_txn_file_closed(fd,result ? errno : 0); assert(!result); }
static int finish(unsigned char value) { start_write(); int fd=open_image(); write_image(fd,value,IMAGE_SIZE); close_image(fd); return runtime_save_txn_umount(mounted,1); }
static int lookup(int slot) { return runtime_reward_lookup(scope,grant,payload,sizeof(payload),slot); }
static void assert_kept_extra(void) {
    char path[1400]; join(path,sizeof(path),canonical,"keep-this.txt"); struct stat st; assert(!stat(path,&st)); assert((st.st_mode&0777)==0640);
    int fd=open(path,O_RDONLY); char bytes[4]; assert(fd>=0 && read(fd,bytes,4)==4 && !memcmp(bytes,"keep",4)); close(fd);
#ifdef __APPLE__
    char attr[8]; assert(getxattr(path,"services.phobos.test",attr,sizeof(attr),0,0)==8 && !memcmp(attr,"metadata",8));
#endif
}
static void normal_and_reward(void) {
    setup();
    assert(runtime_reward_lookup("",grant,payload,sizeof(payload),0)==-EINVAL);
    assert(runtime_reward_lookup(scope,grant,payload,sizeof(payload),10)==-EINVAL);
    assert(runtime_reward_lookup(scope,grant,payload,4097,0)==-EINVAL);
    assert(!finish('N')); assert(marker(canonical,"userdata0000")=='N'); assert(marker(canonical,"backup0000")=='O'); assert_kept_extra();
    begin(); assert(!finish('U')); /* an unrelated/pre-award native snapshot stays uncommitted */
    assert(marker(canonical,"userdata0000")=='N'); assert(!lookup(0));
    arm(); assert(!finish('R')); assert(marker(canonical,"userdata0000")=='R'); assert(marker(canonical,"backup0000")=='R'); assert(lookup(0)==1); assert(lookup(9)==1);
    unsigned char conflict[]={1,2,3,5}; assert(runtime_reward_lookup(scope,grant,conflict,sizeof(conflict),0)==-EEXIST);
    assert(runtime_reward_begin(scope,grant,payload,sizeof(payload),0)==-EALREADY);
    assert(runtime_reward_lookup("other-account",grant,payload,sizeof(payload),0)==0);
    assert(!finish('S')); assert(lookup(3)==1); assert(marker(canonical,"userdata0000")=='S'); assert(marker(canonical,"backup0000")=='R'); assert_kept_extra();
    runtime_save_txn_test_reset(); configure(); assert(lookup(8)==1);
    puts("save transaction: normal saves, pinned preimage, atomic receipt, account scope, later autosave: ok");
}
static void bad_images(void) {
    setup(); start_write(); assert(runtime_reward_begin(scope,grant,payload,sizeof(payload),0)==-EBUSY); assert(!runtime_save_txn_umount(mounted,1));
    begin(); arm(); start_write(); int fd=open_image(); write_image(fd,'P',IMAGE_SIZE/2); close_image(fd);
    assert(runtime_save_txn_umount(mounted,1)==-EBADMSG); assert(marker(canonical,"userdata0000")=='O'); assert(!lookup(0));
    start_write(); fd=open_image(); write_image(fd,'E',IMAGE_SIZE);
    runtime_save_txn_file_written(fd,IMAGE_SIZE,1,-EIO); close_image(fd);
    assert(runtime_save_txn_umount(mounted,1)==-EIO); assert(!lookup(0));
    start_write(); fd=open_image(); write_image(fd,'T',IMAGE_SIZE); assert(!ftruncate(fd,IMAGE_SIZE/2)); runtime_save_txn_file_truncated(fd,IMAGE_SIZE/2,0); close_image(fd);
    assert(runtime_save_txn_umount(mounted,1)==-EBADMSG); assert(!lookup(0));
    start_write(); fd=open_image(); write_image(fd,'Q',IMAGE_SIZE); close_image(fd);
    char current[1400]; join(current,sizeof(current),mounted,"userdata0000"); runtime_save_txn_path_mutated(current);
    assert(runtime_save_txn_umount(mounted,1)==-EIO); assert(!lookup(0));
    start_write(); fd=open_image(); write_image(fd,'L',IMAGE_SIZE);
    assert(!ftruncate(fd,IMAGE_SIZE)); runtime_save_txn_file_truncated(fd,IMAGE_SIZE,0);
    assert(runtime_save_txn_umount(mounted,1)==-EBUSY); close_image(fd);
    assert(!runtime_save_txn_umount(mounted,1)); assert(lookup(0)==1);
    puts("save transaction: active writers, incomplete images, write/truncate/path failures: ok");
}
static void profile_and_cancel(void) {
    setup(); begin(); assert(!runtime_reward_cancel_before_apply(grant)); begin(); arm(); start_write();
    char path[1400]; join(path,sizeof(path),mounted,"userdata0010"); int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600); assert(fd>=0);
    runtime_save_txn_file_opened(fd,path,1); write_image(fd,'G',IMAGE_SIZE); close_image(fd);
    assert(!runtime_save_txn_umount(mounted,1)); assert(!lookup(0)); assert(marker(canonical,"userdata0000")=='O');
    assert(!runtime_reward_disarm(grant)); assert(!runtime_reward_cancel_before_apply(grant));
    assert(runtime_reward_arm(grant,0)==-EINVAL);
    puts("save transaction: profile-only saves cannot certify rewards; queue refusal disarms: ok");
}
static void injected_failure(const char *point,int exchanged) {
    setup(); begin(); arm(); fault_point=point; fail_at_fault=1; runtime_save_txn_test_fault(fault);
    assert(finish('F')==-EIO); runtime_save_txn_test_fault(NULL);
    assert(marker(canonical,"userdata0000")== (exchanged ? 'F' : 'O'));
    assert(lookup(0)==exchanged);
    if(!exchanged) { assert(!finish('V')); assert(lookup(0)==1); }
    assert_kept_extra();
}
static int stages(void) {
    char parent[1200]; strcpy(parent,canonical); *strrchr(parent,'/')=0;
    DIR *dir=opendir(parent); assert(dir); int count=0;
    for(struct dirent *e;(e=readdir(dir));) if(!strncmp(e->d_name,".bb-save-txn-SPRJ0005-",22)) ++count;
    closedir(dir); return count;
}
static void crash(const char *point,int exchanged,int before_mount) {
    setup(); runtime_save_txn_test_reset();
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        configure(); begin();
        if(before_mount) { kill(getpid(),SIGKILL); _exit(201); }
        arm(); fault_point=point; kill_at_fault=1; runtime_save_txn_test_fault(fault);
        (void)finish('K'); _exit(202);
    }
    int status; assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    configure(); assert(!stages()); assert(marker(canonical,"userdata0000")==(exchanged ? 'K' : 'O'));
    assert(lookup(9)==exchanged); assert_kept_extra();
    if(!exchanged) { begin(); arm(); assert(!finish('A')); assert(lookup(0)==1); }
}
static void crash_mid_write(void) {
    setup(); runtime_save_txn_test_reset();
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        configure(); begin(); arm(); start_write(); int fd=open_image(); write_image(fd,'H',IMAGE_SIZE/2);
        kill(getpid(),SIGKILL); _exit(204);
    }
    int status; assert(waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
    configure(); assert(!stages()); assert(marker(canonical,"userdata0000")=='O'); assert(!lookup(0));
    begin(); arm(); assert(!finish('J')); assert(lookup(0)==1);
}
static void corruption_and_lock(void) {
    setup(); begin(); arm(); assert(!finish('C'));
    pid_t child=fork(); assert(child>=0);
    if(!child) {
        runtime_save_txn_test_reset(); runtime_save_txn_configure(1,root,"CUSA00900"); char path[1200];
        int result=runtime_save_txn_mount(canonical,0,path,sizeof(path)); _exit(result==-EWOULDBLOCK || result==-EAGAIN ? 0 : 203);
    }
    int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    char directory[1400]; join(directory,sizeof(directory),canonical,".bb-reward-receipts"); DIR *dir=opendir(directory); assert(dir);
    char receipt[1600]={0}; for(struct dirent *e;(e=readdir(dir));) if(e->d_name[0]!='.') join(receipt,sizeof(receipt),directory,e->d_name);
    closedir(dir); assert(*receipt); assert(!truncate(receipt,8)); assert(lookup(0)==-EBADMSG);
    puts("save transaction: concurrent profile lock and malformed receipt refusal: ok");
}
static void preserved_links_are_read_only(void) {
    setup();
    char link_path[1400],target[1400]; join(link_path,sizeof(link_path),canonical,"linked-save"); join(target,sizeof(target),canonical,"userdata0000");
    assert(!symlink(target,link_path)); start_write();
    char staged_link[1400],staged_image[1400]; join(staged_link,sizeof(staged_link),mounted,"linked-save"); join(staged_image,sizeof(staged_image),mounted,"userdata0000");
    assert(runtime_save_txn_write_allowed(staged_link)==-ELOOP);
    assert(!runtime_save_txn_write_allowed(staged_image));
    assert(runtime_save_txn_write_allowed(target)==-EROFS);
    assert(!runtime_save_txn_umount(mounted,1));
    struct stat st; assert(!lstat(link_path,&st) && S_ISLNK(st.st_mode));
    puts("save transaction: preserved symlinks cannot redirect a staged write: ok");
}
static void receipt_limit_does_not_pin_saves(void) {
    setup();
    char directory[1400]; join(directory,sizeof(directory),canonical,".bb-reward-receipts");
    assert(!mkdir(directory,0700));
    for(unsigned i=0;i<4096;++i) {
        char path[1500]; assert(snprintf(path,sizeof(path),"%s/capacity-%04u.receipt",directory,i)>0);
        int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0); assert(!close(fd));
    }
    assert(runtime_reward_begin(scope,grant,payload,sizeof(payload),0)==-ENOSPC);
    assert(runtime_reward_arm(grant,0)==-EINVAL); /* Begin refused before any reward or pin. */
    assert(!finish('L')); assert(marker(canonical,"userdata0000")=='L'); assert(!lookup(0));
    assert_kept_extra();
    puts("save transaction: full receipt ledger refuses awards without pinning native saves: ok");
}
int main(void) {
    normal_and_reward(); bad_images(); profile_and_cancel();
    injected_failure("before_flush",0); injected_failure("before_swap",0); injected_failure("after_swap",1);
    puts("save transaction: flush/exchange errors preserve a complete save and receipt pair: ok");
    crash(NULL,0,1); crash_mid_write(); crash("before_flush",0,0); crash("before_swap",0,0); crash("after_swap",1,0);
    puts("save transaction: SIGKILL before award/save, during save, before and after exchange, replay: ok");
    corruption_and_lock(); preserved_links_are_read_only(); receipt_limit_does_not_pin_saves(); cleanup();
    puts("save transaction: all tests passed"); return 0;
}
