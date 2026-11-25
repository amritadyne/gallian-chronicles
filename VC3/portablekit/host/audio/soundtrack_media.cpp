#include "audio/soundtrack_media.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <system_error>

#if defined(PORTABLEKIT_HAS_SOUNDTRACK_IMPORT)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#endif

namespace portablekit::audio::soundtrack {

#if defined(PORTABLEKIT_HAS_SOUNDTRACK_IMPORT)

namespace {

std::string utf8(const std::filesystem::path &p) {
    const auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

std::string av_message(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof text);
    return text;
}

// The leading number of "3", "3/25" or "03 of 25"; 0 when there is none.
int leading_number(const char *text) {
    if (text == nullptr) return 0;
    while (*text == ' ') ++text;
    return std::isdigit(static_cast<unsigned char>(*text)) ? std::atoi(text) : 0;
}

const char *tag(const AVDictionary *a, const AVDictionary *b, const char *key) {
    if (const AVDictionaryEntry *e = av_dict_get(a, key, nullptr, AV_DICT_IGNORE_SUFFIX)) return e->value;
    if (const AVDictionaryEntry *e = av_dict_get(b, key, nullptr, AV_DICT_IGNORE_SUFFIX)) return e->value;
    return nullptr;
}

// FFmpeg's own messages (an MP3's "Could not update timestamps for skipped
// samples" and the like) say nothing the player can act on: errors only.
void quiet_ffmpeg() {
    static const bool once = [] {
        av_log_set_level(AV_LOG_ERROR);
        return true;
    }();
    (void)once;
}

struct Input {
    AVFormatContext *format{};
    AVCodecContext *codec{};
    int stream{-1};
    ~Input() {
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    bool open(const std::filesystem::path &file, bool with_decoder, std::string &error) {
        quiet_ffmpeg();
        int r = avformat_open_input(&format, utf8(file).c_str(), nullptr, nullptr);
        if (r < 0) {
            error = av_message(r);
            return false;
        }
        if ((r = avformat_find_stream_info(format, nullptr)) < 0) {
            error = av_message(r);
            return false;
        }
        const AVCodec *decoder = nullptr;
        stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
        if (stream < 0 || decoder == nullptr) {
            error = "no audio in the file";
            return false;
        }
        if (!with_decoder) return true;
        codec = avcodec_alloc_context3(decoder);
        if (codec == nullptr || avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) < 0) {
            error = "can't set up the decoder";
            return false;
        }
        if ((r = avcodec_open2(codec, decoder, nullptr)) < 0) {
            error = av_message(r);
            return false;
        }
        return true;
    }
};

} // namespace

bool media_available() noexcept { return true; }

std::optional<FileInfo> probe(const std::filesystem::path &file) {
    Input in;
    std::string error;
    if (!in.open(file, false, error)) return std::nullopt;
    FileInfo info;
    const AVStream *s = in.format->streams[in.stream];
    if (s->duration != AV_NOPTS_VALUE)
        info.seconds = static_cast<double>(s->duration) * av_q2d(s->time_base);
    else if (in.format->duration != AV_NOPTS_VALUE)
        info.seconds = static_cast<double>(in.format->duration) / AV_TIME_BASE;
    const AVDictionary *a = in.format->metadata;
    const AVDictionary *b = s->metadata;
    if (const char *t = tag(a, b, "title")) info.title = t;
    if (const char *t = tag(a, b, "album")) info.album = t;
    info.track = leading_number(tag(a, b, "track"));
    info.disc = leading_number(tag(a, b, "disc"));
    return info;
}

std::optional<Stereo> decode(const std::filesystem::path &file, std::string &error) {
    Input in;
    if (!in.open(file, true, error)) return std::nullopt;
    SwrContext *swr = nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (in.codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&in.codec->ch_layout, in.codec->ch_layout.nb_channels);
    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, kRate, &in.codec->ch_layout, in.codec->sample_fmt,
                            in.codec->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr) < 0) {
        swr_free(&swr);
        error = "can't convert the file's audio to 44.1 kHz stereo";
        return std::nullopt;
    }
    Stereo out;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    std::vector<float> chunk;
    const auto convert = [&](const AVFrame *f) {
        const int room = swr_get_out_samples(swr, f ? f->nb_samples : 0);
        if (room <= 0) return;
        chunk.resize(static_cast<std::size_t>(room) * 2);
        auto *dst = reinterpret_cast<std::uint8_t *>(chunk.data());
        const int got = swr_convert(swr, &dst, room, f ? const_cast<const std::uint8_t **>(f->extended_data) : nullptr,
                                    f ? f->nb_samples : 0);
        if (got <= 0) return;
        for (int i = 0; i < got * 2; ++i) out.samples.push_back(chunk[static_cast<std::size_t>(i)] * 32768.0f);
    };
    const auto drain = [&]() {
        while (avcodec_receive_frame(in.codec, frame) >= 0) {
            convert(frame);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(in.format, packet) >= 0) {
        if (packet->stream_index == in.stream && avcodec_send_packet(in.codec, packet) >= 0) drain();
        av_packet_unref(packet);
    }
    avcodec_send_packet(in.codec, nullptr);
    drain();
    convert(nullptr); // what the resampler still holds
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    if (out.samples.empty()) {
        error = "the file decoded to nothing";
        return std::nullopt;
    }
    return out;
}

