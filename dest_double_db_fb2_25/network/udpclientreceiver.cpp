#include "udpclientreceiver.h"
//#include "../core/uint24.h"
#include <QDebug>
#include <QDataStream>
#include <QVariant>
#include <QThread>

UDPClientReceiver::UDPClientReceiver(QObject *parent)
    : QObject(parent)
    , m_port(0)
    , m_isReceiving(false)
    , m_recvThread(nullptr)
    , m_recvWorker(nullptr)
    , m_ewDataBuffer(RINGBUF_CAPACITY)
    , m_nsDataBuffer(RINGBUF_CAPACITY)
    , m_tdDataBuffer(RINGBUF_CAPACITY)
    , m_fftThread(nullptr)
    , m_fftWorker(nullptr)
    , m_lastSequence(0)
    , m_firstPacket(true)
    , m_everReceived(false)
    , m_currentFrameCount(0)
    , m_fftTimer(nullptr)
    , m_useSubPacketMode(false)  // 默认使用传统模式（一帧一包）
    , m_nextExpectedFrame(0)
    , m_useNewPacketFormat(false)  // 默认不使用新格式
    , m_newFormatPacketsReceived(0)
    , m_reorderBuffer(nullptr)
    , m_currentChannelConfig("110")  // 默认双通道
    , m_udpTimeoutTimer(nullptr)
    , m_udpTimeoutMs(5000)  // 默认5秒超时
    , m_lastNewFmtPacketNumber(0)
    , m_hasLastNewFmtPacketNumber(false)
    , m_newFmtAnomalyCount(0)
    , m_hasFirstTimestamp(false)
    , m_packetsSinceLoopDetect(0)
    , m_receivedFrameIndex(0)
    , m_lastDrawFrameIndex(0)
    , m_drawFrameInterval(100)  // 默认每100帧绘制一次
    , m_maxDrawQueueSize(10)
{
    memset(&m_firstTimestampRaw, 0, sizeof(m_firstTimestampRaw));
    m_packetQueue = nullptr;
    m_packetWorker = nullptr;
    m_workerThread = nullptr;
    m_stats.receiveStartTime = 0;
    m_stats.lastFetchTime = 0;
    m_stats.totalPacketsReceived = 0;
    m_stats.totalFramesReceived = 0;
    m_stats.lostPackets = 0;
    m_stats.outOfOrderPackets = 0;
    m_stats.duplicatePackets = 0;
    m_stats.resyncCount = 0;
    m_stats.fetchCount = 0;
    m_stats.fftProcessCount = 0;

    // [诊断] 初始化诊断字段
    m_stats.queueCurrent = 0;
    m_stats.queueMaxDepth = 0;
    m_stats.queueDropped = 0;
    m_stats.queueEnqueued = 0;
    m_stats.queueDequeued = 0;
    m_stats.slowDrains = 0;
    m_stats.readyReadMaxDrainMs = 0.0;
    m_stats.readyReadTotalDrains = 0;
    m_stats.readyReadTotalPackets = 0;
    m_stats.readyReadMaxBatch = 0;

    // 创建FFT处理线程
    m_fftThread = new QThread();
    m_fftWorker = new FFTWorker();
    m_fftWorker->moveToThread(m_fftThread);
    connect(m_fftWorker, &FFTWorker::spectrumTripletReady,
        this,        &UDPClientReceiver::spectrumTripletReady,
        Qt::QueuedConnection);
    // 注册类型以便在线程间传递
    qRegisterMetaType<QVector<quint16>>("QVector<quint16>");

    connect(m_fftThread, &QThread::finished, m_fftWorker, &QObject::deleteLater);

    m_fftThread->start();
    qDebug() << "[客户端] FFT处理线程已启动";

    // 创建FFT处理定时器 - 异步处理，避免阻塞接收
    m_fftTimer = new QTimer(this);
    connect(m_fftTimer, &QTimer::timeout, this, &UDPClientReceiver::processBatchFFT);
    m_fftTimer->start(1); // 1ms间隔，异步处理FFT

    // 创建重排序缓冲区
    m_reorderBuffer = new ReorderBuffer(this);
    m_reorderBuffer->setTimeout(100);  // 100ms超时
    connect(m_reorderBuffer, &ReorderBuffer::orderedPacketReady,
            this, &UDPClientReceiver::onOrderedPacketReady);

    // 创建UDP超时检测定时器
    m_udpTimeoutTimer = new QTimer(this);
    connect(m_udpTimeoutTimer, &QTimer::timeout, this, &UDPClientReceiver::checkUDPTimeout);
    m_udpTimeoutTimer->start(1000);  // 每秒检测一次
}

UDPClientReceiver::~UDPClientReceiver()
{
    stopReceiving();

    // 清理 PacketWorker 线程
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait(3000);
        delete m_workerThread;
        m_workerThread = nullptr;
    }

    // 清理 UDP 接收线程
    if (m_recvThread) {
        m_recvThread->quit();
        m_recvThread->wait(3000);
        delete m_recvThread;
        m_recvThread = nullptr;
    }

    // 清理 FFT 处理线程
    if (m_fftThread) {
        m_fftThread->quit();
        m_fftThread->wait(3000);
        delete m_fftThread;
        m_fftThread = nullptr;
    }

    delete m_packetQueue;
    m_packetQueue = nullptr;
    m_packetWorker = nullptr;
    m_recvWorker = nullptr;
    m_fftWorker = nullptr;
}

bool UDPClientReceiver::initialize(quint16 port)
{
    m_port = port;
    qDebug() << "[客户端] 初始化完成，端口:" << port;
    return true;
}

