#include "localfilereader.h"
#include <QDebug>
#include <QFileInfo>

LocalFileReader::LocalFileReader(QObject *parent)
    : QObject(parent)
    , m_frameSize(SAMPLES_PER_FRAME)
    , m_totalFrames(0)
    , m_currentFrame(0)
    , m_fileSize(0)
    , m_isProcessing(false)
    , m_isPaused(false)
    , m_ewFFTBuffer(RINGBUF_CAPACITY)
    , m_nsFFTBuffer(RINGBUF_CAPACITY)
    , m_tdFFTBuffer(RINGBUF_CAPACITY)
    , m_fftProcessorNS(nullptr)
    , m_fftProcessorEW(nullptr)
    , m_fftProcessorTD(nullptr)
    , m_processTimer(nullptr)
    , m_framesPerBatch(FRAMES_PER_BATCH)
    , m_processingSpeed(0)  // 0表示尽可能快处理（原来是200帧/秒太慢）
{
    // 创建FFT处理器
    m_fftProcessorNS = new FFTProcessor(this);
    m_fftProcessorEW = new FFTProcessor(this);
    m_fftProcessorTD = new FFTProcessor(this);

    // 默认采样率应用到FFT处理器（可在菜单切换）
    setSampleRate(m_sampleRate);

    // 创建处理定时器
    m_processTimer = new QTimer(this);
    connect(m_processTimer, &QTimer::timeout, this, &LocalFileReader::processBatch);
}

LocalFileReader::~LocalFileReader()
{
    stopProcessing();
    closeFile();
}

void LocalFileReader::setSampleRate(double fsHz)
{
    if (fsHz <= 0) return;
    m_sampleRate = fsHz;
    if (m_fftProcessorNS) m_fftProcessorNS->setSampleRate(fsHz);
    if (m_fftProcessorEW) m_fftProcessorEW->setSampleRate(fsHz);
    if (m_fftProcessorTD) m_fftProcessorTD->setSampleRate(fsHz);
}

void LocalFileReader::setAnalysisHop(int hopSamples)
{
    if (hopSamples <= 0) return;
    if (m_fftProcessorNS) m_fftProcessorNS->setAnalysisHop(hopSamples);
    if (m_fftProcessorEW) m_fftProcessorEW->setAnalysisHop(hopSamples);
    if (m_fftProcessorTD) m_fftProcessorTD->setAnalysisHop(hopSamples);
}

void LocalFileReader::setEnabledChannels(bool ewEnabled, bool nsEnabled, bool tdEnabled)
{
    m_channelEWEnabled = ewEnabled;
    m_channelNSEnabled = nsEnabled;
    m_channelTDEnabled = tdEnabled;

    qDebug() << "[LocalFileReader] 通道配置更新 - EW:" << ewEnabled
             << "NS:" << nsEnabled << "TD:" << tdEnabled;
}

double LocalFileReader::recordingDurationSeconds() const
{
    if (m_fileSize <= 0 || m_sampleRate <= 0) return 0.0;

    // 根据启用的通道数计算时长
    int enabledChannels = 0;
    if (m_channelEWEnabled) enabledChannels++;
    if (m_channelNSEnabled) enabledChannels++;
    if (m_channelTDEnabled) enabledChannels++;

    if (enabledChannels == 0) return 0.0;

    const double totalSamplesPerChannel = static_cast<double>(m_fileSize) / (enabledChannels * 3.0);
    return totalSamplesPerChannel / m_sampleRate;
}



void LocalFileReader::openFileAsync(const QString &filePath)
{
    // 复用同步接口；失败时 openFile() 内部会 emit error
    openFile(filePath);
}

void LocalFileReader::closeFileAsync()
{
    closeFile();
}

