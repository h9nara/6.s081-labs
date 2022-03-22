#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void spawn(int rfd) {
    int pr, x;
    int p[2];
    
    if (read(rfd, &pr, sizeof(int)) > 0) {
        printf("prime %d\n", pr);
    } else {
        exit(0);
    }
    pipe(p);
    if (fork() == 0) {
        // in child process.
        close(p[1]);
        spawn(p[0]);
    } else {
        // in parent process.
        close(p[0]);
        while (read(rfd, &x, sizeof(int)) > 0) {
            if (x % pr == 0) {
                continue;
            }
            write(p[1], &x, sizeof(int)); 
        }
        close(p[1]);
        wait(0);
    }
    exit(0);
}

int main(int argc, char *argv[]) {
    int p[2];

    pipe(p);
    if (fork() != 0) {
        // close the read end in parent process.
        close(p[0]);
        for (int i = 2; i <= 35; i++) {
            if (write(p[1], &i, sizeof(int)) < 0) {
                fprintf(2, "failed to give input to child process\n");
                exit(1);
            }
        }
        // finished sending input, close the write end of the pipe.
        close(p[1]);
        wait(0); 
    } else {
        // in child process.
        close(p[1]);
        spawn(p[0]);
    }
    exit(0);
}
