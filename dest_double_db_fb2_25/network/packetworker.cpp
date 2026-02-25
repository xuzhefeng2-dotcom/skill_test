#include "packetworker.h"
#include "../protocol/packetformat.h"
#include "../core/channelseparator.h"
#include <QElapsedTimer>
#include <QDebug>
#include <QtEndian>

inline bool seqAhead(quint32 a, quint32 b) {
    return (qint32)(a - b) > 0;
}

PacketWorker::PacketWorker(PacketQueue* queue, const QString& channelConfig, QObject* parent)
    : QObject(parent)
    , m_queue(queue)
    , m_channelConfig(channelConfig)
    , m_running(true)
    , m_lastSeq(0)
    , m_seqInitialized(false)
{
}

PacketWorker::~PacketWorker()
{
}

void PacketWorker::stop()
{
    m_running.store(false);
}

void PacketWorker::processQueue()
{
    if (!m_running.load()) {
        return;
    }

    QElapsedTimer timer;
    timer.start();

    const int maxBatch = 1000;       // 每轮最多处理包数（增大以提升生产侧吞吐）
    const qint64 maxTimeNs = 8000000; // 每轮最多运行 8ms，避免单次占用过长

    while (m_running.load() && timer.nsecsElapsed() < maxTimeNs) {
        QVector<QByteArray> batch = m_queue->dequeueBatch(maxBatch);
        if (batch.isEmpty()) {
            break;
        }

        for (const QByteArray& data : batch) {
            if (!m_running.load()) {
                break;
            }
            processPacket(data);
        }
    }

    m_queue->clearScheduled();

    if (m_queue->size() > 0 && m_running.load()) {
        if (m_queue->trySchedule()) {
            QMetaObject::invokeMethod(this, "processQueue", Qt::QueuedConnection);
        }
    }
}

void PacketWorker::processPacket(const QByteArray& data)
{
    using namespace NewPacketConfig;

    if (data.size() != TOTAL_PACKET_SIZE) {
        return;
    }

    const NewPacketHeader* header = reinterpret_cast<const NewPacketHeader*>(data.constData());

    // 验证帧头
    if (header->magic != MAGIC_NUMBER) {
        return;
    }

    // 心跳包：帧类型=0
    if (header->frameType == FrameType::HEARTBEAT) {
        emit heartbeatReceived(header->frameSequence);
        return;
    }

    // 非数据包，忽略
    if (header->frameType != FrameType::DATA) {
        return;
    }

quint32 seq = header->frameSequence;

// quint32 le = qToLittleEndian(seq);
// const uchar* p = reinterpret_cast<const uchar*>(&le);

// QString leHex = QString("%1 %2 %3 %4")
//     .arg(p[0], 2, 16, QChar('0'))
//     .arg(p[1], 2, 16, QChar('0'))
//     .arg(p[2], 2, 16, QChar('0'))
//     .arg(p[3], 2, 16, QChar('0'))
//     .toUpper();

// qDebug() << QString("帧号: %1  (LE bytes: %2)")
//             .arg(seq)
//             .arg(leHex);
			
			
    if (!m_seqInitialized) {
        m_lastSeq = seq;
        m_seqInitialized = true;
    } else {
        if (seq == m_lastSeq) {
            return;
        }

        quint32 expected = m_lastSeq + 1;
        if (seqAhead(seq, expected)) {
            quint32 lost = seq - expected;
            emit packetsLost(lost);
        }

        if (seqAhead(seq, m_lastSeq)) {
            m_lastSeq = seq;
        } else {
            return;
        }
    }

    QByteArray payload = data.mid(HEADER_SIZE, DATA_SIZE);
    PacketTimestamp timestamp = header->timestamp;
    quint8 channelCount = header->channelCount;
    quint8 sampleRate = header->sampleRate;

    // 在 worker 线程内完成通道分离，避免主线程耗时（原在主线程 onPacketParsed 中）
    QString channelConfigStr = (channelCount == 3) ? QString("111") : QString("110");
    ChannelConfig config = ChannelConfig::fromString(channelConfigStr);
    if (!config.isValid()) {
        return;
    }
    QVector<quint32> ewData, nsData, tdData;
    bool success = ChannelSeparator::separateFromByteArray(
        payload,
        ChannelSeparator::Format_NewPacket,
        config,
        ewData,
        nsData,
        tdData
    );
    if (!success) {
        return;
    }

    SeparatedFrame frame;
    frame.ewData = std::move(ewData);
    frame.nsData = std::move(nsData);
    frame.tdData = std::move(tdData);
    frame.seq = seq;
    frame.timestamp = timestamp;
    frame.channelCount = channelCount;
    frame.sampleRate = sampleRate;
    emit separatedFrames(frame);
}
