#include "datareader.h"
//#include "../core/uint24.h"
#include <QDebug>

DataReader::DataReader(QObject *parent)
    : QObject(parent)
    , m_frameSize(5000)
    , m_totalFrames(0)
    , m_currentFrame(0)
    , m_fileSize(0)
{
}

DataReader::~DataReader()
{
    closeFile();
}

bool DataReader::openFile(const QString &filePath)
{
    closeFile();

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        emit error(QString("无法打开文件: %1").arg(filePath));
        return false;
    }

    m_fileSize = m_file.size();
    // 每帧双通道数据: m_frameSize * 2 * 3字节（每通道3字节）
    qint64 frameBytes = m_frameSize * 2 * 3;
    m_totalFrames = m_fileSize / frameBytes;
    m_currentFrame = 0;

    qDebug() << "[DataReader] 打开文件成功:" << filePath;
    qDebug() << "  文件大小:" << m_fileSize << "字节";
    qDebug() << "  总帧数:" << m_totalFrames;
    qDebug() << "  每帧大小:" << frameBytes << "字节（3字节/通道）";

    return true;
}

void DataReader::closeFile()
{
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_totalFrames = 0;
    m_currentFrame = 0;
}

DataFrame DataReader::readNextFrame()
{
    DataFrame frame;
    frame.frameNumber = m_currentFrame;
    frame.isValid = false;

    if (!m_file.isOpen() || atEnd()) {
        return frame;
    }

    // 预分配内存
    frame.ewData.resize(m_frameSize);
    frame.nsData.resize(m_frameSize);

    // 读取双通道交替数据（每通道3字节）
    qint64 frameBytes = m_frameSize * 2 * 3;
    frame.rawData.resize(frameBytes);
    qint64 bytesRead = m_file.read(frame.rawData.data(), frameBytes);

    if (bytesRead != frameBytes) {
        emit error("读取数据失败");
        return frame;
    }

    // 从原始数据分离EW和NS通道（3字节转4字节，高位为0）
    const quint8* buffer = reinterpret_cast<const quint8*>(frame.rawData.constData());
    for (int i = 0; i < m_frameSize; i++) {
        // 每个采样点：NS(3字节) + EW(3字节)
        int nsOffset = i * 6;      // NS通道偏移
        int ewOffset = i * 6 + 3;  // EW通道偏移

        // 读取3字节并转换为4字节（小端序，高位为0）
        frame.nsData[i] = (static_cast<quint32>(buffer[nsOffset]) |
                          (static_cast<quint32>(buffer[nsOffset + 1]) << 8) |
                          (static_cast<quint32>(buffer[nsOffset + 2]) << 16));

        frame.ewData[i] = (static_cast<quint32>(buffer[ewOffset]) |
                          (static_cast<quint32>(buffer[ewOffset + 1]) << 8) |
                          (static_cast<quint32>(buffer[ewOffset + 2]) << 16));
    }

    frame.isValid = true;
    m_currentFrame++;

    // 发送进度
    int progress = (m_currentFrame * 100) / m_totalFrames;
    emit progressChanged(progress);

    return frame;
}

void DataReader::reset()
{
    if (m_file.isOpen()) {
        m_file.seek(0);
        m_currentFrame = 0;
    }
}

bool DataReader::atEnd() const
{
    return m_currentFrame >= m_totalFrames || m_file.atEnd();
}
