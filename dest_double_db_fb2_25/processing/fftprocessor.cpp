#include "fftprocessor.h"
#include <QDebug>
#include <cmath>
#include <algorithm>
#include <limits>
#include <QElapsedTimer>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FFTProcessor::FFTProcessor(QObject *parent)
    : QObject(parent)
    , m_filterInitialized(false)
    , m_filterStateIndex(0)
    , m_fftInput(nullptr)
    , m_fftOutput(nullptr)
    , m_fftPlan(nullptr)
    , m_fftInitialized(false)
    , m_processedSamples(0)
    , m_maxValue(-200.0)
    , m_totalSegments(0)
    , m_fftConvSize(0)
    , m_filterFFT(nullptr)
    , m_convInput(nullptr)
    , m_convInputFFT(nullptr)
    , m_convOutputFFT(nullptr)
    , m_convOutput(nullptr)
    , m_convForwardPlan(nullptr)
    , m_convInversePlan(nullptr)
    , m_filterFFTInitialized(false)
    , m_baseTimestampSet(false)
{
    // 导入FFTW wisdom以加速plan创建
    int wisdom_loaded = fftw_import_wisdom_from_filename("fftw_wisdom.dat");
    if (wisdom_loaded) {
        qDebug() << "[FFTProcessor] FFTW wisdom加载成功";
    } else {
        qDebug() << "[FFTProcessor] FFTW wisdom文件不存在，将进行首次测量";
    }

    // 注释掉滤波器初始化，因为实际未使用滤波功能
    // initializeFilter();
    // initializeFilterFFT();  // 初始化FFT卷积
    initializeFFT();

    // 预计算频率向量和有效频率索引
    const int numUniquePts = NFFT / 2;
    for (int i = 0; i < numUniquePts; ++i) {
        double freqVal = m_fs / NFFT * i / 1000.0;  // kHz
        if (freqVal >= 0.1 && freqVal <= 2000.0) {
            m_validFreqIndices.append(i);
            m_freqVector.append(freqVal);
        }
    }

    qDebug() << "FFTProcessor初始化完成，有效频点数:" << m_freqVector.size();
}

FFTProcessor::~FFTProcessor()
{
    cleanupFFT();
    cleanupFilterFFT();
}

void FFTProcessor::setSampleRate(double fsHz)
{
    if (fsHz <= 0) return;
    if (qFuzzyCompare(m_fs, fsHz)) return;
    m_fs = fsHz;

    // 采样率变化会影响滤波器设计与频率/时间刻度，重建相关缓存
    clearBuffer();
    // 注释掉滤波器初始化，因为实际未使用滤波功能
    // initializeFilter();
    // initializeFilterFFT();

    // 重新预计算频率向量与有效频率索引
    m_freqVector.clear();
    m_validFreqIndices.clear();
    const int numUniquePts = NFFT / 2;
    for (int i = 0; i < numUniquePts; ++i) {
        double freqVal = m_fs / NFFT * i / 1000.0;  // kHz
        if (freqVal >= 0.1 && freqVal <= 2000.0) {
            m_validFreqIndices.append(i);
            m_freqVector.append(freqVal);
        }
    }
    }

void FFTProcessor::setAnalysisHop(int hop)
{
    // hop 以样本点为单位。文件回放时可设大一些以提升速度/降低时间分辨率。
    m_analysisHop = qMax(1, hop);
}



void FFTProcessor::initializeFilter()
{
    if (m_filterInitialized) {
        return;
    }

    // 带通滤波器设计
    QVector<double> fcuts = {100, 300, 50000, 53000};
    QVector<double> mags = {0, 1, 0};
    double Ap = 1.0;
    double As = 40.0;
    QVector<double> devs(3);
    devs[0] = std::pow(10.0, -As / 20.0);
    devs[1] = (std::pow(10.0, Ap / 20.0) - 1.0) / (std::pow(10.0, Ap / 20.0) + 1.0);
    devs[2] = std::pow(10.0, -As / 20.0);

    int filter_n;
    QVector<double> Wn;
    double filter_beta;
    kaiserord(fcuts, mags, devs, m_fs, filter_n, Wn, filter_beta);

    QVector<double> kaiser_window = kaiser(filter_n + 1, filter_beta);
    m_filterCoeffs = fir1(filter_n, Wn, "bandpass", kaiser_window);

    // 初始化滤波器状态
    m_filterState.resize(m_filterCoeffs.size(), 0.0);

    m_filterInitialized = true;
    qDebug() << "滤波器初始化完成，阶数:" << filter_n;
}

