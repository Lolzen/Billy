#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class Decoder;
class QThread;
class SinkWorker;

// Gapless playback engine.
//
// A decoder thread keeps a few seconds of decoded stereo float audio queued.
// When a track ends it immediately continues with the track announced via
// setNext(), so consecutive tracks are written back to back into the same
// output stream. The output stream lives in its own thread (so a busy GUI
// never causes drop-outs) and is only reopened when the sample rate changes
// between two tracks.
class AudioEngine : public QObject
{
    Q_OBJECT
public:
    enum State { Stopped, Playing, Paused };
    Q_ENUM(State)

    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    void play(const QString &path, quint64 id, double startSeconds = 0);
    // Announces which track follows 'afterId'. Ignored when the decoder is no
    // longer working on 'afterId'. An id of 0 means "nothing follows".
    void setNext(quint64 afterId, const QString &path, quint64 id);
    void pause();
    void resume();
    void stop();
    void seek(double seconds);
    // The file of a known track was renamed.
    void updatePath(quint64 id, const QString &path) { m_paths.insert(id, path); }

    void setVolume(int percent); // 0..100
    int volume() const { return m_volumePercent; }

    void setOutputDevice(const QByteArray &deviceId); // empty: system default
    QByteArray outputDevice() const { return m_deviceId; }
    QString outputDescription() const;

    void setBufferLength(int ms);
    int bufferLength() const { return m_bufferMs; }

    State state() const { return m_state; }
    quint64 currentId() const { return m_currentId; }
    double position() const;  // seconds into the current track
    double duration() const;  // seconds, -1 if unknown

    // Peak levels (linear, 0..1) of the audio played since the last call.
    void takeLevels(float &left, float &right);

signals:
    void trackStarted(quint64 id);      // playback reached a new track (gapless change)
    void decodingStarted(quint64 id);   // good moment to call setNext()
    void finished();                    // nothing left to play
    void error(quint64 id, const QString &message);
    void stateChanged(AudioEngine::State state);

private:
    friend class SinkWorker;
    friend class PcmSource;

    struct Chunk
    {
        std::vector<float> samples; // interleaved stereo
        size_t offset = 0;          // frames already consumed
        quint64 trackId = 0;
        quint64 seq = 0;            // unique per started track instance
        int rate = 0;
        long long trackFrame = 0;   // frame index (within the track) of samples[0]
        long long totalFrames = -1;
    };

    void decoderLoop();
    void flushLocked();
    void setState(State s);
    void poll();
    void requestSink(int rate);
    void closeSink();
    // Called by the output thread.
    qint64 render(char *data, qint64 maxBytes);

    // --- shared state, guarded by m_mutex
    mutable std::mutex m_mutex;
    std::condition_variable m_cond;
    std::thread m_thread;
    bool m_quit = false;
    bool m_cmdOpen = false;
    bool m_cmdStop = false;
    QString m_cmdPath;
    quint64 m_cmdId = 0;
    quint64 m_cmdSeq = 0;
    double m_cmdSeconds = 0;
    quint64 m_seqCounter = 0;

    std::unique_ptr<Decoder> m_decoder;
    quint64 m_decodingId = 0;
    quint64 m_decodingSeq = 0;
    long long m_decodeFrame = 0;
    bool m_decoderEof = true;
    bool m_endOfStream = true; // eof and nothing follows

    QString m_nextPath;
    quint64 m_nextId = 0;
    quint64 m_nextAfterId = 0;

    std::deque<Chunk> m_chunks;
    long long m_queuedFrames = 0;
    unsigned m_generation = 0;
    int m_bufferMsShared = 3000;

    // Output (render) state.
    int m_sinkRate = 0;
    bool m_sinkFloat = true;
    quint64 m_playingId = 0;
    quint64 m_playingSeq = 0;
    long long m_playingFrame = 0;
    long long m_playingTotal = -1;
    int m_playingRate = 0;
    int m_wantRate = 0;        // != 0: output must be reopened at this rate
    bool m_drained = false;    // queue empty and end of stream reached
    float m_peakL = 0, m_peakR = 0;

    std::atomic<float> m_gain{1.0f};
    std::atomic<long long> m_latencyFrames{0};

    // --- main thread only
    QThread *m_outputThread = nullptr;
    SinkWorker *m_worker = nullptr;
    int m_requestedRate = 0;
    QTimer m_pollTimer;
    State m_state = Stopped;
    quint64 m_currentId = 0;
    quint64 m_currentSeq = 0;
    int m_volumePercent = 100;
    QByteArray m_deviceId;
    int m_bufferMs = 3000;
    int m_drainTicks = 0;
    QHash<quint64, QString> m_paths;
};
