#include "spectrogramwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QtMath>
#include <cmath>
#include <algorithm>
#include <QMouseEvent>
#include <QEvent>
SpectrogramWidget::SpectrogramWidget(QWidget *parent)
    : QWidget(parent)
    , m_channelName("CH-A")
    , m_orientation("N-S")
    , m_isLogScale(false)
    , m_freqMinKHz(0.1)
    , m_freqMaxKHz(2000.0)
    , m_displayTimeRange(20.0)
    , m_colorMapTimeSlots(2032)
    , m_colorMapFreqCount(2048)
    {
        setMinimumSize(800, 400);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    
        QVBoxLayout *mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(5, 5, 5, 5);
        mainLayout->setSpacing(2);
    
        // ✅ 标题栏（做成一个 QWidget，才能接鼠标事件）
        m_titleBarWidget = new QWidget(this);
        m_titleBarWidget->setFixedHeight(32);
        m_titleBarWidget->setStyleSheet("background: transparent;");
        m_titleBarWidget->installEventFilter(this);
    
        QHBoxLayout *titleLayout = new QHBoxLayout(m_titleBarWidget);
        titleLayout->setContentsMargins(0, 0, 0, 0);
        titleLayout->setSpacing(8);
    
        m_titleLabel = new QLabel(QString("[%1] VLF Magnetic (%2) - Spectrogram")
                                  .arg(m_channelName).arg(m_orientation), m_titleBarWidget);
        m_titleLabel->setStyleSheet("color: white; font-size: 12pt; font-weight: bold;");
        m_titleLabel->installEventFilter(this); // 标题文字区域也能拖
    
        // 线性/对数切换按钮
        m_scaleButton = new QPushButton("对数坐标轴", m_titleBarWidget);
        m_scaleButton->setFixedSize(90, 24);
        m_scaleButton->setStyleSheet(R"(
            QPushButton {
                background-color: #5DADE2;
                color: white;
                border: none;
                border-radius: 4px;
                font-size: 11pt;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #6DBDF2; }
            QPushButton:pressed { background-color: #4A9DD1; }
        )");
        connect(m_scaleButton, &QPushButton::clicked, this, &SpectrogramWidget::toggleScale);
    
        // 关闭按钮
        QPushButton *closeButton = new QPushButton("×", m_titleBarWidget);
        closeButton->setFixedSize(24, 24);
        closeButton->setStyleSheet(R"(
            QPushButton {
                background-color: transparent;
                color: white;
                border: 1px solid #555;
                border-radius: 3px;
                font-size: 16pt;
                font-weight: bold;
            }
            QPushButton:hover { background-color: #d9534f; border: 1px solid #d9534f; }
            QPushButton:pressed { background-color: #c9302c; }
        )");
        connect(closeButton, &QPushButton::clicked, this, &SpectrogramWidget::closeRequested);
    
        titleLayout->addWidget(m_titleLabel);
        titleLayout->addSpacing(20);
        titleLayout->addWidget(m_scaleButton);
        titleLayout->addStretch();
        titleLayout->addWidget(closeButton);
    
        mainLayout->addWidget(m_titleBarWidget);
    
        // 瀑布图
        setupPlot();
        mainLayout->addWidget(m_plot);
    }
    

void SpectrogramWidget::setupPlot()
{
    m_plot = new QCustomPlot();
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->setBackground(Qt::black);

    // 坐标轴样式
    m_plot->xAxis->setBasePen(QPen(Qt::white));
    m_plot->yAxis->setBasePen(QPen(Qt::white));
    m_plot->xAxis->setTickPen(QPen(Qt::white));
    m_plot->yAxis->setTickPen(QPen(Qt::white));
    m_plot->xAxis->setSubTickPen(QPen(Qt::transparent));
    m_plot->yAxis->setSubTickPen(QPen(Qt::transparent));
    m_plot->xAxis->setTickLabelColor(Qt::white);
    m_plot->yAxis->setTickLabelColor(Qt::white);
    m_plot->xAxis->setLabelColor(Qt::white);
    m_plot->yAxis->setLabelColor(Qt::white);

    // 网格
    m_plot->xAxis->grid()->setVisible(false);
    m_plot->yAxis->grid()->setVisible(false);

    // 颜色图
    m_colorMap = new QCPColorMap(m_plot->xAxis, m_plot->yAxis);

    // 颜色刻度条
    m_colorScale = new QCPColorScale(m_plot);
    m_plot->plotLayout()->addElement(0, 1, m_colorScale);
    m_colorScale->setType(QCPAxis::atRight);
    m_colorMap->setColorScale(m_colorScale);

    // 使用边距组确保色条和瀑布图的绘图区域高度对齐
    QCPMarginGroup *marginGroup = new QCPMarginGroup(m_plot);
    m_plot->axisRect()->setMarginGroup(QCP::msTop | QCP::msBottom, marginGroup);
    m_colorScale->setMarginGroup(QCP::msTop | QCP::msBottom, marginGroup);

    // 坐标轴标签
    m_plot->xAxis->setLabel("Time (s)");
    m_plot->yAxis->setLabel("Frequency (kHz)");

    // 颜色刻度条样式
    m_colorScale->axis()->setBasePen(QPen(Qt::white));
    m_colorScale->axis()->setTickPen(QPen(Qt::white));
    m_colorScale->axis()->setSubTickPen(QPen(Qt::transparent));
    m_colorScale->axis()->setTickLabelColor(Qt::white);
    m_colorScale->axis()->setLabelColor(Qt::white);

    // Y轴:线性刻度
    QSharedPointer<QCPAxisTicker> linearTicker(new QCPAxisTicker);
    linearTicker->setTickCount(6);
    m_plot->yAxis->setTicker(linearTicker);
    m_plot->yAxis->setScaleType(QCPAxis::stLinear);
    m_plot->yAxis->setRange(m_freqMinKHz, m_freqMaxKHz);

    // X轴：普通数值时间轴
    QSharedPointer<QCPAxisTicker> xTicker(new QCPAxisTicker);
    m_plot->xAxis->setTicker(xTicker);
    m_plot->xAxis->setLabel("Time (s)");
    m_plot->xAxis->setNumberFormat("f");
    m_plot->xAxis->setNumberPrecision(2);
    m_plot->xAxis->setRange(0, m_displayTimeRange);

    // 初始化colorMap大小
    m_colorMap->data()->setSize(m_colorMapTimeSlots, m_colorMapFreqCount);
    m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange), QCPRange(m_freqMinKHz, m_freqMaxKHz));

    // 初始化所有单元格为最小值（显示蓝色）
    for (int x = 0; x < m_colorMapTimeSlots; ++x) {
        for (int y = 0; y < m_colorMapFreqCount; ++y) {
            m_colorMap->data()->setCell(x, y, 40);
        }
    }

    // 数据范围
    m_colorMap->setDataRange(QCPRange(40, 110));
    m_colorScale->setDataRange(QCPRange(40, 110));

    // Jet色图
    m_colorMap->setGradient(QCPColorGradient::gpJet);
    m_colorMap->setInterpolate(false);
}

void SpectrogramWidget::setCurrentChannel(const QString &channelName, const QString &orientation)
{
    m_channelName = channelName;
    m_orientation = orientation;
    m_titleLabel->setText(QString("[%1] VLF Magnetic (%2) - Spectrogram").arg(channelName).arg(orientation));
}

void SpectrogramWidget::initializeColorMapSize(int timeSlots, int freqCount)
{
    m_colorMapTimeSlots = timeSlots;
    m_colorMapFreqCount = freqCount;

    // 重新设置colorMap的大小
    m_colorMap->data()->setSize(timeSlots, freqCount);
    m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange), QCPRange(m_freqMinKHz, m_freqMaxKHz));

    // 清空数据（设置为最小值）
    for (int x = 0; x < timeSlots; ++x) {
        for (int y = 0; y < freqCount; ++y) {
            m_colorMap->data()->setCell(x, y, 40);
        }
    }

    // 初始化原始数据缓存
    m_rawSpectrumCache.resize(timeSlots);
    for (int x = 0; x < timeSlots; ++x) {
        m_rawSpectrumCache[x].resize(freqCount);
        m_rawSpectrumCache[x].fill(40.0);
    }

    // 如果是对数模式，重新构建索引映射表
    if (m_isLogScale) {
        buildLogFreqIndex();
    }

    m_plot->replot();
}

