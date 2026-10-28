#include "skin/SkinView.h"

#include <QCoreApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <random>

namespace {

constexpr int kFrame = 8;       // outer frame around the picture
constexpr int kStrip = 16;      // progress strip at the top
constexpr int kBar = 24;        // control bar at the bottom
constexpr int kBarStep = 7;     // spectrum: bar width + gap
constexpr int kSegment = 3;     // spectrum: segment height + gap
constexpr int kResizeBorder = 6;

const QColor kAlbumColor(0xf3, 0xe7, 0xa1);
const QColor kTextColor(0xf4, 0xf4, 0xf4);

double smoothstep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// Text with a soft shadow, elided to 'width'.
void shadowText(QPainter &p, int x, int y, int width, const QString &text, const QFont &font, const QColor &color)
{
    p.setFont(font);
    const QString elided = QFontMetrics(font).elidedText(text, Qt::ElideRight, width);
    p.setPen(QColor(0, 0, 0, 150));
    p.drawText(x + 1, y + 1, elided);
    p.setPen(color);
    p.drawText(x, y, elided);
}

QFont pixelFont(int px, bool bold = false, bool italic = false)
{
    QFont f;
    f.setPixelSize(px);
    f.setBold(bold);
    f.setItalic(italic);
    return f;
}

} // namespace

SkinView::SkinView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(520, 260);
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        update();
    });
    m_animation.setInterval(33);
    connect(&m_animation, &QTimer::timeout, this, &SkinView::animate);
}

// ------------------------------------------------------------------ state

void SkinView::setPage(Page page)
{
    m_page = page;
    if (m_playlist) {
        m_playlist->setGeometry(m_listRect);
        m_playlist->setVisible(page == PlaylistPage);
        if (page == PlaylistPage)
            m_playlist->setFocus();
    }
    emit pageChanged(page);
    update();
}

void SkinView::setPlaylistWidget(QWidget *widget)
{
    m_playlist = widget;
    if (!widget)
        return;
    widget->setParent(this);
    widget->setGeometry(m_listRect);
    widget->setVisible(m_page == PlaylistPage);
}

void SkinView::setTrackInfo(const TrackInfo &info)
{
    m_info = info;
    m_coverScaled = QImage();
    update();
}

void SkinView::setProgress(double fraction, const QString &time)
{
    if (qFuzzyCompare(fraction + 2, m_progress + 2) && time == m_time)
        return;
    if (!m_seeking)
        m_progress = fraction;
    m_time = time;
    update(m_progressRect.adjusted(-2, -2, 2, 2));
    update(m_barRect);
}

int SkinView::spectrumBands() const
{
    return qMax(8, m_spectrumRect.width() / kBarStep);
}

void SkinView::setSpectrum(const std::vector<float> &levels)
{
    m_target = levels;
    if (!m_animation.isActive() && isVisible())
        m_animation.start();
}

void SkinView::setPlaying(bool playing)
{
    if (playing == m_playing)
        return;
    m_playing = playing;
    update(m_barRect);
}

void SkinView::setVolume(int percent)
{
    m_volume = percent;
    update(m_barRect);
}

void SkinView::showMessage(const QString &text, int ms)
{
    m_message = text;
    if (text.isEmpty())
        m_messageTimer.stop();
    else
        m_messageTimer.start(ms);
    update();
}

void SkinView::animate()
{
    const size_t n = size_t(spectrumBands());
    if (m_bars.size() != n)
        m_bars.assign(n, 0.f);
    bool moving = false;
    for (size_t i = 0; i < n; ++i) {
        // Map the analyser bands onto the visible bars.
        float target = 0.f;
        if (!m_target.empty())
            target = m_target[std::min(m_target.size() - 1, i * m_target.size() / n)];
        float &bar = m_bars[i];
        if (target > bar)
            bar += (target - bar) * 0.7f; // quick rise
        else
            bar = std::max(target, bar - 0.03f); // slow fall
        moving |= bar > 0.001f || target > 0.001f;
    }
    if (!moving)
        m_animation.stop();
    update(m_spectrumRect.adjusted(-2, -2, 2, 2));
}

// ----------------------------------------------------------------- layout

