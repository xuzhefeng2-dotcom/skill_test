#include "websocketclient.h"
#include <QDateTime>
#include <QDebug>

WebSocketClient::WebSocketClient(QObject *parent)
    : QObject(parent)
    , m_webSocket(nullptr)
    , m_heartbeatInterval(2000)      // 默认2秒
    , m_heartbeatTimeoutCount(3)     // 默认3次超时
    , m_missedHeartbeats(0)
{
    m_webSocket = new QWebSocket();

    connect(m_webSocket, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &WebSocketClient::onDisconnected);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &WebSocketClient::onTextMessageReceived);
    connect(m_webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &WebSocketClient::onError);

    // 创建心跳发送定时器
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &WebSocketClient::sendHeartbeat);

    // 创建心跳检测定时器
    m_heartbeatCheckTimer = new QTimer(this);
    connect(m_heartbeatCheckTimer, &QTimer::timeout, this, &WebSocketClient::checkHeartbeatTimeout);
}

WebSocketClient::~WebSocketClient()
{
    // 停止心跳定时器
    if (m_heartbeatTimer) {
        m_heartbeatTimer->stop();
    }
    if (m_heartbeatCheckTimer) {
        m_heartbeatCheckTimer->stop();
    }

    if (m_webSocket) {
        m_webSocket->close();
        m_webSocket->deleteLater();
    }
}

bool WebSocketClient::connectToServer(const QString &url)
{
    m_serverUrl = url;
    qDebug() << "[WebSocket] 正在连接到:" << url;
    m_webSocket->open(QUrl(url));
    return true;
}

// play.md协议命令实现
void WebSocketClient::sendCollectCommand(const QString& channel, double sampleRate)
{
    QJsonObject data;
    data["channel"] = channel;
    data["sample_rate"] = sampleRate;  // 添加采样率参数
    QJsonObject cmd = buildCommand("collect", "start collect", data);
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送COLLECT命令，通道:" << channel << "采样率:" << sampleRate;
}

void WebSocketClient::sendDisplayCommand(const QString& channel)
{
    QJsonObject data;
    data["channel"] = channel;
    QJsonObject cmd = buildCommand("display", "start display", data);
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送DISPLAY命令，通道:" << channel;
}

void WebSocketClient::sendStatusSubscribe(int cycle)
{
    if (cycle != 1 && cycle != 5 && cycle != 10) {
        qWarning() << "[WebSocket] 无效的订阅周期:" << cycle;
        return;
    }

    QJsonObject data;
    data["cycle"] = cycle;
    QJsonObject cmd = buildCommand("status_subscribe", "subscribe status", data);
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送STATUS_SUBSCRIBE命令，周期:" << cycle << "秒";
}

void WebSocketClient::sendStatusACK()
{
    QJsonObject data;
    data["type"] = "ack";
    data["code"] = 0;
    data["msg"] = "status updated";
    QJsonObject cmd = buildCommand("status", "status ack", data);
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送STATUS ACK";
}

void WebSocketClient::disconnectFromServer()
{
    // 停止心跳定时器
    if (m_heartbeatTimer) {
        m_heartbeatTimer->stop();
    }
    if (m_heartbeatCheckTimer) {
        m_heartbeatCheckTimer->stop();
    }

    if (m_webSocket && m_webSocket->isValid()) {
        m_webSocket->close();
    }
}

bool WebSocketClient::isConnected() const
{
    return m_webSocket && m_webSocket->isValid();
}

void WebSocketClient::sendStartCommand()
{
    QJsonObject cmd = buildCommand("start", "start");
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送START命令";
}

void WebSocketClient::sendDisplayCommand()
{
    QJsonObject cmd = buildCommand("display", "开始显示");
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送DISPLAY命令";
}

void WebSocketClient::sendStopCollectCommand()
{
    QJsonObject cmd = buildCommand("stop_collect", "stop collect");
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送STOP_COLLECT命令";
}

void WebSocketClient::sendStopDisplayCommand()
{
    QJsonObject cmd = buildCommand("stop_display", "stop display");
    sendMessage(cmd);
    qDebug() << "[WebSocket] 已发送STOP_DISPLAY命令";
}

