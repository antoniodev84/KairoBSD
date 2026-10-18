#include "../include/command.h"
#include "http.h"
uint64_t http_now(void)
{
    struct ksysinfo info;
    return sysinfo(&info) ? 0 : info.uptime_ns;
}
int http_pause(uint64_t deadline)
{
    if (http_now() >= deadline) {
        errno = ETIMEDOUT;
        return -1;
    }
    struct ktimespec delay = {0, 10000000};
    return nanosleep(&delay, NULL);
}
int http_file_write(int fd, const void *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, (const uint8_t *)data + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!count) {
            errno = EIO;
            return -1;
        }
        offset += count;
    }
    return 0;
}
int http_send_all(int fd, const void *data, size_t length, unsigned timeout)
{
    size_t offset = 0;
    uint64_t deadline = http_now() + (uint64_t)timeout * 1000000000;
    while (offset < length) {
        ssize_t count = send(fd, (const uint8_t *)data + offset, length - offset, MSG_NOSIGNAL);
        if (count > 0) {
            offset += count;
            deadline = http_now() + (uint64_t)timeout * 1000000000;
        } else if (count < 0 && errno == EAGAIN) {
            if (http_pause(deadline))
                return -1;
        } else {
            if (!count)
                errno = EPIPE;
            return -1;
        }
    }
    return 0;
}
int http_connect(const char *host, uint16_t port, unsigned timeout)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses;
    int error = getaddrinfo(host, NULL, &hints, &addresses);
    if (error) {
        fprintf(stderr, "%s: %s\n", host, gai_strerror(error));
        errno = error == EAI_SYSTEM ? errno : ENETUNREACH;
        return -1;
    }
    int result = -1, saved = ECONNREFUSED;
    for (struct addrinfo *entry = addresses; entry; entry = entry->ai_next) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            saved = errno;
            break;
        }
        struct sockaddr_in address = *(struct sockaddr_in *)entry->ai_addr;
        address.sin_port = htons(port);
        uint64_t deadline = http_now() + (uint64_t)timeout * 1000000000;
        for (;;) {
            if (!connect(fd, (struct sockaddr *)&address, sizeof(address))) {
                result = fd;
                break;
            }
            if (errno != EINPROGRESS && errno != EALREADY)
                break;
            if (http_pause(deadline))
                break;
        }
        saved = errno;
        if (result >= 0)
            break;
        close(fd);
        if (saved == EINTR)
            break;
    }
    freeaddrinfo(addresses);
    errno = saved;
    return result;
}
ssize_t http_read(struct http_reader *reader, void *data, size_t length)
{
    if (!length)
        return 0;
    if (reader->offset == reader->used) {
        uint64_t deadline = http_now() + (uint64_t)reader->timeout * 1000000000;
        for (;;) {
            ssize_t count = recv(reader->fd, reader->data, sizeof(reader->data), 0);
            if (count < 0 && errno == EAGAIN) {
                if (http_pause(deadline))
                    return -1;
                continue;
            }
            if (count <= 0)
                return count;
            reader->offset = 0;
            reader->used = count;
            break;
        }
    }
    size_t available = reader->used - reader->offset;
    if (length > available)
        length = available;
    memcpy(data, reader->data + reader->offset, length);
    reader->offset += length;
    return length;
}
int http_line(struct http_reader *reader, char *line, size_t capacity, size_t *budget)
{
    size_t used = 0;
    bool carriage = false;
    for (;;) {
        unsigned char byte;
        ssize_t count = http_read(reader, &byte, 1);
        if (count <= 0) {
            if (!count)
                errno = EIO;
            return -1;
        }
        if (!*budget || used + 1 >= capacity) {
            errno = EOVERFLOW;
            return -1;
        }
        (*budget)--;
        if (carriage) {
            if (byte != '\n') {
                errno = EINVAL;
                return -1;
            }
            line[used] = 0;
            return 0;
        }
        if (byte == '\r') {
            carriage = true;
        } else if (byte == '\n') {
            line[used] = 0;
            return 0;
        } else if (!byte || (byte < 32 && byte != '\t') || byte == 127) {
            errno = EINVAL;
            return -1;
        } else {
            line[used++] = byte;
        }
    }
}
static bool token(unsigned c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           strchr("!#$%&'*+-.^_`|~", c);
}
int http_header(char *line, char **name, char **value)
{
    char *colon = strchr(line, ':');
    if (!colon || colon == line)
        return -1;
    for (char *p = line; p < colon; p++)
        if (!token((uint8_t)*p))
            return -1;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t')
        colon++;
    char *end = colon + strlen(colon);
    while (end > colon && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = 0;
    *name = line;
    *value = colon;
    return 0;
}
int http_number(const char *text, uint64_t *output)
{
    if (!*text)
        return -1;
    uint64_t value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || value > (UINT64_MAX - (*text - '0')) / 10)
            return -1;
        value = value * 10 + *text - '0';
    }
    *output = value;
    return 0;
}
int http_port(const char *text, uint16_t *output)
{
    uint64_t value;
    if (http_number(text, &value) || !value || value > 65535)
        return -1;
    *output = value;
    return 0;
}
int http_hex(unsigned c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
int main(int argc, char **argv)
{
    return command_is(httpd) ? httpd_main(argc, argv) : wget_main(argc, argv);
}