void SkinView::resizeEvent(QResizeEvent *)
{
    layoutParts();
    renderBackground();
}

void SkinView::layoutParts()
{
    m_content = rect().adjusted(kFrame, kFrame, -kFrame, -kFrame);
    m_progressRect = QRect(m_content.left() + 6, m_content.top() + 4, m_content.width() - 12, 8);
    m_barRect = QRect(m_content.left(), m_content.bottom() - kBar + 1, m_content.width(), kBar);

    const int specH = std::clamp(m_content.height() / 5, 24, 64) / kSegment * kSegment;
    const int specLeft = m_content.left() + int(m_content.width() * 0.18);
    const int specWidth = int(m_content.width() * 0.62) / kBarStep * kBarStep;
    m_spectrumRect = QRect(specLeft, m_barRect.top() - specH - 1, specWidth, specH);

    const int top = m_content.top() + kStrip;
    // The picture ends above the spectrum.
    const int coverSide = std::clamp(m_spectrumRect.top() - 8 - (top + 40), 64, 220);
    const int coverLeft = m_content.left() + std::max(24, int(m_content.width() * 0.065));
    m_coverRect = QRect(coverLeft, top + 40, coverSide, coverSide);
    m_infoRect = QRect(m_coverRect.right() + 22, m_coverRect.top() + 6,
                       m_content.right() - m_coverRect.right() - 40, m_spectrumRect.top() - m_coverRect.top() - 6);
    m_listRect = QRect(m_content.left() + 16, top + 10, m_content.width() - 32, m_spectrumRect.top() - top - 16);
    m_coverScaled = QImage();

    // Control bar: glyph buttons and text buttons on the left, window buttons on the right.
    m_buttons.clear();
    const QFontMetrics fm(pixelFont(9, true));
    int x = m_barRect.left() + 6;
    const QList<QPair<Button, QString>> glyphs = {
        {Prev, tr("Previous")}, {PlayPause, tr("Play / pause")}, {Next, tr("Next")},
        {Stop, tr("Stop")},     {Eject, tr("Open folder")},
    };
    for (const auto &g : glyphs) {
        m_buttons.append({g.first, QRect(x, m_barRect.top(), 20, kBar), QString(), g.second});
        x += 20;
    }
    x += 10;
    for (const auto &t : {qMakePair(Info, tr("INFO")), qMakePair(Playlist, tr("PLAYLIST"))}) {
        const int w = fm.horizontalAdvance(t.second) + 22;
        m_buttons.append({t.first, QRect(x, m_barRect.top(), w, kBar), t.second, QString()});
        x += w;
    }
    int r = m_barRect.right() - 4;
    m_buttons.append({Menu, QRect(r - 30, m_barRect.top() - 9, 30, 32), QString(), tr("Menu")});
    r -= 30 + 8;
    for (const auto &t : {qMakePair(Minimize, tr("MINIMIZE")), qMakePair(Close, tr("CLOSE"))}) {
        const int w = fm.horizontalAdvance(t.second) + 16;
        m_buttons.append({t.first, QRect(r - w, m_barRect.top(), w, kBar), t.second, QString()});
        r -= w;
    }
    const int vw = fm.horizontalAdvance(QStringLiteral("100%")) + 16;
    m_buttons.append({Volume, QRect(r - vw - 6, m_barRect.top(), vw, kBar), QString(),
                      tr("Volume: drag up/down or use the mouse wheel")});

    if (m_playlist)
        m_playlist->setGeometry(m_listRect);
}

