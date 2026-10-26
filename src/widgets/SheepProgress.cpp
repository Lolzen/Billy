#include "widgets/SheepProgress.h"

#include <QMouseEvent>
#include <QPainter>

#include <cmath>

SheepProgress::SheepProgress(BarType type, Qt::Orientation orientation, QWidget *parent)
    : QWidget(parent), m_type(type), m_orientation(orientation)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void SheepProgress::setMaximum(int max)
{
    m_max = qMax(1, max);
    update();
}

void SheepProgress::setValue(double value)
{
    value = qBound(0.0, value, double(m_max));
    if (qFuzzyCompare(value + 1, m_value + 1))
        return;
    m_value = value;
    update();
}

void SheepProgress::setLevels(int left, int right)
{
    left = qBound(0, left, m_max);
    right = qBound(0, right, m_max);
    if (left == m_left && right == m_right)
        return;
    m_left = left;
    m_right = right;
    update();
}

void SheepProgress::setLedGeometry(int ledWidth, int ledSeparation, int stereoSeparation)
{
    m_ledWidth = ledWidth;
    m_ledSeparation = ledSeparation;
    m_stereoSeparation = stereoSeparation;
    updateGeometry();
    update();
}

QSize SheepProgress::sizeHint() const
{
    if (m_type == Normal)
        return m_orientation == Qt::Horizontal ? QSize(120, 16) : QSize(16, 37);
    const int len = 2 + m_max * (m_ledWidth + m_ledSeparation) - m_ledSeparation + 2;
    return m_orientation == Qt::Horizontal ? QSize(len, 16) : QSize(16, len);
}

void SheepProgress::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette pal = palette();
    const QColor bg = pal.color(QPalette::Window);
    const QColor bar = pal.color(QPalette::Highlight);
    QRect r = rect();
    p.fillRect(r, bg);

    // Sunken 1px frame, like the original flat Windows look.
    p.setPen(pal.color(QPalette::Mid));
    p.drawLine(r.topLeft(), r.topRight());
    p.drawLine(r.topLeft(), r.bottomLeft());
    p.setPen(pal.color(QPalette::Light));
    p.drawLine(r.bottomLeft(), r.bottomRight());
    p.drawLine(r.topRight(), r.bottomRight());
    r.adjust(2, 2, -2, -2);
    if (r.width() <= 0 || r.height() <= 0)
        return;

    if (m_type == Normal) {
        const double f = m_value / m_max;
        QRect fill = r;
        if (m_orientation == Qt::Horizontal)
            fill.setWidth(int(std::round(r.width() * f)));
        else
            fill.setTop(r.bottom() - int(std::round(r.height() * f)) + 1);
        if (fill.width() > 0 && fill.height() > 0)
            p.fillRect(fill, bar);
        return;
    }

    // Stereo LEDs: left channel on top (or left), right channel below (or right).
    QColor off = bar;
    off.setAlphaF(0.18);
    const bool horizontal = m_orientation == Qt::Horizontal;
    const int across = horizontal ? r.height() : r.width();
    const int channelSize = (across - m_stereoSeparation) / 2;
    const int along = horizontal ? r.width() : r.height();
    const double step = double(along + m_ledSeparation) / m_max;
    for (int ch = 0; ch < 2; ++ch) {
        const int lit = ch == 0 ? m_left : m_right;
        const int c0 = ch * (channelSize + m_stereoSeparation);
        for (int i = 0; i < m_max; ++i) {
            const int a0 = int(std::round(i * step));
            const int a1 = int(std::round((i + 1) * step)) - m_ledSeparation;
            QRect led;
            if (horizontal)
                led = QRect(r.left() + a0, r.top() + c0, a1 - a0, channelSize);
            else
                led = QRect(r.left() + c0, r.bottom() - a1 + 1, channelSize, a1 - a0);
            p.fillRect(led, i < lit ? bar : off);
        }
    }
}

double SheepProgress::fractionAt(const QPoint &pos) const
{
    if (m_orientation == Qt::Horizontal)
        return qBound(0.0, double(pos.x() - 2) / qMax(1, width() - 4), 1.0);
    return qBound(0.0, 1.0 - double(pos.y() - 2) / qMax(1, height() - 4), 1.0);
}

void SheepProgress::mousePressEvent(QMouseEvent *e)
{
    m_dragging = e->button() == Qt::LeftButton;
    emit mousePressed(fractionAt(e->position().toPoint()), e->button());
}

void SheepProgress::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging)
        emit mouseDragged(fractionAt(e->position().toPoint()));
}

void SheepProgress::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        emit mouseReleased(fractionAt(e->position().toPoint()));
    }
}

// ------------------------------------------------------------- MeterScale

MeterScale::MeterScale(QWidget *parent) : QWidget(parent)
{
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    setFont(f);
}

void MeterScale::setScale(const QList<QPair<int, QString>> &labels, int leds)
{
    m_labels = labels;
    m_leds = qMax(1, leds);
    update();
}

QSize MeterScale::sizeHint() const
{
    return QSize(140, fontMetrics().height());
}

void MeterScale::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setPen(palette().color(QPalette::WindowText));
    const QFontMetrics fm = fontMetrics();
    const double step = double(width() - 4) / m_leds;
    for (const auto &label : m_labels) {
        const int w = fm.horizontalAdvance(label.second);
        int x = 2 + int(step * label.first + step / 2) - w / 2;
        x = qBound(0, x, width() - w);
        p.drawText(x, fm.ascent(), label.second);
    }
}

namespace LevelScale {

int leds6(double dB)
{
    static const double thresholds[] = {-90, -60, -30, -15, -6, -3, -1};
    int n = 0;
    for (double t : thresholds)
        if (dB >= t)
            ++n;
    return n;
}

int leds14(double dB)
{
    static const double thresholds[] = {-90, -60, -40, -30, -25, -20, -15, -12, -10, -8, -6, -4, -2, -0.05};
    int n = 0;
    for (double t : thresholds)
        if (dB >= t)
            ++n;
    return n;
}

} // namespace LevelScale
