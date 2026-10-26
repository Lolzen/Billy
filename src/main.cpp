#include "MainWindow.h"
#include "Settings.h"
#include "SingleInstance.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Billy"));
    QApplication::setApplicationVersion(QStringLiteral(BILLY_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("billy"));
    QApplication::setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Billy - a no nonsense, gapless audio player"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("files"), QStringLiteral("Music files, folders or playlists to open."),
                                 QStringLiteral("[files...]"));
    parser.process(app);

    // Make relative paths absolute before they are sent to another process.
    QStringList files;
    for (const QString &f : parser.positionalArguments())
        files << (f.contains(QLatin1String("://")) ? f : QDir::current().absoluteFilePath(f));

    Settings settings;
    settings.load();
    SingleInstance instance;
    if (!settings.multipleInstances) {
        if (instance.sendToRunning(files))
            return 0;
        instance.listen();
    }

    MainWindow window;
    QObject::connect(&instance, &SingleInstance::messageReceived, &window,
                     [&window](const QStringList &args) {
                         if (args.isEmpty())
                             window.mprisRaise();
                         else
                             window.openPaths(args, true);
                     });
    window.restoreSession();
    window.show();
    if (!files.isEmpty())
        window.openPaths(files, false);
    return app.exec();
}
