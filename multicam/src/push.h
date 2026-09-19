#ifndef MULTICAM_PUSH_H
#define MULTICAM_PUSH_H

#include <stdint.h>

typedef struct {
    void *session;
} push_t;

/* 打开 SDK 视频会话（独占生产者：mhcamera 在跑会 EBUSY）。 */
int push_open(push_t *p);
int push_yuv420p(push_t *p, const uint8_t *frame, int w, int h, uint64_t pts_us);
void push_close(push_t *p);

#endif
