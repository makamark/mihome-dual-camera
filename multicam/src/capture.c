#include "capture.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* keyint 超过此值不做窗口解码：置灰阈值/检测节拍都会被拉爆 */
#define GOP_WINDOW_MAX_KEYINT_MS 3500

static void capture_log_limit(void)
{
    /* 窗口外跳帧会让解码器打 "Missing reference picture" 类日志（每 GOP 一条），
       收紧 ffmpeg 全局日志级别防刷量（解码失败仍由 last_error/计数暴露） */
    av_log_set_level(AV_LOG_ERROR);
}

void capture_set_window(capture_t *cap, int enabled, int frames)
{
    cap->window_enabled = enabled && frames > 0;
    if (cap->window_enabled)
        cap->window_frames = frames;
}

int64_t capture_keyint_ms(capture_t *cap)
{
    return cap->keyint_ms;
}

/* 槽位分辨率按首帧上锁：运行期分辨率变化（换源/切主子码流）只丢帧并计数，
   绝不重新分配——消费方的画布 tile 缓冲按启动尺寸分配，扩容会溢出。 */
static void slot_store(capture_t *cap, const AVFrame *f)
{
    int w = f->width, h = f->height;
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    pthread_mutex_lock(&cap->lock);
    if (cap->slot.data && (cap->slot.w != w || cap->slot.h != h)) {
        cap->size_drops++;
        snprintf(cap->last_error, sizeof(cap->last_error),
                 "dropped %dx%d frame, slot locked at %dx%d", w, h, cap->slot.w, cap->slot.h);
        pthread_mutex_unlock(&cap->lock);
        return;
    }
    if (!cap->slot.data) {
        cap->slot.data = malloc(ysz + 2 * usz);
        if (!cap->slot.data) {
            cap->slot.w = cap->slot.h = 0;
            pthread_mutex_unlock(&cap->lock);
            return;
        }
        cap->slot.w = w;
        cap->slot.h = h;
    }
    uint8_t *d = cap->slot.data;
    for (int row = 0; row < h; row++)
        memcpy(d + (size_t)row * w, f->data[0] + (size_t)row * f->linesize[0], w);
    for (int row = 0; row < h / 2; row++)
        memcpy(d + ysz + (size_t)row * (w / 2), f->data[1] + (size_t)row * f->linesize[1], w / 2);
    for (int row = 0; row < h / 2; row++)
        memcpy(d + ysz + usz + (size_t)row * (w / 2), f->data[2] + (size_t)row * f->linesize[2], w / 2);
    cap->slot.updated_ms = now_ms();
    cap->slot.frame_count++;
    pthread_mutex_unlock(&cap->lock);
}

static int open_stream(capture_t *cap, AVFormatContext **fmt_out, AVCodecContext **dec_out,
                       int *stream_idx)
{
    *fmt_out = NULL;
    *dec_out = NULL;
    char errbuf[128];
    AVFormatContext *fmt = avformat_alloc_context();
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); /* 5s socket 超时(µs) */
    av_dict_set(&opts, "probesize", "262144", 0);        /* 探测缓冲上限 256KB（省启动峰值） */
    av_dict_set(&opts, "analyzeduration", "1000000", 0); /* 流分析上限 1s */
    int rc = avformat_open_input(&fmt, cap->url, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        av_strerror(rc, errbuf, sizeof(errbuf));
        snprintf(cap->last_error, sizeof(cap->last_error), "open_input rc=%d %s", rc, errbuf);
        return -1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        snprintf(cap->last_error, sizeof(cap->last_error), "find_stream_info failed");
        avformat_close_input(&fmt);
        return -1;
    }
    int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (idx < 0) {
        snprintf(cap->last_error, sizeof(cap->last_error), "no video stream");
        avformat_close_input(&fmt);
        return -1;
    }
    const AVCodec *codec = avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!codec) {
        snprintf(cap->last_error, sizeof(cap->last_error), "no decoder");
        avformat_close_input(&fmt);
        return -1;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
    /* 单核 A53：多线程帧解码只放大参考帧缓冲（每线程一套，~MB 级）不提速 */
    dec->thread_count = 1;
    if (avcodec_open2(dec, codec, NULL) < 0) {
        snprintf(cap->last_error, sizeof(cap->last_error), "avcodec_open2 failed");
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }
    *fmt_out = fmt;
    *dec_out = dec;
    *stream_idx = idx;
    return 0;
}