bool LocalFileReader::openFile(const QString &filePath)
{
    closeFile();

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        emit error(QString("无法打开文件: %1").arg(filePath));
        return false;
    }

    m_filePath = filePath;
    m_recordingFileName = QFileInfo(filePath).fileName();
    m_fileSize = m_file.size();

    // ✅ 根据用户启用的通道数计算总帧数（不再根据文件大小自动判断）
    int enabledChannels = 0;
    if (m_channelEWEnabled) enabledChannels++;
    if (m_channelNSEnabled) enabledChannels++;
    if (m_channelTDEnabled) enabledChannels++;

    if (enabledChannels == 0) {
        QString errorMsg = "至少需要启用一个通道";
        qCritical() << "[LocalFileReader]" << errorMsg;
        emit error(errorMsg);
        m_file.close();
        return false;
    }

    // 计算每帧字节数和总帧数
    qint64 frameBytes = m_frameSize * enabledChannels * 3;  // 每通道3字节
    m_totalFrames = m_fileSize / frameBytes;
    m_currentFrame = 0;

    qDebug() << "[LocalFileReader] 打开文件成功:" << filePath;
    qDebug() << "  文件大小:" << m_fileSize << "字节";
    qDebug() << "  启用通道数:" << enabledChannels;
    qDebug() << "  EW:" << (m_channelEWEnabled ? "启用" : "禁用")
             << " NS:" << (m_channelNSEnabled ? "启用" : "禁用")
             << " TD:" << (m_channelTDEnabled ? "启用" : "禁用");
    qDebug() << "  总帧数:" << m_totalFrames;
    qDebug() << "  每帧大小:" << frameBytes << "字节（3字节/通道）";

    emit fileOpened(m_recordingFileName, m_totalFrames);
    return true;
}

void LocalFileReader::closeFile()
{
    stopProcessing();

    if (m_file.isOpen()) {
        m_file.close();
    }

    m_totalFrames = 0;
    m_currentFrame = 0;
    m_filePath.clear();
    m_recordingFileName.clear();
}

void LocalFileReader::startProcessing()
{
    if (!m_file.isOpen()) {
        emit error("请先打开文件");
        return;
    }

    if (m_isProcessing) {
        return;
    }

    // 清空缓冲区
    m_ewFFTBuffer.clear();
    m_nsFFTBuffer.clear();
    m_fftProcessorNS->clearBuffer();
    m_fftProcessorEW->clearBuffer();

    // 重置到文件开头
    m_file.seek(0);
    m_currentFrame = 0;

    m_isProcessing = true;
    m_isPaused = false;

    // 计算定时器间隔
    // 每批处理FRAMES_PER_BATCH帧，处理速度是m_processingSpeed帧/秒
    int intervalMs = (m_processingSpeed > 0) ?
                         (m_framesPerBatch * 1000 / m_processingSpeed) : 10;
    m_processTimer->start(intervalMs);

    emit processingStarted();
    qDebug() << "[LocalFileReader] 开始处理，间隔:" << intervalMs << "ms";
}

void LocalFileReader::stopProcessing()
{
    if (!m_isProcessing) {
        return;
    }

    m_processTimer->stop();
    m_isProcessing = false;
    m_isPaused = false;

    emit processingStopped();
    // 已删除调试输出：[LocalFileReader] 停止处理
}

void LocalFileReader::pauseProcessing()
{
    if (!m_isProcessing || m_isPaused) {
        return;
    }

    m_processTimer->stop();
    m_isPaused = true;
    emit processingPaused();
}

void LocalFileReader::resumeProcessing()
{
    if (!m_isProcessing || !m_isPaused) {
        return;
    }

    int intervalMs = (m_processingSpeed > 0) ?
                         (m_framesPerBatch * 1000 / m_processingSpeed) : 10;
    m_processTimer->start(intervalMs);
    m_isPaused = false;
    emit processingResumed();
}

void LocalFileReader::reset()
{
    bool wasProcessing = m_isProcessing;
    stopProcessing();

    if (m_file.isOpen()) {
        m_file.seek(0);
        m_currentFrame = 0;
    }

    // 清空缓冲区
    m_ewFFTBuffer.clear();
    m_nsFFTBuffer.clear();
    m_fftProcessorNS->clearBuffer();
    m_fftProcessorEW->clearBuffer();

    if (wasProcessing) {
        startProcessing();
    }
}

QString LocalFileReader::recordingDuration() const
{
    const double durationSec = recordingDurationSeconds();
    const int hours = static_cast<int>(durationSec / 3600.0);
    const int minutes = static_cast<int>((durationSec - hours * 3600.0) / 60.0);
    const double seconds = durationSec - hours * 3600.0 - minutes * 60.0;

    // 带小数秒（例如 00:00:08.32）
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 5, 'f', 2, QChar('0'));
}

void LocalFileReader::setProcessingSpeed(int framesPerSecond)
{
    m_processingSpeed = framesPerSecond;

    if (m_isProcessing && !m_isPaused) {
        int intervalMs = (m_processingSpeed > 0) ?
                             (m_framesPerBatch * 1000 / m_processingSpeed) : 10;
        m_processTimer->setInterval(intervalMs);
    }
}

