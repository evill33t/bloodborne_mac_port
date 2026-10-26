/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static char folder[]="/tmp/bb-coop-trophy-XXXXXX";
const char *runtime_file_user_dir(void) {return folder;}
int main(void) {
    assert(mkdtemp(folder));
    assert((uint32_t)runtime_coop_record_trophy(-1)==0x8055160a);
    assert((uint32_t)runtime_coop_record_trophy(40)==0x8055160a);
    assert(!runtime_coop_record_trophy(5));
    assert((uint32_t)runtime_coop_record_trophy(5)==0x8055160c);
    char path[256];snprintf(path,sizeof(path),"%s/trophies.log",folder);
    struct stat info;assert(!stat(path,&info) && (info.st_mode&0777)==0600);
    FILE *file=fopen(path,"r");int id;long long when;
    assert(file && fscanf(file,"%d %lld",&id,&when)==2 && id==5 && when>0);
    assert(fscanf(file,"%d",&id)==EOF);fclose(file);
    unlink(path);assert(!symlink("must-not-create",path));
    assert((uint32_t)runtime_coop_record_trophy(6)==0x80551604);
    unlink(path);rmdir(folder);
    puts("PASS: shared trophy persistence, duplicate admission, private file and symlink rejection");
}
