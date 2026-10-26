#include "audio/Decoder.h"

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QStringDecoder>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

// FFmpeg 5.1 replaced the channel layout bit masks by AVChannelLayout.
#define BILLY_CH_LAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100))

namespace {

void initFfmpeg()
{
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(qEnvironmentVariableIsSet("BILLY_FFMPEG_LOG") ? AV_LOG_INFO : AV_LOG_FATAL);
        avformat_network_init();
    });
}

constexpr qint64 kStreamStallMs = 30000; // give up a silent stream after this

qint64 nowMs()
{
    return av_gettime_relative() / 1000;
}

QString ffError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// ICY titles are usually UTF-8, but old servers send Latin-1.
QString decodeText(const QByteArray &raw)
{
    auto decoder = QStringDecoder(QStringDecoder::Utf8);
    QString text = decoder(raw);
    if (decoder.hasError())
        text = QString::fromLatin1(raw);
    return text.trimmed();
}

QString dictValue(AVDictionary *dict, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(dict, key, nullptr, 0);
    return e && e->value ? decodeText(QByteArray(e->value)) : QString();
}

} // namespace

struct Decoder::Private
{
    AVFormatContext *fmt = nullptr;
    AVCodecContext *codec = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    int streamIndex = -1;
    QString location;
    int outRate = 0;
    int nativeRate = 0;
    long long total = -1;
    long long produced = 0;   // output frames returned so far
    bool trimToTotal = false; // container length is exact, cut the encoder padding
    bool stream = false;
    bool seekable = false;

    // Current resampler input, to notice format changes inside a stream.
    int swrFormat = -1, swrRate = 0, swrChannels = 0;
    quint64 swrMask = 0;

    std::vector<float> pending; // converted stereo samples not yet returned
    size_t pendingPos = 0;
    bool sentFlush = false;
    bool eof = false;
    bool failed = false;
    QString error;

    double seekTarget = -1;     // seconds; drop decoded audio before it
    long long dropFrames = 0;

    std::shared_ptr<std::atomic_bool> abort = std::make_shared<std::atomic_bool>(false);
    std::atomic<qint64> lastProgress{0}; // streams: when audio last arrived
    bool stalled = false;

    // FFmpeg reconnects to a dropped stream forever; stop after a while.
    static int interruptCallback(void *opaque)
    {
        auto *p = static_cast<Private *>(opaque);
        if (p->abort->load())
            return 1;
        if (p->stream && p->lastProgress.load() && nowMs() - p->lastProgress.load() > kStreamStallMs) {
            p->stalled = true;
            return 1;
        }
        return 0;
    }
    QString name, title;
    bool metaChanged = false;
    int packetCounter = 0;

    ~Private() { close(); }

    void close()
    {
        swr_free(&swr);
        avcodec_free_context(&codec);
        if (fmt)
            avformat_close_input(&fmt);
        av_packet_free(&pkt);
        av_frame_free(&frame);
    }

    AVStream *st() const { return fmt->streams[streamIndex]; }

    bool setupResampler(const AVFrame *f)
    {
#if BILLY_CH_LAYOUT
        const int channels = f->ch_layout.nb_channels;
        const quint64 mask = f->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? f->ch_layout.u.mask : 0;
#else
        const int channels = f->channels;
        const quint64 mask = f->channel_layout;
#endif
        if (swr && f->format == swrFormat && f->sample_rate == swrRate && channels == swrChannels && mask == swrMask)
            return true;
        swr_free(&swr);
        swrFormat = f->format;
        swrRate = f->sample_rate;
        swrChannels = channels;
        swrMask = mask;
        if (channels <= 0 || f->sample_rate <= 0)
            return false;
#if BILLY_CH_LAYOUT
        AVChannelLayout in{};
        if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&in, channels);
        else
            av_channel_layout_copy(&in, &f->ch_layout);
        AVChannelLayout out{};
        av_channel_layout_default(&out, 2);
        const int ret = swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_FLT, outRate, &in, AVSampleFormat(f->format),
                                            f->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&in);
        av_channel_layout_uninit(&out);
        if (ret < 0)
            return false;
#else
        const qint64 inLayout = mask ? qint64(mask) : av_get_default_channel_layout(channels);
        swr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, outRate, inLayout,
                                 AVSampleFormat(f->format), f->sample_rate, 0, nullptr);
        if (!swr)
            return false;
