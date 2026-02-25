#ifndef CHANNELSEPARATOR_H
#define CHANNELSEPARATOR_H

#include <QVector>
#include <QtGlobal>
#include <QString>

/**
 * @brief 通道配置（三位二进制格式）
 *
 * 格式："ABC"（三位二进制字符串）
 * - A（第1位）：EW通道 (1=启用, 0=禁用)
 * - B（第2位）：NS通道 (1=启用, 0=禁用)
 * - C（第3位）：保留通道 (1=启用, 0=禁用，暂未使用)
 *
 * 示例：
 * - "000" -> 所有通道禁用
 * - "100" -> 仅EW通道
 * - "010" -> 仅NS通道
 * - "110" -> EW + NS双通道
 * - "111" -> 三通道全开
 */
struct ChannelConfig {
    bool enableEW;       // EW通道开关（第1位）
    bool enableNS;       // NS通道开关（第2位）
    bool enableReserved; // 保留通道开关（第3位，暂未使用）

    ChannelConfig()
        : enableEW(false), enableNS(false), enableReserved(false) {}

    ChannelConfig(bool ew, bool ns, bool reserved = false)
        : enableEW(ew), enableNS(ns), enableReserved(reserved) {}

    /**
     * @brief 从三位二进制字符串解析
     * @param channelStr 三位二进制字符串 "ABC"
     * @return ChannelConfig对象
     */
    static ChannelConfig fromString(const QString& channelStr);

    /**
     * @brief 转换为三位二进制字符串
     * @return 三位二进制字符串 "ABC"
     */
    QString toString() const;

    /**
     * @brief 是否为双通道模式（EW + NS）
     */
    bool isDualChannel() const { return enableEW && enableNS; }

    /**
     * @brief 是否为单通道模式
     */
    bool isSingleChannel() const {
        int count = (enableEW ? 1 : 0) + (enableNS ? 1 : 0) + (enableReserved ? 1 : 0);
        return count == 1;
    }

    /**
     * @brief 是否所有通道都禁用
     */
    bool isAllDisabled() const {
        return !enableEW && !enableNS && !enableReserved;
    }

    /**
     * @brief 获取启用的通道数量
     */
    int enabledChannelCount() const {
        return (enableEW ? 1 : 0) + (enableNS ? 1 : 0) + (enableReserved ? 1 : 0);
    }

    /**
     * @brief 验证配置是否有效
     */
    bool isValid() const {
        return enabledChannelCount() > 0;
    }
};

/**
 * @brief 通道分离器（无状态，纯静态方法）
 *
 * 支持16位和24位数据格式
 */
class ChannelSeparator
{
public:
    enum DataFormat {
        Format_Traditional,  // 传统格式：EW0, NS0, EW1, NS1, ... (16位)
        Format_NewPacket     // 新格式（955字节）：NS0, EW0, NS1, EW1, ... (24位)
    };

    // 16位数据分离（传统格式）
    static bool separateTraditionalFormat(
        const quint16* interleavedData,
        int sampleCount,
        const ChannelConfig& config,
        QVector<quint16>& ewOut,
        QVector<quint16>& nsOut
    );

    // 16位数据分离（新格式）
    static bool separateNewFormat(
        const quint16* interleavedData,
        int sampleCount,
        const ChannelConfig& config,
        QVector<quint16>& nsOut,
        QVector<quint16>& ewOut
    );

    // 24位数据分离（新格式）
    static bool separateNewFormat24(
        const QByteArray& data,
        const ChannelConfig& config,
        QVector<quint32>& nsOut,
        QVector<quint32>& ewOut
    );

    // 通用接口（16位）
    static bool separate(
        const quint16* interleavedData,
        int sampleCount,
        DataFormat format,
        const ChannelConfig& config,
        QVector<quint16>& ewOut,
        QVector<quint16>& nsOut
    );

    // 从QByteArray分离（16位）
    static bool separateFromByteArray(
        const QByteArray& data,
        DataFormat format,
        const ChannelConfig& config,
        QVector<quint16>& ewOut,
        QVector<quint16>& nsOut
    );

    // 从QByteArray分离（24位）
    static bool separateFromByteArray(
        const QByteArray& data,
        DataFormat format,
        const ChannelConfig& config,
        QVector<quint32>& ewOut,
        QVector<quint32>& nsOut
    );

    // 从QByteArray分离（24位，支持三通道）
    static bool separateFromByteArray(
        const QByteArray& data,
        DataFormat format,
        const ChannelConfig& config,
        QVector<quint32>& ewOut,
        QVector<quint32>& nsOut,
        QVector<quint32>& tdOut  // TD通道输出
    );

    static bool validateDataLength(int dataLength, int expectedSampleCount);

private:
    ChannelSeparator() = delete;
    ~ChannelSeparator() = delete;
    ChannelSeparator(const ChannelSeparator&) = delete;
    ChannelSeparator& operator=(const ChannelSeparator&) = delete;
};

#endif // CHANNELSEPARATOR_H
