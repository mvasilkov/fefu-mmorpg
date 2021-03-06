#-------------------------------------------------
#
# Project created by QtCreator 2012-03-05T10:38:43
#
#-------------------------------------------------

QT += network

QT -= gui

DESTDIR = ../lib

TARGET = QtWebsocket
TEMPLATE = lib
CONFIG += staticlib

CONFIG(debug, debug|release) {
    win32: TARGET = $$join(TARGET,,,d)
}

SOURCES += \
    QWsServer.cpp \
    QWsSocket.cpp \
    QWsHandshake.cpp \
    QWsFrame.cpp \
    QTlsServer.cpp \
    functions.cpp

HEADERS += \
    QWsServer.h \
    QWsSocket.h \
    QWsHandshake.h \
    QWsFrame.h \
    QTlsServer.h \
    functions.h \
    WsEnums.h