static void *capture_thread(void *arg)
{
    capture_t *cap = arg;
    static pthread_once_t log_once = PTHREAD_ONCE_INIT;
    pthread_once(&log_once, capture_log_limit);
    while (!cap->stop) {
        AVFormatContext *fmt = NULL;
        AVCodecContext *dec = NULL;
        int idx = -1;
        if (open_stream(cap, &fmt, &dec, &idx) != 0) {
            usleep(3 * 1000 * 1000);
            continue;
        }
        cap->burst_left = 0; /* 新会话，窗口预算重置（keyint 测量保留） */
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frm = av_frame_alloc();
        bool alive = true;
        while (!cap->stop && alive) {
            int rc = av_read_frame(fmt, pkt);
            if (rc < 0) {
                snprintf(cap->last_error, sizeof(cap->last_error), "av_read_frame rc=%d", rc);
                alive = false;
                break;
            }
            if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }

            /* ---- GOP 窗口解码状态机（见 capture.h 注释） ---- */
            {
                int is_key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
                if (is_key) {
                    int64_t tm = now_ms();
                    if (cap->last_key_ms) {
                        int d = (int)(tm - cap->last_key_ms);
                        if (d > 100 && d < 20000)
                            cap->keyint_ms = cap->keyint_ms ? (cap->keyint_ms + d) / 2 : d;
                    }
                    cap->last_key_ms = tm;
                    cap->key_count++;
                    if (!cap->window_active && cap->window_enabled &&
                        cap->keyint_ms > 0 && cap->keyint_ms <= GOP_WINDOW_MAX_KEYINT_MS) {
                        cap->window_active = 1;
                        fprintf(stderr, "multicam: GOP 窗口解码启用（keyint≈%dms，每 GOP 解 %d 帧）\n",
                                cap->keyint_ms, cap->window_frames);
                    }
                }
                if (!cap->window_enabled)
                    cap->window_active = 0; /* 标定旁路/配置关闭 → 恢复全解 */
                if (cap->window_active) {
                    if (is_key) {
                        cap->burst_left = cap->window_frames;
                        dec->skip_frame = AVDISCARD_NONE;
                    } else if (cap->burst_left > 0) {
                        cap->burst_left--;
                        dec->skip_frame = AVDISCARD_NONE;
                    } else {
                        dec->skip_frame = AVDISCARD_NONKEY;
                        cap->skipped_pkts++;
                    }
                } else {
                    dec->skip_frame = AVDISCARD_NONE;
                }
            }

            rc = avcodec_send_packet(dec, pkt);
            if (rc < 0 && rc != AVERROR(EAGAIN)) alive = false;
            while (rc >= 0) {
                rc = avcodec_receive_frame(dec, frm);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) { alive = false; break; }
                if (frm->format == AV_PIX_FMT_YUV420P ||
                    frm->format == AV_PIX_FMT_YUVJ420P) {
                    slot_store(cap, frm);   /* yuvj420p 与 yuv420p 内存布局相同 */
                } else if (frm->format != cap->warned_fmt) {
                    cap->warned_fmt = frm->format;
                    fprintf(stderr, "multicam: 像素格式 %s 不支持，帧被丢弃（本格式只告警一次）\n",
                            av_get_pix_fmt_name((enum AVPixelFormat)frm->format));
                }
                if (frm->format != AV_PIX_FMT_YUV420P && frm->format != AV_PIX_FMT_YUVJ420P)
                    cap->pixfmt_drops++;
                av_frame_unref(frm);
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        av_frame_free(&frm);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        if (!cap->stop) {
            cap->reconnects++;
            usleep(3 * 1000 * 1000);
        }
    }
    return NULL;
}

