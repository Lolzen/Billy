#pragma once

#include <QStringList>
#include <QWidget>

// A flat, sunken bar: either a plain progress bar or a stereo LED meter.
class SheepProgress : public QWidget
{
    Q_OBJECT
public:
    enum BarType { Normal, StereoLeds };

    explicit SheepProgress(BarType type, Qt::Orientation orientation, QWidget *parent = nullptr);

    void setMaximum(int max);
    int maximum() const { return m_max; }
    void setValue(double value);             // Normal bar
    void setLevels(int left, int right);     // LED meter: number of lit LEDs
    void setLedGeometry(int ledWidth, int ledSeparation, int stereoSeparation);
    QSize sizeHint() const override;

signals:
    void mousePressed(double fraction, Qt::MouseButton button);
    void mouseDragged(double fraction);
    void mouseReleased(double fraction);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    double fractionAt(const QPoint &p) const;

    BarType m_type;
    Qt::Orientation m_orientation;
    int m_max = 100;
    double m_value = 0;
    int m_left = 0, m_right = 0;
    int m_ledWidth = 3, m_ledSeparation = 1, m_stereoSeparation = 2;
    bool m_dragging = false;
};

// Text scale (e.g. "-oo 30 20 12 8 4 0") under the 14 LED meter.
class MeterScale : public QWidget
{
    Q_OBJECT
public:
    explicit MeterScale(QWidget *parent = nullptr);
    // Labels with their LED index (0-based) out of 'leds' LEDs.
    void setScale(const QList<QPair<int, QString>> &labels, int leds);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QList<QPair<int, QString>> m_labels;
    int m_leds = 14;
};

// Converts a peak level to the number of lit LEDs on Billy's calibrated scales.
namespace LevelScale {
int leds6(double dB);   // 7 LEDs: signal, -60, -30, -15, -6, -3, -1 dB
int leds14(double dB);  // 14 LEDs: -oo, -60, -40, ... -2, 0 dB
}
