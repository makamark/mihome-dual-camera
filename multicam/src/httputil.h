#ifndef MULTICAM_HTTPUTIL_H
#define MULTICAM_HTTPUTIL_H

#include <stddef.h>

/* 极简 HTTP/1.0 客户端（AF_UNIX，面向 go2rtc api socket）。
   成功返回 0，resp 填充响应体（已剥离头部）。 */
int http_unix(const char *sock_path, const char *method, const char *path,
              const char *body, char *resp, size_t resp_cap);

/* 同上，Content-Type 可指定（xiaomi-phone 要求精确 form-urlencoded）。 */
int http_unix_ex(const char *sock_path, const char *method, const char *path,
                 const char *body, const char *content_type,
                 char *resp, size_t resp_cap);

/* 同上，读写超时可指定秒数（授权等云往返调用需 20s+，默认 5s 不够）。 */
int http_unix_to(const char *sock_path, const char *method, const char *path,
                 const char *body, const char *content_type, int timeout_sec,
                 char *resp, size_t resp_cap);

#endif