void FFTProcessor::initializeFFT()
{
    if (m_fftInitialized) {
        return;
    }

    // 预分配FFTW内存
    m_fftInput = static_cast<double*>(fftw_malloc(sizeof(double) * NFFT));
    m_fftOutput = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * (NFFT / 2 + 1)));
    m_fftPlan = fftw_plan_dft_r2c_1d(NFFT, m_fftInput, m_fftOutput, FFTW_MEASURE);

    // 预计算Hamming窗
    m_hammingWindow.resize(NFFT);
    for (int i = 0; i < NFFT; ++i) {
        m_hammingWindow[i] = 0.54 - 0.46 * std::cos(2.0 * M_PI * i / (NFFT - 1));
        //m_hammingWindow[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / (NFFT - 1));
    }

    m_fftInitialized = true;
    qDebug() << "FFTW初始化完成";

    // 导出FFTW wisdom以加速后续启动
    fftw_export_wisdom_to_filename("fftw_wisdom.dat");
}

void FFTProcessor::cleanupFFT()
{
    if (m_fftPlan) {
        fftw_destroy_plan(m_fftPlan);
        m_fftPlan = nullptr;
    }
    if (m_fftInput) {
        fftw_free(m_fftInput);
        m_fftInput = nullptr;
    }
    if (m_fftOutput) {
        fftw_free(m_fftOutput);
        m_fftOutput = nullptr;
    }
    m_fftInitialized = false;
}

void FFTProcessor::initializeFilterFFT()
{
    if (m_filterFFTInitialized || m_filterCoeffs.isEmpty()) {
        return;
    }

    const int M = m_filterCoeffs.size();  // 滤波器长度（约2787）
    const int L = FFT_CONV_BLOCK_SIZE;    // 块大小 8192

    // FFT大小：选择2的幂次，满足 N >= L + M - 1
    // L + M - 1 = 8192 + 2787 - 1 = 10978
    // 选择 16384 (2^14)
    m_fftConvSize = 16384;

    qDebug() << "[FFT卷积] 滤波器长度:" << M
             << "块大小:" << L
             << "FFT大小:" << m_fftConvSize;

    // 分配内存
    m_filterFFT = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * m_fftConvSize));
    m_convInput = static_cast<double*>(fftw_malloc(sizeof(double) * m_fftConvSize));
    m_convInputFFT = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * m_fftConvSize));
    m_convOutputFFT = static_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * m_fftConvSize));
    m_convOutput = static_cast<double*>(fftw_malloc(sizeof(double) * m_fftConvSize));

    // 创建FFT计划（使用FFTW_MEASURE获得最优性能）
    m_convForwardPlan = fftw_plan_dft_r2c_1d(m_fftConvSize, m_convInput, m_convInputFFT, FFTW_MEASURE);
    m_convInversePlan = fftw_plan_dft_c2r_1d(m_fftConvSize, m_convOutputFFT, m_convOutput, FFTW_MEASURE);

    // 预计算滤波器的FFT H(f)
    // 1. 零填充滤波器系数到FFT大小
    std::fill(m_convInput, m_convInput + m_fftConvSize, 0.0);
    for (int i = 0; i < M; ++i) {
        m_convInput[i] = m_filterCoeffs[i];
    }

    // 2. 执行FFT
    fftw_execute_dft_r2c(m_convForwardPlan, m_convInput, m_filterFFT);

    // 3. 初始化overlap缓冲区（大小为 M-1）
    m_overlapBuffer.resize(M - 1);
    m_overlapBuffer.fill(0.0);

    m_filterFFTInitialized = true;
    qDebug() << "[FFT卷积] 初始化完成，overlap缓冲区大小:" << m_overlapBuffer.size();

    // 导出FFTW wisdom以加速后续启动
    fftw_export_wisdom_to_filename("fftw_wisdom.dat");
}

