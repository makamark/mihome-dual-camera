#include "httputil.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int http_unix_to(const char *sock_path, const char *method, const char *path,
                 const char *body, const char *content_type, int timeout_sec,
                 char *resp, size_t resp_cap)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    /* go2rtc 挂起时不许卡死调用方（g2r_start 的就绪轮询依赖每次调用能返回） */
    struct timeval tv = { .tv_sec = timeout_sec > 0 ? timeout_sec : 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    char head[512];
    int body_len = body ? (int)strlen(body) : 0;
    int hl = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.0\r\n"
                      "Host: localhost\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %d\r\n"
                      "Connection: close\r\n\r\n",
                      method, path, content_type, body_len);
    if (write(fd, head, hl) != hl || (body_len && write(fd, body, body_len) != body_len)) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    while (got + 1 < resp_cap) {
        ssize_t n = read(fd, resp + got, resp_cap - 1 - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    resp[got] = 0;
    char *p = strstr(resp, "\r\n\r\n");
    if (p) {
        size_t body_len2 = got - (size_t)(p + 4 - resp);
        memmove(resp, p + 4, body_len2);
        resp[body_len2] = 0;
    }
    return 0;
}

int http_unix_ex(const char *sock_path, const char *method, const char *path,
                 const char *body, const char *content_type,
                 char *resp, size_t resp_cap)
{
    return http_unix_to(sock_path, method, path, body, content_type, 5, resp, resp_cap);
}

int http_unix(const char *sock_path, const char *method, const char *path,
              const char *body, char *resp, size_t resp_cap)
{
    return http_unix_ex(sock_path, method, path, body, "application/json", resp, resp_cap);
}