void SpectrogramWidget::setColorMapColumnRaw(int timeSlot, const double* spectrum, int freqCount)
{
    if (timeSlot < 0 || timeSlot >= m_colorMap->data()->keySize()) {
        return;
    }
    if (!spectrum || freqCount <= 0) {
        return;
    }

    int ny = m_colorMapFreqCount;
    int dataSize = qMin(freqCount, ny);

    // 缓存原始数据
    if (timeSlot >= 0 && timeSlot < m_rawSpectrumCache.size()) {
        if (m_rawSpectrumCache[timeSlot].size() != dataSize) {
            m_rawSpectrumCache[timeSlot].resize(dataSize);
        }
        for (int f = 0; f < dataSize; ++f) {
            m_rawSpectrumCache[timeSlot][f] = spectrum[f];
        }
    }

    if (m_isLogScale) {
        // 对数模式：使用预计算的索引映射
        if (m_logFreqIndex.size() != ny) {
            buildLogFreqIndex();
        }

        for (int y = 0; y < ny; ++y) {
            int freqIndex = m_logFreqIndex[y];
            if (freqIndex < dataSize) {
                m_colorMap->data()->setCell(timeSlot, y, spectrum[freqIndex]);
            }
        }
    } else {
        // 线性模式：直接设置
        for (int f = 0; f < dataSize; ++f) {
            m_colorMap->data()->setCell(timeSlot, f, spectrum[f]);
        }
    }
}

