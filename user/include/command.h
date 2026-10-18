#ifndef KAIRO_COMMAND_H
#define KAIRO_COMMAND_H
enum {
    KAIRO_CMD_echo,
    KAIRO_CMD_cat,
    KAIRO_CMD_ls,
    KAIRO_CMD_pwd,
    KAIRO_CMD_mkdir,
    KAIRO_CMD_rm,
    KAIRO_CMD_uname,
    KAIRO_CMD_meminfo,
    KAIRO_CMD_uptime,
    KAIRO_CMD_ln,
    KAIRO_CMD_chmod,
    KAIRO_CMD_mount,
    KAIRO_CMD_stat,
    KAIRO_CMD_cp,
    KAIRO_CMD_sleep,
    KAIRO_CMD_true,
    KAIRO_CMD_false,
    KAIRO_CMD_partscan,
    KAIRO_CMD_kill,
    KAIRO_CMD_chroot,
    KAIRO_CMD_ifconfig,
    KAIRO_CMD_ping,
    KAIRO_CMD_nc,
    KAIRO_CMD_nslookup,
    KAIRO_CMD_wget,
    KAIRO_CMD_httpd,
    KAIRO_CMD_as,
    KAIRO_CMD_ld,
};
#ifndef KAIRO_APP
#error KAIRO_APP must identify the command being built
#endif
#define command_is(name) (KAIRO_APP == KAIRO_CMD_##name)
#endif
