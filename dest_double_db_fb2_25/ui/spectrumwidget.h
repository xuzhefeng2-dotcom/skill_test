#ifndef SPECTRUMWIDGET_H
#define SPECTRUMWIDGET_H

#include <QWidget>
#include <QVector>
#include "qcustomplot.h"

/**
 * @brief 小型频谱图控件（可点击，带边框高亮）
 *
 * 用于显示单个通道的实时频谱，支持点击选择
 */
class SpectrumWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SpectrumWidget(const QString &channelName, const QString &orientation, QWidget *parent = nullptr);
    ~SpectrumWidget() = default;

    // 设置频谱数据
    void setSpectrumData(const QVector<double> &freq, const QVector<double> &spectrum);

    // 设置选中状态（高亮边框）
    void setSelected(bool selected);
    bool isSelected() const { return m_isSelected; }

    // 获取通道名称
    QString channelName() const { return m_channelName; }

    // 刷新显示
    void replot();

    // 设置采样率（动态调整频率范围）
    void setSampleRate(double fsHz);

    // 设置对数/线性坐标轴
    void setLogScale(bool isLog,double fsHz);

signals:
    void clicked(const QString &channelName);  // 点击信号
private:
    void setupPlot();
    void updateBorder();

    QString m_channelName;      // 通道名称（CH-A, CH-B, CH-C）
    QString m_orientation;      // 方向标识（N-S, E-W, T-D）
    bool m_isSelected;          // 是否被选中
    bool m_isLogScale;          // 是否为对数坐标轴
    double m_freqMaxKHz;        // 最大频率（kHz）

    QCustomPlot *m_plot;
    QCPGraph *m_graph;
    QPushButton *m_titleButton;  // 改为按钮
};

#endif // SPECTRUMWIDGET_H
