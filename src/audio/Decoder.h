#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

// Decodes a file or an internet stream with FFmpeg to interleaved stereo
// 32-bit float samples at a chosen sample rate.
//
// FFmpeg removes encoder delay and padding (LAME/Xing headers, Ogg pre-skip,
// MP4 edit lists), which is what makes consecutive tracks play gapless.
class Decoder
{
public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder &) = delete;
    Decoder &operator=(const Decoder &) = delete;

    // 'outputRate' 0 keeps the native sample rate of the source.
    bool open(const QString &location, int outputRate = 0);
    // Returns the number of stereo frames written to 'out' (capacity 'frames').
    // 0 means end of stream or error, see errorString().
    long read(float *out, long frames);
    bool seek(double seconds);

    int sampleRate() const;    // output rate
    int nativeRate() const;
    long long totalFrames() const; // at the output rate, -1 if unknown
    bool isStream() const;
    int bitrate() const;       // kbps, 0 if unknown
    QString codecName() const;
    // Status bar text like the original: "MP3 192 kbps Stereo", "WAV 44100 Hz Stereo".
    QString formatDescription() const;
    QString errorString() const;
    bool failed() const;       // read() ended because of an error, not EOF

    // Internet streams: station name and the current title (ICY or Ogg
    // metadata). Returns true once per change.
    bool takeMetadataChange(QString &name, QString &title);

    // Thread safe: setting it aborts blocking network I/O.
    std::shared_ptr<std::atomic_bool> abortFlag() const;

    static bool isUrl(const QString &location);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

// Describes a file without decoding it (used by "file info" and "total time").
struct AudioInfo
{
    bool ok = false;
    QString error;
    QString format;      // e.g. "MP3 (MPEG audio layer 3)"
    int sampleRate = 0;
    int channels = 0;
    int bitrate = 0;     // kbps, 0 when unknown
    double seconds = -1; // -1 when unknown
    QStringList tags;    // "Title: ...", "Artist: ..." lines
};

namespace Audio {

// Lower case extensions without dot.
const QStringList &supportedExtensions();
bool isSupported(const QString &path);
QString fileDialogFilter();

AudioInfo probe(const QString &path, bool withTags);

QString formatTime(double seconds, bool tenths = false);

} // namespace Audio
