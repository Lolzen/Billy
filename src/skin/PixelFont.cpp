#include "skin/PixelFont.h"

#include <QHash>
#include <QPainter>
#include <QStringList>

namespace PixelFont {

namespace {

// Five rows per glyph, '#' is a set pixel. All rows of a glyph have the same width.
const QHash<QChar, QStringList> &glyphs()
{
    static const QHash<QChar, QStringList> table = [] {
        QHash<QChar, QStringList> t;
        auto add = [&t](char c, std::initializer_list<const char *> rows) {
            QStringList list;
            for (const char *r : rows)
                list << QString::fromLatin1(r);
            t.insert(QLatin1Char(c), list);
        };
        add('A', {".##.", "#..#", "####", "#..#", "#..#"});
        add('B', {"###.", "#..#", "###.", "#..#", "###."});
        add('C', {".##.", "#..#", "#...", "#..#", ".##."});
        add('D', {"###.", "#..#", "#..#", "#..#", "###."});
        add('E', {"####", "#...", "###.", "#...", "####"});
        add('F', {"####", "#...", "###.", "#...", "#..."});
        add('G', {".###", "#...", "#.##", "#..#", ".###"});
        add('H', {"#..#", "#..#", "####", "#..#", "#..#"});
        add('I', {"#", "#", "#", "#", "#"});
        add('J', {"...#", "...#", "...#", "#..#", ".##."});
        add('K', {"#..#", "#.#.", "##..", "#.#.", "#..#"});
        add('L', {"#..", "#..", "#..", "#..", "###"});
        add('M', {"#...#", "##.##", "#.#.#", "#...#", "#...#"});
        add('N', {"#..#", "##.#", "#.##", "#..#", "#..#"});
        add('O', {".##.", "#..#", "#..#", "#..#", ".##."});
        add('P', {"###.", "#..#", "###.", "#...", "#..."});
        add('Q', {".##.", "#..#", "#..#", "#.#.", ".#.#"});
        add('R', {"###.", "#..#", "###.", "#.#.", "#..#"});
        add('S', {".###", "#...", ".##.", "...#", "###."});
        add('T', {"###", ".#.", ".#.", ".#.", ".#."});
        add('U', {"#..#", "#..#", "#..#", "#..#", ".##."});
        add('V', {"#...#", "#...#", ".#.#.", ".#.#.", "..#.."});
        add('W', {"#...#", "#...#", "#.#.#", "##.##", "#...#"});
        add('X', {"#..#", "#..#", ".##.", "#..#", "#..#"});
        add('Y', {"#.#", "#.#", ".#.", ".#.", ".#."});
        add('Z', {"####", "..#.", ".#..", "#...", "####"});
        add('0', {".##.", "#..#", "#..#", "#..#", ".##."});
        add('1', {".#", "##", ".#", ".#", ".#"});
        add('2', {"###.", "...#", ".##.", "#...", "####"});
        add('3', {"###.", "...#", ".##.", "...#", "###."});
        add('4', {"#..#", "#..#", "####", "...#", "...#"});
        add('5', {"####", "#...", "###.", "...#", "###."});
        add('6', {".##.", "#...", "###.", "#..#", ".##."});
        add('7', {"####", "...#", "..#.", ".#..", ".#.."});
        add('8', {".##.", "#..#", ".##.", "#..#", ".##."});
        add('9', {".##.", "#..#", ".###", "...#", ".##."});
        add(':', {".", "#", ".", "#", "."});
        add('/', {"..#", "..#", ".#.", "#..", "#.."});
        add('%', {"#..#", "..#.", ".#..", "#...", "#..#"});
        add('-', {"...", "...", "###", "...", "..."});
        add('.', {".", ".", ".", ".", "#"});
        add(' ', {"..", "..", "..", "..", ".."});
        return t;
    }();
    return table;
}

} // namespace

int textWidth(const QString &text, int scale)
{
    int w = 0;
    for (const QChar c : text) {
        const auto it = glyphs().constFind(c.toUpper());
        w += (it == glyphs().constEnd() ? 2 : it->first().size()) + 1;
    }
    return qMax(0, w - 1) * scale;
}

void draw(QPainter &p, int x, int y, const QString &text, const QColor &color, int scale)
{
    for (const QChar c : text) {
        const auto it = glyphs().constFind(c.toUpper());
        if (it == glyphs().constEnd()) {
            x += 3 * scale;
            continue;
        }
        const QStringList &rows = *it;
        for (int r = 0; r < rows.size(); ++r)
            for (int col = 0; col < rows[r].size(); ++col)
                if (rows[r][col] == QLatin1Char('#'))
                    p.fillRect(x + col * scale, y + r * scale, scale, scale, color);
        x += (rows.first().size() + 1) * scale;
    }
}

} // namespace PixelFont
