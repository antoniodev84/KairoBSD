#include "net.h"
#define ARP_ENTRIES 64
#define PENDING_LIMIT 64
#define REASSEMBLY_LIMIT 8
struct arp_entry {
    struct net_card *card;
    uint32_t address;
    uint8_t mac[6];
    uint64_t expires, requested;
};
struct pending_packet {
    struct pending_packet *next;
    struct net_card *card;
    uint32_t target;
    uint64_t expires;
    uint16_t length;
    uint8_t data[];
};
struct reassembly {
    struct net_card *card;
    uint32_t source, destination;
    uint16_t id;
    uint8_t protocol;
    uint32_t total, received, highest;
    uint64_t expires;
    uint8_t *data, *present;
};
static spinlock_t network_lock = SPINLOCK_INIT;
static struct net_card *cards[NET_MAX_CARDS];
static unsigned card_count, pending_count;
static struct arp_entry arp_cache[ARP_ENTRIES];
static struct pending_packet *pending_head, *pending_tail;
static struct reassembly fragments[REASSEMBLY_LIMIT];
static uint64_t random_state;
static uint16_t identification;
static const uint8_t broadcast_mac[6] = {255, 255, 255, 255, 255, 255};
uint16_t net_read16(const void *pointer)
{
    const uint8_t *p = pointer;
    return ((uint16_t)p[0] << 8) | p[1];
}
uint32_t net_read32(const void *pointer)
{
    const uint8_t *p = pointer;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void net_write16(void *pointer, uint16_t value)
{
    uint8_t *p = pointer;
    p[0] = value >> 8;
    p[1] = value;
}
void net_write32(void *pointer, uint32_t value)
{
    uint8_t *p = pointer;
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
}
static uint32_t checksum_add(uint32_t sum, const uint8_t *data, size_t length)
{
    while (length >= 2) {
        sum += net_read16(data);
        data += 2;
        length -= 2;
    }
    if (length)
        sum += (uint16_t)*data << 8;
    return sum;
}
static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}
uint16_t net_checksum(const void *data, size_t length)
{
    return checksum_finish(checksum_add(0, data, length));
}
uint16_t net_transport_checksum(uint32_t source, uint32_t destination, unsigned protocol,
                                const void *data, size_t length)
{
    uint32_t sum = (source >> 16) + (source & 0xffff) + (destination >> 16) +
                   (destination & 0xffff) + protocol + length;
    return checksum_finish(checksum_add(sum, data, length));
}
void net_lock(void)
{
    spin_lock(&network_lock);
}
void net_unlock(void)
{
    spin_unlock(&network_lock);
}
uint32_t net_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state >> 16;
}
static bool unicast(uint32_t address)
{
    return address && address != UINT32_MAX && (address >> 24) < 224 && (address >> 24) != 0;
}
static bool local_address(uint32_t address)
{
    if ((address >> 24) == 127)
        return true;
    for (unsigned i = 0; i < card_count; i++)
        if (cards[i]->up && cards[i]->address && cards[i]->address == address)
            return true;
    return false;
}
static struct net_card *route(uint32_t destination, uint32_t source)
{
    struct net_card *best = NULL;
    uint32_t best_mask = 0;
    for (unsigned i = 0; i < card_count; i++) {
        struct net_card *card = cards[i];
        if (!card->up || !card->link || !card->address || (source && source != card->address))
            continue;
        if ((destination & card->netmask) == (card->address & card->netmask) &&
            (!best || card->netmask > best_mask)) {
            best = card;
            best_mask = card->netmask;
        }
    }
    if (best)
        return best;
    for (unsigned i = 0; i < card_count; i++) {
        struct net_card *card = cards[i];
        if (card->up && card->link && card->address && card->gateway &&
            (!source || source == card->address))
            return card;
    }
    return NULL;
}
uint32_t net_source(uint32_t destination)
{
    if ((destination >> 24) == 127)
        return 0x7f000001;
    if (local_address(destination))
        return destination;
    struct net_card *card = route(destination, 0);
    return card ? card->address : 0;
}
static int ethernet_send(struct net_card *card, const uint8_t *mac, unsigned type, const void *data,
                         size_t length)
{
    if (!card->up || !card->link)
        return -ENETDOWN;
    if (length > NET_MTU)
        return -EMSGSIZE;
    uint8_t frame[1514];
    memcpy(frame, mac, 6);
    memcpy(frame + 6, card->mac, 6);
    net_write16(frame + 12, type);
    memcpy(frame + 14, data, length);
    return card->transmit(card, frame, length + 14);
}
static int arp_send(struct net_card *card, const uint8_t *mac, uint32_t target, bool reply)
{
    uint8_t packet[28] = {0};
    net_write16(packet, 1);
    net_write16(packet + 2, 0x0800);
    packet[4] = 6;
    packet[5] = 4;
    net_write16(packet + 6, reply ? 2 : 1);
    memcpy(packet + 8, card->mac, 6);
    net_write32(packet + 14, card->address);
    if (reply)
        memcpy(packet + 18, mac, 6);
    net_write32(packet + 24, target);
    return ethernet_send(card, mac, 0x0806, packet, sizeof(packet));
}
static struct arp_entry *arp_find(struct net_card *card, uint32_t address, bool create)
{
    struct arp_entry *oldest = &arp_cache[0];
    for (unsigned i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *entry = &arp_cache[i];
        if (entry->card == card && entry->address == address)
            return entry;
        if (!entry->card || entry->expires < oldest->expires)
            oldest = entry;
    }
    if (!create)
        return NULL;
    memset(oldest, 0, sizeof(*oldest));
    oldest->card = card;
    oldest->address = address;
    return oldest;
}
static void arp_receive(struct net_card *card, const uint8_t *packet, size_t length,
                        const uint8_t *ethernet_source)
{
    if (length < 28 || net_read16(packet) != 1 || net_read16(packet + 2) != 0x0800 ||
        packet[4] != 6 || packet[5] != 4 ||
        (net_read16(packet + 6) != 1 && net_read16(packet + 6) != 2))
        return;
    uint32_t source = net_read32(packet + 14), target = net_read32(packet + 24);
    if ((packet[8] & 1) || memcmp(packet + 8, ethernet_source, 6))
        return;
    dhcp_conflict(card, source, target, packet + 8);
    if (!card->address)
        return;
    if (target == card->address && unicast(source) && source != card->address &&
        (source & card->netmask) == (card->address & card->netmask)) {
        struct arp_entry *entry = arp_find(card, source, true);
        memcpy(entry->mac, packet + 8, 6);
        entry->expires = monotonic_ns() + 60000000000UL;
    }
    if (net_read16(packet + 6) == 1 && target == card->address)
        arp_send(card, packet + 8, source, true);
}
static int queue_ip(struct net_card *card, uint32_t target, const void *data, size_t length)
{
    if (pending_count >= PENDING_LIMIT)
        return -ENOBUFS;
    struct pending_packet *packet = kmalloc(sizeof(*packet) + length);
    if (!packet)
        return -ENOMEM;
    packet->next = NULL;
    packet->card = card;
    packet->target = target;
    packet->length = length;
    packet->expires = monotonic_ns() + 5000000000UL;
    memcpy(packet->data, data, length);
    if (pending_tail)
        pending_tail->next = packet;
    else
        pending_head = packet;
    pending_tail = packet;
    pending_count++;
    return 0;
}
static int output_ip(struct net_card *card, uint32_t target, const void *data, size_t length)
{
    uint64_t now = monotonic_ns();
    if (!card)
        return queue_ip(NULL, target, data, length);
    if (target == UINT32_MAX || target == (card->address | ~card->netmask))
        return ethernet_send(card, broadcast_mac, 0x0800, data, length);
    struct arp_entry *entry = arp_find(card, target, true);
    if (entry->expires > now) {
        int result = ethernet_send(card, entry->mac, 0x0800, data, length);
        if (result != -EAGAIN)
            return result;
    }
    int result = queue_ip(card, target, data, length);
    if (!result && (!entry->requested || now - entry->requested >= 1000000000)) {
        arp_send(card, broadcast_mac, target, false);
        entry->requested = now;
    }
    return result;
}
int net_ipv4_send(uint32_t source, uint32_t destination, unsigned protocol, const void *data,
                  size_t length, bool dont_fragment)
{
    if (length > 65515)
        return -EMSGSIZE;
    if (!destination || (destination >> 24) >= 224)
        return -ENETUNREACH;
    if (!source)
        source = net_source(destination);
    if (!source || !local_address(source))
        return -EADDRNOTAVAIL;
    bool local = local_address(destination);
    if (((source >> 24) == 127) != ((destination >> 24) == 127))
        return -ENETUNREACH;
    struct net_card *card = local ? NULL : route(destination, source);
    if (!local && !card)
        return -ENETUNREACH;
    if (dont_fragment && length > NET_MTU - 20)
        return -EMSGSIZE;
    unsigned count = length ? (length + 1479) / 1480 : 1;
    if ((local || length > NET_MTU - 20) && pending_count + count > PENDING_LIMIT)
        return -ENOBUFS;
    uint16_t id = ++identification;
    size_t offset = 0;
    do {
        size_t amount = length - offset;
        if (amount > 1480)
            amount = 1480;
        uint8_t packet[NET_MTU] = {0};
        packet[0] = 0x45;
        net_write16(packet + 2, amount + 20);
        net_write16(packet + 4, id);
        uint16_t fragment = offset / 8;
        if (offset + amount < length)
            fragment |= 0x2000;
        if (dont_fragment)
            fragment |= 0x4000;
        net_write16(packet + 6, fragment);
        packet[8] = 64;
        packet[9] = protocol;
        net_write32(packet + 12, source);
        net_write32(packet + 16, destination);
        net_write16(packet + 10, net_checksum(packet, 20));
        if (amount)
            memcpy(packet + 20, (const uint8_t *)data + offset, amount);
        uint32_t target = destination;
        if (card && (destination & card->netmask) != (card->address & card->netmask) &&
            destination != UINT32_MAX)
            target = card->gateway;
        int result = output_ip(card, target, packet, amount + 20);
        if (result)
            return result;
        offset += amount;
    } while (offset < length);
    return 0;
}
static void icmp_error(uint32_t source, uint32_t destination, unsigned type, unsigned code,
                       const uint8_t *original, size_t length)
{
    if (!unicast(source) || !local_address(destination))
        return;
    size_t header = (original[0] & 15) * 4;
    if (length < header || (net_read16(original + 6) & 0x1fff))
        return;
    if (original[9] == IPPROTO_ICMP && length > header &&
        (original[header] == 3 || original[header] == 4 || original[header] == 5 ||
         original[header] == 11 || original[header] == 12))
        return;
    static uint64_t epoch;
    static unsigned budget;
    uint64_t now = monotonic_ns();
    if (now - epoch >= 1000000000) {
        epoch = now;
        budget = 0;
    }
    if (budget++ >= 32)
        return;
    size_t quoted = length < header + 8 ? length : header + 8;
    uint8_t packet[76] = {0};
    packet[0] = type;
    packet[1] = code;
    memcpy(packet + 8, original, quoted);
    net_write16(packet + 2, net_checksum(packet, quoted + 8));
    net_ipv4_send(destination, source, IPPROTO_ICMP, packet, quoted + 8, false);
}
static void deliver_ip(uint32_t source, uint32_t destination, unsigned protocol,
                       const uint8_t *payload, size_t length, const uint8_t *original,
                       size_t original_length)
{
    if (protocol == IPPROTO_ICMP) {
        if (length < 8 || net_checksum(payload, length))
            return;
        net_transport_receive(source, destination, protocol, payload, length);
        if (payload[0] == 8 && payload[1] == 0 && local_address(destination) && unicast(source)) {
            uint8_t *reply = kmalloc(length);
            if (!reply)
                return;
            memcpy(reply, payload, length);
            reply[0] = 0;
            net_write16(reply + 2, 0);
            net_write16(reply + 2, net_checksum(reply, length));
            net_ipv4_send(destination, source, IPPROTO_ICMP, reply, length, false);
            kfree(reply);
        } else if ((payload[0] == 3 || payload[0] == 11) && length >= 36) {
            const uint8_t *quoted = payload + 8;
            unsigned header = (quoted[0] & 15) * 4;
            if ((quoted[0] >> 4) == 4 && header >= 20 && length >= 8 + header + 8 &&
                net_read32(quoted + 12) == destination && !(net_read16(quoted + 6) & 0x1fff)) {
                int error = payload[0] == 11  ? ETIMEDOUT
                            : payload[1] == 3 ? ECONNREFUSED
                            : payload[1] == 4 ? EMSGSIZE
                                              : ENETUNREACH;
                net_socket_error(net_read32(quoted + 12), net_read32(quoted + 16), quoted[9],
                                 net_read16(quoted + header), net_read16(quoted + header + 2),
                                 error);
            }
        }
        return;
    }
    if (protocol == IPPROTO_TCP || protocol == IPPROTO_UDP) {
        net_transport_receive(source, destination, protocol, payload, length);
        return;
    }
    if (original)
        icmp_error(source, destination, 3, 2, original, original_length);
}
static void reassembly_clear(struct reassembly *entry)
{
    kfree(entry->data);
    kfree(entry->present);
    memset(entry, 0, sizeof(*entry));
}
static void ipv4_receive(struct net_card *card, const uint8_t *packet, size_t length)
{
    if (length < 20 || (packet[0] >> 4) != 4)
        return;
    unsigned header = (packet[0] & 15) * 4;
    unsigned total = net_read16(packet + 2);
    uint16_t fragment = net_read16(packet + 6);
    uint32_t source = net_read32(packet + 12), destination = net_read32(packet + 16);
    if (header < 20 || header > length || total < header || total > length ||
        net_checksum(packet, header) || !packet[8] || (fragment & 0x8000) || !unicast(source) ||
        (!card && !local_address(destination)) ||
        (card && ((source >> 24) == 127 || (destination >> 24) == 127)))
        return;
    if (card && packet[9] == IPPROTO_UDP && !(fragment & 0x3fff) && total >= header + 8) {
        const uint8_t *udp = packet + header;
        unsigned udp_length = net_read16(udp + 4);
        if (net_read16(udp) == 67 && net_read16(udp + 2) == 68) {
            if (udp_length >= 8 && udp_length <= total - header &&
                (!net_read16(udp + 6) ||
                 !net_transport_checksum(source, destination, IPPROTO_UDP, udp, udp_length)))
                dhcp_receive(card, source, udp + 8, udp_length - 8);
            return;
        }
    }
    if (card && !card->address)
        return;
    if (!local_address(destination) && destination != UINT32_MAX &&
        (!card || destination != (card->address | ~card->netmask)))
        return;
    const uint8_t *payload = packet + header;
    unsigned payload_length = total - header;
    if (!(fragment & 0x3fff)) {
        deliver_ip(source, destination, packet[9], payload, payload_length, packet, total);
        return;
    }
    if ((fragment & 0x4000) || !payload_length || ((fragment & 0x2000) && (payload_length & 7)))
        return;
    uint32_t offset = (fragment & 0x1fff) * 8;
    if (offset + payload_length > 65515)
        return;
    uint64_t now = monotonic_ns();
    struct reassembly *entry = NULL, *available = NULL;
    for (unsigned i = 0; i < REASSEMBLY_LIMIT; i++) {
        struct reassembly *candidate = &fragments[i];
        if (candidate->data && candidate->expires <= now)
            reassembly_clear(candidate);
        if (!candidate->data)
            available = candidate;
        else if (candidate->card == card && candidate->source == source &&
                 candidate->destination == destination && candidate->protocol == packet[9] &&
                 candidate->id == net_read16(packet + 4))
            entry = candidate;
    }
    if (!entry) {
        entry = available;
        if (!entry)
            return;
        entry->data = kmalloc(65515);
        entry->present = kzalloc(8192);
        if (!entry->data || !entry->present) {
            reassembly_clear(entry);
            return;
        }
        entry->card = card;
        entry->source = source;
        entry->destination = destination;
        entry->protocol = packet[9];
        entry->id = net_read16(packet + 4);
        entry->expires = now + 30000000000UL;
    }
    if ((entry->total && offset + payload_length > entry->total) ||
        (!(fragment & 0x2000) && ((entry->total && entry->total != offset + payload_length) ||
                                  entry->highest > offset + payload_length))) {
        reassembly_clear(entry);
        return;
    }
    for (unsigned i = offset; i < offset + payload_length; i++) {
        if (entry->present[i / 8] & (1U << (i % 8))) {
            reassembly_clear(entry);
            return;
        }
    }
    for (unsigned i = offset; i < offset + payload_length; i++)
        entry->present[i / 8] |= 1U << (i % 8);
    memcpy(entry->data + offset, payload, payload_length);
    entry->received += payload_length;
    if (entry->highest < offset + payload_length)
        entry->highest = offset + payload_length;
    if (!(fragment & 0x2000))
        entry->total = offset + payload_length;
    if (entry->total && entry->received == entry->total) {
        deliver_ip(source, destination, entry->protocol, entry->data, entry->total, NULL, 0);
        reassembly_clear(entry);
    }
}
void net_receive(struct net_card *card, const void *data, size_t length)
{
    const uint8_t *frame = data;
    if (!card->up || length < 14 || length > 1514 || (frame[6] & 1))
        return;
    if (memcmp(frame, card->mac, 6) && memcmp(frame, broadcast_mac, 6))
        return;
    switch (net_read16(frame + 12)) {
        case 0x0806:
            arp_receive(card, frame + 14, length - 14, frame + 6);
            break;
        case 0x0800:
            ipv4_receive(card, frame + 14, length - 14);
            break;
    }
}
void net_udp_unreachable(uint32_t source, uint32_t destination, const void *data, size_t length)
{
    uint8_t original[28] = {0};
    original[0] = 0x45;
    original[9] = IPPROTO_UDP;
    net_write16(original + 2, length + 20);
    net_write32(original + 12, source);
    net_write32(original + 16, destination);
    memcpy(original + 20, data, 8);
    icmp_error(source, destination, 3, 3, original, sizeof(original));
}
void net_init(void)
{
    random_state = rdtsc() ^ monotonic_ns() ^ 0x4b6169726f425344UL;
    if (!random_state)
        random_state = 1;
}
int net_register(struct net_card *card)
{
    if (card_count == NET_MAX_CARDS)
        return -ENOSPC;
    card->index = card_count;
    card->up = true;
    cards[card_count++] = card;
    card->poll(card);
    dhcp_start(card);
    kprintf("Network en%u: %02x:%02x:%02x:%02x:%02x:%02x\n", card->index, card->mac[0],
            card->mac[1], card->mac[2], card->mac[3], card->mac[4], card->mac[5]);
    return 0;
}
static void pending_tick(uint64_t now)
{
    struct pending_packet *packet = pending_head;
    pending_head = pending_tail = NULL;
    pending_count = 0;
    while (packet) {
        struct pending_packet *next = packet->next;
        bool done = false;
        if (!packet->card) {
            ipv4_receive(NULL, packet->data, packet->length);
            done = true;
        } else {
            struct net_card *card = packet->card;
            struct arp_entry *entry = arp_find(card, packet->target, true);
            if (packet->expires <= now || !card->up || !card->link) {
                done = true;
            } else if (entry->expires > now) {
                int result = ethernet_send(card, entry->mac, 0x0800, packet->data, packet->length);
                done = result != -EAGAIN;
            } else if (!entry->requested || now - entry->requested >= 1000000000) {
                arp_send(card, broadcast_mac, packet->target, false);
                entry->requested = now;
            }
        }
        if (done) {
            kfree(packet);
        } else {
            packet->next = NULL;
            if (pending_tail)
                pending_tail->next = packet;
            else
                pending_head = packet;
            pending_tail = packet;
            pending_count++;
        }
        packet = next;
    }
}
void net_worker(void)
{
    for (;;) {
        uint64_t flags = irq_save();
        net_lock();
        for (unsigned i = 0; i < card_count; i++)
            cards[i]->poll(cards[i]);
        uint64_t now = monotonic_ns();
        pending_tick(now);
        net_socket_tick(now);
        for (unsigned i = 0; i < card_count; i++)
            dhcp_tick(cards[i], now);
        for (unsigned i = 0; i < REASSEMBLY_LIMIT; i++)
            if (fragments[i].data && fragments[i].expires <= now)
                reassembly_clear(&fragments[i]);
        net_unlock();
        irq_restore(flags);
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}
void net_set_address(struct net_card *card, uint32_t address, uint32_t mask, uint32_t gateway,
                     const uint32_t *dns)
{
    bool changed = card->address != address || card->netmask != mask || card->gateway != gateway;
    card->address = address;
    card->netmask = mask;
    card->gateway = gateway;
    card->dns[0] = dns ? dns[0] : 0;
    card->dns[1] = dns ? dns[1] : 0;
    if (!changed)
        return;
    struct pending_packet **link = &pending_head;
    pending_tail = NULL;
    while (*link) {
        struct pending_packet *packet = *link;
        if (packet->card == card) {
            *link = packet->next;
            pending_count--;
            kfree(packet);
        } else {
            pending_tail = packet;
            link = &packet->next;
        }
    }
    for (unsigned i = 0; i < ARP_ENTRIES; i++)
        if (arp_cache[i].card == card)
            memset(&arp_cache[i], 0, sizeof(arp_cache[i]));
    for (unsigned i = 0; i < REASSEMBLY_LIMIT; i++)
        if (fragments[i].card == card)
            reassembly_clear(&fragments[i]);
}
int net_dhcp_send(struct net_card *card, uint32_t source, uint32_t destination, const void *payload,
                  size_t length)
{
    if (length > 1472)
        return -EMSGSIZE;
    uint8_t packet[1500] = {0};
    uint8_t *udp = packet + 20;
    net_write16(udp, 68);
    net_write16(udp + 2, 67);
    net_write16(udp + 4, length + 8);
    memcpy(udp + 8, payload, length);
    uint16_t checksum = net_transport_checksum(source, destination, IPPROTO_UDP, udp, length + 8);
    net_write16(udp + 6, checksum ? checksum : 0xffff);
    if (destination != UINT32_MAX)
        return net_ipv4_send(source, destination, IPPROTO_UDP, udp, length + 8, false);
    packet[0] = 0x45;
    net_write16(packet + 2, length + 28);
    net_write16(packet + 4, ++identification);
    packet[8] = 64;
    packet[9] = IPPROTO_UDP;
    net_write32(packet + 12, source);
    net_write32(packet + 16, destination);
    net_write16(packet + 10, net_checksum(packet, 20));
    return ethernet_send(card, broadcast_mac, 0x0800, packet, length + 28);
}
int net_arp_probe(struct net_card *card, uint32_t address)
{
    uint8_t packet[28] = {0};
    net_write16(packet, 1);
    net_write16(packet + 2, 0x0800);
    packet[4] = 6;
    packet[5] = 4;
    net_write16(packet + 6, 1);
    memcpy(packet + 8, card->mac, 6);
    net_write32(packet + 24, address);
    return ethernet_send(card, broadcast_mac, 0x0806, packet, sizeof(packet));
}
void net_arp_announce(struct net_card *card)
{
    arp_send(card, broadcast_mac, card->address, false);
}
int net_configure(struct task *task, uint64_t pointer, bool set)
{
    struct knetconfig config;
    if (copy_in(&config, pointer, sizeof(config)))
        return -EFAULT;
    if (!user_range(task->root, pointer, sizeof(config), true))
        return -EFAULT;
    if (set && task->uid)
        return -EPERM;
    net_lock();
    if (config.index >= card_count) {
        net_unlock();
        return -ENODEV;
    }
    struct net_card *card = cards[config.index];
    if (set) {
        uint32_t address = __builtin_bswap32(config.address);
        uint32_t mask = __builtin_bswap32(config.netmask);
        uint32_t gateway = __builtin_bswap32(config.gateway);
        uint32_t inverse = ~mask;
        if (config.up > 1 || (inverse & (inverse + 1)) ||
            (address && (!unicast(address) || (address >> 24) == 127)) ||
            (gateway && (!unicast(gateway) || (gateway & mask) != (address & mask)))) {
            net_unlock();
            return -EINVAL;
        }
        for (unsigned i = 0; i < card_count; i++) {
            if (cards[i] != card && address && cards[i]->address == address) {
                net_unlock();
                return -EADDRINUSE;
            }
        }
        uint32_t dns[2] = {__builtin_bswap32(config.dns[0]), __builtin_bswap32(config.dns[1])};
        if (config.dhcp > 1 || (dns[0] && !unicast(dns[0])) || (dns[1] && !unicast(dns[1]))) {
            net_unlock();
            return -EINVAL;
        }
        card->up = config.up;
        if (config.dhcp) {
            if (!card->dhcp.enabled)
                dhcp_start(card);
            else {
                card->dns[0] = dns[0];
                card->dns[1] = dns[1];
            }
        } else {
            dhcp_stop(card, true);
            net_set_address(card, address, mask, gateway, dns);
        }
    }
    config.address = __builtin_bswap32(card->address);
    config.netmask = __builtin_bswap32(card->netmask);
    config.gateway = __builtin_bswap32(card->gateway);
    config.dns[0] = __builtin_bswap32(card->dns[0]);
    config.dns[1] = __builtin_bswap32(card->dns[1]);
    config.dhcp = card->dhcp.enabled;
    config.dhcp_state = card->dhcp.state;
    uint64_t now = monotonic_ns();
    config.lease_remaining =
        card->dhcp.expires > now ? (card->dhcp.expires - now) / 1000000000UL : 0;
    config.up = card->up && card->link;
    memcpy(config.mac, card->mac, 6);
    net_unlock();
    return copy_out(pointer, &config, sizeof(config));
}