void FFTProcessor::cleanupFilterFFT()
{
    if (m_convForwardPlan) {
        fftw_destroy_plan(m_convForwardPlan);
        m_convForwardPlan = nullptr;
    }
    if (m_convInversePlan) {
        fftw_destroy_plan(m_convInversePlan);
        m_convInversePlan = nullptr;
    }
    if (m_filterFFT) {
        fftw_free(m_filterFFT);
        m_filterFFT = nullptr;
    }
    if (m_convInput) {
        fftw_free(m_convInput);
        m_convInput = nullptr;
    }
    if (m_convInputFFT) {
        fftw_free(m_convInputFFT);
        m_convInputFFT = nullptr;
    }
    if (m_convOutputFFT) {
        fftw_free(m_convOutputFFT);
        m_convOutputFFT = nullptr;
    }
    if (m_convOutput) {
        fftw_free(m_convOutput);
        m_convOutput = nullptr;
    }
    m_filterFFTInitialized = false;
}

void FFTProcessor::filterIncremental(const double *input, int inputLen)
{
    const int nb = m_filterCoeffs.size();
    const double *b = m_filterCoeffs.constData();

    // 预留空间
    int oldSize = m_filteredBuffer.size();
    m_filteredBuffer.resize(oldSize + inputLen);

    // 确保状态数组已初始化
    if (m_filterState.size() != nb) {
        m_filterState.resize(nb);
        m_filterState.fill(0.0);
        m_filterStateIndex = 0;
    }

    double *state = m_filterState.data();

    for (int i = 0; i < inputLen; ++i) {
        // 将新输入写入当前位置
        state[m_filterStateIndex] = input[i];

        // 计算输出（使用环形缓冲区方式访问）
        double sum = 0.0;
        int idx = m_filterStateIndex;
        for (int j = 0; j < nb; ++j) {
            sum += b[j] * state[idx];
            idx--;
            if (idx < 0) idx = nb - 1;
        }
        m_filteredBuffer[oldSize + i] = sum;

        // 移动环形缓冲区索引
        m_filterStateIndex++;
        if (m_filterStateIndex >= nb) m_filterStateIndex = 0;
    }
}

void FFTProcessor::filterFFTConvolution(const double *input, int inputLen)
{
    if (!m_filterFFTInitialized) {
        qWarning() << "[FFT卷积] 未初始化，无法执行";
        return;
    }

    const int M = m_filterCoeffs.size();  // 滤波器长度（2787）
    const int L = FFT_CONV_BLOCK_SIZE;    // 块大小（8192）
    const int N = m_fftConvSize;          // FFT大小（16384）

    // 预留输出空间
    int oldSize = m_filteredBuffer.size();
    m_filteredBuffer.resize(oldSize + inputLen);

    int inputPos = 0;  // 当前处理到的输入位置
    int outputPos = oldSize;  // 当前写入的输出位置

    // 分块处理输入数据
    while (inputPos < inputLen) {
        // 1. 取出当前块（最多L个样本）
        int blockLen = std::min(L, inputLen - inputPos);

        // 2. 零填充输入块到FFT大小
        std::fill(m_convInput, m_convInput + N, 0.0);
        for (int i = 0; i < blockLen; ++i) {
            m_convInput[i] = input[inputPos + i];
        }

        // 3. 对输入块执行FFT：X(f)
        fftw_execute_dft_r2c(m_convForwardPlan, m_convInput, m_convInputFFT);

        // 4. 频域相乘：Y(f) = H(f) × X(f)
        // 注意：fftw_complex是double[2]，[0]是实部，[1]是虚部
        // 复数乘法：(a+bi)(c+di) = (ac-bd) + (ad+bc)i
        // r2c FFT只返回N/2+1个复数（利用实信号的对称性）
        for (int i = 0; i < N/2 + 1; ++i) {
            double h_re = m_filterFFT[i][0];
            double h_im = m_filterFFT[i][1];
            double x_re = m_convInputFFT[i][0];
            double x_im = m_convInputFFT[i][1];

            m_convOutputFFT[i][0] = h_re * x_re - h_im * x_im;  // 实部
            m_convOutputFFT[i][1] = h_re * x_im + h_im * x_re;  // 虚部
        }

        // 5. 执行IFFT：y[n]
        fftw_execute_dft_c2r(m_convInversePlan, m_convOutputFFT, m_convOutput);

        // 6. FFTW的IFFT不自动归一化，需要除以N
        for (int i = 0; i < N; ++i) {
            m_convOutput[i] /= N;
        }

        // 7. Overlap-Add：将当前块的输出与overlap缓冲区累加
        // 输出的有效长度是 L + M - 1
        int validOutputLen = blockLen + M - 1;

        // 7.1 先处理overlap区域（前M-1个样本）
        for (int i = 0; i < M - 1; ++i) {
            m_filteredBuffer[outputPos + i] = m_overlapBuffer[i] + m_convOutput[i];
        }

        // 7.2 处理当前块的非重叠部分
        for (int i = M - 1; i < blockLen; ++i) {
            m_filteredBuffer[outputPos + i] = m_convOutput[i];
        }

        // 7.3 保存新的overlap区域（用于下一块）
        for (int i = 0; i < M - 1; ++i) {
            m_overlapBuffer[i] = m_convOutput[blockLen + i];
        }

        // 8. 移动到下一块
        inputPos += blockLen;
        outputPos += blockLen;
    }
}

