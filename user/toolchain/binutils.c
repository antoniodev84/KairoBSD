#include "../include/command.h"
#include <libkario.h>
#include "tcc.h"
int main(int argc, char **argv)
{
    const char *name = KAIRO_APP_NAME;
    bool assembler = command_is(as);
    const char *output = assembler ? "a.o" : "a.out";
    TCCState *s = tcc_new();
    if (!s) {
        perror(name);
        return 1;
    }
    s->nostdlib = 1;
    s->nostdinc = 1;
    s->static_link = 1;
    s->has_text_addr = 1;
    s->text_addr = 0x400000;
    int result = 1, inputs = 0;
    if (tcc_set_output_type(s, assembler ? TCC_OUTPUT_OBJ : TCC_OUTPUT_EXE) < 0)
        goto done;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-o") && i + 1 < argc) {
            output = argv[++i];
            continue;
        }
        if (!strcmp(arg, "-c") && assembler)
            continue;
        if (!strcmp(arg, "-static") && !assembler)
            continue;
        if (!strcmp(arg, "-L") && i + 1 < argc) {
            tcc_add_library_path(s, argv[++i]);
            continue;
        }
        if (!strncmp(arg, "-L", 2) && arg[2]) {
            tcc_add_library_path(s, arg + 2);
            continue;
        }
        if (!strncmp(arg, "-l", 2) && arg[2] && !assembler) {
            if (tcc_add_library(s, arg + 2) < 0)
                goto done;
            inputs++;
            continue;
        }
        if (*arg == '-') {
            fprintf(stderr, "%s: unsupported option: %s\n", name, arg);
            goto done;
        }
        s->filetype = assembler ? AFF_TYPE_ASM : AFF_TYPE_BIN;
        if (tcc_add_file(s, arg) < 0)
            goto done;
        inputs++;
    }
    if (!inputs) {
        fprintf(stderr, "%s: no input files\n", name);
        goto done;
    }
    if (tcc_output_file(s, output) < 0) {
        unlink(output);
        goto done;
    }
    if (!assembler && chmod(output, 0755) < 0) {
        perror(output);
        unlink(output);
        goto done;
    }
    result = 0;
done:
    tcc_delete(s);
    return result;
}
