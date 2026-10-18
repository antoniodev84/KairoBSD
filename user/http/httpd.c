#include "http.h"
#include <time.h>
#define HTTP_CLIENTS 24
#define HTTP_BUFFER_SIZE 8192
struct client {
    int socket, file;
    char *request;
    size_t request_length;
    uint8_t output[HTTP_BUFFER_SIZE];
    size_t output_length, output_offset;
    uint64_t remaining, deadline;
    bool responding, head;
};
static volatile sig_atomic_t stopping;
static void stop_server(int signo)
{
    stopping = 1;
}
static void close_client(struct client *client)
{
    if (client->socket >= 0)
        close(client->socket);
    if (client->file >= 0)
        close(client->file);
    free(client->request);
    memset(client, 0, sizeof(*client));
    client->socket = client->file = -1;
}
static const char *reason(unsigned status)
{
    switch (status) {
        case 200:
            return "OK";
        case 206:
            return "Partial Content";
        case 301:
            return "Moved Permanently";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 414:
            return "URI Too Long";
        case 416:
            return "Range Not Satisfiable";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
    }
}
static void http_date(char *text, size_t capacity)
{
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t now = time(NULL);
    struct tm *date = gmtime(&now);
    if (!date || date->tm_wday < 0 || date->tm_wday > 6 || date->tm_mon < 0 || date->tm_mon > 11) {
        text[0] = 0;
        return;
    }
    snprintf(text, capacity, "Date: %s, %02d %s %04d %02d:%02d:%02d GMT\r\n", days[date->tm_wday],
             date->tm_mday, months[date->tm_mon], date->tm_year + 1900, date->tm_hour, date->tm_min,
             date->tm_sec);
}
static int response_header(struct client *client, unsigned status, uint64_t length,
                           const char *type, const char *extra)
{
    char date[80];
    http_date(date, sizeof(date));
    int count = snprintf((char *)client->output, sizeof(client->output),
                         "HTTP/1.1 %u %s\r\n%sServer: KairoBSD-httpd/1\r\n"
                         "Content-Length: %lu\r\nContent-Type: %s\r\n"
                         "Connection: close\r\nX-Content-Type-Options: nosniff\r\n%s\r\n",
                         status, reason(status), date, length, type, extra ? extra : "");
    if (count < 0 || (size_t)count >= sizeof(client->output))
        return -1;
    client->output_length = count;
    client->output_offset = 0;
    client->responding = true;
    client->deadline = http_now() + 30000000000UL;
    free(client->request);
    client->request = NULL;
    return 0;
}
static int error_response(struct client *client, unsigned status, const char *extra)
{
    if (client->file >= 0) {
        close(client->file);
        client->file = -1;
    }
    client->remaining = 0;
    char body[128];
    int length = snprintf(body, sizeof(body), "%u %s\n", status, reason(status));
    if (response_header(client, status, length, "text/plain; charset=utf-8", extra))
        return -1;
    if (!client->head) {
        memcpy(client->output + client->output_length, body, length);
        client->output_length += length;
    }
    return 0;
}
static const char *content_type(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (!extension)
        return "application/octet-stream";
    if (!strcasecmp(extension, ".html") || !strcasecmp(extension, ".htm"))
        return "text/html; charset=utf-8";
    if (!strcasecmp(extension, ".txt") || !strcasecmp(extension, ".css") ||
        !strcasecmp(extension, ".csv"))
        return !strcasecmp(extension, ".css") ? "text/css; charset=utf-8"
                                              : "text/plain; charset=utf-8";
    if (!strcasecmp(extension, ".js"))
        return "text/javascript; charset=utf-8";
    if (!strcasecmp(extension, ".json"))
        return "application/json";
    if (!strcasecmp(extension, ".png"))
        return "image/png";
    if (!strcasecmp(extension, ".jpg") || !strcasecmp(extension, ".jpeg"))
        return "image/jpeg";
    if (!strcasecmp(extension, ".gif"))
        return "image/gif";
    if (!strcasecmp(extension, ".svg"))
        return "image/svg+xml";
    if (!strcasecmp(extension, ".ico"))
        return "image/x-icon";
    if (!strcasecmp(extension, ".pdf"))
        return "application/pdf";
    if (!strcasecmp(extension, ".wasm"))
        return "application/wasm";
    return "application/octet-stream";
}
static int decode_path(const char *target, char *output)
{
    if (*target != '/' || target[1] == '/' || strchr(target, '#'))
        return 400;
    size_t used = 0;
    while (*target && *target != '?') {
        unsigned c = (uint8_t)*target++;
        if (c == '%') {
            if (!target[0] || !target[1] || http_hex(target[0]) < 0 || http_hex(target[1]) < 0)
                return 400;
            c = http_hex(target[0]) * 16 + http_hex(target[1]);
            target += 2;
        }
        if (c <= 31 || c == 127 || c == '\\')
            return 400;
        if (used + 1 >= PATH_MAX)
            return 414;
        output[used++] = c;
    }
    output[used] = 0;
    const char *part = output;
    while (*part) {
        while (*part == '/')
            part++;
        const char *end = part;
        while (*end && *end != '/')
            end++;
        size_t size = end - part;
        if ((size == 1 && part[0] == '.') || (size == 2 && part[0] == '.' && part[1] == '.'))
            return 403;
        part = end;
    }
    size_t first = 0;
    while (first < used && output[first] == '/')
        first++;
    memmove(output, output + first, used - first + 1);
    if (!output[0])
        strcpy(output, ".");
    return 0;
}
static int parse_range(char *text, uint64_t size, uint64_t *offset, uint64_t *length)
{
    if (strncmp(text, "bytes=", 6))
        return 1;
    text += 6;
    if (strchr(text, ','))
        return 1;
    char *dash = strchr(text, '-');
    if (!dash || strchr(dash + 1, '-'))
        return -1;
    *dash++ = 0;
    if (!size)
        return -1;
    uint64_t first, last;
    if (!*text) {
        if (http_number(dash, &last) || !last)
            return -1;
        if (last > size)
            last = size;
        *offset = size - last;
        *length = last;
    } else {
        if (http_number(text, &first) || first >= size)
            return -1;
        if (*dash) {
            if (http_number(dash, &last) || last < first)
                return -1;
            if (last >= size)
                last = size - 1;
        } else {
            last = size - 1;
        }
        *offset = first;
        *length = last - first + 1;
    }
    return 0;
}
static int file_error(struct client *client)
{
    unsigned status = errno == EACCES || errno == ELOOP     ? 403
                      : errno == ENOENT || errno == ENOTDIR ? 404
                      : errno == ENAMETOOLONG               ? 414
                                                            : 500;
    return error_response(client, status, NULL);
}
static int prepare_response(struct client *client)
{
    char *request = client->request;
    char *line_end = strstr(request, "\r\n");
    if (!line_end)
        return error_response(client, 400, NULL);
    *line_end = 0;
    char *target = strchr(request, ' ');
    if (!target)
        return error_response(client, 400, NULL);
    *target++ = 0;
    char *version = strchr(target, ' ');
    if (!version)
        return error_response(client, 400, NULL);
    *version++ = 0;
    bool modern = !strcmp(version, "HTTP/1.1");
    if ((!modern && strcmp(version, "HTTP/1.0")) || strlen(target) >= HTTP_URL_LIMIT)
        return error_response(client, strlen(target) >= HTTP_URL_LIMIT ? 414 : 400, NULL);
    client->head = !strcmp(request, "HEAD");
    if (strcmp(request, "GET") && !client->head)
        return error_response(client, 405, "Allow: GET, HEAD\r\n");
    for (const char *p = target; *p; p++)
        if ((uint8_t)*p <= 32 || (uint8_t)*p >= 127)
            return error_response(client, 400, NULL);
    char *range = NULL;
    unsigned hosts = 0;
    bool content_length = false;
    uint64_t body_length = 0;
    char *line = line_end + 2;
    while (*line) {
        char *end = strstr(line, "\r\n");
        if (!end)
            return error_response(client, 400, NULL);
        *end = 0;
        if (!*line)
            break;
        for (const char *p = line; *p; p++)
            if (((uint8_t)*p < 32 && *p != '\t') || (uint8_t)*p == 127)
                return error_response(client, 400, NULL);
        char *name, *value;
        if (http_header(line, &name, &value))
            return error_response(client, 400, NULL);
        if (!strcasecmp(name, "Host")) {
            if (++hosts > 1 || !*value || strpbrk(value, " \t/\\@#?"))
                return error_response(client, 400, NULL);
        } else if (!strcasecmp(name, "Content-Length")) {
            uint64_t length;
            if (http_number(value, &length) || (content_length && length != body_length))
                return error_response(client, 400, NULL);
            content_length = true;
            body_length = length;
        } else if (!strcasecmp(name, "Transfer-Encoding")) {
            return error_response(client, 400, NULL);
        } else if (!strcasecmp(name, "Range")) {
            if (range)
                return error_response(client, 400, NULL);
            range = value;
        }
        line = end + 2;
    }
    if ((modern && hosts != 1) || body_length)
        return error_response(client, 400, NULL);
    char path[PATH_MAX];
    int status = decode_path(target, path);
    if (status)
        return error_response(client, status, NULL);
    client->file = open(path, O_RDONLY | O_NONBLOCK | O_NOSYMLINKS | O_CLOEXEC);
    if (client->file < 0)
        return file_error(client);
    struct kstat stat;
    if (fstat(client->file, &stat))
        return error_response(client, 500, NULL);
    if (S_ISDIR(stat.mode)) {
        close(client->file);
        client->file = -1;
        const char *query = strchr(target, '?');
        size_t target_length = query ? (size_t)(query - target) : strlen(target);
        if (target_length && target[target_length - 1] != '/') {
            char location[HTTP_URL_LIMIT + 32];
            int count = snprintf(location, sizeof(location), "Location: %.*s/%s\r\n",
                                 (int)target_length, target, query ? query : "");
            if (count < 0 || (size_t)count >= sizeof(location))
                return error_response(client, 414, NULL);
            return error_response(client, 301, location);
        }
        size_t used = strlen(path);
        if (used + 12 >= sizeof(path))
            return error_response(client, 414, NULL);
        if (path[used - 1] != '/')
            path[used++] = '/';
        strcpy(path + used, "index.html");
        client->file = open(path, O_RDONLY | O_NONBLOCK | O_NOSYMLINKS | O_CLOEXEC);
        if (client->file < 0)
            return file_error(client);
        if (fstat(client->file, &stat))
            return error_response(client, 500, NULL);
    }
    if ((stat.mode & S_IFMT) != S_IFREG)
        return error_response(client, 403, NULL);
    uint64_t offset = 0, length = stat.size;
    status = 200;
    char extra[160] = "Accept-Ranges: bytes\r\n";
    if (range && !client->head) {
        int result = parse_range(range, stat.size, &offset, &length);
        if (result < 0) {
            snprintf(extra, sizeof(extra), "Content-Range: bytes */%lu\r\n", stat.size);
            return error_response(client, 416, extra);
        }
        if (!result) {
            status = 206;
            snprintf(extra, sizeof(extra),
                     "Accept-Ranges: bytes\r\nContent-Range: bytes %lu-%lu/%lu\r\n", offset,
                     offset + length - 1, stat.size);
        }
    }
    if (offset && (offset > INT64_MAX || lseek(client->file, offset, SEEK_SET) < 0))
        return error_response(client, 500, NULL);
    client->remaining = client->head ? 0 : length;
    return response_header(client, status, length, content_type(path), extra);
}
static int receive_request(struct client *client)
{
    size_t room = HTTP_HEADER_LIMIT - client->request_length;
    if (!room)
        return error_response(client, 431, NULL);
    ssize_t count = recv(client->socket, client->request + client->request_length, room, 0);
    if (count < 0)
        return errno == EAGAIN ? 0 : -1;
    if (!count)
        return -1;
    size_t previous = client->request_length;
    client->request_length += count;
    client->request[client->request_length] = 0;
    for (size_t i = previous; i < client->request_length; i++)
        if (!client->request[i])
            return error_response(client, 400, NULL);
    if (!strstr(client->request, "\r\n\r\n"))
        return client->request_length == HTTP_HEADER_LIMIT ? error_response(client, 431, NULL) : 0;
    return prepare_response(client);
}
static int send_response(struct client *client)
{
    if (client->output_offset == client->output_length) {
        if (!client->remaining)
            return -1;
        size_t amount =
            client->remaining < sizeof(client->output) ? client->remaining : sizeof(client->output);
        ssize_t count = read(client->file, client->output, amount);
        if (count <= 0) {
            if (count < 0)
                perror("httpd read");
            else
                fprintf(stderr, "httpd: file truncated during transfer\n");
            return -1;
        }
        client->remaining -= count;
        client->output_length = count;
        client->output_offset = 0;
    }
    ssize_t count = send(client->socket, client->output + client->output_offset,
                         client->output_length - client->output_offset, MSG_NOSIGNAL);
    if (count < 0)
        return errno == EAGAIN ? 0 : -1;
    if (!count)
        return -1;
    client->output_offset += count;
    client->deadline = http_now() + 30000000000UL;
    return 0;
}
int httpd_main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("usage: httpd [-f] [-b IPV4] [-p PORT] [-h DIRECTORY]");
        return 0;
    }
    uint16_t port = 8080;
    const char *root = ".", *bind_address = "0.0.0.0";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            if (http_port(argv[++i], &port))
                goto usage;
        } else if (!strcmp(argv[i], "-h") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(argv[i], "-b") && i + 1 < argc)
            bind_address = argv[++i];
        else if (strcmp(argv[i], "-f"))
            goto usage;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1)
        goto usage;
    if (chdir(root)) {
        perror("httpd root");
        return 1;
    }
    signal(SIGINT, stop_server);
    signal(SIGTERM, stop_server);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        perror("httpd socket");
        return 1;
    }
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
        listen(listener, HTTP_CLIENTS)) {
        perror("httpd listen");
        close(listener);
        return 1;
    }
    struct client *clients = calloc(HTTP_CLIENTS, sizeof(*clients));
    if (!clients) {
        close(listener);
        perror("httpd allocation");
        return 1;
    }
    for (unsigned i = 0; i < HTTP_CLIENTS; i++)
        clients[i].socket = clients[i].file = -1;
    printf("httpd: serving %s on http://%s:%u/\n", root, bind_address, port);
    int result = 0;
    while (!stopping) {
        for (unsigned budget = 0; budget < HTTP_CLIENTS; budget++) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    perror("httpd accept");
                    result = 1;
                    stopping = 1;
                }
                break;
            }
            struct client *slot = NULL;
            for (unsigned i = 0; i < HTTP_CLIENTS; i++)
                if (clients[i].socket < 0) {
                    slot = &clients[i];
                    break;
                }
            if (!slot || fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
                close(fd);
                continue;
            }
            slot->request = malloc(HTTP_HEADER_LIMIT + 1);
            if (!slot->request) {
                close(fd);
                continue;
            }
            slot->socket = fd;
            slot->deadline = http_now() + 10000000000UL;
        }
        uint64_t now = http_now();
        for (unsigned i = 0; i < HTTP_CLIENTS; i++) {
            struct client *client = &clients[i];
            if (client->socket < 0)
                continue;
            if (now >= client->deadline) {
                if (!client->responding && !error_response(client, 408, NULL))
                    continue;
                close_client(client);
                continue;
            }
            int error = client->responding ? send_response(client) : receive_request(client);
            if (error)
                close_client(client);
        }
        if (!stopping) {
            struct ktimespec delay = {0, 10000000};
            if (nanosleep(&delay, NULL) && errno != EINTR) {
                perror("httpd wait");
                result = 1;
                break;
            }
        }
    }
    for (unsigned i = 0; i < HTTP_CLIENTS; i++)
        close_client(&clients[i]);
    free(clients);
    close(listener);
    return result;
usage:
    fprintf(stderr, "usage: httpd [-f] [-b IPV4] [-p PORT] [-h DIRECTORY]\n");
    return 1;
}
