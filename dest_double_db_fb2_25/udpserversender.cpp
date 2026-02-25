#include "udpserversender.h"
#include <QDebug>
#include <QDataStream>
#include <thread>
#include <chrono>

UDPServerSender::UDPServerSender(QObject *parent)
    : QObject(parent)
    , m_socket(nullptr)
    , m_port(0)
    , m_clientPort(0)
    , m_dataReader(nullptr)
    , m_sendTimer(nullptr)
    , m_isSending(false)
    , m_currentFrameIndex(0)
    , m_totalFrames(0)
    , m_packetSequence(0)
    , m_useSubPacketMode(false)  // 默认使用传统模式（一帧一包）
    , m_useNewPacketFormat(false)  // 默认不使用新格式
    , m_currentSampleOffset(0)
{
    m_socket = new QUdpSocket(this);
    m_sendTimer = new QTimer(this);
    m_sendTimer->setInterval(SEND_INTERVAL_MS);  // 20ms帧间间隔
    connect(m_sendTimer, &QTimer::timeout, this, &UDPServerSender::sendNextBatch);
}

UDPServerSender::~UDPServerSender()
{
    stopSending();
    if (m_dataReader) {
        delete m_dataReader;
    }
}

bool UDPServerSender::initialize(quint16 port)
{
    m_port = port;
    if (!m_socket->bind(QHostAddress::Any, port)) {
        emit error(QString("无法绑定端口 %1: %2").arg(port).arg(m_socket->errorString()));
        return false;
    }

    qDebug() << "[服务器] 已绑定端口" << port;
    return true;
}

bool UDPServerSender::openCosFile(const QString &filePath)
{
    if (m_dataReader) {
        delete m_dataReader;
    }

    m_dataReader = new DataReader();
    if (!m_dataReader->openFile(filePath)) {
        emit error("无法打开COS文件: " + filePath);
        return false;
    }

    m_totalFrames = m_dataReader->totalFrames();
    m_currentFrameIndex = 0;
    m_packetSequence = 0;

    qDebug() << "[服务器] 打开文件成功,总帧数:" << m_totalFrames;
    emit fileOpened(filePath);
    return true;
}

void UDPServerSender::startSending()
{
    if (!m_dataReader) {
        emit error("请先打开COS文件");
        return;
    }

    if (m_clientAddress.isNull()) {
        emit error("客户端地址未设置");
        return;
    }

    m_isSending = true;
    m_currentFrameIndex = 0;
    m_packetSequence = 0;
    m_currentSampleOffset = 0;  // 重置采样点偏移
    m_dataReader->reset();

    m_elapsedTimer.start();
    m_sendTimer->start();
    emit sendingStarted();

    QString mode = m_useNewPacketFormat ? "新格式(955字节)" :
                   (m_useSubPacketMode ? "帧内分包" : "传统模式");
    qDebug() << "[服务器] 开始发送数据到" << m_clientAddress.toString() << ":" << m_clientPort
             << "模式:" << mode;
}

void UDPServerSender::stopSending()
{
    m_sendTimer->stop();
    m_isSending = false;
    emit sendingStopped();
    qDebug() << "[服务器] 停止发送";
}

void UDPServerSender::setClientAddress(const QHostAddress &address, quint16 port)
{
    m_clientAddress = address;
    m_clientPort = port;
    qDebug() << "[服务器] 设置客户端地址:" << address.toString() << ":" << port;
}

void UDPServerSender::sendNextBatch()
{
    if (!m_isSending || !m_dataReader) {
        return;
    }

    // 检查是否已发送完所有数据
    if (m_dataReader->atEnd()) {
        qint64 totalTime = m_elapsedTimer.elapsed();
        qDebug() << "\n========== 服务器发送统计摘要 ==========";
        qDebug() << "[服务器] 发送完成!";
        qDebug() << "  总帧数:" << m_currentFrameIndex;
        qDebug() << "  总包数:" << m_packetSequence << "（序列号0到" << (m_packetSequence - 1) << "）";
        qDebug() << "  总耗时:" << totalTime << "ms";
        qDebug() << "  平均速率:" << QString::number((m_currentFrameIndex * 1000.0) / totalTime, 'f', 1) << "帧/秒";

        QString mode = m_useNewPacketFormat ? "新格式(955字节)" :
                       (m_useSubPacketMode ? "帧内分包（无IP分片）" : "多帧打包（有IP分片）");
        qDebug() << "  发送模式:" << mode;
        if (!m_useSubPacketMode && !m_useNewPacketFormat) {
            qDebug() << "  每包帧数:" << FRAMES_PER_BATCH;
        }
        qDebug() << "=========================================\n";
        stopSending();
        emit allDataSent();
        return;
    }

    // 根据模式选择发送方式
    if (m_useNewPacketFormat) {
        // 新格式：批量发送完整一帧（10个包连续发送，0ms间隔）
        using namespace NewPacketConfig;
        for (int i = 0; i < PACKETS_PER_FRAME; i++) {
            sendNewFormatPacket();

            // 检查是否到达文件末尾
            if (m_dataReader->atEnd()) {
                break;
            }
        }
    } else if (m_useSubPacketMode) {
        // 帧内分包模式：读取1帧，拆分发送
        DataFrame frame = m_dataReader->readNextFrame();
        if (frame.isValid) {
            sendFrameWithSubPackets(frame, m_currentFrameIndex);
            m_currentFrameIndex++;
        }
    } else {
        // 传统模式：读取多帧，打包发送
        sendMultipleFrames();
    }

    // 更新进度
    int progress = (m_currentFrameIndex * 100) / m_totalFrames;
    emit progressChanged(progress);
}

