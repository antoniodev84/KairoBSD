#include "net.h"
struct net_socket *net_sockets;
static struct vnode_ops socket_ops;
static bool port_available(struct net_socket *socket, uint16_t port, uint32_t address)
{
    for (struct net_socket *other = net_sockets; other; other = other->next) {
        if (other == socket || !other->bound || other->type != socket->type ||
            other->protocol != socket->protocol || other->local_port != port)
            continue;
        if (!address || !other->local_address || address == other->local_address)
            return false;
    }
    return true;
}
static int bind_socket(struct net_socket *socket, uint32_t address, uint16_t port)
{
    if (socket->bound)
        return -EINVAL;
    if (address && net_source(address) != address)
        return -EADDRNOTAVAIL;
    if (socket->type != SOCK_RAW) {
        if (port && port < 1024 && this_cpu()->current->uid)
            return -EACCES;
        if (!port) {
            unsigned first = net_random() % 16384;
            for (unsigned i = 0; i < 16384; i++) {
                uint16_t candidate = 49152 + ((first + i) % 16384);
                if (port_available(socket, candidate, address)) {
                    port = candidate;
                    break;
                }
            }
            if (!port)
                return -EADDRINUSE;
        } else if (!port_available(socket, port, address)) {
            return -EADDRINUSE;
        }
    }
    socket->local_address = address;
    socket->local_port = port;
    socket->bound = true;
    return 0;
}
static struct net_socket *socket_new(unsigned type, unsigned protocol)
{
    struct net_socket *socket = kzalloc(sizeof(*socket));
    if (!socket)
        return NULL;
    socket->type = type;
    socket->protocol = protocol;
    socket->mss = NET_TCP_MSS;
    socket->peer_window = NET_TCP_WINDOW;
    socket->congestion_window = 2 * NET_TCP_MSS;
    socket->slow_threshold = NET_TCP_WINDOW;
    socket->retransmit_timeout = 1000000000;
    socket->next = net_sockets;
    net_sockets = socket;
    return socket;
}
struct net_socket *net_tcp_child(struct net_socket *parent)
{
    if (parent->children >= parent->backlog)
        return NULL;
    struct net_socket *socket = socket_new(SOCK_STREAM, IPPROTO_TCP);
    if (socket) {
        socket->parent = parent;
        socket->bound = true;
        socket->local_port = parent->local_port;
        parent->children++;
    }
    return socket;
}
static void free_packets(struct net_socket *socket)
{
    while (socket->receive_head) {
        struct net_packet *packet = socket->receive_head;
        socket->receive_head = packet->next;
        kfree(packet);
    }
    socket->receive_tail = NULL;
    socket->receive_bytes = 0;
}
void net_socket_free(struct net_socket *socket)
{
    struct net_socket **link = &net_sockets;
    while (*link && *link != socket)
        link = &(*link)->next;
    if (*link)
        *link = socket->next;
    if (socket->parent) {
        struct net_socket *parent = socket->parent;
        struct net_socket **child = &parent->accept_head;
        parent->accept_tail = NULL;
        while (*child) {
            if (*child == socket)
                *child = socket->accept_next;
            else {
                parent->accept_tail = *child;
                child = &(*child)->accept_next;
            }
        }
        if (parent->children)
            parent->children--;
    }
    free_packets(socket);
    struct tcp_segment *segment = socket->transmit_head;
    while (segment) {
        struct tcp_segment *next = segment->next;
        kfree(segment);
        segment = next;
    }
    segment = socket->reorder_head;
    while (segment) {
        struct tcp_segment *next = segment->next;
        kfree(segment);
        segment = next;
    }
    kfree(socket);
}
int net_queue_packet(struct net_socket *socket, uint32_t address, uint16_t port, const void *data,
                     size_t length)
{
    if (length > NET_RX_LIMIT - socket->receive_bytes || socket->read_closed)
        return -ENOBUFS;
    unsigned count = 0;
    for (struct net_packet *p = socket->receive_head; p; p = p->next)
        if (++count >= 128)
            return -ENOBUFS;
    struct net_packet *packet = kmalloc(sizeof(*packet) + length);
    if (!packet)
        return -ENOMEM;
    packet->next = NULL;
    packet->address = address;
    packet->port = port;
    packet->offset = 0;
    packet->length = length;
    if (length)
        memcpy(packet->data, data, length);
    if (socket->receive_tail)
        socket->receive_tail->next = packet;
    else
        socket->receive_head = packet;
    socket->receive_tail = packet;
    socket->receive_bytes += length;
    return 0;
}
static int receive_socket(struct net_socket *socket, void *data, size_t length, unsigned flags,
                          struct sockaddr_in *source)
{
    if (socket->type == SOCK_STREAM && !socket->connected && !socket->peer_closed && !socket->error)
        return -ENOTCONN;
    if (socket->read_closed)
        return 0;
    if (socket->type == SOCK_STREAM && !length)
        return 0;
    struct net_packet *packet = socket->receive_head;
    if (!packet) {
        if (socket->error) {
            int error = socket->error;
            socket->error = 0;
            return -error;
        }
        return socket->peer_closed ? 0 : -EAGAIN;
    }
    if (source) {
        memset(source, 0, sizeof(*source));
        source->sin_family = AF_INET;
        source->sin_addr.s_addr = __builtin_bswap32(packet->address);
        source->sin_port = __builtin_bswap16(packet->port);
    }
    if (socket->type != SOCK_STREAM && !length && !(flags & MSG_PEEK)) {
        socket->receive_head = packet->next;
        if (!socket->receive_head)
            socket->receive_tail = NULL;
        socket->receive_bytes -= packet->length;
        kfree(packet);
        return 0;
    }
    size_t copied = 0;
    while (packet && (copied < length || (socket->type != SOCK_STREAM && !packet->length))) {
        size_t amount = packet->length - packet->offset;
        if (amount > length - copied)
            amount = length - copied;
        if (amount)
            memcpy((uint8_t *)data + copied, packet->data + packet->offset, amount);
        copied += amount;
        if (flags & MSG_PEEK) {
            if (socket->type != SOCK_STREAM)
                break;
            packet = packet->next;
            continue;
        }
        packet->offset += amount;
        socket->receive_bytes -= amount;
        if (packet->offset == packet->length || socket->type != SOCK_STREAM) {
            socket->receive_bytes -= packet->length - packet->offset;
            socket->receive_head = packet->next;
            if (!socket->receive_head)
                socket->receive_tail = NULL;
            kfree(packet);
            packet = socket->receive_head;
        }
        if (socket->type != SOCK_STREAM)
            break;
    }
    if (!(flags & MSG_PEEK) && socket->type == SOCK_STREAM && socket->state != TCP_TIME_WAIT)
        tcp_acknowledge(socket);
    return copied;
}
static int send_socket(struct net_socket *socket, const void *data, size_t length,
                       const struct sockaddr_in *target)
{
    if (socket->write_closed)
        return -EPIPE;
    if (socket->error) {
        int error = socket->error;
        socket->error = 0;
        return -error;
    }
    if (socket->type == SOCK_STREAM) {
        if (target)
            return -EISCONN;
        return tcp_send(socket, data, length);
    }
    uint32_t destination =
        target ? __builtin_bswap32(target->sin_addr.s_addr) : socket->remote_address;
    uint16_t port = target ? __builtin_bswap16(target->sin_port) : socket->remote_port;
    if (!destination)
        return -EDESTADDRREQ;
    if (socket->type == SOCK_DGRAM && !port)
        return -EINVAL;
    if (!socket->bound) {
        int result = bind_socket(socket, 0, 0);
        if (result)
            return result;
    }
    uint32_t source = socket->local_address ? socket->local_address : net_source(destination);
    if (!source)
        return -ENETUNREACH;
    if (socket->type == SOCK_RAW) {
        if (length < 8 || length > 65515)
            return -EMSGSIZE;
        uint8_t *packet = kmalloc(length);
        if (!packet)
            return -ENOMEM;
        memcpy(packet, data, length);
        net_write16(packet + 2, 0);
        net_write16(packet + 2, net_checksum(packet, length));
        int result = net_ipv4_send(source, destination, IPPROTO_ICMP, packet, length, false);
        kfree(packet);
        return result ? result : (int)length;
    }
    if (length > 65507)
        return -EMSGSIZE;
    uint8_t *packet = kmalloc(length + 8);
    if (!packet)
        return -ENOMEM;
    net_write16(packet, socket->local_port);
    net_write16(packet + 2, port);
    net_write16(packet + 4, length + 8);
    net_write16(packet + 6, 0);
    if (length)
        memcpy(packet + 8, data, length);
    uint16_t checksum =
        net_transport_checksum(source, destination, IPPROTO_UDP, packet, length + 8);
    net_write16(packet + 6, checksum ? checksum : 0xffff);
    int result = net_ipv4_send(source, destination, IPPROTO_UDP, packet, length + 8, false);
    kfree(packet);
    return result ? result : (int)length;
}
static int socket_read(struct vnode *node, uint64_t offset, void *data, size_t length)
{
    net_lock();
    int result = receive_socket(node->data, data, length, 0, NULL);
    net_unlock();
    return result;
}
static int socket_write(struct vnode *node, uint64_t offset, const void *data, size_t length)
{
    net_lock();
    int result = send_socket(node->data, data, length, NULL);
    net_unlock();
    return result;
}
static void socket_destroy(struct vnode *node)
{
    net_lock();
    struct net_socket *socket = node->data;
    if (socket->state == TCP_LISTEN) {
        struct net_socket *child = net_sockets;
        while (child) {
            struct net_socket *next = child->next;
            if (child->parent == socket) {
                tcp_abort(child, ECONNABORTED, true);
                net_socket_free(child);
            }
            child = next;
        }
    }
    socket->orphan = true;
    free_packets(socket);
    if (socket->type != SOCK_STREAM || socket->state == TCP_CLOSED || socket->state == TCP_LISTEN) {
        net_socket_free(socket);
    } else if (socket->state == TCP_SYN_SENT || socket->state == TCP_SYN_RECEIVED) {
        tcp_abort(socket, ECONNABORTED, true);
        net_socket_free(socket);
    } else {
        socket->read_closed = true;
        int result = tcp_shutdown(socket);
        if (result && result != -ENOTCONN)
            tcp_abort(socket, ECONNABORTED, true);
        socket->deadline = monotonic_ns() + 120000000000UL;
    }
    net_unlock();
}
static struct vnode_ops socket_ops = {
    .read = socket_read, .write = socket_write, .destroy = socket_destroy};