void UDPClientReceiver::startReceiving()
{
    // 1. 创建 PacketQueue（如果不存在）
    if (!m_packetQueue) {
        m_packetQueue = new PacketQueue();
    }

    // 2. 创建 PacketWorker 线程（如果不存在）
    if (!m_packetWorker) {
        m_workerThread = new QThread();
        m_packetWorker = new PacketWorker(m_packetQueue, m_currentChannelConfig);
        m_packetWorker->moveToThread(m_workerThread);

        qRegisterMetaType<ParsedPacket>("ParsedPacket");
        qRegisterMetaType<SeparatedFrame>("SeparatedFrame");

        connect(m_packetQueue, &PacketQueue::dataAvailable,
                m_packetWorker, &PacketWorker::processQueue, Qt::QueuedConnection);
        connect(m_packetWorker, &PacketWorker::separatedFrames,
                this, &UDPClientReceiver::onSeparatedFrames, Qt::QueuedConnection);
        connect(m_packetWorker, &PacketWorker::heartbeatReceived,
                this, &UDPClientReceiver::onHeartbeat, Qt::QueuedConnection);
        connect(m_packetWorker, &PacketWorker::packetsLost,
                this, &UDPClientReceiver::onPacketsLost, Qt::QueuedConnection);
        connect(m_workerThread, &QThread::finished,
                m_packetWorker, &QObject::deleteLater);

        m_workerThread->start(QThread::HighPriority);
    }

    // 3. 创建 UDP 接收线程（如果不存在）
    if (!m_recvWorker) {
        m_recvThread = new QThread();  // 无 parent，避免 double-delete
        m_recvWorker = new UdpReceiverWorker(m_port, m_packetQueue);
        m_recvWorker->moveToThread(m_recvThread);

        // 连接初始化完成信号
        connect(m_recvWorker, &UdpReceiverWorker::initialized,
                this, &UDPClientReceiver::onRecvWorkerInitialized, Qt::QueuedConnection);
        connect(m_recvWorker, &UdpReceiverWorker::error,
                this, &UDPClientReceiver::error, Qt::QueuedConnection);
        connect(m_recvThread, &QThread::finished,
                m_recvWorker, &QObject::deleteLater);

        m_recvThread->start(QThread::TimeCriticalPriority);

        // ✅ 修复：使用 BlockingQueuedConnection 等待 initialize() 完成
        bool initSuccess = false;
        bool invokeOk = QMetaObject::invokeMethod(
            m_recvWorker,
            "initialize",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(bool, initSuccess)
        );

        if (!invokeOk || !initSuccess) {
            qCritical() << "[客户端] ❌ UDP 初始化失败，中止启动";
            qCritical() << "[客户端] invokeOk=" << invokeOk << "initSuccess=" << initSuccess;

            // 清理资源
            m_recvThread->quit();
            m_recvThread->wait(3000);
            delete m_recvThread;
            m_recvThread = nullptr;
            m_recvWorker = nullptr;

            emit error("[客户端] UDP 端口绑定失败");
            return;  // ← 中止启动流程
        }

        qDebug() << "[客户端] ✅ UDP 接收线程初始化成功（已验证 bind）";
    }

    m_isReceiving = true;
    m_everReceived = false;
    m_currentFrameCount = 0;
    m_stats.receiveStartTime = 0;
    m_stats.lastFetchTime = 0;
    m_stats.totalFramesReceived = 0;
    m_stats.totalPacketsReceived = 0;
    m_stats.lostPackets = 0;
    m_stats.outOfOrderPackets = 0;
    m_stats.duplicatePackets = 0;
    m_stats.resyncCount = 0;
    m_stats.fetchCount = 0;
    m_stats.fftProcessCount = 0;

    clear();

    // 根据当前格式调用对应的重置函数
    if (m_useNewPacketFormat) {
        resetNewFormatState();
    } else {
        resetTraditionalState();
    }

    // 清空重排序缓冲区
    if (m_reorderBuffer) {
        m_reorderBuffer->clear();
    }

    // 启动UDP超时检测
    m_lastUDPPacketTime.start();

    // 启动FFT处理定时器（1ms 更频繁取批，提升处理速度）
    if (m_fftTimer) {
        m_fftTimer->start(1);
    }

    QString mode = m_useNewPacketFormat ? "新格式(955字节)" :
                   (m_useSubPacketMode ? "帧内分包" : "传统模式");
    qDebug() << "[客户端] 开始接收数据, 模式:" << mode << "通道配置:" << m_currentChannelConfig;
}

void UDPClientReceiver::stopReceiving()
{
    m_isReceiving = false;

    // 停止 PacketWorker
    if (m_packetWorker) {
        m_packetWorker->stop();
    }

    // 停止 UDP 接收（无需 disconnect，socket 在独立线程里）
    // 线程退出由析构函数负责

    // 停止FFT处理定时器
    if (m_fftTimer) {
        m_fftTimer->stop();
    }

    // 【关键修复】强制处理剩余的不足一批的帧
    if (m_currentFrameCount > 0) {
        qDebug() << QString("[客户端] 强制处理剩余 %1 帧（未达到批次阈值 %2）")
                    .arg(m_currentFrameCount)
                    .arg(FRAMES_PER_BATCH);
        emit batchReady(m_currentFrameCount);
        emit statsUpdated(m_stats);
        m_currentFrameCount = 0;
    }

    // 【关键修复】强制FFT处理剩余数据
    // 检查缓冲区是否还有数据
    int remainingNS = m_nsDataBuffer.size();
    int remainingEW = m_ewDataBuffer.size();
    if (remainingNS > 0 || remainingEW > 0) {
        qDebug() << QString("[客户端] 强制FFT处理剩余数据: NS=%1帧, EW=%2帧")
                    .arg(remainingNS).arg(remainingEW);
        processBatchFFT();  // 触发一次FFT处理
    }

    // 诊断：显示接收统计摘要
    qDebug() << "\n========== 客户端接收统计摘要 ==========";

    // 根据当前格式显示对应的模式和序号
    if (m_useNewPacketFormat) {
        // 更新最终统计
        updateDiagnosticStats();

        qDebug() << "接收模式: 新格式(955字节)";
        qDebug() << "最后收到的包号:" << m_lastNewFmtPacketNumber;
        qDebug() << "总接收包数:" << m_stats.totalPacketsReceived;
        qDebug() << "总接收帧数:" << m_stats.totalFramesReceived;
        qDebug() << "检测到的丢包数:" << m_stats.lostPackets;
        qDebug() << "乱序包数:" << m_stats.outOfOrderPackets;
        qDebug() << "重复包数:" << m_stats.duplicatePackets;
        qDebug() << "重同步次数:" << m_stats.resyncCount;

        qDebug() << "\n[Queue 诊断]";
        qDebug() << "  当前深度:" << m_stats.queueCurrent;
        qDebug() << "  峰值深度:" << m_stats.queueMaxDepth;
        qDebug() << "  队列丢弃:" << m_stats.queueDropped;
        qDebug() << "  累计入队:" << m_stats.queueEnqueued;
        qDebug() << "  累计出队:" << m_stats.queueDequeued;

        qDebug() << "\n[readyRead Drain 诊断]";
        qDebug() << "  慢 drain 次数 (>2ms):" << m_stats.slowDrains;
        qDebug() << "  最大 drain 耗时:" << QString::number(m_stats.readyReadMaxDrainMs, 'f', 2) << "ms";
        qDebug() << "  总 drain 次数:" << m_stats.readyReadTotalDrains;
        qDebug() << "  总 drain 包数:" << m_stats.readyReadTotalPackets;
        qDebug() << "  单次最大包数:" << m_stats.readyReadMaxBatch;
        if (m_stats.readyReadTotalDrains > 0) {
            double avgBatch = static_cast<double>(m_stats.readyReadTotalPackets) / m_stats.readyReadTotalDrains;
            qDebug() << "  平均每次 drain 包数:" << QString::number(avgBatch, 'f', 1);
        }

        qDebug() << "\n[诊断分析 - 新格式]";
        qDebug() << "如果服务器发送了N个包（包号0到N-1）：";
        qDebug() << "  理论最后包号 = N - 1";
        qDebug() << "  实际最后包号 =" << m_lastNewFmtPacketNumber;
        qDebug() << "  实际包数 =" << m_stats.totalPacketsReceived;

        // [诊断] 给出结论性建议
        qDebug() << "\n[丢包分析结论]";
        if (m_stats.lostPackets > 0) {
            if (m_stats.queueDropped > 0) {
                qWarning() << "  ⚠ 队列丢弃 =" << m_stats.queueDropped << "包";
                qWarning() << "  ➜ 结论：PacketQueue 已满，PacketWorker 处理不过来";
            } else if (m_stats.slowDrains > 0 && m_stats.readyReadMaxDrainMs > 5.0) {
                qWarning() << "  ⚠ 检测到" << m_stats.slowDrains << "次慢 drain，最大耗时"
                           << QString::number(m_stats.readyReadMaxDrainMs, 'f', 2) << "ms";
                qWarning() << "  ➜ 结论：readyRead drain 太慢，阻塞了 socket 接收";
            } else {
                qWarning() << "  ⚠ 队列无丢弃，drain 无异常";
                qWarning() << "  ➜ 结论：丢包发生在 Qt 应用层之前（内核 UDP rcvbuf / 网卡）";
                qWarning() << "  ➜ 建议：检查内核 UDP 统计（netstat -su | grep 'receive errors'）";
            }
        } else {
            qDebug() << "  ✓ 无丢包，接收正常";
        }
    } else if (m_useSubPacketMode) {
        qDebug() << "接收模式: 帧内分包模式";
        qDebug() << "最后收到的帧号:" << m_lastSequence;
        qDebug() << "总接收包数:" << m_stats.totalPacketsReceived;
        qDebug() << "总接收帧数:" << m_stats.totalFramesReceived;
        qDebug() << "检测到的丢包数:" << m_stats.lostPackets;
        qDebug() << "重组器待处理帧数:" << m_frameReassembler.pendingFrameCount() << "(未完整的帧)";

        qDebug() << "\n[诊断分析 - 帧内分包模式]";
        qDebug() << "如果服务器发送了N帧（帧号0到N-1）：";
        qDebug() << "  理论最后帧号 = N - 1";
        qDebug() << "  实际最后帧号 =" << m_lastSequence;
        qDebug() << "  理论包数 = N × 12（每帧12个子包）";
        qDebug() << "  实际包数 =" << m_stats.totalPacketsReceived;
    } else {
        qDebug() << "接收模式: 传统模式";
        qDebug() << "最后收到的包序列号:" << m_lastSequence;
        qDebug() << "总接收包数:" << m_stats.totalPacketsReceived;
        qDebug() << "总接收帧数:" << m_stats.totalFramesReceived;
        qDebug() << "检测到的丢包数:" << m_stats.lostPackets;

        qDebug() << "\n[诊断分析 - 传统模式]";
        qDebug() << "如果服务器发送了N个包（序列号0到N-1）：";
        qDebug() << "  理论最后序列号 = N - 1";
        qDebug() << "  实际最后序列号 =" << m_lastSequence;
        qDebug() << "  可能未统计的尾部丢包 ≈" << "理论值 - " << m_lastSequence << "- 1";
    }

    qDebug() << "FFT处理次数:" << m_stats.fftProcessCount;
    qDebug() << "FFT缓冲区: NS=" << m_fftWorker->getNSFFTBuffer().size() << "个结果, EW=" << m_fftWorker->getEWFFTBuffer().size() << "个结果";
    qDebug() << "=========================================\n";
    qDebug() << "[客户端] 停止接收";
}