// 传统模式：发送多帧数据
void UDPServerSender::sendMultipleFrames()
{
    // 准备数据包
    QByteArray datagram;
    QDataStream stream(&datagram, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);

    // 写入包头: 序列号(4字节) + 帧数(4字节)
    stream << m_packetSequence;
    stream << static_cast<quint32>(FRAMES_PER_BATCH);

    int framesInPacket = 0;

    // 读取并发送多帧数据
    for (int i = 0; i < FRAMES_PER_BATCH; i++) {
        if (m_dataReader->atEnd()) {
            break;
        }

        DataFrame frame = m_dataReader->readNextFrame();
        if (!frame.isValid) {
            break;
        }

        // 直接写入原始数据（交替格式：NS,EW,NS,EW...）
        datagram.append(frame.rawData);

        framesInPacket++;
        m_currentFrameIndex++;
    }

    // 发送数据包
    if (framesInPacket > 0) {
        qint64 bytesSent = m_socket->writeDatagram(datagram, m_clientAddress, m_clientPort);
        if (bytesSent < 0) {
            emit error("发送失败: " + m_socket->errorString());
        } else {
            emit packetSent(framesInPacket, m_packetSequence);

            // 每100个包输出一次日志
            if (m_packetSequence % 100 == 0) {
                qDebug() << "[服务器] 已发送包" << m_packetSequence << "包含" << framesInPacket << "帧,共" << m_currentFrameIndex << "帧";
            }
        }
        m_packetSequence++;
    }
}

// 帧内分包模式：发送单帧数据（拆分为多个小包）
void UDPServerSender::sendFrameWithSubPackets(const DataFrame &frame, quint32 frameNumber)
{
    // 发送EW通道（6个子包，连续发送）
    sendChannelData(frame.ewData, frameNumber, CHANNEL_EW);

    // 通道间短暂延迟（100微秒），降低12个包的突发程度
    // 这样每帧分为两个6包的小突发，降低瞬时压力
    std::this_thread::sleep_for(std::chrono::microseconds(100));

    // 发送NS通道（6个子包，连续发送）
    sendChannelData(frame.nsData, frameNumber, CHANNEL_NS);
}

// 发送单个通道的数据（拆分为多个小包，24位数据）
void UDPServerSender::sendChannelData(const QVector<quint32> &channelData, quint32 frameNumber, quint8 channelId)
{
    using namespace PacketConfig;

    // 计算需要的子包数量（24位数据，每个采样点3字节）
    int totalBytes = channelData.size() * 3;
    int totalSubPackets = (totalBytes + MAX_DATA_PER_PACKET - 1) / MAX_DATA_PER_PACKET;

    // 拆分发送
    for (int subPacketIdx = 0; subPacketIdx < totalSubPackets; subPacketIdx++) {
        // 计算当前子包的数据范围
        int startSample = subPacketIdx * (MAX_DATA_PER_PACKET / 3);  // 每个采样点3字节
        int remainingSamples = channelData.size() - startSample;
        int samplesInPacket = qMin(remainingSamples, MAX_DATA_PER_PACKET / 3);
        int dataBytes = samplesInPacket * 3;

        // 构建数据包
        QByteArray datagram;
        datagram.reserve(HEADER_SIZE + dataBytes);

        // 填充包头
        PacketHeader header;
        header.frameNumber = frameNumber;
        header.subPacketIndex = static_cast<quint16>(subPacketIdx);
        header.totalSubPackets = static_cast<quint16>(totalSubPackets);
        header.channelId = channelId;
        header.reserved1 = 0;
        header.dataLength = static_cast<quint16>(dataBytes);
        header.reserved2 = 0;
        header.reserved3 = 0;

        datagram.append(reinterpret_cast<const char*>(&header), HEADER_SIZE);

        // 填充数据段（24位数据，每个采样点3字节）
        for (int i = 0; i < samplesInPacket; i++) {
            quint32 sample = channelData[startSample + i];
            // 只取低24位，按小端序存储3字节
            quint8 byte0 = sample & 0xFF;
            quint8 byte1 = (sample >> 8) & 0xFF;
            quint8 byte2 = (sample >> 16) & 0xFF;
            datagram.append(byte0);
            datagram.append(byte1);
            datagram.append(byte2);
        }

        // 发送数据包
        qint64 bytesSent = m_socket->writeDatagram(datagram, m_clientAddress, m_clientPort);
        if (bytesSent < 0) {
            emit error(QString("发送失败: %1").arg(m_socket->errorString()));
            return;
        }

        m_packetSequence++;

        // 移除延迟：Windows下微秒级sleep精度差，反而增加丢包
        // 改用批量发送+通道间延迟的策略

        // 定期输出日志（每500个子包）
        if (m_packetSequence % 500 == 0) {
            qDebug() << QString("[服务器] 已发送 %1 个子包，当前帧 %2，通道 %3")
                        .arg(m_packetSequence)
                        .arg(frameNumber)
                        .arg(channelId == CHANNEL_EW ? "EW" : "NS");
        }
    }
}

