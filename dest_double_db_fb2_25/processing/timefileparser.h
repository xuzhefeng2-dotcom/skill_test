#ifndef TIMEFILEPARSER_H
#define TIMEFILEPARSER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QDateTime>

// 时间戳结构体
struct TimestampData {
    quint8 year;        // 年份（0-127，实际为20xx）
    quint8 month;       // 月份（1-12）
    quint8 day;         // 日期（1-31）
    quint8 hour;        // 小时（0-23）
    quint8 minute;      // 分钟（0-59）
    quint8 second;      // 秒（0-59）
    quint32 cnt_10ns;   // 10纳秒计数器（0-99999999，对应0-0.99999999秒）

    double totalSeconds;  // 从当天00:00:00开始的总秒数
    double diffMicroseconds;  // 与前一个时间戳的间隔（微秒）

    // 格式化为字符串
    QString toString() const;
};

class TimeFileParser : public QObject
{
    Q_OBJECT

public:
    explicit TimeFileParser(QObject *parent = nullptr);
    ~TimeFileParser();

    // 解析时间文件
    bool parseTimeFile(const QString &inputFilePath, const QString &outputFilePath);

    // 获取解析结果
    const QVector<TimestampData>& getTimestamps() const { return m_timestamps; }
    int getTimestampCount() const { return m_timestamps.size(); }

signals:
    void progressChanged(int current, int total);
    void parseFinished(bool success, const QString &message);
    void error(const QString &errorMessage);

private:
    // 从64位数据中解析时间戳
    TimestampData parseTimestamp(quint64 rawData);

    // 计算总秒数（用于计算时间间隔）
    double calculateTotalSeconds(const TimestampData &ts);

    QVector<TimestampData> m_timestamps;
};

#endif // TIMEFILEPARSER_H
