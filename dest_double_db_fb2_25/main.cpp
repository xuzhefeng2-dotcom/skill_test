#include "mainwindow.h"
#include <QApplication>
#include <QtGlobal>

// ✅ 禁用所有 qDebug/qWarning 输出的消息处理器
void noOutputMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(type)
    Q_UNUSED(context)
    Q_UNUSED(msg)
    // 什么都不做，吞掉所有消息
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ✅ 安装消息处理器以禁用控制台输出
    // qInstallMessageHandler(noOutputMessageHandler);  // 已注释：重新启用控制台输出

    MainWindow window;
    window.show();

    return app.exec();
}
