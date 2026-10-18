#include "http.h"
struct url {
    char host[254], path[HTTP_URL_LIMIT];
    uint16_t port;
};
struct response {
    unsigned status;
    bool has_length, chunked;
    uint64_t length;
    char location[HTTP_URL_LIMIT];
};
static int parse_url(const char *text, struct url *url)
{
    if (strncasecmp(text, "http://", 7)) {
        fprintf(stderr, "wget: only http:// URLs are supported\n");
        return -1;
    }
    text += 7;
    const char *end = text;
    while (*end && *end != '/' && *end != '?' && *end != '#')
        end++;
    size_t length = end - text;
    if (!length || length >= 262 || memchr(text, '@', length) || memchr(text, '[', length))
        return -1;
    const char *colon = memchr(text, ':', length);
    size_t host_length = colon ? (size_t)(colon - text) : length;
    if (!host_length || host_length >= sizeof(url->host))
        return -1;
    for (size_t i = 0; i < host_length; i++) {
        unsigned c = (uint8_t)text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_'))
            return -1;
    }
    memcpy(url->host, text, host_length);
    url->host[host_length] = 0;
    url->port = 80;
    if (colon) {
        char port[6];
        size_t count = end - colon - 1;
        if (!count || count >= sizeof(port))
            return -1;
        memcpy(port, colon + 1, count);
        port[count] = 0;
        if (http_port(port, &url->port))
            return -1;
    }
    size_t at = 0;
    if (*end != '/')
        url->path[at++] = '/';
    while (*end && *end != '#') {
        unsigned c = (uint8_t)*end++;
        if (c <= 32 || c >= 127 || c == '\\' || at + 1 >= sizeof(url->path))
            return -1;
        url->path[at++] = c;
    }
    url->path[at] = 0;
    return 0;
}
static int redirect_url(struct url *url, const char *location)
{
    if (!*location)
        return -1;
    const char *colon = strchr(location, ':');
    if (!strncasecmp(location, "http://", 7) ||
        (colon && (size_t)(colon - location) < strcspn(location, "/?#")))
        return parse_url(location, url);
    char text[HTTP_URL_LIMIT + 280];
    int length;
    if (location[0] == '/' && location[1] == '/') {
        length = snprintf(text, sizeof(text), "http:%s", location);
    } else {
        char path[HTTP_URL_LIMIT];
        strcpy(path, url->path);
        char *query = strchr(path, '?');
        if (query)
            *query = 0;
        if (location[0] == '/')
            path[0] = 0;
        else if (location[0] != '?' && location[0] != '#') {
            char *slash = strrchr(path, '/');
            if (!slash)
                return -1;
            slash[1] = 0;
        }
        length =
            snprintf(text, sizeof(text), "http://%s:%u%s%s", url->host, url->port, path, location);
    }
    if (length < 0 || (size_t)length >= sizeof(text))
        return -1;
    struct url next;
    if (parse_url(text, &next))
        return -1;
    char normalized[HTTP_URL_LIMIT];
    size_t used = 1;
    normalized[0] = '/';
    const char *p = next.path;
    while (*p && *p != '?') {
        while (*p == '/')
            p++;
        if (!*p || *p == '?')
            break;
        const char *end = p;
        while (*end && *end != '/' && *end != '?')
            end++;
        size_t count = end - p;
        if (count == 2 && p[0] == '.' && p[1] == '.') {
            if (used > 1) {
                used--;
                while (used > 1 && normalized[used - 1] != '/')
                    used--;
            }
        } else if (!(count == 1 && p[0] == '.')) {
            if (used > 1 && normalized[used - 1] != '/')
                normalized[used++] = '/';
            if (used + count + 2 >= sizeof(normalized))
                return -1;
            memcpy(normalized + used, p, count);
            used += count;
        }
        if (*end == '/' && used > 1 && normalized[used - 1] != '/')
            normalized[used++] = '/';
        p = end;
    }
    size_t rest = strlen(p);
    if (used + rest >= sizeof(normalized))
        return -1;
    memcpy(normalized + used, p, rest + 1);
    strcpy(next.path, normalized);
    *url = next;
    return 0;
}
static int read_response(struct http_reader *reader, struct response *response)
{
    char line[HTTP_LINE_LIMIT];
    size_t budget = HTTP_HEADER_LIMIT;
    for (unsigned interim = 0; interim < 9; interim++) {
        memset(response, 0, sizeof(*response));
        if (http_line(reader, line, sizeof(line), &budget))
            return -1;
        errno = EINVAL;
        if ((strncmp(line, "HTTP/1.0 ", 9) && strncmp(line, "HTTP/1.1 ", 9)) || strlen(line) < 12 ||
            line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' || line[11] < '0' ||
            line[11] > '9' || (line[12] && line[12] != ' '))
            return -1;
        response->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
        bool encoding = false;
        for (;;) {
            if (http_line(reader, line, sizeof(line), &budget))
                return -1;
            if (!*line)
                break;
            errno = EINVAL;
            char *name, *value;
            if (http_header(line, &name, &value))
                return -1;
            if (!strcasecmp(name, "Content-Length")) {
                uint64_t length;
                if (http_number(value, &length) ||
                    (response->has_length && length != response->length))
                    return -1;
                response->has_length = true;
                response->length = length;
            } else if (!strcasecmp(name, "Transfer-Encoding")) {
                if (response->chunked || strcasecmp(value, "chunked"))
                    return -1;
                response->chunked = true;
            } else if (!strcasecmp(name, "Content-Encoding")) {
                encoding |= *value && strcasecmp(value, "identity");
            } else if (!strcasecmp(name, "Location")) {
                if (response->location[0] || strlen(value) >= sizeof(response->location))
                    return -1;
                strcpy(response->location, value);
            }
        }
        if (response->has_length && response->chunked)
            return -1;
        if (response->status >= 200) {
            if (encoding && response->status < 300) {
                fprintf(stderr, "wget: unsupported content encoding\n");
                return -1;
            }
            if (response->status == 204 || response->status == 205) {
                if (response->chunked || (response->has_length && response->length))
                    return -1;
                response->has_length = true;
                response->length = 0;
            }
            return 0;
        }
        if (response->status == 101)
            return -1;
    }
    return -1;
}
static int copy_body(struct http_reader *reader, int output, uint64_t length, bool exact,
                     uint64_t *total)
{
    uint8_t data[4096];
    while (!exact || length) {
        size_t amount = sizeof(data);
        if (exact && length < amount)
            amount = length;
        ssize_t count = http_read(reader, data, amount);
        if (count < 0)
            return -1;
        if (!count) {
            if (exact) {
                errno = EIO;
                return -1;
            }
            return 0;
        }
        if ((uint64_t)count > UINT64_MAX - *total) {
            errno = EOVERFLOW;
            return -1;
        }
        if (http_file_write(output, data, count))
            return -1;
        *total += count;
        if (exact)
            length -= count;
    }
    return 0;
}
static int copy_chunked(struct http_reader *reader, int output, uint64_t *total)
{
    char line[HTTP_LINE_LIMIT];
    for (;;) {
        size_t budget = HTTP_LINE_LIMIT;
        if (http_line(reader, line, sizeof(line), &budget))
            return -1;
        errno = EINVAL;
        char *p = line;
        uint64_t length = 0;
        unsigned digits = 0;
        for (; http_hex((uint8_t)*p) >= 0; p++) {
            unsigned digit = http_hex((uint8_t)*p);
            if (length > (UINT64_MAX - digit) / 16)
                return -1;
            length = length * 16 + digit;
            digits++;
        }
        if (!digits || (*p && *p != ';'))
            return -1;
        if (!length) {
            budget = HTTP_HEADER_LIMIT;
            for (;;) {
                if (http_line(reader, line, sizeof(line), &budget))
                    return -1;
                if (!*line)
                    return 0;
                errno = EINVAL;
                char *name, *value;
                if (http_header(line, &name, &value) || !strcasecmp(name, "Content-Length") ||
                    !strcasecmp(name, "Transfer-Encoding"))
                    return -1;
            }
        }
        if (copy_body(reader, output, length, true, total))
            return -1;
        uint8_t ending[2];
        for (unsigned i = 0; i < 2; i++)
            if (http_read(reader, &ending[i], 1) != 1)
                return -1;
        if (ending[0] != '\r' || ending[1] != '\n') {
            errno = EINVAL;
            return -1;
        }
    }
}
static void default_name(const struct url *url, char *name)
{
    const char *end = strchr(url->path, '?');
    if (!end)
        end = url->path + strlen(url->path);
    const char *start = end;
    while (start > url->path && start[-1] != '/')
        start--;
    size_t used = 0;
    bool valid = end > start;
    while (start < end && used < NAME_MAX) {
        unsigned c = (uint8_t)*start++;
        if (c == '%') {
            if (end - start < 2 || http_hex(start[0]) < 0 || http_hex(start[1]) < 0) {
                valid = false;
                break;
            }
            c = http_hex(start[0]) * 16 + http_hex(start[1]);
            start += 2;
        }
        if (c <= 32 || c == 127 || c == '/' || c == '\\') {
            valid = false;
            break;
        }
        name[used++] = c;
    }
    name[used] = 0;
    if (!valid || start != end || !strcmp(name, ".") || !strcmp(name, ".."))
        strcpy(name, "index.html");
}
int wget_main(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        puts("usage: wget [-q] [-T SECONDS] [-O FILE|-] http://HOST[:PORT]/PATH");
        return 0;
    }
    const char *output_name = NULL, *address = NULL;
    unsigned timeout = 30;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q"))
            quiet = true;
        else if (!strcmp(argv[i], "-O") && i + 1 < argc)
            output_name = argv[++i];
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) {
            uint64_t value;
            if (http_number(argv[++i], &value) || !value || value > 3600)
                goto usage;
            timeout = value;
        } else if (!address && argv[i][0] != '-')
            address = argv[i];
        else
            goto usage;
    }
    if (!address)
        goto usage;
    struct url url;
    if (parse_url(address, &url)) {
        fprintf(stderr, "wget: invalid or unsupported URL\n");
        return 1;
    }
    for (unsigned redirects = 0; redirects <= 10; redirects++) {
        int fd = http_connect(url.host, url.port, timeout);
        if (fd < 0) {
            perror("wget connect");
            return 1;
        }
        char request[HTTP_URL_LIMIT + 512], host[262];
        if (url.port == 80)
            snprintf(host, sizeof(host), "%s", url.host);
        else
            snprintf(host, sizeof(host), "%s:%u", url.host, url.port);
        int length =
            snprintf(request, sizeof(request),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: KairoBSD-wget/1\r\n"
                     "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                     url.path, host);
        if (length < 0 || (size_t)length >= sizeof(request) ||
            http_send_all(fd, request, length, timeout)) {
            perror("wget request");
            close(fd);
            return 1;
        }
        struct http_reader reader = {.fd = fd, .timeout = timeout};
        struct response response;
        errno = EINVAL;
        if (read_response(&reader, &response)) {
            perror("wget response");
            close(fd);
            return 1;
        }
        if (response.status == 301 || response.status == 302 || response.status == 303 ||
            response.status == 307 || response.status == 308) {
            close(fd);
            if (redirects == 10 || redirect_url(&url, response.location)) {
                fprintf(stderr, "wget: invalid redirect or redirect limit exceeded\n");
                return 1;
            }
            continue;
        }
        if (response.status < 200 || response.status >= 300 || response.status == 206) {
            fprintf(stderr, "wget: HTTP %u\n", response.status);
            close(fd);
            return 1;
        }
        char filename[PATH_MAX], base[NAME_MAX + 1];
        int output = -1;
        if (output_name) {
            if (!strcmp(output_name, "-"))
                output = 1;
            else
                output = open(output_name, O_WRONLY | O_CREAT | O_TRUNC, 0666U);
        } else {
            default_name(&url, base);
            for (unsigned suffix = 0; suffix < 1000; suffix++) {
                int count = suffix ? snprintf(filename, sizeof(filename), "%s.%u", base, suffix)
                                   : snprintf(filename, sizeof(filename), "%s", base);
                if (count < 0 || (size_t)count >= sizeof(filename)) {
                    errno = ENAMETOOLONG;
                    break;
                }
                output = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0666U);
                if (output >= 0 || errno != EEXIST)
                    break;
            }
        }
        if (output < 0) {
            perror("wget output");
            close(fd);
            return 1;
        }
        uint64_t total = 0;
        int error = response.chunked
                        ? copy_chunked(&reader, output, &total)
                        : copy_body(&reader, output, response.length, response.has_length, &total);
        int saved = errno;
        close(fd);
        if (output != 1 && close(output) && !error) {
            error = -1;
            saved = errno;
        }
        if (error) {
            errno = saved;
            perror("wget download incomplete");
            if (output != 1)
                fprintf(stderr, "wget: partial output retained in %s (%lu bytes)\n",
                        output_name ? output_name : filename, total);
            return 1;
        }
        if (!quiet)
            fprintf(stderr, "wget: saved %lu bytes to %s\n", total,
                    output_name ? output_name : filename);
        return 0;
    }
usage:
    fprintf(stderr, "usage: wget [-q] [-T SECONDS] [-O FILE|-] http://HOST[:PORT]/PATH\n");
    return 1;
}
