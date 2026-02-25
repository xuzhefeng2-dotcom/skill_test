#ifndef PACKETFORMAT_H
#define PACKETFORMAT_H

#include <QtGlobal>

/**
 * @brief UDP数据包格式定义
 *
 * 帧格式：[帧头4B][帧类型1B][帧序号4B][通道数1B][采样率1B][时间8B][数据936B]
 * 总包大小：955字节
 * - 包头：19字节
 * - 数据：936字节（312个采样点，每通道ringbuf，fft）
 *
 * 帧类型：0=心跳包（数据全为0），1=数据包
 * 通道数：2或3
 * 采样率：0=250kHz，1=4MHz
 */

// 时间戳结构（8字节，位压缩格式）
// 位布局（大端序 uint64）：
//   bit 63-57: year   (7位, 0-127, 表示20xx年)
//   bit 56-53: month  (4位, 1-12)
//   bit 52-48: day    (5位, 1-31)
//   bit 47-43: hour   (5位, 0-23, UTC时间)
//   bit 42-37: minute (6位, 0-59)
//   bit 36-31: second (6位, 0-59)
//   bit 30-0:  cnt_10ns (31位, 10纳秒计数器)
#pragma pack(push, 1)
struct PacketTimestamp {
    quint8 raw[8];  // 位压缩的8字节原始数据
};
#pragma pack(pop)

static_assert(sizeof(PacketTimestamp) == 8, "PacketTimestamp must be 8 bytes");

// 数据包头结构（19字节）
// [帧头4B][帧类型1B][帧序号4B][通道数1B][采样率1B][时间8B]
#pragma pack(push, 1)
struct NewPacketHeader {
    quint32 magic;            // 帧头：固定 0x1F1F1F1F
    quint8  frameType;        // 帧类型：0=心跳包，1=数据包
    quint32 frameSequence;    // 帧序号：uint32，从0开始递增
    quint8  channelCount;     // 通道数：2或3
    quint8  sampleRate;       // 采样率分辨率：0=250kHz，1=4MHz
    PacketTimestamp timestamp; // 时间戳（8字节）
};
#pragma pack(pop)

static_assert(sizeof(NewPacketHeader) == 19, "NewPacketHeader must be 19 bytes");

// 帧类型常量
namespace FrameType {
    constexpr quint8 HEARTBEAT = 0;    // 心跳包（数据全为0）
    constexpr quint8 DATA = 1;         // 数据包
}

// 采样率常量
namespace SampleRateId {
    constexpr quint8 RATE_250K = 0;    // 250kHz
    constexpr quint8 RATE_4M = 1;      // 4MHz
}

// 配置常量（24位数据格式）
namespace NewPacketConfig {
    constexpr quint32 MAGIC_NUMBER = 0x1F1F1F1F;                // 帧头固定值
    constexpr int HEADER_SIZE = sizeof(NewPacketHeader);        // 19字节
    constexpr int DATA_SIZE = 936;                              // 每包数据大小（字节）
    constexpr int TOTAL_PACKET_SIZE = HEADER_SIZE + DATA_SIZE;  // 955字节

    constexpr int BYTES_PER_SAMPLE = 3;                         // 每个通道采样点3字节（24位）
    constexpr int CHANNELS_PER_POINT = 3;                       // 每个采样点3通道（NS+EW+TD）
    constexpr int BYTES_PER_POINT = BYTES_PER_SAMPLE * CHANNELS_PER_POINT;  // 每个完整采样点9字节
    constexpr int SAMPLES_PER_PACKET = DATA_SIZE / BYTES_PER_POINT;  // 每包采样点数：104个三通道采样点
    constexpr int SAMPLES_PER_FRAME = 5000;                     // 每帧采样点数
    constexpr int PACKETS_PER_FRAME = (SAMPLES_PER_FRAME + SAMPLES_PER_PACKET - 1) / SAMPLES_PER_PACKET;  // 每帧需要49个包

    constexpr int FRAME_INTERVAL_MS = 20;                       // 帧间间隔：20ms（帧内连续发送）
}

// =============== 旧的帧内分包格式（已弃用，保留兼容性） ===============
#pragma pack(push, 1)
struct PacketHeader {
    quint32 frameNumber;      // 帧序号（全局递增）
    quint16 subPacketIndex;   // 子包索引（从0开始）
    quint16 totalSubPackets;  // 该帧的总子包数
    quint8  channelId;        // 通道标识：0=EW, 1=NS
    quint8  reserved1;        // 保留字段
    quint16 dataLength;       // 本包数据段长度（字节数）
    quint32 reserved2;        // 保留字段
    quint32 reserved3;        // 保留字段
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 20, "PacketHeader must be 20 bytes");

namespace PacketConfig {
    constexpr int HEADER_SIZE = sizeof(PacketHeader);
    constexpr int MAX_PACKET_SIZE = 1400;
    constexpr int MAX_DATA_PER_PACKET = MAX_PACKET_SIZE - HEADER_SIZE;
    constexpr int SAMPLES_PER_FRAME = 5000;
    constexpr int BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2;
    constexpr int SUBPACKETS_PER_CHANNEL = (BYTES_PER_FRAME + MAX_DATA_PER_PACKET - 1) / MAX_DATA_PER_PACKET;
    constexpr int SUBPACKETS_PER_FRAME = SUBPACKETS_PER_CHANNEL * 2;
}

enum ChannelId : quint8 {
    CHANNEL_EW = 0,
    CHANNEL_NS = 1
};

#endif // PACKETFORMAT_H
