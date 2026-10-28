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
constexpr int kSinkBufferMs = 400;
constexpr int kPollMs = 20;
constexpr int kStreamReserveMs = 1500;
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
        if (m_abort)
            *m_abort = true;
    }
    m_cond.notify_all();
    m_thread.join();
    closeSink();
    m_outputThread->quit();
    m_outputThread->wait();
}

void AudioEngine::play(const QString &path, quint64 id, double startSeconds, int outputRate)
{
    m_paths.insert(id, path);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_abort)
            *m_abort = true; // e.g. a stream that is still connecting
        m_cmdOpen = true;
        m_cmdRate = outputRate;
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
        if (m_abort)
            *m_abort = true;
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
    if (m_formatSeq) {
        m_formatSeq = 0;
        emit formatChanged(QString());
    }
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
    int rate;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rate = m_playingRate;
    }
    // Keep the output rate, so that seeking never reopens the device.
    play(path, m_currentId, qMax(0.0, seconds), rate);
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

bool AudioEngine::isBuffering() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_buffering && m_state == Playing;
}

double AudioEngine::duration() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playingRate <= 0 || m_playingTotal < 0)
        return -1;
    return double(m_playingTotal) / m_playingRate;
}

int AudioEngine::recentSamples(float *out, int frames) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sinkRate <= 0 || m_state != Playing)
        return 0;
    const quint64 size = m_scope.size();
    frames = int(qMin<quint64>(quint64(frames), size / 2));
    // What leaves the speakers now was written 'latency' frames ago.
    const quint64 latency = quint64(qMax(0LL, m_latencyFrames.load()));
    const quint64 end = m_scopeWritten > latency ? m_scopeWritten - latency : 0;
    for (int i = 0; i < frames; ++i) {
        const quint64 pos = end + quint64(i) - quint64(frames);
        out[i] = pos < end ? m_scope[pos & (size - 1)] : 0.f; // (wraps around before the start)
    }
    return m_sinkRate;
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
    m_buffering = false;
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
    quint64 seq, id, metaSeq;
    int wantRate, frontRate = 0;
    bool drained;
    QString metaName, metaTitle, format;
    bool haveFormat = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto &f : m_formats) {
            if (f.first == m_playingSeq) {
                format = f.second;
                haveFormat = true;
            }
        }
        metaSeq = m_metaSeq;
        metaName = m_metaName;
        metaTitle = m_metaTitle;
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
    if (seq && seq != m_formatSeq && haveFormat) {
        m_formatSeq = seq;
        emit formatChanged(format);
    }
    if (metaSeq != m_seenMetaSeq) {
        m_seenMetaSeq = metaSeq;
        emit streamMetadata(id, metaName, metaTitle);
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

    // Streams start, and continue after an underrun, only with a reserve.
    if (m_buffering && (m_chunks.empty() || m_chunks.front().stream)) {
        const long long reserve = (long long)m_sinkRate * kStreamReserveMs / 1000;
        if (m_queuedFrames >= reserve || m_decoderEof || m_cmdOpen)
            m_buffering = false;
    }
    while (!m_buffering && produced < frames && !m_chunks.empty()) {
        Chunk &c = m_chunks.front();
        if (c.rate != m_sinkRate) {
            m_wantRate = c.rate;
            break;
        }
        if (c.hasMeta) {
            c.hasMeta = false;
            m_metaName = c.metaName;
            m_metaTitle = c.metaTitle;
            ++m_metaSeq;
        }
        const size_t available = c.samples.size() / 2 - c.offset;
        const size_t n = std::min<size_t>(available, size_t(frames - produced));
        const float *in = c.samples.data() + c.offset * 2;
        const size_t scopeMask = m_scope.size() - 1;
        for (size_t i = 0; i < n * 2; i += 2) {
            m_scope[m_scopeWritten++ & scopeMask] = 0.5f * (in[i] + in[i + 1]);
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
        m_playingStream = c.stream;
        if (c.offset * 2 >= c.samples.size()) {
            m_queuedFrames -= (long long)(c.samples.size() / 2);
            m_chunks.pop_front();
        }
    }
    if (m_buffering != m_loggedBuffering) {
        m_loggedBuffering = m_buffering;
        qCDebug(lcEngine) << "buffering" << m_buffering << "queued" << m_queuedFrames << "eof" << m_decoderEof;
    }
    m_peakL = peakL;
    m_peakR = peakR;
    if (m_chunks.empty() && m_endOfStream && !m_cmdOpen)
        m_drained = true;
    else if (m_chunks.empty() && m_playingStream && !m_decoderEof)
        m_buffering = true; // the network could not keep up

    lock.unlock();
    m_cond.notify_all();

    // Never starve the device: an idle sink would stop pulling.
    if (produced < frames)
        std::memset(data + produced * 2 * bytesPerSample, 0, size_t((frames - produced) * 2 * bytesPerSample));
    return frames * 2 * bytesPerSample;
}

void AudioEngine::decoderLoop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_quit) {
        if (m_cmdStop) {
            m_cmdStop = false;
            auto old = std::move(m_decoder);
            m_abort.reset();
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
        int openRate = 0;
        bool explicitOpen = false;
        if (m_cmdOpen) {
            m_cmdOpen = false;
            explicitOpen = true;
            openPath = m_cmdPath;
            openId = m_cmdId;
            openSeq = m_cmdSeq;
            openSeconds = m_cmdSeconds;
            openRate = m_cmdRate;
        } else if (m_decoderEof && m_decodingId && m_nextId && m_nextAfterId == m_decodingId) {
            openPath = m_nextPath;
            openId = m_nextId;
            openSeq = ++m_seqCounter;
            openRate = m_chainRate; // resample to keep the output stream running
            m_nextId = 0;
        }
        if (openId) {
            const unsigned gen = m_generation;
            m_decodingId = openId;
            m_decodingSeq = openSeq;
            m_decoderEof = true;
            m_endOfStream = false;
            auto old = std::move(m_decoder);
            auto dec = std::make_unique<Decoder>();
            m_abort = dec->abortFlag();
            lock.unlock();
            old.reset();
            QString err;
            if (!dec->open(openPath, openRate)) {
                err = dec->errorString();
                if (err.isEmpty())
                    err = tr("File can't be opened.");
                dec.reset();
            } else if (openSeconds > 0) {
                dec->seek(openSeconds);
            }
            lock.lock();
            if (gen != m_generation || m_cmdOpen || m_cmdStop || m_quit)
                continue; // superseded while opening
            m_decoder = std::move(dec);
            m_decoderEof = !m_decoder;
            m_decodeFrame = 0;
            if (m_decoder) {
                m_chainRate = m_decoder->sampleRate();
                m_formats.emplace_back(openSeq, m_decoder->formatDescription());
                if (m_formats.size() > 8)
                    m_formats.pop_front();
                m_decodeFrame = (long long)(openSeconds * m_decoder->sampleRate());
                if (explicitOpen) {
                    m_playingRate = m_decoder->sampleRate();
                    m_playingTotal = m_decoder->totalFrames();
                    m_playingFrame = m_decodeFrame;
                    m_buffering = m_decoder->isStream();
                }
                if (m_decoder->isStream()) {
                    const QString codec = m_decoder->codecName();
                    const int kbps = m_decoder->bitrate();
                    QMetaObject::invokeMethod(
                        this, [this, openId, codec, kbps] { emit streamInfo(openId, codec, kbps); }, Qt::QueuedConnection);
                }
            } else {
                m_abort.reset();
            }
            qCDebug(lcEngine) << "decoding" << openId << openPath << (explicitOpen ? "(explicit)" : "(gapless)")
                              << (m_decoder ? m_decoder->nativeRate() : 0) << "->" << m_chainRate << err;
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
            lock.unlock();
            Chunk chunk;
            chunk.samples.resize(size_t(kBlockFrames) * 2);
            const long n = dec->read(chunk.samples.data(), kBlockFrames);
            chunk.samples.resize(size_t(qMax(0L, n)) * 2);
            chunk.hasMeta = dec->takeMetadataChange(chunk.metaName, chunk.metaTitle);
            lock.lock();
            if (gen != m_generation || m_decoder.get() != dec)
                continue;
            if (n <= 0) {
                m_decoderEof = true;
                if (!(m_nextId && m_nextAfterId == m_decodingId))
                    m_endOfStream = true;
                if (dec->failed()) {
                    const quint64 id = m_decodingId;
                    const QString err = dec->errorString();
                    qCDebug(lcEngine) << "decoder error" << id << err;
                    QMetaObject::invokeMethod(this, [this, id, err] { emit error(id, err); }, Qt::QueuedConnection);
                }
                continue;
            }
            chunk.trackId = m_decodingId;
            chunk.seq = m_decodingSeq;
            chunk.rate = rate;
            chunk.trackFrame = m_decodeFrame;
            chunk.totalFrames = dec->totalFrames();
            chunk.stream = dec->isStream();
            m_decodeFrame += n;
            m_queuedFrames += n;
            m_drained = false;
            m_chunks.push_back(std::move(chunk));
            continue;
        }
        m_cond.wait(lock);
    }
}
