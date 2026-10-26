#include "audio/AudioEngine.h"
#include "audio/Decoder.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QLoggingCategory>
#include <QMediaDevices>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>

Q_LOGGING_CATEGORY(lcEngine, "billy.engine", QtInfoMsg)

namespace {
constexpr long kBlockFrames = 4096;
constexpr int kSinkBufferMs = 200;
constexpr int kPollMs = 20;
} // namespace

// QIODevice that the QAudioSink pulls PCM data from.
class PcmSource : public QIODevice
{
public:
    explicit PcmSource(AudioEngine *engine) : m_engine(engine) {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return (1 << 20) + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char *data, qint64 maxSize) override { return m_engine->render(data, maxSize); }
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    AudioEngine *m_engine;
};

// Owns the QAudioSink. Lives in the engine's output thread.
class SinkWorker : public QObject
{
public:
    explicit SinkWorker(AudioEngine *engine) : m_engine(engine)
    {
        // Parented, so that it moves to the output thread together with the worker.
        m_latencyTimer = new QTimer(this);
        m_latencyTimer->setInterval(kPollMs);
        connect(m_latencyTimer, &QTimer::timeout, this, &SinkWorker::updateLatency);
    }

    ~SinkWorker() override { close(); }

    // Returns an error message, empty on success.
    QString open(const QByteArray &deviceId, int rate, bool paused)
    {
        close();
        QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (!deviceId.isEmpty()) {
            for (const QAudioDevice &d : QMediaDevices::audioOutputs()) {
                if (d.id() == deviceId) {
                    device = d;
                    break;
                }
            }
        }
        if (device.isNull())
            return QObject::tr("Can't open sound device.");

        QAudioFormat format;
        format.setSampleRate(rate);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Float);
        if (!device.isFormatSupported(format))
            format.setSampleFormat(QAudioFormat::Int16);

        {
            std::lock_guard<std::mutex> lock(m_engine->m_mutex);
            m_engine->m_sinkRate = rate;
            m_engine->m_sinkFloat = format.sampleFormat() == QAudioFormat::Float;
            m_engine->m_wantRate = 0;
        }
        m_sink = new QAudioSink(device, format, this);
        m_sink->setBufferSize(format.bytesForDuration(kSinkBufferMs * 1000));
        m_source = new PcmSource(m_engine);
        m_source->open(QIODevice::ReadOnly);
        m_sink->start(m_source);
        qCDebug(lcEngine) << "output opened" << rate << "Hz" << format.sampleFormat() << device.description()
                          << "buffer" << m_sink->bufferSize();
        if (m_sink->error() == QAudio::OpenError) {
            close();
            return QObject::tr("Can't open sound device.");
        }
        if (paused)
            m_sink->suspend();
        m_latencyTimer->start();
        return {};
    }

    void close()
    {
        m_latencyTimer->stop();
        if (m_sink) {
            m_sink->stop();
            delete m_sink;
            m_sink = nullptr;
        }
        delete m_source;
        m_source = nullptr;
        m_engine->m_latencyFrames = 0;
        std::lock_guard<std::mutex> lock(m_engine->m_mutex);
        m_engine->m_sinkRate = 0;
    }

    void suspend()
    {
        if (m_sink)
            m_sink->suspend();
    }

    void resume()
    {
        if (m_sink)
            m_sink->resume();
    }

private:
    void updateLatency()
    {
        if (!m_sink)
            return;
        const QAudioFormat f = m_sink->format();
        const qint64 buffered = qMax<qint64>(0, m_sink->bufferSize() - m_sink->bytesFree());
        m_engine->m_latencyFrames = f.bytesPerFrame() > 0 ? buffered / f.bytesPerFrame() : 0;
    }

    AudioEngine *m_engine;
    QAudioSink *m_sink = nullptr;
    PcmSource *m_source = nullptr;
    QTimer *m_latencyTimer;
};

AudioEngine::AudioEngine(QObject *parent)
    : QObject(parent)
{
    m_outputThread = new QThread(this);
    m_outputThread->setObjectName(QStringLiteral("billy-output"));
    m_worker = new SinkWorker(this);
    m_worker->moveToThread(m_outputThread);
    connect(m_outputThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_outputThread->start(QThread::TimeCriticalPriority);

    m_thread = std::thread([this] { decoderLoop(); });

    m_pollTimer.setInterval(kPollMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &AudioEngine::poll);
}

AudioEngine::~AudioEngine()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_cond.notify_all();
    m_thread.join();
    closeSink();
    m_outputThread->quit();
    m_outputThread->wait();
}