void SkinView::renderBackground()
{
    const QSize size = m_content.size();
    if (size.isEmpty())
        return;
    const int w = size.width(), h = size.height();
    std::mt19937 rng(1977); // same streaks every time
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    const double p1 = uni(rng) * 6.28, p2 = uni(rng) * 6.28, p3 = uni(rng) * 6.28, p4 = uni(rng) * 6.28;
    // The streaks are defined on a 740px wide reference, so they keep their width.
    const double scale = 740.0 / std::max(w, 1);

    std::vector<QColor> columns(static_cast<size_t>(w));
    std::vector<double> lengths(static_cast<size_t>(w));
    double jitter = 0;
    for (int x = 0; x < w; ++x) {
        const double t = double(x) / w;
        const double xr = x * scale;
        if (x % 2 == 0)
            jitter = (uni(rng) - 0.5) * 0.28;
        double v = 0.42 + 0.20 * std::sin(xr * 0.021 + p1) + 0.12 * std::sin(xr * 0.067 + p2)
            + 0.09 * std::sin(xr * 0.19 + p3) + jitter;
        v *= 0.35 + 0.65 * smoothstep(0.0, 0.08, t);        // dark left edge
        v *= 1.0 - 0.88 * smoothstep(0.78, 1.0, t);         // fading into the dark right
        v *= 1.0 + 0.25 * smoothstep(0.45, 0.7, t);         // bright teal-green middle
        const double hue = 0.60 - 0.22 * t;                 // blue -> teal -> green
        const double sat = 0.70 + 0.18 * std::sin(xr * 0.013 + p4);
        columns[size_t(x)] = QColor::fromHsvF(float(hue), float(std::clamp(sat, 0.0, 1.0)), float(std::clamp(v, 0.0, 1.0)));
        lengths[size_t(x)] = 0.55 + 0.45 * uni(rng);
    }

    m_background = QImage(size, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto *line = reinterpret_cast<QRgb *>(m_background.scanLine(y));
        const double fy = double(y) / h;
        const double rowDark = 1.0 - 0.45 * fy * fy; // darker towards the bottom
        for (int x = 0; x < w; ++x) {
            const double streak = std::clamp(1.35 - fy / lengths[size_t(x)], 0.45, 1.0);
            const double f = rowDark * streak;
            const QColor &c = columns[size_t(x)];
            line[x] = qRgb(int(c.red() * f), int(c.green() * f), int(c.blue() * f));
        }
    }
}

// ------------------------------------------------------------------ paint

void SkinView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    paintFrame(p);
    paintProgress(p);
    paintInfo(p);
    paintSpectrum(p);
    paintBar(p);
}

void SkinView::paintFrame(QPainter &p)
{
    p.fillRect(rect(), QColor(0x18, 0x18, 0x18));
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QColor(0x5c, 0x5c, 0x5c));
    p.drawRect(rect().adjusted(2, 2, -3, -3));
    p.setPen(QColor(0xa8, 0xa8, 0xa8));
    p.drawRect(rect().adjusted(5, 5, -6, -6));
    p.drawImage(m_content.topLeft(), m_background);
    p.setPen(QColor(0, 0, 0, 170));
    p.drawRect(m_content.adjusted(0, 0, -1, -1));
}

void SkinView::paintProgress(QPainter &p)
{
    const QRect strip(m_content.left(), m_content.top(), m_content.width(), kStrip);
    p.fillRect(strip, QColor(0, 0, 0, 110));
    p.fillRect(QRect(strip.left(), strip.bottom(), strip.width(), 1), QColor(255, 255, 255, 45));
    p.fillRect(m_progressRect, QColor(255, 255, 255, 26));
    if (m_progress >= 0) {
        QRect fill = m_progressRect;
        fill.setWidth(int(m_progressRect.width() * std::clamp(m_progress, 0.0, 1.0)));
        p.fillRect(fill, QColor(255, 255, 255, 95));
        p.fillRect(QRect(fill.left(), fill.top(), fill.width(), 1), QColor(255, 255, 255, 70));
    }
    if (!m_message.isEmpty()) {
        const QFont f = pixelFont(10);
        const int w = QFontMetrics(f).horizontalAdvance(m_message) + 4;
        const int x = std::max(m_content.left() + 12, m_content.right() - 12 - w);
        shadowText(p, x, strip.bottom() + 16, m_content.right() - 12 - x, m_message, f, QColor(255, 255, 255, 220));
    }
}

