#define _GNU_SOURCE
#include "capture.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <errno.h>
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

/* 源分辨率上限（像素数）：157MB 设备上 1080p 以上的解码参考帧会挤爆内存，
   超限在打开阶段即拒绝（提示用户改用子码流） */
#define CAPTURE_MAX_PIXELS (1920 * 1088)

/* 置灰原因（面板显示）：捕获线程写、主线程读，均在 cap->lock 内 */
static void set_note(capture_t *cap, const char *note)
{
    pthread_mutex_lock(&cap->lock);
    snprintf(cap->note, sizeof(cap->note), "%s", note);
    pthread_mutex_unlock(&cap->lock);
}

/* 像素格式可转换判定：YUV（含灰度）平面/半平面、小端、≤16bit。
   NV12/422/444/10bit 等统一按 desc 的 plane/step/offset/shift 逐点采样转 420P */
static bool pixfmt_convertible(const AVPixFmtDescriptor *d)
{
    if (!d || d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM |
                          AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_FLOAT |
                          AV_PIX_FMT_FLAG_BE))
        return false;
    if (d->comp[0].depth < 8 || d->comp[0].depth > 16) return false;
    return d->nb_components == 1 || d->nb_components == 2 || d->nb_components >= 3;
}

static inline unsigned comp_at(const AVFrame *f, const AVComponentDescriptor *c,
                               int x, int y)
{
    const uint8_t *p = f->data[c->plane] + (ptrdiff_t)y * f->linesize[c->plane] +
                       (ptrdiff_t)x * c->step + c->offset;
    if (c->depth <= 8) return p[0];
    unsigned v = ((unsigned)p[0] | (unsigned)p[1] << 8) >> c->shift;
    return v >> (c->depth - 8);
}

/* 解码帧 → 槽位（YUV420P 连续布局）。分辨率变化（rtsp 摄像头切码流、
   换源后同槽位复用）就地重分配：读方（paint/快照）都在同一把锁内按
   当时的 w/h 访问，重分配不会越界。奇数宽高向下取偶。 */
static void slot_store(capture_t *cap, const AVFrame *f)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get((enum AVPixelFormat)f->format);
    int w = f->width & ~1, h = f->height & ~1;
    if (w <= 0 || h <= 0) return;
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    pthread_mutex_lock(&cap->lock);
    if (!cap->slot.data || cap->slot.w != w || cap->slot.h != h) {
        if (cap->slot.data) {
            cap->size_changes++;
            fprintf(stderr, "multicam: 源分辨率 %dx%d → %dx%d，槽位重建\n",
                    cap->slot.w, cap->slot.h, w, h);
        }
        uint8_t *nd = realloc(cap->slot.data, ysz + 2 * usz);
        if (!nd) {
            free(cap->slot.data);
            cap->slot.data = NULL;
            cap->slot.w = cap->slot.h = 0;
            pthread_mutex_unlock(&cap->lock);
            return;
        }
        cap->slot.data = nd;
        cap->slot.w = w;
        cap->slot.h = h;
    }
    uint8_t *dy = cap->slot.data, *du = dy + ysz, *dv = du + usz;
    int cw = w / 2, ch = h / 2;
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        /* 快路径：yuvj420p 与 yuv420p 内存布局相同 */
        for (int row = 0; row < h; row++)
            memcpy(dy + (size_t)row * w, f->data[0] + (size_t)row * f->linesize[0], w);
        for (int row = 0; row < ch; row++) {
            memcpy(du + (size_t)row * cw, f->data[1] + (size_t)row * f->linesize[1], cw);
            memcpy(dv + (size_t)row * cw, f->data[2] + (size_t)row * f->linesize[2], cw);
        }
    } else {
        const AVComponentDescriptor *cy = &d->comp[0];
        for (int row = 0; row < h; row++) {
            uint8_t *o = dy + (size_t)row * w;
            if (cy->depth == 8 && cy->step == 1 && cy->offset == 0)
                memcpy(o, f->data[cy->plane] + (size_t)row * f->linesize[cy->plane], w);
            else
                for (int x = 0; x < w; x++) o[x] = (uint8_t)comp_at(f, cy, x, row);
        }
        if (d->nb_components >= 3) {
            /* 输出色度点 (c,r) 对应亮度 (2c,2r)，按源色度下采样比例换算 */
            int sw = d->log2_chroma_w, sh = d->log2_chroma_h;
            for (int r = 0; r < ch; r++) {
                int sr = (2 * r) >> sh;
                uint8_t *ou = du + (size_t)r * cw, *ov = dv + (size_t)r * cw;
                for (int c = 0; c < cw; c++) {
                    int sc = (2 * c) >> sw;
                    ou[c] = (uint8_t)comp_at(f, &d->comp[1], sc, sr);
                    ov[c] = (uint8_t)comp_at(f, &d->comp[2], sc, sr);
                }
            }
        } else {
            memset(du, 128, 2 * usz);   /* 灰度源 */
        }
    }
    cap->slot.updated_ms = now_ms();
    cap->slot.frame_count++;
    cap->note[0] = 0;
    pthread_mutex_unlock(&cap->lock);
}

