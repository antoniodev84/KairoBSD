#include <libkario.h>
#define STAGES 8
#define JOBS 8
struct command {
    char *argv[33];
    unsigned argc;
    char *input, *output;
    bool append;
};
struct job {
    int group;
    pid_t pids[STAGES];
    unsigned count;
    bool stopped;
    char command[128];
};
static struct job jobs[JOBS];
static volatile sig_atomic_t interrupted;
static int shell_group, shell_pid;
static void on_interrupt(int signo)
{
    int saved = errno;
    interrupted = 1;
    write(1, "\n", 1);
    errno = saved;
}
static int line_read(char *line, size_t capacity)
{
    size_t n = 0;
    bool overflow = false;
    interrupted = 0;
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            if (errno == EINTR || interrupted) {
                clearerr(stdin);
                line[0] = 0;
                return 0;
            }
            return -1;
        }
        if (c == '\r' || c == '\n') {
            putchar('\n');
            line[n] = 0;
            return overflow ? -2 : 0;
        }
        if (c == 4 && !n)
            return -1;
        if (c == 8 || c == 127) {
            if (n) {
                n--;
                fputs("\b \b", stdout);
            }
            continue;
        }
        if (c >= 32 && c < 127) {
            if (n + 1 < capacity) {
                line[n++] = c;
                putchar(c);
            } else
                overflow = true;
        }
    }
}
static int parse(const char *source, char *storage, struct command *commands, bool *background)
{
    unsigned stage = 0;
    char *out = storage;
    memset(commands, 0, sizeof(*commands) * STAGES);
    *background = false;
    while (*source) {
        while (*source == ' ' || *source == '\t')
            source++;
        if (!*source)
            break;
        if (*source == '|') {
            if (!commands[stage].argc || ++stage == STAGES)
                return -1;
            source++;
            continue;
        }
        if (*source == '&') {
            source++;
            while (*source == ' ' || *source == '\t')
                source++;
            if (*source)
                return -1;
            *background = true;
            break;
        }
        int redirection = 0;
        bool append = false;
        if (*source == '<' || *source == '>') {
            redirection = *source++;
            if (redirection == '>' && *source == '>') {
                append = true;
                source++;
            }
            while (*source == ' ' || *source == '\t')
                source++;
        }
        if (!*source || *source == '|' || *source == '&' || *source == '<' || *source == '>')
            return -1;
        char *word = out;
        char quote = 0;
        while (*source) {
            char c = *source;
            if (!quote && (c == ' ' || c == '\t' || c == '|' || c == '&' || c == '<' || c == '>'))
                break;
            source++;
            if (!quote && (c == '\'' || c == '"')) {
                quote = c;
                continue;
            }
            if (quote && c == quote) {
                quote = 0;
                continue;
            }
            if (c == '\\' && quote != '\'') {
                if (!*source)
                    return -1;
                c = *source++;
            }
            *out++ = c;
        }
        if (quote)
            return -1;
        *out++ = 0;
        struct command *command = &commands[stage];
        if (redirection == '<')
            command->input = word;
        else if (redirection == '>') {
            command->output = word;
            command->append = append;
        } else {
            if (command->argc == 32)
                return -1;
            command->argv[command->argc++] = word;
        }
    }
    return commands[stage].argc ? (int)stage + 1 : stage ? -1 : 0;
}
static bool alive(struct job *job)
{
    for (unsigned i = 0; i < job->count; i++)
        if (job->pids[i])
            return true;
    return false;
}
static void reap_jobs(void)
{
    for (unsigned i = 0; i < JOBS; i++)
        if (jobs[i].group) {
            for (unsigned n = 0; n < jobs[i].count; n++)
                if (jobs[i].pids[n]) {
                    int status;
                    pid_t p = waitpid(jobs[i].pids[n], &status, WNOHANG | WUNTRACED | WCONTINUED);
                    if (p > 0) {
                        if (WIFSTOPPED(status))
                            jobs[i].stopped = true;
                        else if (WIFCONTINUED(status))
                            jobs[i].stopped = false;
                        else
                            jobs[i].pids[n] = 0;
                    } else if (p < 0 && errno == ECHILD)
                        jobs[i].pids[n] = 0;
                }
            if (!alive(&jobs[i])) {
                printf("[%u] Done %s\n", i + 1, jobs[i].command);
                memset(&jobs[i], 0, sizeof(jobs[i]));
            }
        }
}
static int foreground(struct job *job, unsigned index, bool resume)
{
    if (tcsetpgrp(0, job->group)) {
        perror("sh foreground");
        return 1;
    }
    if (resume && kill(-job->group, SIGCONT))
        perror("fg");
    job->stopped = false;
    int result = 0;
    for (unsigned i = 0; i < job->count; i++)
        if (job->pids[i]) {
            int status;
            pid_t p;
            do {
                p = waitpid(job->pids[i], &status, WUNTRACED);
            } while (p < 0 && errno == EINTR);
            if (p < 0) {
                if (errno != ECHILD)
                    perror("sh waitpid");
                job->pids[i] = 0;
                result = 1;
                continue;
            }
            if (WIFSTOPPED(status)) {
                job->stopped = true;
                result = 128 + WSTOPSIG(status);
            } else {
                job->pids[i] = 0;
                if (i + 1 == job->count)
                    result = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
            }
        }
    if (tcsetpgrp(0, shell_group))
        perror("sh restore terminal");
    if (job->stopped)
        printf("[%u] Stopped %s\n", index + 1, job->command);
    else
        memset(job, 0, sizeof(*job));
    return result;
}
static int builtin(struct command *command, int last, bool *handled)
{
    char **argv = command->argv;
    unsigned argc = command->argc;
    *handled = true;
    if (!strcmp(argv[0], "exit"))
        exit(argc > 1 ? atoi(argv[1]) : last);
    if (!strcmp(argv[0], "cd")) {
        if (chdir(argc > 1 ? argv[1] : "/")) {
            perror("cd");
            return 1;
        }
        return 0;
    }
    if (!strcmp(argv[0], "help")) {
        puts("Builtins: cd exit help jobs fg bg\nPipelines: command | command; background: "
             "&\nRedirection: < > >>; quoted arguments and escapes.\nSignals: Ctrl-C, Ctrl-Z, kill "
             "[-SIGNAL] pid or -process-group.");
        return 0;
    }
    if (!strcmp(argv[0], "jobs")) {
        reap_jobs();
        for (unsigned i = 0; i < JOBS; i++)
            if (jobs[i].group)
                printf("[%u] %s %d %s\n", i + 1, jobs[i].stopped ? "Stopped" : "Running",
                       jobs[i].group, jobs[i].command);
        return 0;
    }
    if (!strcmp(argv[0], "fg") || !strcmp(argv[0], "bg")) {
        if (getpid() != shell_pid) {
            fprintf(stderr, "sh: job control unavailable in pipeline child\n");
            return 1;
        }
        int index = argc > 1 ? atoi(argv[1] + (argv[1][0] == '%')) - 1 : -1;
        if (argc == 1)
            for (unsigned i = 0; i < JOBS; i++)
                if (jobs[i].group)
                    index = i;
        if (index < 0 || index >= JOBS || !jobs[index].group) {
            fprintf(stderr, "sh: no such job\n");
            return 1;
        }
        if (argv[0][0] == 'f')
            return foreground(&jobs[index], index, true);
        if (kill(-jobs[index].group, SIGCONT)) {
            perror("bg");
            return 1;
        }
        jobs[index].stopped = false;
        return 0;
    }
    *handled = false;
    return 0;
}
static void redirect(const struct command *command)
{
    if (command->input) {
        int fd = open(command->input, O_RDONLY);
        if (fd < 0 || dup2(fd, 0) < 0) {
            perror(command->input);
            exit(1);
        }
        if (fd != 0)
            close(fd);
    }
    if (command->output) {
        int fd = open(command->output, O_WRONLY | O_CREAT | (command->append ? O_APPEND : O_TRUNC),
                      0666U);
        if (fd < 0 || dup2(fd, 1) < 0) {
            perror(command->output);
            exit(1);
        }
        if (fd != 1)
            close(fd);
    }
}
static int launch(struct command *commands, unsigned count, bool background, const char *line,
                  int last)
{
    unsigned slot;
    for (slot = 0; slot < JOBS; slot++)
        if (!jobs[slot].group)
            break;
    if (slot == JOBS) {
        fprintf(stderr, "sh: job table full\n");
        return 1;
    }
    int channels[STAGES - 1][2], gate[2];
    unsigned made = 0;
    for (; made + 1 < count; made++)
        if (pipe(channels[made]))
            break;
    if (made + 1 < count || pipe(gate)) {
        for (unsigned i = 0; i < made; i++) {
            close(channels[i][0]);
            close(channels[i][1]);
        }
        perror("sh pipe");
        return 1;
    }
    sigset_t blocked = 0, old;
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGQUIT);
    sigaddset(&blocked, SIGTSTP);
    sigprocmask(SIG_BLOCK, &blocked, &old);
    struct job *job = &jobs[slot];
    snprintf(job->command, sizeof(job->command), "%s", line);
    bool failure = false;
    for (unsigned i = 0; i < count; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("sh fork");
            failure = true;
            break;
        }
        if (!pid) {
            if (setpgid(0, job->group)) {
                perror("sh child group");
                exit(1);
            }
            close(gate[1]);
            if (i && dup2(channels[i - 1][0], 0) < 0)
                exit(1);
            if (i + 1 < count && dup2(channels[i][1], 1) < 0)
                exit(1);
            for (unsigned k = 0; k < made; k++) {
                close(channels[k][0]);
                close(channels[k][1]);
            }
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGPIPE, SIG_DFL);
            char byte;
            while (read(gate[0], &byte, 1) < 0 && errno == EINTR) {
            }
            close(gate[0]);
            sigprocmask(SIG_SETMASK, &old, NULL);
            redirect(&commands[i]);
            bool handled;
            int result = builtin(&commands[i], last, &handled);
            if (handled)
                exit(result);
            execvp(commands[i].argv[0], commands[i].argv);
            perror(commands[i].argv[0]);
            exit(127);
        }
        if (!job->group)
            job->group = pid;
        job->pids[job->count++] = pid;
        if (setpgid(pid, job->group) && getpgid(pid) != job->group) {
            perror("sh parent group");
            failure = true;
            break;
        }
    }
    for (unsigned i = 0; i < made; i++) {
        close(channels[i][0]);
        close(channels[i][1]);
    }
    close(gate[0]);
    if (!background && job->group)
        tcsetpgrp(0, job->group);
    close(gate[1]);
    sigprocmask(SIG_SETMASK, &old, NULL);
    if (failure) {
        if (job->group) {
            kill(-job->group, SIGKILL);
            foreground(job, slot, false);
        }
        return 1;
    }
    if (background) {
        printf("[%u] %d\n", slot + 1, job->group);
        return 0;
    }
    return foreground(job, slot, false);
}
int main(void)
{
    shell_pid = getpid();
    if (setpgid(0, 0))
        perror("sh group");
    shell_group = getpgrp();
    if (tcsetpgrp(0, shell_group))
        perror("sh terminal");
    struct sigaction interrupt_action = {.sa_handler = on_interrupt};
    sigaction(SIGINT, &interrupt_action, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    char line[1024], storage[1024], cwd[PATH_MAX];
    int last = 0;
    for (;;) {
        reap_jobs();
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "?");
        printf("root@kario:%s# ", cwd);
        int result = line_read(line, sizeof(line));
        if (result == -1)
            return last;
        if (result == -2) {
            fprintf(stderr, "sh: line too long\n");
            continue;
        }
        struct command commands[STAGES];
        bool background;
        int count = parse(line, storage, commands, &background);
        if (count < 0) {
            fprintf(stderr, "sh: syntax error\n");
            continue;
        }
        if (!count)
            continue;
        const char *name = commands[0].argv[0];
        bool internal = !strcmp(name, "cd") || !strcmp(name, "exit") || !strcmp(name, "help") ||
                        !strcmp(name, "jobs") || !strcmp(name, "fg") || !strcmp(name, "bg");
        if (count == 1 && !background && internal) {
            int saved_input = -1, saved_output = -1;
            if (commands[0].input || commands[0].output) {
                int pair[2];
                if (pipe(pair)) {
                    perror("sh descriptors");
                    continue;
                }
                saved_input = pair[0];
                saved_output = pair[1];
                if (dup2(0, saved_input) < 0 || dup2(1, saved_output) < 0) {
                    close(saved_input);
                    close(saved_output);
                    continue;
                }
                if (commands[0].input) {
                    int fd = open(commands[0].input, O_RDONLY);
                    if (fd < 0) {
                        perror(commands[0].input);
                        close(saved_input);
                        close(saved_output);
                        continue;
                    }
                    dup2(fd, 0);
                    close(fd);
                }
                if (commands[0].output) {
                    int fd =
                        open(commands[0].output,
                             O_WRONLY | O_CREAT | (commands[0].append ? O_APPEND : O_TRUNC), 0666U);
                    if (fd < 0) {
                        perror(commands[0].output);
                        dup2(saved_input, 0);
                        close(saved_input);
                        close(saved_output);
                        continue;
                    }
                    dup2(fd, 1);
                    close(fd);
                }
            }
            bool handled;
            last = builtin(&commands[0], last, &handled);
            if (saved_input >= 0) {
                dup2(saved_input, 0);
                dup2(saved_output, 1);
                close(saved_input);
                close(saved_output);
            }
            if (handled)
                continue;
        }
        last = launch(commands, count, background, line, last);
    }
}
