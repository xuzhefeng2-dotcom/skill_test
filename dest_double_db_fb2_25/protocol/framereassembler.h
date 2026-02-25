#ifndef FRAMEREASSEMBLER_H
#define FRAMEREASSEMBLER_H

#include <QHash>
#include <QVector>
#include <QDebug>
#include "packetformat.h"

/**
 * @brief 帧重组器 - 接收子包并重组为完整帧
 */
class FrameReassembler
{
public:
    FrameReassembler() = default;

    /**
     * @brief 添加接收到的子包
     * @param header 包头
     * @param data 数据段
     * @return true表示该帧已经完整，可以提取
     */
    bool addSubPacket(const PacketHeader &header, const QByteArray &data);

    /**
     * @brief 提取完整的帧数据（24位数据存储为quint32）
     * @param frameNumber 帧号
     * @param ewData 输出：EW通道数据
     * @param nsData 输出：NS通道数据
     * @return true表示成功提取
     */
    bool extractFrame(quint32 frameNumber, QVector<quint32> &ewData, QVector<quint32> &nsData);

    /**
     * @brief 清理旧的不完整帧（防止内存泄漏）
     * @param keepCount 保留最近N帧
     */
    void cleanOldFrames(int keepCount = 200);

    /**
     * @brief 获取当前正在重组的帧数量
     */
    int pendingFrameCount() const { return m_frames.size(); }

private:
    // 单个通道的重组数据
    struct ChannelData {
        QHash<quint16, QByteArray> subPackets;  // 子包索引 -> 数据（QHash性能优于QMap）
        quint16 totalSubPackets = 0;            // 总子包数
        bool isComplete = false;                // 是否完整

        bool checkComplete() {
            if (totalSubPackets == 0) return false;
            isComplete = (subPackets.size() == totalSubPackets);
            return isComplete;
        }
    };

    // 单帧的重组数据
    struct FrameData {
        ChannelData ewChannel;
        ChannelData nsChannel;

        bool isBothChannelsComplete() const {
            return ewChannel.isComplete && nsChannel.isComplete;
        }
    };

    QHash<quint32, FrameData> m_frames;  // 帧号 -> 帧数据（QHash O(1)查找，优于QMap O(log n)）

    // 统计信息
    int m_totalFramesReassembled = 0;
    int m_totalSubPacketsReceived = 0;
};

#endif // FRAMEREASSEMBLER_H