/* ffmpeg 打开失败 → 面板可读原因（rtsp 状态码经 ff_http_averror 映射） */
static const char *open_error_note(int rc)
{
    if (rc == AVERROR_HTTP_UNAUTHORIZED) return "认证失败（用户名或密码错误）";
    if (rc == AVERROR_HTTP_FORBIDDEN) return "无访问权限（403）";
    if (rc == AVERROR_HTTP_NOT_FOUND) return "地址路径不存在（404）";
    if (rc == AVERROR_HTTP_BAD_REQUEST || rc == AVERROR_HTTP_OTHER_4XX) return "摄像头拒绝请求（检查地址路径）";
    if (rc == AVERROR_HTTP_SERVER_ERROR) return "摄像头内部错误（5xx）";
    if (rc == AVERROR(ECONNREFUSED)) return "连接被拒绝（端口错误或未开启 RTSP）";
    if (rc == AVERROR(ETIMEDOUT)) return "连接超时（IP 不通或被拦截）";
    if (rc == AVERROR(EHOSTUNREACH) || rc == AVERROR(ENETUNREACH)) return "网络不可达（检查 IP）";
    if (rc == AVERROR_PROTOCOL_NOT_FOUND) return "不支持的协议（仅支持 rtsp://）";
    return NULL;
}

static int interrupt_cb(void *opaque)
{
    return ((capture_t *)opaque)->stop ? 1 : 0;
}

/* 可被 stop 打断的等待（换源时不让主循环干等重连间隔） */
static void sleep_unless_stop(capture_t *cap, int ms)
{
    for (int t = 0; t < ms && !cap->stop; t += 100) usleep(100 * 1000);
}

static int open_stream(capture_t *cap, AVFormatContext **fmt_out, AVCodecContext **dec_out,
                       int *stream_idx)
{
    *fmt_out = NULL;
    *dec_out = NULL;
    char errbuf[128];
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) return -1;
    fmt->interrupt_callback.callback = interrupt_cb;   /* stop 时打断阻塞的连接/读 */
    fmt->interrupt_callback.opaque = cap;
    AVDictionary *opts = NULL;
    /* 默认 TCP 交错（丢包不花屏）；摄像头拒绝 TCP 时自动改试 UDP */
    av_dict_set(&opts, "rtsp_transport", cap->rtsp_udp ? "udp" : "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); /* 5s socket 超时(µs) */
    av_dict_set(&opts, "probesize", "262144", 0);        /* 探测缓冲上限 256KB（省启动峰值） */
    av_dict_set(&opts, "analyzeduration", "1000000", 0); /* 流分析上限 1s */
    int rc = avformat_open_input(&fmt, cap->url, NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        av_strerror(rc, errbuf, sizeof(errbuf));
        snprintf(cap->last_error, sizeof(cap->last_error), "open_input(%s) rc=%d %s",
                 cap->rtsp_udp ? "udp" : "tcp", rc, errbuf);
        const char *note = open_error_note(rc);
        if (!note) {
            /* 原因不明（多为 SETUP 传输协商失败）：下次换另一种传输再试 */
            cap->rtsp_udp = !cap->rtsp_udp;
            note = "无法建立视频会话（已自动改试其他传输方式）";
        }
        set_note(cap, note);
        return -1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        snprintf(cap->last_error, sizeof(cap->last_error), "find_stream_info failed");
        set_note(cap, "无法识别视频流");
        avformat_close_input(&fmt);
        return -1;
    }
    int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (idx < 0) {
        snprintf(cap->last_error, sizeof(cap->last_error), "no video stream");
        set_note(cap, "地址里没有视频流");
        avformat_close_input(&fmt);
        return -1;
    }
    AVCodecParameters *par = fmt->streams[idx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        snprintf(cap->last_error, sizeof(cap->last_error), "no decoder (%s)",
                 avcodec_get_name(par->codec_id));
        set_note(cap, "视频编码不支持（仅 H.264 / H.265 / MJPEG）");
        avformat_close_input(&fmt);
        return -1;
    }
    if ((int64_t)par->width * par->height > CAPTURE_MAX_PIXELS) {
        snprintf(cap->last_error, sizeof(cap->last_error), "source %dx%d too large",
                 par->width, par->height);
        set_note(cap, "分辨率过高（请改用 1080p 以下的子码流）");
        avformat_close_input(&fmt);
        return -1;
    }
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    /* 单核 A53：多线程帧解码只放大参考帧缓冲（每线程一套，~MB 级）不提速 */
    dec->thread_count = 1;
    if (avcodec_open2(dec, codec, NULL) < 0) {
        snprintf(cap->last_error, sizeof(cap->last_error), "avcodec_open2 failed");
        set_note(cap, "解码器打开失败");
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }
    *fmt_out = fmt;
    *dec_out = dec;
    *stream_idx = idx;
    return 0;
}

