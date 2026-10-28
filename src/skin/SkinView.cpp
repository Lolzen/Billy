#include "skin/SkinView.h"
#include "skin/PixelFont.h"

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

// Geometry of the skin at its default size of 700x300 (from the layout of
// the original foobar2000 skin); the window can be resized around it.
constexpr int kFrame = 6;       // frame around the picture
constexpr int kSeekY = 12;      // seek bar: y and height
constexpr int kSeekH = 10;
constexpr int kPanelY = 22;     // info/playlist panel
constexpr int kBarH = 22;       // button row
constexpr int kBarStep = 7;     // spectrum: bar width + gap
constexpr int kSegment = 3;     // spectrum: segment height + gap
constexpr int kSpectrumH = 42;
constexpr int kResizeBorder = 5;

const QColor kAlbumColor(0xf3, 0xe7, 0xa1);
const QColor kTextColor(0xf4, 0xf4, 0xf4);
const QColor kLabelColor(0xc8, 0xc8, 0xc8);
const QColor kLabelHover(0xff, 0xff, 0xff);

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

// The skin was designed with Trebuchet MS; use it when it is there.
QFont pixelFont(int px, bool bold = false, bool italic = false)
{
    QFont f;
    f.setFamilies({QStringLiteral("Trebuchet MS"), QStringLiteral("Fira Sans"), QStringLiteral("Ubuntu"),
                   QStringLiteral("DejaVu Sans")});
    f.setPixelSize(px);
    f.setBold(bold);
    f.setItalic(italic);
    return f;
}

// Small pixel-art glyphs of the button row, 5 rows high.
const QStringList &glyphRows(int id)
{
    static const QList<QStringList> glyphs = {
        {"#...#...#", "#..##..##", "#.###.###", "#..##..##", "#...#...#"}, // previous
        {"#..", "##.", "###", "##.", "#.."},                               // play
        {"#.#", "#.#", "#.#", "#.#", "#.#"},                               // pause
        {"#...#...#", "##..##..#", "###.###.#", "##..##..#", "#...#...#"}, // next
        {"####", "####", "####", "####", "...."},                          // stop
        {"..#..", ".###.", "#####", ".....", "#####"},                     // eject
    };
    return glyphs[std::clamp(id, 0, int(glyphs.size()) - 1)];
}

// Glass as in the skin: a light edge on top, an upper half that darkens
// downwards, a sharp step to dark in the middle and a lower half that
// brightens again. Values are grey levels; 'shift' brightens (hover).
struct Glass
{
    int top, upperFrom, upperTo, lowerFrom, lowerTo;
};
constexpr Glass kTileGlass{104, 61, 46, 27, 49};  // opaque button tiles
constexpr Glass kBarGlass{81, 48, 37, 18, 39};    // the row behind the buttons

QColor grey(int v, int alpha = 255)
{
    v = std::clamp(v, 0, 255);
    return QColor(v, v, v, alpha);
}