void AudioEngine::play(const QString &path, quint64 id, double startSeconds)
{
    m_paths.insert(id, path);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cmdOpen = true;
        m_cmdStop = false;
        m_cmdPath = path;
        m_cmdId = id;
        m_cmdSeconds = startSeconds;
        m_cmdSeq = ++m_seqCounter;
        flushLocked();
        // Report the new track right away instead of after the first buffer.
        m_playingId = id;
        m_playingSeq = m_cmdSeq;
        m_playingFrame = 0;
        m_playingTotal = -1;
        m_currentSeq = m_cmdSeq;
    }
    m_cond.notify_all();
    m_currentId = id;
    m_drainTicks = 0;
    if (m_state == Paused)
        resume();
    setState(Playing);
    m_pollTimer.start();
}

void AudioEngine::setNext(quint64 afterId, const QString &path, quint64 id)
{
    if (id)
        m_paths.insert(id, path);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (afterId != m_decodingId)
            return;
        m_nextAfterId = afterId;
        m_nextId = id;
        m_nextPath = path;
        if (id)
            m_endOfStream = false;
        else if (m_decoderEof)
            m_endOfStream = true;
    }
    m_cond.notify_all();
}

void AudioEngine::pause()
{
    if (m_state != Playing)
        return;
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->suspend(); }, Qt::QueuedConnection);
    setState(Paused);
}

void AudioEngine::resume()
{
    if (m_state != Paused)
        return;
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->resume(); }, Qt::QueuedConnection);
    setState(Playing);
}

void AudioEngine::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cmdOpen = false;
        m_cmdStop = true;
        flushLocked();
        m_playingId = 0;
        m_playingSeq = 0;
        m_playingFrame = 0;
        m_playingTotal = -1;
    }
    m_cond.notify_all();
    m_pollTimer.stop();
    closeSink();
    m_currentId = 0;
    m_currentSeq = 0;
    setState(Stopped);
}

void AudioEngine::seek(double seconds)
{
    if (m_state == Stopped || !m_currentId)
        return;
    const QString path = m_paths.value(m_currentId);
    if (path.isEmpty())
        return;
    const State before = m_state;
    play(path, m_currentId, qMax(0.0, seconds));
    if (before == Paused)
        pause();
}

void AudioEngine::setVolume(int percent)
{
    m_volumePercent = qBound(0, percent, 100);
    // A perceptual (squared) curve, so that 50 % sounds roughly half as loud.
    const float v = m_volumePercent / 100.0f;
    m_gain = v * v;
}

void AudioEngine::setOutputDevice(const QByteArray &deviceId)
{
    if (deviceId == m_deviceId)
        return;
    m_deviceId = deviceId;
    if (m_requestedRate) {
        const int rate = m_requestedRate;
        closeSink();
        requestSink(rate);
    }
}

QString AudioEngine::outputDescription() const
{
    if (!m_deviceId.isEmpty()) {
        for (const QAudioDevice &d : QMediaDevices::audioOutputs())
            if (d.id() == m_deviceId)
                return d.description();
    }
    return QMediaDevices::defaultAudioOutput().description();
}

void AudioEngine::setBufferLength(int ms)
{
    m_bufferMs = qBound(500, ms, 10000);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bufferMsShared = m_bufferMs;
}

double AudioEngine::position() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playingRate <= 0)
        return 0;
    const long long frame = std::max(0LL, m_playingFrame - m_latencyFrames.load());
    return double(frame) / m_playingRate;
}

double AudioEngine::duration() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playingRate <= 0 || m_playingTotal < 0)
        return -1;
    return double(m_playingTotal) / m_playingRate;
}

void AudioEngine::takeLevels(float &left, float &right)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    left = m_peakL;
    right = m_peakR;
    m_peakL = m_peakR = 0;
}

void AudioEngine::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void AudioEngine::flushLocked()
{
    m_chunks.clear();
    m_queuedFrames = 0;
    ++m_generation;
    m_drained = false;
    m_wantRate = 0;
    m_peakL = m_peakR = 0;
}

void AudioEngine::requestSink(int rate)
{
    m_requestedRate = rate;
    const QByteArray device = m_deviceId;
    const bool paused = m_state == Paused;
    QMetaObject::invokeMethod(
        m_worker,
        [this, device, rate, paused] {
            const QString err = m_worker->open(device, rate, paused);
            if (!err.isEmpty()) {
                QMetaObject::invokeMethod(
                    this,
                    [this, err] {
                        emit error(m_currentId, err);
                        stop();
                    },
                    Qt::QueuedConnection);
            }
        },
        Qt::QueuedConnection);
}