// 重置传统模式状态
void UDPClientReceiver::resetTraditionalState()
{
    m_firstPacket = true;
    m_lastSequence = 0;
    m_nextExpectedFrame = 0;

    // 清空帧重组器（使用 cleanOldFrames(0) 清空所有帧）
    if (m_useSubPacketMode) {
        m_frameReassembler.cleanOldFrames(0);
    }

    qDebug() << "[客户端] 已重置传统模式状态";
}

// 重置新格式状态
void UDPClientReceiver::resetNewFormatState()
{
    m_lastNewFmtPacketNumber = 0;
    m_hasLastNewFmtPacketNumber = false;
    m_newFmtAnomalyCount = 0;

    // 清空新格式缓冲区
    m_newFormatNSBuffer.clear();
    m_newFormatEWBuffer.clear();
    m_newFormatNSBuffer.reserve(SAMPLES_PER_FRAME);
    m_newFormatEWBuffer.reserve(SAMPLES_PER_FRAME);
    m_newFormatPacketsReceived = 0;
    m_hasFirstTimestamp = false;
    m_packetsSinceLoopDetect = 0;
    memset(&m_firstTimestampRaw, 0, sizeof(m_firstTimestampRaw));

    qDebug() << "[客户端] 已重置新格式状态";
}

void UDPClientReceiver::clear()
{
    m_ewDataBuffer.clear();
    m_nsDataBuffer.clear();
    m_tdDataBuffer.clear();
}

void UDPClientReceiver::resetPipeline()
{
    qDebug() << "[UDPClientReceiver] 重置整个数据管线";

    // 1. 清空原始数据ringbuffer
    m_ewDataBuffer.clear();
    m_nsDataBuffer.clear();
    m_tdDataBuffer.clear();

    // 2. 清空FFT处理器内部状态 + FFT结果缓冲区
    if (m_fftWorker) {
        QMetaObject::invokeMethod(m_fftWorker, "resetAll", Qt::QueuedConnection);
    }
}

// [注意] onReadyRead 已移除，UDP 接收现在由 UdpReceiverWorker 在独立线程处理

void UDPClientReceiver::onRecvWorkerInitialized(bool success)
{
    if (success) {
        qDebug() << "[客户端] UDP 接收线程初始化成功";
    } else {
        qWarning() << "[客户端] UDP 接收线程初始化失败";
    }
}

// 处理传统模式的数据包
void UDPClientReceiver::processTraditionalPacket(const QByteArray &datagram)
{
    // 解析包头
    QByteArray data = datagram;
    QDataStream stream(&data, QIODevice::ReadOnly);
    stream.setByteOrder(QDataStream::LittleEndian);

    quint32 packetSequence;
    quint32 framesInPacket;
    stream >> packetSequence;
    stream >> framesInPacket;

        // 检测丢包
        if (!m_firstPacket) {
            quint32 expectedSequence = m_lastSequence + 1;
            if (packetSequence != expectedSequence) {
                int lost = packetSequence - expectedSequence;
                m_stats.lostPackets += lost;
                qWarning() << "[客户端] 检测到丢包!期望序列号:" << expectedSequence
                           << "实际:" << packetSequence << "丢失:" << lost;
            }
        } else {
            m_firstPacket = false;
            // 诊断：记录第一个包的序列号
            qDebug() << "[客户端诊断] 收到第一个包，序列号:" << packetSequence
                     << "（如果不是0，说明开头有丢包）";
        }
        m_lastSequence = packetSequence;

        // 解析帧数据 - 24位格式：按通道配置解包（每个采样点3字节）
// 兼容两种顺序：
// - 双通道（EW+NS）：[EW0][NS0][EW1][NS1]...
// - 三通道（TD+EW+NS）：[TD0][EW0][NS0][TD1][EW1][NS1]...
        for (quint32 i = 0; i < framesInPacket; i++) {
            const bool enableTD = (m_currentChannelConfig.length() >= 3 && m_currentChannelConfig[2] == '1');

            QVector<quint32> ewFrame(SAMPLES_PER_FRAME);
            QVector<quint32> nsFrame(SAMPLES_PER_FRAME);
            QVector<quint32> tdFrame;  // 按需创建
            if (enableTD) {
                tdFrame.resize(SAMPLES_PER_FRAME);
            }

            for (int j = 0; j < SAMPLES_PER_FRAME; j++) {
                if (enableTD) {
                    // 读取3字节TD数据
                    quint8 tdByte0, tdByte1, tdByte2;
                    stream >> tdByte0 >> tdByte1 >> tdByte2;
                    tdFrame[j] = (static_cast<quint32>(tdByte0) |
                                 (static_cast<quint32>(tdByte1) << 8) |
                                 (static_cast<quint32>(tdByte2) << 16));
                }

                // 读取3字节EW数据
                quint8 ewByte0, ewByte1, ewByte2;
                stream >> ewByte0 >> ewByte1 >> ewByte2;
                ewFrame[j] = (static_cast<quint32>(ewByte0) |
                             (static_cast<quint32>(ewByte1) << 8) |
                             (static_cast<quint32>(ewByte2) << 16));

                // 读取3字节NS数据
                quint8 nsByte0, nsByte1, nsByte2;
                stream >> nsByte0 >> nsByte1 >> nsByte2;
                nsFrame[j] = (static_cast<quint32>(nsByte0) |
                             (static_cast<quint32>(nsByte1) << 8) |
                             (static_cast<quint32>(nsByte2) << 16));
            }

            // 只存入原始数据ringbuffer，不在接收线程中做FFT（避免阻塞接收）
            m_ewDataBuffer.push(ewFrame);
            m_nsDataBuffer.push(nsFrame);
            if (enableTD) {
                m_tdDataBuffer.push(tdFrame);
            }

            m_stats.totalFramesReceived++;
        }

    emit dataReceived(framesInPacket);
    emit statsUpdated(m_stats);

    // FFT处理由定时器异步执行，不在接收线程中处理
}

