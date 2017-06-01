#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>

#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

static AVFormatContext *ifmt_ctx;
static AVFormatContext *ofmt_ctx;

typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
} StreamContext;
static StreamContext *stream_ctx;

typedef struct OutputStream {
    AVStream *st;
    AVCodecContext *enc;
    /* pts of the next frame that will be generated */
    int64_t next_pts;
    AVFrame *frame;
} OutputStream;

static OutputStream ost = {0};

static int video_idx;


static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;

    /* Write the compressed frame to the media file. */
    log_packet(fmt_ctx, pkt);
    return av_interleaved_write_frame(fmt_ctx, pkt);
}

static int open_input_file(const char *src_filename)
{
    int ret;
    unsigned int i;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, src_filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    stream_ctx = av_mallocz_array(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (ifmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = i;
            AVStream *stream = ifmt_ctx->streams[i];
            AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
            AVCodecContext *codec_ctx;
            if (!dec) {
                av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
                return AVERROR_DECODER_NOT_FOUND;
            }
            codec_ctx = avcodec_alloc_context3(dec);
            if (!codec_ctx) {
                av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
                return AVERROR(ENOMEM);
            }
            ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
            codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
            stream_ctx[0].dec_ctx = codec_ctx;
        }
    }
    av_dump_format(ifmt_ctx, 0, src_filename, 0);
    return 0;
}


static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *picture;
    int ret;

    picture = av_frame_alloc();
    if (!picture)
        return NULL;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return picture;
}


static void fill_yuv_image(AVFrame *pict, int frame_index,
                           int width, int height)
{
    int x, y, i;

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

static int open_output_file(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i;

    ofmt_ctx = NULL;
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", filename);
    if (!ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        in_stream = ifmt_ctx->streams[i];
        if (i == video_idx) {
            ost.st = out_stream;
            dec_ctx = stream_ctx[0].dec_ctx;
            encoder = avcodec_find_encoder(dec_ctx->codec_id);

            if (!encoder) {
                av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
                return AVERROR_INVALIDDATA;
            }
            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
                av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
                return AVERROR(ENOMEM);
            }

            ost.enc = enc_ctx;

            stream_ctx[0].enc_ctx = enc_ctx;
            enc_ctx->codec_id = dec_ctx->codec_id;

            enc_ctx->bit_rate = 400000;
            enc_ctx->width    = 352;
            enc_ctx->height   = 288;
            out_stream->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
            enc_ctx->time_base       = ost.st->time_base;

            enc_ctx->gop_size      = 12; /* emit one intra frame every twelve frames at most */
            enc_ctx->pix_fmt       = STREAM_PIX_FMT;

            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
                return ret;
            }

            ost.frame = alloc_picture(enc_ctx->pix_fmt, enc_ctx->width, enc_ctx->height);
            if (!ost.frame) {
                printf("Could not allocate video frame");
                exit(1);
            }

            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
                return ret;
            }
        } else {
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                printf( "Failed to copy context from input to output stream codec context\n");
                return ret;
            }
            out_stream->time_base = in_stream->time_base;
        }
        // if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
        out_stream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }
    return 0;
}

static AVFrame *get_video_frame(OutputStream *ost)
{
    AVCodecContext *c = ost->enc;

    if (av_frame_make_writable(ost->frame) < 0)
        exit(1);

    fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);

    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}


static int filter_encode_write_frame(AVFrame *frame, unsigned int stream_index)
{
    int ret;
    AVFrame *filt_frame;
    AVCodecContext *c;
    int got_packet = 0;
    AVPacket pkt = { 0 };

    av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters\n");

    c = ost.enc;

    filt_frame = get_video_frame(&ost);

    av_init_packet(&pkt);

    /* encode the image */
    ret = avcodec_encode_video2(c, &pkt, filt_frame, &got_packet);
    if (ret < 0) {
        fprintf(stderr, "Error encoding video frame: %s\n", av_err2str(ret));
        exit(1);
    }

    if (got_packet) {
        ret = write_frame(ofmt_ctx, &c->time_base, ost.st, &pkt);
    } else {
        ret = 0;
    }

    if (ret < 0) {
        fprintf(stderr, "Error while writing video frame: %s\n", av_err2str(ret));
        exit(1);
    }

    return ret;
}
int main(int argc, char *argv[])
{
    const char * src_filename = "rtmp://live.hkstv.hk.lxdns.com/live/hks";
    //const char * src_filename = "test.flv";
    const char * dst_filename = "rtmp://localhost/publishlive/livestream";
    //const char * dst_filename = "demo.flv";

    int frame_count = 0;

    int ret, got_frame;
    AVPacket packet = { .data = NULL, .size = 0 };
    AVFrame *frame = NULL;
    enum AVMediaType type;

    // Register
    av_register_all();
    // Network
    avformat_network_init();

    if ((ret = open_input_file(src_filename)) < 0)
        goto end;
    if ((ret = open_output_file(dst_filename)) < 0)
        goto end;

    while (1) {
        if(ret = av_read_frame(ifmt_ctx, &packet) < 0)
            break;
        if(packet.stream_index == video_idx){
            printf("Send %8d video frames to output URL\n",frame_count);
            frame_count++;
            frame = av_frame_alloc();
            if (!frame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            ret = avcodec_decode_video2(stream_ctx[0].dec_ctx, frame,
                    &got_frame, &packet);
            if (ret < 0) {
                av_frame_free(&frame);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }

            if (got_frame) {
                frame->pts = av_frame_get_best_effort_timestamp(frame);
                ret = filter_encode_write_frame(frame, video_idx);
                av_frame_free(&frame);
                if (ret < 0)
                    goto end;
            } else {
                av_frame_free(&frame);
            }
        } else {
            log_packet(ofmt_ctx, &packet);

            ret = av_interleaved_write_frame(ofmt_ctx, &packet);

            if (ret < 0) {
                printf( "Error muxing packet\n");
                break;
            }
        }

        av_packet_unref(&packet);
    }

    av_write_trailer(ofmt_ctx);
end:
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_close(ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    if (ret < 0 && ret != AVERROR_EOF) {
        printf( "Error occurred.\n");
        return -1;
    }
    return 0;
}