// 新格式发送：发送单个数据包（955字节）
void UDPServerSender::sendNewFormatPacket()
{
    using namespace NewPacketConfig;

    // 如果需要读取新帧
    static DataFrame currentFrame;  // 保存当前帧数据
    static bool frameLoaded = false;

    if (m_currentSampleOffset == 0 || !frameLoaded) {
        if (m_dataReader->atEnd()) {
            return;
        }
        currentFrame = m_dataReader->readNextFrame();
        if (!currentFrame.isValid) {
            return;
        }
        frameLoaded = true;
    }

    // 准备数据包
    QByteArray datagram;
    datagram.reserve(TOTAL_PACKET_SIZE);

    // 构建包头（19字节）
    NewPacketHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = MAGIC_NUMBER;           // 0x1F1F1F1F
    header.frameType = FrameType::DATA;    // 数据包
    header.frameSequence = m_packetSequence;
    header.channelCount = 3;               // 默认3通道
    header.sampleRate = SampleRateId::RATE_250K;  // 默认250kHz

    // 添加包头
    datagram.append(reinterpret_cast<const char*>(&header), HEADER_SIZE);

    // 添加数据部分（1000字节，NS/EW交替，250个采样点）
    // 数据格式：NS0, EW0, NS1, EW1, ..., NS249, EW249
    for (int i = 0; i < SAMPLES_PER_PACKET; i++) {
        int sampleIndex = m_currentSampleOffset + i;

        // 检查越界
        if (sampleIndex >= SAMPLES_PER_FRAME) {
            qWarning() << "[服务器] 采样点索引越界:" << sampleIndex;
            break;
        }

        // 交替添加NS和EW数据（先NS后EW）
        quint16 nsSample = currentFrame.nsData[sampleIndex];
        quint16 ewSample = currentFrame.ewData[sampleIndex];

        datagram.append(reinterpret_cast<const char*>(&nsSample), sizeof(quint16));
        datagram.append(reinterpret_cast<const char*>(&ewSample), sizeof(quint16));
    }

    // 发送数据包
    qint64 bytesSent = m_socket->writeDatagram(datagram, m_clientAddress, m_clientPort);
    if (bytesSent < 0) {
        emit error(QString("发送失败: %1").arg(m_socket->errorString()));
        return;
    }

    // 更新状态
    m_currentSampleOffset += SAMPLES_PER_PACKET;

    // 检查是否完成一帧
    if (m_currentSampleOffset >= SAMPLES_PER_FRAME) {
        m_currentSampleOffset = 0;
        m_currentFrameIndex++;
        frameLoaded = false;

        // 每10帧输出一次日志
        if (m_currentFrameIndex % 10 == 0) {
            qDebug() << QString("[服务器-新格式] 已发送 %1 帧, 总包数: %2")
                        .arg(m_currentFrameIndex)
                        .arg(m_packetSequence + 1);
        }
    }

    emit packetSent(1, m_packetSequence);

    // 每100个包输出一次详细日志
    if (m_packetSequence % 100 == 0) {
        qDebug() << QString("[服务器-新格式] 包号:%1, 当前帧:%2, 帧内偏移:%3/%4, 包大小:%5字节")
                    .arg(m_packetSequence)
                    .arg(m_currentFrameIndex)
                    .arg(m_currentSampleOffset)
                    .arg(SAMPLES_PER_FRAME)
                    .arg(datagram.size());
    }

    m_packetSequence++;
}
