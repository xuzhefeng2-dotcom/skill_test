#ifndef UDPSERVERSENDER_H
#define UDPSERVERSENDER_H

#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include "datareader.h"
#include "packetformat.h"

/**
 * @brief UDP服务器发送器
 *
 * 功能:读取COS文件,每次发送5帧数据到客户端,间隔10ms
 * 线程:主线程(使用定时器)
 */
class UDPServerSender : public QObject
{
    Q_OBJECT
public:
    explicit UDPServerSender(QObject *parent = nullptr);
    ~UDPServerSender();

    // 初始化服务器
    bool initialize(quint16 port);

    // 打开COS文件
    bool openCosFile(const QString &filePath);

    // 开始发送数据
    void startSending();

    // 停止发送
    void stopSending();

    // 设置客户端地址(从第一个连接请求中获取)
    void setClientAddress(const QHostAddress &address, quint16 port);

    // 设置是否使用帧内分包模式（避免IP分片）
    void setUseSubPacketMode(bool enable) { m_useSubPacketMode = enable; }

    // 设置是否使用新的数据包格式（955字节格式）
    void setUseNewPacketFormat(bool enable) { m_useNewPacketFormat = enable; }

signals:
    void fileOpened(const QString &filePath);
    void sendingStarted();
    void sendingStopped();
    void progressChanged(int percent);
    void allDataSent();
    void error(const QString &message);
    void packetSent(int frameCount, quint32 sequence);

private slots:
    void sendNextBatch();  // 发送下一批数据(5帧)

private:
    // 新格式发送：发送单个数据包（955字节）
    void sendNewFormatPacket();

    // 帧内分包发送：发送单帧数据（拆分为多个小包）
    void sendFrameWithSubPackets(const DataFrame &frame, quint32 frameNumber);

    // 发送单个通道的数据（拆分为多个小包，24位数据）
    void sendChannelData(const QVector<quint32> &channelData, quint32 frameNumber, quint8 channelId);

    // 传统发送：发送多帧数据（一个大包）
    void sendMultipleFrames();
    QUdpSocket *m_socket;
    quint16 m_port;
    QHostAddress m_clientAddress;
    quint16 m_clientPort;

    DataReader *m_dataReader;
    QTimer *m_sendTimer;
    QElapsedTimer m_elapsedTimer;

    bool m_isSending;
    qint64 m_currentFrameIndex;
    qint64 m_totalFrames;
    quint32 m_packetSequence;
    bool m_useSubPacketMode;  // 是否使用帧内分包模式
    bool m_useNewPacketFormat;  // 是否使用新的955字节格式

    // 新格式发送状态
    int m_currentSampleOffset;  // 当前帧内的采样点偏移（用于新格式）

    static constexpr int SAMPLES_PER_FRAME = 5000;
    static constexpr int FRAMES_PER_BATCH = 1;       // 每批发送1帧（传统模式）
    static constexpr int SEND_INTERVAL_MS = 20;      // 帧间间隔20ms（新格式模式下）
};

#endif // UDPSERVERSENDER_H