void SpectrogramWidget::clearColorMap()
{
    if (!m_colorMap) {
        return;
    }

    // 清空colorMap的所有数据
    for (int x = 0; x < m_colorMapTimeSlots; ++x) {
        for (int y = 0; y < m_colorMapFreqCount; ++y) {
            m_colorMap->data()->setCell(x, y, 40);
        }
    }

    // 清空原始数据缓存
    m_rawSpectrumCache.clear();
    m_rawSpectrumCache.resize(m_colorMapTimeSlots);
}

void SpectrogramWidget::replot()
{
    m_plot->replot();
}

void SpectrogramWidget::setSampleRate(double fsHz)
{
    m_freqMinKHz = 0.1;

    // ✅ 250kHz采样率时，频率范围限制为0~100 kHz
    if (fsHz == 250000.0) {
        m_freqMaxKHz = 100.0;
    } else {
        m_freqMaxKHz = std::max(m_freqMinKHz, fsHz / 2000.0);
    }

    if (m_isLogScale) {
        applyLogScale(fsHz);
    } else {
        applyLinearScale();
    }

    m_plot->replot();
}

void SpectrogramWidget::setDisplayTimeRange(double seconds)
{
    if (seconds <= 0) return;

    m_displayTimeRange = seconds;

    if (m_plot) {
        m_plot->xAxis->setRange(0, seconds);
    }

    if (m_colorMap) {
        m_colorMap->data()->setKeyRange(QCPRange(0, seconds));
    }
}

void SpectrogramWidget::updateTimeAxis(const QDateTime &timestamp)
{
    if (!timestamp.isValid()) return;

    // 使用NetworkTimeAxisTicker显示真实时间戳
    if (m_plot && m_plot->xAxis) {
        // 创建NetworkTimeAxisTicker并设置为X轴刻度器
        // 注意：这里需要包含相应的头文件
        // 暂时使用简单的标签更新
        m_plot->xAxis->setLabel(QString("Time (s) - Start: %1").arg(timestamp.toString("HH:mm:ss")));

// qDebug() << "[SpectrogramWidget] 更新时间轴，起始时间戳:" << timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz")
    //                  << "显示范围:" << m_displayTimeRange << "秒";

        // 强制重绘
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void SpectrogramWidget::toggleScale(double fsHz)
{
    m_isLogScale = !m_isLogScale;

    if (m_isLogScale) {
        m_scaleButton->setText("线性坐标轴");
        m_scaleButton->setStyleSheet(R"(
            QPushButton {
                background-color: #5DADE2;
                color: white;
                border: none;
                border-radius: 4px;
                font-size: 11pt;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: #6DBDF2;
            }
            QPushButton:pressed {
                background-color: #4A9DD1;
            }
        )");
        buildLogFreqIndex();
        applyLogScale(fsHz);
    } else {
        m_scaleButton->setText("对数坐标轴");
        m_scaleButton->setStyleSheet(R"(
            QPushButton {
                background-color: #5DADE2;
                color: white;
                border: none;
                border-radius: 4px;
                font-size: 11pt;
                font-weight: bold;
            }
            QPushButton:hover {
                background-color: #6DBDF2;
            }
            QPushButton:pressed {
                background-color: #4A9DD1;
            }
        )");
        applyLinearScale();
    }

    redrawColorMapFromCache();
    m_plot->replot();
    emit scaleChanged(m_isLogScale);
}

void SpectrogramWidget::applyLinearScale()
{
    QSharedPointer<QCPAxisTicker> linearTicker(new QCPAxisTicker);
    linearTicker->setTickCount(6);
    m_plot->yAxis->setTicker(linearTicker);
    m_plot->yAxis->setScaleType(QCPAxis::stLinear);
    m_plot->yAxis->setRange(m_freqMinKHz, m_freqMaxKHz);

    m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange), QCPRange(m_freqMinKHz, m_freqMaxKHz));
}

