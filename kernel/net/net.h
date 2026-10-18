#ifndef KAIRO_NET_H
#define KAIRO_NET_H
#include <kernel.h>
#define NET_MAX_CARDS 4
#define NET_MTU 1500
#define NET_IP_MAX 65535
#define NET_RX_LIMIT 65536
#define NET_TCP_MSS 1460
#define NET_TCP_WINDOW 32768
#define NET_TCP_FIN 0x01
#define NET_TCP_SYN 0x02
#define NET_TCP_RST 0x04
#define NET_TCP_PSH 0x08
#define NET_TCP_ACK 0x10
enum dhcp_state {
    DHCP_DISABLED,
    DHCP_SELECTING,
    DHCP_REQUESTING,
    DHCP_PROBING,
    DHCP_BOUND,
    DHCP_RENEWING,
    DHCP_REBINDING
};
struct dhcp_client {
    bool enabled;
    enum dhcp_state state;
    uint32_t transaction, offered, server, mask, gateway, dns[2];
    unsigned attempts;
    uint64_t started, next, renew, rebind, expires;
};
struct net_card {
    uint8_t mac[6];
    bool link, up;
    uint32_t address, netmask, gateway, dns[2];
    struct dhcp_client dhcp;
    unsigned index;
    int (*transmit)(struct net_card *, const void *, size_t);
    void (*poll)(struct net_card *);
    void *driver;
};
struct net_packet {
    struct net_packet *next;
    uint32_t address;
    uint16_t port, length, offset;
    uint8_t data[];
};
struct tcp_segment {
    struct tcp_segment *next;
    uint32_t sequence;
    uint16_t length;
    uint8_t flags, retries;
    uint64_t sent;
    uint8_t data[];
};
enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};
struct net_socket {
    struct net_socket *next, *parent, *accept_next, *accept_head, *accept_tail;
    unsigned type, protocol;
    enum tcp_state state;
    uint32_t local_address, remote_address;
    uint16_t local_port, remote_port;
    bool bound, connected, connecting, read_closed, write_closed, peer_closed, orphan;
    int error;
    unsigned backlog, children;
    size_t receive_bytes;
    struct net_packet *receive_head, *receive_tail;
    struct tcp_segment *transmit_head, *transmit_tail, *reorder_head;
    uint32_t send_unacknowledged, send_next, send_highest, receive_next;
    uint32_t peer_window, congestion_window, slow_threshold;
    unsigned duplicate_acks;
    uint16_t mss;
    uint64_t deadline, retransmit_timeout, last_activity;
};
extern struct net_socket *net_sockets;
uint16_t net_read16(const void *);
uint32_t net_read32(const void *);
void net_write16(void *, uint16_t);
void net_write32(void *, uint32_t);
uint16_t net_checksum(const void *, size_t);
uint16_t net_transport_checksum(uint32_t, uint32_t, unsigned, const void *, size_t);
uint32_t net_random(void);
void net_lock(void);
void net_unlock(void);
void dhcp_start(struct net_card *);
void dhcp_stop(struct net_card *, bool);
void dhcp_tick(struct net_card *, uint64_t);
void dhcp_receive(struct net_card *, uint32_t, const uint8_t *, size_t);
void dhcp_conflict(struct net_card *, uint32_t, uint32_t, const uint8_t *);
int net_dhcp_send(struct net_card *, uint32_t, uint32_t, const void *, size_t);
void net_set_address(struct net_card *, uint32_t, uint32_t, uint32_t, const uint32_t *);
int net_arp_probe(struct net_card *, uint32_t);
void net_arp_announce(struct net_card *);
int net_register(struct net_card *);
void net_receive(struct net_card *, const void *, size_t);
int net_ipv4_send(uint32_t, uint32_t, unsigned, const void *, size_t, bool);
uint32_t net_source(uint32_t);
int net_configure(struct task *, uint64_t, bool);
void net_transport_receive(uint32_t, uint32_t, unsigned, const uint8_t *, size_t);
void net_socket_tick(uint64_t);
void net_socket_error(uint32_t, uint32_t, unsigned, uint16_t, uint16_t, int);
int net_queue_packet(struct net_socket *, uint32_t, uint16_t, const void *, size_t);
void net_socket_free(struct net_socket *);
struct net_socket *net_tcp_child(struct net_socket *);
void net_udp_unreachable(uint32_t, uint32_t, const void *, size_t);
void tcp_receive(uint32_t, uint32_t, const uint8_t *, size_t);
void tcp_tick(struct net_socket *, uint64_t);
int tcp_connect(struct net_socket *);
int tcp_send(struct net_socket *, const void *, size_t);
int tcp_shutdown(struct net_socket *);
void tcp_abort(struct net_socket *, int, bool);
void tcp_acknowledge(struct net_socket *);
#endif