#endif
        if (channels == 1) {
            // Mono goes to both speakers at full level (swr would use -3 dB).
            const double matrix[2] = {1.0, 1.0};
            swr_set_matrix(swr, matrix, 1);
        }
        if (swr_init(swr) < 0) {
            swr_free(&swr);
            return false;
        }
        return true;
    }

    void appendConverted(const uint8_t **in, int inSamples)
    {
        const int capacity = swr_get_out_samples(swr, inSamples) + 32;
        if (capacity <= 0)
            return;
        const size_t old = pending.size();
        pending.resize(old + size_t(capacity) * 2);
        uint8_t *out = reinterpret_cast<uint8_t *>(pending.data() + old);
        const int n = swr_convert(swr, &out, capacity, in, inSamples);
        pending.resize(old + size_t(qMax(0, n)) * 2);
        if (dropFrames > 0 && n > 0) {
            const long long k = qMin<long long>(dropFrames, n);
            pending.erase(pending.begin() + long(old), pending.begin() + long(old + size_t(k) * 2));
            dropFrames -= k;
        }
    }

    void convertFrame()
    {
        if (!setupResampler(frame))
            return;
        if (seekTarget >= 0) {
            const int64_t ts = frame->best_effort_timestamp;
            if (ts != AV_NOPTS_VALUE) {
                const int64_t start = st()->start_time != AV_NOPTS_VALUE ? st()->start_time : 0;
                const double pos = double(ts - start) * av_q2d(st()->time_base);
                if (seekTarget > pos)
                    dropFrames = (long long)std::llround((seekTarget - pos) * outRate);
            }
            seekTarget = -1;
        }
        appendConverted(const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
    }

    void pollMetadata()
    {
        // Shoutcast/Icecast "ICY" metadata of MP3/AAC streams.
        if (fmt->pb) {
            uint8_t *value = nullptr;
            if (av_opt_get(fmt->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &value) >= 0 && value) {
                const QByteArray packet(reinterpret_cast<const char *>(value));
                av_free(value);
                const int start = packet.indexOf("StreamTitle='");
                if (start >= 0) {
                    const int from = start + 13;
                    int end = packet.indexOf("';", from);
                    if (end < 0)
                        end = packet.lastIndexOf('\'');
                    const QString t = decodeText(packet.mid(from, end > from ? end - from : 0));
                    if (t != title) {
                        title = t;
                        metaChanged = true;
                    }
                }
            }
        }
        // Ogg streams update their Vorbis comments instead.
        if ((fmt->event_flags & AVFMT_EVENT_FLAG_METADATA_UPDATED)
            || (st()->event_flags & AVSTREAM_EVENT_FLAG_METADATA_UPDATED)) {
            fmt->event_flags &= ~AVFMT_EVENT_FLAG_METADATA_UPDATED;
            st()->event_flags &= ~AVSTREAM_EVENT_FLAG_METADATA_UPDATED;
            QString t = dictValue(st()->metadata, "title");
            QString a = dictValue(st()->metadata, "artist");
            if (t.isEmpty()) {
                t = dictValue(fmt->metadata, "title");
                a = dictValue(fmt->metadata, "artist");
            }
            if (!a.isEmpty() && !t.isEmpty())
                t = a + QStringLiteral(" - ") + t;
            if (!t.isEmpty() && t != title) {
                title = t;
                metaChanged = true;
            }
        }
    }

    // Decodes the next frame into 'pending'. Returns false at the end.
    bool decodeMore()
    {
        while (true) {
            int ret = avcodec_receive_frame(codec, frame);
            if (ret == 0) {
                convertFrame();
                av_frame_unref(frame);
                return true;
            }
            if (ret == AVERROR_EOF) {
                // Drain the resampler.
                if (swr)
                    appendConverted(nullptr, 0);
                eof = true;
                return pending.size() > pendingPos;
            }
            if (ret != AVERROR(EAGAIN)) {
                error = ffError(ret);
                failed = true;
                eof = true;
                return false;
            }
            if (sentFlush) {
                eof = true;
                return false;
            }
            ret = av_read_frame(fmt, pkt);
            if (ret < 0) {
                if (abort->load()) {
                    error = QObject::tr("Aborted.");
                    failed = true;
                } else if (stream || stalled) {
                    error = QObject::tr("Error: Lost connection");
                    failed = true;
                } else if (ret != AVERROR_EOF && !(fmt->pb && avio_feof(fmt->pb))) {
                    error = ffError(ret);
                    failed = true;
                }
                avcodec_send_packet(codec, nullptr);
                sentFlush = true;
                continue;
            }
            if (pkt->stream_index != streamIndex) {
                av_packet_unref(pkt);
                continue;
            }
            if (stream && (++packetCounter % 16) == 0)
                pollMetadata();
            ret = avcodec_send_packet(codec, pkt);
            av_packet_unref(pkt);
            // A damaged packet is skipped, like any player does.
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_INVALIDDATA) {
                error = ffError(ret);
                failed = true;
                eof = true;
                return false;
            }
        }
    }
};