// 处理帧内分包模式的数据包
void UDPClientReceiver::processSubPacket(const QByteArray &datagram)
{
    // 检查数据包大小
    if (datagram.size() < static_cast<int>(sizeof(PacketHeader))) {
        qWarning() << "[客户端] 子包太小，丢弃";
        return;
    }

    // 解析包头
    const PacketHeader *header = reinterpret_cast<const PacketHeader*>(datagram.constData());
    QByteArray data = datagram.mid(sizeof(PacketHeader));

    // 验证数据长度
    if (data.size() != header->dataLength) {
        qWarning() << "[客户端] 子包数据长度不匹配";
        return;
    }

    // 添加到重组器
    bool frameComplete = m_frameReassembler.addSubPacket(*header, data);

    if (frameComplete) {
        // 提取完整帧（24位数据存储为quint32）
        QVector<quint32> ewFrame, nsFrame;
        if (m_frameReassembler.extractFrame(header->frameNumber, ewFrame, nsFrame)) {
            // 检测丢帧
            if (header->frameNumber != m_nextExpectedFrame) {
                int lostFrames = header->frameNumber - m_nextExpectedFrame;
                m_stats.lostPackets += lostFrames * PacketConfig::SUBPACKETS_PER_FRAME;
                qWarning() << "[客户端] 检测到丢帧! 期望帧号:" << m_nextExpectedFrame
                           << "实际:" << header->frameNumber << "丢失:" << lostFrames << "帧";
            }
            m_nextExpectedFrame = header->frameNumber + 1;

            // 更新最后收到的序列号（用帧号表示）
            m_lastSequence = header->frameNumber;

            // 存入ringbuffer
            m_ewDataBuffer.push(std::move(ewFrame));
            m_nsDataBuffer.push(std::move(nsFrame));

            m_stats.totalFramesReceived++;
            m_currentFrameCount++;

            emit dataReceived(1);

            // 检查是否满批
            checkAndFetchBatch();

            // 更频繁地清理旧帧，避免内存累积影响性能
            // 从每100帧改为每10帧清理一次，保留最近20帧
            if (m_stats.totalFramesReceived % 10 == 0) {
                m_frameReassembler.cleanOldFrames(20);
            }
        }
    }
}

void UDPClientReceiver::checkAndFetchBatch()
{
    // 每满5帧取出一次
    if (m_currentFrameCount >= FRAMES_PER_BATCH) {
        // 计算从接收开始到现在的时间(微秒精度)
        qint64 fetchTimeNs = m_elapsedTimer.nsecsElapsed();
        qint64 startTimeNs = m_stats.receiveStartTime * 1000000;  // 毫秒转纳秒
        double timeSinceStartMs = (fetchTimeNs - startTimeNs) / 1000000.0;  // 纳秒转毫秒,保留小数

        m_stats.lastFetchTime = m_elapsedTimer.elapsed();
        m_stats.fetchCount++;

        // 优化: 减少日志输出频率,避免qDebug拖慢性能
        // 只在每10次取出时输出一次日志
        if (m_stats.fetchCount % 10 == 0) {
            qDebug() << QString("[客户端] 第%1次取出:累积%2帧,从接收开始到取出耗时:%3ms")
                        .arg(m_stats.fetchCount)
                        .arg(m_currentFrameCount)
                        .arg(timeSinceStartMs, 0, 'f', 2);  // 保留2位小数

            qDebug() << QString("  [统计] 总接收:%1帧,%2包,丢包:%3,缓冲区EW:%4帧,NS:%5帧")
                        .arg(m_stats.totalFramesReceived)
                        .arg(m_stats.totalPacketsReceived)
                        .arg(m_stats.lostPackets)
                        .arg(m_ewDataBuffer.size())
                        .arg(m_nsDataBuffer.size());

            // 性能监控：监控重组器pending帧数量，如果过多说明有丢包累积
            int pendingFrames = m_frameReassembler.pendingFrameCount();
            if (pendingFrames > 10) {
                qWarning() << QString("  [警告] 重组器待处理帧数过多: %1 (可能有丢包累积)")
                              .arg(pendingFrames);
            }
        }

        emit batchReady(m_currentFrameCount);
        emit statsUpdated(m_stats);

        // 重置计数器和接收开始时间,准备下一批
        m_currentFrameCount = 0;
        m_stats.receiveStartTime = 0;  // 重置,下一批数据重新计时
    }
}

