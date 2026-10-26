/* SPDX-License-Identifier: GPL-2.0-or-later
 * The shared trophy board must have a durable local recipient. Use the port's
 * existing trophies.log format (id, Unix time), without re-publishing received unlocks. */
#include "runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int32_t runtime_coop_record_trophy(int32_t id) {
    if(id<0 || id>=40) return (int32_t)0x8055160a;
    char path[1200];
    if(snprintf(path,sizeof(path),"%s/trophies.log",runtime_file_user_dir())>=(int)sizeof(path))
        return (int32_t)0x80551604;
    int fd=open(path,O_RDWR|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0) goto failed;
    struct stat info;
    if(fstat(fd,&info) || !S_ISREG(info.st_mode) || flock(fd,LOCK_EX)) {close(fd);goto failed;}
    FILE *file=fdopen(fd,"a+");
    if(!file) {close(fd);goto failed;}
    rewind(file);
    char line[128];int previous;long long timestamp;
    while(fgets(line,sizeof(line),file)) {
        if(sscanf(line,"%d %lld",&previous,&timestamp)==2 && previous==id && timestamp>0) {
            fclose(file);return (int32_t)0x8055160c;
        }
    }
    if(ferror(file) || fseek(file,0,SEEK_END) || fprintf(file,"%d %lld\n",id,(long long)time(NULL))<=0 ||
       fflush(file) || fsync(fd)) {fclose(file);goto failed;}
    if(fclose(file)) goto failed;
    printf("Runtime: trophy %d unlocked\n",id);
    return 0;
failed:
    fprintf(stderr,"Co-op: could not record trophy %d in %s: %s\n",id,path,strerror(errno));
    return (int32_t)0x80551604;
}
