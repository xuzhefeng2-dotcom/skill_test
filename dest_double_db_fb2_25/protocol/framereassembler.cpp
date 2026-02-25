#include "framereassembler.h"

bool FrameReassembler::addSubPacket(const PacketHeader &header, const QByteArray &data)
{
    m_totalSubPacketsReceived++;

    // 获取或创建帧数据
    FrameData &frame = m_frames[header.frameNumber];

    // 选择通道
    ChannelData *channel = nullptr;
    if (header.channelId == CHANNEL_EW) {
        channel = &frame.ewChannel;
    } else if (header.channelId == CHANNEL_NS) {
        channel = &frame.nsChannel;
    } else {
        qWarning() << "[重组器] 未知通道ID:" << header.channelId;
        return false;
    }

    // 设置总子包数（第一次收到时）
    if (channel->totalSubPackets == 0) {
        channel->totalSubPackets = header.totalSubPackets;
    } else if (channel->totalSubPackets != header.totalSubPackets) {
        qWarning() << "[重组器] 帧" << header.frameNumber
                   << "通道" << (int)header.channelId
                   << "总子包数不一致!";
        return false;
    }

    // 存储子包数据
    channel->subPackets[header.subPacketIndex] = data;

    // 检查通道是否完整
    channel->checkComplete();

    // 返回整个帧是否完整
    if (frame.isBothChannelsComplete()) {
        m_totalFramesReassembled++;
        return true;
    }

    return false;
}

bool FrameReassembler::extractFrame(quint32 frameNumber, QVector<quint32> &ewData, QVector<quint32> &nsData)
{
    auto it = m_frames.find(frameNumber);
    if (it == m_frames.end()) {
        return false;
    }

    const FrameData &frame = it.value();
    if (!frame.isBothChannelsComplete()) {
        return false;
    }

    // 重组EW通道（24位数据，3字节/采样点）
    ewData.clear();
    ewData.reserve(PacketConfig::SAMPLES_PER_FRAME);
    for (int i = 0; i < frame.ewChannel.totalSubPackets; i++) {
        const QByteArray &subPacket = frame.ewChannel.subPackets[i];
        const quint8 *bytes = reinterpret_cast<const quint8*>(subPacket.constData());
        int byteCount = subPacket.size();

        // 每3字节构成一个24位采样点
        for (int j = 0; j < byteCount; j += 3) {
            if (j + 2 < byteCount) {
                quint32 sample = (static_cast<quint32>(bytes[j]) |
                                 (static_cast<quint32>(bytes[j + 1]) << 8) |
                                 (static_cast<quint32>(bytes[j + 2]) << 16));
                ewData.append(sample);
            }
        }
    }

    // 重组NS通道（24位数据，3字节/采样点）
    nsData.clear();
    nsData.reserve(PacketConfig::SAMPLES_PER_FRAME);
    for (int i = 0; i < frame.nsChannel.totalSubPackets; i++) {
        const QByteArray &subPacket = frame.nsChannel.subPackets[i];
        const quint8 *bytes = reinterpret_cast<const quint8*>(subPacket.constData());
        int byteCount = subPacket.size();

        // 每3字节构成一个24位采样点
        for (int j = 0; j < byteCount; j += 3) {
            if (j + 2 < byteCount) {
                quint32 sample = (static_cast<quint32>(bytes[j]) |
                                 (static_cast<quint32>(bytes[j + 1]) << 8) |
                                 (static_cast<quint32>(bytes[j + 2]) << 16));
                nsData.append(sample);
            }
        }
    }

    // 删除已提取的帧
    m_frames.erase(it);

    return true;
}

void FrameReassembler::cleanOldFrames(int keepCount)
{
    if (m_frames.size() <= keepCount) {
        return;
    }

    // 找出最大的帧号（QHash没有lastKey，需要遍历）
    if (m_frames.isEmpty()) return;
	keepCount = qMax(keepCount, 200);
    quint32 maxFrameNumber = 0;
    for (auto it = m_frames.constBegin(); it != m_frames.constEnd(); ++it) {
        if (it.key() > maxFrameNumber) {
            maxFrameNumber = it.key();
        }
    }

    quint32 minFrameToKeep = (maxFrameNumber > static_cast<quint32>(keepCount))
                               ? (maxFrameNumber - keepCount)
                               : 0;

    // 删除旧帧
    auto it = m_frames.begin();
    while (it != m_frames.end()) {
        if (it.key() < minFrameToKeep) {
            it = m_frames.erase(it);
        } else {
            ++it;
        }
    }
}
