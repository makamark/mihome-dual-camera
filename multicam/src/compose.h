#ifndef MULTICAM_COMPOSE_H
#define MULTICAM_COMPOSE_H

#include <stdint.h>

/* 网格拼接 n 路 YUV420P：所有源必须同尺寸且为偶数尺寸，
   dst_w == cols*src_w，dst_h == ceil(n/cols)*src_h。平铺拷贝，无缩放。
   cols==n（或 n<=cols）退化为旧版单行横排。 */
void compose_grid_yuv420p(uint8_t *dst, int dst_w, int dst_h, int cols,
                          const uint8_t *const *srcs, int src_w, int src_h, int n);

#endif
