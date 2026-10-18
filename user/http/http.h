#ifndef KAIRO_HTTP_H
#define KAIRO_HTTP_H
#include <arpa/inet.h>
#include <netdb.h>
#define HTTP_HEADER_LIMIT 16384
#define HTTP_LINE_LIMIT 4096
#define HTTP_URL_LIMIT 2048
struct http_reader {
    int fd;
    unsigned timeout;
    uint8_t data[4096];
    size_t offset, used;
};
uint64_t http_now(void);
int http_pause(uint64_t);
int http_send_all(int, const void *, size_t, unsigned);
int http_file_write(int, const void *, size_t);
int http_connect(const char *, uint16_t, unsigned);
ssize_t http_read(struct http_reader *, void *, size_t);
int http_line(struct http_reader *, char *, size_t, size_t *);
int http_header(char *, char **, char **);
int http_number(const char *, uint64_t *);
int http_port(const char *, uint16_t *);
int http_hex(unsigned);
int wget_main(int, char **);
int httpd_main(int, char **);
#endif
