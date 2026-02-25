#include "timestamploader.h"
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <QtMath>

TimeStampLoader::TimeStampLoader(QObject *parent)
    : QObject(parent)
{
    // ✅ 预编译正则表达式（避免每行都重新编译，性能提升10-100倍）
    m_regexTimestamp.setPattern(R"((\d{4})年(\d{2})月(\d{2})日 (\d{2}):(\d{2}):(\d{2})\.(\d+))");
    m_regexSeconds.setPattern(R"((\d{2}):(\d{2}):(\d{2}\.\d+))");

    // 启用优化选项
    m_regexTimestamp.optimize();
    m_regexSeconds.optimize();
}

TimeStampLoader::~TimeStampLoader()
{
}

QDateTime TimeStampLoader::parseTimestampLine(const QString &line)
{
    // ✅ 使用预编译的正则表达式
    QRegularExpressionMatch match = m_regexTimestamp.match(line);

    if (!match.hasMatch()) {
        return QDateTime();
    }

    int year = match.captured(1).toInt();
    int month = match.captured(2).toInt();
    int day = match.captured(3).toInt();
    int hour = match.captured(4).toInt();
    int minute = match.captured(5).toInt();
    int second = match.captured(6).toInt();
    QString fractionStr = match.captured(7);

    // 小数部分转毫秒（只取前3位）
    int msec = 0;
    if (fractionStr.length() >= 3) {
        msec = fractionStr.left(3).toInt();
    } else if (fractionStr.length() == 2) {
        msec = fractionStr.toInt() * 10;
    } else if (fractionStr.length() == 1) {
        msec = fractionStr.toInt() * 100;
    }

    QDate date(year, month, day);
    QTime time(hour, minute, second, msec);
    return QDateTime(date, time);
}

double TimeStampLoader::getPreciseSeconds(const QString &line)
{
    // ✅ 使用预编译的正则表达式
    QRegularExpressionMatch match = m_regexSeconds.match(line);

    if (match.hasMatch()) {
        int hour = match.captured(1).toInt();
        int minute = match.captured(2).toInt();
        double second = match.captured(3).toDouble();

        // 转换为当天的总秒数
        return hour * 3600.0 + minute * 60.0 + second;
    }

    return 0.0;
}

