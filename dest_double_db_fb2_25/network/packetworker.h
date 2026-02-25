#ifndef PACKETWORKER_H
#define PACKETWORKER_H

#include <QObject>
#include <QByteArray>
#include <atomic>
#include "packetqueue.h"
#include "../protocol/packetformat.h"

struct ParsedPacket
{
    quint32 seq;            // 帧序号
    QByteArray payload;     // 数据载荷（936字节）
    PacketTimestamp timestamp; // 时间戳
    quint8 channelCount;    // 通道数（2或3）
    quint8 sampleRate;      // 采样率（0=250kHz，1=4MHz）
};

Q_DECLARE_METATYPE(ParsedPacket)

/** 通道分离后的单帧数据（在 PacketWorker 线程内完成分离，主线程只做 push/统计） */
struct SeparatedFrame
{
    QVector<quint32> ewData;
    QVector<quint32> nsData;
    QVector<quint32> tdData;
    quint32 seq;
    PacketTimestamp timestamp;
    quint8 channelCount;
    quint8 sampleRate;
};

Q_DECLARE_METATYPE(SeparatedFrame)

class PacketWorker : public QObject
{
    Q_OBJECT
public:
    explicit PacketWorker(PacketQueue* queue, const QString& channelConfig, QObject* parent = nullptr);
    ~PacketWorker();

    void stop();

public slots:
    void processQueue();

signals:
    void packetParsed(const ParsedPacket& packet);
    /** 通道已在 worker 线程分离完毕，主线程槽只做 ringbuffer push 与统计 */
    void separatedFrames(const SeparatedFrame& frame);
    void heartbeatReceived(quint32 seq);
    void packetsLost(quint32 count);

private:
    void processPacket(const QByteArray& data);

    PacketQueue* m_queue;
    QString m_channelConfig;
    std::atomic<bool> m_running;

    quint32 m_lastSeq;
    bool m_seqInitialized;
};

#endif
