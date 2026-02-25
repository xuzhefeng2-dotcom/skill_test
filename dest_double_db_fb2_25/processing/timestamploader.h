#ifndef TIMESTAMPLOADER_H
#define TIMESTAMPLOADER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QDateTime>
#include <QRegularExpression>

// 整秒时刻标记
struct SecondMark {
    int lineIndex;           // 时间戳文件行号
    QDateTime absoluteTime;  // 绝对时间
    double preciseSeconds;   // 精确秒数（带小数，如44.88888）
    double relativeSeconds;  // 相对秒数（0, 1, 2...）

    SecondMark()
        : lineIndex(0)
        , preciseSeconds(0.0)
        , relativeSeconds(0.0)
    {}
};

class TimeStampLoader : public QObject
{
    Q_OBJECT

public:
    explicit TimeStampLoader(QObject *parent = nullptr);
    ~TimeStampLoader();

    // ✅ 异步加载（在子线程执行）
    Q_INVOKABLE void loadAsync(const QString &txtPath);

    // 获取最后加载的数据
    const QVector<SecondMark>& getSecondMarks() const { return m_secondMarks; }
    int getSecondCount() const { return m_secondMarks.size(); }

    // 获取时间范围
    QDateTime getStartTime() const;
    QDateTime getEndTime() const;
    double getDurationSeconds() const;

signals:
    void progressChanged(int current, int total);
    void loadFinished(bool success, const QString &message);
    void error(const QString &errorMessage);

private:
    // 从文本行解析时间戳（优化版：复用正则表达式对象）
    QDateTime parseTimestampLine(const QString &line);

    // 从文本行提取精确秒数（优化版：复用正则表达式对象）
    double getPreciseSeconds(const QString &line);

    QVector<SecondMark> m_secondMarks;

    // ✅ 预编译正则表达式（避免每行都重新编译）
    QRegularExpression m_regexTimestamp;
    QRegularExpression m_regexSeconds;
};

#endif // TIMESTAMPLOADER_H
