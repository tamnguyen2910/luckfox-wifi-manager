#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QByteArray>
#include <QProcess>
#include <QDebug>
#include <QFile>

#include "wifimanager.h"

int main(int argc, char *argv[])
{
    // Environment for embedded Qt (Luckfox Pico Ultra W, RV1106)
    // Based on the exact setup from the embedded-qt skill – must run BEFORE QGuiApplication.
    // Linuxfb scanout depends on explicit size; fonts may be missing; timezone is required.
    qputenv("QT_QPA_PLATFORM", QByteArray("linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480"));
    // Time zone matters for QDateTime and chrono-based features.
    qputenv("TZ", QByteArray("Asia/Ho_Chi_Minh"));
    // High‑DPI scaling: 1.0 disables Qt scaling on weak ARM; needed for readability.
    qputenv("QT_SCALE_FACTOR", QByteArray("1.0"));
    qputenv("QT_FONT_DPI", QByteArray("96"));
    // Font directory must exist on the target, otherwise Qt can't render.
    if (QFile("/usr/share/fonts/dejavu/").exists()) {
        qputenv("QT_QPA_FONTDIR", QByteArray("/usr/share/fonts/dejavu/"));
    } else {
        qWarning() << "[Main] Font dir /usr/share/fonts/dejavu/ not found, using default.";
    }
    // High‑DPI scaling attribute (enforced by Qt for embedded devices).
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);

    QGuiApplication app(argc, argv);

    // Register C++ backend to QML
    WifiManager wifiManager;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("wifiManager", &wifiManager);

    // Load QML from resource
    const QUrl url(QStringLiteral("qrc:/qml/main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);

    engine.load(url);

    return app.exec();
}