void SkinView::paintInfo(QPainter &p)
{
    if (m_page != InfoPage) {
        // A dark glass pane behind the playlist.
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor(255, 255, 255, 40));
        p.setBrush(QColor(0, 0, 0, 105));
        p.drawRoundedRect(QRectF(m_listRect).adjusted(-6.5, -4.5, 6.5, 4.5), 4, 4);
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);
    const QRect c = m_coverRect;

    // The picture hangs on a string from a nail.
    const QPointF nail(c.center().x() + 0.5, c.top() - 28);
    p.setPen(QPen(QColor(10, 10, 10, 220), 1.4));
    p.drawLine(nail, QPointF(c.left() + c.width() * 0.2, c.top()));
    p.drawLine(nail, QPointF(c.right() - c.width() * 0.2, c.top()));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(15, 15, 15));
    p.drawEllipse(nail, 2.6, 2.6);
    p.setBrush(QColor(255, 255, 255, 90));
    p.drawEllipse(nail + QPointF(-0.8, -0.8), 0.9, 0.9);

    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(c.translated(4, 5), QColor(0, 0, 0, 90));
    p.fillRect(c.translated(2, 2), QColor(0, 0, 0, 90));
    if (!m_info.cover.isNull()) {
        if (m_coverScaled.size() != c.size()) {
            // Fill the square, cropping a non-square picture in the middle.
            m_coverScaled = m_info.cover.scaled(c.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            m_coverScaled = m_coverScaled.copy((m_coverScaled.width() - c.width()) / 2,
                                               (m_coverScaled.height() - c.height()) / 2, c.width(), c.height());
        }
        p.drawImage(c.topLeft(), m_coverScaled);
    } else {
        QLinearGradient g(c.topLeft(), c.bottomRight());
        g.setColorAt(0, QColor(20, 40, 70, 230));
        g.setColorAt(1, QColor(5, 15, 25, 240));
        p.fillRect(c, g);
        p.setRenderHint(QPainter::Antialiasing);
        QFont note = pixelFont(c.height() / 2);
        p.setFont(note);
        p.setPen(QColor(255, 255, 255, 60));
        p.drawText(c, Qt::AlignCenter, QStringLiteral("♪"));
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QColor(0, 0, 0, 200));
    p.drawRect(c.adjusted(-2, -2, 1, 1));
    p.setPen(QColor(230, 230, 230, 210));
    p.drawRect(c.adjusted(-1, -1, 0, 0));

    // Text
    p.setRenderHint(QPainter::TextAntialiasing);
    const int x = m_infoRect.left();
    const int w = m_infoRect.width();
    int y = m_infoRect.top() + 22;
    if (!m_info.album.isEmpty()) {
        shadowText(p, x, y, w, m_info.album, pixelFont(16), kAlbumColor);
        y += 22;
    }
    if (!m_info.artist.isEmpty()) {
        shadowText(p, x, y, w, m_info.artist, pixelFont(13), kTextColor);
        y += 20;
    }
    if (!m_info.title.isEmpty()) {
        shadowText(p, x, y, w, m_info.title, pixelFont(13, false, true), kTextColor);
        y += 20;
    }
    y += 18;
    for (const QString &line : m_info.details) {
        if (y > m_infoRect.bottom())
            break;
        shadowText(p, x, y, w, line, pixelFont(11), kTextColor);
        y += 17;
    }
}

void SkinView::paintSpectrum(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing, false);
    const int segments = m_spectrumRect.height() / kSegment;
    const int bars = spectrumBands();
    for (int i = 0; i < bars; ++i) {
        const float level = i < int(m_bars.size()) ? m_bars[size_t(i)] : 0.f;
        const int lit = int(std::lround(level * segments));
        const int x = m_spectrumRect.left() + i * kBarStep;
        // The lowest segment always shows, as a dashed base line.
        for (int s = 0; s < std::max(lit, 1); ++s) {
            const int y = m_spectrumRect.bottom() - s * kSegment - 1;
            // The higher, the less transparent.
            const int alpha = lit == 0 ? 70 : int(60 + 195.0 * (s + 1) / segments);
            p.fillRect(QRect(x, y, kBarStep - 2, kSegment - 1), QColor(240, 240, 240, alpha));
        }
    }
}

