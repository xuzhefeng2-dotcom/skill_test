#include "packetqueue.h"
#include <QElapsedTimer>
#include <QDebug>
PacketQueue::PacketQueue(QObject* parent)
    : QObject(parent)
    , m_totalEnqueued(0)
    , m_totalDequeued(0)
    , m_droppedByQueue(0)
    , m_maxDepth(0)
    , m_maxSize(32768)
    , m_scheduled(false)
{
}

PacketQueue::~PacketQueue()
{
}

void PacketQueue::enqueue(QByteArray&& data)
{
	    static QElapsedTimer timer;
    static bool started = false;
    static qint64 lastReportMs = 0;

    if (!started) {
        timer.start();
        started = true;
    }
    bool shouldSchedule = false;

    {
        QMutexLocker lock(&m_mutex);

        if (m_queue.size() >= m_maxSize) {
            m_droppedByQueue++;
            return;
        }

        m_queue.enqueue(std::move(data));
		        qint64 now = timer.elapsed();
        if (now - lastReportMs >= 5000) {
            lastReportMs = now;
            // 若 maxDepth 突然升高：多为控制台被挂起（如 Windows 下误点触发的 QuickEdit），按回车恢复后队列会排空
            qDebug() << "[PacketQueue]"
                     << "elapsed(s)=" << now / 1000
                     << "queue=" << m_queue.size()
                     << "maxDepth=" << m_maxDepth
                     << "dropped=" << m_droppedByQueue
                     << "enq=" << m_totalEnqueued
                     << "deq=" << m_totalDequeued;
        }
        m_totalEnqueued++;

        int depth = m_queue.size();
        if (depth > m_maxDepth) {
            m_maxDepth = depth;
        }

        shouldSchedule = (depth == 1);
    }

    if (shouldSchedule && trySchedule()) {
        emit dataAvailable();
    }
}

QVector<QByteArray> PacketQueue::dequeueBatch(int max)
{
    QMutexLocker lock(&m_mutex);
    QVector<QByteArray> batch;
    batch.reserve(qMin(max, m_queue.size()));

    while (!m_queue.isEmpty() && batch.size() < max) {
        batch.append(m_queue.dequeue());
        m_totalDequeued++;
    }

    return batch;
}

int PacketQueue::size() const
{
    QMutexLocker lock(&m_mutex);
    return m_queue.size();
}

bool PacketQueue::trySchedule()
{
    bool expected = false;
    return m_scheduled.compare_exchange_strong(expected, true);
}
