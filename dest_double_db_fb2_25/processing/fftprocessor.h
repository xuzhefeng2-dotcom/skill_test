#ifndef FFTPROCESSOR_H
#define FFTPROCESSOR_H

#include <QObject>
#include <QVector>
#include <QMetaType>
#include <QDateTime>
#include <fftw3.h>

// 单列频谱结果（用于同步绘制）
struct SingleColumnResult {
    QVector<double> freq;       // 频率向量 (kHz)
    QVector<double> spectrum;   // 单列频谱数据 (dB)
    double time;                // 时间点 (秒)
    int freqCount;              // 频率点数
    bool isValid;               // 是否有效

    SingleColumnResult() : time(0), freqCount(0), isValid(false) {}
};
Q_DECLARE_METATYPE(SingleColumnResult)

// 频谱处理结果结构体
struct SpectrumResult {
    QVector<double> time;       // 时间向量 (秒)
    QVector<double> freq;       // 频率向量 (kHz)
    QVector<double> spectrum;   // 扁平化频谱数据 (dB)
    int freqCount;              // 频率点数
    int timeCount;              // 时间点数
    QDateTime timestamp;        // 数据时间戳（对应最后一个时间点的时间）
    bool isValid;               // 是否有效
};

/**
 * @brief FFT处理器（优化版）
 *
 * 增量处理模式：使用单向滤波+滤波器状态保持连续性
 */
class FFTProcessor : public QObject
{
    Q_OBJECT
public:
    explicit FFTProcessor(QObject *parent = nullptr);
    ~FFTProcessor();

    // 采样率设置（Hz），默认 4MHz
    void setSampleRate(double fsHz);
    double sampleRate() const { return m_fs; }

    // 暴露步进大小，便于按采样率计算时间轴
    static constexpr int hopSize() { return HOP_SIZE; }


    // 文件回放/降分辨率显示用的分析步进（单位：样本点）。默认等于 hopSize()
    void setAnalysisHop(int hop);
    int analysisHop() const { return m_analysisHop; }

    /**
     * @brief 追加数据并增量处理（支持16位和24位数据）
     * @param rawData 新增的原始数据（16位或24位）
     * @param timestamp 数据时间戳（可选，默认使用当前时间）
     * @return 处理结果
     */
    SpectrumResult appendAndProcess(const QVector<quint16> &rawData, const QDateTime &timestamp = QDateTime());
    SpectrumResult appendAndProcess(const QVector<quint32> &rawData24, const QDateTime &timestamp = QDateTime());

    /**
     * @brief 清空缓冲区
     */
    Q_INVOKABLE void clearBuffer();

    /**
     * @brief 获取当前缓冲区大小
     */
    int bufferSize() const { return m_filteredBuffer.size(); }
    double maxValue() const { return m_maxValue; }
private:
int m_analysisHop = HOP_SIZE;
    double m_fs = 4000000.0;  // 采样率 Hz（默认4MHz，可在菜单切换）
    // 滤波器相关函数
    double besselI0(double x);
    QVector<double> kaiser(int n, double beta);
    void kaiserord(const QVector<double> &fcuts, const QVector<double> &mags,
                   const QVector<double> &devs, double fs,
                   int &n, QVector<double> &Wn, double &beta);
    QVector<double> fir1(int n, const QVector<double> &Wn,
                         const QString &ftype, const QVector<double> &window);

    // 单向滤波（带状态，支持连续处理）
    void filterIncremental(const double *input, int inputLen);

    // FFT卷积加速滤波（Overlap-Add方法）
    void filterFFTConvolution(const double *input, int inputLen);

    // 初始化函数
    void initializeFilter();
    void initializeFFT();
    void initializeFilterFFT();  // 初始化滤波器的FFT卷积
    void cleanupFFT();
    void cleanupFilterFFT();  // 清理滤波器FFT资源

    // 缓冲区管理（防止UDP流式模式下的内存泄漏）
    void trimBuffersIfNeeded();

    // 参数常量
    static constexpr int NFFT = 2048;                   // FFT点数
    static constexpr int OVERLAP = 1024;                // 重叠点数 (50%)
    static constexpr int HOP_SIZE = NFFT - OVERLAP;     // 步进大小 (1024)
    static constexpr double HALF_MAX = 65535.0 / 2.0;   // ADC中间值（16位）
    static constexpr double HALF_MAX_24 = 8388608.0;    // ADC中间值（24位，2^23）
    static constexpr int MAX_TIME_POINTS = 1000;        // 最大时间点数（10秒数据）
    static constexpr double FIXED_MAX_DB = 60.0;        // 固定最大值 60dB

    // UDP流式模式下的缓冲区限制（防止内存泄漏）
    static constexpr int MAX_FILTERED_BUFFER_SAMPLES = 4000000 * 30;  // 保留30秒原始数据
    static constexpr int MAX_SPECTRUM_CACHE_SEGMENTS = 15000;         // 保留约30秒FFT结果

    // 滤波器系数
    QVector<double> m_filterCoeffs;
    bool m_filterInitialized;

    // 滤波器状态（用于连续滤波）
    QVector<double> m_filterState;
    int m_filterStateIndex;  // 环形缓冲区索引

    // FFT卷积加速相关（Overlap-Add方法）
    static constexpr int FFT_CONV_BLOCK_SIZE = 8192;  // 块大小（2的幂次）
    int m_fftConvSize;                    // FFT大小（>= BLOCK_SIZE + filterLen - 1）
    fftw_complex *m_filterFFT;            // 滤波器的FFT（预计算）
    double *m_convInput;                  // 卷积输入缓冲
    fftw_complex *m_convInputFFT;         // 输入FFT结果
    fftw_complex *m_convOutputFFT;        // 输出FFT结果
    double *m_convOutput;                 // 卷积输出缓冲
    fftw_plan m_convForwardPlan;          // 正向FFT计划
    fftw_plan m_convInversePlan;          // 逆向FFT计划
    QVector<double> m_overlapBuffer;      // Overlap-Add的重叠区域
    bool m_filterFFTInitialized;          // 滤波器FFT是否已初始化

    // FFTW预分配（复用，避免重复分配）
    double *m_fftInput;
    fftw_complex *m_fftOutput;
    fftw_plan m_fftPlan;
    bool m_fftInitialized;

    // Hamming窗（预计算）
    QVector<double> m_hammingWindow;

    // 已滤波的数据缓冲
    QVector<double> m_filteredBuffer;

    // 已处理的频谱结果缓存
    QVector<QVector<double>> m_spectrumCache;  // [timeIndex][freqIndex]
    QVector<double> m_timeVector;
    QVector<double> m_freqVector;
    QVector<int> m_validFreqIndices;

    int m_processedSamples;     // 已处理的采样点数（已做FFT的）
    double m_maxValue;          // 当前最大值（用于归一化）
    int m_totalSegments;        // 总段数（用于时间计算）

    // 时间戳基准（用于计算准确的时间戳）
    QDateTime m_baseTimestamp;  // 第一个数据包的时间戳
    bool m_baseTimestampSet;    // 是否已设置基准时间戳
};

#endif // FFTPROCESSOR_H