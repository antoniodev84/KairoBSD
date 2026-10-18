#include "../include/command.h"
#include <arpa/inet.h>
#include <netdb.h>
static uint64_t now_ns(void)
{
    struct ksysinfo info;
    return sysinfo(&info) ? 0 : info.uptime_ns;
}
static void wait_network(void)
{
    struct ktimespec delay = {0, 10000000};
    nanosleep(&delay, NULL);
}
static int parse_interface(const char *name)
{
    if (name[0] != 'e' || name[1] != 'n' || name[2] < '0' || name[2] > '9')
        return -1;
    unsigned index = 0;
    for (const char *p = name + 2; *p; p++) {
        if (*p < '0' || *p > '9' || index > 100)
            return -1;
        index = index * 10 + *p - '0';
    }
    return index;
}
static void show_interface(const struct knetconfig *config)
{
    char address[16], mask[16], gateway[16], dns[2][16];
    inet_ntop(AF_INET, &config->address, address, sizeof(address));
    inet_ntop(AF_INET, &config->netmask, mask, sizeof(mask));
    inet_ntop(AF_INET, &config->gateway, gateway, sizeof(gateway));
    inet_ntop(AF_INET, &config->dns[0], dns[0], sizeof(dns[0]));
    inet_ntop(AF_INET, &config->dns[1], dns[1], sizeof(dns[1]));
    printf("en%u %s %02x:%02x:%02x:%02x:%02x:%02x\n  inet %s netmask %s gateway %s\n",
           config->index, config->up ? "UP" : "DOWN", config->mac[0], config->mac[1],
           config->mac[2], config->mac[3], config->mac[4], config->mac[5], address, mask, gateway);
    const char *states[] = {"disabled", "selecting", "requesting", "probing",
                            "bound",    "renewing",  "rebinding"};
    const char *state = config->dhcp_state < sizeof(states) / sizeof(states[0])
                            ? states[config->dhcp_state]
                            : "unknown";
    printf("  dns %s %s dhcp %s lease %lu s\n", dns[0], dns[1], state, config->lease_remaining);
}
static int interface_command(int argc, char **argv)
{
    if (argc == 1) {
        for (unsigned i = 0; i < 4; i++) {
            struct knetconfig config = {.index = i};
            if (netconfig(&config, 0)) {
                if (errno == ENODEV)
                    break;
                perror("ifconfig");
                return 1;
            }
            show_interface(&config);
        }
        return 0;
    }
    int index = parse_interface(argv[1]);
    if (index < 0 || (argc != 2 && argc != 3 && argc != 4 && argc != 5)) {
        fprintf(stderr,
                "usage: ifconfig [enN [up|down|dhcp|dns DNS1 [DNS2]|ADDRESS NETMASK GATEWAY]]\n");
        return 1;
    }
    struct knetconfig config = {.index = index};
    if (netconfig(&config, 0)) {
        perror("ifconfig");
        return 1;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[2], "dns")) {
        if (inet_pton(AF_INET, argv[3], &config.dns[0]) != 1 ||
            (argc == 5 && inet_pton(AF_INET, argv[4], &config.dns[1]) != 1)) {
            fprintf(stderr, "ifconfig: invalid DNS server\n");
            return 1;
        }
        if (argc == 4)
            config.dns[1] = 0;
    } else if (argc == 3 && !strcmp(argv[2], "dhcp")) {
        config.dhcp = 1;
        config.up = 1;
    } else if (argc == 3) {
        if (strcmp(argv[2], "up") && strcmp(argv[2], "down")) {
            fprintf(stderr, "ifconfig: expected up or down\n");
            return 1;
        }
        config.up = !strcmp(argv[2], "up");
    } else if (argc == 5) {
        if (inet_pton(AF_INET, argv[2], &config.address) != 1 ||
            inet_pton(AF_INET, argv[3], &config.netmask) != 1 ||
            inet_pton(AF_INET, argv[4], &config.gateway) != 1) {
            fprintf(stderr, "ifconfig: invalid IPv4 address\n");
            return 1;
        }
        config.up = 1;
        config.dhcp = 0;
    } else if (argc != 2) {
        fprintf(stderr, "ifconfig: invalid arguments\n");
        return 1;
    }
    if (argc > 2 && netconfig(&config, 1)) {
        perror("ifconfig");
        return 1;
    }
    show_interface(&config);
    return 0;
}
static int resolve_address(const char *name, struct in_addr *address, int type)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = type};
    struct addrinfo *result;
    int error = getaddrinfo(name, NULL, &hints, &result);
    if (error) {
        fprintf(stderr, "%s: %s\n", name, gai_strerror(error));
        return -1;
    }
    *address = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return 0;
}
static int lookup_command(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: nslookup NAME\n");
        return 1;
    }
    struct addrinfo hints = {
        .ai_family = AF_INET, .ai_socktype = SOCK_STREAM, .ai_flags = AI_CANONNAME};
    struct addrinfo *result;
    int error = getaddrinfo(argv[1], NULL, &hints, &result);
    if (error) {
        fprintf(stderr, "nslookup: %s\n", gai_strerror(error));
        return 1;
    }
    printf("Name: %s\n", result->ai_canonname ? result->ai_canonname : argv[1]);
    for (struct addrinfo *entry = result; entry; entry = entry->ai_next) {
        char text[INET_ADDRSTRLEN];
        struct sockaddr_in *address = (struct sockaddr_in *)entry->ai_addr;
        printf("Address: %s\n", inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)));
    }
    freeaddrinfo(result);
    return 0;
}
static int ping_command(int argc, char **argv)
{
    struct sockaddr_in target = {.sin_family = AF_INET};
    if (argc != 2) {
        fprintf(stderr, "usage: ping HOST\n");
        return 1;
    }
    if (resolve_address(argv[1], &target.sin_addr, SOCK_DGRAM))
        return 1;
    int fd = socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK, IPPROTO_ICMP);
    if (fd < 0) {
        perror("ping socket");
        return 1;
    }
    unsigned received = 0;
    uint16_t identifier = getpid();
    for (unsigned sequence = 1; sequence <= 4; sequence++) {
        uint8_t request[64] = {8};
        request[4] = identifier >> 8;
        request[5] = identifier;
        request[6] = sequence >> 8;
        request[7] = sequence;
        for (unsigned i = 8; i < sizeof(request); i++)
            request[i] = i;
        uint64_t start = now_ns();
        if (sendto(fd, request, sizeof(request), 0, (const struct sockaddr *)&target,
                   sizeof(target)) < 0) {
            perror("ping send");
            close(fd);
            return 1;
        }
        bool matched = false;
        while (now_ns() - start < 1000000000) {
            uint8_t response[2048];
            struct sockaddr_in source;
            socklen_t size = sizeof(source);
            ssize_t length =
                recvfrom(fd, response, sizeof(response), 0, (struct sockaddr *)&source, &size);
            if (length >= 8 && source.sin_addr.s_addr == target.sin_addr.s_addr &&
                response[0] == 0 && response[1] == 0 && response[4] == request[4] &&
                response[5] == request[5] && response[6] == request[6] &&
                response[7] == request[7]) {
                printf("%ld bytes from %s: seq=%u time=%lu us\n", length, argv[1], sequence,
                       (now_ns() - start) / 1000);
                received++;
                matched = true;
                break;
            }
            if (length < 0 && errno != EAGAIN) {
                perror("ping receive");
                close(fd);
                return 1;
            }
            wait_network();
        }
        if (!matched)
            printf("timeout: seq=%u\n", sequence);
    }
    printf("4 transmitted, %u received\n", received);
    close(fd);
    return received ? 0 : 1;
}
static int output_all(int fd, const uint8_t *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, data + offset, length - offset);
        if (count < 0) {
            if (errno == EAGAIN) {
                wait_network();
                continue;
            }
            return -1;
        }
        if (!count)
            return -1;
        offset += count;
    }
    return 0;
}
static int netcat_command(int argc, char **argv)
{
    bool udp = false, listener = false;
    int at = 1;
    while (at < argc && argv[at][0] == '-') {
        if (!strcmp(argv[at], "-u"))
            udp = true;
        else if (!strcmp(argv[at], "-l"))
            listener = true;
        else
            break;
        at++;
    }
    struct sockaddr_in address = {.sin_family = AF_INET};
    if (argc - at != 2) {
        fprintf(stderr, "usage: nc [-u] [-l] HOST PORT\n");
        return 1;
    }
    if (resolve_address(argv[at], &address.sin_addr, udp ? SOCK_DGRAM : SOCK_STREAM))
        return 1;
    unsigned port = 0;
    for (const char *p = argv[at + 1]; *p; p++) {
        if (*p < '0' || *p > '9' || port > 6553) {
            port = 0;
            break;
        }
        port = port * 10 + *p - '0';
    }
    if (!port || port > 65535) {
        fprintf(stderr, "nc: invalid port\n");
        return 1;
    }
    address.sin_port = htons(port);
    int fd = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) {
        perror("nc socket");
        return 1;
    }
    if (listener) {
        if (bind(fd, (const struct sockaddr *)&address, sizeof(address)))
            goto failure;
        if (!udp) {
            if (listen(fd, 8))
                goto failure;
            int client = accept(fd, NULL, NULL);
            if (client < 0)
                goto failure;
            close(fd);
            fd = client;
        } else {
            uint8_t *first = malloc(65507);
            if (!first)
                goto failure;
            socklen_t length = sizeof(address);
            ssize_t count = recvfrom(fd, first, 65507, 0, (struct sockaddr *)&address, &length);
            if (count < 0 || connect(fd, (const struct sockaddr *)&address, sizeof(address))) {
                free(first);
                goto failure;
            }
            int result = output_all(1, first, count);
            free(first);
            if (result)
                goto failure;
        }
    } else if (connect(fd, (const struct sockaddr *)&address, sizeof(address))) {
        goto failure;
    }
    int input = 0;
    struct kstat input_stat;
    if (!fstat(0, &input_stat) && (input_stat.mode & S_IFMT) == S_IFCHR) {
        int console = open("/dev/console", O_RDONLY | O_NONBLOCK);
        if (console >= 0) {
            struct kstat console_stat;
            if (!fstat(console, &console_stat) && console_stat.ino == input_stat.ino)
                input = console;
            else
                close(console);
        }
    }
    uint8_t *data = malloc(65535);
    if (!data) {
        if (input)
            close(input);
        goto failure;
    }
    int stdin_flags = fcntl(input, F_GETFL);
    if (stdin_flags < 0 || fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(input, F_SETFL, stdin_flags | O_NONBLOCK) < 0) {
        if (input)
            close(input);
        free(data);
        goto failure;
    }
    bool input_done = false;
    uint64_t drain_deadline = 0;
    int status = 0;
    for (;;) {
        ssize_t count = recv(fd, data, udp ? 65535 : 4096, MSG_DONTWAIT);
        if (count > 0) {
            if (output_all(1, data, count)) {
                status = 1;
                break;
            }
        } else if (!count && !udp) {
            break;
        } else if (count < 0 && errno != EAGAIN) {
            perror("nc receive");
            status = 1;
            break;
        }
        if (!input_done) {
            count = read(input, data, udp ? 65507 : 4096);
            if (count > 0) {
                if (output_all(fd, data, count)) {
                    perror("nc send");
                    status = 1;
                    break;
                }
            } else if (!count) {
                input_done = true;
                if (udp)
                    drain_deadline = now_ns() + 1000000000;
                if (!udp && shutdown(fd, SHUT_WR)) {
                    perror("nc shutdown");
                    status = 1;
                    break;
                }
            } else if (errno != EAGAIN) {
                perror("nc stdin");
                status = 1;
                break;
            }
        }
        if (drain_deadline && now_ns() >= drain_deadline)
            break;
        wait_network();
    }
    if (fcntl(input, F_SETFL, stdin_flags) < 0)
        status = 1;
    if (input)
        close(input);
    free(data);
    close(fd);
    return status;
failure:
    perror("nc");
    close(fd);
    return 1;
}
int main(int argc, char **argv)
{
    if (command_is(ifconfig))
        return interface_command(argc, argv);
    if (command_is(nslookup))
        return lookup_command(argc, argv);
    if (command_is(ping))
        return ping_command(argc, argv);
    return netcat_command(argc, argv);
}