Stereo change_tempo(const Stereo &input, double tempo) {
    Stereo out;
    AVFilterGraph *graph = avfilter_graph_alloc();
    AVFilterContext *source = nullptr, *sink = nullptr, *stretch = nullptr, *format = nullptr;
    char args[256];
    std::snprintf(args, sizeof args, "sample_rate=%d:sample_fmt=flt:channel_layout=stereo:time_base=1/%d", kRate, kRate);
    char tempo_args[64];
    std::snprintf(tempo_args, sizeof tempo_args, "tempo=%.10f", tempo);
    bool ok = graph != nullptr &&
              avfilter_graph_create_filter(&source, avfilter_get_by_name("abuffer"), "in", args, nullptr, graph) >= 0 &&
              avfilter_graph_create_filter(&stretch, avfilter_get_by_name("atempo"), "tempo", tempo_args, nullptr, graph) >= 0 &&
              avfilter_graph_create_filter(&format, avfilter_get_by_name("aformat"), "format",
                                           "sample_fmts=flt:sample_rates=44100:channel_layouts=stereo", nullptr, graph) >= 0 &&
              avfilter_graph_create_filter(&sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph) >= 0 &&
              avfilter_link(source, 0, stretch, 0) >= 0 && avfilter_link(stretch, 0, format, 0) >= 0 &&
              avfilter_link(format, 0, sink, 0) >= 0 && avfilter_graph_config(graph, nullptr) >= 0;
    AVFrame *frame = av_frame_alloc();
    const auto pull = [&]() {
        while (av_buffersink_get_frame(sink, frame) >= 0) {
            const auto *data = reinterpret_cast<const float *>(frame->extended_data[0]);
            out.samples.insert(out.samples.end(), data, data + frame->nb_samples * 2);
            av_frame_unref(frame);
        }
    };
    constexpr int kChunk = 4096;
    const std::size_t frames = input.frames();
    for (std::size_t at = 0; ok && at < frames; at += kChunk) {
        const int n = static_cast<int>(std::min<std::size_t>(kChunk, frames - at));
        AVFrame *in = av_frame_alloc();
        in->format = AV_SAMPLE_FMT_FLT;
        in->sample_rate = kRate;
        in->nb_samples = n;
        in->pts = static_cast<std::int64_t>(at);
        av_channel_layout_default(&in->ch_layout, 2);
        if (av_frame_get_buffer(in, 0) < 0) {
            av_frame_free(&in);
            ok = false;
            break;
        }
        auto *dst = reinterpret_cast<float *>(in->extended_data[0]);
        for (int i = 0; i < n * 2; ++i) dst[i] = input.samples[at * 2 + static_cast<std::size_t>(i)] / 32768.0f;
        if (av_buffersrc_add_frame(source, in) < 0) ok = false;
        av_frame_free(&in);
        pull();
    }
    if (ok && av_buffersrc_add_frame(source, nullptr) >= 0) pull(); // flush what the filter still holds
    for (float &v : out.samples) v *= 32768.0f;
    av_frame_free(&frame);
    avfilter_graph_free(&graph);
    if (!ok) out.samples.clear();
    return out;
}