/* GOP 窗口解码状态机（rtsp/aiv1 两输入路径共用，见 capture.h 注释） */
static void window_step(capture_t *cap, bool is_key, AVCodecContext *dec)
{
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

/* 解码器取帧入槽（两输入路径共用）；返回 -1 解码器致命错误，0 正常 */
static int drain_frames(capture_t *cap, AVCodecContext *dec, AVFrame *frm)
{
    int rc;
    while ((rc = avcodec_receive_frame(dec, frm)) >= 0) {
        if (frm->format == AV_PIX_FMT_YUV420P || frm->format == AV_PIX_FMT_YUVJ420P ||
            pixfmt_convertible(av_pix_fmt_desc_get((enum AVPixelFormat)frm->format))) {
            if ((int64_t)frm->width * frm->height <= CAPTURE_MAX_PIXELS)
                slot_store(cap, frm);
            else
                set_note(cap, "分辨率过高（请改用 1080p 以下的子码流）");
        } else {
            if (frm->format != cap->warned_fmt) {
                cap->warned_fmt = frm->format;
                fprintf(stderr, "multicam: 像素格式 %s 不支持，帧被丢弃（本格式只告警一次）\n",
                        av_get_pix_fmt_name((enum AVPixelFormat)frm->format));
            }
            cap->pixfmt_drops++;
            set_note(cap, "画面像素格式不支持");
        }
        av_frame_unref(frm);
    }
    return (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) ? 0 : -1;
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
            sleep_unless_stop(cap, 3000);
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
                if (!cap->stop) set_note(cap, "视频流中断，正在重连");
                alive = false;
                break;
            }
            if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }

            window_step(cap, (pkt->flags & AV_PKT_FLAG_KEY) != 0, dec);

            rc = avcodec_send_packet(dec, pkt);
            if (rc < 0 && rc != AVERROR(EAGAIN)) alive = false;
            else if (rc >= 0 && drain_frames(cap, dec, frm) != 0) alive = false;
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        av_frame_free(&frm);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        if (!cap->stop) {
            cap->reconnects++;
            sleep_unless_stop(cap, 3000);
        }
    }
    return NULL;
}

/* ---------- AIV1 输入模式：go2rtc 实例的 media socket 直消费 ----------
   线协议 vendor 自 mhcamera（github.com/wade-hello/mhcamera，MIT）：
   AIVH 握手（32B）→ AIVF 帧头（48B）+ AVCC 载荷（4 字节大端长度前缀 NAL）。
   服务端逐字节一致于我们随包的 go2rtc 二进制（补丁 0002 哈希核对过），
   且只接受"实例父进程"作为消费者（SO_PEERCRED pid == 实例 pid 的父）——
   multicam 既是父进程又是消费者，天然通过。 */

#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define AIV1_VERSION          1u
#define AIV1_HELLO_SIZE       32u
#define AIV1_FRAME_HDR_SIZE   48u
#define AIV1_MAX_PAYLOAD      (8u * 1024u * 1024u)
#define AIV1_TIMESCALE        1000000u
#define AIV1_CODEC_H264       1u
#define AIV1_CODEC_H265       2u
#define AIV1_FMT_AVCC         1u
#define AIV1_FLAG_KEYFRAME    (1u << 0)
#define AIV1_FLAG_DISCONT     (1u << 1)
#define AIV1_NAL_H264_SPS     (1u << 0)
#define AIV1_NAL_H264_PPS     (1u << 1)
#define AIV1_NAL_H264_IDR     (1u << 2)
#define AIV1_NAL_H264_SYNC    (AIV1_NAL_H264_SPS | AIV1_NAL_H264_PPS | AIV1_NAL_H264_IDR)
#define AIV1_NAL_H265_VPS     (1u << 3)
#define AIV1_NAL_H265_SPS     (1u << 4)
#define AIV1_NAL_H265_PPS     (1u << 5)
#define AIV1_NAL_H265_IRAP    (1u << 6)
#define AIV1_NAL_H265_SYNC    (AIV1_NAL_H265_VPS | AIV1_NAL_H265_SPS | AIV1_NAL_H265_PPS | AIV1_NAL_H265_IRAP)
#define AIV1_READ_TIMEOUT_S   10   /* 无帧超时：GOP≈3s，10s 无帧视为会话死亡 */

