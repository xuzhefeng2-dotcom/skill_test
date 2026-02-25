#include "reorderbuffer.h"
#include <QDebug>

ReorderBuffer::ReorderBuffer(QObject *parent)
    : QObject(parent)
    , m_expectedPacketNumber(0)
    , m_timeoutMs(100)
{
    m_stats.totalPacketsReceived = 0;
    m_stats.reorderedPackets = 0;
    m_stats.timeoutFlushCount = 0;
    m_stats.expectedPacketNumber = 0;

    m_timeoutTimer = new QTimer(this);
    connect(m_timeoutTimer, &QTimer::timeout, this, &ReorderBuffer::onTimeout);
}

ReorderBuffer::~ReorderBuffer()
{
    m_timeoutTimer->stop();
}

void ReorderBuffer::insertPacket(quint32 packetNumber, const QByteArray& data)
{
    m_stats.totalPacketsReceived++;

    // 检测序列号重置（新的display命令）
    if (packetNumber == 0 && m_expectedPacketNumber != 0) {
        qDebug() << "[ReorderBuffer] 检测到序列号重置，清空缓冲区";
        clear();
    }

    // 如果是期望的包号，直接输出
    if (packetNumber == m_expectedPacketNumber) {
        emit orderedPacketReady(packetNumber, data);
        m_expectedPacketNumber++;
        m_stats.expectedPacketNumber = m_expectedPacketNumber;

        // 处理缓冲区中的连续包
        processBuffer();

        // 重置超时定时器
        m_lastPacketTimer.start();
        if (!m_timeoutTimer->isActive()) {
            m_timeoutTimer->start(m_timeoutMs);
        }
    }
    // 如果是未来的包，存入缓冲区
    else if (packetNumber > m_expectedPacketNumber) {
        m_buffer.insert(packetNumber, data);
        m_stats.reorderedPackets++;

        qDebug() << QString("[ReorderBuffer] 乱序包：期望=%1, 实际=%2, 缓冲区大小=%3")
                    .arg(m_expectedPacketNumber)
                    .arg(packetNumber)
                    .arg(m_buffer.size());

        // 启动超时定时器
        m_lastPacketTimer.start();
        if (!m_timeoutTimer->isActive()) {
            m_timeoutTimer->start(m_timeoutMs);
        }
    }
    // 如果是过去的包（重复包），丢弃
    else {
        qWarning() << QString("[ReorderBuffer] 丢弃过期包：期望=%1, 实际=%2")
                      .arg(m_expectedPacketNumber)
                      .arg(packetNumber);
    }
}

void ReorderBuffer::processBuffer()
{
    // 从缓冲区中取出连续的包
    while (m_buffer.contains(m_expectedPacketNumber)) {
        QByteArray data = m_buffer.take(m_expectedPacketNumber);
        emit orderedPacketReady(m_expectedPacketNumber, data);
        m_expectedPacketNumber++;
        m_stats.expectedPacketNumber = m_expectedPacketNumber;
    }

    // 如果缓冲区为空，停止定时器
    if (m_buffer.isEmpty()) {
        m_timeoutTimer->stop();
    }
}

void ReorderBuffer::onTimeout()
{
    // 检查是否超时
    if (m_lastPacketTimer.elapsed() >= m_timeoutMs) {
        qWarning() << QString("[ReorderBuffer] 超时强制处理，缓冲区大小=%1")
                      .arg(m_buffer.size());
        forceFlush();
        m_stats.timeoutFlushCount++;
    }
}

void ReorderBuffer::forceFlush()
{
    // 强制输出缓冲区中的所有包（按序号排序）
    QList<quint32> keys = m_buffer.keys();
    for (quint32 packetNumber : keys) {
        QByteArray data = m_buffer.take(packetNumber);

        // 检测丢包
        if (packetNumber > m_expectedPacketNumber) {
            qWarning() << QString("[ReorderBuffer] 检测到丢包：期望=%1, 实际=%2, 丢失=%3个包")
                          .arg(m_expectedPacketNumber)
                          .arg(packetNumber)
                          .arg(packetNumber - m_expectedPacketNumber);
        }

        emit orderedPacketReady(packetNumber, data);
        m_expectedPacketNumber = packetNumber + 1;
        m_stats.expectedPacketNumber = m_expectedPacketNumber;
    }

    m_timeoutTimer->stop();
}

void ReorderBuffer::setTimeout(int timeoutMs)
{
    m_timeoutMs = timeoutMs;
}

void ReorderBuffer::clear()
{
    m_buffer.clear();
    m_expectedPacketNumber = 0;
    m_stats.expectedPacketNumber = 0;
    m_timeoutTimer->stop();
}
