#include "audio/Decoder.h"

#include <QFile>
#include <QObject>
#include <QFileInfo>
#include <QStringDecoder>

#include <mpg123.h>
#include <sndfile.h>

#include <cmath>
#include <mutex>

namespace {

void initMpg123()
{
    static std::once_flag once;
    std::call_once(once, [] {
#if MPG123_API_VERSION < 46
        mpg123_init();
#endif
    });
}

QString extensionOf(const QString &path)
{
    return QFileInfo(path).suffix().toLower();
}

bool isMpeg(const QString &path)
{
    const QString ext = extensionOf(path);
    return ext == QLatin1String("mp3") || ext == QLatin1String("mp2") || ext == QLatin1String("mp1");
}

// ---------------------------------------------------------------- mpg123

class Mpg123Decoder : public Decoder
{
public:
    ~Mpg123Decoder() override { close(); }

    bool open(const QString &path) override
    {
        initMpg123();
        int err = MPG123_OK;
        m_handle = mpg123_new(nullptr, &err);
        if (!m_handle) {
            m_error = QString::fromUtf8(mpg123_plain_strerror(err));
            return false;
        }
        // Gapless decoding (LAME/Xing encoder delay + padding removal) is what
        // makes album playback seamless.
        mpg123_param(m_handle, MPG123_FLAGS, MPG123_GAPLESS | MPG123_QUIET, 0.);
        mpg123_param(m_handle, MPG123_ADD_FLAGS, MPG123_FORCE_FLOAT, 0.);
        mpg123_format_none(m_handle);
        const long *rates = nullptr;
        size_t count = 0;
        mpg123_rates(&rates, &count);
        for (size_t i = 0; i < count; ++i)
            mpg123_format(m_handle, rates[i], MPG123_MONO | MPG123_STEREO, MPG123_ENC_FLOAT_32);

        if (mpg123_open(m_handle, QFile::encodeName(path).constData()) != MPG123_OK) {
            m_error = QString::fromUtf8(mpg123_strerror(m_handle));
            close();
            return false;
        }
        long rate = 0;
        int channels = 0, encoding = 0;
        if (mpg123_getformat(m_handle, &rate, &channels, &encoding) != MPG123_OK) {
            m_error = QString::fromUtf8(mpg123_strerror(m_handle));
            close();
            return false;
        }
        // Lock the output format so that a stray frame does not change it.
        mpg123_format_none(m_handle);
        mpg123_format(m_handle, rate, channels, encoding);
        m_rate = int(rate);
        m_channels = channels;
        const off_t len = mpg123_length(m_handle);
        m_totalFrames = len > 0 ? (long long)len : -1;
        return true;
    }

    long read(float *out, long frames) override
    {
        if (!m_handle)
            return 0;
        size_t done = 0;
        const size_t bytes = size_t(frames) * size_t(m_channels) * sizeof(float);
        int ret = mpg123_read(m_handle, reinterpret_cast<unsigned char *>(out), bytes, &done);
        while (ret == MPG123_NEW_FORMAT && done == 0)
            ret = mpg123_read(m_handle, reinterpret_cast<unsigned char *>(out), bytes, &done);
        if (ret != MPG123_OK && ret != MPG123_DONE && ret != MPG123_NEW_FORMAT && done == 0)
            return 0;
        return long(done / (size_t(m_channels) * sizeof(float)));
    }

    bool seek(long long frame) override
    {
        return m_handle && mpg123_seek(m_handle, off_t(frame), SEEK_SET) >= 0;
    }

private:
    void close()
    {
        if (m_handle) {
            mpg123_close(m_handle);
            mpg123_delete(m_handle);
            m_handle = nullptr;
        }
    }

    mpg123_handle *m_handle = nullptr;
};

// -------------------------------------------------------------- libsndfile

class SndfileDecoder : public Decoder
{
public:
    ~SndfileDecoder() override
    {
        if (m_file)
            sf_close(m_file);
    }

    bool open(const QString &path) override
    {
        SF_INFO info{};
        m_file = sf_open(QFile::encodeName(path).constData(), SFM_READ, &info);
        if (!m_file) {
            m_error = QString::fromUtf8(sf_strerror(nullptr));
            return false;
        }
        // Integer files are scaled to [-1, 1].
        sf_command(m_file, SFC_SET_NORM_FLOAT, nullptr, SF_TRUE);
        m_rate = info.samplerate;
        m_channels = info.channels;
        m_totalFrames = info.frames > 0 ? (long long)info.frames : -1;
        m_seekable = info.seekable;
        return m_rate > 0 && m_channels > 0;
    }

