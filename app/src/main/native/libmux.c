#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <jni.h>
#include <android/log.h>

const char* LOG_TAG = "LIBMUX";
const size_t BUFF_SIZE = 4096;

int to_android_log_level(int level) {
    switch (level) {
        case AV_LOG_PANIC:
        case AV_LOG_FATAL: return ANDROID_LOG_FATAL;
        case AV_LOG_ERROR: return ANDROID_LOG_ERROR;
        case AV_LOG_WARNING: return ANDROID_LOG_WARN;
        case AV_LOG_INFO: return ANDROID_LOG_INFO;
        default: return ANDROID_LOG_UNKNOWN;
    }
}

void log_callback(void *ptr, int level, const char *fmt, va_list vl)
{
    char line[1024];
    static int print_prefix = 1;
    av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);
    __android_log_print(to_android_log_level(level), LOG_TAG, "%s", line);
}

void log_err(int result, const char* name) {
    if (result < 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s: %d", name, result);
    }
}

struct Muxer {
    JavaVM* jvm;
    jobject thiz;
    jmethodID write_packet_method_id;
    jmethodID seek_method_id;
};

int write_packet(void* opaque, uint8_t* buff, int buff_size) {
    struct Muxer* muxer = opaque;
    JNIEnv *env;
    (*muxer->jvm)->GetEnv(muxer->jvm, (void **) &env, JNI_VERSION_1_6);
    jbyteArray jarray = (*env)->NewByteArray(env, buff_size);
    (*env)->SetByteArrayRegion(env, jarray, 0, buff_size, (const signed char*)buff);
    (*env)->CallVoidMethod(env, muxer->thiz, muxer->write_packet_method_id, jarray, buff_size);
    return buff_size;
}

int64_t seek(void* opaque, int64_t offset, int whence) {
    struct Muxer* muxer = opaque;
    JNIEnv *env;
    (*muxer->jvm)->GetEnv(muxer->jvm, (void **) &env, JNI_VERSION_1_6);
    (*env)->CallVoidMethod(env, muxer->thiz, muxer->seek_method_id, offset);
    return offset;
}

jlong Java_sushi_hardcore_droidfs_video_1recording_FFmpegMuxer_allocContext(JNIEnv *env, jobject thiz) {
    av_log_set_callback(log_callback);
    av_log_set_level(AV_LOG_INFO);
    const AVOutputFormat *format = av_guess_format("mp4", NULL, NULL);
    struct Muxer* muxer = malloc(sizeof(struct Muxer));
    (*env)->GetJavaVM(env, &muxer->jvm);
    muxer->thiz = (*env)->NewGlobalRef(env, thiz);
    jclass class = (*env)->GetObjectClass(env, thiz);
    muxer->write_packet_method_id = (*env)->GetMethodID(env, class, "writePacket", "([B)V");
    muxer->seek_method_id = (*env)->GetMethodID(env, class, "seek", "(J)V");
    AVIOContext* avio_context = avio_alloc_context(av_malloc(BUFF_SIZE), BUFF_SIZE, 1, muxer, NULL, write_packet, seek);
    AVFormatContext* fc = avformat_alloc_context();
    fc->oformat = format;
    fc->pb = avio_context;
    return (jlong) fc;
}

JNIEXPORT jint JNICALL
Java_sushi_hardcore_droidfs_video_1recording_FFmpegMuxer_addAudioTrack(JNIEnv *env, jobject thiz, jlong format_context, jint bitrate, jint sample_rate,
                                                                       jint channel_count) {
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext* codec_context = avcodec_alloc_context3(encoder);
    av_channel_layout_default(&codec_context->ch_layout, channel_count);
    codec_context->sample_rate = sample_rate;
    codec_context->sample_fmt = encoder->sample_fmts[0];
    codec_context->bit_rate = bitrate;
    codec_context->time_base = (AVRational) {1, sample_rate};
    avcodec_open2(codec_context, encoder, NULL);
    AVStream* stream = avformat_new_stream((AVFormatContext *) format_context, NULL);
    avcodec_parameters_from_context(stream->codecpar, codec_context);
    avcodec_free_context(&codec_context);
    return stream->index;
}

JNIEXPORT jint JNICALL
Java_sushi_hardcore_droidfs_video_1recording_FFmpegMuxer_addVideoTrack(JNIEnv *env, jobject thiz,
                                                                       jlong format_context,
                                                                       jint bitrate,
                                                                       jint frame_rate,
                                                                       jint width,
                                                                       jint height,
                                                                       jint orientation_hint) {
    AVStream* stream = avformat_new_stream((AVFormatContext *) format_context, NULL);
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_H264;
    stream->codecpar->bit_rate = bitrate;
    stream->codecpar->width = width;
    stream->codecpar->height = height;
    stream->codecpar->format = AV_PIX_FMT_YUVJ420P;
    stream->time_base = (AVRational) {1, frame_rate};
    uint8_t* matrix = av_stream_new_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, sizeof(int32_t) * 9);
    av_display_rotation_set((int32_t *) matrix, orientation_hint);
    return stream->index;
}

JNIEXPORT jint JNICALL
Java_sushi_hardcore_droidfs_video_1recording_FFmpegMuxer_writeHeaders(JNIEnv *env, jobject thiz, jlong format_context) {
    av_dump_format((AVFormatContext *) format_context, 0, NULL, 1);
    int result = avformat_write_header((AVFormatContext *) format_context, NULL);
    log_err(result, "avformat_write_header");
    return result;
}

JNIEXPORT void JNICALL
Java_sushi_hardcore_droidfs_video_1recording_FFmpegMuxer_writePacket(JNIEnv *env, jobject thiz, jlong format_context,
                                                                     jbyteArray buffer, jlong pts, jint stream_index,
                                                                     jboolean is_key_frame) {
    AVPacket* packet = av_packet_alloc();
    int size = (*env)->GetArrayLength(env, buffer);
    av_new_packet(packet, size);
    packet->stream_index = stream_index;
    AVRational r;
    r.num = 1;
    r.den = 1000000;
    packet->pts = av_rescale_q(pts, r, ((AVFormatContext*)format_context)->streams[stream_index]->time_base);
    packet->dts = packet->pts;
    uint8_t* buff = malloc(size);
    (*env)->GetByteArrayRegion(env, buffer, 0, size, (signed char*)buff);
    packet->data = buff;
    if (is_key_frame) {
        packet->flags = AV_PKT_FLAG_KEY;
    }
    log_err(av_write_frame((AVFormatContext *)format_context, packet), "av_write_frame");
    free(buff);
    av_packet_free(&packet);
}

JNIEXPORT void JNICALL
Java_sushi_hardcore_droidfs_video_1recording_FFmpegMuxer_writeTrailer(JNIEnv *env, jobject thiz, jlong format_context) {
    log_err(av_write_trailer((AVFormatContext *) format_context), "av_write_trailer");
}

JNIEXPORT void JNICALL
Java_sushi_hardcore_droidfs_video_1recording_FFmpegMuxer_release(JNIEnv *env, jobject thiz, jlong format_context) {
    AVFormatContext* fc = (AVFormatContext *) format_context;
    av_free(fc->pb->buffer);
    (*env)->DeleteGlobalRef(env, ((struct Muxer*)fc->pb->opaque)->thiz);
    free(fc->pb->opaque);
    avio_context_free(&fc->pb);
    avformat_free_context(fc);
}