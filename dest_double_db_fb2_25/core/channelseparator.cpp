#include "channelseparator.h"
#include <QDebug>

// ============================================================================
// ChannelConfig 实现
// ============================================================================

ChannelConfig ChannelConfig::fromString(const QString& channelStr)
{
    if (channelStr.length() != 3) {
        qWarning() << "[ChannelConfig] 无效的通道字符串（必须是3位）:" << channelStr;
        return ChannelConfig(false, false, false);
    }

    for (int i = 0; i < 3; i++) {
        if (channelStr[i] != '0' && channelStr[i] != '1') {
            qWarning() << "[ChannelConfig] 通道字符串包含非法字符:" << channelStr;
            return ChannelConfig(false, false, false);
        }
    }

    bool ew = (channelStr[0] == '1');
    bool ns = (channelStr[1] == '1');
    bool reserved = (channelStr[2] == '1');

    return ChannelConfig(ew, ns, reserved);
}

QString ChannelConfig::toString() const
{
    QString result;
    result += enableEW ? '1' : '0';
    result += enableNS ? '1' : '0';
    result += enableReserved ? '1' : '0';
    return result;
}

// ============================================================================
// ChannelSeparator 实现
// ============================================================================

bool ChannelSeparator::separateTraditionalFormat(
    const quint16* interleavedData,
    int sampleCount,
    const ChannelConfig& config,
    QVector<quint16>& ewOut,
    QVector<quint16>& nsOut)
{
    if (!interleavedData || sampleCount <= 0) {
        qWarning() << "[ChannelSeparator] 无效的输入参数";
        return false;
    }

    if (!config.isValid()) {
        qWarning() << "[ChannelSeparator] 无效的通道配置（所有通道都禁用）";
        return false;
    }

    if (config.enableReserved) {
        qWarning() << "[ChannelSeparator] 保留通道暂未实现，将被忽略";
    }

    ewOut.clear();
    nsOut.clear();

    if (config.enableEW) {
        ewOut.reserve(sampleCount);
    }
    if (config.enableNS) {
        nsOut.reserve(sampleCount);
    }

    for (int i = 0; i < sampleCount; i++) {
        if (config.enableEW) {
            ewOut.append(interleavedData[i * 2]);
        }
        if (config.enableNS) {
            nsOut.append(interleavedData[i * 2 + 1]);
        }
    }

    return true;
}

bool ChannelSeparator::separateNewFormat(
    const quint16* interleavedData,
    int sampleCount,
    const ChannelConfig& config,
    QVector<quint16>& nsOut,
    QVector<quint16>& ewOut)
{
    if (!interleavedData || sampleCount <= 0) {
        qWarning() << "[ChannelSeparator] 无效的输入参数";
        return false;
    }

    if (!config.isValid()) {
        qWarning() << "[ChannelSeparator] 无效的通道配置（所有通道都禁用）";
        return false;
    }

    if (config.enableReserved) {
        qWarning() << "[ChannelSeparator] 保留通道暂未实现，将被忽略";
    }

    nsOut.clear();
    ewOut.clear();

    if (config.enableNS) {
        nsOut.reserve(sampleCount);
    }
    if (config.enableEW) {
        ewOut.reserve(sampleCount);
    }

    for (int i = 0; i < sampleCount; i++) {
        if (config.enableNS) {
            nsOut.append(interleavedData[i * 2]);
        }
        if (config.enableEW) {
            ewOut.append(interleavedData[i * 2 + 1]);
        }
    }

    return true;
}

bool ChannelSeparator::separate(
    const quint16* interleavedData,
    int sampleCount,
    DataFormat format,
    const ChannelConfig& config,
    QVector<quint16>& ewOut,
    QVector<quint16>& nsOut)
{
    switch (format) {
    case Format_Traditional:
        return separateTraditionalFormat(interleavedData, sampleCount, config, ewOut, nsOut);

    case Format_NewPacket:
        return separateNewFormat(interleavedData, sampleCount, config, nsOut, ewOut);

    default:
        qWarning() << "[ChannelSeparator] 未知的数据格式:" << format;
        return false;
    }
}

bool ChannelSeparator::separateFromByteArray(
    const QByteArray& data,
    DataFormat format,
    const ChannelConfig& config,
    QVector<quint16>& ewOut,
    QVector<quint16>& nsOut)
{
    if (data.size() % 2 != 0) {
        qWarning() << "[ChannelSeparator] 数据长度不是2的倍数:" << data.size();
        return false;
    }

    int sampleCount = data.size() / 4;
    const quint16* dataPtr = reinterpret_cast<const quint16*>(data.constData());

    return separate(dataPtr, sampleCount, format, config, ewOut, nsOut);
}

