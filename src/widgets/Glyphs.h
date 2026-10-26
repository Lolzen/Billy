#pragma once

#include <QColor>
#include <QIcon>

// Small vector glyphs for the play bar and the tray icon.
namespace Glyphs {

enum Glyph { Play, Pause, Stop, Previous, Next };

QIcon icon(Glyph glyph, const QColor &color);
QIcon appIcon();

} // namespace Glyphs