    long read(float *out, long frames) override
    {
        return m_file ? long(sf_readf_float(m_file, out, frames)) : 0;
    }

    bool seek(long long frame) override
    {
        return m_file && m_seekable && sf_seek(m_file, sf_count_t(frame), SEEK_SET) >= 0;
    }

private:
    SNDFILE *m_file = nullptr;
    bool m_seekable = false;
};

QString latin1OrUtf8(const char *s)
{
    if (!s)
        return {};
    QByteArray raw(s);
    auto decoder = QStringDecoder(QStringDecoder::Utf8);
    QString text = decoder(raw);
    if (decoder.hasError())
        text = QString::fromLatin1(raw);
    return text.trimmed();
}

QString fromMpgString(const mpg123_string *s)
{
    if (!s || !s->p || s->fill == 0)
        return {};
    return QString::fromUtf8(s->p, int(s->fill) - 1).trimmed();
}

QString fromFixed(const char *s, int len)
{
    QByteArray raw(s, len);
    const int nul = raw.indexOf('\0');
    if (nul >= 0)
        raw.truncate(nul);
    return QString::fromLatin1(raw).trimmed();
}

void addTag(QStringList &tags, const char *label, const QString &value)
{
    if (!value.isEmpty())
        tags << QStringLiteral("%1: %2").arg(QLatin1String(label), value);
}

AudioInfo probeMpeg(const QString &path, bool withTags, bool fullScan)
{
    AudioInfo info;
    initMpg123();
    int err = MPG123_OK;
    mpg123_handle *h = mpg123_new(nullptr, &err);
    if (!h) {
        info.error = QString::fromUtf8(mpg123_plain_strerror(err));
        return info;
    }
    mpg123_param(h, MPG123_FLAGS, MPG123_GAPLESS | MPG123_QUIET, 0.);
    if (mpg123_open(h, QFile::encodeName(path).constData()) != MPG123_OK) {
        info.error = QString::fromUtf8(mpg123_strerror(h));
        mpg123_delete(h);
        return info;
    }
    long rate = 0;
    int channels = 0, enc = 0;
    if (mpg123_getformat(h, &rate, &channels, &enc) != MPG123_OK) {
        info.error = QObject::tr("File is not an audio file.");
        mpg123_close(h);
        mpg123_delete(h);
        return info;
    }
    if (fullScan)
        mpg123_scan(h);
    mpg123_frameinfo fi{};
    mpg123_info(h, &fi);
    static const char *versions[] = {"1", "2", "2.5"};
    info.format = QStringLiteral("MPEG %1 Layer %2")
                      .arg(QLatin1String(versions[qBound(0, int(fi.version), 2)]))
                      .arg(QString(QLatin1Char('I')).repeated(qBound(1, fi.layer, 3)));
    if (fi.vbr != MPG123_CBR)
        info.format += QStringLiteral(" VBR");
    info.sampleRate = int(rate);
    info.channels = channels;
    info.bitrate = fi.vbr == MPG123_ABR ? fi.abr_rate : fi.bitrate;
    const off_t len = mpg123_length(h);
    if (len > 0 && rate > 0)
        info.seconds = double(len) / double(rate);
    if (info.seconds > 0 && fi.vbr != MPG123_CBR) {
        const qint64 bytes = QFileInfo(path).size();
        info.bitrate = int(bytes * 8 / info.seconds / 1000.0);
    }

    if (withTags) {
        // Reading a little forces the parser past ID3v2 at the start of the file.
        unsigned char buf[4096];
        size_t done = 0;
        mpg123_read(h, buf, sizeof buf, &done);
        mpg123_id3v1 *v1 = nullptr;
        mpg123_id3v2 *v2 = nullptr;
        if (mpg123_id3(h, &v1, &v2) == MPG123_OK) {
            if (v2) {
                info.tags << QStringLiteral("ID3v2.%1").arg(v2->version);
                addTag(info.tags, "Title", fromMpgString(v2->title));
                addTag(info.tags, "Artist", fromMpgString(v2->artist));
                addTag(info.tags, "Album", fromMpgString(v2->album));
                addTag(info.tags, "Year", fromMpgString(v2->year));
                addTag(info.tags, "Genre", fromMpgString(v2->genre));
                for (size_t i = 0; i < v2->texts; ++i) {
                    const mpg123_text &t = v2->text[i];
                    const QByteArray id(t.id, 4);
                    if (id == "TRCK")
                        addTag(info.tags, "Track", fromMpgString(&t.text));
                    else if (id == "TCOM")
                        addTag(info.tags, "Composer", fromMpgString(&t.text));
                    else if (id == "TENC" || id == "TSSE")
                        addTag(info.tags, "Encoder", fromMpgString(&t.text));
                }
                for (size_t i = 0; i < v2->comments; ++i)
                    addTag(info.tags, "Comment", fromMpgString(&v2->comment_list[i].text));
            }
            if (v1) {
                if (!info.tags.isEmpty())
                    info.tags << QString();
                info.tags << QStringLiteral("ID3v1");
                addTag(info.tags, "Title", fromFixed(v1->title, 30));
                addTag(info.tags, "Artist", fromFixed(v1->artist, 30));
                addTag(info.tags, "Album", fromFixed(v1->album, 30));
                addTag(info.tags, "Year", fromFixed(v1->year, 4));
                addTag(info.tags, "Comment", fromFixed(v1->comment, 30));
            }
        }
    }
    mpg123_close(h);
    mpg123_delete(h);
    info.ok = true;
    return info;
}

AudioInfo probeSndfile(const QString &path, bool withTags)
{
    AudioInfo info;
    SF_INFO sfi{};
    SNDFILE *f = sf_open(QFile::encodeName(path).constData(), SFM_READ, &sfi);
    if (!f) {
        info.error = QString::fromUtf8(sf_strerror(nullptr));
        return info;
    }
    SF_FORMAT_INFO major{};
    major.format = sfi.format & SF_FORMAT_TYPEMASK;
    sf_command(nullptr, SFC_GET_FORMAT_INFO, &major, sizeof major);
    SF_FORMAT_INFO sub{};
    sub.format = sfi.format & SF_FORMAT_SUBMASK;
    sf_command(nullptr, SFC_GET_FORMAT_INFO, &sub, sizeof sub);
    info.format = QString::fromUtf8(major.name ? major.name : "Unknown format");
    if (sub.name)
        info.format += QStringLiteral(" (%1)").arg(QString::fromUtf8(sub.name));
    info.sampleRate = sfi.samplerate;
    info.channels = sfi.channels;
    if (sfi.frames > 0 && sfi.samplerate > 0) {
        info.seconds = double(sfi.frames) / sfi.samplerate;
        info.bitrate = int(QFileInfo(path).size() * 8 / info.seconds / 1000.0);
    }
    if (withTags) {
        static const struct { int id; const char *label; } fields[] = {
            {SF_STR_TITLE, "Title"},     {SF_STR_ARTIST, "Artist"},   {SF_STR_ALBUM, "Album"},
            {SF_STR_TRACKNUMBER, "Track"}, {SF_STR_DATE, "Date"},     {SF_STR_GENRE, "Genre"},
            {SF_STR_COMMENT, "Comment"}, {SF_STR_SOFTWARE, "Encoder"}, {SF_STR_COPYRIGHT, "Copyright"},
        };
        for (const auto &field : fields)
            addTag(info.tags, field.label, latin1OrUtf8(sf_get_string(f, field.id)));
    }
    sf_close(f);
    info.ok = true;
    return info;
}

} // namespace

