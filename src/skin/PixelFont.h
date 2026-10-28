#pragma once

#include <QColor>
#include <QString>

class QPainter;

// A tiny 5 pixel high bitmap font for the labels of the skin ("INFO",
// "PLAYLIST", "CLOSE", the playing time...). Upper case letters, digits and
// a few signs. 'scale' enlarges every pixel (for big screens).
namespace PixelFont {

int textWidth(const QString &text, int scale = 1);
constexpr int height(int scale = 1) { return 5 * scale; }
// Draws 'text' with its top left corner at (x, y).
void draw(QPainter &p, int x, int y, const QString &text, const QColor &color, int scale = 1);

} // namespace PixelFont
