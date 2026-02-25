QT       += core gui printsupport network websockets

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11

TARGET = dest_double
TEMPLATE = app

# 强制启用控制台输出
CONFIG += console

DEFINES += QT_DEPRECATED_WARNINGS

# 定义源文件目录
INCLUDEPATH += . \
               core \
               network \
               processing \
               protocol \
               ui \
               third_party

# 核心模块（独立可复用）
HEADERS += \
    core/channelseparator.h \
    core/reorderbuffer.h \
    core/ringbuffer.h \

SOURCES += \
    core/channelseparator.cpp \
    core/reorderbuffer.cpp \

# 网络模块
HEADERS += \
    network/websocketclient.h \
    network/udpclientreceiver.h \
    network/packetqueue.h \
    network/packetworker.h \
    network/udpreceiverworker.h

SOURCES += \
    network/websocketclient.cpp \
    network/udpclientreceiver.cpp \
    network/packetqueue.cpp \
    network/packetworker.cpp \
    network/udpreceiverworker.cpp

# 处理模块
HEADERS += \
    processing/fftprocessor.h \
    processing/fftworker.h \
    processing/datareader.h \
    processing/localfilereader.h \
    processing/plotworker.h \
    processing/timefileparser.h \
    processing/timestamploader.h

SOURCES += \
    processing/fftprocessor.cpp \
    processing/fftworker.cpp \
    processing/datareader.cpp \
    processing/localfilereader.cpp \
    processing/plotworker.cpp \
    processing/timefileparser.cpp \
    processing/timestamploader.cpp

# 协议模块
HEADERS += \
    protocol/packetformat.h \
    protocol/framereassembler.h

SOURCES += \
    protocol/framereassembler.cpp

# UI模块
HEADERS += \
    ui/mainwindow.h \
    ui/vlfsensorwidget.h \
    ui/infopanelwidget.h \
    ui/spectrumwidget.h \
    ui/spectrogramwidget.h

SOURCES += \
    ui/mainwindow.cpp \
    ui/vlfsensorwidget.cpp \
    ui/infopanelwidget.cpp \
    ui/spectrumwidget.cpp \
    ui/spectrogramwidget.cpp

# 第三方库
HEADERS += \
    third_party/qcustomplot.h

SOURCES += \
    third_party/qcustomplot.cpp

# 主程序
SOURCES += main.cpp

# 保留的旧文件（暂不删除）
SOURCES += udpserversender.cpp
HEADERS += udpserversender.h

# FFTW库配置
INCLUDEPATH += $$PWD/fftw-3.3.5-dll64
LIBS += -L$$PWD/fftw-3.3.5-dll64 -lfftw3-3

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