void AudioEngine::closeSink()
{
    if (!m_requestedRate)
        return;
    m_requestedRate = 0;
    // Blocking: the sink must be gone before e.g. a new one is opened.
    QMetaObject::invokeMethod(m_worker, [w = m_worker] { w->close(); }, Qt::BlockingQueuedConnection);
}

void AudioEngine::poll()
{
    quint64 seq, id;
    int wantRate, frontRate = 0;
    bool drained;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        seq = m_playingSeq;
        id = m_playingId;
        wantRate = m_wantRate;
        drained = m_drained;
        if (!m_chunks.empty())
            frontRate = m_chunks.front().rate;
    }

    if (seq && seq != m_currentSeq) {
        qCDebug(lcEngine) << "track started (gapless)" << id;
        m_currentSeq = seq;
        m_currentId = id;
        emit trackStarted(id);
    }

    if (m_state == Stopped)
        return;

    // (Re)open the output when needed.
    int rate = wantRate;
    if (!m_requestedRate && frontRate)
        rate = frontRate;
    if (rate && rate != m_requestedRate) {
        qCDebug(lcEngine) << "output rate change" << m_requestedRate << "->" << rate;
        closeSink();
        requestSink(rate);
    }

    if (drained && m_state == Playing) {
        // Give the output device time to play what it still has buffered.
        if (++m_drainTicks * kPollMs >= kSinkBufferMs + 100) {
            qCDebug(lcEngine) << "finished";
            stop();
            emit finished();
        }
    } else {
        m_drainTicks = 0;
    }
}

qint64 AudioEngine::render(char *data, qint64 maxBytes)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    const int bytesPerSample = m_sinkFloat ? 4 : 2;
    const qint64 frames = maxBytes / (2 * bytesPerSample);
    if (frames <= 0)
        return 0;
    const float gain = m_gain.load();
    float peakL = m_peakL, peakR = m_peakR;
    qint64 produced = 0;
    float *outF = reinterpret_cast<float *>(data);
    qint16 *outI = reinterpret_cast<qint16 *>(data);

    while (produced < frames && !m_chunks.empty()) {
        Chunk &c = m_chunks.front();
        if (c.rate != m_sinkRate) {
            m_wantRate = c.rate;
            break;
        }
        const size_t available = c.samples.size() / 2 - c.offset;
        const size_t n = std::min<size_t>(available, size_t(frames - produced));
        const float *in = c.samples.data() + c.offset * 2;
        for (size_t i = 0; i < n * 2; i += 2) {
            const float l = in[i] * gain, r = in[i + 1] * gain;
            peakL = std::max(peakL, std::fabs(l));
            peakR = std::max(peakR, std::fabs(r));
            const qint64 o = (produced * 2) + qint64(i);
            if (m_sinkFloat) {
                outF[o] = l;
                outF[o + 1] = r;
            } else {
                outI[o] = qint16(std::lrint(std::clamp(l, -1.0f, 1.0f) * 32767.0f));
                outI[o + 1] = qint16(std::lrint(std::clamp(r, -1.0f, 1.0f) * 32767.0f));
            }
        }
        c.offset += n;
        produced += qint64(n);
        m_playingId = c.trackId;
        m_playingSeq = c.seq;
        m_playingFrame = c.trackFrame + (long long)c.offset;
        m_playingTotal = c.totalFrames;
        m_playingRate = c.rate;
        if (c.offset * 2 >= c.samples.size()) {
            m_queuedFrames -= (long long)(c.samples.size() / 2);
            m_chunks.pop_front();
        }
    }
    m_peakL = peakL;
    m_peakR = peakR;
    if (m_chunks.empty() && m_endOfStream && !m_cmdOpen)
        m_drained = true;
    lock.unlock();
    m_cond.notify_all();

    // Never starve the device: an idle sink would stop pulling.
    if (produced < frames)
        std::memset(data + produced * 2 * bytesPerSample, 0, size_t((frames - produced) * 2 * bytesPerSample));
    return frames * 2 * bytesPerSample;
}

