#ifndef PACKETQUEUE_H
#define PACKETQUEUE_H

#include <QObject>
#include <QQueue>
#include <QByteArray>
#include <QMutex>
#include <QVector>
#include <atomic>

class PacketQueue : public QObject
{
    Q_OBJECT
public:
    explicit PacketQueue(QObject* parent = nullptr);
    ~PacketQueue();

    void enqueue(QByteArray&& data);
    QVector<QByteArray> dequeueBatch(int max);
    int size() const;

    bool trySchedule();
    void clearScheduled() { m_scheduled.store(false); }

    quint64 totalEnqueued() const { return m_totalEnqueued; }
    quint64 totalDequeued() const { return m_totalDequeued; }
    quint64 droppedByQueue() const { return m_droppedByQueue; }
    int maxDepth() const { return m_maxDepth; }

    void setMaxSize(int maxSize) { m_maxSize = maxSize; }
    int maxSize() const { return m_maxSize; }

signals:
    void dataAvailable();

private:
    mutable QMutex m_mutex;
    QQueue<QByteArray> m_queue;
    quint64 m_totalEnqueued, m_totalDequeued, m_droppedByQueue;
    int m_maxDepth, m_maxSize;
    std::atomic<bool> m_scheduled;
};
#endif