void UDPClientReceiver::processBatchFFT()
{
    // 异步FFT处理，避免阻塞接收线程

    // 性能测量：记录FFT处理开始时间
    QElapsedTimer fftTimer;
    fftTimer.start();

    // 检查是否有数据进行FFT处理
    // 为了保证轨迹能完整绘制到文件末尾，有数据就处理，不再强制等待凑满 FRAMES_PER_BATCH 帧
    const bool enableTD = (m_currentChannelConfig.length() >= 3 && m_currentChannelConfig[2] == '1');

    if (m_nsDataBuffer.size() < 1 || m_ewDataBuffer.size() < 1 || (enableTD && m_tdDataBuffer.size() < 1)) {
        return; // 当前没有可处理的数据
    }

    // 诊断：检查buffer积压情况
    static QElapsedTimer bufferCheckTimer;
    static bool bufferCheckInit = false;
    if (!bufferCheckInit) {
        bufferCheckTimer.start();
        bufferCheckInit = true;
    }

    if (bufferCheckTimer.elapsed() > 2000) {  // 每2秒检查一次
        int nsDataSize = m_nsDataBuffer.size();
        int ewDataSize = m_ewDataBuffer.size();
        int tdDataSize = enableTD ? m_tdDataBuffer.size() : 0;
        int nsFFTSize = m_fftWorker->getNSFFTBuffer().size();
        int ewFFTSize = m_fftWorker->getEWFFTBuffer().size();
        int tdFFTSize = enableTD ? m_fftWorker->getTDFFTBuffer().size() : 0;

        double dataUsage = qMax(qMax(nsDataSize, ewDataSize), tdDataSize) * 100.0 / RINGBUF_CAPACITY;
        double fftUsage = qMax(qMax(nsFFTSize, ewFFTSize), tdFFTSize) * 100.0 / 1000;

        if (dataUsage > 50.0 || fftUsage > 50.0) {
            qWarning() << QString("[Buffer积压] Data: NS=%1 EW=%2 TD=%3 (max=%4, %5%) FFT: NS=%6 EW=%7 TD=%8 (%9%)")
                          .arg(nsDataSize).arg(ewDataSize).arg(tdDataSize)
                          .arg(qMax(qMax(nsDataSize, ewDataSize), tdDataSize)).arg(dataUsage, 0, 'f', 1)
                          .arg(nsFFTSize).arg(ewFFTSize).arg(tdFFTSize).arg(fftUsage, 0, 'f', 1);
        }
        bufferCheckTimer.restart();
    }

    // 确定实际要处理的帧数
	// 原错误代码（初始化列表形式，部分环境不支持）
//int framesToProcess = qMin({m_nsDataBuffer.size(), m_ewDataBuffer.size(), FRAMES_PER_BATCH});

// 修正后代码（嵌套 qMin 调用，兼容所有 Qt 版本）
int framesToProcess = qMin(qMin(m_nsDataBuffer.size(), m_ewDataBuffer.size()), FRAMES_PER_BATCH);
    if (enableTD) {
        framesToProcess = qMin(framesToProcess, m_tdDataBuffer.size());
    }
    if (!m_isReceiving) {
        // 停止时处理所有剩余数据
        framesToProcess = qMin(m_nsDataBuffer.size(), m_ewDataBuffer.size());
        if (enableTD) {
            framesToProcess = qMin(framesToProcess, m_tdDataBuffer.size());
        }
    }

    // 从RingBuffer中取出数据（24位数据存储为quint32） - 预分配内存提高效率
    QVector<quint32> nsData, ewData, tdData;
    nsData.reserve(framesToProcess * SAMPLES_PER_FRAME);
    ewData.reserve(framesToProcess * SAMPLES_PER_FRAME);
    if (enableTD) {
        tdData.reserve(framesToProcess * SAMPLES_PER_FRAME);
    }

    // 批量取出数据 - 优化内存分配
    int processedFrames = 0;
    for (int i = 0; i < framesToProcess; i++) {
        QVector<quint32> nsFrame, ewFrame, tdFrame;
        const bool ok2 = (m_nsDataBuffer.tryPop(nsFrame) && m_ewDataBuffer.tryPop(ewFrame));
        const bool ok3 = (!enableTD) || m_tdDataBuffer.tryPop(tdFrame);
        if (ok2 && ok3) {
            // 直接追加，避免中间拷贝
            nsData.append(nsFrame);
            ewData.append(ewFrame);
            if (enableTD) {
                tdData.append(tdFrame);
            }
            processedFrames++;
        }

    }

    // 如果成功取出数据,进行FFT处理
    if (processedFrames > 0) {
        // 将数据传递给FFT线程处理（异步）
        if (enableTD) {
            QMetaObject::invokeMethod(m_fftWorker, "processData",
                                      Qt::QueuedConnection,
                                      Q_ARG(QVector<quint32>, tdData),
                                      Q_ARG(QVector<quint32>, nsData),
                                      Q_ARG(QVector<quint32>, ewData),
                                      Q_ARG(QDateTime, m_lastPacketTimestamp));
        } else {
            QMetaObject::invokeMethod(m_fftWorker, "processData",
                                      Qt::QueuedConnection,
                                      Q_ARG(QVector<quint32>, nsData),
                                      Q_ARG(QVector<quint32>, ewData),
                                      Q_ARG(QDateTime, m_lastPacketTimestamp));
        }

        // 增加FFT处理计数
        m_stats.fftProcessCount++;

        // 首次执行时打印日志
        if (m_stats.fftProcessCount == 1) {
            qDebug() << "[FFT] 首次FFT处理启动! 处理了" << processedFrames << "帧数据";
        }

        // 每100次打印一次统计
        if (m_stats.fftProcessCount % 100 == 0) {
            qDebug() << QString("[FFT] 已提交%1次处理请求，本次%2帧")
                        .arg(m_stats.fftProcessCount)
                        .arg(processedFrames)
                        << "NS_max=" << m_fftWorker->getNSMaxValue()
                        << "EW_max=" << m_fftWorker->getEWMaxValue();
        }

        // 停止时处理剩余数据的日志
        if (!m_isReceiving && processedFrames > 0) {
            qDebug() << QString("[FFT] 停止时提交剩余 %1 帧处理")
                        .arg(processedFrames);
        }

        // 每500次打印一次缓冲区状态
        if (m_stats.fftProcessCount % 500 == 0) {
            qDebug() << QString("  缓冲区状态: Data(NS=%1,EW=%2,TD=%3)")
                        .arg(m_nsDataBuffer.size())
                        .arg(m_ewDataBuffer.size())
                        .arg(enableTD ? m_tdDataBuffer.size() : 0);
        }
        // ========== 频谱绘制触发逻辑：每12次FFT构造一条NS+EW频谱 ==========
        static int s_fftResultCount = 0;
        s_fftResultCount++;
    
        // 每12次 FFT 处理，尝试构造一条 NS+EW 的 DrawableSpectrum
        if (m_fftWorker) {
            DrawableSpectrum drawable;
            drawable.frameIndex = m_stats.fftProcessCount;  // 或者用 s_fftResultCount
            drawable.hasEW = false;
            drawable.hasNS = false;
            drawable.hasTD = false;
    
            // 从 FFT 结果 RingBuffer 中各取一个最新结果（非阻塞）
            RingBuffer<SpectrumResult>& nsFFT = m_fftWorker->getNSFFTBuffer();
            RingBuffer<SpectrumResult>& ewFFT = m_fftWorker->getEWFFTBuffer();
    
            SpectrumResult nsResult, ewResult;
            if (nsFFT.tryPop(nsResult)) {
                drawable.nsSpectrum = std::move(nsResult);
                drawable.hasNS = true;
            }
            if (ewFFT.tryPop(ewResult)) {
                drawable.ewSpectrum = std::move(ewResult);
                drawable.hasEW = true;
            }
    
            // 只有两路都拿到了，才入队，保证两通道“同时绘制”
            if (drawable.hasNS && drawable.hasEW) {
                QMutexLocker locker(&m_drawQueueMutex);
    
                while (m_drawQueue.size() >= m_maxDrawQueueSize) {
                    m_drawQueue.dequeue();  // 丢掉最老的
                }
    
                m_drawQueue.enqueue(std::move(drawable));
                // 不一定要发 drawableSpectrumReady，用不上可以删
            }
        }
        // ========== 频谱绘制触发逻辑结束 ==========    
    }
}

// 处理新格式的数据包（955字节）
void UDPClientReceiver::processNewFormatPacket(const QByteArray &datagram)
{
    m_lastUDPPacketTime.start();

    if (datagram.size() != NewPacketConfig::TOTAL_PACKET_SIZE) {
        return;
    }

    if (m_packetQueue) {
        QByteArray copy = datagram;
        m_packetQueue->enqueue(std::move(copy));

        int queueDepth = m_packetQueue->size();
        static qint64 lastDepthWarn = 0;
        if (queueDepth > 2048) {
            qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastDepthWarn > 5000) {
                qDebug() << "[UDP] Queue depth:" << queueDepth;
                lastDepthWarn = now;
            }
        }
    } else {
        processNewFormatPacketLegacy(datagram);
    }
}

