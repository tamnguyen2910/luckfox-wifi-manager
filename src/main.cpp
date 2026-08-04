#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QByteArray>
#include <QProcess>
#include <QDebug>

#include "wifimanager.h"

int main(int argc, char *argv[])
{
    // MUST set env vars BEFORE QGuiApplication construction (embedded-qt pattern)
    qputenv("QT_QPA_PLATFORM", QByteArray("linuxfb:fb=/dev/fb0:size=800x480:mmsize=800x480"));
    qputenv("QT_QPA_FONTDIR", QByteArray("/usr/share/fonts/dejavu/"));
    qputenv("QT_SCALE_FACTOR", QByteArray("1.0"));
    qputenv("QT_FONT_DPI", QByteArray("96"));

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