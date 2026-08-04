QT += quick core
CONFIG += c++11

TARGET = wifi-manager

SOURCES += \
    src/main.cpp \
    src/wifimanager.cpp

HEADERS += \
    src/wifimanager.h

RESOURCES += qml.qrc