bool LocalFileReader::readNextFrame(QVector<quint32> &ewData, QVector<quint32> &nsData, QVector<quint32> *tdData)
{
    if (!m_file.isOpen() || m_currentFrame >= m_totalFrames) {
        return false;
    }

    // 计算启用的通道数
    int enabledChannels = 0;
    if (m_channelEWEnabled) enabledChannels++;
    if (m_channelNSEnabled) enabledChannels++;
    if (m_channelTDEnabled) enabledChannels++;

    if (enabledChannels == 0) {
        return false;
    }

    // 预分配内存
    ewData.resize(m_frameSize);
    nsData.resize(m_frameSize);
    if (tdData && m_channelTDEnabled) {
        tdData->resize(m_frameSize);
    }

    // 读取数据（每通道3字节）
    qint64 frameBytes = m_frameSize * enabledChannels * 3;
    QByteArray buffer(frameBytes, 0);
    qint64 bytesRead = m_file.read(buffer.data(), frameBytes);

    if (bytesRead != frameBytes) {
        return false;
    }

    // 根据启用的通道数解析数据
    const quint8* data = reinterpret_cast<const quint8*>(buffer.constData());

    if (enabledChannels == 3 && m_channelEWEnabled && m_channelNSEnabled && m_channelTDEnabled) {
        // 三通道模式：TD + EW + NS
        for (int i = 0; i < m_frameSize; i++) {
            int tdOffset = i * 9;      // TD通道偏移
            int ewOffset = i * 9 + 3;  // EW通道偏移
            int nsOffset = i * 9 + 6;  // NS通道偏移

            if (tdData) {
                (*tdData)[i] = (static_cast<quint32>(data[tdOffset]) |
                               (static_cast<quint32>(data[tdOffset + 1]) << 8) |
                               (static_cast<quint32>(data[tdOffset + 2]) << 16));
            }

            ewData[i] = (static_cast<quint32>(data[ewOffset]) |
                        (static_cast<quint32>(data[ewOffset + 1]) << 8) |
                        (static_cast<quint32>(data[ewOffset + 2]) << 16));

            nsData[i] = (static_cast<quint32>(data[nsOffset]) |
                        (static_cast<quint32>(data[nsOffset + 1]) << 8) |
                        (static_cast<quint32>(data[nsOffset + 2]) << 16));
        }
    } else if (enabledChannels == 2 && m_channelEWEnabled && m_channelNSEnabled) {
        // 双通道模式：EW + NS
        for (int i = 0; i < m_frameSize; i++) {
            int nsOffset = i * 6;      // NS通道偏移
            int ewOffset = i * 6 + 3;  // EW通道偏移

            nsData[i] = (static_cast<quint32>(data[nsOffset]) |
                        (static_cast<quint32>(data[nsOffset + 1]) << 8) |
                        (static_cast<quint32>(data[nsOffset + 2]) << 16));

            ewData[i] = (static_cast<quint32>(data[ewOffset]) |
                        (static_cast<quint32>(data[ewOffset + 1]) << 8) |
                        (static_cast<quint32>(data[ewOffset + 2]) << 16));
        }
    } else {
        // 其他组合：按顺序读取启用的通道
        int bytesPerSample = enabledChannels * 3;
        for (int i = 0; i < m_frameSize; i++) {
            int offset = 0;

            if (m_channelTDEnabled && tdData) {
                (*tdData)[i] = (static_cast<quint32>(data[i * bytesPerSample + offset]) |
                               (static_cast<quint32>(data[i * bytesPerSample + offset + 1]) << 8) |
                               (static_cast<quint32>(data[i * bytesPerSample + offset + 2]) << 16));
                offset += 3;
            }

            if (m_channelEWEnabled) {
                ewData[i] = (static_cast<quint32>(data[i * bytesPerSample + offset]) |
                            (static_cast<quint32>(data[i * bytesPerSample + offset + 1]) << 8) |
                            (static_cast<quint32>(data[i * bytesPerSample + offset + 2]) << 16));
                offset += 3;
            }

            if (m_channelNSEnabled) {
                nsData[i] = (static_cast<quint32>(data[i * bytesPerSample + offset]) |
                            (static_cast<quint32>(data[i * bytesPerSample + offset + 1]) << 8) |
                            (static_cast<quint32>(data[i * bytesPerSample + offset + 2]) << 16));
                offset += 3;
            }
        }
    }

    m_currentFrame++;
    return true;
}

