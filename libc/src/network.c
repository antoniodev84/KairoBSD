#include <arpa/inet.h>
extern int64_t __kario_call(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define NET_CALL(n, a, b, c, d, e, f)                                                              \
    __kario_call(SYS_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d),              \
                 (uint64_t)(e), (uint64_t)(f))
int socket(int family, int type, int protocol)
{
    return NET_CALL(socket, family, type, protocol, 0, 0, 0);
}
int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    return NET_CALL(bind, fd, address, length, 0, 0, 0);
}
int connect(int fd, const struct sockaddr *address, socklen_t length)
{
    return NET_CALL(connect, fd, address, length, 0, 0, 0);
}
int listen(int fd, int backlog)
{
    if (backlog < 0)
        backlog = 0;
    return NET_CALL(listen, fd, backlog, 0, 0, 0, 0);
}
int accept(int fd, struct sockaddr *address, socklen_t *length)
{
    return NET_CALL(accept, fd, address, length, 0, 0, 0);
}
ssize_t sendto(int fd, const void *data, size_t length, int flags, const struct sockaddr *address,
               socklen_t address_length)
{
    return NET_CALL(sendto, fd, data, length, flags, address, address_length);
}
ssize_t recvfrom(int fd, void *data, size_t length, int flags, struct sockaddr *address,
                 socklen_t *address_length)
{
    return NET_CALL(recvfrom, fd, data, length, flags, address, address_length);
}
ssize_t send(int fd, const void *data, size_t length, int flags)
{
    return sendto(fd, data, length, flags, NULL, 0);
}
ssize_t recv(int fd, void *data, size_t length, int flags)
{
    return recvfrom(fd, data, length, flags, NULL, NULL);
}
int shutdown(int fd, int how)
{
    return NET_CALL(shutdown, fd, how, 0, 0, 0, 0);
}
int getsockname(int fd, struct sockaddr *address, socklen_t *length)
{
    return NET_CALL(getsockname, fd, address, length, 0, 0, 0);
}
int getpeername(int fd, struct sockaddr *address, socklen_t *length)
{
    return NET_CALL(getpeername, fd, address, length, 0, 0, 0);
}
int getsockopt(int fd, int level, int option, void *value, socklen_t *length)
{
    return NET_CALL(getsockopt, fd, level, option, value, length, 0);
}
int netconfig(struct knetconfig *config, int set)
{
    return NET_CALL(netconfig, config, set, 0, 0, 0, 0);
}
uint16_t htons(uint16_t value)
{
    return __builtin_bswap16(value);
}
uint16_t ntohs(uint16_t value)
{
    return __builtin_bswap16(value);
}
uint32_t htonl(uint32_t value)
{
    return __builtin_bswap32(value);
}
uint32_t ntohl(uint32_t value)
{
    return __builtin_bswap32(value);
}
int inet_pton(int family, const char *text, void *output)
{
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    if (!text || !output) {
        errno = EFAULT;
        return -1;
    }
    uint8_t bytes[4];
    for (unsigned i = 0; i < 4; i++) {
        unsigned value = 0, digits = 0;
        while (*text >= '0' && *text <= '9') {
            if (++digits > 3)
                return 0;
            value = value * 10 + *text++ - '0';
        }
        if (!digits || value > 255 || (i < 3 ? *text != '.' : *text != 0))
            return 0;
        bytes[i] = value;
        if (i < 3)
            text++;
    }
    memcpy(output, bytes, sizeof(bytes));
    return 1;
}
const char *inet_ntop(int family, const void *address, char *output, socklen_t length)
{
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if (!address || !output) {
        errno = EFAULT;
        return NULL;
    }
    const uint8_t *bytes = address;
    char text[INET_ADDRSTRLEN];
    int count = snprintf(text, sizeof(text), "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
    if (length <= (unsigned)count) {
        errno = ENOSPC;
        return NULL;
    }
    memcpy(output, text, count + 1);
    return output;
}
in_addr_t inet_addr(const char *text)
{
    struct in_addr address;
    return inet_pton(AF_INET, text, &address) == 1 ? address.s_addr : INADDR_NONE;
}
char *inet_ntoa(struct in_addr address)
{
    static char text[INET_ADDRSTRLEN];
    return (char *)inet_ntop(AF_INET, &address, text, sizeof(text));
}