double FFTProcessor::besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    double x_half = x / 2.0;

    for (int k = 1; k < 50; ++k) {
        term *= (x_half / k) * (x_half / k);
        sum += term;
        if (term < 1e-12 * sum) {
            break;
        }
    }

    return sum;
}

QVector<double> FFTProcessor::kaiser(int n, double beta)
{
    QVector<double> w(n);
    double i0_beta = besselI0(beta);

    for (int i = 0; i < n; ++i) {
        double t = 2.0 * i / (n - 1) - 1.0;
        w[i] = besselI0(beta * std::sqrt(1.0 - t * t)) / i0_beta;
    }

    return w;
}

void FFTProcessor::kaiserord(const QVector<double> &fcuts, const QVector<double> &mags,
                              const QVector<double> &devs, double fs,
                              int &n, QVector<double> &Wn, double &beta)
{
    Q_UNUSED(mags);

    double df1 = (fcuts[1] - fcuts[0]) / fs;
    double df2 = (fcuts[3] - fcuts[2]) / fs;
    double df = std::min(df1, df2);

    double dev = devs[0];
    double A = -20.0 * std::log10(dev);

    if (A > 50.0) {
        beta = 0.1102 * (A - 8.7);
    } else if (A >= 21.0) {
        beta = 0.5842 * std::pow(A - 21.0, 0.4) + 0.07886 * (A - 21.0);
    } else {
        beta = 0.0;
    }

    n = static_cast<int>(std::ceil((A - 8.0) / (2.285 * 2.0 * M_PI * df)));
    if (n % 2 == 0) {
        n++;
    }

    Wn.resize(2);
    Wn[0] = (fcuts[1] + fcuts[0]) / 2.0 / (fs / 2.0);
    Wn[1] = (fcuts[2] + fcuts[3]) / 2.0 / (fs / 2.0);
}

QVector<double> FFTProcessor::fir1(int n, const QVector<double> &Wn,
                                    const QString &ftype, const QVector<double> &window)
{
    QVector<double> h(n + 1);
    int M = n / 2;

    if (ftype == "bandpass") {
        double wc1 = Wn[0] * M_PI;
        double wc2 = Wn[1] * M_PI;

        for (int i = 0; i <= n; ++i) {
            int k = i - M;
            if (k == 0) {
                h[i] = (wc2 - wc1) / M_PI;
            } else {
                h[i] = (std::sin(wc2 * k) - std::sin(wc1 * k)) / (M_PI * k);
            }

            if (i < window.size()) {
                h[i] *= window[i];
            }
        }
    }

    return h;
}