bool write_flac(const std::filesystem::path &file, const std::vector<std::int16_t> &pcm, std::string &error) {
    const std::filesystem::path part = file.string() + ".part";
    AVFormatContext *out = nullptr;
    AVCodecContext *enc = nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool ok = false;
    do {
        if (avformat_alloc_output_context2(&out, nullptr, "flac", utf8(part).c_str()) < 0 || out == nullptr) {
            error = "can't make a FLAC file";
            break;
        }
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
        if (codec == nullptr) {
            error = "this FFmpeg has no FLAC encoder";
            break;
        }
        enc = avcodec_alloc_context3(codec);
        enc->sample_fmt = AV_SAMPLE_FMT_S16;
        enc->sample_rate = kRate;
        enc->time_base = AVRational{1, kRate};
        av_channel_layout_default(&enc->ch_layout, 2);
        if (avcodec_open2(enc, codec, nullptr) < 0) {
            error = "can't start the FLAC encoder";
            break;
        }
        AVStream *stream = avformat_new_stream(out, nullptr);
        if (stream == nullptr || avcodec_parameters_from_context(stream->codecpar, enc) < 0) {
            error = "can't make a FLAC stream";
            break;
        }
        stream->time_base = enc->time_base;
        int r = avio_open(&out->pb, utf8(part).c_str(), AVIO_FLAG_WRITE);
        if (r < 0) {
            error = av_message(r);
            break;
        }
        if ((r = avformat_write_header(out, nullptr)) < 0) {
            error = av_message(r);
            break;
        }
        const auto send = [&](AVFrame *f) -> bool {
            if (avcodec_send_frame(enc, f) < 0) return false;
            while (avcodec_receive_packet(enc, packet) >= 0) {
                av_packet_rescale_ts(packet, enc->time_base, stream->time_base);
                packet->stream_index = stream->index;
                if (av_interleaved_write_frame(out, packet) < 0) return false;
            }
            return true;
        };
        const int per_frame = enc->frame_size > 0 ? enc->frame_size : 4608;
        const std::size_t frames = pcm.size() / 2;
        bool sent = true;
        for (std::size_t at = 0; sent && at < frames; at += static_cast<std::size_t>(per_frame)) {
            const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(per_frame), frames - at));
            frame->format = AV_SAMPLE_FMT_S16;
            frame->sample_rate = kRate;
            frame->nb_samples = n;
            frame->pts = static_cast<std::int64_t>(at);
            av_channel_layout_default(&frame->ch_layout, 2);
            if (av_frame_get_buffer(frame, 0) < 0) {
                sent = false;
                break;
            }
            std::memcpy(frame->extended_data[0], pcm.data() + at * 2, static_cast<std::size_t>(n) * 4);
            sent = send(frame);
            av_frame_unref(frame);
        }
        if (!sent || !send(nullptr) || av_write_trailer(out) < 0) {
            error = "writing the FLAC file failed";
            break;
        }
        ok = true;
    } while (false);
    if (out != nullptr && out->pb != nullptr) avio_closep(&out->pb);
    avformat_free_context(out);
    avcodec_free_context(&enc);
    av_frame_free(&frame);
    av_packet_free(&packet);
    std::error_code ec;
    if (!ok) {
        std::filesystem::remove(part, ec);
        return false;
    }
    std::filesystem::remove(file, ec);
    std::filesystem::rename(part, file, ec);
    if (ec) {
        error = "can't replace " + utf8(file.filename()) + ": " + ec.message();
        return false;
    }
    return true;
}

std::optional<std::vector<std::int16_t>> read_pcm16(const std::filesystem::path &file, std::string &error) {
    std::optional<Stereo> audio = decode(file, error);
    if (!audio) return std::nullopt;
    std::vector<std::int16_t> out(audio->samples.size());
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<std::int16_t>(std::clamp(std::nearbyint(audio->samples[i]), -32768.0f, 32767.0f));
    return out;
}

#else // without libavformat / libavfilter / libswresample

bool media_available() noexcept { return false; }
std::optional<FileInfo> probe(const std::filesystem::path &) { return std::nullopt; }
std::optional<Stereo> decode(const std::filesystem::path &, std::string &error) {
    error = "this build can't read audio files";
    return std::nullopt;
}
Stereo change_tempo(const Stereo &, double) { return {}; }
bool write_flac(const std::filesystem::path &, const std::vector<std::int16_t> &, std::string &error) {
    error = "this build can't write FLAC";
    return false;
}
std::optional<std::vector<std::int16_t>> read_pcm16(const std::filesystem::path &, std::string &error) {
    error = "this build can't read FLAC";
    return std::nullopt;
}

#endif

} // namespace portablekit::audio::soundtrack