void paintGlass(QPainter &p, const QRect &r, const Glass &g, int shift = 0, int alpha = 255)
{
    const int h = r.height();
    const int mid = std::max(2, int(std::lround(h * 0.545))); // row 12 of 22
    for (int y = 0; y < h; ++y) {
        int v;
        if (y == 0) {
            v = g.top;
        } else if (y < mid) {
            v = g.upperFrom + (g.upperTo - g.upperFrom) * (y - 1) / std::max(1, mid - 2);
        } else {
            v = g.lowerFrom + (g.lowerTo - g.lowerFrom) * (y - mid) / std::max(1, h - mid - 1);
        }
        p.fillRect(QRect(r.left(), r.top() + y, r.width(), 1), grey(v + shift, alpha));
    }
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
    const int w = width(), h = height();
    m_content = rect().adjusted(kFrame, kFrame, -kFrame, -kFrame);
    m_progressRect = QRect(kFrame, kSeekY, w - 2 * kFrame, kSeekH);
    m_barRect = QRect(kFrame, h - kFrame - kBarH, w - 2 * kFrame, kBarH);
    const QRect panel(kFrame, kPanelY, w - 2 * kFrame, m_barRect.top() - kPanelY);

    // The spectrum sits above the buttons, in the middle like in the screenshot.
    const int specWidth = int(panel.width() * 0.66) / kBarStep * kBarStep;
    m_spectrumRect = QRect(panel.left() + int(panel.width() * 0.2), m_barRect.top() - kSpectrumH - 1, specWidth,
                           kSpectrumH / kSegment * kSegment);

    // Cover 175px at (45,30) of the panel, text from x=240; smaller in a small window.
    const int coverSide = std::clamp(m_spectrumRect.top() - 2 - (panel.top() + 30), 64, 175);
    m_coverRect = QRect(panel.left() + 45, panel.top() + 30, coverSide, coverSide);
    m_infoRect = QRect(panel.left() + std::max(240, m_coverRect.right() + 20 - panel.left()), panel.top() + 18,
                       panel.right() - 20 - (panel.left() + 240), m_spectrumRect.top() - panel.top() - 18);
    m_listRect = panel.adjusted(0, 2, 0, -2);
    m_coverScaled = QImage();

    // Button row: the same positions and sizes as the original skin.
    m_buttons.clear();
    const int y = m_barRect.top();
    const QList<QPair<Button, QString>> glyphs = {
        {Prev, tr("Previous")}, {PlayPause, tr("Play / pause")}, {Next, tr("Next")},
        {Stop, tr("Stop")},     {Eject, tr("Open folder")},
    };
    int x = kFrame;
    for (const auto &g : glyphs) {
        m_buttons.append({g.first, QRect(x, y, 23, kBarH), QString(), g.second});
        x += 23;
    }
    m_buttons.append({Info, QRect(x, y, 44, kBarH), QStringLiteral("INFO"), tr("Info")});
    x += 44;
    m_buttons.append({Playlist, QRect(x, y, 44, kBarH), QStringLiteral("PLAYLIST"), tr("Playlist")});
    // The alien cat is a bit bigger than the row and sticks out above it.
    m_buttons.append({Menu, QRect(w - 36, y - 9, 30, 29), QString(), tr("Menu")});
    m_buttons.append({Minimize, QRect(w - 80, y, 37, kBarH), QStringLiteral("MINIMIZE"), QString()});
    m_buttons.append({Close, QRect(w - 120, y, 31, kBarH), QStringLiteral("CLOSE"), QString()});
    const int vw = PixelFont::textWidth(QStringLiteral("100%")) + 12;
    m_buttons.append({Volume, QRect(w - 120 - 8 - vw, y, vw, kBarH), QString(),
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
    // Like the skin: a dark outer line, a light inner one, the picture inside.
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), QColor(0x22, 0x22, 0x22));
    p.setPen(QColor(0x0c, 0x0c, 0x0c));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.setPen(QColor(0x9a, 0x9a, 0x9a));
    p.drawRect(rect().adjusted(2, 2, -3, -3));
    p.setPen(QColor(0x50, 0x50, 0x50));
    p.drawRect(rect().adjusted(4, 4, -5, -5));
    p.drawImage(m_content.topLeft(), m_background);
    p.setPen(QColor(0, 0, 0, 170));
    p.drawRect(m_content.adjusted(0, 0, -1, -1));
}

void SkinView::paintProgress(QPainter &p)
{
    // A glass strip: dark edges, light lines, glossy upper and darker lower half.
    // The played part is the same glass in light grey and less transparent.
    const QRect r = m_progressRect;
    static const int track[10] = {0, 98, 45, 45, 45, 12, 12, 12, 98, 0};
    static const int played[10] = {60, 186, 168, 168, 168, 158, 158, 158, 186, 60};
    const int fillW = m_progress >= 0 ? int(r.width() * std::clamp(m_progress, 0.0, 1.0)) : 0;
    for (int y = 0; y < r.height(); ++y) {
        const int row = std::min(9, y * 10 / r.height());
        if (fillW > 0)
            p.fillRect(QRect(r.left(), r.top() + y, fillW, 1), grey(played[row], 190));
        p.fillRect(QRect(r.left() + fillW, r.top() + y, r.width() - fillW, 1), grey(track[row], 128));
    }

    if (!m_message.isEmpty()) {
        const QFont f = pixelFont(10);
        const int w = QFontMetrics(f).horizontalAdvance(m_message) + 4;
        const int x = std::max(m_content.left() + 12, m_content.right() - 12 - w);
        shadowText(p, x, kPanelY + 14, m_content.right() - 12 - x, m_message, f, QColor(255, 255, 255, 220));
    }
}

