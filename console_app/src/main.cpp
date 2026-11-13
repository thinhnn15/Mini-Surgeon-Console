#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QTimer>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Mini Surgeon Console"));
    QGuiApplication::setApplicationVersion(QStringLiteral(MSC_VERSION));

    const bool smokeTest = QCoreApplication::arguments().contains(QStringLiteral("--smoke"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
        Qt::QueuedConnection);
    engine.loadFromModule("MiniConsole", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    if (smokeTest) {
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
    }
    return QGuiApplication::exec();
}
