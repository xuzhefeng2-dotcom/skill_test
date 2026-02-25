#ifndef REORDERBUFFER_H
#define REORDERBUFFER_H

#include <QObject>
#include <QMap>
#include <QTimer>
#include <QByteArray>
#include <QElapsedTimer>

/**
 * @brief UDP包重排序缓冲区
 *
 * 功能：
 * 1. 接收乱序的UDP包
 * 2. 按PacketNumber排序
 * 3. 超时强制处理（100ms）
 * 4. 输出有序的数据包
 */
class ReorderBuffer : public QObject
{
    Q_OBJECT
public:
    explicit ReorderBuffer(QObject *parent = nullptr);
    ~ReorderBuffer();

    /**
     * @brief 插入UDP包
     * @param packetNumber 包序号
     * @param data 包数据（不含包头）
     */
    void insertPacket(quint32 packetNumber, const QByteArray& data);

    /**
     * @brief 设置超时时间（毫秒）
     * @param timeoutMs 超时时间（默认100ms）
     */
    void setTimeout(int timeoutMs);

    /**
     * @brief 清空缓冲区
     */
    void clear();

    /**
     * @brief 获取缓冲区大小
     */
    int bufferSize() const { return m_buffer.size(); }

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        quint32 totalPacketsReceived;   // 总接收包数
        quint32 reorderedPackets;       // 重排序的包数
        quint32 timeoutFlushCount;      // 超时强制处理次数
        quint32 expectedPacketNumber;   // 期望的下一个包号
    };
    Stats getStats() const { return m_stats; }

signals:
    /**
     * @brief 有序数据包就绪
     * @param packetNumber 包序号
     * @param data 包数据
     */
    void orderedPacketReady(quint32 packetNumber, const QByteArray& data);

private slots:
    void onTimeout();

private:
    QMap<quint32, QByteArray> m_buffer;  // PacketNumber -> Data
    quint32 m_expectedPacketNumber;      // 期望的下一个包号
    QTimer* m_timeoutTimer;              // 超时定时器
    int m_timeoutMs;                     // 超时时间（毫秒）
    QElapsedTimer m_lastPacketTimer;     // 最后一个包的时间
    Stats m_stats;                       // 统计信息

    void processBuffer();                // 处理缓冲区
    void forceFlush();                   // 强制刷新缓冲区
};

#endif // REORDERBUFFER_H
