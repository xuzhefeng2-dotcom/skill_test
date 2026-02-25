#ifndef LOCALFILEREADER_H
#define LOCALFILEREADER_H

#include <QObject>
#include <QFile>
#include <QVector>
#include <QString>
#include <QTimer>
#include "ringbuffer.h"
#include "fftprocessor.h"

/**
 * @brief 本地文件读取器
 *
 * 直接读取COS文件数据，分离双通道，进行FFT处理
 * 不需要UDP传输，直接用于本地文件分析
 */
class LocalFileReader : public QObject
{
    Q_OBJECT
public:
    explicit LocalFileReader(QObject *parent = nullptr);
    ~LocalFileReader();

    // 打开文件
    bool openFile(const QString &filePath);

public slots:
    // 异步打开文件（用于线程化：在LocalFileReader所属线程中执行）
    void openFileAsync(const QString &filePath);
    void closeFileAsync();

    // 关闭文件
    void closeFile();

    // 开始处理
    void startProcessing();

    // 停止处理
    void stopProcessing();

    // 暂停/继续处理
    void pauseProcessing();
    void resumeProcessing();

    // 重置到文件开头
    void reset();

public:

    // 是否正在处理
    bool isProcessing() const { return m_isProcessing; }

    // 是否暂停
    bool isPaused() const { return m_isPaused; }

    // 获取文件信息
    QString filePath() const { return m_filePath; }
    qint64 fileSizeBytes() const { return m_fileSize; }

    int totalFrames() const { return m_totalFrames; }
    int currentFrame() const { return m_currentFrame; }
    double progress() const { return m_totalFrames > 0 ? (double)m_currentFrame / m_totalFrames * 100.0 : 0.0; }
    
    // 采样率（Hz），用于时间轴/时长计算
    void setSampleRate(double fsHz);
    // 文件回放模式：设置分析步进（样本点），用于限制瀑布图列数以提升速度
    void setAnalysisHop(int hopSamples);

    // ✅ 设置启用的通道（由用户菜单控制）
    void setEnabledChannels(bool ewEnabled, bool nsEnabled, bool tdEnabled);

    double sampleRate() const { return m_sampleRate; }
    double recordingDurationSeconds() const;
    QString recordingFileName() const { return m_recordingFileName; }
    QString recordingDuration() const;

    // 获取FFT结果ringbuffer引用
    RingBuffer<SpectrumResult>& getEWFFTBuffer() { return m_ewFFTBuffer; }
    RingBuffer<SpectrumResult>& getNSFFTBuffer() { return m_nsFFTBuffer; }
    RingBuffer<SpectrumResult>& getTDFFTBuffer() { return m_tdFFTBuffer; }

    // 设置处理速度（帧/秒，0表示尽可能快）
    void setProcessingSpeed(int framesPerSecond);

signals:
    void fileOpened(const QString &fileName, int totalFrames);
    void frameProcessed(int frameNumber, int totalFrames);
    void processingStarted();
    void processingStopped();
    void processingPaused();
    void processingResumed();
    void processingFinished();
    void batchProcessingFinished(int totalFFTResults);  // 批量处理完成信号
    void error(const QString &message);
    void progressChanged(double percent);

private slots:
    void processBatch();

public:
    // 批量处理模式：一次性读取所有数据并FFT
    void startBatchProcessing(const QString &filePath);

private:
    double m_sampleRate = 250000.0;  // 默认250kHz（用户需求）
    // 读取下一帧数据（3字节转4字节，高位为0）
    bool readNextFrame(QVector<quint32> &ewData, QVector<quint32> &nsData, QVector<quint32> *tdData = nullptr);
    
    // 批量处理：一次性读取所有帧数据
    bool readAllFramesBatch(const QString &filePath);

    QFile m_file;
    QString m_filePath;
    QString m_recordingFileName;
    int m_frameSize;        // 每帧采样点数
    int m_totalFrames;      // 总帧数
    int m_currentFrame;     // 当前帧号
    qint64 m_fileSize;      // 文件大小

    // ✅ 通道启用状态（由用户菜单控制，默认全部启用）
    bool m_channelEWEnabled = true;
    bool m_channelNSEnabled = true;
    bool m_channelTDEnabled = true;

    bool m_isProcessing;
    bool m_isPaused;

    // FFT结果环形缓冲区（逐帧模式使用）
    RingBuffer<SpectrumResult> m_ewFFTBuffer;
    RingBuffer<SpectrumResult> m_nsFFTBuffer;
    RingBuffer<SpectrumResult> m_tdFFTBuffer;

    // FFT处理器
    FFTProcessor *m_fftProcessorNS;
    FFTProcessor *m_fftProcessorEW;
    FFTProcessor *m_fftProcessorTD;

    // 处理定时器
    QTimer *m_processTimer;
    int m_framesPerBatch;   // 每批处理帧数
    int m_processingSpeed;  // 处理速度（帧/秒）

    // 数据格式常量（用户需求：936字节/帧，312采样点）
    static constexpr int SAMPLES_PER_FRAME = 312;     // 每帧312采样点
    static constexpr int FRAME_SIZE_BYTES = 936;      // 每帧936字节（双通道：312*3*2=1872，但用户说是936）
    static constexpr int BYTES_PER_SAMPLE_PER_CHANNEL = 3;  // 每通道每采样点3字节
    static constexpr int FRAMES_PER_BATCH = 20;
    static constexpr int RINGBUF_CAPACITY = 1000;
};

#endif // LOCALFILEREADER_H