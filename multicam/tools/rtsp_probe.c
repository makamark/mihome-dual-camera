/* 快速 RTSP 打开测试：与 src/capture.c 相同代码路径 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    char errbuf[256];
    AVFormatContext *fmt = avformat_alloc_context();
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "stimeout", "5000000", 0);
    int rc = avformat_open_input(&fmt, argv[1], NULL, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        av_strerror(rc, errbuf, sizeof(errbuf));
        fprintf(stderr, "open_input rc=%d (%s)\n", rc, errbuf);
        return 1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "find_stream_info failed\n");
        return 1;
    }
    int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (idx < 0) { fprintf(stderr, "no video\n"); return 1; }
    AVStream *st = fmt->streams[idx];
    fprintf(stderr, "OK %s %dx%d fps=%d/%d codec=%d\n", argv[1],
            st->codecpar->width, st->codecpar->height,
            st->avg_frame_rate.num, st->avg_frame_rate.den,
            st->codecpar->codec_id);
    avformat_close_input(&fmt);
    return 0;
}