// 24位数据版本
bool ChannelSeparator::separateFromByteArray(
    const QByteArray& data,
    DataFormat format,
    const ChannelConfig& config,
    QVector<quint32>& ewOut,
    QVector<quint32>& nsOut)
{
    Q_UNUSED(format);  // 24位格式固定为新格式

    if (!config.isValid()) {
        qWarning() << "[ChannelSeparator] 无效的通道配置";
        return false;
    }

    // 检查是否启用TD通道（第3位）
    bool hasTD = config.enableReserved;
    int bytesPerSample = hasTD ? 9 : 6;  // 三通道9字节，双通道6字节

    if (data.size() % bytesPerSample != 0) {
        qWarning() << "[ChannelSeparator] 数据长度不匹配:" << data.size()
                   << "字节，期望" << bytesPerSample << "字节的倍数";
        return false;
    }

    ewOut.clear();
    nsOut.clear();

    const quint8* bytes = reinterpret_cast<const quint8*>(data.constData());
    int sampleCount = data.size() / bytesPerSample;

    ewOut.reserve(sampleCount);
    nsOut.reserve(sampleCount);

    if (hasTD) {
        // 三通道格式：TD(3字节) + EW(3字节) + NS(3字节) = 9字节/采样点
        for (int i = 0; i < sampleCount; i++) {
            int offset = i * 9;

            // TD通道（前3字节）- 暂时跳过，不输出
            // quint32 tdSample = (static_cast<quint32>(bytes[offset]) |
            //                    (static_cast<quint32>(bytes[offset + 1]) << 8) |
            //                    (static_cast<quint32>(bytes[offset + 2]) << 16));

            // EW通道（中间3字节）
            quint32 ewSample = (static_cast<quint32>(bytes[offset + 3]) |
                               (static_cast<quint32>(bytes[offset + 4]) << 8) |
                               (static_cast<quint32>(bytes[offset + 5]) << 16));
            ewOut.append(ewSample);

            // NS通道（后3字节）
            quint32 nsSample = (static_cast<quint32>(bytes[offset + 6]) |
                               (static_cast<quint32>(bytes[offset + 7]) << 8) |
                               (static_cast<quint32>(bytes[offset + 8]) << 16));
            nsOut.append(nsSample);
        }
    } else {
        // 双通道格式：EW(3字节) + NS(3字节) = 6字节/采样点
        for (int i = 0; i < sampleCount; i++) {
            int offset = i * 6;

            // EW通道（前3字节）
            quint32 ewSample = (static_cast<quint32>(bytes[offset]) |
                               (static_cast<quint32>(bytes[offset + 1]) << 8) |
                               (static_cast<quint32>(bytes[offset + 2]) << 16));
            ewOut.append(ewSample);

            // NS通道（后3字节）
            quint32 nsSample = (static_cast<quint32>(bytes[offset + 3]) |
                               (static_cast<quint32>(bytes[offset + 4]) << 8) |
                               (static_cast<quint32>(bytes[offset + 5]) << 16));
            nsOut.append(nsSample);
        }
    }

    return true;
}

// 24位数据版本（支持三通道输出）
bool ChannelSeparator::separateFromByteArray(
    const QByteArray& data,
    DataFormat format,
    const ChannelConfig& config,
    QVector<quint32>& ewOut,
    QVector<quint32>& nsOut,
    QVector<quint32>& tdOut)
{
    Q_UNUSED(format);  // 24位格式固定为新格式

    if (!config.isValid()) {
        qWarning() << "[ChannelSeparator] 无效的通道配置";
        return false;
    }

    // 检查是否启用TD通道（第3位）
    bool hasTD = config.enableReserved;
    int bytesPerSample = hasTD ? 9 : 6;  // 三通道9字节，双通道6字节

    if (data.size() % bytesPerSample != 0) {
        qWarning() << "[ChannelSeparator] 数据长度不匹配:" << data.size()
                   << "字节，期望" << bytesPerSample << "字节的倍数";
        return false;
    }

    ewOut.clear();
    nsOut.clear();
    tdOut.clear();

    const quint8* bytes = reinterpret_cast<const quint8*>(data.constData());
    int sampleCount = data.size() / bytesPerSample;

    ewOut.reserve(sampleCount);
    nsOut.reserve(sampleCount);
    if (hasTD) {
        tdOut.reserve(sampleCount);
    }

    if (hasTD) {
        // 三通道格式：TD(3字节) + EW(3字节) + NS(3字节) = 9字节/采样点
        for (int i = 0; i < sampleCount; i++) {
            int offset = i * 9;

            // TD通道（前3字节）
            quint32 tdSample = (static_cast<quint32>(bytes[offset]) |
                               (static_cast<quint32>(bytes[offset + 1]) << 8) |
                               (static_cast<quint32>(bytes[offset + 2]) << 16));
            tdOut.append(tdSample);

            // EW通道（中间3字节）
            quint32 ewSample = (static_cast<quint32>(bytes[offset + 3]) |
                               (static_cast<quint32>(bytes[offset + 4]) << 8) |
                               (static_cast<quint32>(bytes[offset + 5]) << 16));
            ewOut.append(ewSample);

            // NS通道（后3字节）
            quint32 nsSample = (static_cast<quint32>(bytes[offset + 6]) |
                               (static_cast<quint32>(bytes[offset + 7]) << 8) |
                               (static_cast<quint32>(bytes[offset + 8]) << 16));
            nsOut.append(nsSample);
        }
    } else {
        // 双通道格式：EW(3字节) + NS(3字节) = 6字节/采样点
        for (int i = 0; i < sampleCount; i++) {
            int offset = i * 6;

            // EW通道（前3字节）
            quint32 ewSample = (static_cast<quint32>(bytes[offset]) |
                               (static_cast<quint32>(bytes[offset + 1]) << 8) |
                               (static_cast<quint32>(bytes[offset + 2]) << 16));
            ewOut.append(ewSample);

            // NS通道（后3字节）
            quint32 nsSample = (static_cast<quint32>(bytes[offset + 3]) |
                               (static_cast<quint32>(bytes[offset + 4]) << 8) |
                               (static_cast<quint32>(bytes[offset + 5]) << 16));
            nsOut.append(nsSample);
        }
    }

    return true;
}

bool ChannelSeparator::validateDataLength(int dataLength, int expectedSampleCount)
{
    int expectedLength = expectedSampleCount * 4;
    return (dataLength == expectedLength);
}