SpectrumResult FFTProcessor::appendAndProcess(const QVector<quint16> &rawData,
                                              const QDateTime &timestamp)
{
    // ===== DIAG =====
    static QElapsedTimer timer;
    static bool started = false;
    static qint64 lastReportMs = 0;
    static int callCount = 0;

    if (!started) {
        timer.start();
        started = true;
    }

    QElapsedTimer localTimer;
    localTimer.start();
    // =================

    SpectrumResult result;
    result.isValid = false;
    result.freqCount = 0;
    result.timeCount = 0;

    // 设置基准时间戳
    if (!m_baseTimestampSet && timestamp.isValid()) {
        m_baseTimestamp = timestamp;
        m_baseTimestampSet = true;
        qDebug() << "[FFTProcessor] 设置基准时间戳:"
                 << m_baseTimestamp.toString("yyyy-MM-dd HH:mm:ss.zzz");
    }

    if (rawData.isEmpty()) {
        return result;
    }

    // 1. 新数据 → double → 滤波
    QVector<double> newData(rawData.size());
    for (int i = 0; i < rawData.size(); ++i) {
        newData[i] = rawData[i] - HALF_MAX;
    }

    //filterFFTConvolution(newData.constData(), newData.size());
    m_filteredBuffer.append(newData);

    // ===== 内存泄漏修复：定期清理缓冲区 =====
    trimBuffersIfNeeded();

    // ===== 关键修复点：不再信"推算的段数" =====
    int appendedSegments = 0;   // ★ FIX：真实生成的 FFT 列数

    int totalSamples = m_filteredBuffer.size();
    int availableForFFT = totalSamples - NFFT;
    if (availableForFFT < 0) {
        return result;
    }

    int totalSegments = availableForFFT / m_analysisHop + 1;
    int processedSegments =
        (m_processedSamples > 0)
            ? (m_processedSamples - NFFT) / m_analysisHop + 1
            : 0;

    const double dt = static_cast<double>(m_analysisHop) / m_fs;
    const int numValidFreqs = m_validFreqIndices.size();

    // 2. 只处理真正还没处理过的段
    for (int seg = processedSegments; seg < totalSegments; ++seg) {
        int startIdx = seg * m_analysisHop;
        if (startIdx + NFFT > totalSamples) {
            break;
        }

        // 拷贝窗函数数据
        const double *filteredData = m_filteredBuffer.constData();
        for (int j = 0; j < NFFT; ++j) {
            m_fftInput[j] = filteredData[startIdx + j] * m_hammingWindow[j];
        }

        fftw_execute(m_fftPlan);

        QVector<double> segSpectrum(numValidFreqs);
        bool hasValidValue = false;

        for (int i = 0; i < numValidFreqs; ++i) {
            int freqIdx = m_validFreqIndices[i];

            // if (freqIdx < 5) {
            //     segSpectrum[i] = -200.0;
            //     continue;
            // }

            double re = m_fftOutput[freqIdx][0];
            double im = m_fftOutput[freqIdx][1];
            // double mag = std::sqrt(re * re + im * im);
            // double scaledMag = (2.0 / NFFT) * mag;

            // double dbVal = (scaledMag > 1e-20)
            //                    ? 10.0 * std::log10(scaledMag)
            //                    : -200.0;
double mag = std::sqrt(re * re + im * im);
const double eps = 1e-12;                 // 类似 MATLAB 的 eps 防止 log(0)
double dbVal = 20.0 * std::log10(mag / NFFT + eps);
            segSpectrum[i] = dbVal;

            if (dbVal > -199.0) {
                hasValidValue = true;
                if (m_maxValue < -199.0) {
                    m_maxValue = dbVal;
                } else if (dbVal > m_maxValue) {
                    m_maxValue += 0.3 * (dbVal - m_maxValue);
                }
            }
        }

        // ★ FIX：只有真正生成了“有效 FFT 列”才推进时间
        /* 若整列无有效值，仍然写入一列占位数据以保证时间轴连续 */

        m_spectrumCache.append(segSpectrum);
        m_timeVector.append(seg * dt);
        // 统一按 seg 位置记录时间，避免跳段/过滤导致时间轴变长或不连续
        m_totalSegments = qMax(m_totalSegments, seg + 1);
        appendedSegments++;
    }

    // ★ FIX：本轮没有任何 FFT 输出，直接无效返回
    if (appendedSegments <= 0) {
        return result;
    }

    // 更新已处理样本数
    m_processedSamples = (processedSegments + appendedSegments - 1) * m_analysisHop + NFFT;

    // 3. 只构建"真实存在的新增列"并进行归一化处理
    int startTimeIdx = m_spectrumCache.size() - appendedSegments;
    if (startTimeIdx < 0) startTimeIdx = 0;

    QVector<double> newTimeVector;
    QVector<double> flatSpectrum;
    flatSpectrum.reserve(numValidFreqs * appendedSegments);

    for (int t = startTimeIdx; t < m_spectrumCache.size(); ++t) {
        newTimeVector.append(m_timeVector[t]);
    }

    // 收集所有原始dB值
    for (int f = 0; f < numValidFreqs; ++f) {
        for (int t = startTimeIdx; t < m_spectrumCache.size(); ++t) {
            flatSpectrum.append(m_spectrumCache[t][f]);
            //m_timeVector.append( (double)m_timeVector.size() * (double)HOP_SIZE / m_fs );


        }
    }

    // 归一化处理（映射到40~110范围，与MATLAB一致）
    if (flatSpectrum.size() > 0) {
        // 1. 找到最小值和最大值
        double minDB = flatSpectrum[0];
        double maxDB = flatSpectrum[0];
        for (int i = 1; i < flatSpectrum.size(); ++i) {
            if (flatSpectrum[i] < minDB) minDB = flatSpectrum[i];
            if (flatSpectrum[i] > maxDB) maxDB = flatSpectrum[i];
        }

        // 2. 归一化到 0~1
        double range = maxDB - minDB;
        if (range > 1e-10) {  // 避免除以0
            for (int i = 0; i < flatSpectrum.size(); ++i) {
                flatSpectrum[i] = (flatSpectrum[i] - minDB) / range;
            }
        }

        // 3. 映射到 40~110（dB范围，与MATLAB的caxis一致）
        const double NORM_MIN = 40;
        const double NORM_MAX = 110.0;
        for (int i = 0; i < flatSpectrum.size(); ++i) {
            flatSpectrum[i] = NORM_MIN + flatSpectrum[i] * (NORM_MAX - NORM_MIN);
        }
    }
    result.time = newTimeVector;
    result.freq = m_freqVector;
    result.spectrum = flatSpectrum;
    result.freqCount = numValidFreqs;
    result.timeCount = appendedSegments;
    result.isValid = true;

    if (m_baseTimestampSet) {
        result.timestamp =
            m_baseTimestamp.addMSecs(static_cast<qint64>((m_timeVector.isEmpty()?0.0:m_timeVector.last()) * 1000.0));
    } else {
        result.timestamp = timestamp.isValid()
                               ? timestamp
                               : QDateTime::currentDateTime();
    }

    // ===== DIAG =====
    qint64 costMs = localTimer.elapsed();
    callCount++;

    qint64 now = timer.elapsed();
    if (now - lastReportMs >= 5000) {
        lastReportMs = now;
        qDebug() << "[FFT]"
                 << "elapsed(s)=" << now / 1000
                 << "calls/s=" << callCount / 5
                 << "lastCall(ms)=" << costMs
                 << "bufferSize=" << m_filteredBuffer.size()
                 << "spectra=" << m_spectrumCache.size();
        callCount = 0;
    }
    // =================

    // ★ FIX：强一致性保护（可长期保留）
    Q_ASSERT(m_timeVector.size() == m_spectrumCache.size());

    return result;
}


