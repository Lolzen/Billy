#include "dialogs/InfoDialog.h"

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QTextBrowser>
#include <QVBoxLayout>

InfoDialog::InfoDialog(const QString &title, const QString &text, QWidget *parent, bool html)
    : QDialog(parent)
{
    setWindowTitle(title);
    resize(480, 460);
    auto *view = new QTextBrowser(this);
    view->setOpenExternalLinks(true);
    if (html) {
        view->setHtml(text);
    } else {
        view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        view->setPlainText(text);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(view, 1);
    layout->addWidget(buttons);
}

QString InfoDialog::shortcutsText()
{
    return tr(
        "BASICS\n"
        "  Play selected file              Enter, Numpad Enter\n"
        "  Pause / unpause                 Space, Numpad 0\n"
        "  Stop                            2x Numpad 0, Numpad 0 while paused\n"
        "  Next / previous track           Tab / Shift+Tab, Numpad 6 / 4\n"
        "  Rewind to start of track        Backspace\n"
        "  Toggle play mode                F9, Numpad 8, click on statusbar\n"
        "  Play modes                      Alt+N repeat list, Alt+R repeat file,\n"
        "                                  Alt+L play list, Alt+F play file, Alt+S shuffle\n"
        "  Exit                            Esc\n"
        "\n"
        "PLAYLIST\n"
        "  Open folder                     F4, Numpad *\n"
        "  Add folder                      F3, Numpad /\n"
        "  Add file(s)                     Ctrl+L\n"
        "  Add internet radio URL          Ctrl+R\n"
        "  Open playlist                   Ctrl+O\n"
        "  Save playlist                   Ctrl+S\n"
        "  Clear list                      Ctrl+N\n"
        "  Queue / unqueue                 Insert, Numpad 9\n"
        "  Remove from list                Del, Numpad .\n"
        "  Move file to trash              Shift+Del\n"
        "  Crop selected or queued         Ctrl+Del\n"
        "  Remove missing files            Alt+Del\n"
        "  Remove duplicates               Ctrl+Alt+D\n"
        "  Randomize list                  Ctrl+Alt+S\n"
        "  Move items                      Ctrl+Up/Down, drag items\n"
        "  Select all                      Ctrl+A\n"
        "  Selection up / down             Numpad 8 / 2 (with NumLock off: arrows)\n"
        "  Cut / copy / paste entries      Ctrl+X / Ctrl+C / Ctrl+V\n"
        "  Find                            just type, Ctrl+F\n"
        "  Jump to playing file            ~ (key left of 1)\n"
        "  Refresh (new/removed files)     F5\n"
        "  Toggle view style               F8\n"
        "\n"
        "EXTRA\n"
        "  Shortcuts                       F1\n"
        "  Settings                        F6\n"
        "  Rename file / file info         F2\n"
        "  Multiple filename renamer       F2 on multiple selection\n"
        "  Show folder in file manager     Ctrl+E\n"
        "  Open configuration folder       Ctrl+Alt+E\n"
        "  Add playlist to favorites       Ctrl+D\n"
        "  Organize favorites              Ctrl+B\n"
        "  Open favorite 1..10             Ctrl+1 .. Ctrl+0\n"
        "  Playing time of selection       Ctrl+Alt+T\n"
        "  Sleep timer                     Alt+T\n"
        "  Copy selection to MP3 player    Ctrl+M\n"
        "  Clear shuffle/queue history     Ctrl+Shift+C\n"
        "  Minimize to tray                F11\n"
        "  Maximize / restore              F12\n"
        "\n"
        "SOUND\n"
        "  Volume                          Numpad + / -, drag the % sign up/down\n"
        "  Soft mute                       Pause key\n"
        "  Seek 4 sec                      Left / right, Numpad 1 / 3\n"
        "  Seek fine (0.1 sec)             Shift + left / right\n"
        "  Seek big (60 sec)               Ctrl + left / right\n"
        "  Toggle level meters             Ctrl+Shift+L\n"
        "\n"
        "Media keys work system wide through MPRIS.\n");
}

QString InfoDialog::aboutHtml()
{
    return tr("<h2>Billy for Linux %1</h2>"
              "<p>A no nonsense audio player that quickly plays an entire directory of music files, "
              "gapless, and fully controllable by keyboard.</p>"
              "<p>This is an independent re-implementation for Linux, written from scratch in C++/Qt, "
              "modelled after the Windows freeware player <b>Billy</b> (2004&ndash;2008) "
              "by Sanne Schaap / Sheep Productions. It contains no code or artwork of the original.</p>"
              "<p><b>Credits of the original Billy</b><br>"
              "Code: Sanne Schaap<br>Artwork, concept and testing: Emiel de Jager<br>"
              "Level meters algorithm &amp; calibration: Gert-Jan Wiersema<br>"
              "Support &amp; code portions: Laurenz van Gaalen</p>"
              "<p><b>This version uses</b><br>"
              "Qt %2 and FFmpeg (libavformat, libavcodec, libswresample) for decoding, "
              "resampling and internet radio</p>")
        .arg(QStringLiteral(BILLY_VERSION), QString::fromLatin1(qVersion()));
}