Decoder::Decoder() : d(std::make_unique<Private>()) {}
Decoder::~Decoder() = default;

bool Decoder::isUrl(const QString &location)
{
    const int sep = location.indexOf(QLatin1String("://"));
    return sep > 0 && !location.startsWith(QLatin1String("file://"), Qt::CaseInsensitive);
}

bool Decoder::open(const QString &location, int outputRate)
{
    initFfmpeg();
    d->close();
    d->location = location;
    d->stream = isUrl(location);

    d->fmt = avformat_alloc_context();
    d->fmt->interrupt_callback.callback = &Private::interruptCallback;
    d->fmt->interrupt_callback.opaque = d.get();
    d->lastProgress = d->stream ? nowMs() : 0;

    AVDictionary *opts = nullptr;
    if (d->stream) {
        av_dict_set(&opts, "icy", "1", 0);
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "10", 0);
        av_dict_set(&opts, "rw_timeout", "15000000", 0);
        av_dict_set(&opts, "user_agent", "Billy/" BILLY_VERSION, 0);
        d->fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    }
    const QByteArray loc = d->stream ? location.toUtf8() : QFile::encodeName(location);
    int ret = avformat_open_input(&d->fmt, loc.constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        d->fmt = nullptr; // freed by avformat_open_input
        d->error = d->stream ? QObject::tr("Error opening URL: %1").arg(ffError(ret))
                             : (QFileInfo::exists(location) ? QObject::tr("Unknown file format.")
                                                            : QObject::tr("File not found."));
        return false;
    }
    if ((ret = avformat_find_stream_info(d->fmt, nullptr)) < 0) {
        d->error = ffError(ret);
        return false;
    }
    d->streamIndex = av_find_best_stream(d->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (d->streamIndex < 0) {
        d->error = QObject::tr("File is not an audio file.");
        return false;
    }
    for (unsigned i = 0; i < d->fmt->nb_streams; ++i)
        if (int(i) != d->streamIndex)
            d->fmt->streams[i]->discard = AVDISCARD_ALL; // e.g. cover art
    AVStream *st = d->st();
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        d->error = QObject::tr("Unsupported file format.");
        return false;
    }
    d->codec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(d->codec, st->codecpar);
    d->codec->pkt_timebase = st->time_base;
    if ((ret = avcodec_open2(d->codec, codec, nullptr)) < 0) {
        d->error = ffError(ret);
        return false;
    }
    d->nativeRate = d->codec->sample_rate;
    if (d->nativeRate <= 0) {
        d->error = QObject::tr("File has illegal sample rate.");
        return false;
    }
    d->outRate = outputRate > 0 ? outputRate : d->nativeRate;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->seekable = !d->stream && d->fmt->pb && (d->fmt->pb->seekable & AVIO_SEEKABLE_NORMAL);

    double seconds = -1;
    if (!d->stream) {
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
            seconds = double(st->duration) * av_q2d(st->time_base);
        else if (d->fmt->duration != AV_NOPTS_VALUE && d->fmt->duration > 0)
            seconds = double(d->fmt->duration) / AV_TIME_BASE;
    }
    d->total = seconds > 0 ? (long long)std::llround(seconds * d->outRate) : -1;
    d->produced = 0;
    // MP4/M4A edit lists give the exact length, but FFmpeg only applies the
    // start of it (encoder delay). Cutting at the length removes the padding.
    const char *demuxer = d->fmt->iformat ? d->fmt->iformat->name : "";
    d->trimToTotal = d->total > 0 && (std::strstr(demuxer, "mp4") || std::strstr(demuxer, "mov"));

    if (d->stream) {
        uint8_t *headers = nullptr;
        if (d->fmt->pb && av_opt_get(d->fmt->pb, "icy_metadata_headers", AV_OPT_SEARCH_CHILDREN, &headers) >= 0
            && headers) {
            const QList<QByteArray> lines = QByteArray(reinterpret_cast<const char *>(headers)).split('\n');
            av_free(headers);
            for (const QByteArray &line : lines)
                if (line.toLower().startsWith("icy-name:"))
                    d->name = decodeText(line.mid(9));
        }
        if (d->name.isEmpty())
            d->name = dictValue(d->fmt->metadata, "icy-name");
        d->pollMetadata();
        d->metaChanged = true;
    }
    return true;
}

