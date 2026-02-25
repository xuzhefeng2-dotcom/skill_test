#ifndef VLFSENSORWIDGET_H
#define VLFSENSORWIDGET_H

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDateTime>
#include <QPushButton>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsProxyWidget>
#include "qcustomplot.h"
#include "processing/timestamploader.h"  // 包含SecondMark定义

// 绝对时间轴刻度器（用于显示完整的年月日时分秒）
class AbsoluteTimeAxisTicker : public QCPAxisTicker
{
public:
    AbsoluteTimeAxisTicker(const QVector<SecondMark> &marks)
        : m_marks(marks) {}

    void setSecondMarks(const QVector<SecondMark> &marks) {
        m_marks = marks;
    }

protected:
    QString getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision) override;

    // ✅ 重写getTicks方法，强制每2秒一个刻度
    QVector<double> createTickVector(double tickStep, const QCPRange &range) override;

private:
    QVector<SecondMark> m_marks;
};

// 网络模式时间戳刻度器（基于起始时间戳动态显示）
class NetworkTimeAxisTicker : public QCPAxisTicker
{
public:
    NetworkTimeAxisTicker(const QDateTime &startTime, double displayRange)
        : m_startTime(startTime), m_displayRange(displayRange) {}

    void setStartTime(const QDateTime &startTime) {
        m_startTime = startTime;
    }

protected:
    QString getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision) override;
    QVector<double> createTickVector(double tickStep, const QCPRange &range) override;

private:
    QDateTime m_startTime;
    double m_displayRange;
};

/**
 * @brief VLF传感器显示组件(增强版)
 *
 * 包含:
 * - 左侧: 时频瀑布图 (spectrogram)
 * - 右侧: 实时频谱图 (spectrum plot)
 *
 * 刷新由外部统一控制
 */
class VLFSensorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VLFSensorWidget(const QString &channelName, const QString &orientation,
                             QWidget *parent = nullptr);

    // 更新时间轴（使用指定的时间戳，东八区本地时间）
    void updateTimeAxis(const QDateTime &timestamp);

    // 获取colorMap用于外部更新
    QCPColorMap* getColorMap() { return m_colorMap; }

    // 初始化colorMap大小（时间槽数，频率点数）
    void initializeColorMapSize(int timeSlots, int freqCount);
    // 动态设置时间轴范围（秒），用于本地文件按时长铺满X轴
    void setDisplayTimeRange(double seconds);

    
    void setSampleRate(double fsHz);
    double freqMinKHz() const { return m_freqMinKHz; }
    double freqMaxKHz() const { return m_freqMaxKHz; }
// 更新瀑布图某一列数据（外部调用，自动处理线性/对数映射）
    void setColorMapColumn(int timeSlot, const QVector<double> &freqValues, const QVector<double> &spectrum);
    // 更轻量的接口：直接提供频谱数组（长度=freqCount），避免主线程构造QVector
    void setColorMapColumnRaw(int timeSlot, const double* spectrum, int freqCount);

    // 更新右侧实时频谱图数据（只更新数据，不刷新）
    void setSpectrumData(const QVector<double> &freq, const QVector<double> &spectrum);

    // 刷新瀑布图
    void replotSpectrogram();

    // 刷新频谱图
    void replotSpectrum();

    // 是否为对数模式
    bool isLogScale() const { return m_isLogScale; }

    // 设置是否自动清空图表
    void setAutoClearPlot(bool enable) { m_autoClearPlot = enable; }

    // 清空colorMap数据
    void clearColorMap();

    // 时间轴模式切换接口
    void setAbsoluteTimeMode(const QVector<SecondMark> &marks);  // 切换到绝对时间模式
    void setRelativeTimeMode();  // 切换到相对时间模式（默认）

signals:
    void closeRequested();

private slots:
    void onScaleButtonClicked();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void setupSpectrogramPlot();
    void setupSpectrumPlot();
    void applyLinearScale();
    void applyLogScale();
    void buildLogFreqIndex();  // 构建对数频率索引映射表
    void redrawColorMapFromCache();  // 从缓存重绘瀑布图（用于模式切换）

    QString m_channelName;
    QString m_orientation;

    // 标题栏
    QLabel *m_titleLabel;
    QPushButton *m_closeButton;
    QPushButton *m_scaleButton;  // 线性/对数切换按钮
    bool m_isLogScale;           // 是否为对数模式

    // 左侧: 时频瀑布图
    QCustomPlot *m_spectrogramPlot;
    QCPColorMap *m_colorMap;
    QCPColorScale *m_colorScale;
    // QDateTime m_bindingStartTime;
    // QSharedPointer<TimeAxisTicker> m_timeTicker;
    QSharedPointer<AbsoluteTimeAxisTicker> m_absoluteTimeTicker;  // 绝对时间刻度器

    // 右侧: 实时频谱图
    QCustomPlot *m_spectrumPlot;
    QCPGraph *m_spectrumGraph;
    QVector<QCPGraph*> m_spectrumGraphs;  // 多个Graph用于渐变色显示
    QCPColorGradient m_colorGradient;     // 颜色梯度（与瀑布图一致）
    bool m_useColoredSpectrum;            // 是否使用渐变色频谱
    static constexpr int COLOR_SEGMENTS = 130;  // 颜色分段数（增加以获得更平滑的渐变）

    // 频谱数据缓存（用于切换时重新计算）
    QVector<double> m_lastFreq;
    QVector<double> m_lastSpectrum;

    // colorMap尺寸
    int m_colorMapTimeSlots;
    int m_colorMapFreqCount;

    // 对数模式频率索引映射表
    QVector<int> m_logFreqIndex;

    // 瀑布图原始数据缓存（用于线性/对数切换时重绘）
    // m_rawSpectrumCache[timeSlot] = QVector<double>(频率点数据)
    QVector<QVector<double>> m_rawSpectrumCache;

    // 初始对齐标志
    bool m_initialAlignmentDone;

    // 自动清空图表标志
    bool m_autoClearPlot;

    // 时间轴显示范围（秒），0表示未设置，会在运行时动态设置
    double m_displayTimeRange = 0.0;

    // 原始FFT数据的频率范围（kHz）
    // 这些值会在setSampleRate()中根据采样率动态调整
    double m_freqMinKHz = 0.3;      // 最小频率，避免DC分量
    double m_freqMaxKHz = 2000.0;   // 最大频率（Nyquist频率，对于4MHz采样率）

    // 瀑布图时间槽总数（colorMap的X轴列数）
    // 这是显示分辨率，与采样率无关，可以根据需要调整
    static constexpr int TOTAL_SLOTS = 1600;
};

#endif // VLFSENSORWIDGET_H