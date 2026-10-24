#define _GNU_SOURCE
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
        if (frm->format == AV_PIX_FMT_YUV420P ||
            frm->format == AV_PIX_FMT_YUVJ420P) {
            slot_store(cap, frm);   /* yuvj420p 与 yuv420p 内存布局相同 */
        } else {
            if (frm->format != cap->warned_fmt) {
                cap->warned_fmt = frm->format;
                fprintf(stderr, "multicam: 像素格式 %s 不支持，帧被丢弃（本格式只告警一次）\n",
                        av_get_pix_fmt_name((enum AVPixelFormat)frm->format));
            }
            cap->pixfmt_drops++;
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
            usleep(3 * 1000 * 1000);
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
            if (!cap->last_error[0])
                snprintf(cap->last_error, sizeof(cap->last_error), "AIVH 握手失败");
            else
                snprintf(cap->last_error, sizeof(cap->last_error),
                         "AIVH 握手失败: %.80s", cap->last_error);
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

/* 缩放映射表宽度上限（crop_1x2 的 tile 固定 400，留余量到 512） */
#define SQUEEZE_MAX_W 512

int64_t capture_paint_squeeze(capture_t *cap, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                              int dst_y_stride, int dst_c_stride,
                              int crop_x, int crop_y,
                              int win_w, int win_h,
                              int out_w, int out_h)
{
    if (out_w <= 0 || out_w > SQUEEZE_MAX_W || out_w & 1) return -3;
    pthread_mutex_lock(&cap->lock);
    if (!cap->slot.data) {
        pthread_mutex_unlock(&cap->lock);
        return -1;
    }
    int w = cap->slot.w, h = cap->slot.h;
    if (win_w <= 0 || win_h <= 0 || win_h != out_h || h < win_h || win_w & 1) {
        pthread_mutex_unlock(&cap->lock);
        return -2;
    }
    if (w < win_w) {   /* 窗口宽于源（低分辨率 rtsp 源）：收敛为整幅 */
        win_w = w & ~1;
        crop_x = 0;
    }
    crop_x &= ~1;   /* 与 capture_paint_crop 一致：色度平面半宽，奇数起点会半像素错位 */
    crop_y &= ~1;
    if (crop_y < 0) crop_y = 0;
    if (crop_y + win_h > h) crop_y = (h - win_h) & ~1;
    if (crop_x < 0) crop_x = (w - win_w) / 2 & ~1;
    if (crop_x > w - win_w) crop_x = (w - win_w) & ~1;

    /* 水平映射表：亮度与色度各自建（采样中心对齐，缩放比相同但原点差半像素）。
       f 取 0..256 定点，(a*(256-f)+b*f+128)>>8 双线性插值，f=0 退化为直拷。 */
    int16_t lx0[SQUEEZE_MAX_W], lx1[SQUEEZE_MAX_W], lfw[SQUEEZE_MAX_W];
    int16_t cx0[SQUEEZE_MAX_W / 2], cx1[SQUEEZE_MAX_W / 2], cfw[SQUEEZE_MAX_W / 2];
    for (int x = 0; x < out_w; x++) {
        double s = crop_x + (x + 0.5) * (double)win_w / out_w - 0.5;
        int x0 = (int)s;
        if (x0 < 0) x0 = 0;
        if (x0 > win_w - 1) x0 = win_w - 1;
        int x1 = x0 + 1 < win_w ? x0 + 1 : x0;
        double f = s - x0;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        lx0[x] = (int16_t)(crop_x + x0);
        lx1[x] = (int16_t)(crop_x + x1);
        lfw[x] = (int16_t)(f * 256 + 0.5);
    }
    int cw_out = out_w / 2, cw_win = win_w / 2, cw_crop = crop_x / 2;
    for (int x = 0; x < cw_out; x++) {
        double s = cw_crop + (x + 0.5) * (double)cw_win / cw_out - 0.5;
        int x0 = (int)s;
        if (x0 < 0) x0 = 0;
        if (x0 > cw_win - 1) x0 = cw_win - 1;
        int x1 = x0 + 1 < cw_win ? x0 + 1 : x0;
        double f = s - x0;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        cx0[x] = (int16_t)(cw_crop + x0);
        cx1[x] = (int16_t)(cw_crop + x1);
        cfw[x] = (int16_t)(f * 256 + 0.5);
    }

    const uint8_t *src = cap->slot.data;
    size_t ysz = (size_t)w * h, usz = ysz / 4;
    for (int row = 0; row < out_h; row++) {
        const uint8_t *sy = src + (size_t)(crop_y + row) * w;
        uint8_t *dy = dst_y + (size_t)row * dst_y_stride;
        for (int x = 0; x < out_w; x++)
            dy[x] = (uint8_t)((sy[lx0[x]] * (256 - lfw[x]) + sy[lx1[x]] * lfw[x] + 128) >> 8);
    }
    for (int row = 0; row < out_h / 2; row++) {
        const uint8_t *su = src + ysz + (size_t)(crop_y / 2 + row) * (w / 2);
        const uint8_t *sv = su + usz;
        uint8_t *du = dst_u + (size_t)row * dst_c_stride;
        uint8_t *dv = dst_v + (size_t)row * dst_c_stride;
        for (int x = 0; x < cw_out; x++) {
            du[x] = (uint8_t)((su[cx0[x]] * (256 - cfw[x]) + su[cx1[x]] * cfw[x] + 128) >> 8);
            dv[x] = (uint8_t)((sv[cx0[x]] * (256 - cfw[x]) + sv[cx1[x]] * cfw[x] + 128) >> 8);
        }
    }
    int64_t age = now_ms() - cap->slot.updated_ms;
    pthread_mutex_unlock(&cap->lock);
    return age;
}
