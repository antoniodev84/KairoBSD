#include "net.h"
static bool sequence_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}
static uint32_t segment_end(const struct tcp_segment *segment)
{
    return segment->sequence + segment->length + !!(segment->flags & NET_TCP_SYN) +
           !!(segment->flags & NET_TCP_FIN);
}
static unsigned receive_window(struct net_socket *socket)
{
    if (socket->read_closed)
        return NET_TCP_WINDOW;
    size_t used = socket->receive_bytes;
    for (struct tcp_segment *segment = socket->reorder_head; segment; segment = segment->next)
        used += segment->length;
    return used < NET_TCP_WINDOW ? NET_TCP_WINDOW - used : 0;
}
static int emit(struct net_socket *socket, uint32_t sequence, uint32_t acknowledgement,
                unsigned flags, const void *data, size_t length)
{
    uint8_t packet[NET_TCP_MSS + 24] = {0};
    unsigned header = flags & NET_TCP_SYN ? 24 : 20;
    if (length > NET_TCP_MSS)
        return -EMSGSIZE;
    net_write16(packet, socket->local_port);
    net_write16(packet + 2, socket->remote_port);
    net_write32(packet + 4, sequence);
    net_write32(packet + 8, acknowledgement);
    packet[12] = (header / 4) << 4;
    packet[13] = flags;
    net_write16(packet + 14, receive_window(socket));
    if (flags & NET_TCP_SYN) {
        packet[20] = 2;
        packet[21] = 4;
        net_write16(packet + 22, NET_TCP_MSS);
    }
    if (length)
        memcpy(packet + header, data, length);
    net_write16(packet + 16, net_transport_checksum(socket->local_address, socket->remote_address,
                                                    IPPROTO_TCP, packet, header + length));
    return net_ipv4_send(socket->local_address, socket->remote_address, IPPROTO_TCP, packet,
                         header + length, true);
}
void tcp_acknowledge(struct net_socket *socket)
{
    if (socket->state != TCP_CLOSED && socket->state != TCP_LISTEN && socket->state != TCP_SYN_SENT)
        emit(socket, socket->send_highest, socket->receive_next, NET_TCP_ACK, NULL, 0);
}
static void reset_reply(uint32_t source, uint32_t destination, const uint8_t *packet, size_t length,
                        unsigned header)
{
    if (packet[13] & NET_TCP_RST)
        return;
    struct net_socket socket = {0};
    socket.local_address = destination;
    socket.remote_address = source;
    socket.local_port = net_read16(packet + 2);
    socket.remote_port = net_read16(packet);
    if (packet[13] & NET_TCP_ACK) {
        emit(&socket, net_read32(packet + 8), 0, NET_TCP_RST, NULL, 0);
    } else {
        uint32_t next = net_read32(packet + 4) + length - header + !!(packet[13] & NET_TCP_SYN) +
                        !!(packet[13] & NET_TCP_FIN);
        emit(&socket, 0, next, NET_TCP_RST | NET_TCP_ACK, NULL, 0);
    }
}
static void free_segments(struct tcp_segment **head)
{
    while (*head) {
        struct tcp_segment *segment = *head;
        *head = segment->next;
        kfree(segment);
    }
}
void tcp_abort(struct net_socket *socket, int error, bool reset)
{
    if (reset && socket->state != TCP_CLOSED && socket->state != TCP_LISTEN)
        emit(socket, socket->send_highest, socket->receive_next, NET_TCP_RST | NET_TCP_ACK, NULL,
             0);
    socket->error = error;
    socket->state = TCP_CLOSED;
    socket->connected = false;
    socket->peer_closed = true;
    free_segments(&socket->transmit_head);
    socket->transmit_tail = NULL;
    free_segments(&socket->reorder_head);
}
static int queue_segment(struct net_socket *socket, unsigned flags, const void *data, size_t length)
{
    struct tcp_segment *segment = kmalloc(sizeof(*segment) + length);
    if (!segment)
        return -ENOMEM;
    memset(segment, 0, sizeof(*segment));
    segment->sequence = socket->send_next;
    segment->length = length;
    segment->flags = flags;
    if (length)
        memcpy(segment->data, data, length);
    if (socket->transmit_tail)
        socket->transmit_tail->next = segment;
    else
        socket->transmit_head = segment;
    socket->transmit_tail = segment;
    socket->send_next = segment_end(segment);
    return 0;
}
static void transmit_pending(struct net_socket *socket, uint64_t now)
{
    uint32_t limit = socket->peer_window;
    if (limit > socket->congestion_window)
        limit = socket->congestion_window;
    for (struct tcp_segment *segment = socket->transmit_head; segment; segment = segment->next) {
        if (segment->sent)
            continue;
        uint32_t end = segment_end(segment);
        if (!(segment->flags & NET_TCP_SYN) && end - socket->send_unacknowledged > limit)
            break;
        int result = emit(socket, segment->sequence, socket->receive_next, segment->flags,
                          segment->data, segment->length);
        if (result) {
            if (result != -EAGAIN && result != -ENOBUFS && result != -ENOMEM)
                socket->error = -result;
            break;
        }
        segment->sent = now ? now : 1;
        if (sequence_before(socket->send_highest, end))
            socket->send_highest = end;
        socket->last_activity = now;
    }
}
int tcp_connect(struct net_socket *socket)
{
    if (socket->state != TCP_CLOSED)
        return -EALREADY;
    if (socket->peer_closed || socket->write_closed)
        return -EINVAL;
    socket->send_next = net_random();
    socket->send_unacknowledged = socket->send_next;
    socket->send_highest = socket->send_next;
    socket->state = TCP_SYN_SENT;
    socket->deadline = monotonic_ns() + 75000000000UL;
    int result = queue_segment(socket, NET_TCP_SYN, NULL, 0);
    if (result) {
        socket->state = TCP_CLOSED;
        return result;
    }
    transmit_pending(socket, monotonic_ns());
    return 0;
}
int tcp_send(struct net_socket *socket, const void *data, size_t length)
{
    if (!socket->connected || (socket->state != TCP_ESTABLISHED && socket->state != TCP_CLOSE_WAIT))
        return socket->peer_closed ? -EPIPE : -ENOTCONN;
    if (!length)
        return 0;
    uint32_t queued = socket->send_next - socket->send_unacknowledged;
    if (queued >= NET_TCP_WINDOW)
        return -EAGAIN;
    size_t room = NET_TCP_WINDOW - queued;
    if (length > room)
        length = room;
    unsigned segments = 0;
    for (struct tcp_segment *segment = socket->transmit_head; segment; segment = segment->next)
        segments++;
    if (segments >= 128)
        return -EAGAIN;
    size_t accepted = 0;
    while (accepted < length && segments < 128) {
        size_t amount = length - accepted;
        if (amount > socket->mss)
            amount = socket->mss;
        int result = queue_segment(socket, NET_TCP_ACK | NET_TCP_PSH,
                                   (const uint8_t *)data + accepted, amount);
        if (result)
            return accepted ? (int)accepted : result;
        accepted += amount;
        segments++;
    }
    transmit_pending(socket, monotonic_ns());
    return accepted;
}
int tcp_shutdown(struct net_socket *socket)
{
    if (socket->write_closed)
        return 0;
    if (socket->state != TCP_ESTABLISHED && socket->state != TCP_CLOSE_WAIT)
        return -ENOTCONN;
    int result = queue_segment(socket, NET_TCP_FIN | NET_TCP_ACK, NULL, 0);
    if (result)
        return result;
    socket->write_closed = true;
    socket->state = socket->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
    socket->deadline = monotonic_ns() + 120000000000UL;
    transmit_pending(socket, monotonic_ns());
    return 0;
}
static void time_wait(struct net_socket *socket)
{
    socket->state = TCP_TIME_WAIT;
    socket->deadline = monotonic_ns() + 60000000000UL;
}
static void finish_received(struct net_socket *socket)
{
    socket->receive_next++;
    socket->peer_closed = true;
    switch (socket->state) {
        case TCP_ESTABLISHED:
        case TCP_SYN_RECEIVED:
            socket->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            socket->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2:
            time_wait(socket);
            break;
        case TCP_TIME_WAIT:
            time_wait(socket);
            break;
        default:
            break;
    }
}
static void parse_mss(struct net_socket *socket, const uint8_t *packet, unsigned header)
{
    socket->mss = 536;
    for (unsigned i = 20; i < header;) {
        unsigned kind = packet[i++];
        if (!kind)
            break;
        if (kind == 1)
            continue;
        if (i >= header)
            break;
        unsigned length = packet[i++];
        if (length < 2 || length - 2 > header - i)
            break;
        if (kind == 2 && length == 4) {
            uint16_t mss = net_read16(packet + i);
            if (mss)
                socket->mss = mss < NET_TCP_MSS ? mss : NET_TCP_MSS;
        }
        i += length - 2;
    }
}
static bool acknowledge(struct net_socket *socket, uint32_t acknowledgement, uint16_t window,
                        uint32_t sequence)
{
    if (sequence_before(socket->send_highest, acknowledgement)) {
        tcp_acknowledge(socket);
        return false;
    }
    if (sequence_before(acknowledgement, socket->send_unacknowledged))
        return true;
    bool changed_window = socket->peer_window != window;
    socket->peer_window = window;
    uint32_t acknowledged = acknowledgement - socket->send_unacknowledged;
    if (!acknowledged) {
        if (!changed_window && socket->transmit_head && socket->transmit_head->sent &&
            ++socket->duplicate_acks == 3) {
            socket->slow_threshold = socket->congestion_window / 2;
            if (socket->slow_threshold < 2U * socket->mss)
                socket->slow_threshold = 2U * socket->mss;
            socket->congestion_window = socket->slow_threshold;
            struct tcp_segment *segment = socket->transmit_head;
            emit(socket, segment->sequence, socket->receive_next, segment->flags, segment->data,
                 segment->length);
            segment->sent = monotonic_ns();
        }
        return true;
    }
    socket->duplicate_acks = 0;
    socket->send_unacknowledged = acknowledgement;
    socket->retransmit_timeout = 1000000000;
    if (socket->congestion_window < socket->slow_threshold)
        socket->congestion_window += acknowledged;
    else
        socket->congestion_window +=
            ((uint32_t)socket->mss * socket->mss) / socket->congestion_window + 1;
    if (socket->congestion_window > NET_TCP_WINDOW)
        socket->congestion_window = NET_TCP_WINDOW;
    bool finished = false;
    while (socket->transmit_head) {
        struct tcp_segment *segment = socket->transmit_head;
        if (sequence_before(acknowledgement, segment_end(segment))) {
            if (sequence_before(segment->sequence, acknowledgement) && segment->length &&
                !(segment->flags & NET_TCP_SYN)) {
                unsigned trim = acknowledgement - segment->sequence;
                if (trim <= segment->length) {
                    memmove(segment->data, segment->data + trim, segment->length - trim);
                    segment->length -= trim;
                    segment->sequence += trim;
                }
            }
            break;
        }
        finished |= !!(segment->flags & NET_TCP_FIN);
        socket->transmit_head = segment->next;
        kfree(segment);
    }
    if (!socket->transmit_head)
        socket->transmit_tail = NULL;
    if (finished) {
        if (socket->state == TCP_FIN_WAIT_1) {
            socket->state = TCP_FIN_WAIT_2;
            socket->deadline = monotonic_ns() + 120000000000UL;
        } else if (socket->state == TCP_CLOSING) {
            time_wait(socket);
        } else if (socket->state == TCP_LAST_ACK) {
            socket->state = TCP_CLOSED;
            socket->connected = false;
        }
    }
    (void)sequence;
    return true;
}
static void accept_ready(struct net_socket *socket)
{
    struct net_socket *parent = socket->parent;
    if (!parent)
        return;
    if (parent->accept_tail)
        parent->accept_tail->accept_next = socket;
    else
        parent->accept_head = socket;
    parent->accept_tail = socket;
}
static void receive_contiguous(struct net_socket *socket, const uint8_t *data, size_t length,
                               unsigned flags)
{
    if (socket->peer_closed)
        return;
    if (length && !socket->read_closed) {
        if (net_queue_packet(socket, socket->remote_address, socket->remote_port, data, length))
            return;
    }
    socket->receive_next += length;
    if (flags & NET_TCP_FIN)
        finish_received(socket);
}
static void receive_data(struct net_socket *socket, uint32_t sequence, const uint8_t *data,
                         size_t length, unsigned flags)
{
    if (!length && !(flags & NET_TCP_FIN))
        return;
    uint32_t end = sequence + length + !!(flags & NET_TCP_FIN);
    if (!sequence_before(socket->receive_next, end)) {
        tcp_acknowledge(socket);
        return;
    }
    if (sequence_before(sequence, socket->receive_next)) {
        uint32_t trim = socket->receive_next - sequence;
        if (trim > length) {
            tcp_acknowledge(socket);
            return;
        }
        data += trim;
        length -= trim;
        sequence += trim;
    }
    unsigned window = receive_window(socket);
    uint32_t distance = sequence - socket->receive_next;
    if (distance >= window && (length || (flags & NET_TCP_FIN))) {
        tcp_acknowledge(socket);
        return;
    }
    if (length > window - distance) {
        length = window - distance;
        flags &= ~NET_TCP_FIN;
    }
    if (sequence == socket->receive_next) {
        receive_contiguous(socket, data, length, flags);
    } else {
        unsigned count = 0;
        for (struct tcp_segment *segment = socket->reorder_head; segment; segment = segment->next) {
            if (++count >= 64)
                return;
            if (sequence_before(sequence, segment_end(segment)) &&
                sequence_before(segment->sequence, end)) {
                tcp_acknowledge(socket);
                return;
            }
        }
        struct tcp_segment *segment = kmalloc(sizeof(*segment) + length);
        if (!segment)
            return;
        memset(segment, 0, sizeof(*segment));
        segment->sequence = sequence;
        segment->length = length;
        segment->flags = flags & NET_TCP_FIN;
        if (length)
            memcpy(segment->data, data, length);
        struct tcp_segment **link = &socket->reorder_head;
        while (*link && sequence_before((*link)->sequence, sequence))
            link = &(*link)->next;
        segment->next = *link;
        *link = segment;
    }
    while (socket->reorder_head &&
           !sequence_before(socket->receive_next, socket->reorder_head->sequence)) {
        struct tcp_segment *segment = socket->reorder_head;
        unsigned trim = socket->receive_next - segment->sequence;
        if (trim <= segment->length) {
            uint32_t before = socket->receive_next;
            receive_contiguous(socket, segment->data + trim, segment->length - trim,
                               segment->flags);
            if (socket->receive_next == before && segment->length > trim)
                break;
        }
        socket->reorder_head = segment->next;
        kfree(segment);
    }
    tcp_acknowledge(socket);
}
void tcp_receive(uint32_t source, uint32_t destination, const uint8_t *packet, size_t length)
{
    if (length < 20 || !net_read16(packet) || !net_read16(packet + 2) ||
        net_transport_checksum(source, destination, IPPROTO_TCP, packet, length))
        return;
    if (net_source(destination) != destination)
        return;
    unsigned header = (packet[12] >> 4) * 4;
    if (header < 20 || header > length)
        return;
    uint16_t source_port = net_read16(packet), destination_port = net_read16(packet + 2);
    uint32_t sequence = net_read32(packet + 4), acknowledgement = net_read32(packet + 8);
    uint16_t window = net_read16(packet + 14);
    unsigned flags = packet[13];
    struct net_socket *socket = NULL, *listener = NULL;
    for (struct net_socket *candidate = net_sockets; candidate; candidate = candidate->next) {
        if (candidate->type != SOCK_STREAM || candidate->local_port != destination_port ||
            (candidate->local_address && candidate->local_address != destination))
            continue;
        if (candidate->state == TCP_LISTEN) {
            if (!listener || candidate->local_address)
                listener = candidate;
        } else if (candidate->remote_address == source && candidate->remote_port == source_port &&
                   candidate->state != TCP_CLOSED) {
            socket = candidate;
            break;
        }
    }
    if (!socket) {
        if (!listener) {
            reset_reply(source, destination, packet, length, header);
            return;
        }
        if (flags & NET_TCP_RST)
            return;
        if (flags & NET_TCP_ACK) {
            reset_reply(source, destination, packet, length, header);
            return;
        }
        if (!(flags & NET_TCP_SYN))
            return;
        socket = net_tcp_child(listener);
        if (!socket)
            return;
        socket->local_address = destination;
        socket->remote_address = source;
        socket->remote_port = source_port;
        socket->receive_next = sequence + 1;
        socket->send_next = net_random();
        socket->send_unacknowledged = socket->send_next;
        socket->send_highest = socket->send_next;
        socket->peer_window = window;
        socket->state = TCP_SYN_RECEIVED;
        socket->deadline = monotonic_ns() + 75000000000UL;
        parse_mss(socket, packet, header);
        if (queue_segment(socket, NET_TCP_SYN | NET_TCP_ACK, NULL, 0)) {
            net_socket_free(socket);
            return;
        }
        transmit_pending(socket, monotonic_ns());
        return;
    }
    socket->last_activity = monotonic_ns();
    if (socket->state == TCP_TIME_WAIT && (flags & NET_TCP_RST))
        return;
    if (socket->state == TCP_SYN_SENT) {
        if ((flags & NET_TCP_ACK) &&
            (sequence_before(acknowledgement, socket->send_unacknowledged + 1) ||
             sequence_before(socket->send_highest, acknowledgement))) {
            reset_reply(source, destination, packet, length, header);
            return;
        }
        if (flags & NET_TCP_RST) {
            if (flags & NET_TCP_ACK)
                tcp_abort(socket, ECONNREFUSED, false);
            return;
        }
        if (!(flags & NET_TCP_SYN))
            return;
        socket->receive_next = sequence + 1;
        socket->peer_window = window;
        parse_mss(socket, packet, header);
        if (flags & NET_TCP_ACK) {
            acknowledge(socket, acknowledgement, window, sequence);
            socket->state = TCP_ESTABLISHED;
            socket->connected = true;
            socket->deadline = 0;
            tcp_acknowledge(socket);
            receive_data(socket, sequence + 1, packet + header, length - header,
                         flags & ~NET_TCP_SYN);
        } else {
            socket->state = TCP_SYN_RECEIVED;
            socket->transmit_head->flags |= NET_TCP_ACK;
            socket->transmit_head->sent = 0;
            transmit_pending(socket, monotonic_ns());
        }
        return;
    }
    if ((flags & NET_TCP_SYN) && socket->state == TCP_SYN_RECEIVED &&
        sequence + 1 == socket->receive_next) {
        if (socket->transmit_head) {
            socket->transmit_head->sent = 0;
            transmit_pending(socket, monotonic_ns());
        }
        return;
    }
    unsigned span = length - header + !!(flags & NET_TCP_SYN) + !!(flags & NET_TCP_FIN);
    unsigned available = receive_window(socket);
    bool acceptable =
        span ? (available && (int32_t)(sequence - socket->receive_next) < (int32_t)available &&
                (int32_t)(sequence + span - socket->receive_next) > 0)
             : (available ? (uint32_t)(sequence - socket->receive_next) < available
                          : sequence == socket->receive_next);
    if (!acceptable) {
        if (!(flags & NET_TCP_RST))
            tcp_acknowledge(socket);
        if (socket->state == TCP_TIME_WAIT && (flags & NET_TCP_FIN))
            time_wait(socket);
        return;
    }
    if (flags & NET_TCP_RST) {
        if (sequence == socket->receive_next)
            tcp_abort(socket, ECONNRESET, false);
        else
            tcp_acknowledge(socket);
        return;
    }
    if (flags & NET_TCP_SYN) {
        tcp_acknowledge(socket);
        return;
    }
    if (!(flags & NET_TCP_ACK))
        return;
    if (socket->state == TCP_SYN_RECEIVED) {
        if (acknowledgement != socket->send_next) {
            reset_reply(source, destination, packet, length, header);
            return;
        }
        socket->state = TCP_ESTABLISHED;
        socket->connected = true;
        socket->deadline = 0;
        accept_ready(socket);
    }
    if (!acknowledge(socket, acknowledgement, window, sequence) || socket->state == TCP_CLOSED)
        return;
    if (socket->state == TCP_TIME_WAIT) {
        if (flags & NET_TCP_FIN) {
            tcp_acknowledge(socket);
            time_wait(socket);
        }
        return;
    }
    receive_data(socket, sequence, packet + header, length - header, flags);
    transmit_pending(socket, monotonic_ns());
}
void tcp_tick(struct net_socket *socket, uint64_t now)
{
    if (socket->state == TCP_CLOSED || socket->state == TCP_LISTEN)
        return;
    if (socket->deadline && now >= socket->deadline) {
        if (socket->state == TCP_TIME_WAIT) {
            socket->state = TCP_CLOSED;
            socket->connected = false;
        } else {
            tcp_abort(socket, ETIMEDOUT, true);
        }
        return;
    }
    struct tcp_segment *segment = socket->transmit_head;
    if (segment && segment->sent && now - segment->sent >= socket->retransmit_timeout) {
        if (segment->retries++ >= 8) {
            tcp_abort(socket, ETIMEDOUT, true);
            return;
        }
        socket->slow_threshold = socket->congestion_window / 2;
        if (socket->slow_threshold < 2U * socket->mss)
            socket->slow_threshold = 2U * socket->mss;
        socket->congestion_window = socket->mss;
        socket->duplicate_acks = 0;
        emit(socket, segment->sequence, socket->receive_next, segment->flags, segment->data,
             segment->length);
        segment->sent = now;
        if (socket->retransmit_timeout < 32000000000UL)
            socket->retransmit_timeout *= 2;
    }
    if (segment && !socket->peer_window && now - socket->last_activity >= 1000000000) {
        emit(socket, socket->send_unacknowledged - 1, socket->receive_next, NET_TCP_ACK, NULL, 0);
        socket->last_activity = now;
    }
    transmit_pending(socket, now);
}
