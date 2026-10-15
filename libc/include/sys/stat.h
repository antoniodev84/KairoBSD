#ifndef _SYS_STAT_H
#define _SYS_STAT_H
#include <libkario.h>
struct stat {
    unsigned long st_ino;
    long st_size;
    unsigned long st_blocks;
    unsigned st_mode, st_nlink, st_uid, st_gid;
    long st_atime, st_mtime, st_ctime;
};
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
int stat(const char *, struct stat *);
#endif