void UDPClientReceiver::processNewFormatPacketLegacy(const QByteArray &datagram)
{
    using namespace NewPacketConfig;

    // 更新最后收到UDP包的时间（无论是数据包还是心跳包）
    m_lastUDPPacketTime.start();

    // 验证包大小
    if (datagram.size() != TOTAL_PACKET_SIZE) {
        qWarning() << "[客户端-新格式] 数据包大小错误: 期望" << TOTAL_PACKET_SIZE
                   << "字节, 实际" << datagram.size() << "字节";
        return;
    }

    // 检查是否是心跳包
    if (isHeartbeatPacket(datagram)) {
        emit udpHeartbeatReceived();
        qDebug() << "[UDPClientReceiver] 收到UDP心跳包";
        return;  // 心跳包不处理数据
    }

    // 解析包头
    const NewPacketHeader *header = reinterpret_cast<const NewPacketHeader*>(datagram.constData());

    // 验证帧头
    if (header->magic != MAGIC_NUMBER) {
        qWarning() << "[客户端-新格式] 帧头错误: 期望" << QString::number(MAGIC_NUMBER, 16)
                   << "实际" << QString::number(header->magic, 16);
        return;
    }

    // 解析时间戳并转换为东八区本地时间
    m_lastPacketTimestamp = parseTimestamp(header->timestamp);

    // ========== 丢包/乱序/重复检测逻辑 ==========
    quint32 currentPacketNumber = header->frameSequence;

    if (!m_hasLastNewFmtPacketNumber) {
        // ========== 第一个包：初始化 ==========
        m_lastNewFmtPacketNumber = currentPacketNumber;
        m_hasLastNewFmtPacketNumber = true;

        if (currentPacketNumber != 0) {
            qDebug() << QString("[新格式-首包] 包号不是0: %1 (可能开头有丢包)")
                        .arg(currentPacketNumber);
        }

    } else {
        // ========== 后续包：检测 ==========

        // 1. 判断重复包（最优先，避免后续误判）
        if (currentPacketNumber == m_lastNewFmtPacketNumber) {
            // 重复包：丢弃
            m_stats.duplicatePackets++;
            m_newFmtAnomalyCount++;

            if (m_newFmtAnomalyCount % PACKET_ANOMALY_LOG_INTERVAL == 1) {
                qDebug() << QString("[新格式-重复] 包号:%1 (累计异常:%2)")
                            .arg(currentPacketNumber)
                            .arg(m_newFmtAnomalyCount);
            }

            return;  // 丢弃重复包，不处理数据
        }

        // 2. 判断推进方向
        if (currentPacketNumber > m_lastNewFmtPacketNumber) {
            // ========== 推进包号（正常包或丢包） ==========
            quint32 expectedPacketNumber = m_lastNewFmtPacketNumber + 1;
            qint32 diff = (qint32)(currentPacketNumber - expectedPacketNumber);

            if (diff == 0) {
                // 正常包：current == expected
                // 不打印日志（正常情况）

            } else if (diff > 0) {
                // 丢包：current > expected
                if (diff < PACKET_GAP_THRESHOLD) {
                    // 正常丢包
                    m_stats.lostPackets += diff;
                    m_newFmtAnomalyCount++;

                    if (m_newFmtAnomalyCount % PACKET_ANOMALY_LOG_INTERVAL == 1) {
                        qWarning() << QString("[新格式-丢包] 期望:%1 实际:%2 丢失:%3包 (累计异常:%4)")
                                      .arg(expectedPacketNumber)
                                      .arg(currentPacketNumber)
                                      .arg(diff)
                                      .arg(m_newFmtAnomalyCount);
                    }
                } else {
                    // 异常gap：触发重同步
                    m_stats.resyncCount++;
                    m_newFmtAnomalyCount++;

                    qWarning() << QString("[新格式-重同步] 异常gap=%1 (期望:%2 实际:%3)")
                                  .arg(diff)
                                  .arg(expectedPacketNumber)
                                  .arg(currentPacketNumber);

                    if (m_reorderBuffer) {
                        m_reorderBuffer->clear();
                        qDebug() << "[新格式-重同步] 已清空ReorderBuffer";
                    }

                    qDebug() << "[新格式-重同步] 完成，从包号" << currentPacketNumber << "继续";
                }
            }

            // 推进 last（max-last 策略）
            m_lastNewFmtPacketNumber = currentPacketNumber;

        } else {
            // ========== 乱序包（current < last） ==========
            qint32 backwardDiff = (qint32)(m_lastNewFmtPacketNumber - currentPacketNumber);

            if (backwardDiff < PACKET_GAP_THRESHOLD) {
                // 正常乱序
                m_stats.outOfOrderPackets++;
                m_newFmtAnomalyCount++;

                if (m_newFmtAnomalyCount % PACKET_ANOMALY_LOG_INTERVAL == 1) {
                    qDebug() << QString("[新格式-乱序] 当前最大包号:%1 收到:%2 落后:%3包 (累计异常:%4)")
                                  .arg(m_lastNewFmtPacketNumber)
                                  .arg(currentPacketNumber)
                                  .arg(backwardDiff)
                                  .arg(m_newFmtAnomalyCount);
                }

                // 不更新 last（保持max-last）
                // 继续处理数据（不丢弃）

            } else {
                // 异常乱序：触发重同步
                m_stats.resyncCount++;
                m_newFmtAnomalyCount++;

                qWarning() << QString("[新格式-重同步] 异常乱序 当前最大:%1 收到:%2 落后:%3包")
                              .arg(m_lastNewFmtPacketNumber)
                              .arg(currentPacketNumber)
                              .arg(backwardDiff);

                if (m_reorderBuffer) {
                    m_reorderBuffer->clear();
                    qDebug() << "[新格式-重同步] 已清空ReorderBuffer";
                }
            }
        }
    }
    // ========== 检测逻辑结束 ==========

    // 提取数据部分（不含包头）
    QByteArray packetData = datagram.mid(HEADER_SIZE, DATA_SIZE);

    // 使用 ChannelSeparator 分离通道
    ChannelConfig config = ChannelConfig::fromString(m_currentChannelConfig);
    if (!config.isValid()) {
        qWarning() << "[UDPClientReceiver] 无效的通道配置:" << m_currentChannelConfig;
        return;
    }

    QVector<quint32> ewData, nsData, tdData;
    bool success = ChannelSeparator::separateFromByteArray(
        packetData,
        ChannelSeparator::Format_NewPacket,
        config,
        ewData,
        nsData,
        tdData  // TD通道输出
    );

    if (!success) {
        qWarning() << "[UDPClientReceiver] 通道分离失败";
        return;
    }

    // 自动检测通道配置：根据实际分离出的数据判断是否有TD通道
    // 如果tdData不为空，说明实际数据是三通道的，自动更新配置
    static bool configDetected = false;
    if (!configDetected && !tdData.isEmpty()) {
        QString detectedConfig = "111";  // 检测到三通道
        if (m_currentChannelConfig != detectedConfig) {
            qDebug() << "[UDPClientReceiver] 自动检测到三通道数据，更新配置:"
                     << m_currentChannelConfig << "->" << detectedConfig;
            m_currentChannelConfig = detectedConfig;
            config = ChannelConfig::fromString(m_currentChannelConfig);
        }
        configDetected = true;
    }

    // 存入RingBuffer
    if (config.enableEW && !ewData.isEmpty()) {
        m_ewDataBuffer.push(ewData);
    }
    if (config.enableNS && !nsData.isEmpty()) {
        m_nsDataBuffer.push(nsData);
    }
    if (config.enableReserved && !tdData.isEmpty()) {
        m_tdDataBuffer.push(tdData);
    }

    // 【删除】不在这里统计 totalPacketsReceived（已在 onReadyRead 统计）
    m_stats.totalFramesReceived++;  // 保留帧统计

    // 每100包输出一次日志
    if (m_stats.totalPacketsReceived % 100 == 0) {
        qDebug() << QString("[UDPClientReceiver] 已接收 %1 包, 缓冲区: NS=%2 EW=%3 TD=%4")
                    .arg(m_stats.totalPacketsReceived)
                    .arg(m_nsDataBuffer.size())
                    .arg(m_ewDataBuffer.size())
                    .arg(m_tdDataBuffer.size());
    }

    emit dataReceived(1);
    emit statsUpdated(m_stats);

    // 【可选扩展点】如果需要恢复 ReorderBuffer，在这里插入包：
    // m_reorderBuffer->insertPacket(currentPacketNumber, packetData);
}
static inline bool seqAhead32(quint32 a, quint32 b) {
    return (qint32)(a - b) > 0;
}

