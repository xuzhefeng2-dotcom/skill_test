#include "plotworker.h"
#include <QtGlobal>
#include <QDebug>
#include <QThread>
PlotWorker::PlotWorker(QObject *parent)
    : QObject(parent)
{
    // 定时轮询（在子线程事件循环中运行）
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &PlotWorker::pollOnce);
}

PlotWorker::~PlotWorker()
{
    stop();
}

void PlotWorker::setBuffers(RingBuffer<SpectrumResult>* ns,
                            RingBuffer<SpectrumResult>* ew,
                            RingBuffer<SpectrumResult>* td)
{
    m_nsBuf = ns;
    m_ewBuf = ew;
    m_tdBuf = td;
}

void PlotWorker::setTimeParameters(double displayTimeRange, int totalSlots)
{
    m_displayTimeRange = displayTimeRange;
    m_totalSlots = totalSlots;
    qDebug() << "[PlotWorker] 设置时间参数: displayTimeRange=" << displayTimeRange
             << "s, totalSlots=" << totalSlots;
}

void PlotWorker::reset(int startSlot)
{
    m_currentSlot = startSlot;
    m_hasStartTimestamp = false;
    m_startTimestamp = QDateTime();
    qDebug() << "[PlotWorker] 重置: startSlot=" << startSlot;
}

void PlotWorker::start()
{
    if (m_running) return;
    m_running = true;
    // 5ms轮询：尽快把结果取出来，避免GUI线程做重活
    m_timer->start(5);
}
void PlotWorker::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_timer) m_timer->stop();
    emit workerStopped();
}

SpectrogramBatch PlotWorker::makeBatch(int channel, int startSlot, const SpectrumResult& r)
{
    SpectrogramBatch b;
    b.channel = channel;
    b.startSlot = startSlot;
    b.count = r.timeCount;
    b.freqCount = r.freqCount;
    b.freq = r.freq;
    b.timestamp = r.timestamp;

    const int T = r.timeCount;
    const int F = r.freqCount;

    if (T <= 0 || F <= 0 || r.spectrum.isEmpty()) {
        b.count = 0;
        return b;
    }

    // slot-major：每一列连续
    b.data.resize(T * F);

    // r.spectrum 是 freq-major: spectrum[f*T + t]
    // 转置到 slot-major: data[t*F + f]
    const double* src = r.spectrum.constData();
    double* dst = b.data.data();

    for (int t = 0; t < T; ++t) {
        double* col = dst + t * F;
        for (int f = 0; f < F; ++f) {
            col[f] = src[f * T + t];
        }
    }

    // 最后一列频谱（用于右侧频谱图）
    b.lastSpectrum.resize(F);
    const int lastT = T - 1;
    for (int f = 0; f < F; ++f) {
        b.lastSpectrum[f] = src[f * T + lastT];
    }

    return b;
}

void PlotWorker::pollOnce()
{
    if (!m_running || !m_nsBuf || !m_ewBuf) return;

    // 取一组同步的NS/EW（TD可选）
    if (m_nsBuf->isEmpty() || m_ewBuf->isEmpty()) return;

    SpectrumResult ns;
    if (!m_nsBuf->tryPop(ns)) return;

    SpectrumResult ew;
    if (!m_ewBuf->tryPop(ew)) {
        m_nsBuf->push(std::move(ns));
        return;
    }

    SpectrumResult td;
    bool hasTD = false;
    // TD通道是可选的，不应该阻塞NS/EW的绘制
    if (m_tdBuf && !m_tdBuf->isEmpty()) {
        if (m_tdBuf->tryPop(td)) {
            hasTD = td.isValid;
        }
    }

    if (!ns.isValid || !ew.isValid || (hasTD && !td.isValid)) return;

    // ✅ 简单顺序递增槽位，快速填充显示
    int startSlot = m_currentSlot;

    // 生成批数据并发给UI
    auto nsBatch = makeBatch(PLOT_CH_NS, startSlot, ns);
    auto ewBatch = makeBatch(PLOT_CH_EW, startSlot, ew);
    nsBatch.hasTD = hasTD;
    ewBatch.hasTD = hasTD;

    if (nsBatch.count > 0) emit batchReady(nsBatch);
    if (ewBatch.count > 0) emit batchReady(ewBatch);

    if (hasTD) {
        auto tdBatch = makeBatch(PLOT_CH_TD, startSlot, td);
        tdBatch.hasTD = true;
        if (tdBatch.count > 0) emit batchReady(tdBatch);
    }

    // 更新当前槽位（基于时间戳计算，而非简单递增）
    m_currentSlot = startSlot + ns.timeCount;
}
