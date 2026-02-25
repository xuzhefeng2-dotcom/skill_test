#ifndef SPECTROGRAMWIDGET_H
#define SPECTROGRAMWIDGET_H

#include <QWidget>
#include <QVector>
#include <QDateTime>
#include <QSharedPointer>
#include "qcustomplot.h"
#include "processing/timestamploader.h"  // 引入SecondMark定义
#include "vlfsensorwidget.h"  // 引入AbsoluteTimeAxisTicker定义

/**
 * @brief 大型瀑布图控件
 *
 * 用于显示单个通道的时频瀑布图，支持线性/对数坐标切换
 */
class SpectrogramWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrogramWidget(QWidget *parent = nullptr);
    ~SpectrogramWidget() = default;

    // 设置当前显示的通道
    void setCurrentChannel(const QString &channelName, const QString &orientation);

    // 初始化colorMap大小
    void initializeColorMapSize(int timeSlots, int freqCount);

    // 设置colorMap列数据（原始指针版本，高性能）
    void setColorMapColumnRaw(int timeSlot, const double* spectrum, int freqCount);

    // 清空colorMap
    void clearColorMap();

    // 刷新显示
    void replot();

    // 设置采样率（动态调整频率范围）
    void setSampleRate(double fsHz);

    // 设置显示时间范围
    void setDisplayTimeRange(double seconds);

    // 更新时间轴
    void updateTimeAxis(const QDateTime &timestamp);

    // 切换到绝对时间模式（本地文件模式）
    void setAbsoluteTimeMode(const QVector<SecondMark> &marks);

    // 线性/对数坐标切换
    void toggleScale(double fsHz);
    bool isLogScale() const { return m_isLogScale; }
protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
signals:
    void scaleChanged(bool isLogScale);  // 坐标切换信号
    void closeRequested();  // 关闭按钮点击信号

private:

    void setupPlot();
    void applyLinearScale();
    void applyLogScale(double fsHz);
    void buildLogFreqIndex();
    void redrawColorMapFromCache();

    QWidget *m_titleBarWidget = nullptr;
    bool m_dragging = false;
    QPoint m_dragOffset;
    
    QString m_channelName;      // 当前通道名称
    QString m_orientation;      // 当前方向标识
    bool m_isLogScale;          // 是否对数坐标
    double m_freqMinKHz;        // 最小频率（kHz）
    double m_freqMaxKHz;        // 最大频率（kHz）
    double m_displayTimeRange;  // 显示时间范围（秒）

    int m_colorMapTimeSlots;    // 时间槽数量
    int m_colorMapFreqCount;    // 频率点数量

    QCustomPlot *m_plot;
    QCPColorMap *m_colorMap;
    QCPColorScale *m_colorScale;
    QLabel *m_titleLabel;
    QPushButton *m_scaleButton;

    // 对数坐标索引映射
    QVector<int> m_logFreqIndex;

    // 原始数据缓存（用于线性/对数切换）
    QVector<QVector<double>> m_rawSpectrumCache;

    // 绝对时间轴刻度器（本地文件模式）
    QSharedPointer<AbsoluteTimeAxisTicker> m_absoluteTimeTicker;
};

#endif // SPECTROGRAMWIDGET_H