void SkinView::paintInfo(QPainter &p)
{
    if (m_page != InfoPage) {
        // A faint shade behind the playlist, so the text stays readable on the streaks.
        p.fillRect(m_listRect, QColor(0, 0, 0, 70));
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);
    const QRect c = m_coverRect;

    // The picture hangs on a string from a nail (a 65x24 triangle).
    const QPointF nail(c.center().x() + 0.5, c.top() - 24);
    p.setPen(QPen(QColor(10, 10, 10, 220), 1.2));
    p.drawLine(nail, QPointF(c.center().x() - 32, c.top()));
    p.drawLine(nail, QPointF(c.center().x() + 33, c.top()));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(15, 15, 15));
    p.drawEllipse(nail, 2.2, 2.2);

    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(c.adjusted(3, 3, 5, 5), QColor(0, 0, 0, 70)); // shadow
    p.fillRect(c.adjusted(1, 1, 3, 3), QColor(0, 0, 0, 90));
    if (!m_info.cover.isNull()) {
        if (m_coverScaled.size() != c.size()) {
            // Fill the square, cropping a non-square picture in the middle.
            m_coverScaled = m_info.cover.scaled(c.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            m_coverScaled = m_coverScaled.copy((m_coverScaled.width() - c.width()) / 2,
                                               (m_coverScaled.height() - c.height()) / 2, c.width(), c.height());
        }
        p.drawImage(c.topLeft(), m_coverScaled);
    } else {
        // No picture: the grey gradient of the skin's empty cover.
        QLinearGradient g(c.topLeft(), c.bottomLeft());
        g.setColorAt(0, QColor(0xa8, 0xa8, 0xa8, 235));
        g.setColorAt(1, QColor(0x3c, 0x3c, 0x3c, 240));
        p.fillRect(c, g);
        p.setRenderHint(QPainter::Antialiasing);
        p.setFont(pixelFont(c.height() / 2));
        p.setPen(QColor(255, 255, 255, 70));
        p.drawText(c, Qt::AlignCenter, QStringLiteral("♫"));
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    // Light 1px border of the cover frame, a dark line outside.
    p.setBrush(Qt::NoBrush);
    p.setPen(QColor(0, 0, 0, 160));
    p.drawRect(c.adjusted(-2, -2, 1, 1));
    p.setPen(QColor(235, 235, 235, 220));
    p.drawRect(c.adjusted(-1, -1, 0, 0));

    // Text: album 12pt, artist and title 10pt, details 8pt (as in the skin).
    p.setRenderHint(QPainter::TextAntialiasing);
    const int x = m_infoRect.left();
    const int w = m_infoRect.width();
    const int top = kPanelY;
    shadowText(p, x, top + 39, w, m_info.album, pixelFont(16), kAlbumColor);
    shadowText(p, x, top + 58, w, m_info.artist, pixelFont(13), kTextColor);
    shadowText(p, x, top + 78, w, m_info.title, pixelFont(13, false, true), kTextColor);
    int y = top + 117;
    for (const QString &line : m_info.details) {
        if (y > m_spectrumRect.top() - 2)
            break;
        if (!line.isEmpty())
            shadowText(p, x, y, w, line, pixelFont(11), kTextColor);
        y += line.isEmpty() ? 6 : 17;
    }
}

void SkinView::paintSpectrum(QPainter &p)
{
    if (m_page != InfoPage)
        return; // like the skin: the analyser belongs to the info page
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
    int index = 0;
    switch (id) {
    case Prev: index = 0; break;
    case PlayPause: index = m_playing ? 2 : 1; break;
    case Next: index = 3; break;
    case Stop: index = 4; break;
    case Eject: index = 5; break;
    default: return;
    }
    const QStringList &rows = glyphRows(index);
    const int gw = rows.first().size();
    const int x0 = r.left() + (r.width() - gw) / 2 - 1;
    const int y0 = r.top() + 9; // the skin puts its glyphs on rows 9..13
    for (int row = 0; row < rows.size(); ++row) {
        QColor shade = c;
        if (row >= 3)
            shade.setAlpha(c.alpha() * 7 / 10); // lower rows a bit lighter, like the originals
        for (int col = 0; col < gw; ++col)
            if (rows[row][col] == QLatin1Char('#'))
                p.fillRect(x0 + col, y0 + row, 1, 1, shade);
    }
}

void SkinView::paintAlien(QPainter &p, const QRect &r, bool hover)
{
    // The "alien cat", designed on the skin's 25x22 grid and scaled to 'r':
    // pointed ears, a wide head and big eyes slanting down to the nose.
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(r.topLeft());
    p.scale(r.width() / 25.0, r.height() / 22.0);

    QPainterPath head;
    head.moveTo(6.5, 0.3);                                // left ear tip
    head.cubicTo(8.0, 1.8, 9.2, 3.6, 9.8, 5.2);           // inner side of the ear
    head.quadTo(12.5, 4.4, 15.2, 5.2);                    // top of the head between the ears
    head.cubicTo(15.8, 3.6, 17.0, 1.8, 18.5, 0.3);        // right ear tip
    head.cubicTo(19.8, 3.0, 20.8, 5.6, 21.6, 8.0);        // outer side of the right ear
    head.cubicTo(23.4, 10.5, 24.2, 14.5, 23.2, 17.6);     // right cheek
    head.cubicTo(21.6, 20.6, 16.5, 22.0, 12.5, 22.0);     // chin
    head.cubicTo(8.5, 22.0, 3.4, 20.6, 1.8, 17.6);
    head.cubicTo(0.8, 14.5, 1.6, 10.5, 3.4, 8.0);         // left cheek
    head.cubicTo(4.2, 5.6, 5.2, 3.0, 6.5, 0.3);
    QLinearGradient g(0, 0, 0, 22);
    const int a = hover ? 140 : 235; // like the original, it fades when hovered
    g.setColorAt(0.0, grey(150, a));
    g.setColorAt(0.5, grey(118, a));
    g.setColorAt(1.0, grey(92, a));
    p.setPen(QPen(grey(20, hover ? 110 : 190), 0.7));
    p.setBrush(g);
    p.drawPath(head);

    // A soft shine on the forehead.
    QRadialGradient shine(12.5, 8.5, 8.0);
    shine.setColorAt(0, QColor(255, 255, 255, hover ? 40 : 60));
    shine.setColorAt(1, QColor(255, 255, 255, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(shine);
    p.drawPath(head);

    // Eyes: big almonds from the outer top to the nose.
    p.setBrush(grey(242, hover ? 160 : 245));
    for (int side : {-1, 1}) {
        auto pt = [side](double x, double y) { return QPointF(12.5 + side * (12.5 - x), y); };
        QPainterPath eye;
        eye.moveTo(pt(3.2, 12.2));
        eye.cubicTo(pt(6.5, 11.8), pt(10.8, 14.6), pt(11.4, 19.0));
        eye.cubicTo(pt(8.0, 19.4), pt(3.4, 16.4), pt(3.2, 12.2));
        p.drawPath(eye);
    }
    // Nose / mouth.
    p.setBrush(grey(235, hover ? 120 : 200));
    p.drawEllipse(QPointF(12.5, 20.4), 0.9, 0.7);
    p.restore();
}

void SkinView::paintBar(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing, false);
    // The row itself is glass too, slightly see-through.
    paintGlass(p, m_barRect, kBarGlass, 0, 235);

    for (const ButtonRect &b : m_buttons) {
        const bool hover = b.id == m_hover;
        const bool active = (b.id == Info && m_page == InfoPage) || (b.id == Playlist && m_page == PlaylistPage);
        if (b.id == Menu)
            continue; // drawn last, it sticks out of the row
        const bool tile = b.id != Close && b.id != Minimize && b.id != Volume;
        if (tile) {
            // Glass tile with a light line on the left and a dark one on the right.
            paintGlass(p, b.rect, kTileGlass, hover ? 26 : active ? 10 : 0);
            p.fillRect(QRect(b.rect.left(), b.rect.top() + 1, 1, b.rect.height() - 1), grey(hover ? 96 : 70, 200));
            p.fillRect(QRect(b.rect.right(), b.rect.top() + 1, 1, b.rect.height() - 1), grey(22, 220));
        }
        // Labels on the tiles are grey, the window buttons white; hover lights them up.
        QColor color = tile ? grey(hover || active ? 235 : 178) : grey(hover ? 255 : 228);
        if (b.id == Volume) {
            const QString text = QStringLiteral("%1%").arg(m_volume);
            PixelFont::draw(p, b.rect.right() - PixelFont::textWidth(text), b.rect.top() + 9, text, color);
            continue;
        }
        if (b.text.isEmpty()) {
            paintGlyph(p, b.id, b.rect, tile ? grey(hover ? 235 : 150) : color);
            continue;
        }
        const int tw = PixelFont::textWidth(b.text);
        PixelFont::draw(p, b.rect.left() + (b.rect.width() - tw) / 2, b.rect.top() + 9, b.text, color);
    }

    // Playing time, left of the volume.
    if (!m_time.isEmpty()) {
        QRect volume;
        for (const ButtonRect &b : m_buttons)
            if (b.id == Volume)
                volume = b.rect;
        const int tw = PixelFont::textWidth(m_time);
        PixelFont::draw(p, volume.left() - tw - 6, m_barRect.top() + 9, m_time, grey(200));
    }

    for (const ButtonRect &b : m_buttons)
        if (b.id == Menu)
            paintAlien(p, b.rect, b.id == m_hover);
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
