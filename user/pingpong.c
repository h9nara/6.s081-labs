#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    int to_child[2];
    int to_parent[2];

    pipe(to_child);
    pipe(to_parent);
    if (fork() == 0) {
        // in child process.
        close(to_child[1]);
        close(to_parent[0]);
        char b[1];
        if (read(to_child[0], b, 1) < 0) {
            fprintf(2, "failed to read from parent\n");
            exit(1);
        }
        printf("%d: received ping\n", getpid());
        if (write(to_parent[1], b, 1) < 0) {
            fprintf(2, "failed to write to parent\n");
            exit(1);
        }
        close(to_parent[1]);
    } else {
        // in parent process.
        close(to_child[0]);
        close(to_parent[1]);
        char b[1] = {'a'};
        if (write(to_child[1], b, 1) < 0) {
            fprintf(2, "failed to write to child\n");
            exit(1);
        }
        close(to_child[1]);
        if (read(to_parent[0], b, 1) < 0) {
            fprintf(2, "failed to read from child\n");
            exit(1);
        }
        printf("%d: received pong\n", getpid());
    }
    exit(0);
}