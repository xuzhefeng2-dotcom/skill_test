#ifndef FFTWORKER_H
#define FFTWORKER_H

#include <QObject>
#include <QVector>
#include <QThread>
#include <QMutex>
#include <QAtomicInt>
#include "fftprocessor.h"
#include "../core/ringbuffer.h"

/**
 * @brief FFT处理工作线程管理器（多线程并行版本）
 *
 * 为每个通道创建独立线程，实现并行FFT处理，提升性能
 */
class FFTWorker : public QObject
{
    Q_OBJECT
public:
    explicit FFTWorker(QObject *parent = nullptr);
    ~FFTWorker();

    // 获取FFT结果缓冲区引用
    RingBuffer<SpectrumResult>& getNSFFTBuffer() { return m_nsFFTBuffer; }
    RingBuffer<SpectrumResult>& getEWFFTBuffer() { return m_ewFFTBuffer; }
    RingBuffer<SpectrumResult>& getTDFFTBuffer() { return m_tdFFTBuffer; }

    // 获取FFTProcessor的访问接口（用于保存STFT数据）
    FFTProcessor* getNSFFTProcessor() { return m_fftProcessorNS; }
    FFTProcessor* getEWFFTProcessor() { return m_fftProcessorEW; }
    FFTProcessor* getTDFFTProcessor() { return m_fftProcessorTD; }
    // fftworker.h 里加：
double getNSMaxValue() const { return m_fftProcessorNS ? m_fftProcessorNS->maxValue() : -200.0; }
double getEWMaxValue() const { return m_fftProcessorEW ? m_fftProcessorEW->maxValue() : -200.0; }
double getTDMaxValue() const { return m_fftProcessorTD ? m_fftProcessorTD->maxValue() : -200.0; }
public slots:
    // 重置所有FFT处理器和结果缓冲区（循环重启时调用）
    void resetAll();

    // 设置采样率（应用到所有通道）
    void setSampleRate(double fsHz);

    // 设置分析步进（应用到所有通道）
    void setAnalysisHop(int hop);

    // 处理一批数据（16位）
    void processData(const QVector<quint16> &nsData, const QVector<quint16> &ewData, const QDateTime &timestamp);
    void processData(const QVector<quint16> &tdData, const QVector<quint16> &nsData, const QVector<quint16> &ewData, const QDateTime &timestamp);

    // 处理一批数据（24位）
    void processData(const QVector<quint32> &nsData, const QVector<quint32> &ewData, const QDateTime &timestamp);
    void processData(const QVector<quint32> &tdData, const QVector<quint32> &nsData, const QVector<quint32> &ewData, const QDateTime &timestamp);

signals:
    // 处理完成信号
    void processingFinished(int nsResultCount, int ewResultCount);
    void spectrumTripletReady(const SpectrumResult &ns,
        const SpectrumResult &ew,const SpectrumResult &td);
        //const SpectrumResult &td); 
    // 内部信号：用于跨线程调用FFTProcessor
    void processNS16(QVector<quint16> data, QDateTime timestamp);
    void processEW16(QVector<quint16> data, QDateTime timestamp);
    void processTD16(QVector<quint16> data, QDateTime timestamp);
    void processNS32(QVector<quint32> data, QDateTime timestamp);
    void processEW32(QVector<quint32> data, QDateTime timestamp);
    void processTD32(QVector<quint32> data, QDateTime timestamp);

private slots:
    // 各通道处理完成的回调
    void onNSProcessed(SpectrumResult result);
    void onEWProcessed(SpectrumResult result);
    void onTDProcessed(SpectrumResult result);

private:
    // FFT处理器（每个在独立线程中）
    FFTProcessor *m_fftProcessorNS;
    FFTProcessor *m_fftProcessorEW;
    FFTProcessor *m_fftProcessorTD;

    // 独立线程
    QThread *m_threadNS;
    QThread *m_threadEW;
    QThread *m_threadTD;

    // FFT结果环形缓冲区
    RingBuffer<SpectrumResult> m_nsFFTBuffer;
    RingBuffer<SpectrumResult> m_ewFFTBuffer;
    RingBuffer<SpectrumResult> m_tdFFTBuffer;

    // 并行处理同步
    QAtomicInt m_pendingTasks;  // 当前批次待完成的任务数
    QMutex m_resultMutex;       // 保护结果计数
    int m_currentBatchNSCount;
    int m_currentBatchEWCount;
};

#endif // FFTWORKER_H