void UDPClientReceiver::onSeparatedFrames(const SeparatedFrame& frame)
{
    // 通道分离已在 PacketWorker 线程完成，此处仅做：seq/时间戳追踪、配置通知、ringbuffer push、统计
    if (!m_hasLastNewFmtPacketNumber) {
        m_lastNewFmtPacketNumber = frame.seq;
        m_hasLastNewFmtPacketNumber = true;
    } else if (seqAhead32(frame.seq, m_lastNewFmtPacketNumber)) {
        m_lastNewFmtPacketNumber = frame.seq;
    }

    if (!m_hasFirstTimestamp) {
        m_firstTimestampRaw = frame.timestamp;
        m_hasFirstTimestamp = true;
        m_packetsSinceLoopDetect = 0;
        qDebug() << "[UDPClientReceiver] 记录第一个数据包时间戳";
    } else {
        m_packetsSinceLoopDetect++;
        if (m_packetsSinceLoopDetect > 10000 &&
            memcmp(frame.timestamp.raw, m_firstTimestampRaw.raw, 8) == 0) {
            qDebug() << "[UDPClientReceiver] 检测到数据循环重启（收到" << m_packetsSinceLoopDetect << "包后），清空图表";
            m_packetsSinceLoopDetect = 0;
            emit dataLoopRestarted();
        }
    }

    using namespace NewPacketConfig;

    static int lastChannelCount = -1;
    static quint8 lastSampleRateId = 255;
    bool channelChanged = (frame.channelCount != lastChannelCount);
    bool rateChanged = (frame.sampleRate != lastSampleRateId);

    if (channelChanged || rateChanged) {
        lastChannelCount = frame.channelCount;
        lastSampleRateId = frame.sampleRate;
        double fsHz = 4000000.0;
        if (frame.sampleRate == SampleRateId::RATE_250K) fsHz = 250000.0;
        else if (frame.sampleRate == SampleRateId::RATE_4M) fsHz = 4000000.0;
        setSampleRate(fsHz);  // 已改为 QueuedConnection，不阻塞主线程
        emit packetConfigChanged(frame.channelCount, fsHz);
        qDebug() << "[UDPClientReceiver] 检测到配置变化:"
                 << "channelCount=" << frame.channelCount
                 << "sampleRateId=" << frame.sampleRate
                 << "fsHz=" << fsHz;
    }

    QString channelConfigForParsing = (frame.channelCount == 3) ? QString("111") : QString("110");
    static QString lastDetectedConfig;
    if (lastDetectedConfig != channelConfigForParsing) {
        if (!lastDetectedConfig.isEmpty()) {
            qDebug() << "[UDPClientReceiver] 通道配置切换:"
                     << lastDetectedConfig << "->" << channelConfigForParsing
                     << "(包头channelCount=" << frame.channelCount << ")";
        }
        lastDetectedConfig = channelConfigForParsing;
        m_currentChannelConfig = channelConfigForParsing;
    }

    if (!frame.ewData.isEmpty()) {
        m_ewDataBuffer.push(frame.ewData);
    }
    if (!frame.nsData.isEmpty()) {
        m_nsDataBuffer.push(frame.nsData);
    }
    if (!frame.tdData.isEmpty()) {
        m_tdDataBuffer.push(frame.tdData);
    }

    m_stats.totalFramesReceived++;
    m_receivedFrameIndex++;

    static int uiCounter = 0;
    uiCounter++;
    if (uiCounter >= 10) {
        uiCounter = 0;
        updateDiagnosticStats();
        emit dataReceived(1);
        emit statsUpdated(m_stats);
    }
}

void UDPClientReceiver::onPacketParsed(const ParsedPacket& packet)
{
    (void)packet;
    // 已废弃：通道分离与入队改由 onSeparatedFrames 处理（分离在 PacketWorker 线程）
}
bool UDPClientReceiver::popDrawableSpectrum(DrawableSpectrum& result)
{
    QMutexLocker locker(&m_drawQueueMutex);
    if (m_drawQueue.isEmpty()) {
        return false;
    }
    result = m_drawQueue.dequeue();
    return true;
}
void UDPClientReceiver::onHeartbeat(quint32 seq)
{
}

void UDPClientReceiver::onPacketsLost(quint32 count)
{
    m_stats.lostPackets += count;
}

// [诊断] 更新诊断统计字段
void UDPClientReceiver::updateDiagnosticStats()
{
    // 更新 PacketQueue 统计
    if (m_packetQueue) {
        m_stats.queueCurrent = m_packetQueue->size();
        m_stats.queueMaxDepth = m_packetQueue->maxDepth();
        m_stats.queueDropped = m_packetQueue->droppedByQueue();
        m_stats.queueEnqueued = m_packetQueue->totalEnqueued();
        m_stats.queueDequeued = m_packetQueue->totalDequeued();
    } else {
        m_stats.queueCurrent = 0;
        m_stats.queueMaxDepth = 0;
        m_stats.queueDropped = 0;
        m_stats.queueEnqueued = 0;
        m_stats.queueDequeued = 0;
    }

    // 更新 UDP drain 统计（从 recvWorker 获取）
    if (m_recvWorker) {
        m_stats.readyReadTotalDrains = m_recvWorker->getTotalDrains();
        m_stats.readyReadTotalPackets = m_recvWorker->getTotalPackets();
        if (m_stats.readyReadTotalDrains > 0) {
            double avgBatch = static_cast<double>(m_stats.readyReadTotalPackets) / m_stats.readyReadTotalDrains;
            m_stats.readyReadMaxBatch = static_cast<int>(avgBatch + 0.5);  // 近似最大值
        }
    } else {
        m_stats.readyReadTotalDrains = 0;
        m_stats.readyReadTotalPackets = 0;
        m_stats.readyReadMaxBatch = 0;
    }

    // slowDrains 和 maxDrainMs 不再统计（recvWorker 内部不计时，避免性能影响）
    m_stats.slowDrains = 0;
    m_stats.readyReadMaxDrainMs = 0.0;
}

// ============================================================================
// play.md协议扩展方法实现
// ============================================================================

void UDPClientReceiver::setChannelConfig(const QString& channelConfig)
{
    m_currentChannelConfig = channelConfig;
    qDebug() << "[UDPClientReceiver] 设置通道配置:" << channelConfig;
}

