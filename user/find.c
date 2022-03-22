#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

char *fmtname(char *path) {
    char *p;
    for(p = path + strlen(path); p >= path && (*p) != '/'; --p);
    return p + 1;
}

void find(char *path, char *name) {
    int fd;
    struct stat st;
    if ((fd = open(path, 0)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return;
    }
    if(fstat(fd, &st) < 0){
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return;
    }

    struct dirent de;
    char buf[512], *p;
    switch(st.type) {
    case T_FILE:
        if (strcmp(fmtname(path), name) == 0) {
            printf("%s\n", path);
        }
        break;
    case T_DIR:
        strcpy(buf, path);
        p = buf + strlen(path);
        *p++ = '/';
        while (read(fd, &de, sizeof(de)) == sizeof(de)) {
            if (de.inum == 0 || strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) {
                continue;
            }
            memmove(p, de.name, DIRSIZ);
            *(p + DIRSIZ) = 0;
            find(buf, name);
        } 
        break;
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(2, "Usage: find dir filename\n");
    }
    find(argv[1], argv[2]);
    exit(0);
}