void LocalFileReader::processBatch()
{
    if (!m_isProcessing || m_isPaused) {
        return;
    }

    // 读取并处理一批数据（使用quint32存储24位数据）
    QVector<quint32> ewBatch, nsBatch, tdBatch;
    ewBatch.reserve(m_framesPerBatch * SAMPLES_PER_FRAME);
    nsBatch.reserve(m_framesPerBatch * SAMPLES_PER_FRAME);
    if (m_channelTDEnabled) {
        tdBatch.reserve(m_framesPerBatch * SAMPLES_PER_FRAME);
    }

    int framesRead = 0;
    for (int i = 0; i < m_framesPerBatch; ++i) {
        QVector<quint32> ewData, nsData, tdData;
        if (!readNextFrame(ewData, nsData, m_channelTDEnabled ? &tdData : nullptr)) {
            break;
        }
        ewBatch.append(ewData);
        nsBatch.append(nsData);
        if (m_channelTDEnabled) {
            tdBatch.append(tdData);
        }
        framesRead++;
    }

    if (framesRead == 0) {
        // 文件读取完成
        stopProcessing();
        emit processingFinished();
        return;
    }

    // 进行FFT处理（使用24位数据接口）
    SpectrumResult tdResult;
    if (m_channelTDEnabled) {
        tdResult = m_fftProcessorTD->appendAndProcess(tdBatch);
    }
    SpectrumResult nsResult = m_fftProcessorNS->appendAndProcess(nsBatch);
    SpectrumResult ewResult = m_fftProcessorEW->appendAndProcess(ewBatch);

    // 将结果存入缓冲区
    if (nsResult.isValid) {
        m_nsFFTBuffer.push(std::move(nsResult));
    }
    if (ewResult.isValid) {
        m_ewFFTBuffer.push(std::move(ewResult));
    }
    if (m_channelTDEnabled && tdResult.isValid) {
        m_tdFFTBuffer.push(std::move(tdResult));
    }

    // 发送进度信号
    double percent = progress();
    emit progressChanged(percent);
    emit frameProcessed(m_currentFrame, m_totalFrames);
}

// bool LocalFileReader::saveSTFTDataToCSV(const QString &outputDir, const QString &fileBaseName)
// {
//     if (!m_fftProcessorNS || !m_fftProcessorEW) {
//         qWarning() << "[LocalFileReader] FFTProcessor未初始化，无法保存STFT数据";
//         return false;
//     }

//     bool success = true;

//     // 保存NS通道数据
//     if (!m_fftProcessorNS->saveSTFTToCSV("NS", outputDir, fileBaseName)) {
//         qWarning() << "[LocalFileReader] NS通道STFT数据保存失败";
//         success = false;
//     }

//     // 保存EW通道数据
//     if (!m_fftProcessorEW->saveSTFTToCSV("EW", outputDir, fileBaseName)) {
//         qWarning() << "[LocalFileReader] EW通道STFT数据保存失败";
//         success = false;
//     }

//     return success;
// }