void SkinView::paintGlyph(QPainter &p, Button id, const QRect &r, const QColor &c)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const QPointF m = QRectF(r).center();
    auto tri = [&p](QPointF a, QPointF b, QPointF d) {
        QPainterPath path;
        path.moveTo(a);
        path.lineTo(b);
        path.lineTo(d);
        path.closeSubpath();
        p.drawPath(path);
    };
    switch (id) {
    case Prev:
        p.drawRect(QRectF(m.x() - 5, m.y() - 3.5, 1.5, 7));
        tri({m.x() - 3.5, m.y()}, {m.x() + 0.5, m.y() - 3.5}, {m.x() + 0.5, m.y() + 3.5});
        tri({m.x() + 0.5, m.y()}, {m.x() + 4.5, m.y() - 3.5}, {m.x() + 4.5, m.y() + 3.5});
        break;
    case PlayPause:
        if (m_playing) {
            p.drawRect(QRectF(m.x() - 3, m.y() - 3.5, 2, 7));
            p.drawRect(QRectF(m.x() + 1, m.y() - 3.5, 2, 7));
        } else {
            tri({m.x() - 2.5, m.y() - 4}, {m.x() + 3.5, m.y()}, {m.x() - 2.5, m.y() + 4});
        }
        break;
    case Next:
        tri({m.x() - 4.5, m.y() - 3.5}, {m.x() - 0.5, m.y()}, {m.x() - 4.5, m.y() + 3.5});
        tri({m.x() - 0.5, m.y() - 3.5}, {m.x() + 3.5, m.y()}, {m.x() - 0.5, m.y() + 3.5});
        p.drawRect(QRectF(m.x() + 3.5, m.y() - 3.5, 1.5, 7));
        break;
    case Stop:
        p.drawRect(QRectF(m.x() - 3, m.y() - 3, 6.5, 6.5));
        break;
    case Eject:
        tri({m.x() - 4, m.y() + 1}, {m.x() + 0.25, m.y() - 4}, {m.x() + 4.5, m.y() + 1});
        p.drawRect(QRectF(m.x() - 4, m.y() + 2.5, 8.5, 1.5));
        break;
    default:
        break;
    }
}

void SkinView::paintAlien(QPainter &p, const QRect &r, bool hover)
{
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF f = QRectF(r).adjusted(3, 1, -3, -1);
    const double cx = f.center().x();
    // Head: wide skull, narrow chin.
    QPainterPath head;
    head.moveTo(cx, f.bottom());
    head.cubicTo(cx - f.width() * 0.18, f.bottom() - 1, f.left(), f.top() + f.height() * 0.62, f.left(), f.top() + f.height() * 0.36);
    head.cubicTo(f.left(), f.top() + 2, cx - f.width() * 0.25, f.top(), cx, f.top());
    head.cubicTo(cx + f.width() * 0.25, f.top(), f.right(), f.top() + 2, f.right(), f.top() + f.height() * 0.36);
    head.cubicTo(f.right(), f.top() + f.height() * 0.62, cx + f.width() * 0.18, f.bottom() - 1, cx, f.bottom());
    QLinearGradient g(f.topLeft(), f.bottomLeft());
    g.setColorAt(0, hover ? QColor(0xf0, 0xf0, 0xf0) : QColor(0xd6, 0xd6, 0xd6));
    g.setColorAt(1, hover ? QColor(0x90, 0x90, 0x90) : QColor(0x6c, 0x6c, 0x6c));
    p.setPen(QPen(QColor(20, 20, 20, 200), 1));
    p.setBrush(g);
    p.drawPath(head);
    // Big slanted eyes.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(8, 8, 8));
    for (int side : {-1, 1}) {
        p.save();
        p.translate(cx + side * f.width() * 0.2, f.top() + f.height() * 0.5);
        p.rotate(side * 32);
        p.drawEllipse(QRectF(-f.width() * 0.17, -f.height() * 0.09, f.width() * 0.34, f.height() * 0.18));
        p.restore();
    }
    p.setBrush(QColor(255, 255, 255, 120));
    p.drawEllipse(QPointF(cx - f.width() * 0.24, f.top() + f.height() * 0.46), 0.9, 0.9);
    p.drawEllipse(QPointF(cx + f.width() * 0.16, f.top() + f.height() * 0.46), 0.9, 0.9);
}