void SpectrogramWidget::applyLogScale(double fsHz)
{
    QSharedPointer<QCPAxisTickerLog> logTicker(new QCPAxisTickerLog);
    logTicker->setLogBase(10);
    logTicker->setSubTickCount(0);
    m_plot->yAxis->setTicker(logTicker);
    m_plot->yAxis->setScaleType(QCPAxis::stLogarithmic);

    double displayMin = 1;
    double displayMax = 100;
    if(fsHz == 250000.0){
         displayMax = 100;
    }
    else if(fsHz == 4000000.0){
         displayMax = 1000;
    }
    m_plot->yAxis->setRange(displayMin, displayMax);

    m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange),
                                 QCPRange(displayMin, displayMax));
}

void SpectrogramWidget::buildLogFreqIndex()
{
    int ny = m_colorMapFreqCount;
    if (ny <= 0) return;

    double displayMin = 1.0;
    double nyquistKHz = m_freqMaxKHz;
    double displayMax = std::pow(10.0, std::floor(std::log10(nyquistKHz)));
    if (nyquistKHz > displayMax * 5.0) {
        displayMax *= 10.0;
    }
    displayMax = std::min(displayMax, nyquistKHz);

    double dataMin = m_freqMinKHz;
    double dataMax = m_freqMaxKHz;

    double logFreqMin = std::log10(displayMin);
    double logFreqMax = std::log10(displayMax);
    double denom = (ny - 1) > 0 ? (ny - 1) : 1;

    m_logFreqIndex.resize(ny);

    for (int y = 0; y < ny; ++y) {
        double logFreq = logFreqMin + y * (logFreqMax - logFreqMin) / denom;
        double freqVal = std::pow(10.0, logFreq);

        double ratio = (freqVal - dataMin) / (dataMax - dataMin);
        int freqIndex = static_cast<int>(ratio * (ny - 1));
        freqIndex = qBound(0, freqIndex, ny - 1);

        m_logFreqIndex[y] = freqIndex;
    }
}

void SpectrogramWidget::redrawColorMapFromCache()
{
    int nx = m_colorMapTimeSlots;
    int ny = m_colorMapFreqCount;

    if (m_rawSpectrumCache.size() != nx) {
        return;
    }

    if (m_isLogScale) {
        if (m_logFreqIndex.size() != ny) {
            buildLogFreqIndex();
        }

        for (int x = 0; x < nx; ++x) {
            const QVector<double> &spectrum = m_rawSpectrumCache[x];
            int dataSize = spectrum.size();
            for (int y = 0; y < ny; ++y) {
                int freqIndex = m_logFreqIndex[y];
                if (freqIndex < dataSize) {
                    m_colorMap->data()->setCell(x, y, spectrum[freqIndex]);
                }
            }
        }
    } else {
        for (int x = 0; x < nx; ++x) {
            const QVector<double> &spectrum = m_rawSpectrumCache[x];
            int dataSize = qMin(spectrum.size(), ny);
            for (int f = 0; f < dataSize; ++f) {
                m_colorMap->data()->setCell(x, f, spectrum[f]);
            }
        }
    }
}

void SpectrogramWidget::setAbsoluteTimeMode(const QVector<SecondMark> &marks)
{
    if (marks.isEmpty()) {
// qWarning() << "[SpectrogramWidget] 时间标记为空，无法切换到绝对时间模式";
        return;
    }

    // 创建绝对时间刻度器
    m_absoluteTimeTicker = QSharedPointer<AbsoluteTimeAxisTicker>::create(marks);
    m_plot->xAxis->setTicker(m_absoluteTimeTicker);

    // 设置X轴范围（0到总秒数）
    double maxSeconds = marks.last().relativeSeconds;
    m_plot->xAxis->setRange(0, maxSeconds);
    m_colorMap->data()->setKeyRange(QCPRange(0, maxSeconds));

    // 更新X轴标签
    m_plot->xAxis->setLabel("Time (Absolute)");

// qDebug() << "[SpectrogramWidget] 已切换到绝对时间模式，时间范围: 0 ~" << maxSeconds << "秒";

    m_plot->replot();
}
bool SpectrogramWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_titleBarWidget || obj == m_titleLabel) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *e = static_cast<QMouseEvent*>(event);
            if (e->button() == Qt::LeftButton) {
                m_dragging = true;
                // window() 就是你的 QDialog 顶层窗口
                m_dragOffset = e->globalPos() - window()->frameGeometry().topLeft();
                return true;
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto *e = static_cast<QMouseEvent*>(event);
            if (m_dragging && (e->buttons() & Qt::LeftButton)) {
                window()->move(e->globalPos() - m_dragOffset);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            m_dragging = false;
            return true;
        }
    }

    return QWidget::eventFilter(obj, event);
}