void FFTProcessor::clearBuffer()
{
    m_filteredBuffer.clear();
    m_spectrumCache.clear();
    m_timeVector.clear();
    m_processedSamples = 0;
    m_maxValue = -200.0;
    m_totalSegments = 0;

    // 重置基准时间戳
    m_baseTimestampSet = false;

    // 重置滤波器状态
    m_filterState.fill(0.0);

    // 重置FFT卷积的overlap缓冲区
    if (m_filterFFTInitialized) {
        m_overlapBuffer.fill(0.0);
    }
}

void FFTProcessor::trimBuffersIfNeeded()
{
    // 1. 清理 m_filteredBuffer（原始滤波数据）
    // 关键：只删除已经被FFT处理过的数据，避免破坏状态机
    if (m_filteredBuffer.size() > MAX_FILTERED_BUFFER_SAMPLES) {
        // 计算可以安全删除的样本数：已处理的样本减去NFFT（需要保留用于overlap）
        int safeToRemove = m_processedSamples - NFFT;

        if (safeToRemove > 0) {
            // 限制删除量，保留一半缓冲区
            int targetSize = MAX_FILTERED_BUFFER_SAMPLES / 2;
            int samplesToRemove = qMin(safeToRemove, m_filteredBuffer.size() - targetSize);

            if (samplesToRemove > 0) {
                m_filteredBuffer.remove(0, samplesToRemove);

                // 更新已处理样本数计数器（减去删除的样本数）
                m_processedSamples -= samplesToRemove;

                qDebug() << "[FFTProcessor] 清理 m_filteredBuffer，删除" << samplesToRemove
                         << "个样本，剩余" << m_filteredBuffer.size() << "个样本"
                         << "已处理样本数调整为" << m_processedSamples;
            }
        }
    }

    // 2. 清理 m_spectrumCache 和 m_timeVector（FFT结果缓存）
    if (m_spectrumCache.size() > MAX_SPECTRUM_CACHE_SEGMENTS) {
        // 删除旧的FFT结果，保留最近的一半
        int segmentsToRemove = m_spectrumCache.size() - MAX_SPECTRUM_CACHE_SEGMENTS / 2;

        if (segmentsToRemove > 0) {
            m_spectrumCache.remove(0, segmentsToRemove);
            m_timeVector.remove(0, segmentsToRemove);

            qDebug() << "[FFTProcessor] 清理 m_spectrumCache，删除" << segmentsToRemove
                     << "个FFT段，剩余" << m_spectrumCache.size() << "个段";
        }
    }
}

