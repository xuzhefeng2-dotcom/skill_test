#include "spectrumwidget.h"
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QLabel>

SpectrumWidget::SpectrumWidget(const QString &channelName, const QString &orientation, QWidget *parent)
    : QWidget(parent)
    , m_channelName(channelName)
    , m_orientation(orientation)
    , m_isSelected(false)
    , m_isLogScale(false)
    , m_freqMaxKHz(2000.0)  // 默认4MHz采样率
{
    setMinimumSize(300, 400);  // 增加高度从200到300
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);  // 设置较小的间距

    // 标题按钮（居中显示，可点击）
    m_titleButton = new QPushButton(QString("[%1] %2").arg(channelName).arg(orientation));
    m_titleButton->setFixedHeight(30);  // 固定高度，避免占用太多空间
    m_titleButton->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_titleButton->setStyleSheet(R"(
        QPushButton {
            background-color: #4A9DD1;
            color: white;
            border: 1px solid #555;
            border-radius: 4px;
            padding: 6px;
            font-size: 10pt;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #6DBDF2;
        }
        QPushButton:pressed {
            background-color: #5DADE2;
        }
    )");
    m_titleButton->setCursor(Qt::PointingHandCursor);
    connect(m_titleButton, &QPushButton::clicked, this, [this]() {
        emit clicked(m_channelName);
    });

    layout->addWidget(m_titleButton);

    // 频谱图
    setupPlot();
    layout->addWidget(m_plot, 1);  // 设置stretch为1，让图表占据剩余空间

    // 初始边框样式
    updateBorder();
    //connect(m_plot, &QCustomPlot::plottableClick,this, &SpectrumWidget::onPlottableClicked);
}

void SpectrumWidget::setupPlot()
{
    m_plot = new QCustomPlot();
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->setBackground(Qt::black);

    // ✅ 性能优化：启用OpenGL硬件加速
    m_plot->setOpenGl(true);
// qDebug() << "[SpectrumWidget] OpenGL加速已启用";

    // ✅ 性能优化：禁用抗锯齿（提升渲染速度）
    m_plot->setAntialiasedElements(QCP::aeNone);
    m_plot->setNotAntialiasedElements(QCP::aeAll);

    // 显示四条坐标轴
    m_plot->axisRect()->setupFullAxesBox(true);

    // 坐标轴样式
    m_plot->xAxis->setBasePen(QPen(Qt::white));
    m_plot->yAxis->setBasePen(QPen(Qt::white));
    m_plot->xAxis2->setBasePen(QPen(Qt::white));
    m_plot->yAxis2->setBasePen(QPen(Qt::white));
    m_plot->xAxis->setTickPen(QPen(Qt::white));
    m_plot->yAxis->setTickPen(QPen(Qt::white));
    m_plot->xAxis2->setTickPen(QPen(Qt::white));
    m_plot->yAxis2->setTickPen(QPen(Qt::white));
    m_plot->xAxis->setSubTickPen(QPen(Qt::transparent));
    m_plot->yAxis->setSubTickPen(QPen(Qt::transparent));
    m_plot->xAxis2->setSubTickPen(QPen(Qt::transparent));
    m_plot->yAxis2->setSubTickPen(QPen(Qt::transparent));
    m_plot->xAxis->setTickLabelColor(Qt::white);
    m_plot->yAxis->setTickLabelColor(Qt::white);
    m_plot->xAxis2->setTickLabelColor(Qt::white);
    m_plot->yAxis2->setTickLabelColor(Qt::white);
    m_plot->xAxis->setLabelColor(Qt::white);
    m_plot->yAxis->setLabelColor(Qt::white);

    // 设置刻度标签显示
    m_plot->xAxis->setTickLabels(true);
    m_plot->xAxis2->setTickLabels(false);
    m_plot->yAxis->setTickLabels(true);
    m_plot->yAxis2->setTickLabels(false);

    // 网格
    m_plot->xAxis->grid()->setVisible(true);
    m_plot->yAxis->grid()->setVisible(true);
    m_plot->xAxis->grid()->setPen(QPen(QColor(50, 50, 50), 0.5));
    m_plot->yAxis->grid()->setPen(QPen(QColor(50, 50, 50), 0.5));

    // 坐标轴标签（X轴=dB，Y轴=频率）
    m_plot->xAxis->setLabel("dB");
    m_plot->yAxis->setLabel("Frequency (kHz)");

    // X轴范围（dB值）
    m_plot->xAxis->setRange(40, 110);

    // Y轴范围（频率kHz）
    m_plot->yAxis->setRange(0, m_freqMaxKHz);

    // 创建图形
    m_graph = m_plot->addGraph(m_plot->xAxis, m_plot->yAxis);
    m_graph->setPen(QPen(QColor(0, 255, 255), 1.5));  // 青色线条
    m_graph->setLineStyle(QCPGraph::lsLine);
    connect(m_plot, &QCustomPlot::mousePress, this, [this](QMouseEvent*) {
        emit clicked(m_channelName);
    });
}