void SkinView::paintBar(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(m_barRect, QColor(0, 0, 0, 150));
    p.fillRect(QRect(m_barRect.left(), m_barRect.top(), m_barRect.width(), 1), QColor(255, 255, 255, 45));

    const QFont font = pixelFont(9, true);
    p.setFont(font);
    const QColor normal(205, 205, 205), bright(255, 255, 255);
    for (const ButtonRect &b : m_buttons) {
        const bool hover = b.id == m_hover;
        if (b.id == Menu) {
            paintAlien(p, b.rect, hover);
            continue;
        }
        if (b.id == Volume) {
            p.setPen(hover ? bright : normal);
            p.drawText(b.rect, Qt::AlignCenter, QStringLiteral("%1%").arg(m_volume));
            continue;
        }
        if (b.text.isEmpty()) {
            paintGlyph(p, b.id, b.rect, hover ? bright : normal);
            p.setRenderHint(QPainter::Antialiasing, false);
            continue;
        }
        const bool active = (b.id == Info && m_page == InfoPage) || (b.id == Playlist && m_page == PlaylistPage);
        p.setPen(hover || active ? bright : normal);
        p.drawText(b.rect, Qt::AlignCenter, b.text);
        if (active)
            p.fillRect(QRect(b.rect.left() + 8, b.rect.bottom() - 4, b.rect.width() - 16, 1), QColor(255, 255, 255, 140));
        if (b.id == Info || b.id == Playlist) {
            // Separators between the text buttons, like in the screenshot.
            p.fillRect(QRect(b.rect.left(), b.rect.top() + 6, 1, kBar - 12), QColor(255, 255, 255, 55));
            if (b.id == Playlist)
                p.fillRect(QRect(b.rect.right(), b.rect.top() + 6, 1, kBar - 12), QColor(255, 255, 255, 55));
        }
    }

    // Playing time, left of the volume.
    if (!m_time.isEmpty()) {
        QRect volume;
        for (const ButtonRect &b : m_buttons)
            if (b.id == Volume)
                volume = b.rect;
        p.setFont(pixelFont(10));
        p.setPen(normal);
        const int w = p.fontMetrics().horizontalAdvance(m_time);
        p.drawText(QRect(volume.left() - w - 8, m_barRect.top(), w, kBar), Qt::AlignVCenter, m_time);
    }
}

// ------------------------------------------------------------------ input

SkinView::Button SkinView::buttonAt(const QPoint &pos) const
{
    for (const ButtonRect &b : m_buttons)
        if (b.rect.contains(pos))
            return b.id;
    return None;
}

Qt::Edges SkinView::edgesAt(const QPoint &pos) const
{
    if (window()->isMaximized())
        return {};
    Qt::Edges edges;
    if (pos.x() < kResizeBorder)
        edges |= Qt::LeftEdge;
    if (pos.x() >= width() - kResizeBorder)
        edges |= Qt::RightEdge;
    if (pos.y() < kResizeBorder)
        edges |= Qt::TopEdge;
    if (pos.y() >= height() - kResizeBorder)
        edges |= Qt::BottomEdge;
    return edges;
}

void SkinView::trigger(Button id, const QPoint &globalPos)
{
    switch (id) {
    case Prev: emit previousClicked(); break;
    case PlayPause: emit playPauseClicked(); break;
    case Next: emit nextClicked(); break;
    case Stop: emit stopClicked(); break;
    case Eject: emit ejectClicked(); break;
    case Info: setPage(InfoPage); break;
    case Playlist: setPage(PlaylistPage); break;
    case Close: emit closeClicked(); break;
    case Minimize: emit minimizeClicked(); break;
    case Menu: {
        QRect alien;
        for (const ButtonRect &b : m_buttons)
            if (b.id == Menu)
                alien = b.rect;
        Q_UNUSED(globalPos);
        emit menuRequested(mapToGlobal(alien.topRight()), true);
        break;
    }
    default: break;
    }
}

