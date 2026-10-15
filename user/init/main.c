#include <libkario.h>
int main(void)
{
    puts("\nWelcome to KarioBSD!");
    for (;;) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("init fork");
            struct ktimespec delay = {1, 0};
            nanosleep(&delay, NULL);
            continue;
        }
        if (!pid) {
            char *args[] = {"/bin/sh", NULL};
            execve(args[0], args, environ);
            perror("init exec /bin/sh");
            exit(127);
        }
        int status;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                perror("init waitpid");
                break;
            }
        }
        fprintf(stderr, "init: shell exited (%d), restarting\n", WEXITSTATUS(status));
    }
}
