#ifndef KAIRO_ARPA_INET_H
#define KAIRO_ARPA_INET_H
#include <netinet/in.h>
#define INET_ADDRSTRLEN 16
int inet_pton(int, const char *, void *);
const char *inet_ntop(int, const void *, char *, socklen_t);
in_addr_t inet_addr(const char *);
char *inet_ntoa(struct in_addr);
#endif