int capture_start(capture_t *cap, const char *url)
{
    memset(cap, 0, sizeof(*cap));
    snprintf(cap->url, sizeof(cap->url), "%s", url);
    pthread_mutex_init(&cap->lock, NULL);
    if (pthread_create(&cap->thread, NULL, capture_thread, cap) != 0)
        return -1;
    cap->thread_started = true;
    return 0;
}

int capture_snapshot_jpeg(capture_t *cap, uint8_t *buf, size_t buf_cap)
{
    if (!cap || !buf || buf_cap < 4096) return -1;
    pthread_mutex_lock(&cap->lock);
    if (!cap->slot.data) {
        pthread_mutex_unlock(&cap->lock);
        return -1;
    }
    int w = cap->slot.w, h = cap->slot.h;

    /* 编码器/输入帧懒初始化（槽位分辨率按首帧锁定，运行期不变） */
    AVCodecContext *enc = cap->snap_enc;
    if (!enc) {
        const AVCodec *c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!c) { pthread_mutex_unlock(&cap->lock); return -1; }
        enc = avcodec_alloc_context3(c);
        if (!enc) { pthread_mutex_unlock(&cap->lock); return -1; }
        enc->width = w;
        enc->height = h;
        enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
        enc->time_base = (AVRational){1, 5};
        enc->qmin = 8;   /* 质量/体积平衡：848x480 室内场景 ~40-90KB */
        enc->qmax = 8;
        if (avcodec_open2(enc, c, NULL) < 0) {
            avcodec_free_context(&enc);
            pthread_mutex_unlock(&cap->lock);
            return -1;
        }
        AVFrame *f = av_frame_alloc();
        if (!f) {
            avcodec_free_context(&enc);
            pthread_mutex_unlock(&cap->lock);
            return -1;
        }
        /* format/尺寸必须先于 get_buffer 设置，否则缓冲分配失败 */
        f->format = enc->pix_fmt;
        f->width = w;
        f->height = h;
        if (av_frame_get_buffer(f, 32) < 0) {
            av_frame_free(&f);
            avcodec_free_context(&enc);
            pthread_mutex_unlock(&cap->lock);
            return -1;
        }
        cap->snap_enc = enc;
        cap->snap_frm = f;
    }
    AVFrame *f = cap->snap_frm;

    /* 槽位（连续平面）→ 编码帧（行距对齐），~600KB 拷贝在锁内 ~1ms */
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    const uint8_t *src = cap->slot.data;
    for (int row = 0; row < h; row++)
        memcpy(f->data[0] + (size_t)row * f->linesize[0], src + (size_t)row * w, w);
    for (int row = 0; row < h / 2; row++) {
        memcpy(f->data[1] + (size_t)row * f->linesize[1], src + ysz + (size_t)row * (w / 2), w / 2);
        memcpy(f->data[2] + (size_t)row * f->linesize[2], src + ysz + usz + (size_t)row * (w / 2), w / 2);
    }
    pthread_mutex_unlock(&cap->lock);

    /* 编码在锁外：enc/frm 仅 bridge 线程（单线程串行服务）访问 */
    f->pts = cap->snap_pts++;
    if (avcodec_send_frame(enc, f) < 0) return -1;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;
    int n = -1;
    if (avcodec_receive_packet(enc, pkt) >= 0) {
        if ((size_t)pkt->size <= buf_cap) {
            memcpy(buf, pkt->data, pkt->size);
            n = pkt->size;
        } else {
            n = -2;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return n;
}

void capture_stop(capture_t *cap)
{
    cap->stop = true;
    if (cap->thread_started) {
        pthread_join(cap->thread, NULL);
        cap->thread_started = false;
    }
    pthread_mutex_destroy(&cap->lock);
    free(cap->slot.data);
    cap->slot.data = NULL;
    if (cap->snap_enc) {
        AVCodecContext *e = cap->snap_enc;
        avcodec_free_context(&e);
        cap->snap_enc = NULL;
    }
    av_frame_free((AVFrame **)&cap->snap_frm);
}

void capture_dims(capture_t *cap, int *w, int *h)
{
    pthread_mutex_lock(&cap->lock);
    *w = cap->slot.w;
    *h = cap->slot.h;
    pthread_mutex_unlock(&cap->lock);
}

int64_t capture_age(capture_t *cap)
{
    pthread_mutex_lock(&cap->lock);
    int64_t age = cap->slot.data ? now_ms() - cap->slot.updated_ms : -1;
    pthread_mutex_unlock(&cap->lock);
    return age;
}

int64_t capture_paint_tile(capture_t *cap, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                           int dst_y_stride, int dst_c_stride,
                           int expect_w, int expect_h)
{
    pthread_mutex_lock(&cap->lock);
    if (!cap->slot.data) {
        pthread_mutex_unlock(&cap->lock);
        return -1;
    }
    if (cap->slot.w != expect_w || cap->slot.h != expect_h) {
        pthread_mutex_unlock(&cap->lock);
        return -2;
    }
    int w = cap->slot.w, h = cap->slot.h;
    const uint8_t *src = cap->slot.data;
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    for (int row = 0; row < h; row++)
        memcpy(dst_y + (size_t)row * dst_y_stride, src + (size_t)row * w, w);
    for (int row = 0; row < h / 2; row++) {
        memcpy(dst_u + (size_t)row * dst_c_stride, src + ysz + (size_t)row * (w / 2), w / 2);
        memcpy(dst_v + (size_t)row * dst_c_stride, src + ysz + usz + (size_t)row * (w / 2), w / 2);
    }
    int64_t age = now_ms() - cap->slot.updated_ms;
    pthread_mutex_unlock(&cap->lock);
    return age;
}

int64_t capture_paint_crop(capture_t *cap, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                           int dst_y_stride, int dst_c_stride,
                           int crop_x, int crop_y,
                           int crop_w, int crop_h)
{
    pthread_mutex_lock(&cap->lock);
    if (!cap->slot.data) {
        pthread_mutex_unlock(&cap->lock);
        return -1;
    }
    int w = cap->slot.w, h = cap->slot.h;
    if (crop_w <= 0 || crop_h <= 0 || w < crop_w || h < crop_h) {
        pthread_mutex_unlock(&cap->lock);
        return -2;
    }
    crop_w &= ~1;
    crop_h &= ~1;
    crop_x &= ~1;
    crop_y &= ~1;
    if (crop_x < 0) crop_x = (w - crop_w) / 2 & ~1;
    if (crop_y < 0) crop_y = (h - crop_h) / 2 & ~1;
    if (crop_x + crop_w > w) crop_x = (w - crop_w) & ~1;
    if (crop_y + crop_h > h) crop_y = (h - crop_h) & ~1;

    const uint8_t *src = cap->slot.data;
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    for (int row = 0; row < crop_h; row++)
        memcpy(dst_y + (size_t)row * dst_y_stride,
               src + (size_t)(crop_y + row) * w + crop_x, crop_w);
    for (int row = 0; row < crop_h / 2; row++) {
        memcpy(dst_u + (size_t)row * dst_c_stride,
               src + ysz + (size_t)(crop_y / 2 + row) * (w / 2) + crop_x / 2, crop_w / 2);
        memcpy(dst_v + (size_t)row * dst_c_stride,
               src + ysz + usz + (size_t)(crop_y / 2 + row) * (w / 2) + crop_x / 2, crop_w / 2);
    }
    int64_t age = now_ms() - cap->slot.updated_ms;
    pthread_mutex_unlock(&cap->lock);
    return age;
}