void SpectrumWidget::setSpectrumData(const QVector<double> &freq, const QVector<double> &spectrum)
{
    if (freq.size() != spectrum.size() || freq.isEmpty()) {
        return;
    }

    // 排序数据（按频率升序）
    QVector<QPair<double, double>> pairs;
    for (int i = 0; i < freq.size(); ++i) {
        pairs.append(qMakePair(freq[i], spectrum[i]));
    }
    std::sort(pairs.begin(), pairs.end(), [](const QPair<double,double>& a, const QPair<double,double>& b) {
        return a.first < b.first;
    });

    QVector<double> sortedFreq, sortedSpectrum;
    for (const auto& p : pairs) {
        sortedFreq.append(p.first);
        sortedSpectrum.append(p.second);
    }

    // X轴=dB值，Y轴=频率 (交换数据绑定)
    m_graph->setData(sortedSpectrum, sortedFreq);
}

void SpectrumWidget::setSelected(bool selected)
{
    if (m_isSelected != selected) {
        m_isSelected = selected;
        updateBorder();
    }
}

void SpectrumWidget::updateBorder()
{
    if (m_isSelected) {
        // 选中状态：高亮边框（青色，3px）
        setStyleSheet("SpectrumWidget { border: 3px solid #00ffff; background-color: #1a1a1a; }");
    } else {
        // 未选中状态：普通边框（灰色，1px）
        setStyleSheet("SpectrumWidget { border: 1px solid #3a3a3a; background-color: #1a1a1a; }");
    }
}

void SpectrumWidget::replot()
{
    m_plot->replot();
}

void SpectrumWidget::setSampleRate(double fsHz)
{
    // ✅ 250kHz采样率时，频率范围限制为0~100 kHz
    if (fsHz == 250000.0) {
        m_freqMaxKHz = 100.0;
    } else {
        m_freqMaxKHz = std::max(0.1, fsHz / 2000.0);  // Nyquist频率（kHz）
    }

    m_plot->yAxis->setRange(0, m_freqMaxKHz);  // Y轴=频率
    m_plot->replot();
}

void SpectrumWidget::setLogScale(bool isLog,double fsHz)
{
    if (m_isLogScale == isLog) {
        return;  // 状态未改变，无需更新
    }

    m_isLogScale = isLog;

    if (isLog) {
        // 对数坐标轴：Y轴（频率）显示 0.1, 1, 10, 100, 1000 kHz
        QSharedPointer<QCPAxisTickerLog> logTicker(new QCPAxisTickerLog);
        logTicker->setLogBase(10);
        logTicker->setSubTickCount(0);
        m_plot->yAxis->setTicker(logTicker);
        m_plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
        if (fsHz == 250000.0) {
            m_plot->yAxis->setRange(0.1, 100);
        } else if (fsHz == 4000000.0) {
            m_plot->yAxis->setRange(0.1, 1000);  // Nyquist频率（kHz）
        }
    } else {
        // 线性坐标轴：Y轴（频率）显示 0 ~ 采样率/2
        QSharedPointer<QCPAxisTicker> linearTicker(new QCPAxisTicker);
        linearTicker->setTickCount(8);
        m_plot->yAxis->setTicker(linearTicker);
        m_plot->yAxis->setScaleType(QCPAxis::stLinear);
        m_plot->yAxis->setRange(0, m_freqMaxKHz);

        // ✅ 在轴级别禁用子刻度（关键优化！）
        m_plot->yAxis->setSubTickLength(0);
        m_plot->xAxis->setSubTickLength(0);
    }

    // ✅ 明确禁用子网格线（提升性能）
    m_plot->yAxis->grid()->setSubGridVisible(false);
    m_plot->xAxis->grid()->setSubGridVisible(false);

    m_plot->replot();
}
