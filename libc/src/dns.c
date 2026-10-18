#include <arpa/inet.h>
#include <netdb.h>
#define DNS_MAX_ADDRESSES 16
#define DNS_CACHE_SIZE 8
#define DNS_TRUNCATED 1
#define DNS_ALIAS 2
struct dns_result {
    char canonical[254];
    uint32_t addresses[DNS_MAX_ADDRESSES];
    unsigned count;
    uint32_t ttl;
};
struct dns_cache {
    char name[254];
    uint32_t servers[2];
    struct dns_result result;
    uint64_t expires;
};
static struct dns_cache cache[DNS_CACHE_SIZE];
static uint32_t transaction_counter;
int h_errno;
static uint16_t read16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}
static uint32_t read32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void write16(uint8_t *p, uint16_t value)
{
    p[0] = value >> 8;
    p[1] = value;
}
static uint64_t clock_ns(void)
{
    struct ksysinfo info;
    return sysinfo(&info) ? 0 : info.uptime_ns;
}
static int pause_query(uint64_t deadline)
{
    if (clock_ns() >= deadline)
        return EAI_AGAIN;
    struct ktimespec delay = {0, 10000000};
    if (nanosleep(&delay, NULL))
        return EAI_SYSTEM;
    return 0;
}
static int normalize(const char *name, char *output)
{
    if (!name)
        return EAI_NONAME;
    size_t length = strlen(name);
    if (length && name[length - 1] == '.')
        length--;
    if (!length || length > 253)
        return EAI_NONAME;
    unsigned label = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned c = (uint8_t)name[i];
        if (c == '.') {
            if (!label)
                return EAI_NONAME;
            label = 0;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_') ||
                ++label > 63)
                return EAI_NONAME;
        }
        output[i] = c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
    }
    if (!label)
        return EAI_NONAME;
    output[length] = 0;
    return 0;
}
static int decode_name(const uint8_t *message, size_t length, size_t *position, char *output)
{
    size_t at = *position, used = 0, end = 0;
    for (unsigned steps = 0; steps < 256; steps++) {
        if (at >= length)
            return EAI_FAIL;
        unsigned label = message[at++];
        if ((label & 0xc0) == 0xc0) {
            if (at >= length)
                return EAI_FAIL;
            unsigned target = ((label & 0x3f) << 8) | message[at++];
            if (target >= length || target >= at - 2)
                return EAI_FAIL;
            if (!end)
                end = at;
            at = target;
            continue;
        }
        if (label & 0xc0)
            return EAI_FAIL;
        if (!label) {
            output[used] = 0;
            *position = end ? end : at;
            return 0;
        }
        if (label > length - at || used + !!used + label > 253)
            return EAI_FAIL;
        if (used)
            output[used++] = '.';
        for (unsigned i = 0; i < label; i++) {
            unsigned c = message[at++];
            if (c <= 32 || c >= 127 || c == '.')
                return EAI_FAIL;
            output[used++] = c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
        }
    }
    return EAI_FAIL;
}
static size_t build_query(uint8_t *message, const char *name, uint16_t id)
{
    memset(message, 0, 512);
    write16(message, id);
    write16(message + 2, 0x0100);
    write16(message + 4, 1);
    size_t at = 12;
    const char *label = name;
    while (*label) {
        const char *end = label;
        while (*end && *end != '.')
            end++;
        size_t size = end - label;
        message[at++] = size;
        memcpy(message + at, label, size);
        at += size;
        label = *end ? end + 1 : end;
    }
    message[at++] = 0;
    write16(message + at, 1);
    write16(message + at + 2, 1);
    return at + 4;
}
static int parse_response(const uint8_t *message, size_t length, uint16_t id, const char *query,
                          struct dns_result *result)
{
    if (length < 12 || read16(message) != id || !(message[2] & 0x80) || (message[2] & 0x78) ||
        read16(message + 4) != 1)
        return EAI_FAIL;
    if (message[2] & 2)
        return DNS_TRUNCATED;
    size_t at = 12;
    char question[254];
    if (decode_name(message, length, &at, question) || strcmp(question, query) || at + 4 > length ||
        read16(message + at) != 1 || read16(message + at + 2) != 1)
        return EAI_FAIL;
    at += 4;
    unsigned code = message[3] & 15;
    if (code)
        return code == 3 ? EAI_NONAME : code == 2 ? EAI_AGAIN : EAI_FAIL;
    size_t answers_at = at;
    unsigned answers = read16(message + 6);
    if (answers > 1024)
        return EAI_FAIL;
    char current[254];
    strcpy(current, query);
    bool aliased = false;
    for (unsigned depth = 0; depth < 9; depth++) {
        at = answers_at;
        char alias[254] = {0};
        result->count = 0;
        for (unsigned i = 0; i < answers; i++) {
            char owner[254];
            if (decode_name(message, length, &at, owner) || at + 10 > length)
                return EAI_FAIL;
            unsigned type = read16(message + at), class = read16(message + at + 2);
            uint32_t ttl = read32(message + at + 4);
            unsigned size = read16(message + at + 8);
            at += 10;
            if (size > length - at)
                return EAI_FAIL;
            if (class == 1 && !strcmp(owner, current)) {
                if (type == 1) {
                    if (size != 4)
                        return EAI_FAIL;
                    uint32_t address;
                    memcpy(&address, message + at, 4);
                    bool duplicate = false;
                    for (unsigned n = 0; n < result->count; n++)
                        duplicate |= result->addresses[n] == address;
                    if (!duplicate && result->count < DNS_MAX_ADDRESSES)
                        result->addresses[result->count++] = address;
                    if (ttl < result->ttl)
                        result->ttl = ttl;
                } else if (type == 5) {
                    size_t end = at;
                    char target[254];
                    if (decode_name(message, length, &end, target) || end != at + size ||
                        !target[0])
                        return EAI_FAIL;
                    if (alias[0] && strcmp(alias, target))
                        return EAI_FAIL;
                    strcpy(alias, target);
                    if (ttl < result->ttl)
                        result->ttl = ttl;
                }
            }
            at += size;
        }
        if (result->count) {
            if (alias[0])
                return EAI_FAIL;
            strcpy(result->canonical, current);
            return 0;
        }
        if (!alias[0]) {
            if (aliased) {
                strcpy(result->canonical, current);
                return DNS_ALIAS;
            }
            return EAI_NONAME;
        }
        if (!strcmp(alias, current))
            return EAI_FAIL;
        strcpy(current, alias);
        aliased = true;
    }
    return EAI_FAIL;
}
static int transfer(int fd, uint8_t *bytes, size_t length, bool sending, uint64_t deadline)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = sending ? send(fd, bytes + done, length - done, MSG_NOSIGNAL)
                                : recv(fd, bytes + done, length - done, 0);
        if (count > 0) {
            done += count;
            continue;
        }
        if (!count)
            return EAI_FAIL;
        if (errno != EAGAIN)
            return errno == EINTR ? EAI_SYSTEM : EAI_AGAIN;
        int error = pause_query(deadline);
        if (error)
            return error;
    }
    return 0;
}
static int tcp_query(const struct sockaddr_in *server, uint8_t *query, size_t query_length,
                     uint16_t id, const char *name, struct dns_result *result)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return EAI_SYSTEM;
    uint64_t deadline = clock_ns() + 5000000000UL;
    int status = EAI_AGAIN;
    for (;;) {
        if (!connect(fd, (const struct sockaddr *)server, sizeof(*server))) {
            status = 0;
            break;
        }
        if (errno != EINPROGRESS && errno != EALREADY) {
            status = errno == EINTR ? EAI_SYSTEM : EAI_AGAIN;
            break;
        }
        status = pause_query(deadline);
        if (status)
            break;
    }
    uint8_t prefix[2];
    write16(prefix, query_length);
    if (!status)
        status = transfer(fd, prefix, 2, true, deadline);
    if (!status)
        status = transfer(fd, query, query_length, true, deadline);
    if (!status)
        status = transfer(fd, prefix, 2, false, deadline);
    unsigned length = read16(prefix);
    uint8_t *response = NULL;
    if (!status) {
        if (length < 12)
            status = EAI_FAIL;
        else if (!(response = malloc(length)))
            status = EAI_MEMORY;
    }
    if (!status)
        status = transfer(fd, response, length, false, deadline);
    if (!status)
        status = parse_response(response, length, id, name, result);
    free(response);
    close(fd);
    return status == DNS_TRUNCATED ? EAI_FAIL : status;
}
static int udp_query(uint32_t address, const char *name, struct dns_result *result)
{
    struct sockaddr_in server = {
        .sin_family = AF_INET, .sin_port = htons(53), .sin_addr = {address}};
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return EAI_SYSTEM;
    if (connect(fd, (const struct sockaddr *)&server, sizeof(server))) {
        close(fd);
        return EAI_AGAIN;
    }
    uint8_t query[512], response[4096];
    uint16_t id = (uint16_t)(clock_ns() ^ ((uint64_t)getpid() << 17) ^ ++transaction_counter);
    size_t length = build_query(query, name, id);
    int status = EAI_AGAIN;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        ssize_t count = send(fd, query, length, 0);
        if (count != (ssize_t)length) {
            status = errno == EINTR ? EAI_SYSTEM : EAI_AGAIN;
            break;
        }
        uint64_t deadline = clock_ns() + (1000000000UL << attempt);
        for (;;) {
            count = recv(fd, response, sizeof(response), 0);
            if (count >= 12 && read16(response) == id) {
                status = parse_response(response, count, id, name, result);
                if (status != EAI_FAIL)
                    break;
            } else if (count < 0 && errno != EAGAIN) {
                status = errno == EINTR ? EAI_SYSTEM : EAI_AGAIN;
                break;
            }
            status = pause_query(deadline);
            if (status)
                break;
        }
        if (status != EAI_AGAIN)
            break;
    }
    close(fd);
    return status == DNS_TRUNCATED ? tcp_query(&server, query, length, id, name, result) : status;
}
static int resolve(const char *name, struct dns_result *result)
{
    char normalized[254];
    int status = normalize(name, normalized);
    if (status)
        return status;
    memset(result, 0, sizeof(*result));
    result->ttl = UINT32_MAX;
    if (inet_pton(AF_INET, normalized, &result->addresses[0]) == 1) {
        result->count = 1;
        strcpy(result->canonical, normalized);
        return 0;
    }
    if (!strcmp(normalized, "localhost")) {
        result->addresses[0] = htonl(INADDR_LOOPBACK);
        result->count = 1;
        strcpy(result->canonical, normalized);
        return 0;
    }
    uint32_t servers[2] = {0};
    for (unsigned i = 0; i < 4 && !servers[0]; i++) {
        struct knetconfig config = {.index = i};
        if (netconfig(&config, 0))
            continue;
        if (config.up && config.address) {
            servers[0] = config.dns[0] ? config.dns[0] : config.dns[1];
            servers[1] = config.dns[0] ? config.dns[1] : 0;
        }
    }
    if (!servers[0])
        return EAI_AGAIN;
    uint64_t now = clock_ns();
    struct dns_cache *slot = &cache[0];
    for (unsigned i = 0; i < DNS_CACHE_SIZE; i++) {
        if (cache[i].expires > now && !strcmp(cache[i].name, normalized) &&
            cache[i].servers[0] == servers[0] && cache[i].servers[1] == servers[1]) {
            *result = cache[i].result;
            return 0;
        }
        if (cache[i].expires < slot->expires)
            slot = &cache[i];
    }
    char current[254];
    strcpy(current, normalized);
    for (unsigned depth = 0; depth < 8; depth++) {
        status = udp_query(servers[0], current, result);
        if (status < 0 && status != EAI_NONAME && servers[1] && servers[1] != servers[0])
            status = udp_query(servers[1], current, result);
        if (status == DNS_ALIAS) {
            if (!strcmp(current, result->canonical))
                return EAI_FAIL;
            strcpy(current, result->canonical);
            continue;
        }
        if (status)
            return status;
        strcpy(slot->name, normalized);
        slot->servers[0] = servers[0];
        slot->servers[1] = servers[1];
        slot->result = *result;
        uint64_t ttl = result->ttl > 86400 ? 86400 : result->ttl;
        slot->expires = clock_ns() + ttl * 1000000000UL;
        return 0;
    }
    return EAI_FAIL;
}
void freeaddrinfo(struct addrinfo *list)
{
    while (list) {
        struct addrinfo *next = list->ai_next;
        free(list->ai_addr);
        free(list->ai_canonname);
        free(list);
        list = next;
    }
}
static int service_port(const char *service, int type, uint16_t *port)
{
    if (!service) {
        *port = 0;
        return 0;
    }
    unsigned value = 0;
    const char *p = service;
    if (!*p)
        return EAI_SERVICE;
    while (*p >= '0' && *p <= '9') {
        if (value > 6553)
            return EAI_SERVICE;
        value = value * 10 + *p++ - '0';
    }
    if (!*p) {
        if (value > 65535)
            return EAI_SERVICE;
        *port = value;
        return 0;
    }
    if (!strcmp(service, "domain"))
        *port = 53;
    else if (type == SOCK_STREAM && !strcmp(service, "http"))
        *port = 80;
    else if (type == SOCK_STREAM && !strcmp(service, "https"))
        *port = 443;
    else
        return EAI_SERVICE;
    return 0;
}
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **output)
{
    if (!output)
        return EAI_FAIL;
    *output = NULL;
    int family = hints ? hints->ai_family : 0;
    int type = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    int flags = hints ? hints->ai_flags : 0;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV))
        return EAI_BADFLAGS;
    if (family && family != AF_INET)
        return EAI_FAMILY;
    if (type && type != SOCK_STREAM && type != SOCK_DGRAM)
        return EAI_SOCKTYPE;
    if (protocol && protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
        return EAI_SERVICE;
    if ((type == SOCK_STREAM && protocol == IPPROTO_UDP) ||
        (type == SOCK_DGRAM && protocol == IPPROTO_TCP))
        return EAI_SOCKTYPE;
    if (!type && protocol)
        type = protocol == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM;
    if (!node && !service)
        return EAI_NONAME;
    if ((flags & AI_NUMERICSERV) && service) {
        for (const char *p = service; *p; p++)
            if (*p < '0' || *p > '9')
                return EAI_NONAME;
    }
    struct dns_result result;
    if (!node) {
        memset(&result, 0, sizeof(result));
        result.count = 1;
        result.addresses[0] = htonl(flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
    } else {
        if ((flags & AI_NUMERICHOST) && inet_pton(AF_INET, node, &result.addresses[0]) != 1)
            return EAI_NONAME;
        int error = resolve(node, &result);
        if (error)
            return error;
    }
    struct addrinfo **tail = output;
    for (unsigned i = 0; i < result.count; i++) {
        for (int candidate = SOCK_STREAM; candidate <= SOCK_DGRAM; candidate++) {
            if (type && type != candidate)
                continue;
            uint16_t port;
            int error = service_port(service, candidate, &port);
            if (error) {
                if (!type)
                    continue;
                freeaddrinfo(*output);
                *output = NULL;
                return error;
            }
            struct addrinfo *entry = calloc(1, sizeof(*entry));
            struct sockaddr_in *address = calloc(1, sizeof(*address));
            if (!entry || !address) {
                free(entry);
                free(address);
                freeaddrinfo(*output);
                *output = NULL;
                return EAI_MEMORY;
            }
            address->sin_family = AF_INET;
            address->sin_port = htons(port);
            address->sin_addr.s_addr = result.addresses[i];
            entry->ai_flags = flags;
            entry->ai_family = AF_INET;
            entry->ai_socktype = candidate;
            entry->ai_protocol = candidate == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
            entry->ai_addrlen = sizeof(*address);
            entry->ai_addr = (struct sockaddr *)address;
            if (tail == output && node && (flags & AI_CANONNAME)) {
                entry->ai_canonname = malloc(strlen(result.canonical) + 1);
                if (!entry->ai_canonname) {
                    free(address);
                    free(entry);
                    freeaddrinfo(*output);
                    *output = NULL;
                    return EAI_MEMORY;
                }
                strcpy(entry->ai_canonname, result.canonical);
            }
            *tail = entry;
            tail = &entry->ai_next;
        }
    }
    return *output ? 0 : EAI_SERVICE;
}
const char *gai_strerror(int error)
{
    switch (error) {
        case 0:
            return "Success";
        case EAI_BADFLAGS:
            return "Invalid resolver flags";
        case EAI_NONAME:
            return "Name or service not found";
        case EAI_AGAIN:
            return "Temporary name resolution failure";
        case EAI_FAIL:
            return "Invalid DNS response or permanent resolver failure";
        case EAI_FAMILY:
            return "Address family not supported";
        case EAI_SOCKTYPE:
            return "Socket type not supported";
        case EAI_SERVICE:
            return "Service or protocol not supported";
        case EAI_MEMORY:
            return "Resolver out of memory";
        case EAI_SYSTEM:
            return "Resolver system error";
        default:
            return "Unknown resolver error";
    }
}
struct hostent *gethostbyname(const char *name)
{
    static struct dns_result result;
    static struct hostent host;
    static char *addresses[DNS_MAX_ADDRESSES + 1];
    static char *aliases[1];
    int error = resolve(name, &result);
    if (error) {
        h_errno = error == EAI_NONAME  ? HOST_NOT_FOUND
                  : error == EAI_AGAIN ? TRY_AGAIN
                                       : NO_RECOVERY;
        return NULL;
    }
    for (unsigned i = 0; i < result.count; i++)
        addresses[i] = (char *)&result.addresses[i];
    addresses[result.count] = NULL;
    host.h_name = result.canonical;
    host.h_aliases = aliases;
    host.h_addrtype = AF_INET;
    host.h_length = 4;
    host.h_addr_list = addresses;
    h_errno = 0;
    return &host;
}
