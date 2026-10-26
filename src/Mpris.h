#pragma once

#include <QObject>

class MainWindow;

// Registers Billy as org.mpris.MediaPlayer2.billy on the session bus, so media
// keys and desktop media controls work system wide (Billy's "global hotkeys").
class Mpris : public QObject
{
    Q_OBJECT
public:
    explicit Mpris(MainWindow *window);
    // Call when the playing track, play state or volume changed.
    void notifyChanged();

private:
    MainWindow *m_window;
    bool m_registered = false;
};
