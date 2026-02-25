#ifndef UDPRECEIVERWORKER_H
#define UDPRECEIVERWORKER_H

#include <QObject>
#include <QUdpSocket>
#include <QElapsedTimer>
#include "packetqueue.h"

/**
 * @brief UDP 接收专用 Worker（运行在独立线程）
 *
 * 职责：
 * - 独占一个线程，专门负责 UDP socket drain
 * - readyRead 回调极简化：只做 readDatagram + enqueue
 * - 避免主线程 UI/FFT 操作阻塞 socket 接收
 */
class UdpReceiverWorker : public QObject
{
    Q_OBJECT
public:
    // 注意：parent 必须为 nullptr（因为要 moveToThread）
    explicit UdpReceiverWorker(quint16 port, PacketQueue* queue, QObject* parent = nullptr);
    ~UdpReceiverWorker();

    // 获取诊断统计（线程安全）
    quint64 getTotalDrains() const { return m_totalDrains; }
    quint64 getTotalPackets() const { return m_totalPackets; }

signals:
    void error(const QString& message);
    void initialized(bool success);  // 初始化结果

public slots:
    // ✅ 修复：将 initialize() 改为 public slot，使其可以通过 QMetaObject::invokeMethod 调用
    bool initialize();

private slots:
    void onReadyRead();

private:
    QUdpSocket* m_socket;
    quint16 m_port;
    PacketQueue* m_queue;

    // 轻量诊断统计（不影响性能）
    quint64 m_totalDrains;   // drain 次数
    quint64 m_totalPackets;  // drain 包数
    qint64 m_lastDiagPrint;  // 上次打印时间（限频 5 秒）

    // 性能优化：预分配缓冲区，避免频繁内存分配
    QByteArray m_reusableBuffer;
    static constexpr int MAX_DATAGRAM_SIZE = 2048;  // 最大UDP包大小
};

#endif // UDPRECEIVERWORKER_H
