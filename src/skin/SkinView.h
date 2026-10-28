#pragma once

#include <QImage>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <vector>

// The alternative, skinned look: an "aurora" background, the album cover on a
// string, track details, an animated spectrum analyser and a translucent
// control bar. Everything is painted here; the playlist widget is borrowed
// from the main window for the playlist page.
class SkinView : public QWidget
{
    Q_OBJECT
public:
    enum Page { InfoPage, PlaylistPage };

    struct TrackInfo
    {
        QString album;    // first line (yellow)
        QString artist;
        QString title;    // italic
        QStringList details;
        QImage cover;
    };

    explicit SkinView(QWidget *parent = nullptr);

    void setPage(Page page);
    Page page() const { return m_page; }
    // Shown on the playlist page (reparented into this view); nullptr releases it.
    void setPlaylistWidget(QWidget *widget);

    void setTrackInfo(const TrackInfo &info);
    // 'fraction' < 0: nothing to show (stopped or a stream).
    void setProgress(double fraction, const QString &time);
    // Levels 0..1 per band of each channel (low frequencies first). They are
    // shown mirrored from the middle: left channel to the left, right to the
    // right. Empty vectors let the bars fall.
    void setSpectrum(const std::vector<float> &left = {}, const std::vector<float> &right = {});
    int spectrumBands() const;
    void setPlaying(bool playing);
    void setVolume(int percent);
    void showMessage(const QString &text, int ms = 4000);

signals:
    void previousClicked();
    void playPauseClicked();
    void nextClicked();
    void stopClicked();
    void ejectClicked();
    void closeClicked();
    void minimizeClicked();
    // 'anchored': open above 'globalPos' (the alien), otherwise at the cursor.
    void menuRequested(const QPoint &globalPos, bool anchored);
    void seekRequested(double fraction);
    void volumeRequested(int percent);
    void pageChanged(SkinView::Page page);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void leaveEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *e) override;
    bool event(QEvent *e) override;

private:
    enum Button { None = -1, Prev, PlayPause, Next, Stop, Eject, Info, Playlist, Volume, Close, Minimize, Menu };
    struct ButtonRect
    {
        Button id;
        QRect rect;
        QString text;    // empty: glyph button
        QString tooltip;
    };

    void layoutParts();
    void renderBackground();
    void paintFrame(QPainter &p);
    void paintProgress(QPainter &p);
    void paintInfo(QPainter &p);
    void paintSpectrum(QPainter &p);
    void paintBar(QPainter &p);
    void paintGlyph(QPainter &p, Button id, const QRect &r, const QColor &c);
    void paintAlien(QPainter &p, const QRect &r, bool hover);
    Button buttonAt(const QPoint &pos) const;
    Qt::Edges edgesAt(const QPoint &pos) const;
    void trigger(Button id, const QPoint &globalPos);
    void animate();

    Page m_page = InfoPage;
    QWidget *m_playlist = nullptr;
    TrackInfo m_info;
    QImage m_coverScaled;
    double m_progress = -1;
    QString m_time;
    bool m_playing = false;
    int m_volume = 100;
    QString m_message;
    QTimer m_messageTimer;

    // Spectrum: target levels from the engine, animated bar heights, peak caps.
    std::vector<float> m_targetLeft, m_targetRight;
    std::vector<float> m_bars;
    std::vector<float> m_peaks;
    std::vector<int> m_peakHold;
    QTimer m_animation;

    QImage m_background;
    QRect m_content, m_progressRect, m_barRect, m_spectrumRect, m_infoRect, m_listRect, m_coverRect;
    QList<ButtonRect> m_buttons;
    Button m_hover = None;
    Button m_pressed = None;
    bool m_seeking = false;
    bool m_volumeDragging = false;
    bool m_forwarding = false;
    int m_volumeDragY = 0, m_volumeDragStart = 0;
};