void UDPClientReceiver::setUDPHeartbeatTimeout(int timeoutMs)
{
    m_udpTimeoutMs = timeoutMs;
    qDebug() << "[UDPClientReceiver] 设置UDP超时:" << timeoutMs << "ms";
}

void UDPClientReceiver::setSampleRate(double fsHz)
{
    if (m_fftWorker) {
        QMetaObject::invokeMethod(m_fftWorker, "setSampleRate",
                                  Qt::QueuedConnection,
                                  Q_ARG(double, fsHz));
        qDebug() << "[UDPClientReceiver] 设置采样率:" << fsHz << "Hz";
    }
}

void UDPClientReceiver::setAnalysisHop(int hop)
{
    if (m_fftWorker) {
        QMetaObject::invokeMethod(m_fftWorker, "setAnalysisHop",
                                  Qt::QueuedConnection,
                                  Q_ARG(int, hop));
        qDebug() << "[UDPClientReceiver] 设置分析步进:" << hop << "样本";
    }
}

void UDPClientReceiver::checkUDPTimeout()
{
    if (!m_isReceiving) {
        return;
    }

    // 检查是否超时
    if (m_lastUDPPacketTime.isValid() && m_lastUDPPacketTime.elapsed() > m_udpTimeoutMs) {
        emit udpLinkLost();
        emit udpTimeout();
    }
}

bool UDPClientReceiver::isHeartbeatPacket(const QByteArray& datagram)
{
    using namespace NewPacketConfig;

    // 验证包大小
    if (datagram.size() != TOTAL_PACKET_SIZE) {
        return false;
    }

    // 解析包头
    const NewPacketHeader *header = reinterpret_cast<const NewPacketHeader*>(datagram.constData());

    // 检查帧类型：0=心跳包
    if (header->frameType == FrameType::HEARTBEAT) {
        return true;
    }

    return false;
}

QDateTime UDPClientReceiver::parseTimestamp(const PacketTimestamp& ts)
{
    // 位压缩时间戳解析（大端序 uint64）
    // bit 63-57: year(7) | bit 56-53: month(4) | bit 52-48: day(5)
    // bit 47-43: hour(5) | bit 42-37: minute(6) | bit 36-31: second(6)
    // bit 30-0: cnt_10ns(31)
    const quint8* b = ts.raw;

    int year   = 2000 + (b[0] >> 1);
    int month  = ((b[0] & 0x1) << 3) | ((b[1] >> 5) & 0x7);
    int day    = b[1] & 0x1F;
    int hour   = (b[2] >> 3) & 0x1F;
    int minute = ((b[2] & 0x7) << 3) | ((b[3] >> 5) & 0x7);
    int second = ((b[3] & 0x1F) << 1) | ((b[4] >> 7) & 0x1);

    quint32 cnt_10ns = (quint32(b[4] & 0x7F) << 24) |
                       (quint32(b[5]) << 16) |
                       (quint32(b[6]) << 8) |
                       quint32(b[7]);
    int ns_total = cnt_10ns * 10;  // 10纳秒 → 纳秒
    int ms = ns_total / 1000000;
    int us = (ns_total % 1000000) / 1000;
    // ns = ns_total % 1000;  // QTime只支持毫秒精度

    // 将UTC时间转换为东八区时间（UTC+8）
    hour += 8;
    if (hour >= 24) {
        hour -= 24;
        day += 1;
        // 简化处理：未考虑月末跨月
    }

    QDate date(year, month, day);
    QTime time(hour, minute, second, ms);

    // 构建东八区时间（UTC+8）
    QDateTime localTime(date, time, QTimeZone::fromSecondsAheadOfUtc(8 * 3600));

    return localTime;
}

void UDPClientReceiver::onOrderedPacketReady(quint32 packetNumber, const QByteArray& data)
{
    // 从ReorderBuffer收到有序的数据包
    qDebug() << "[UDPClientReceiver] 收到重排序包，包号:" << packetNumber << "数据大小:" << data.size();

    // 使用ChannelSeparator分离通道
    ChannelConfig config = ChannelConfig::fromString(m_currentChannelConfig);

    if (!config.isValid()) {
        qWarning() << "[UDPClientReceiver] 无效的通道配置:" << m_currentChannelConfig;
        return;
    }

    QVector<quint32> ewData, nsData, tdData;
    bool success = ChannelSeparator::separateFromByteArray(
        data,
        ChannelSeparator::Format_NewPacket,
        config,
        ewData,
        nsData,
        tdData
    );

    if (!success) {
        qWarning() << "[UDPClientReceiver] 通道分离失败，包号:" << packetNumber;
        return;
    }

    qDebug() << "[UDPClientReceiver] 通道分离成功，EW:" << ewData.size() << "NS:" << nsData.size() << "TD:" << tdData.size();

    // 自动检测通道配置：根据实际分离出的数据判断是否有TD通道
    static bool configDetected3 = false;
    if (!configDetected3 && !tdData.isEmpty()) {
        QString detectedConfig = "111";  // 检测到三通道
        if (m_currentChannelConfig != detectedConfig) {
            qDebug() << "[UDPClientReceiver] 自动检测到三通道数据，更新配置:"
                     << m_currentChannelConfig << "->" << detectedConfig;
            m_currentChannelConfig = detectedConfig;
            config = ChannelConfig::fromString(m_currentChannelConfig);
        }
        configDetected3 = true;
    }

    // 存入RingBuffer
    if (config.enableEW && !ewData.isEmpty()) {
        m_ewDataBuffer.push(ewData);
    }
    if (config.enableNS && !nsData.isEmpty()) {
        m_nsDataBuffer.push(nsData);
    }
    if (config.enableReserved && !tdData.isEmpty()) {
        m_tdDataBuffer.push(tdData);
    }

    m_stats.totalFramesReceived++;

    // 每10帧输出一次日志
    if (m_stats.totalFramesReceived % 10 == 0) {
        qDebug() << QString("[UDPClientReceiver] 已接收 %1 帧（重排序后）, 缓冲区: NS=%2 EW=%3")
                    .arg(m_stats.totalFramesReceived)
                    .arg(m_nsDataBuffer.size())
                    .arg(m_ewDataBuffer.size());
    }

    emit dataReceived(1);
    emit statsUpdated(m_stats);
}

// bool UDPClientReceiver::saveSTFTDataToCSV(const QString &outputDir, const QString &fileBaseName)
// {
//     if (!m_fftWorker) {
//         qWarning() << "[UDPClientReceiver] FFTWorker未初始化，无法保存STFT数据";
//         return false;
//     }

//     bool success = true;

//     // 保存NS通道数据
//     FFTProcessor* nsProcessor = m_fftWorker->getNSFFTProcessor();
//     if (nsProcessor) {
//         if (!nsProcessor->saveSTFTToCSV("NS", outputDir, fileBaseName)) {
//             qWarning() << "[UDPClientReceiver] NS通道STFT数据保存失败";
//             success = false;
//         }
//     }

//     // 保存EW通道数据
//     FFTProcessor* ewProcessor = m_fftWorker->getEWFFTProcessor();
//     if (ewProcessor) {
//         if (!ewProcessor->saveSTFTToCSV("EW", outputDir, fileBaseName)) {
//             qWarning() << "[UDPClientReceiver] EW通道STFT数据保存失败";
//             success = false;
//         }
//     }

//     return success;
// }
