#ifndef KAIRO_NETINET_IN_H
#define KAIRO_NETINET_IN_H
#include <sys/socket.h>
#define INADDR_ANY 0U
#define INADDR_LOOPBACK 0x7f000001U
#define INADDR_BROADCAST 0xffffffffU
#define INADDR_NONE 0xffffffffU
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
uint16_t htons(uint16_t);
uint16_t ntohs(uint16_t);
uint32_t htonl(uint32_t);
uint32_t ntohl(uint32_t);
#endif