long Decoder::read(float *out, long frames)
{
    if (!d->codec)
        return 0;
    if (d->trimToTotal)
        frames = long(qBound(0LL, d->total - d->produced, (long long)frames));
    long done = 0;
    while (done < frames) {
        const size_t available = (d->pending.size() - d->pendingPos) / 2;
        if (available == 0) {
            d->pending.clear();
            d->pendingPos = 0;
            if (d->eof || !d->decodeMore()) {
                if (d->pending.empty())
                    break;
            }
            continue;
        }
        const size_t n = std::min<size_t>(available, size_t(frames - done));
        std::memcpy(out + done * 2, d->pending.data() + d->pendingPos, n * 2 * sizeof(float));
        d->pendingPos += n * 2;
        done += long(n);
    }
    d->produced += done;
    if (done > 0 && d->stream)
        d->lastProgress = nowMs();
    return done;
}

bool Decoder::seek(double seconds)
{
    if (!d->codec || !d->seekable)
        return false;
    AVStream *st = d->st();
    const int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    // Start decoding a bit earlier: MP3 frames depend on the previous ones (bit
    // reservoir), so the first frames after a jump are broken. The extra audio
    // is dropped again below.
    const double preroll = 0.2;
    const int64_t ts = start + int64_t(qMax(0.0, seconds - preroll) / av_q2d(st->time_base));
    if (av_seek_frame(d->fmt, d->streamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return false;
    avcodec_flush_buffers(d->codec);
    swr_free(&d->swr);
    d->pending.clear();
    d->pendingPos = 0;
    d->eof = false;
    d->sentFlush = false;
    d->failed = false;
    d->seekTarget = seconds; // sample accurate: drop what lies before
    d->produced = (long long)std::llround(seconds * d->outRate);
    d->dropFrames = 0;
    return true;
}

int Decoder::sampleRate() const { return d->outRate; }
int Decoder::nativeRate() const { return d->nativeRate; }
long long Decoder::totalFrames() const { return d->total; }
bool Decoder::isStream() const { return d->stream; }
QString Decoder::errorString() const { return d->error; }
bool Decoder::failed() const { return d->failed; }
std::shared_ptr<std::atomic_bool> Decoder::abortFlag() const { return d->abort; }

int Decoder::bitrate() const
{
    if (!d->codec)
        return 0;
    const int64_t br = d->st()->codecpar->bit_rate > 0 ? d->st()->codecpar->bit_rate : d->fmt->bit_rate;
    return br > 0 ? int(br / 1000) : 0;
}

QString Decoder::codecName() const
{
    if (!d->codec)
        return {};
    const AVCodecDescriptor *desc = avcodec_descriptor_get(d->codec->codec_id);
    return desc ? QString::fromUtf8(desc->name).toUpper() : QString();
}

QString Decoder::formatDescription() const
{
    if (!d->codec)
        return {};
    const AVCodecDescriptor *desc = avcodec_descriptor_get(d->codec->codec_id);
    const QString codec = desc ? QString::fromUtf8(desc->name) : QString();
    // Files are named after their type (MP3, OGG, M4A, ...), streams after the codec.
    QString type = d->stream ? QString() : QFileInfo(d->location).suffix().toUpper();
    if (type.isEmpty()) {
        if (codec == QLatin1String("vorbis"))
            type = QStringLiteral("OGG");
        else if (codec.startsWith(QLatin1String("mp3")))
            type = QStringLiteral("MP3");
        else
            type = codec.toUpper();
    }
#if BILLY_CH_LAYOUT
    const int channels = d->codec->ch_layout.nb_channels;
#else
    const int channels = d->codec->channels;
#endif
    const QString layout = channels == 1   ? QStringLiteral("Mono")
                           : channels == 2 ? QStringLiteral("Stereo")
                                           : QStringLiteral("%1 Channels").arg(channels);
    const int kbps = bitrate();
    if (codec.startsWith(QLatin1String("pcm_")) || kbps <= 0)
        return QStringLiteral("%1 %2 Hz %3").arg(type).arg(d->nativeRate).arg(layout);
    return QStringLiteral("%1 %2 kbps %3").arg(type).arg(kbps).arg(layout);
}

bool Decoder::takeMetadataChange(QString &name, QString &title)
{
    if (!d->metaChanged)
        return false;
    d->metaChanged = false;
    name = d->name;
    title = d->title;
    return true;
}

namespace Audio {

const QStringList &supportedExtensions()
{
    static const QStringList exts = {
        QStringLiteral("mp3"),  QStringLiteral("mp2"),  QStringLiteral("mp1"), QStringLiteral("mpa"),
        QStringLiteral("wav"),  QStringLiteral("ogg"),  QStringLiteral("oga"), QStringLiteral("opus"),
        QStringLiteral("flac"), QStringLiteral("aif"),  QStringLiteral("aiff"), QStringLiteral("aifc"),
        QStringLiteral("m4a"),  QStringLiteral("m4b"),  QStringLiteral("aac"), QStringLiteral("ac3"),
        QStringLiteral("wma"),  QStringLiteral("ape"),  QStringLiteral("wv"),  QStringLiteral("mpc"),
        QStringLiteral("mka"),  QStringLiteral("tta"),  QStringLiteral("caf"),
    };
    return exts;
}

bool isSupported(const QString &path)
{
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot < 0)
        return false;
    return supportedExtensions().contains(path.mid(dot + 1).toLower());
}

QString fileDialogFilter()
{
    QStringList all;
    for (const QString &ext : supportedExtensions())
        all << QStringLiteral("*.") + ext;
    return QObject::tr("All supported music files (%1)").arg(all.join(QLatin1Char(' ')))
        + QStringLiteral(";;") + QObject::tr("MPEG files (*.mp3 *.mp2 *.mp1)")
        + QStringLiteral(";;") + QObject::tr("WAVE files (*.wav)")
        + QStringLiteral(";;") + QObject::tr("OGG Vorbis / Opus files (*.ogg *.oga *.opus)")
        + QStringLiteral(";;") + QObject::tr("FLAC Lossless files (*.flac)")
        + QStringLiteral(";;") + QObject::tr("AAC / ALAC files (*.m4a *.m4b *.aac)")
        + QStringLiteral(";;") + QObject::tr("All files (*)");
}

AudioInfo probe(const QString &path, bool withTags)
{
    initFfmpeg();
    AudioInfo info;
    if (!QFileInfo::exists(path)) {
        info.error = QObject::tr("File not found.");
        return info;
    }
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) {
        info.error = QObject::tr("Unknown file format.");
        return info;
    }
    // The header is enough for the length of most files; analyse only when needed.
    if (withTags || fmt->duration == AV_NOPTS_VALUE)
        avformat_find_stream_info(fmt, nullptr);
    const int index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        info.error = QObject::tr("File is not an audio file.");
        avformat_close_input(&fmt);
        return info;
    }
    const AVStream *st = fmt->streams[index];
    const AVCodecParameters *par = st->codecpar;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
    info.format = desc && desc->long_name ? QString::fromUtf8(desc->long_name) : QObject::tr("Unknown format");
    if (fmt->iformat && fmt->iformat->long_name)
        info.format += QStringLiteral(", ") + QString::fromUtf8(fmt->iformat->long_name);
    info.sampleRate = par->sample_rate;
#if BILLY_CH_LAYOUT
    info.channels = par->ch_layout.nb_channels;
#else
    info.channels = par->channels;
#endif
    const int64_t br = par->bit_rate > 0 ? par->bit_rate : fmt->bit_rate;
    info.bitrate = br > 0 ? int(br / 1000) : 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        info.seconds = double(st->duration) * av_q2d(st->time_base);
    else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
        info.seconds = double(fmt->duration) / AV_TIME_BASE;

    if (withTags) {
        static const struct { const char *key; const char *label; } fields[] = {
            {"title", "Title"},       {"artist", "Artist"},     {"album_artist", "Album artist"},
            {"album", "Album"},       {"track", "Track"},       {"disc", "Disc"},
            {"date", "Date"},         {"genre", "Genre"},       {"composer", "Composer"},
            {"encoder", "Encoder"},   {"comment", "Comment"},   {"copyright", "Copyright"},
            {"lyrics", "Lyrics"},
        };
        for (const auto &f : fields) {
            QString v = dictValue(fmt->metadata, f.key);
            if (v.isEmpty())
                v = dictValue(st->metadata, f.key);
            if (!v.isEmpty())
                info.tags << QStringLiteral("%1: %2").arg(QLatin1String(f.label), v);
        }
    }
    avformat_close_input(&fmt);
    info.ok = true;
    return info;
}

QString formatTime(double seconds, bool tenths)
{
    if (seconds < 0)
        seconds = 0;
    const qint64 total = qint64(seconds);
    const qint64 h = total / 3600, m = (total / 60) % 60, s = total % 60;
    QString text = h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                         : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
    if (tenths)
        text += QStringLiteral(".%1").arg(int(std::fmod(seconds, 1.0) * 10));
    return text;
}

} // namespace Audio
