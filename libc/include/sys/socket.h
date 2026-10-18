#ifndef KAIRO_SYS_SOCKET_H
#define KAIRO_SYS_SOCKET_H
#include <libkario.h>
typedef uint32_t socklen_t;
int socket(int, int, int);
int bind(int, const struct sockaddr *, socklen_t);
int connect(int, const struct sockaddr *, socklen_t);
int listen(int, int);
int accept(int, struct sockaddr *, socklen_t *);
ssize_t send(int, const void *, size_t, int);
ssize_t recv(int, void *, size_t, int);
ssize_t sendto(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t recvfrom(int, void *, size_t, int, struct sockaddr *, socklen_t *);
int shutdown(int, int);
int getsockname(int, struct sockaddr *, socklen_t *);
int getpeername(int, struct sockaddr *, socklen_t *);
int getsockopt(int, int, int, void *, socklen_t *);
int netconfig(struct knetconfig *, int);
#endif
