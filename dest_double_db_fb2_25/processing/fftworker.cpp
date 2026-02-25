#include "fftworker.h"
#include <QDebug>

FFTWorker::FFTWorker(QObject *parent)
    : QObject(parent)
    , m_fftProcessorNS(nullptr)
    , m_fftProcessorEW(nullptr)
    , m_fftProcessorTD(nullptr)
    , m_threadNS(nullptr)
    , m_threadEW(nullptr)
    , m_threadTD(nullptr)
    , m_nsFFTBuffer(1000)  // FFT结果缓冲区,容量1000个结果
    , m_ewFFTBuffer(1000)
    , m_tdFFTBuffer(1000)
    , m_pendingTasks(0)
    , m_currentBatchNSCount(0)
    , m_currentBatchEWCount(0)
{
    // 创建NS通道的独立线程
    m_threadNS = new QThread();
    m_fftProcessorNS = new FFTProcessor();
    m_fftProcessorNS->moveToThread(m_threadNS);
    connect(m_threadNS, &QThread::finished, m_fftProcessorNS, &QObject::deleteLater);
    m_threadNS->start();
    m_threadNS->setPriority(QThread::HighPriority);

    // 创建EW通道的独立线程
    m_threadEW = new QThread();
    m_fftProcessorEW = new FFTProcessor();
    m_fftProcessorEW->moveToThread(m_threadEW);
    connect(m_threadEW, &QThread::finished, m_fftProcessorEW, &QObject::deleteLater);
    m_threadEW->start();
    m_threadEW->setPriority(QThread::HighPriority);

    // 创建TD通道的独立线程
    m_threadTD = new QThread();
    m_fftProcessorTD = new FFTProcessor();
    m_fftProcessorTD->moveToThread(m_threadTD);
    connect(m_threadTD, &QThread::finished, m_fftProcessorTD, &QObject::deleteLater);
    m_threadTD->start();
    m_threadTD->setPriority(QThread::HighPriority);

    qDebug() << "[FFTWorker] 多线程并行模式初始化完成 (3个独立FFT线程)";
}

void FFTWorker::resetAll()
{
    qDebug() << "[FFTWorker] 重置所有FFT处理器和结果缓冲区";

    // 清空FFT处理器内部状态（在各自线程中执行）
    if (m_fftProcessorNS) {
        QMetaObject::invokeMethod(m_fftProcessorNS, "clearBuffer", Qt::BlockingQueuedConnection);
    }
    if (m_fftProcessorEW) {
        QMetaObject::invokeMethod(m_fftProcessorEW, "clearBuffer", Qt::BlockingQueuedConnection);
    }
    if (m_fftProcessorTD) {
        QMetaObject::invokeMethod(m_fftProcessorTD, "clearBuffer", Qt::BlockingQueuedConnection);
    }

    // 清空FFT结果缓冲区
    m_nsFFTBuffer.clear();
    m_ewFFTBuffer.clear();
    m_tdFFTBuffer.clear();

    // 重置计数器
    m_pendingTasks.fetchAndStoreRelaxed(0);
    m_currentBatchNSCount = 0;
    m_currentBatchEWCount = 0;
}

FFTWorker::~FFTWorker()
{
    // 停止并清理所有线程
    if (m_threadNS) {
        m_threadNS->quit();
        m_threadNS->wait(3000);
        delete m_threadNS;
    }
    if (m_threadEW) {
        m_threadEW->quit();
        m_threadEW->wait(3000);
        delete m_threadEW;
    }
    if (m_threadTD) {
        m_threadTD->quit();
        m_threadTD->wait(3000);
        delete m_threadTD;
    }
    qDebug() << "[FFTWorker] 销毁 (所有FFT线程已停止)";
}

void FFTWorker::processData(const QVector<quint32> &nsData, const QVector<quint32> &ewData, const QDateTime &timestamp)
{
    // 重置批次计数
    m_resultMutex.lock();
    m_currentBatchNSCount = 0;
    m_currentBatchEWCount = 0;
    m_resultMutex.unlock();

    // 设置待完成任务数
    m_pendingTasks=2;  // NS + EW

    // 并行处理NS通道（在独立线程中）
    QMetaObject::invokeMethod(this, [this, nsData, timestamp]() {
        SpectrumResult nsResult = m_fftProcessorNS->appendAndProcess(nsData, timestamp);
        onNSProcessed(nsResult);
    }, Qt::QueuedConnection);

    // 并行处理EW通道（在独立线程中）
    QMetaObject::invokeMethod(this, [this, ewData, timestamp]() {
        SpectrumResult ewResult = m_fftProcessorEW->appendAndProcess(ewData, timestamp);
        onEWProcessed(ewResult);
    }, Qt::QueuedConnection);
}