// 24位数据重载函数
SpectrumResult FFTProcessor::appendAndProcess(const QVector<quint32> &rawData24, const QDateTime &timestamp)
{
    SpectrumResult result;
    result.isValid = false;
    result.freqCount = 0;
    result.timeCount = 0;

    // 设置基准时间戳
    if (!m_baseTimestampSet && timestamp.isValid()) {
        m_baseTimestamp = timestamp;
        m_baseTimestampSet = true;
        qDebug() << "[FFTProcessor] 设置基准时间戳(24位):"
                 << m_baseTimestamp.toString("yyyy-MM-dd HH:mm:ss.zzz");
    }

    if (rawData24.isEmpty()) {
        return result;
    }

    // 将24位数据转换为double（有符号24位格式）
    // 如果最高位（bit 23）为1，说明是负数，需要进行符号扩展
    QVector<double> newData(rawData24.size());
    for (int i = 0; i < rawData24.size(); ++i) {
        quint32 val = rawData24[i];

        // 检查最高位（bit 23）是否为1
        if (val & 0x800000) {
            // 负数：进行符号扩展（将bit 24-31设为1）
            qint32 signedVal = val | 0xFF000000;
            newData[i] = static_cast<double>(signedVal);
        } else {
            // 正数：直接转换
            newData[i] = static_cast<double>(val);
        }
    }

    // 打印转换后的前30个数据值（用于验证通道对应关系）
    static int convertPrintCount = 0;
    if (convertPrintCount < 1 && newData.size() > 0) {
        qDebug() << "\n========== 3字节数据转换后前30个值 ==========";
        qDebug() << "索引\t原始24位\t转换后\t\t对应MATLAB";
        for (int i = 0; i < qMin(30, newData.size()); ++i) {
            // 计算对应的MATLAB通道和索引
            int matlab_idx = i / 3;
            QString ch_name;
            if (i % 3 == 0) ch_name = "CH2";  // NS在前
            else if (i % 3 == 1) ch_name = "CH1";  // EW
            else ch_name = "CH3";  // 第三个通道

            qDebug() << QString("%1\t%2\t%3\t%4[%5]")
                        .arg(i)
                        .arg(rawData24[i])
                        .arg(newData[i], 0, 'f', 2)
                        .arg(ch_name)
                        .arg(matlab_idx);
        }
        qDebug() << "==============================================";
        convertPrintCount++;
    }

    // 关闭滤波器：直接将数据追加到缓冲区，不进行滤波处理
    m_filteredBuffer.append(newData);

    // ===== 内存泄漏修复：定期清理缓冲区 =====
    trimBuffersIfNeeded();

    // 后续处理与16位版本相同
    int totalSamples = m_filteredBuffer.size();
    int availableForFFT = totalSamples - NFFT;
    if (availableForFFT < 0) {
        return result;
    }

    int totalSegments = availableForFFT / m_analysisHop + 1;
    int processedSegments =
        (m_processedSamples > 0)
            ? (m_processedSamples - NFFT) / m_analysisHop + 1
            : 0;

    const double dt = static_cast<double>(m_analysisHop) / m_fs;
    const int numValidFreqs = m_validFreqIndices.size();
    int appendedSegments = 0;

    // 处理FFT段
    for (int seg = processedSegments; seg < totalSegments; ++seg) {
        int startIdx = seg * m_analysisHop;
        if (startIdx + NFFT > totalSamples) {
            break;
        }

        const double *filteredData = m_filteredBuffer.constData();
        for (int j = 0; j < NFFT; ++j) {
            m_fftInput[j] = filteredData[startIdx + j] * m_hammingWindow[j];
        }

        fftw_execute(m_fftPlan);

        QVector<double> segSpectrum(numValidFreqs);
        bool hasValidValue = false;

        for (int i = 0; i < numValidFreqs; ++i) {
            int freqIdx = m_validFreqIndices[i];
            // if (freqIdx < 5) {
            //     segSpectrum[i] = -200.0;
            //     continue;
            // }

            double re = m_fftOutput[freqIdx][0];
            double im = m_fftOutput[freqIdx][1];
            // double mag = std::sqrt(re * re + im * im);
            // double scaledMag = (2.0 / NFFT) * mag;
            // double dbVal = (scaledMag > 1e-20) ? 20.0 * std::log10(scaledMag) : -200.0;
            double mag = std::sqrt(re * re + im * im);
            const double eps = 1e-12;
            double dbVal = 20.0 * std::log10(mag / NFFT + eps);
            segSpectrum[i] = dbVal;

            if (dbVal > -199.0) {
                hasValidValue = true;
            }
        }

        /* 若整列无有效值，仍然写入一列占位数据以保证时间轴连续 */

        m_spectrumCache.append(segSpectrum);
        m_timeVector.append(seg * dt);
        // 统一按 seg 位置记录时间，避免跳段/过滤导致时间轴变长或不连续
        m_totalSegments = qMax(m_totalSegments, seg + 1);
        appendedSegments++;
    }

    if (appendedSegments <= 0) {
        return result;
    }

    m_processedSamples = (processedSegments + appendedSegments - 1) * m_analysisHop + NFFT;

    // 构建结果并进行归一化处理
    int startTimeIdx = m_spectrumCache.size() - appendedSegments;
    if (startTimeIdx < 0) startTimeIdx = 0;

    QVector<double> newTimeVector;
    QVector<double> flatSpectrum;
    flatSpectrum.reserve(numValidFreqs * appendedSegments);

    for (int t = startTimeIdx; t < m_spectrumCache.size(); ++t) {
        newTimeVector.append(m_timeVector[t]);
    }

    // 收集所有原始dB值
    for (int f = 0; f < numValidFreqs; ++f) {
        for (int t = startTimeIdx; t < m_spectrumCache.size(); ++t) {
            flatSpectrum.append(m_spectrumCache[t][f]);
        }
    }

    // 已删除：FFT后的dB值打印（影响性能）

    // // 归一化处理（映射到40~100范围）
    // // 1. 找到最小值和最大值
    // double minDB = flatSpectrum[0];
    // double maxDB = flatSpectrum[0];
    // for (int i = 1; i < flatSpectrum.size(); ++i) {
    //     if (flatSpectrum[i] < minDB) minDB = flatSpectrum[i];
    //     if (flatSpectrum[i] > maxDB) maxDB = flatSpectrum[i];
    // }

    // // 2. 归一化到 0~1
    // double range = maxDB - minDB;
    // if (range > 1e-10) {  // 避免除以0
    //     for (int i = 0; i < flatSpectrum.size(); ++i) {
    //         flatSpectrum[i] = (flatSpectrum[i] - minDB) / range;
    //     }
    // }

    // // 3. 映射到 40~100（dB范围）
    // const double NORM_MIN = 40;
    // const double NORM_MAX = 100.0;
    // for (int i = 0; i < flatSpectrum.size(); ++i) {
    //     flatSpectrum[i] = NORM_MIN + flatSpectrum[i] * (NORM_MAX - NORM_MIN);
    // }

    // // 打印归一化后的前10个dB值
    // if (fftPrintCount < 1 && flatSpectrum.size() > 0) {
    //     qDebug() << QString("\n========== 归一化后前10个dB值（40~100范围）==========");
    //     qDebug() << QString("原始范围: min=%1 dB, max=%2 dB").arg(minDB, 0, 'f', 2).arg(maxDB, 0, 'f', 2);
    //     for (int i = 0; i < qMin(10, flatSpectrum.size()); ++i) {
    //         qDebug() << QString("  [%1] 归一化dB = %2").arg(i).arg(flatSpectrum[i], 0, 'f', 2);
    //     }
    //     fftPrintCount++;
    // }

    result.time = newTimeVector;
    result.freq = m_freqVector;
    result.spectrum = flatSpectrum;
    result.freqCount = numValidFreqs;
    result.timeCount = appendedSegments;
    result.isValid = true;

    if (m_baseTimestampSet) {
        result.timestamp = m_baseTimestamp.addMSecs(static_cast<qint64>((m_timeVector.isEmpty()?0.0:m_timeVector.last()) * 1000.0));
    } else {
        result.timestamp = timestamp.isValid() ? timestamp : QDateTime::currentDateTime();
    }

    return result;
}