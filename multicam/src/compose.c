#include "compose.h"
#include <string.h>

void compose_grid_yuv420p(uint8_t *dst, int dst_w, int dst_h, int cols,
                          const uint8_t *const *srcs, int src_w, int src_h, int n)
{
    size_t tile_y = (size_t)src_w * src_h;
    size_t tile_uv = (size_t)(src_w / 2) * (src_h / 2);
    size_t canvas_y = (size_t)dst_w * dst_h;
    int cw = dst_w / 2, ch = dst_h / 2, cw_t = src_w / 2;
    int t, y, p;

    for (t = 0; t < n; t++) {
        int ox = (t % cols) * src_w, oy = (t / cols) * src_h;
        for (y = 0; y < src_h; y++)
            memcpy(dst + (size_t)(oy + y) * dst_w + ox,
                   srcs[t] + (size_t)y * src_w, src_w);
    }

    for (p = 0; p < 2; p++) {
        uint8_t *d = dst + canvas_y + (size_t)p * cw * ch;
        for (t = 0; t < n; t++) {
            int ox = (t % cols) * cw_t, oy = (t / cols) * (src_h / 2);
            for (y = 0; y < src_h / 2; y++)
                memcpy(d + (size_t)(oy + y) * cw + ox,
                       srcs[t] + tile_y + (size_t)p * tile_uv + (size_t)y * cw_t,
                       cw_t);
        }
    }
}