void FFTWorker::processData(const QVector<quint32> &tdData, const QVector<quint32> &nsData, const QVector<quint32> &ewData, const QDateTime &timestamp)
{
    // 重置批次计数
    m_resultMutex.lock();
    m_currentBatchNSCount = 0;
    m_currentBatchEWCount = 0;
    m_resultMutex.unlock();

    // 设置待完成任务数
    m_pendingTasks=3;  // TD + NS + EW

    // 并行处理TD通道
    QMetaObject::invokeMethod(this, [this, tdData, timestamp]() {
        SpectrumResult tdResult = m_fftProcessorTD->appendAndProcess(tdData, timestamp);
        onTDProcessed(tdResult);
    }, Qt::QueuedConnection);

    // 并行处理NS通道
    QMetaObject::invokeMethod(this, [this, nsData, timestamp]() {
        SpectrumResult nsResult = m_fftProcessorNS->appendAndProcess(nsData, timestamp);
        onNSProcessed(nsResult);
    }, Qt::QueuedConnection);

    // 并行处理EW通道
    QMetaObject::invokeMethod(this, [this, ewData, timestamp]() {
        SpectrumResult ewResult = m_fftProcessorEW->appendAndProcess(ewData, timestamp);
        onEWProcessed(ewResult);
    }, Qt::QueuedConnection);
}

void FFTWorker::processData(const QVector<quint16> &nsData, const QVector<quint16> &ewData, const QDateTime &timestamp)
{
    // 16位版本 - 转换为32位后调用
    QVector<quint32> nsData32(nsData.size());
    QVector<quint32> ewData32(ewData.size());
    for (int i = 0; i < nsData.size(); ++i) {
        nsData32[i] = nsData[i];
    }
    for (int i = 0; i < ewData.size(); ++i) {
        ewData32[i] = ewData[i];
    }
    processData(nsData32, ewData32, timestamp);
}

void FFTWorker::processData(const QVector<quint16> &tdData, const QVector<quint16> &nsData, const QVector<quint16> &ewData, const QDateTime &timestamp)
{
    // 16位版本 - 转换为32位后调用
    QVector<quint32> tdData32(tdData.size());
    QVector<quint32> nsData32(nsData.size());
    QVector<quint32> ewData32(ewData.size());
    for (int i = 0; i < tdData.size(); ++i) {
        tdData32[i] = tdData[i];
    }
    for (int i = 0; i < nsData.size(); ++i) {
        nsData32[i] = nsData[i];
    }
    for (int i = 0; i < ewData.size(); ++i) {
        ewData32[i] = ewData[i];
    }
    processData(tdData32, nsData32, ewData32, timestamp);
}

void FFTWorker::onNSProcessed(SpectrumResult result)
{
    // 保存结果
    if (result.isValid) {
        m_nsFFTBuffer.push(std::move(result));
        m_resultMutex.lock();
        m_currentBatchNSCount = 1;
        m_resultMutex.unlock();
    }

    // 减少待完成任务计数
    int remaining = m_pendingTasks.fetchAndAddRelaxed(-1) - 1;

    // 如果所有任务完成，发出信号
    if (remaining == 0) {
        m_resultMutex.lock();
        int nsCount = m_currentBatchNSCount;
        int ewCount = m_currentBatchEWCount;
        m_resultMutex.unlock();
        emit processingFinished(nsCount, ewCount);
    }
}

void FFTWorker::onEWProcessed(SpectrumResult result)
{
    // 保存结果
    if (result.isValid) {
        m_ewFFTBuffer.push(std::move(result));
        m_resultMutex.lock();
        m_currentBatchEWCount = 1;
        m_resultMutex.unlock();
    }

    // 减少待完成任务计数
    int remaining = m_pendingTasks.fetchAndAddRelaxed(-1) - 1;

    // 如果所有任务完成，发出信号
    if (remaining == 0) {
        m_resultMutex.lock();
        int nsCount = m_currentBatchNSCount;
        int ewCount = m_currentBatchEWCount;
        m_resultMutex.unlock();
        emit processingFinished(nsCount, ewCount);
    }
}

void FFTWorker::onTDProcessed(SpectrumResult result)
{
    // 保存结果
    if (result.isValid) {
        m_tdFFTBuffer.push(std::move(result));
    }

    // 减少待完成任务计数
    int remaining = m_pendingTasks.fetchAndAddRelaxed(-1) - 1;

    // 如果所有任务完成，发出信号
    if (remaining == 0) {
        m_resultMutex.lock();
        int nsCount = m_currentBatchNSCount;
        int ewCount = m_currentBatchEWCount;
        m_resultMutex.unlock();
        emit processingFinished(nsCount, ewCount);
    }
}

void FFTWorker::setSampleRate(double fsHz)
{
    // 跨线程调用，使用QMetaObject::invokeMethod
    QMetaObject::invokeMethod(m_fftProcessorNS, [this, fsHz]() {
        m_fftProcessorNS->setSampleRate(fsHz);
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(m_fftProcessorEW, [this, fsHz]() {
        m_fftProcessorEW->setSampleRate(fsHz);
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(m_fftProcessorTD, [this, fsHz]() {
        m_fftProcessorTD->setSampleRate(fsHz);
    }, Qt::QueuedConnection);

    qDebug() << "[FFTWorker] 设置采样率:" << fsHz << "Hz (应用到3个并行线程)";
}

void FFTWorker::setAnalysisHop(int hop)
{
    // 跨线程调用，使用QMetaObject::invokeMethod
    QMetaObject::invokeMethod(m_fftProcessorNS, [this, hop]() {
        m_fftProcessorNS->setAnalysisHop(hop);
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(m_fftProcessorEW, [this, hop]() {
        m_fftProcessorEW->setAnalysisHop(hop);
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(m_fftProcessorTD, [this, hop]() {
        m_fftProcessorTD->setAnalysisHop(hop);
    }, Qt::QueuedConnection);

    qDebug() << "[FFTWorker] 设置分析步进:" << hop << "样本 (应用到3个并行线程)";
}
