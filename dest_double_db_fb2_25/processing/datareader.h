#ifndef DATAREADER_H
#define DATAREADER_H

#include <QObject>
#include <QFile>
#include <QVector>
#include <QString>
#include <QDateTime>

// 数据帧结构（24位数据）
struct DataFrame {
    quint32 frameNumber;        // 帧序号
    QVector<quint32> ewData;    // EW通道数据（24位）
    QVector<quint32> nsData;    // NS通道数据（24位）
    QByteArray rawData;         // 原始数据（交替格式：NS,EW,NS,EW...，每个3字节）
    QDateTime timestamp;        // 数据包时间戳（东八区本地时间）
    bool isValid;               // 是否有效
};

/**
 * @brief COS文件读取器
 *
 * 读取COS文件,按帧分段输出双通道数据
 * 数据格式:双通道交替存储的uint16数据
 */
class DataReader : public QObject
{
    Q_OBJECT
public:
    explicit DataReader(QObject *parent = nullptr);
    ~DataReader();

    // 打开文件
    bool openFile(const QString &filePath);

    // 关闭文件
    void closeFile();

    // 读取下一帧数据
    DataFrame readNextFrame();

    // 重置到文件开头
    void reset();

    // 是否到达文件末尾
    bool atEnd() const;

    // 获取总帧数
    int totalFrames() const { return m_totalFrames; }

    // 获取当前帧号
    int currentFrame() const { return m_currentFrame; }

    // 设置每帧大小(采样点数,默认5000)
    void setFrameSize(int size) { m_frameSize = size; }
    int frameSize() const { return m_frameSize; }

signals:
    void frameRead(const DataFrame &frame);
    void error(const QString &message);
    void progressChanged(int percent);

private:
    QFile m_file;
    int m_frameSize;        // 每帧采样点数
    int m_totalFrames;      // 总帧数
    int m_currentFrame;     // 当前帧号
    qint64 m_fileSize;      // 文件大小
};

#endif // DATAREADER_H
