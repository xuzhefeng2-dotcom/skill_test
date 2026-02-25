#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTimer>
#include <QElapsedTimer>

/**
 * @brief WebSocket客户端（扩展版 - play.md协议）
 *
 * 功能:
 * 1. 连接到Go服务端WebSocket
 * 2. 发送控制指令（collect/display/stop等）
 * 3. 接收ACK响应
 * 4. 接收状态推送（按订阅周期）
 * 5. 心跳机制（2秒周期，3次超时判定断开）
 */
class WebSocketClient : public QObject
{
    Q_OBJECT
public:
    explicit WebSocketClient(QObject *parent = nullptr);
    ~WebSocketClient();

    // 连接到服务器
    bool connectToServer(const QString &url);

    // 断开连接
    void disconnectFromServer();

    // play.md协议命令
    void sendCollectCommand(const QString& channel, double sampleRate);  // 发送collect命令（通道参数+采样率）
    void sendDisplayCommand(const QString& channel);      // 发送display命令（通道参数）
    void sendStopCollectCommand();                        // 发送stop_collect命令
    void sendStopDisplayCommand();                        // 发送stop_display命令
    void sendStatusSubscribe(int cycle);                  // 订阅状态栏（周期：1/5/10秒）
    void sendStatusACK();                                 // 发送status ACK

    // 旧版命令（保留兼容）
    void sendStartCommand();         // 发送START指令
    void sendDisplayCommand();       // 发送DISPLAY指令（无参数版本）

    // 获取连接状态
    bool isConnected() const;

    // 心跳配置
    void setHeartbeatInterval(int intervalMs);            // 设置心跳间隔（默认2000ms）
    void setHeartbeatTimeout(int count);                  // 设置超时阈值（默认3次）

signals:
    void connected();
    void disconnected();
    void ackReceived(const QString &cmd, int code, const QString &message);
    void commandACKReceived(const QString &cmd, int code, const QString &msg, const QJsonObject &data);  // play.md协议ACK（带数据负载）
    void statusReceived(const QJsonObject &statusData);   // 状态推送
    void error(const QString &message);
    void heartbeatTimeout();                              // 心跳超时
    void connectionLost();                                // 连接丢失

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onError(QAbstractSocket::SocketError error);
    void sendHeartbeat();                                 // 发送心跳
    void checkHeartbeatTimeout();                         // 检测心跳超时

private:
    QJsonObject buildCommand(const QString &cmd, const QString &description);
    QJsonObject buildCommand(const QString &cmd, const QString &description, const QJsonObject &data);
    void sendMessage(const QJsonObject &message);
    void handleMessage(const QJsonObject &message);

    QWebSocket *m_webSocket;
    QString m_serverUrl;

    // 心跳机制
    QTimer* m_heartbeatTimer;                             // 心跳发送定时器
    QTimer* m_heartbeatCheckTimer;                        // 心跳检测定时器
    int m_heartbeatInterval;                              // 心跳间隔（毫秒）
    int m_heartbeatTimeoutCount;                          // 超时阈值（次数）
    int m_missedHeartbeats;                               // 连续未收到心跳的次数
    QElapsedTimer m_lastHeartbeatTime;                    // 最后一次收到心跳的时间
};

#endif // WEBSOCKETCLIENT_H
