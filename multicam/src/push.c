#include "push.h"
#include <ainice/video.h>
#include <errno.h>
#include <stdio.h>

int push_open(push_t *p)
{
    int rc = ainice_video_open((ainice_video_session_t **)&p->session);
    if (rc != 0)
        fprintf(stderr, "multicam: video open failed rc=%d errno=%d "
                        "(rc=-EBUSY 表示独占生产者冲突：先停用 mhcamera)\n", rc, errno);
    return rc;
}

int push_yuv420p(push_t *p, const uint8_t *frame, int w, int h, uint64_t pts_us)
{
    struct ainice_video_frame f;
    f.format = AINICE_VIDEO_FRAME_FORMAT_YUV420P;
    f.width = (uint32_t)w;
    f.height = (uint32_t)h;
    f.size = (uint32_t)((size_t)w * h * 3 / 2);
    f.pts = pts_us;
    f.data = frame;
    return ainice_video_send_frame(p->session, &f);
}

void push_close(push_t *p)
{
    if (p->session) {
        ainice_video_close(p->session);
        p->session = NULL;
    }
}
