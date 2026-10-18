#include "net.h"
#define DHCP_COOKIE 0x63825363U
struct dhcp_options {
    uint32_t mask, gateway, dns[2], server, lease, renewal, rebinding;
    uint8_t type, overload;
    bool has_mask, has_dns, has_gateway;
};
static bool valid_address(uint32_t address)
{
    return address && (address >> 24) != 0 && (address >> 24) != 127 && (address >> 24) < 224;
}
static bool parse_options(const uint8_t *bytes, size_t length, struct dhcp_options *options,
                          bool main)
{
    for (size_t at = 0; at < length;) {
        unsigned code = bytes[at++];
        if (code == 255)
            return true;
        if (!code)
            continue;
        if (at == length)
            return false;
        unsigned size = bytes[at++];
        if (size > length - at)
            return false;
        const uint8_t *value = bytes + at;
        switch (code) {
            case 1:
                if (size != 4)
                    return false;
                options->mask = net_read32(value);
                options->has_mask = true;
                break;
            case 3:
                if (!size || size % 4)
                    return false;
                options->gateway = net_read32(value);
                options->has_gateway = true;
                break;
            case 6:
                if (!size || size % 4)
                    return false;
                options->dns[0] = net_read32(value);
                options->dns[1] = size >= 8 ? net_read32(value + 4) : 0;
                options->has_dns = true;
                break;
            case 51:
            case 58:
            case 59:
            case 54:
                if (size != 4)
                    return false;
                if (code == 51)
                    options->lease = net_read32(value);
                else if (code == 58)
                    options->renewal = net_read32(value);
                else if (code == 59)
                    options->rebinding = net_read32(value);
                else
                    options->server = net_read32(value);
                break;
            case 52:
                if (!main || size != 1 || value[0] > 3)
                    return false;
                options->overload = value[0];
                break;
            case 53:
                if (size != 1 || options->type)
                    return false;
                options->type = value[0];
                break;
        }
        at += size;
    }
    return true;
}
static size_t option32(uint8_t *packet, size_t at, unsigned code, uint32_t value)
{
    packet[at++] = code;
    packet[at++] = 4;
    net_write32(packet + at, value);
    return at + 4;
}
static int send_message(struct net_card *card, unsigned type)
{
    struct dhcp_client *client = &card->dhcp;
    uint8_t packet[320] = {0};
    packet[0] = 1;
    packet[1] = 1;
    packet[2] = 6;
    net_write32(packet + 4, client->transaction);
    uint64_t elapsed = (monotonic_ns() - client->started) / 1000000000;
    net_write16(packet + 8, elapsed > 65535 ? 65535 : elapsed);
    bool renewing = client->state == DHCP_RENEWING || client->state == DHCP_REBINDING;
    if (renewing || type == 7)
        net_write32(packet + 12, card->address);
    if (!renewing && type != 7)
        net_write16(packet + 10, 0x8000);
    memcpy(packet + 28, card->mac, 6);
    net_write32(packet + 236, DHCP_COOKIE);
    size_t at = 240;
    packet[at++] = 53;
    packet[at++] = 1;
    packet[at++] = type;
    packet[at++] = 61;
    packet[at++] = 7;
    packet[at++] = 1;
    memcpy(packet + at, card->mac, 6);
    at += 6;
    if (client->state == DHCP_REQUESTING || type == 4) {
        at = option32(packet, at, 50, client->offered);
        at = option32(packet, at, 54, client->server);
    } else if (type == 7) {
        at = option32(packet, at, 54, client->server);
    }
    if (type == 1 || type == 3) {
        packet[at++] = 55;
        packet[at++] = 7;
        packet[at++] = 1;
        packet[at++] = 3;
        packet[at++] = 6;
        packet[at++] = 51;
        packet[at++] = 54;
        packet[at++] = 58;
        packet[at++] = 59;
        packet[at++] = 57;
        packet[at++] = 2;
        net_write16(packet + at, 1500);
        at += 2;
    }
    packet[at++] = 255;
    uint32_t destination =
        client->state == DHCP_RENEWING || type == 7 ? client->server : UINT32_MAX;
    return net_dhcp_send(card, renewing || type == 7 ? card->address : 0, destination, packet,
                         at < 300 ? 300 : at);
}
void dhcp_stop(struct net_card *card, bool release)
{
    if (release && card->address && card->dhcp.enabled && card->dhcp.server)
        send_message(card, 7);
    memset(&card->dhcp, 0, sizeof(card->dhcp));
}
static void restart(struct net_card *card, uint64_t delay)
{
    bool enabled = card->dhcp.enabled;
    memset(&card->dhcp, 0, sizeof(card->dhcp));
    card->dhcp.enabled = enabled;
    card->dhcp.state = DHCP_SELECTING;
    card->dhcp.transaction = net_random();
    card->dhcp.started = monotonic_ns();
    card->dhcp.next = card->dhcp.started + delay;
    net_set_address(card, 0, 0, 0, NULL);
}
void dhcp_start(struct net_card *card)
{
    card->dhcp.enabled = true;
    restart(card, net_random() % 1000000000U);
}
void dhcp_conflict(struct net_card *card, uint32_t source, uint32_t target, const uint8_t *mac)
{
    struct dhcp_client *client = &card->dhcp;
    if (client->state != DHCP_PROBING || !memcmp(mac, card->mac, 6))
        return;
    if (source == client->offered || (!source && target == client->offered)) {
        send_message(card, 4);
        restart(card, 10000000000UL);
    }
}
void dhcp_receive(struct net_card *card, uint32_t source, const uint8_t *packet, size_t length)
{
    struct dhcp_client *client = &card->dhcp;
    if (!client->enabled || length < 240 || packet[0] != 2 || packet[1] != 1 || packet[2] != 6 ||
        net_read32(packet + 4) != client->transaction || memcmp(packet + 28, card->mac, 6) ||
        net_read32(packet + 236) != DHCP_COOKIE)
        return;
    struct dhcp_options options = {0};
    if (!parse_options(packet + 240, length - 240, &options, true) ||
        ((options.overload & 1) && !parse_options(packet + 108, 128, &options, false)) ||
        ((options.overload & 2) && !parse_options(packet + 44, 64, &options, false)))
        return;
    if (!valid_address(options.server))
        return;
    if (client->state != DHCP_SELECTING && client->state != DHCP_REBINDING &&
        options.server != client->server)
        return;
    if (options.type == 6 && (client->state == DHCP_REQUESTING || client->state == DHCP_RENEWING ||
                              client->state == DHCP_REBINDING)) {
        restart(card, 1000000000);
        return;
    }
    uint32_t address = net_read32(packet + 16);
    if (options.type == 2 && client->state == DHCP_SELECTING && valid_address(address)) {
        client->offered = address;
        client->server = options.server;
        client->state = DHCP_REQUESTING;
        client->attempts = 0;
        client->next = monotonic_ns();
        return;
    }
    if (options.type != 5 || (client->state != DHCP_REQUESTING && client->state != DHCP_RENEWING &&
                              client->state != DHCP_REBINDING))
        return;
    if (!address && client->state != DHCP_REQUESTING)
        address = card->address;
    if (!valid_address(address) ||
        (client->state == DHCP_REQUESTING && address != client->offered) || !options.lease ||
        (options.has_dns &&
         (!valid_address(options.dns[0]) || (options.dns[1] && !valid_address(options.dns[1])))))
        return;
    uint32_t mask = options.has_mask ? options.mask : card->netmask;
    if (!options.has_mask && !mask) {
        unsigned first = address >> 24;
        mask = first < 128 ? 0xff000000 : first < 192 ? 0xffff0000 : 0xffffff00;
    }
    if (!options.has_gateway && card->address == address)
        options.gateway = card->gateway;
    uint32_t inverse = ~mask;
    if ((inverse & (inverse + 1)) ||
        (options.gateway &&
         (!valid_address(options.gateway) || (options.gateway & mask) != (address & mask))))
        return;
    uint64_t now = monotonic_ns();
    uint64_t lease = (uint64_t)options.lease * 1000000000UL;
    uint64_t renewal = options.renewal ? (uint64_t)options.renewal * 1000000000UL : lease / 2;
    uint64_t rebinding =
        options.rebinding ? (uint64_t)options.rebinding * 1000000000UL : lease - lease / 8;
    if (!renewal || renewal >= rebinding || rebinding >= lease) {
        renewal = lease / 2;
        rebinding = lease - lease / 8;
    }
    client->server = options.server;
    client->offered = address;
    client->mask = mask;
    client->gateway = options.gateway;
    client->dns[0] = options.has_dns ? options.dns[0] : card->dns[0];
    client->dns[1] = options.has_dns ? options.dns[1] : card->dns[1];
    client->renew = options.lease == UINT32_MAX ? UINT64_MAX : now + renewal;
    client->rebind = options.lease == UINT32_MAX ? UINT64_MAX : now + rebinding;
    client->expires = options.lease == UINT32_MAX ? UINT64_MAX : now + lease;
    if (card->address == address) {
        net_set_address(card, address, mask, client->gateway, client->dns);
        client->state = DHCP_BOUND;
    } else {
        client->state = DHCP_PROBING;
        client->attempts = 0;
        client->next = now + net_random() % 1000000000U;
    }
    (void)source;
}
void dhcp_tick(struct net_card *card, uint64_t now)
{
    struct dhcp_client *client = &card->dhcp;
    if (!client->enabled)
        return;
    if (client->expires && now >= client->expires) {
        restart(card, 0);
        return;
    }
    if (!card->up || !card->link)
        return;
    if (client->state == DHCP_BOUND) {
        if (now < client->renew)
            return;
        client->state = DHCP_RENEWING;
        client->transaction = net_random();
        client->started = now;
        client->next = now;
        client->attempts = 0;
    }
    if (client->state == DHCP_RENEWING && now >= client->rebind) {
        client->state = DHCP_REBINDING;
        client->next = now;
        client->attempts = 0;
    }
    if (now < client->next)
        return;
    if (client->state == DHCP_PROBING) {
        if (client->attempts < 3) {
            if (net_arp_probe(card, client->offered)) {
                client->next = now + 1000000000;
                return;
            }
            client->attempts++;
            client->next = now + 1000000000UL + net_random() % 1000000000U;
        } else {
            net_set_address(card, client->offered, client->mask, client->gateway, client->dns);
            client->state = DHCP_BOUND;
            net_arp_announce(card);
            kprintf("DHCP en%u: %u.%u.%u.%u\n", card->index, card->address >> 24,
                    (card->address >> 16) & 255, (card->address >> 8) & 255, card->address & 255);
        }
        return;
    }
    if (client->state == DHCP_REQUESTING && client->attempts >= 5) {
        restart(card, 1000000000);
        return;
    }
    int result = send_message(card, client->state == DHCP_SELECTING ? 1 : 3);
    if (result) {
        client->next = now + 1000000000;
        return;
    }
    unsigned shift = client->attempts < 4 ? client->attempts : 4;
    uint64_t interval = (4UL << shift) * 1000000000UL;
    if (client->state == DHCP_RENEWING || client->state == DHCP_REBINDING) {
        uint64_t boundary = client->state == DHCP_RENEWING ? client->rebind : client->expires;
        interval = (boundary - now) / 2;
        if (interval < 1000000000)
            interval = 1000000000;
    }
    client->next = now + interval + net_random() % 1000000000U;
    if (client->attempts < 255)
        client->attempts++;
}
