#pragma once

#include <QString>
#include <QStringList>
#include <memory>

// Decodes an audio file to interleaved 32-bit float samples.
class Decoder
{
public:
    virtual ~Decoder() = default;

    virtual bool open(const QString &path) = 0;
    // Returns the number of frames written to 'out' (capacity 'frames').
    // 0 means end of stream (or error).
    virtual long read(float *out, long frames) = 0;
    virtual bool seek(long long frame) = 0;

    int sampleRate() const { return m_rate; }
    int channels() const { return m_channels; }
    long long totalFrames() const { return m_totalFrames; }
    QString errorString() const { return m_error; }

    // Creates a suitable decoder for 'path' (based on the extension).
    static std::unique_ptr<Decoder> create(const QString &path);

protected:
    int m_rate = 0;
    int m_channels = 0;
    long long m_totalFrames = -1;
    QString m_error;
};

// Describes a file without decoding it (used by "file info" and "total time").
struct AudioInfo
{
    bool ok = false;
    QString error;
    QString format;      // e.g. "MPEG 1 Layer III"
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

// 'withTags' also collects embedded tag text. 'fullScan' makes MP3 lengths
// exact (slower for VBR files without a Xing/LAME header).
AudioInfo probe(const QString &path, bool withTags, bool fullScan = false);

QString formatTime(double seconds, bool tenths = false);

} // namespace Audio