void AudioEngine::decoderLoop()
{
    std::vector<float> raw;
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_quit) {
        if (m_cmdStop) {
            m_cmdStop = false;
            auto old = std::move(m_decoder);
            m_decodingId = 0;
            m_decoderEof = true;
            m_endOfStream = true;
            m_nextId = 0;
            lock.unlock();
            old.reset();
            lock.lock();
            continue;
        }

        // Opening a track: either explicitly requested or the gapless follow-up.
        QString openPath;
        quint64 openId = 0, openSeq = 0;
        double openSeconds = 0;
        bool explicitOpen = false;
        if (m_cmdOpen) {
            m_cmdOpen = false;
            explicitOpen = true;
            openPath = m_cmdPath;
            openId = m_cmdId;
            openSeq = m_cmdSeq;
            openSeconds = m_cmdSeconds;
        } else if (m_decoderEof && m_decodingId && m_nextId && m_nextAfterId == m_decodingId) {
            openPath = m_nextPath;
            openId = m_nextId;
            openSeq = ++m_seqCounter;
            m_nextId = 0;
        }
        if (openId) {
            const unsigned gen = m_generation;
            m_decodingId = openId;
            m_decodingSeq = openSeq;
            m_decoderEof = true;
            m_endOfStream = false;
            auto old = std::move(m_decoder);
            lock.unlock();
            old.reset();
            std::unique_ptr<Decoder> dec = Decoder::create(openPath);
            QString err;
            if (!dec->open(openPath)) {
                err = dec->errorString();
                if (err.isEmpty())
                    err = tr("File can't be opened.");
                dec.reset();
            } else if (openSeconds > 0) {
                dec->seek((long long)(openSeconds * dec->sampleRate()));
            }
            lock.lock();
            if (gen != m_generation || m_cmdOpen || m_cmdStop || m_quit)
                continue; // superseded while opening
            m_decoder = std::move(dec);
            m_decoderEof = !m_decoder;
            m_decodeFrame = m_decoder ? (long long)(openSeconds * m_decoder->sampleRate()) : 0;
            if (m_decoder && explicitOpen) {
                m_playingRate = m_decoder->sampleRate();
                m_playingTotal = m_decoder->totalFrames();
                m_playingFrame = m_decodeFrame;
            }
            qCDebug(lcEngine) << "decoding" << openId << openPath << (explicitOpen ? "(explicit)" : "(gapless)") << err;
            if (!err.isEmpty())
                QMetaObject::invokeMethod(this, [this, openId, err] { emit error(openId, err); }, Qt::QueuedConnection);
            QMetaObject::invokeMethod(this, [this, openId] { emit decodingStarted(openId); }, Qt::QueuedConnection);
            continue;
        }

        const int rate = m_decoder ? m_decoder->sampleRate() : 44100;
        const long long maxFrames = (long long)m_bufferMsShared * rate / 1000;
        if (m_decoder && !m_decoderEof && m_queuedFrames < maxFrames) {
            Decoder *dec = m_decoder.get();
            const unsigned gen = m_generation;
            const int ch = dec->channels();
            lock.unlock();
            raw.resize(size_t(kBlockFrames) * size_t(ch));
            const long n = dec->read(raw.data(), kBlockFrames);
            Chunk chunk;
            if (n > 0) {
                chunk.samples.resize(size_t(n) * 2);
                float *out = chunk.samples.data();
                for (long i = 0; i < n; ++i) {
                    const float *f = raw.data() + size_t(i) * size_t(ch);
                    float l, r;
                    if (ch == 1) {
                        l = r = f[0];
                    } else if (ch == 2) {
                        l = f[0];
                        r = f[1];
                    } else {
                        // Simple downmix: front channels plus centre/rear at -3 dB.
                        const float c = f[2] * 0.7071f;
                        l = f[0] + c + (ch > 4 ? f[4] * 0.7071f : 0.f);
                        r = f[1] + c + (ch > 5 ? f[5] * 0.7071f : 0.f);
                    }
                    out[i * 2] = l;
                    out[i * 2 + 1] = r;
                }
            }
            lock.lock();
            if (gen != m_generation || m_decoder.get() != dec)
                continue;
            if (n <= 0) {
                m_decoderEof = true;
                if (!(m_nextId && m_nextAfterId == m_decodingId))
                    m_endOfStream = true;
                continue;
            }
            chunk.trackId = m_decodingId;
            chunk.seq = m_decodingSeq;
            chunk.rate = rate;
            chunk.trackFrame = m_decodeFrame;
            chunk.totalFrames = dec->totalFrames();
            m_decodeFrame += n;
            m_queuedFrames += n;
            m_drained = false;
            m_chunks.push_back(std::move(chunk));
            continue;
        }
        m_cond.wait(lock);
    }
}