static uint16_t aiv1_u16le(const uint8_t *d) { return (uint16_t)d[0] | (uint16_t)d[1] << 8; }
static uint32_t aiv1_u32le(const uint8_t *d)
{
    return (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
}
static uint64_t aiv1_u64le(const uint8_t *d)
{
    return (uint64_t)aiv1_u32le(d) | (uint64_t)aiv1_u32le(d + 4) << 32;
}
static uint32_t aiv1_u32be(const uint8_t *d)
{
    return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
}

typedef struct {
    uint32_t codec, max_payload;
    uint64_t stream_epoch;
} aiv1_hello_t;

typedef struct {
    uint32_t codec, flags;
    uint64_t stream_epoch, output_seq, pts_us;
    uint32_t payload_len;
} aiv1_frame_t;

static int aiv1_parse_hello(const uint8_t *d, size_t size, aiv1_hello_t *h)
{
    uint32_t codec, max_payload;
    uint64_t epoch;
    if (size != AIV1_HELLO_SIZE || memcmp(d, "AIVH", 4) != 0 ||
        aiv1_u16le(d + 4) != AIV1_VERSION || aiv1_u16le(d + 6) != AIV1_HELLO_SIZE ||
        aiv1_u32le(d + 12) != AIV1_FMT_AVCC || aiv1_u32le(d + 16) != AIV1_TIMESCALE)
        return -1;
    codec = aiv1_u32le(d + 8);
    max_payload = aiv1_u32le(d + 20);
    epoch = aiv1_u64le(d + 24);
    /* 先校验后赋值：解析失败不污染调用方已持有的 hello（断线重连场景） */
    if ((codec != AIV1_CODEC_H264 && codec != AIV1_CODEC_H265) ||
        max_payload == 0 || max_payload > AIV1_MAX_PAYLOAD || epoch == 0)
        return -1;
    h->codec = codec;
    h->max_payload = max_payload;
    h->stream_epoch = epoch;
    return 0;
}

static int aiv1_parse_frame(const uint8_t *d, size_t size,
                            const aiv1_hello_t *h, aiv1_frame_t *f)
{
    if (size != AIV1_FRAME_HDR_SIZE || memcmp(d, "AIVF", 4) != 0 ||
        aiv1_u16le(d + 4) != AIV1_VERSION || aiv1_u16le(d + 6) != AIV1_FRAME_HDR_SIZE ||
        aiv1_u32le(d + 44) != 0)
        return -1;
    f->codec = aiv1_u32le(d + 8);
    f->flags = aiv1_u32le(d + 12);
    f->stream_epoch = aiv1_u64le(d + 16);
    f->output_seq = aiv1_u64le(d + 24);
    f->pts_us = aiv1_u64le(d + 32);
    f->payload_len = aiv1_u32le(d + 40);
    if (f->codec != h->codec ||
        (f->flags & ~(AIV1_FLAG_KEYFRAME | AIV1_FLAG_DISCONT)) != 0 ||
        f->stream_epoch != h->stream_epoch || f->output_seq == 0 ||
        f->payload_len == 0 || f->payload_len > h->max_payload)
        return -1;
    return 0;
}

/* AVCC 结构校验 + 关键帧 NAL 识别（VPS/SPS/PPS/IRAP 齐全才算完整同步帧） */
static int aiv1_avcc_inspect(uint32_t codec, const uint8_t *d, size_t size, unsigned *nal_flags)
{
    size_t off = 0;
    unsigned flags = 0;
    if (!d || !nal_flags || size == 0 ||
        (codec != AIV1_CODEC_H264 && codec != AIV1_CODEC_H265)) return -1;
    while (off < size) {
        uint32_t n;
        if (size - off < 4) return -1;
        n = aiv1_u32be(d + off);
        if (n == 0 || n > size - off - 4 ||
            (codec == AIV1_CODEC_H265 && n < 2)) return -1;
        if (codec == AIV1_CODEC_H264) {
            uint32_t t = d[off + 4] & 0x1f;
            if (t == 7) flags |= AIV1_NAL_H264_SPS;
            else if (t == 8) flags |= AIV1_NAL_H264_PPS;
            else if (t == 5) flags |= AIV1_NAL_H264_IDR;
        } else {
            uint32_t t = (d[off + 4] >> 1) & 0x3f;
            if (t == 32) flags |= AIV1_NAL_H265_VPS;
            else if (t == 33) flags |= AIV1_NAL_H265_SPS;
            else if (t == 34) flags |= AIV1_NAL_H265_PPS;
            else if (t >= 16 && t <= 23) flags |= AIV1_NAL_H265_IRAP;
        }
        off += 4 + (size_t)n;
    }
    *nal_flags = flags;
    return 0;
}

/* AVCC → Annex B 原地转换（4 字节长度前缀 → 00 00 00 01 起始码） */
static int aiv1_avcc_to_annexb(uint32_t codec, uint8_t *d, size_t size)
{
    size_t off = 0;
    unsigned ignored;
    if (aiv1_avcc_inspect(codec, d, size, &ignored) != 0) return -1;
    while (off < size) {
        uint32_t n = aiv1_u32be(d + off);
        d[off] = 0; d[off + 1] = 0; d[off + 2] = 0; d[off + 3] = 1;
        off += 4 + (size_t)n;
    }
    return 0;
}

static int aiv1_connect(capture_t *cap)
{
    struct sockaddr_un addr;
    struct ucred cred;
    socklen_t clen = sizeof(cred);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = AIV1_READ_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", cap->aiv1_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(cap->last_error, sizeof(cap->last_error),
                 "media socket 连接失败: %s", strerror(errno));
        close(fd);
        return -1;
    }
    /* 消费者身份校验：peer 必须是当前 go2rtc 实例（父进程=本进程），
       防止接到重建前的旧实例或外来 socket */
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 ||
        cred.pid != (pid_t)*cap->aiv1_peer_pid || cred.uid != geteuid()) {
        snprintf(cap->last_error, sizeof(cap->last_error),
                 "media socket peer 不符（pid=%d 期望 %ld）", cred.pid, *cap->aiv1_peer_pid);
        close(fd);
        return -1;
    }
    cap->aiv1_fd = fd;
    return fd;
}