static int install_socket(struct task *task, struct net_socket *socket, unsigned flags)
{
    unsigned fd;
    for (fd = 0; fd < MAX_FDS && task->fds[fd]; fd++)
        ;
    if (fd == MAX_FDS)
        return -EMFILE;
    struct file *file = kzalloc(sizeof(*file));
    struct vnode *node = kzalloc(sizeof(*node));
    if (!file || !node) {
        kfree(file);
        kfree(node);
        return -ENOMEM;
    }
    node->mode = S_IFSOCK | 0600;
    node->nlink = 1;
    node->uid = task->uid;
    node->gid = task->gid;
    node->data = socket;
    node->ops = &socket_ops;
    atomic_init(&node->refs, 1);
    atomic_flag_clear(&node->lock.flag);
    file->node = node;
    file->flags = O_RDWR | (flags & O_NONBLOCK);
    atomic_init(&file->refs, 1);
    atomic_flag_clear(&file->lock.flag);
    task->fds[fd] = file;
    task->fd_flags[fd] = flags & O_CLOEXEC ? FD_CLOEXEC : 0;
    return fd;
}
int net_socket(struct task *task, unsigned family, unsigned type, unsigned protocol)
{
    if (family != AF_INET)
        return -EAFNOSUPPORT;
    unsigned flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if ((type == SOCK_STREAM && protocol && protocol != IPPROTO_TCP) ||
        (type == SOCK_DGRAM && protocol && protocol != IPPROTO_UDP) ||
        (type == SOCK_RAW && protocol != IPPROTO_ICMP) ||
        (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW))
        return -EPROTONOSUPPORT;
    if (type == SOCK_RAW && task->uid)
        return -EPERM;
    if (!protocol)
        protocol = type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
    net_lock();
    struct net_socket *socket = socket_new(type, protocol);
    int result = socket ? install_socket(task, socket, flags) : -ENOMEM;
    if (result < 0 && socket)
        net_socket_free(socket);
    net_unlock();
    return result;
}
static int address_in(struct sockaddr_in *address, uint64_t pointer, uint64_t length)
{
    if (length < sizeof(*address))
        return -EINVAL;
    if (copy_in(address, pointer, sizeof(*address)))
        return -EFAULT;
    return address->sin_family == AF_INET ? 0 : -EAFNOSUPPORT;
}
static int address_validate(struct task *task, uint64_t address, uint64_t length_pointer)
{
    if (!address)
        return 0;
    uint32_t length;
    if (copy_in(&length, length_pointer, sizeof(length)) ||
        !user_range(task->root, length_pointer, sizeof(length), true))
        return -EFAULT;
    if (length > sizeof(struct sockaddr_in))
        length = sizeof(struct sockaddr_in);
    return user_range(task->root, address, length, true) ? 0 : -EFAULT;
}
static int address_out(uint64_t pointer, uint64_t length_pointer, const struct sockaddr_in *address)
{
    if (!pointer)
        return 0;
    uint32_t length;
    if (copy_in(&length, length_pointer, sizeof(length)))
        return -EFAULT;
    if (length > sizeof(*address))
        length = sizeof(*address);
    if (copy_out(pointer, address, length))
        return -EFAULT;
    length = sizeof(*address);
    return copy_out(length_pointer, &length, sizeof(length));
}
static void socket_address(struct net_socket *socket, bool peer, struct sockaddr_in *address)
{
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_addr.s_addr =
        __builtin_bswap32(peer ? socket->remote_address : socket->local_address);
    address->sin_port = __builtin_bswap16(peer ? socket->remote_port : socket->local_port);
}
int net_control(struct task *task, unsigned operation, uint64_t fd, uint64_t b, uint64_t c,
                uint64_t d, uint64_t e, uint64_t f)
{
    if (operation == SYS_netconfig)
        return b > 1 ? -EINVAL : net_configure(task, fd, b);
    if (fd >= MAX_FDS || !task->fds[fd])
        return -EBADF;
    struct file *file = task->fds[fd];
    if (file->node->ops != &socket_ops)
        return -ENOTSOCK;
    struct net_socket *socket = file->node->data;
    struct sockaddr_in address = {0};
    int result = 0;
    if (operation == SYS_bind || operation == SYS_connect || (operation == SYS_sendto && e)) {
        result =
            address_in(&address, operation == SYS_sendto ? e : b, operation == SYS_sendto ? f : c);
        if (result)
            return result;
    }
    if (operation == SYS_accept || operation == SYS_getsockname || operation == SYS_getpeername ||
        operation == SYS_recvfrom) {
        result = address_validate(task, operation == SYS_recvfrom ? e : b,
                                  operation == SYS_recvfrom ? f : c);
        if (result)
            return result;
    }
    if (operation == SYS_sendto || operation == SYS_recvfrom) {
        if (c > 65535 || (c && !user_range(task->root, b, c, operation == SYS_recvfrom)))
            return c > 65535 ? -EMSGSIZE : -EFAULT;
        if (d & ~(MSG_DONTWAIT | (operation == SYS_recvfrom ? MSG_PEEK : MSG_NOSIGNAL)))
            return -ENOTSUP;
    }
    if (operation == SYS_getsockopt) {
        uint32_t length;
        if (b != SOL_SOCKET)
            return -ENOTSUP;
        if (copy_in(&length, e, sizeof(length)) ||
            !user_range(task->root, e, sizeof(length), true) ||
            !user_range(task->root, d, sizeof(int), true))
            return -EFAULT;
        if (length < sizeof(int))
            return -EINVAL;
    }
    net_lock();
    switch (operation) {
        case SYS_getsockopt: {
            int value;
            if (c == SO_ERROR) {
                value = socket->error;
                socket->error = 0;
                if (!value && socket->connecting && !socket->connected)
                    value = EINPROGRESS;
                if (value != EINPROGRESS)
                    socket->connecting = false;
            } else if (c == SO_TYPE) {
                value = socket->type;
            } else if (c == SO_ACCEPTCONN) {
                value = socket->state == TCP_LISTEN;
            } else {
                result = -ENOTSUP;
                break;
            }
            uint32_t length = sizeof(value);
            result = copy_out(d, &value, sizeof(value));
            if (!result)
                result = copy_out(e, &length, sizeof(length));
            break;
        }
        case SYS_bind:
            result = bind_socket(socket, __builtin_bswap32(address.sin_addr.s_addr),
                                 __builtin_bswap16(address.sin_port));
            break;
        case SYS_connect: {
            uint32_t destination = __builtin_bswap32(address.sin_addr.s_addr);
            uint16_t port = __builtin_bswap16(address.sin_port);
            if (!destination || (socket->type != SOCK_RAW && !port)) {
                result = -EINVAL;
                break;
            }
            if (socket->type == SOCK_STREAM && socket->connecting) {
                if (socket->remote_address != destination || socket->remote_port != port) {
                    result = -EALREADY;
                } else if (socket->error) {
                    result = -socket->error;
                    socket->error = 0;
                    socket->connecting = false;
                } else if (socket->connected) {
                    socket->connecting = false;
                    result = 0;
                } else {
                    result = file->flags & O_NONBLOCK ? -EALREADY : -EAGAIN;
                }
                break;
            }
            if (socket->connected && socket->type == SOCK_STREAM) {
                result = -EISCONN;
                break;
            }
            if (!socket->bound && (result = bind_socket(socket, 0, 0)))
                break;
            uint32_t source =
                socket->local_address ? socket->local_address : net_source(destination);
            if (!source) {
                result = -ENETUNREACH;
                break;
            }
            socket->remote_address = destination;
            socket->remote_port = port;
            if (socket->type == SOCK_STREAM) {
                socket->local_address = source;
                result = tcp_connect(socket);
                if (!result) {
                    socket->connecting = true;
                    result = file->flags & O_NONBLOCK ? -EINPROGRESS : -EAGAIN;
                }
            } else {
                socket->connected = true;
            }
            break;
        }
        case SYS_listen:
            if (socket->type != SOCK_STREAM ||
                (socket->state != TCP_CLOSED && socket->state != TCP_LISTEN)) {
                result = -EINVAL;
                break;
            }
            if (!socket->bound && (result = bind_socket(socket, 0, 0)))
                break;
            socket->backlog = b < 1 ? 1 : b > 64 ? 64 : b;
            socket->state = TCP_LISTEN;
            break;
        case SYS_accept: {
            if (socket->state != TCP_LISTEN) {
                result = -EINVAL;
                break;
            }
            struct net_socket *child = socket->accept_head;
            if (!child) {
                result = -EAGAIN;
                break;
            }
            socket_address(child, true, &address);
            result = address_out(b, c, &address);
            if (result)
                break;
            result = install_socket(task, child, 0);
            if (result >= 0) {
                socket->accept_head = child->accept_next;
                if (!socket->accept_head)
                    socket->accept_tail = NULL;
                child->parent = NULL;
                child->accept_next = NULL;
                socket->children--;
            }
            break;
        }
        case SYS_sendto:
            result = send_socket(socket, (const void *)b, c, e ? &address : NULL);
            break;
        case SYS_recvfrom:
            result = receive_socket(socket, (void *)b, c, d, &address);
            if (result >= 0) {
                int error = address_out(e, f, &address);
                if (error)
                    result = error;
            }
            break;
        case SYS_shutdown:
            if (b > SHUT_RDWR) {
                result = -EINVAL;
                break;
            }
            if (!socket->connected) {
                result = -ENOTCONN;
                break;
            }
            if (b != SHUT_RD) {
                if (socket->type == SOCK_STREAM)
                    result = tcp_shutdown(socket);
                else
                    socket->write_closed = true;
            }
            if (!result && b != SHUT_WR) {
                socket->read_closed = true;
                free_packets(socket);
                if (socket->type == SOCK_STREAM)
                    tcp_acknowledge(socket);
            }
            break;
        case SYS_getsockname:
        case SYS_getpeername:
            if (operation == SYS_getpeername && !socket->connected) {
                result = -ENOTCONN;
                break;
            }
            socket_address(socket, operation == SYS_getpeername, &address);
            result = address_out(b, c, &address);
            break;
        default:
            result = -ENOSYS;
            break;
    }
    net_unlock();
    return result;
}
void net_transport_receive(uint32_t source, uint32_t destination, unsigned protocol,
                           const uint8_t *data, size_t length)
{
    if (protocol == IPPROTO_TCP) {
        tcp_receive(source, destination, data, length);
        return;
    }
    if (protocol == IPPROTO_ICMP) {
        for (struct net_socket *socket = net_sockets; socket; socket = socket->next)
            if (socket->type == SOCK_RAW &&
                (!socket->local_address || socket->local_address == destination) &&
                (!socket->connected || socket->remote_address == source))
                net_queue_packet(socket, source, 0, data, length);
        return;
    }
    if (length < 8 || net_read16(data + 4) < 8 || net_read16(data + 4) > length)
        return;
    length = net_read16(data + 4);
    if (net_read16(data + 6) &&
        net_transport_checksum(source, destination, IPPROTO_UDP, data, length))
        return;
    uint16_t source_port = net_read16(data), destination_port = net_read16(data + 2);
    struct net_socket *best = NULL;
    for (struct net_socket *socket = net_sockets; socket; socket = socket->next) {
        if (socket->type != SOCK_DGRAM || !socket->bound ||
            socket->local_port != destination_port ||
            (socket->local_address && socket->local_address != destination) ||
            (socket->connected &&
             (socket->remote_address != source || socket->remote_port != source_port)))
            continue;
        if (!best || socket->connected || socket->local_address)
            best = socket;
    }
    if (best) {
        net_queue_packet(best, source, source_port, data + 8, length - 8);
    } else {
        net_udp_unreachable(source, destination, data, length);
    }
}
void net_socket_error(uint32_t source, uint32_t destination, unsigned protocol,
                      uint16_t source_port, uint16_t destination_port, int error)
{
    for (struct net_socket *socket = net_sockets; socket; socket = socket->next) {
        if (socket->protocol != protocol || socket->local_port != source_port ||
            (socket->local_address && socket->local_address != source) ||
            socket->remote_address != destination || socket->remote_port != destination_port)
            continue;
        if (socket->type == SOCK_STREAM && socket->state == TCP_SYN_SENT)
            tcp_abort(socket, error, false);
        else if (socket->type == SOCK_DGRAM)
            socket->error = error;
    }
}
void net_socket_tick(uint64_t now)
{
    struct net_socket *socket = net_sockets;
    while (socket) {
        struct net_socket *next = socket->next;
        if (socket->type == SOCK_STREAM)
            tcp_tick(socket, now);
        if ((socket->orphan && socket->state == TCP_CLOSED) ||
            (socket->parent && socket->state == TCP_CLOSED))
            net_socket_free(socket);
        socket = next;
    }
}