QJsonObject WebSocketClient::buildCommand(const QString &cmd, const QString &description)
{
    QJsonObject message;
    message["from"] = "client";
    message["to"] = "server";
    message["cmd"] = cmd;
    message["dis"] = description;
    message["date"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    message["data"] = QJsonObject();
    return message;
}

QJsonObject WebSocketClient::buildCommand(const QString &cmd, const QString &description, const QJsonObject &data)
{
    QJsonObject message;
    message["from"] = "client";
    message["to"] = "server";
    message["cmd"] = cmd;
    message["dis"] = description;
    message["date"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    message["data"] = data;
    return message;
}

void WebSocketClient::sendMessage(const QJsonObject &message)
{
    if (!isConnected()) {
        emit error("未连接到服务器");
        return;
    }

    QJsonDocument doc(message);
    QString jsonString = doc.toJson(QJsonDocument::Compact);
    m_webSocket->sendTextMessage(jsonString);
}

void WebSocketClient::onConnected()
{
    qDebug() << "[WebSocket] 已连接到服务器，启动心跳";

    // 启动心跳发送定时器
    m_heartbeatTimer->start(m_heartbeatInterval);

    // 启动心跳检测定时器（每秒检测一次）
    m_heartbeatCheckTimer->start(1000);

    // 记录初始时间
    m_lastHeartbeatTime.start();
    m_missedHeartbeats = 0;

    // 【移除自动订阅】不再自动订阅状态栏，改为手动订阅
    // sendStatusSubscribe(1);  // 旧代码：自动订阅1秒周期

    emit connected();
}

void WebSocketClient::onDisconnected()
{
    qDebug() << "[WebSocket] 已断开连接";
    emit disconnected();
}

void WebSocketClient::onTextMessageReceived(const QString &message)
{
    // 收到任何消息都重置心跳计数器
    m_lastHeartbeatTime.restart();
    m_missedHeartbeats = 0;

    // 解析JSON消息
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        qWarning() << "[WebSocket] 收到无效JSON消息";
        return;
    }

    QJsonObject obj = doc.object();
    handleMessage(obj);
}

void WebSocketClient::handleMessage(const QJsonObject &message)
{
    QString cmd = message["cmd"].toString();
    QString from = message["from"].toString();

    if (from != "server") {
        return;
    }

    if (cmd == "heartbeat") {
        // 收到心跳响应
        qDebug() << "[WebSocket] 收到心跳响应";
    }
    else if (cmd == "status") {
        // 检查是否是ACK类型
        QJsonObject data = message["data"].toObject();
        QString type = data["type"].toString();

        if (type == "ack") {
            // 这是客户端发送的ACK，忽略
            return;
        }

        // 状态推送消息
        emit statusReceived(data);

        // 自动发送ACK
        sendStatusACK();
    }
    else {
        // 其他命令的ACK响应
        QJsonObject ackData = message["data"].toObject();
        int code = ackData["code"].toInt();
        // 兼容两种字段名：msg 和 message
        QString msg = ackData["msg"].toString();
        if (msg.isEmpty()) {
            msg = ackData["message"].toString();
        }

        qDebug() << "[WebSocket] 收到ACK:" << cmd << "code=" << code << "msg=" << msg;

        // 发送信号，包含完整的数据负载
        emit ackReceived(cmd, code, msg);
        emit commandACKReceived(cmd, code, msg, ackData);  // 新信号，包含完整数据
    }
}

// 心跳机制实现
void WebSocketClient::sendHeartbeat()
{
    QJsonObject data;
    data["heart"] = "ping";
    QJsonObject cmd = buildCommand("heartbeat", "客户端心跳", data);
    sendMessage(cmd);
    static int heartbeatLogCount = 0;
    if (++heartbeatLogCount % 10 == 1) {
        qDebug() << "[WebSocket] 发送心跳 (每10次打印一次，避免控制台刷屏导致误触挂起)";
    }
}

void WebSocketClient::checkHeartbeatTimeout()
{
    // 计算距离上次收到消息的时间
    qint64 elapsed = m_lastHeartbeatTime.elapsed();

    // 如果超过心跳间隔，增加计数器
    if (elapsed > m_heartbeatInterval) {
        m_missedHeartbeats++;

        qWarning() << QString("[WebSocket] 心跳超时 %1/%2 次，距离上次消息: %3ms")
                      .arg(m_missedHeartbeats)
                      .arg(m_heartbeatTimeoutCount)
                      .arg(elapsed);

        // 达到超时阈值
        if (m_missedHeartbeats >= m_heartbeatTimeoutCount) {
            qCritical() << "[WebSocket] 连接丢失（心跳超时）";

            // 停止心跳定时器
            m_heartbeatTimer->stop();
            m_heartbeatCheckTimer->stop();

            // 发出信号
            emit heartbeatTimeout();
            emit connectionLost();
        }
    }
}

void WebSocketClient::setHeartbeatInterval(int intervalMs)
{
    m_heartbeatInterval = intervalMs;
    if (m_heartbeatTimer->isActive()) {
        m_heartbeatTimer->setInterval(intervalMs);
    }
}

void WebSocketClient::setHeartbeatTimeout(int count)
{
    m_heartbeatTimeoutCount = count;
}

void WebSocketClient::onError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)
    QString errorString = m_webSocket->errorString();
    qWarning() << "[WebSocket] 错误:" << errorString;
    emit this->error(errorString);
}