static int aiv1_read_exact(capture_t *cap, int fd, void *buf, size_t size)
{
    uint8_t *p = buf;
    size_t off = 0;
    while (off < size) {
        ssize_t n = recv(fd, p + off, size - off, 0);
        if (n > 0) { off += (size_t)n; continue; }
        if (n == 0) { snprintf(cap->last_error, sizeof(cap->last_error), "media socket 对端关闭"); return -1; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            snprintf(cap->last_error, sizeof(cap->last_error), "media socket 读超时（%ds 无数据）", AIV1_READ_TIMEOUT_S);
        else
            snprintf(cap->last_error, sizeof(cap->last_error), "media socket 读失败: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int aiv1_buffer_reserve(capture_t *cap, size_t size)
{
    size_t need = size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (need < size) return -1;
    if (need <= cap->aiv1_buf_cap) return 0;
    uint8_t *grown = realloc(cap->aiv1_buf, need);
    if (!grown) return -1;
    cap->aiv1_buf = grown;
    cap->aiv1_buf_cap = need;
    return 0;
}

static AVCodecContext *aiv1_open_decoder(uint32_t codec)
{
    enum AVCodecID id = codec == AIV1_CODEC_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec *c = avcodec_find_decoder(id);
    if (!c) return NULL;
    AVCodecContext *dec = avcodec_alloc_context3(c);
    if (!dec) return NULL;
    dec->thread_count = 1;          /* 单核 A53：多线程只放大参考帧缓冲 */
    if (avcodec_open2(dec, c, NULL) < 0) {
        avcodec_free_context(&dec);
        return NULL;
    }
    return dec;
}

static void *aiv1_thread(void *arg)
{
    capture_t *cap = arg;
    static pthread_once_t log_once2 = PTHREAD_ONCE_INIT;
    pthread_once(&log_once2, capture_log_limit);
    while (!cap->stop) {
        cap->last_error[0] = 0;   /* 每会话重置：否则"首个错误优先"判断会一直沿用旧会话的错误 */
        int fd = aiv1_connect(cap);
        if (fd < 0) {
            usleep(3 * 1000 * 1000);
            continue;
        }
        /* 握手 */
        aiv1_hello_t hello;
        uint8_t hdr[AIV1_HELLO_SIZE];
        AVCodecContext *dec = NULL;
        bool ok = aiv1_read_exact(cap, fd, hdr, sizeof(hdr)) == 0 &&
                  aiv1_parse_hello(hdr, sizeof(hdr), &hello) == 0;
        if (!ok) {
            char prev[96];
            snprintf(prev, sizeof(prev), "%s", cap->last_error);
            if (!prev[0])
                snprintf(cap->last_error, sizeof(cap->last_error), "AIVH 握手失败");
            else
                snprintf(cap->last_error, sizeof(cap->last_error),
                         "AIVH 握手失败: %.80s", prev);
            goto session_end;
        }
        dec = aiv1_open_decoder(hello.codec);
        if (!dec) {
            snprintf(cap->last_error, sizeof(cap->last_error), "解码器打开失败（codec=%u）", hello.codec);
            goto session_end;
        }
        fprintf(stderr, "multicam: AIV1 会话建立（codec=%s epoch=%llu）\n",
                hello.codec == AIV1_CODEC_H265 ? "h265" : "h264",
                (unsigned long long)hello.stream_epoch);
        cap->burst_left = 0;
        {
            bool waiting_sync = true;      /* 等待完整同步关键帧（断流/换代后） */
            bool have_seq = false;
            uint64_t last_seq = 0;
            bool alive = true;
            AVFrame *frm = av_frame_alloc();
            AVPacket *pkt = av_packet_alloc();
            while (!cap->stop && alive) {
                aiv1_frame_t f;
                uint8_t fh[AIV1_FRAME_HDR_SIZE];
                if (aiv1_read_exact(cap, fd, fh, sizeof(fh)) != 0 ||
                    aiv1_parse_frame(fh, sizeof(fh), &hello, &f) != 0 ||
                    f.payload_len > AIV1_MAX_PAYLOAD ||
                    aiv1_buffer_reserve(cap, f.payload_len) != 0 ||
                    aiv1_read_exact(cap, fd, cap->aiv1_buf, f.payload_len) != 0) {
                    if (!cap->last_error[0])
                        snprintf(cap->last_error, sizeof(cap->last_error), "AIVF 帧读取失败");
                    alive = false;
                    break;
                }
                memset(cap->aiv1_buf + f.payload_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                unsigned nal = 0;
                if (aiv1_avcc_inspect(f.codec, cap->aiv1_buf, f.payload_len, &nal) != 0) {
                    snprintf(cap->last_error, sizeof(cap->last_error), "AVCC 结构非法");
                    alive = false;
                    break;
                }
                bool key = (f.flags & AIV1_FLAG_KEYFRAME) != 0;
                bool complete = (f.codec == AIV1_CODEC_H264 ?
                                 (nal & AIV1_NAL_H264_SYNC) == AIV1_NAL_H264_SYNC :
                                 (nal & AIV1_NAL_H265_SYNC) == AIV1_NAL_H265_SYNC);
                if (key != complete) {   /* 协议一致性：关键帧必须自带参数集 */
                    snprintf(cap->last_error, sizeof(cap->last_error), "关键帧缺少参数集");
                    alive = false;
                    break;
                }
                if (have_seq && f.output_seq != last_seq + 1)
                    waiting_sync = true;  /* 序列断档：等下一个完整关键帧 */
                last_seq = f.output_seq;
                have_seq = true;
                if (f.flags & AIV1_FLAG_DISCONT)
                    waiting_sync = true;
                if (waiting_sync) {
                    if (!key) continue;
                    waiting_sync = false;
                }
                window_step(cap, key, dec);
                if (aiv1_avcc_to_annexb(f.codec, cap->aiv1_buf, f.payload_len) != 0) {
                    snprintf(cap->last_error, sizeof(cap->last_error), "AVCC→AnnexB 转换失败");
                    alive = false;
                    break;
                }
                pkt->data = cap->aiv1_buf;
                pkt->size = (int)f.payload_len;
                int rc = avcodec_send_packet(dec, pkt);
                if (rc < 0 && rc != AVERROR(EAGAIN)) {
                    snprintf(cap->last_error, sizeof(cap->last_error), "解码 send rc=%d", rc);
                    alive = false;
                } else if (rc >= 0 && drain_frames(cap, dec, frm) != 0) {
                    snprintf(cap->last_error, sizeof(cap->last_error), "解码 receive 失败");
                    alive = false;
                }
            }
            av_frame_free(&frm);
            av_packet_free(&pkt);
        }
session_end:
        avcodec_free_context(&dec);
        if (cap->aiv1_fd >= 0) { close(cap->aiv1_fd); cap->aiv1_fd = -1; }
        else if (fd >= 0) close(fd);
        if (!cap->stop) {
            cap->reconnects++;
            fprintf(stderr, "multicam: AIV1 会话结束（%.100s），3s 后重连\n", cap->last_error);
            usleep(3 * 1000 * 1000);
        }
    }
    return NULL;
}

static int capture_start_common(capture_t *cap)
{
    memset(cap, 0, sizeof(*cap));
    cap->aiv1_fd = -1;
    pthread_mutex_init(&cap->lock, NULL);
    return 0;
}

int capture_start(capture_t *cap, const char *url)
{
    capture_start_common(cap);
    snprintf(cap->url, sizeof(cap->url), "%s", url);
    if (pthread_create(&cap->thread, NULL, capture_thread, cap) != 0)
        return -1;
    cap->thread_started = true;
    return 0;
}

int capture_start_aiv1(capture_t *cap, const char *sock_path, const long *peer_pid)
{
    capture_start_common(cap);
    cap->aiv1 = true;
    snprintf(cap->aiv1_path, sizeof(cap->aiv1_path), "%s", sock_path);
    cap->aiv1_peer_pid = peer_pid;
    if (pthread_create(&cap->thread, NULL, aiv1_thread, cap) != 0)
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
    /* 快照尺寸：源宽 >848 时等比缩小（1080p 全幅 JPEG 经 base64 回传太大） */
    int sw = w, sh = h;
    if (sw > 848) {
        sw = 848;
        sh = (int)((int64_t)h * 848 / w) & ~1;
    }

    /* 编码器/输入帧懒初始化；源分辨率变化后尺寸不符则重建 */
    AVCodecContext *enc = cap->snap_enc;
    if (enc && (enc->width != sw || enc->height != sh)) {
        avcodec_free_context(&enc);
        av_frame_free((AVFrame **)&cap->snap_frm);
        cap->snap_enc = NULL;
    }
    if (!enc) {
        const AVCodec *c = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!c) { pthread_mutex_unlock(&cap->lock); return -1; }
        enc = avcodec_alloc_context3(c);
        if (!enc) { pthread_mutex_unlock(&cap->lock); return -1; }
        enc->width = sw;
        enc->height = sh;
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
        f->width = sw;
        f->height = sh;
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

    /* 槽位（连续平面）→ 编码帧（行距对齐），拷贝在锁内 ~1ms；
       缩小时最近邻取样（只给人看取景位置，不需要插值） */
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    const uint8_t *src = cap->slot.data;
    if (sw == w) {
        for (int row = 0; row < h; row++)
            memcpy(f->data[0] + (size_t)row * f->linesize[0], src + (size_t)row * w, w);
        for (int row = 0; row < h / 2; row++) {
            memcpy(f->data[1] + (size_t)row * f->linesize[1], src + ysz + (size_t)row * (w / 2), w / 2);
            memcpy(f->data[2] + (size_t)row * f->linesize[2], src + ysz + usz + (size_t)row * (w / 2), w / 2);
        }
    } else {
        for (int row = 0; row < sh; row++) {
            const uint8_t *sr = src + (size_t)((int64_t)row * h / sh) * w;
            uint8_t *d = f->data[0] + (size_t)row * f->linesize[0];
            for (int x = 0; x < sw; x++) d[x] = sr[(int64_t)x * w / sw];
        }
        int cw = w / 2, ch = h / 2, scw = sw / 2, sch = sh / 2;
        for (int row = 0; row < sch; row++) {
            size_t so = (size_t)((int64_t)row * ch / sch) * cw;
            uint8_t *du = f->data[1] + (size_t)row * f->linesize[1];
            uint8_t *dv = f->data[2] + (size_t)row * f->linesize[2];
            for (int x = 0; x < scw; x++) {
                int sx = (int)((int64_t)x * cw / scw);
                du[x] = src[ysz + so + sx];
                dv[x] = src[ysz + usz + so + sx];
            }
        }
    }
    pthread_mutex_unlock(&cap->lock);

    /* 编码在 cap->lock 外：enc/frm 仅 bridge 线程访问；调用方持 panel 的
       g_caps_lock，主线程 capture_stop 前会经同锁解绑，不会中途释放 */
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
    if (cap->aiv1) {
        /* shutdown 立即唤醒阻塞在 recv 的 AIV1 线程（SO_RCVTIMEO 最长 10s） */
        int fd = cap->aiv1_fd;
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
    }
    if (cap->thread_started) {
        pthread_join(cap->thread, NULL);
        cap->thread_started = false;
    }
    pthread_mutex_destroy(&cap->lock);
    free(cap->slot.data);
    cap->slot.data = NULL;
    cap->slot.w = cap->slot.h = 0;
    free(cap->aiv1_buf);
    cap->aiv1_buf = NULL;
    cap->aiv1_buf_cap = 0;
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

/* 缩放映射表上限（crop_1x2 的 tile 固定 400×480，留余量） */
#define FIT_MAX_W 512
#define FIT_MAX_H 512

int capture_fit_window(int src_w, int src_h, int squeeze_pct, int out_w, int out_h)
{
    if (src_w <= 0 || src_h <= 0 || out_h <= 0) return 0;
    if (squeeze_pct < 100) squeeze_pct = 100;
    int win = (int)((int64_t)src_h * out_w * squeeze_pct / ((int64_t)out_h * 100)) & ~1;
    if (win > src_w) win = src_w & ~1;
    if (win < 2) win = 2;
    return win;
}

/* 一维采样表：输出 n 点映射到窗口 [org, org+win) 内的双线性两点与权重（0..256）。
   采样中心对齐：s = (i+0.5)·win/n − 0.5，相对窗口起点，取下标再加 org */
static void build_axis(int n, int win, int org, int16_t *i0, int16_t *i1, int16_t *fw)
{
    for (int i = 0; i < n; i++) {
        double sp = (i + 0.5) * (double)win / n - 0.5;
        int a = (int)sp;
        if (a < 0) a = 0;
        if (a > win - 1) a = win - 1;
        int b = a + 1 < win ? a + 1 : a;
        double f = sp - a;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        i0[i] = (int16_t)(org + a);
        i1[i] = (int16_t)(org + b);
        fw[i] = (int16_t)(f * 256 + 0.5);
    }
}

/* 单平面二维双线性：src 行距 sstride，输出 ow×oh */
static void scale_plane(const uint8_t *src, int sstride, uint8_t *dst, int dstride,
                        int ow, int oh,
                        const int16_t *x0, const int16_t *x1, const int16_t *xf,
                        const int16_t *y0, const int16_t *y1, const int16_t *yf)
{
    for (int r = 0; r < oh; r++) {
        const uint8_t *ra = src + (size_t)y0[r] * sstride;
        const uint8_t *rb = src + (size_t)y1[r] * sstride;
        uint8_t *d = dst + (size_t)r * dstride;
        int fy = yf[r];
        if (fy == 0) {          /* 行对齐（垂直 1:1，如 480 高源）：只做水平插值 */
            for (int x = 0; x < ow; x++)
                d[x] = (uint8_t)((ra[x0[x]] * (256 - xf[x]) + ra[x1[x]] * xf[x] + 128) >> 8);
            continue;
        }
        for (int x = 0; x < ow; x++) {
            int ha = ra[x0[x]] * (256 - xf[x]) + ra[x1[x]] * xf[x];
            int hb = rb[x0[x]] * (256 - xf[x]) + rb[x1[x]] * xf[x];
            d[x] = (uint8_t)((ha * (256 - fy) + hb * fy + 32768) >> 16);
        }
    }
}

int64_t capture_paint_fit(capture_t *cap, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                          int dst_y_stride, int dst_c_stride,
                          int crop_x, int squeeze_pct, int out_w, int out_h,
                          int *src_w, int *src_h, int *win_w)
{
    if (out_w <= 0 || out_h <= 0 || out_w > FIT_MAX_W || out_h > FIT_MAX_H ||
        (out_w | out_h) & 1)
        return -3;
    pthread_mutex_lock(&cap->lock);
    if (!cap->slot.data) {
        pthread_mutex_unlock(&cap->lock);
        return -1;
    }
    int w = cap->slot.w, h = cap->slot.h;
    int ww = capture_fit_window(w, h, squeeze_pct, out_w, out_h);
    if (src_w) *src_w = w;
    if (src_h) *src_h = h;
    if (win_w) *win_w = ww;
    if (crop_x < 0) crop_x = (w - ww) / 2;
    if (crop_x > w - ww) crop_x = w - ww;
    crop_x &= ~1;   /* 色度平面半宽：奇数起点会半像素错位 */

    const uint8_t *src = cap->slot.data;
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    if (ww == out_w && h == out_h) {
        /* 窗口与 tile 等大（848×480 源 1.0×）：逐行直拷 */
        for (int row = 0; row < out_h; row++)
            memcpy(dst_y + (size_t)row * dst_y_stride, src + (size_t)row * w + crop_x, out_w);
        for (int row = 0; row < out_h / 2; row++) {
            memcpy(dst_u + (size_t)row * dst_c_stride,
                   src + ysz + (size_t)row * (w / 2) + crop_x / 2, out_w / 2);
            memcpy(dst_v + (size_t)row * dst_c_stride,
                   src + ysz + usz + (size_t)row * (w / 2) + crop_x / 2, out_w / 2);
        }
    } else {
        int16_t lx0[FIT_MAX_W], lx1[FIT_MAX_W], lxf[FIT_MAX_W];
        int16_t ly0[FIT_MAX_H], ly1[FIT_MAX_H], lyf[FIT_MAX_H];
        build_axis(out_w, ww, crop_x, lx0, lx1, lxf);
        build_axis(out_h, h, 0, ly0, ly1, lyf);
        scale_plane(src, w, dst_y, dst_y_stride, out_w, out_h, lx0, lx1, lxf, ly0, ly1, lyf);
        /* 色度：同比例、原点按半分辨率各自对齐 */
        build_axis(out_w / 2, ww / 2, crop_x / 2, lx0, lx1, lxf);
        build_axis(out_h / 2, h / 2, 0, ly0, ly1, lyf);
        scale_plane(src + ysz, w / 2, dst_u, dst_c_stride, out_w / 2, out_h / 2,
                    lx0, lx1, lxf, ly0, ly1, lyf);
        scale_plane(src + ysz + usz, w / 2, dst_v, dst_c_stride, out_w / 2, out_h / 2,
                    lx0, lx1, lxf, ly0, ly1, lyf);
    }
    int64_t age = now_ms() - cap->slot.updated_ms;
    pthread_mutex_unlock(&cap->lock);
    return age;
}

void capture_note(capture_t *cap, char *out, size_t cap_len)
{
    pthread_mutex_lock(&cap->lock);
    snprintf(out, cap_len, "%s", cap->note);
    pthread_mutex_unlock(&cap->lock);
}