void SkinView::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (e->button() == Qt::RightButton) {
        emit menuRequested(e->globalPosition().toPoint(), false);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    if (const Qt::Edges edges = edgesAt(pos)) {
        if (window()->windowHandle())
            window()->windowHandle()->startSystemResize(edges);
        return;
    }
    m_pressed = buttonAt(pos);
    if (m_pressed == Volume) {
        m_volumeDragging = true;
        m_volumeDragY = int(e->globalPosition().y());
        m_volumeDragStart = m_volume;
        return;
    }
    if (m_pressed != None)
        return;
    if (m_progress >= 0 && m_progressRect.adjusted(0, -4, 0, 4).contains(pos)) {
        m_seeking = true;
        m_progress = std::clamp(double(pos.x() - m_progressRect.left()) / m_progressRect.width(), 0.0, 1.0);
        update(m_progressRect.adjusted(-2, -2, 2, 2));
        return;
    }
    // Anywhere else: move the window.
    if (window()->windowHandle())
        window()->windowHandle()->startSystemMove();
}

void SkinView::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (m_volumeDragging) {
        emit volumeRequested(std::clamp(m_volumeDragStart + (m_volumeDragY - int(e->globalPosition().y())), 0, 100));
        return;
    }
    if (m_seeking) {
        m_progress = std::clamp(double(pos.x() - m_progressRect.left()) / m_progressRect.width(), 0.0, 1.0);
        update(m_progressRect.adjusted(-2, -2, 2, 2));
        return;
    }
    const Qt::Edges edges = edgesAt(pos);
    if ((edges & Qt::LeftEdge && edges & Qt::TopEdge) || (edges & Qt::RightEdge && edges & Qt::BottomEdge))
        setCursor(Qt::SizeFDiagCursor);
    else if ((edges & Qt::RightEdge && edges & Qt::TopEdge) || (edges & Qt::LeftEdge && edges & Qt::BottomEdge))
        setCursor(Qt::SizeBDiagCursor);
    else if (edges & (Qt::LeftEdge | Qt::RightEdge))
        setCursor(Qt::SizeHorCursor);
    else if (edges & (Qt::TopEdge | Qt::BottomEdge))
        setCursor(Qt::SizeVerCursor);
    else if (buttonAt(pos) == Volume)
        setCursor(Qt::SizeVerCursor);
    else if (buttonAt(pos) != None || (m_progress >= 0 && m_progressRect.adjusted(0, -4, 0, 4).contains(pos)))
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
    const Button hover = buttonAt(pos);
    if (hover != m_hover) {
        m_hover = hover;
        update(m_barRect.adjusted(0, -10, 0, 0));
    }
}

void SkinView::mouseReleaseEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (m_volumeDragging) {
        m_volumeDragging = false;
        m_pressed = None;
        return;
    }
    if (m_seeking) {
        m_seeking = false;
        emit seekRequested(m_progress);
        return;
    }
    const Button released = buttonAt(pos);
    if (m_pressed != None && released == m_pressed)
        trigger(released, e->globalPosition().toPoint());
    m_pressed = None;
}

void SkinView::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Double click on the picture area switches between info and playlist.
    if (e->button() == Qt::LeftButton && buttonAt(e->position().toPoint()) == None
        && !m_barRect.contains(e->position().toPoint()))
        setPage(m_page == InfoPage ? PlaylistPage : InfoPage);
}

void SkinView::wheelEvent(QWheelEvent *e)
{
    const int step = e->angleDelta().y() > 0 ? 2 : -2;
    emit volumeRequested(std::clamp(m_volume + step, 0, 100));
}

void SkinView::keyPressEvent(QKeyEvent *e)
{
    // Typing, cursor keys etc. go to the playlist even when it is not shown.
    // A key the playlist ignores comes back here (we are its parent): let it go up.
    if (!m_playlist || m_forwarding) {
        QWidget::keyPressEvent(e);
        return;
    }
    m_forwarding = true;
    QCoreApplication::sendEvent(m_playlist, e);
    m_forwarding = false;
}

void SkinView::leaveEvent(QEvent *)
{
    if (m_hover != None) {
        m_hover = None;
        update(m_barRect.adjusted(0, -10, 0, 0));
    }
}

bool SkinView::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        for (const ButtonRect &b : m_buttons) {
            if (b.rect.contains(he->pos()) && !b.tooltip.isEmpty()) {
                QToolTip::showText(he->globalPos(), b.tooltip, this, b.rect);
                return true;
            }
        }
        QToolTip::hideText();
        e->ignore();
        return true;
    }
    if (e->type() == QEvent::Hide)
        m_animation.stop();
    return QWidget::event(e);
}