std::unique_ptr<Decoder> Decoder::create(const QString &path)
{
    if (isMpeg(path))
        return std::make_unique<Mpg123Decoder>();
    return std::make_unique<SndfileDecoder>();
}

namespace Audio {

const QStringList &supportedExtensions()
{
    static const QStringList exts = {
        QStringLiteral("mp3"), QStringLiteral("mp2"),  QStringLiteral("mp1"),
        QStringLiteral("wav"), QStringLiteral("ogg"),  QStringLiteral("oga"),
        QStringLiteral("flac"), QStringLiteral("opus"), QStringLiteral("aif"),
        QStringLiteral("aiff"),
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
        + QStringLiteral(";;") + QObject::tr("WAVE files (*.wav)")
        + QStringLiteral(";;") + QObject::tr("MPEG files (*.mp3 *.mp2 *.mp1)")
        + QStringLiteral(";;") + QObject::tr("OGG Vorbis / Opus files (*.ogg *.oga *.opus)")
        + QStringLiteral(";;") + QObject::tr("FLAC Lossless files (*.flac)")
        + QStringLiteral(";;") + QObject::tr("All files (*)");
}

AudioInfo probe(const QString &path, bool withTags, bool fullScan)
{
    if (!QFileInfo::exists(path)) {
        AudioInfo info;
        info.error = QObject::tr("File not found.");
        return info;
    }
    return isMpeg(path) ? probeMpeg(path, withTags, fullScan) : probeSndfile(path, withTags);
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
