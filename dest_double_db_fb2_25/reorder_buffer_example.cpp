/**
 * UDP 乱序处理与重排序缓冲区示例
 *
 * 功能：
 * 1. 检测并处理 UDP 数据包乱序
 * 2. 使用重排序缓冲区保证数据按序输出
 * 3. 超时机制防止无限等待丢失的包
 * 4. 统计丢包、乱序、超时等信息
 */

#include <QObject>
#include <QMap>
#include <QElapsedTimer>
#include <QDebug>
#include <QVector>

// ============================================
// 例子 1：基础重排序缓冲区（无超时）
// ============================================

class BasicReorderBuffer {
public:
    BasicReorderBuffer() : m_expectedSeq(0) {}

    // 接收到数据包时调用
    void onPacketReceived(quint32 seq, const QVector<quint16>& data) {
        qDebug() << "[基础版] 收到包:" << seq << "期望:" << m_expectedSeq;

        if (seq == m_expectedSeq) {
            // 情况1：正好是期望的序列号，直接处理
            processPacket(seq, data);
            m_expectedSeq++;

            // 检查缓冲区是否有后续连续的包
            flushContinuousPackets();

        } else if (seq > m_expectedSeq) {
            // 情况2：序列号大于期望值，说明中间有包还没到，先缓存
            qDebug() << "[基础版] 缓存包" << seq << "等待" << m_expectedSeq;
            m_buffer[seq] = data;

        } else {
            // 情况3：序列号小于期望值，说明是重复包或过期包，丢弃
            qDebug() << "[基础版] 丢弃过期包:" << seq;
        }

        qDebug() << "[基础版] 缓冲区大小:" << m_buffer.size();
    }

private:
    void processPacket(quint32 seq, const QVector<quint16>& data) {
        qDebug() << "[基础版] ✓ 处理包" << seq << "数据量:" << data.size();
        // 这里进行实际的数据处理（FFT等）
    }

    void flushContinuousPackets() {
        // 输出所有连续的包
        while (m_buffer.contains(m_expectedSeq)) {
            processPacket(m_expectedSeq, m_buffer[m_expectedSeq]);
            m_buffer.remove(m_expectedSeq);
            m_expectedSeq++;
        }
    }

    QMap<quint32, QVector<quint16>> m_buffer;  // 序列号 → 数据
    quint32 m_expectedSeq;                      // 期望的下一个序列号
};


// ============================================
// 例子 2：带超时的重排序缓冲区（生产级）
// ============================================

class ReorderBufferWithTimeout {
public:
    struct BufferedPacket {
        QVector<quint16> data;
        QElapsedTimer timer;

        BufferedPacket() {
            timer.start();
        }

        BufferedPacket(const QVector<quint16>& d) : data(d) {
            timer.start();
        }
    };

    struct Statistics {
        quint64 totalReceived = 0;      // 总接收包数
        quint64 inOrderReceived = 0;    // 顺序到达的包数
        quint64 reorderedPackets = 0;   // 乱序后重排的包数
        quint64 lostPackets = 0;        // 丢失的包数
        quint64 timeoutPackets = 0;     // 超时强制输出的包数
        quint64 duplicatePackets = 0;   // 重复包数
        int maxBufferSize = 0;          // 缓冲区最大使用量
    };

    ReorderBufferWithTimeout(int timeoutMs = 100, int maxBufferSize = 50)
        : m_expectedSeq(0)
        , m_timeoutMs(timeoutMs)
        , m_maxBufferSize(maxBufferSize)
    {}

    void onPacketReceived(quint32 seq, const QVector<quint16>& data) {
        m_stats.totalReceived++;

        qDebug() << QString("[超时版] 收到包:%1 期望:%2 缓冲:%3")
                    .arg(seq).arg(m_expectedSeq).arg(m_buffer.size());

        if (seq == m_expectedSeq) {
            // 情况1：顺序到达
            m_stats.inOrderReceived++;
            processPacket(seq, data);
            m_expectedSeq++;
            flushContinuousPackets();

        } else if (seq > m_expectedSeq) {
            // 情况2：乱序到达，需要缓存

            // 检查缓冲区是否已满
            if (m_buffer.size() >= m_maxBufferSize) {
                qWarning() << "[超时版] 缓冲区已满，强制跳过到" << seq;
                forceSkipTo(seq);
            }

            // 缓存数据包
            m_buffer[seq] = BufferedPacket(data);

            // 更新统计
            if (m_buffer.size() > m_stats.maxBufferSize) {
                m_stats.maxBufferSize = m_buffer.size();
            }

            // 检查超时
            checkTimeout();

        } else {
            // 情况3：过期包（可能是重复包）
            m_stats.duplicatePackets++;
            qDebug() << "[超时版] 丢弃过期包:" << seq;
        }
    }

    const Statistics& getStatistics() const {
        return m_stats;
    }