// ✅ 异步加载函数（在子线程执行）
void TimeStampLoader::loadAsync(const QString &txtPath)
{
    m_secondMarks.clear();

    QFile file(txtPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString errorMsg = QString("无法打开时间戳文件: %1").arg(txtPath);
        emit error(errorMsg);
        emit loadFinished(false, errorMsg);
        return;
    }

    QTextStream in(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    in.setCodec("UTF-8");
#else
    in.setEncoding(QStringConverter::Utf8);
#endif

    qDebug() << "[TimeStampLoader] 开始异步解析时间戳文件:" << txtPath;

    // 1. 读取第一行，作为基准
    if (in.atEnd()) {
        QString errorMsg = "时间戳文件为空";
        emit error(errorMsg);
        emit loadFinished(false, errorMsg);
        return;
    }

    QString firstLine = in.readLine();
    double firstPreciseSeconds = getPreciseSeconds(firstLine);
    QDateTime firstTime = parseTimestampLine(firstLine);

    if (!firstTime.isValid() || firstPreciseSeconds == 0.0) {
        QString errorMsg = "无法解析第一行时间戳";
        emit error(errorMsg);
        emit loadFinished(false, errorMsg);
        return;
    }

    // 记录第一个时刻（0秒位置）
    SecondMark firstMark;
    firstMark.lineIndex = 0;
    firstMark.absoluteTime = firstTime;
    firstMark.preciseSeconds = firstPreciseSeconds;
    firstMark.relativeSeconds = 0;
    m_secondMarks.append(firstMark);

    double baseFraction = firstPreciseSeconds - qFloor(firstPreciseSeconds);
    qDebug() << "[TimeStampLoader] 基准时间:" << firstTime.toString("yyyy-MM-dd HH:mm:ss.zzz")
             << "精确秒数:" << QString::number(firstPreciseSeconds, 'f', 8)
             << "小数部分:" << QString::number(baseFraction, 'f', 8);

    // 2. 单次遍历，查找所有整秒时刻
    int lineIndex = 1;
    int currentTarget = 1;  // 当前要找的目标（1秒后、2秒后...）
    double bestDiff = 999999;
    int bestLineIndex = -1;
    QDateTime bestTime;
    double bestPreciseSeconds = 0;

    // ✅ 进度反馈优化：每处理1000行汇报一次
    int progressInterval = 1000;
    int lastReportedLine = 0;

    while (!in.atEnd()) {
        QString line = in.readLine();
        double currentPreciseSeconds = getPreciseSeconds(line);
        QDateTime currentTime = parseTimestampLine(line);

        if (!currentTime.isValid() || currentPreciseSeconds == 0.0) {
            lineIndex++;
            continue;
        }

        // 计算目标秒数
        double targetSeconds = firstPreciseSeconds + currentTarget;
        double diff = qAbs(currentPreciseSeconds - targetSeconds);

        // 更新最佳候选
        if (diff < bestDiff) {
            bestDiff = diff;
            bestLineIndex = lineIndex;
            bestTime = currentTime;
            bestPreciseSeconds = currentPreciseSeconds;
        }

        // 判断是否已经超过目标（开始寻找下一个目标）
        if (currentPreciseSeconds > targetSeconds + 0.5) {
            // 记录当前找到的最佳时刻
            if (bestLineIndex >= 0) {
                SecondMark mark;
                mark.lineIndex = bestLineIndex;
                mark.absoluteTime = bestTime;
                mark.preciseSeconds = bestPreciseSeconds;
                mark.relativeSeconds = currentTarget;
                m_secondMarks.append(mark);

                // 只在调试模式下打印详细信息
#ifdef QT_DEBUG
                qDebug() << QString("[TimeStampLoader] %1秒: 行%2, 时间=%3, 精确秒数=%4, 误差=%5ms")
                    .arg(currentTarget)
                    .arg(bestLineIndex)
                    .arg(bestTime.toString("HH:mm:ss.zzz"))
                    .arg(bestPreciseSeconds, 0, 'f', 8)
                    .arg(bestDiff * 1000, 0, 'f', 2);
#endif
            }

            // 准备查找下一个目标
            currentTarget++;
            bestDiff = 999999;
            bestLineIndex = -1;
        }

        lineIndex++;

        // ✅ 进度反馈：每1000行或每找到一个整秒时刻汇报一次
        if (lineIndex - lastReportedLine >= progressInterval) {
            emit progressChanged(m_secondMarks.size(), lineIndex);
            lastReportedLine = lineIndex;
        }
    }

    // 处理最后一个时刻
    if (bestLineIndex >= 0 && bestDiff < 0.5) {
        SecondMark mark;
        mark.lineIndex = bestLineIndex;
        mark.absoluteTime = bestTime;
        mark.preciseSeconds = bestPreciseSeconds;
        mark.relativeSeconds = currentTarget;
        m_secondMarks.append(mark);

#ifdef QT_DEBUG
        qDebug() << QString("[TimeStampLoader] %1秒: 行%2, 时间=%3, 精确秒数=%4, 误差=%5ms")
            .arg(currentTarget)
            .arg(bestLineIndex)
            .arg(bestTime.toString("HH:mm:ss.zzz"))
            .arg(bestPreciseSeconds, 0, 'f', 8)
            .arg(bestDiff * 1000, 0, 'f', 2);
#endif
    }

    file.close();

    QString successMsg = QString("成功提取 %1 个整秒时刻（共 %2 行）").arg(m_secondMarks.size()).arg(lineIndex);
    qDebug() << "[TimeStampLoader]" << successMsg;
    emit loadFinished(true, successMsg);
}

QDateTime TimeStampLoader::getStartTime() const
{
    if (m_secondMarks.isEmpty()) {
        return QDateTime();
    }
    return m_secondMarks.first().absoluteTime;
}

QDateTime TimeStampLoader::getEndTime() const
{
    if (m_secondMarks.isEmpty()) {
        return QDateTime();
    }
    return m_secondMarks.last().absoluteTime;
}

double TimeStampLoader::getDurationSeconds() const
{
    return m_secondMarks.size();
}