// 批量处理模式：一次性读取所有数据并FFT
void LocalFileReader::startBatchProcessing(const QString &filePath)
{
    qDebug() << "[LocalFileReader] 开始批量处理模式:" << filePath;
    
    // 打开文件
    if (!openFile(filePath)) {
        emit error("无法打开文件：" + filePath);
        return;
    }
    
    emit processingStarted();
    m_isProcessing = true;
    
    // 读取所有帧数据并进行FFT处理
    int processedFrames = 0;
    int totalFFTResults = 0;
    
    // 累积缓冲区（用于FFT）
    QVector<qint32> ewAccumulator;
    QVector<qint32> nsAccumulator;
    QVector<qint32> tdAccumulator;
    
    ewAccumulator.reserve(SAMPLES_PER_FRAME * 20);  // 预留空间
    nsAccumulator.reserve(SAMPLES_PER_FRAME * 20);
    tdAccumulator.reserve(SAMPLES_PER_FRAME * 20);
    
    while (m_currentFrame < m_totalFrames) {
        QVector<quint32> ewFrame, nsFrame, tdFrame;
        
        // 读取一帧数据
        bool success = readNextFrame(ewFrame, nsFrame, m_channelTDEnabled ? &tdFrame : nullptr);
        if (!success) {
            qWarning() << "[LocalFileReader] 读取帧失败:" << m_currentFrame;
            break;
        }
        
        // 转换为有符号24位数据
        QVector<qint32> ewSigned(ewFrame.size());
        QVector<qint32> nsSigned(nsFrame.size());
        QVector<qint32> tdSigned;
        
        for (int i = 0; i < ewFrame.size(); i++) {
            // 24位有符号扩展
            quint32 ew24 = ewFrame[i] & 0xFFFFFF;
            if (ew24 & 0x800000) {  // 符号位为1
                ewSigned[i] = static_cast<qint32>(ew24 | 0xFF000000);
            } else {
                ewSigned[i] = static_cast<qint32>(ew24);
            }
            
            quint32 ns24 = nsFrame[i] & 0xFFFFFF;
            if (ns24 & 0x800000) {
                nsSigned[i] = static_cast<qint32>(ns24 | 0xFF000000);
            } else {
                nsSigned[i] = static_cast<qint32>(ns24);
            }
        }
        
        if (m_channelTDEnabled && !tdFrame.isEmpty()) {
            tdSigned.resize(tdFrame.size());
            for (int i = 0; i < tdFrame.size(); i++) {
                quint32 td24 = tdFrame[i] & 0xFFFFFF;
                if (td24 & 0x800000) {
                    tdSigned[i] = static_cast<qint32>(td24 | 0xFF000000);
                } else {
                    tdSigned[i] = static_cast<qint32>(td24);
                }
            }
        }
        
        // 追加到累积缓冲区
        ewAccumulator.append(ewSigned);
        nsAccumulator.append(nsSigned);
        if (m_channelTDEnabled) {
            tdAccumulator.append(tdSigned);
        }
        
        processedFrames++;
        m_currentFrame++;
        
        // 定期发送进度
        if (processedFrames % 100 == 0) {
            double percent = (double)m_currentFrame / m_totalFrames * 100.0;
            emit progressChanged(percent);
        }
    }
    
    qDebug() << "[LocalFileReader] 数据读取完成，共" << processedFrames << "帧";
    qDebug() << "[LocalFileReader] EW累积采样点:" << ewAccumulator.size();
    
    // 批量进行FFT处理
    qDebug() << "[LocalFileReader] 开始批量FFT处理...";
    
    // 转换为quint32格式（FFTProcessor需要）
    QVector<quint32> ewAccumulatorU32(ewAccumulator.size());
    QVector<quint32> nsAccumulatorU32(nsAccumulator.size());
    QVector<quint32> tdAccumulatorU32;
    
    for (int i = 0; i < ewAccumulator.size(); i++) {
        ewAccumulatorU32[i] = static_cast<quint32>(ewAccumulator[i] & 0xFFFFFF);
    }
    for (int i = 0; i < nsAccumulator.size(); i++) {
        nsAccumulatorU32[i] = static_cast<quint32>(nsAccumulator[i] & 0xFFFFFF);
    }
    if (m_channelTDEnabled && !tdAccumulator.isEmpty()) {
        tdAccumulatorU32.resize(tdAccumulator.size());
        for (int i = 0; i < tdAccumulator.size(); i++) {
            tdAccumulatorU32[i] = static_cast<quint32>(tdAccumulator[i] & 0xFFFFFF);
        }
    }
    
    // EW通道FFT
    if (m_channelEWEnabled) {
        m_fftProcessorEW->clearBuffer();  // 清空之前的数据
        SpectrumResult ewResult = m_fftProcessorEW->appendAndProcess(ewAccumulatorU32);
        while (ewResult.isValid) {
            m_ewFFTBuffer.push(std::move(ewResult));
            totalFFTResults++;
            ewResult = m_fftProcessorEW->appendAndProcess(QVector<quint32>());  // 继续处理缓存的数据
        }
    }
    
    // NS通道FFT
    if (m_channelNSEnabled) {
        m_fftProcessorNS->clearBuffer();
        SpectrumResult nsResult = m_fftProcessorNS->appendAndProcess(nsAccumulatorU32);
        while (nsResult.isValid) {
            m_nsFFTBuffer.push(std::move(nsResult));
            nsResult = m_fftProcessorNS->appendAndProcess(QVector<quint32>());
        }
    }
    
    // TD通道FFT
    if (m_channelTDEnabled && !tdAccumulator.isEmpty()) {
        m_fftProcessorTD->clearBuffer();
        SpectrumResult tdResult = m_fftProcessorTD->appendAndProcess(tdAccumulatorU32);
        while (tdResult.isValid) {
            m_tdFFTBuffer.push(std::move(tdResult));
            tdResult = m_fftProcessorTD->appendAndProcess(QVector<quint32>());
        }
    }
    
    qDebug() << "[LocalFileReader] FFT处理完成，共生成" << totalFFTResults << "个FFT结果";
    
    m_isProcessing = false;
    emit progressChanged(100.0);
    emit batchProcessingFinished(totalFFTResults);
    emit processingFinished();
}