    void printStatistics() const {
        qDebug() << "========== 重排序缓冲区统计 ==========";
        qDebug() << "总接收包数:" << m_stats.totalReceived;
        qDebug() << "顺序到达:" << m_stats.inOrderReceived
                 << QString("(%1%)").arg(m_stats.inOrderReceived * 100.0 / m_stats.totalReceived, 0, 'f', 1);
        qDebug() << "乱序重排:" << m_stats.reorderedPackets
                 << QString("(%1%)").arg(m_stats.reorderedPackets * 100.0 / m_stats.totalReceived, 0, 'f', 1);
        qDebug() << "丢失包数:" << m_stats.lostPackets;
        qDebug() << "超时强制输出:" << m_stats.timeoutPackets;
        qDebug() << "重复包数:" << m_stats.duplicatePackets;
        qDebug() << "缓冲区峰值:" << m_stats.maxBufferSize;
        qDebug() << "======================================";
    }

private:
    void processPacket(quint32 seq, const QVector<quint16>& data) {
        qDebug() << QString("[超时版] ✓ 处理包 %1 (数据量:%2)").arg(seq).arg(data.size());
        // 这里进行实际的数据处理（FFT等）
    }

    void flushContinuousPackets() {
        // 输出所有连续的包
        while (m_buffer.contains(m_expectedSeq)) {
            m_stats.reorderedPackets++;
            processPacket(m_expectedSeq, m_buffer[m_expectedSeq].data);
            m_buffer.remove(m_expectedSeq);
            m_expectedSeq++;
        }
    }

    void checkTimeout() {
        if (m_buffer.isEmpty()) {
            return;
        }

        // 检查最早缓存的包是否超时
        auto it = m_buffer.begin();
        if (it.value().timer.elapsed() > m_timeoutMs) {
            quint32 oldestSeq = it.key();

            qWarning() << QString("[超时版] ⚠ 检测到超时! 等待包 %1 已超过 %2ms")
                          .arg(m_expectedSeq).arg(m_timeoutMs);
            qWarning() << QString("[超时版] 强制跳过 %1 → %2 (丢失 %3 个包)")
                          .arg(m_expectedSeq).arg(oldestSeq).arg(oldestSeq - m_expectedSeq);

            // 统计丢失的包数
            m_stats.lostPackets += (oldestSeq - m_expectedSeq);
            m_stats.timeoutPackets++;

            // 强制跳过到最早缓存的包
            m_expectedSeq = oldestSeq;
            flushContinuousPackets();
        }
    }

    void forceSkipTo(quint32 targetSeq) {
        qWarning() << QString("[超时版] ⚠ 缓冲区已满，强制跳过 %1 → %2")
                      .arg(m_expectedSeq).arg(targetSeq);

        m_stats.lostPackets += (targetSeq - m_expectedSeq);
        m_expectedSeq = targetSeq;
        m_buffer.clear();
    }

    QMap<quint32, BufferedPacket> m_buffer;
    quint32 m_expectedSeq;
    int m_timeoutMs;
    int m_maxBufferSize;
    Statistics m_stats;
};


// ============================================
// 测试代码
// ============================================

void testBasicReorderBuffer() {
    qDebug() << "\n========== 测试例子1：基础重排序缓冲区 ==========\n";

    BasicReorderBuffer buffer;

    // 模拟接收顺序：100, 102, 101, 104, 103
    QVector<quint16> dummyData = {1, 2, 3, 4, 5};

    buffer.onPacketReceived(100, dummyData);
    qDebug() << "---";

    buffer.onPacketReceived(102, dummyData);
    qDebug() << "---";

    buffer.onPacketReceived(101, dummyData);  // 乱序包到达
    qDebug() << "---";

    buffer.onPacketReceived(104, dummyData);
    qDebug() << "---";

    buffer.onPacketReceived(103, dummyData);  // 乱序包到达
    qDebug() << "---";
}

void testReorderBufferWithTimeout() {
    qDebug() << "\n========== 测试例子2：带超时的重排序缓冲区 ==========\n";

    ReorderBufferWithTimeout buffer(100, 50);  // 100ms超时，最大缓冲50个包

    QVector<quint16> dummyData = {1, 2, 3, 4, 5};

    // 场景：200, 203, 202, 204, 205（201丢失）
    buffer.onPacketReceived(200, dummyData);
    qDebug() << "---";

    buffer.onPacketReceived(203, dummyData);
    qDebug() << "---";

    buffer.onPacketReceived(202, dummyData);
    qDebug() << "---";

    // 模拟等待101ms（超时）
    QThread::msleep(101);

    buffer.onPacketReceived(204, dummyData);  // 触发超时检查
    qDebug() << "---";

    buffer.onPacketReceived(205, dummyData);
    qDebug() << "---";

    // 打印统计信息
    buffer.printStatistics();
}

void testWorstCase() {
    qDebug() << "\n========== 测试例子3：极端乱序场景 ==========\n";

    ReorderBufferWithTimeout buffer(50, 20);

    QVector<quint16> dummyData = {1, 2, 3};

    // 极端乱序：收到 300, 310, 305, 301, 302, 303, 304
    QVector<quint32> receiveOrder = {300, 310, 305, 301, 302, 303, 304};

    for (quint32 seq : receiveOrder) {
        buffer.onPacketReceived(seq, dummyData);
        qDebug() << "---";
        QThread::msleep(10);  // 模拟接收间隔
    }

    buffer.printStatistics();
}

// 主函数
int main() {
    testBasicReorderBuffer();
    testReorderBufferWithTimeout();
    testWorstCase();

    return 0;
}
