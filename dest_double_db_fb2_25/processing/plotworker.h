#ifndef PLOTWORKER_H
#define PLOTWORKER_H

#include <QObject>
#include <QTimer>
#include <QVector>
#include <QDateTime>
#include "core/ringbuffer.h"
#include "processing/fftprocessor.h"

// 注意：不要使用 CHANNEL_NS/EW/TD，避免与 protocol/packetformat.h 冲突
enum PlotChannel {
    PLOT_CH_NS = 0,
    PLOT_CH_EW = 1,
    PLOT_CH_TD = 2
};

// 一批瀑布图列数据（slot-major：每列连续）
struct SpectrogramBatch {
    int channel = PLOT_CH_NS;
    int startSlot = 0;      // 起始timeSlot
    int count = 0;          // 列数
    int freqCount = 0;      // 频点数
    QVector<double> freq;   // 频率轴
    QVector<double> data;   // 大小 = count*freqCount，data[t*freqCount + f]
    QVector<double> lastSpectrum; // 最后一列频谱（大小=freqCount）
    QDateTime timestamp;    // 本批对应时间戳（用于时间轴）
    bool hasTD = false;
};
Q_DECLARE_METATYPE(SpectrogramBatch)

/**
 * @brief 子线程绘图数据准备器（只做数据整理，不碰任何UI对象，不做FFT）
 *
 * 从 UDPClientReceiver / LocalFileReader 提供的 FFT RingBuffer 中取 SpectrumResult，
 * 将 freq-major 格式转换为 slot-major 批数据，然后通过信号发送到 UI 线程绘制。
 */
class PlotWorker : public QObject
{
    Q_OBJECT
public:
    explicit PlotWorker(QObject *parent = nullptr);
    ~PlotWorker() override;

    void setBuffers(RingBuffer<SpectrumResult>* ns,
                    RingBuffer<SpectrumResult>* ew,
                    RingBuffer<SpectrumResult>* td);

public slots:
    // 设置时间参数（用于基于时间戳计算槽位）
    void setTimeParameters(double displayTimeRange, int totalSlots);
    void start();
    void stop();
    void reset(int startSlot = 0);

private slots:
    void pollOnce();

signals:
    void batchReady(const SpectrogramBatch &batch);
    void workerStopped();

private:
    RingBuffer<SpectrumResult>* m_nsBuf = nullptr;
    RingBuffer<SpectrumResult>* m_ewBuf = nullptr;
    RingBuffer<SpectrumResult>* m_tdBuf = nullptr;

    QTimer* m_timer = nullptr;
    bool m_running = false;
    int m_currentSlot = 0;

    // 时间参数（用于基于时间戳计算槽位）
    double m_displayTimeRange = 100.0;  // 显示时间范围（秒）
    int m_totalSlots = 2032;            // 总槽数
    QDateTime m_startTimestamp;         // 起始时间戳（第一个数据包）
    bool m_hasStartTimestamp = false;   // 是否已记录起始时间戳

    static SpectrogramBatch makeBatch(int channel, int startSlot, const SpectrumResult& r);
};

#endif // PLOTWORKER_H
