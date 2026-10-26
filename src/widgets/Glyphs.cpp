#include "widgets/Glyphs.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace Glyphs {

namespace {

QPixmap render(Glyph glyph, const QColor &color, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 16.0, size / 16.0);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    auto triangle = [&p](double x, double dir) {
        // dir > 0: pointing right
        QPainterPath path;
        if (dir > 0) {
            path.moveTo(x, 3);
            path.lineTo(x + 5, 8);
            path.lineTo(x, 13);
        } else {
            path.moveTo(x + 5, 3);
            path.lineTo(x, 8);
            path.lineTo(x + 5, 13);
        }
        path.closeSubpath();
        p.drawPath(path);
    };

    switch (glyph) {
    case Play:
        triangle(6, 1);
        break;
    case Pause:
        p.drawRect(QRectF(4, 4, 3, 8));
        p.drawRect(QRectF(9, 4, 3, 8));
        break;
    case Stop:
        p.drawRect(QRectF(4, 4, 8, 8));
        break;
    case Previous:
        p.drawRect(QRectF(4, 3, 2, 10));
        triangle(7, -1);
        break;
    case Next:
        triangle(4, 1);
        p.drawRect(QRectF(10, 3, 2, 10));
        break;
    }
    return pm;
}

} // namespace

QIcon icon(Glyph glyph, const QColor &color)
{
    QIcon icon;
    for (int size : {16, 24, 32, 48})
        icon.addPixmap(render(glyph, color, size));
    QColor disabled = color;
    disabled.setAlphaF(0.4);
    for (int size : {16, 24, 32})
        icon.addPixmap(render(glyph, disabled, size), QIcon::Disabled);
    return icon;
}

QIcon appIcon()
{
    QIcon icon(QStringLiteral(":/billy.svg"));
    return icon;
}

} // namespace Glyphs
