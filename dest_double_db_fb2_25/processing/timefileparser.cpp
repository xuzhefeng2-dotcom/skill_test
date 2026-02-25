#include "timefileparser.h"
#include <QFile>
#include <QDataStream>
#include <QTextStream>
#include <QDebug>
#include <QFileInfo>

// Qt 5/6 兼容性处理
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    #include <QTextCodec>
#endif

TimeFileParser::TimeFileParser(QObject *parent)
    : QObject(parent)
{
}

TimeFileParser::~TimeFileParser()
{
}

QString TimestampData::toString() const
{
    // 格式：2026年01月23日 11:52:44.89350835
    // 秒位包含小数部分（10纳秒精度，8位小数）
    double preciseSecond = second + (cnt_10ns * 1e-8);

    return QString("20%1年%2月%3日 %4:%5:%6")
        .arg(year, 2, 10, QChar('0'))
        .arg(month, 2, 10, QChar('0'))
        .arg(day, 2, 10, QChar('0'))
        .arg(hour, 2, 10, QChar('0'))
        .arg(minute, 2, 10, QChar('0'))
        .arg(preciseSecond, 11, 'f', 8, QChar('0'));
}

TimestampData TimeFileParser::parseTimestamp(quint64 rawData)
{
    TimestampData ts;

    // 解析位域（参考MATLAB脚本）
    ts.year      = (rawData >> 57) & 0x7F;        // bit 57-63 (7位)
    ts.month     = (rawData >> 53) & 0x0F;        // bit 53-56 (4位)
    ts.day       = (rawData >> 48) & 0x1F;        // bit 48-52 (5位)
    ts.hour      = (rawData >> 43) & 0x1F;        // bit 43-47 (5位)
    ts.minute    = (rawData >> 37) & 0x3F;        // bit 37-42 (6位)
    ts.second    = (rawData >> 31) & 0x3F;        // bit 31-36 (6位)
    ts.cnt_10ns  = rawData & 0x7FFFFFFF;          // bit 0-30 (31位)

    // ✅ 关键：将UTC时间转换为东八区时间（UTC+8）
    ts.hour += 8;
    if (ts.hour >= 24) {
        ts.hour -= 24;
        ts.day += 1;
        // 注意：这里简化处理，没有考虑月末跨月的情况
        // 如果需要严格处理，应该使用QDateTime
    }

    // 计算总秒数（用于计算时间间隔）
    ts.totalSeconds = calculateTotalSeconds(ts);
    ts.diffMicroseconds = 0.0;  // 初始化为0，后续计算

    return ts;
}

double TimeFileParser::calculateTotalSeconds(const TimestampData &ts)
{
    // 从当天00:00:00开始的总秒数
    return ts.hour * 3600.0 + ts.minute * 60.0 + ts.second + ts.cnt_10ns * 1e-8;
}

bool TimeFileParser::parseTimeFile(const QString &inputFilePath, const QString &outputFilePath)
{
    // 1. 打开输入文件
    QFile inputFile(inputFilePath);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        QString errorMsg = QString("无法打开文件: %1").arg(inputFilePath);
        emit error(errorMsg);
        emit parseFinished(false, errorMsg);
        return false;
    }

    // 2. 读取所有64位时间戳数据（大端序）
    QDataStream in(&inputFile);
    in.setByteOrder(QDataStream::BigEndian);  // 设置为大端序

    m_timestamps.clear();

    qint64 fileSize = inputFile.size();
    int totalCount = fileSize / 8;  // 每个时间戳8字节

    qDebug() << "[TimeFileParser] 文件大小:" << fileSize << "字节";
    qDebug() << "[TimeFileParser] 预计时间戳数量:" << totalCount;

    // 3. 读取并解析所有时间戳
    int count = 0;
    while (!in.atEnd()) {
        quint64 rawData;
        in >> rawData;

        if (in.status() != QDataStream::Ok) {
            break;
        }

        TimestampData ts = parseTimestamp(rawData);
        m_timestamps.append(ts);

        count++;
        if (count % 10000 == 0) {
            emit progressChanged(count, totalCount);
        }
    }

    inputFile.close();

    qDebug() << "[TimeFileParser] 成功读取" << count << "个时间戳数据";

    if (m_timestamps.isEmpty()) {
        QString errorMsg = "文件中没有有效的时间戳数据";
        emit error(errorMsg);
        emit parseFinished(false, errorMsg);
        return false;
    }

    // 4. 计算时间间隔（微秒）
    for (int i = 0; i < m_timestamps.size(); ++i) {
        if (i == 0) {
            m_timestamps[i].diffMicroseconds = 0.0;
        } else {
            // 差值（秒）* 1,000,000 = 微秒
            double diffSeconds = m_timestamps[i].totalSeconds - m_timestamps[i-1].totalSeconds;
            m_timestamps[i].diffMicroseconds = diffSeconds * 1e6;
        }
    }

    // 5. 写入文本文件
    QFile outputFile(outputFilePath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString errorMsg = QString("无法创建输出文件: %1").arg(outputFilePath);
        emit error(errorMsg);
        emit parseFinished(false, errorMsg);
        return false;
    }

    QTextStream out(&outputFile);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");  // Qt 5: 设置UTF-8编码
#else
    out.setEncoding(QStringConverter::Utf8);  // Qt 6: 设置UTF-8编码
#endif

    for (int i = 0; i < m_timestamps.size(); ++i) {
        const TimestampData &ts = m_timestamps[i];

        // 格式化输出：与MATLAB版本一致
        // 时间字符串
        QString timeStr = ts.toString();

        // 写入：时间 + 时差
        out << timeStr << QString("    时差：%1 us\n")
               .arg(ts.diffMicroseconds, 10, 'f', 2, QChar(' '));

        if ((i + 1) % 10000 == 0) {
            emit progressChanged(i + 1, m_timestamps.size());
        }
    }

    outputFile.close();

    QString successMsg = QString("解析完成。共解析 %1 个时间戳，保存到: %2")
                             .arg(m_timestamps.size())
                             .arg(outputFilePath);
    qDebug() << "[TimeFileParser]" << successMsg;
    emit parseFinished(true, successMsg);

    return true;
}
