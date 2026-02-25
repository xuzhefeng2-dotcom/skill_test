#include "vlfsensorwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QtMath>
#include <cmath>
#include <algorithm>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QDebug>

// AbsoluteTimeAxisTicker实现
QString AbsoluteTimeAxisTicker::getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision)
{
    Q_UNUSED(locale)
    Q_UNUSED(formatChar)
    Q_UNUSED(precision)

    // tick表示相对秒数（0, 1, 2, 3...）
    int index = static_cast<int>(tick);
    if (index >= 0 && index < m_marks.size()) {
        QDateTime dt = m_marks[index].absoluteTime;

        // ✅ 首尾显示完整信息：秒\n年月日\n时分
        // 中间只显示秒数
        if (index == 0 || index == m_marks.size() - 1) {
            // 第一个和最后一个刻度：显示 秒\n年月日\n时分
            return dt.toString("ss\nyyyy-MM-dd\nHH:mm");
        } else {
            // 中间刻度：只显示秒
            return dt.toString("ss");
        }
    }

    // 如果超出范围，返回数字
    return QString::number(tick, 'f', 0);
}

// ✅ 强制每2秒一个刻度
QVector<double> AbsoluteTimeAxisTicker::createTickVector(double tickStep, const QCPRange &range)
{
    Q_UNUSED(tickStep)  // 忽略默认步长，使用固定的2秒间隔

    QVector<double> ticks;
    if (m_marks.isEmpty()) {
        return ticks;
    }

    // 固定步长：2秒
    const double fixedStep = 2.0;

    // 计算起始和结束位置（向下/向上取整到2的倍数）
    int startIndex = static_cast<int>(qFloor(range.lower / fixedStep)) * fixedStep;
    int endIndex = static_cast<int>(qCeil(range.upper / fixedStep)) * fixedStep;

    // 生成刻度（每2秒一个）
    for (int i = startIndex; i <= endIndex; i += static_cast<int>(fixedStep)) {
        if (i >= 0 && i < m_marks.size()) {
            ticks.append(static_cast<double>(i));
        }
    }

    return ticks;
}

// NetworkTimeAxisTicker实现
QString NetworkTimeAxisTicker::getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision)
{
    Q_UNUSED(locale)
    Q_UNUSED(formatChar)
    Q_UNUSED(precision)

    // tick是相对秒数（0, 2, 4, 6...）
    // 转换为实际时间戳
    QDateTime tickTime = m_startTime.addMSecs(static_cast<qint64>(tick * 1000));

    // 首尾显示完整信息，中间只显示时分秒
    if (qAbs(tick) < 0.1 || qAbs(tick - m_displayRange) < 0.1) {
        // 起始和结束位置：显示 年月日\n时分秒
        return tickTime.toString("yyyy-MM-dd\nHH:mm:ss");
    } else {
        // 中间刻度：只显示时分秒
        return tickTime.toString("HH:mm:ss");
    }
}

QVector<double> NetworkTimeAxisTicker::createTickVector(double tickStep, const QCPRange &range)
{
    Q_UNUSED(tickStep)  // 忽略默认步长，使用固定的2秒间隔

    QVector<double> ticks;

    // 固定步长：2秒
    const double fixedStep = 2.0;

    // 计算起始和结束位置（向下/向上取整到2的倍数）
    double startTick = qFloor(range.lower / fixedStep) * fixedStep;
    double endTick = qCeil(range.upper / fixedStep) * fixedStep;

    // 生成刻度（每2秒一个）
    for (double t = startTick; t <= endTick; t += fixedStep) {
        if (t >= 0 && t <= m_displayRange) {
            ticks.append(t);
        }
    }

    return ticks;
}

// // TimeAxisTicker实现
// QString TimeAxisTicker::getTickLabel(double tick, const QLocale &locale, QChar formatChar, int precision)
// {
//     Q_UNUSED(locale)
//     Q_UNUSED(formatChar)
//     Q_UNUSED(precision)

//     //QDateTime tickTime = m_startTime.addMSecs(static_cast<qint64>(tick * 1000));
//     QDateTime tickTime = m_startTime.addMSecs(static_cast<qint64>(tick * 1600));
//     // 起始位置和终点位置：三行显示 - 秒、年月日、时分
//     //if (qFuzzyCompare(tick, 0.0) || qFuzzyCompare(tick, 10.0)) {
//     if (qFuzzyCompare(tick, 0.0) || qFuzzyCompare(tick, 16.0)) {
//         return tickTime.toString("ss\nyyyy-MM-dd\nHH:mm");
//     }

//     // 中间刻度：只显示秒
//     return tickTime.toString("ss");
// }

// VLFSensorWidget实现
VLFSensorWidget::VLFSensorWidget(const QString &channelName, const QString &orientation, QWidget *parent)
    : QWidget(parent)
    , m_channelName(channelName)
    , m_orientation(orientation)
    , m_isLogScale(false)
    , m_spectrumGraph(nullptr)
    , m_useColoredSpectrum(false)  // 禁用渐变色，使用单条纯色线
    , m_colorMapTimeSlots(TOTAL_SLOTS)
    , m_colorMapFreqCount(994)  // 改为与频谱图相同的分辨率（50 Hz）
    , m_initialAlignmentDone(false)
    , m_autoClearPlot(false)
{
    setMinimumSize(1000, 200);  // 减小最小高度（从250改为200）
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(2);

    // 标题栏
    QHBoxLayout *titleLayout = new QHBoxLayout();
    m_titleLabel = new QLabel("[" + channelName + "] VLF Magnetic (" + orientation + ") Monitor");
    m_titleLabel->setStyleSheet("color: white; font-size: 12pt; font-weight: bold;");

    // 线性/对数切换按钮（显示点击后将切换到的模式）
    // 当前是线性模式，按钮显示"对数坐标轴"表示点击后切换到对数
    m_scaleButton = new QPushButton("对数坐标轴");
    m_scaleButton->setFixedSize(90, 24);
    m_scaleButton->setStyleSheet(R"(
        QPushButton {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #3498db, stop:1 #1abc9c);
            color: white;
            border: none;
            border-radius: 4px;
            font-size: 11pt;
            font-weight: bold;
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #2980b9, stop:1 #16a085);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #1a5276, stop:1 #0e6655);
        }
    )");
    connect(m_scaleButton, &QPushButton::clicked, this, &VLFSensorWidget::onScaleButtonClicked);

    m_closeButton = new QPushButton("×");
    m_closeButton->setFixedSize(20, 20);
    m_closeButton->setStyleSheet(R"(
        QPushButton {
            background-color: transparent;
            color: white;
            border: none;
            font-size: 14pt;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #e74c3c;
            border-radius: 10px;
        }
    )");
    connect(m_closeButton, &QPushButton::clicked, this, &VLFSensorWidget::closeRequested);

    titleLayout->addWidget(m_titleLabel);
    titleLayout->addSpacing(300);  // 标题右侧固定300像素
    titleLayout->addWidget(m_scaleButton);
    titleLayout->addStretch();
    titleLayout->addWidget(m_closeButton);
    mainLayout->addLayout(titleLayout);

    // 内容区域: 左侧瀑布图 + 右侧频谱图
    QHBoxLayout *contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(5);  // 设置间距

    // 左侧: 时频瀑布图
    setupSpectrogramPlot();
    contentLayout->addWidget(m_spectrogramPlot, 3);  // 比例3

    // 右侧: 实时频谱图
    setupSpectrumPlot();
    contentLayout->addWidget(m_spectrumPlot, 1);  // 比例1（直接使用频谱图，不旋转）

    mainLayout->addLayout(contentLayout);
    setLayout(mainLayout);
}

void VLFSensorWidget::setupSpectrogramPlot()
{
    m_spectrogramPlot = new QCustomPlot();
    m_spectrogramPlot->setMinimumSize(600, 220);  // 减小最小高度（从300改为220）
    m_spectrogramPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_spectrogramPlot->setBackground(Qt::black);

    // 坐标轴样式
    m_spectrogramPlot->xAxis->setBasePen(QPen(Qt::white));
    m_spectrogramPlot->yAxis->setBasePen(QPen(Qt::white));
    m_spectrogramPlot->xAxis->setTickPen(QPen(Qt::white));
    m_spectrogramPlot->yAxis->setTickPen(QPen(Qt::white));
    m_spectrogramPlot->xAxis->setSubTickPen(QPen(Qt::transparent));  // 隐藏子刻度
    m_spectrogramPlot->yAxis->setSubTickPen(QPen(Qt::transparent));  // 隐藏子刻度
    m_spectrogramPlot->xAxis->setTickLabelColor(Qt::white);
    m_spectrogramPlot->yAxis->setTickLabelColor(Qt::white);
    m_spectrogramPlot->xAxis->setLabelColor(Qt::white);
    m_spectrogramPlot->yAxis->setLabelColor(Qt::white);

    // 网格
    m_spectrogramPlot->xAxis->grid()->setVisible(false);
    m_spectrogramPlot->yAxis->grid()->setVisible(false);
    m_spectrogramPlot->xAxis->grid()->setPen(QPen(QColor(50, 50, 50), 0.5));
    m_spectrogramPlot->yAxis->grid()->setPen(QPen(QColor(50, 50, 50), 0.5));

    // 颜色图
    m_colorMap = new QCPColorMap(m_spectrogramPlot->xAxis, m_spectrogramPlot->yAxis);

    // 颜色刻度条
    m_colorScale = new QCPColorScale(m_spectrogramPlot);
    m_spectrogramPlot->plotLayout()->addElement(0, 1, m_colorScale);
    m_colorScale->setType(QCPAxis::atRight);
    m_colorMap->setColorScale(m_colorScale);

    // 使用边距组确保色条和瀑布图的绘图区域高度对齐
    QCPMarginGroup *marginGroup = new QCPMarginGroup(m_spectrogramPlot);
    m_spectrogramPlot->axisRect()->setMarginGroup(QCP::msTop | QCP::msBottom, marginGroup);
    m_colorScale->setMarginGroup(QCP::msTop | QCP::msBottom, marginGroup);

    // 坐标轴标签
    m_spectrogramPlot->xAxis->setLabel("Time（s）");
    m_spectrogramPlot->yAxis->setLabel("Frequency (kHz)");

    // 颜色刻度条样式
    m_colorScale->axis()->setBasePen(QPen(Qt::white));
    m_colorScale->axis()->setTickPen(QPen(Qt::white));
    m_colorScale->axis()->setSubTickPen(QPen(Qt::transparent));  // 隐藏子刻度
    m_colorScale->axis()->setTickLabelColor(Qt::white);
    m_colorScale->axis()->setLabelColor(Qt::white);

    // Y轴:线性刻度 0.3-130 kHz
    QSharedPointer<QCPAxisTicker> linearTicker(new QCPAxisTicker);
    linearTicker->setTickCount(6);
    m_spectrogramPlot->yAxis->setTicker(linearTicker);
    m_spectrogramPlot->yAxis->setScaleType(QCPAxis::stLinear);
    m_spectrogramPlot->yAxis->setRange(m_freqMinKHz, m_freqMaxKHz);

    // // X轴:使用自定义时间刻度器
    // m_bindingStartTime = QDateTime::currentDateTime();
    // m_timeTicker = QSharedPointer<TimeAxisTicker>(new TimeAxisTicker(m_bindingStartTime));
    // m_timeTicker->setTickCount(16);
    // m_spectrogramPlot->xAxis->setTicker(m_timeTicker);
    // m_spectrogramPlot->xAxis->setRange(0, m_displayTimeRange);  // 设置初始X轴范围 0-10秒
    // X轴：普通数值时间轴（秒，从0开始，类似MATLAB）
    QSharedPointer<QCPAxisTicker> xTicker(new QCPAxisTicker);
    m_spectrogramPlot->xAxis->setTicker(xTicker);

    m_spectrogramPlot->xAxis->setLabel("Time (s)");
    m_spectrogramPlot->xAxis->setNumberFormat("f");
    m_spectrogramPlot->xAxis->setNumberPrecision(2);

    // 初始范围：0~m_displayTimeRange 秒
    m_spectrogramPlot->xAxis->setRange(0, m_displayTimeRange);


    // 初始化colorMap大小
    m_colorMap->data()->setSize(TOTAL_SLOTS, 100);
    m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange), QCPRange(m_freqMinKHz, m_freqMaxKHz));

    // 初始化所有单元格为最小值（显示蓝色）
    for (int x = 0; x < TOTAL_SLOTS; ++x) {
        for (int y = 0; y < 100; ++y) {
            m_colorMap->data()->setCell(x, y, 40);
        }
    }

    // 数据范围
    m_colorMap->setDataRange(QCPRange(40, 110));
    m_colorScale->setDataRange(QCPRange(40, 110));

    // Jet色图
    m_colorMap->setGradient(QCPColorGradient::gpJet);
    m_colorMap->setInterpolate(false);  // 关闭插值提升性能
}

void VLFSensorWidget::setupSpectrumPlot()
{
    m_spectrumPlot = new QCustomPlot();
    m_spectrumPlot->setMinimumSize(200, 220);
    m_spectrumPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_spectrumPlot->setBackground(Qt::black);

    // 显示四条坐标轴
    m_spectrumPlot->axisRect()->setupFullAxesBox(true);

    // 坐标轴样式（四条轴）
    m_spectrumPlot->xAxis->setBasePen(QPen(Qt::white));
    m_spectrumPlot->yAxis->setBasePen(QPen(Qt::white));
    m_spectrumPlot->xAxis2->setBasePen(QPen(Qt::white));
    m_spectrumPlot->yAxis2->setBasePen(QPen(Qt::white));
    m_spectrumPlot->xAxis->setTickPen(QPen(Qt::white));
    m_spectrumPlot->yAxis->setTickPen(QPen(Qt::white));
    m_spectrumPlot->xAxis2->setTickPen(QPen(Qt::white));
    m_spectrumPlot->yAxis2->setTickPen(QPen(Qt::white));
    m_spectrumPlot->xAxis->setSubTickPen(QPen(Qt::transparent));  // 隐藏子刻度
    m_spectrumPlot->yAxis->setSubTickPen(QPen(Qt::transparent));  // 隐藏子刻度
    m_spectrumPlot->xAxis2->setSubTickPen(QPen(Qt::transparent));  // 隐藏子刻度
    m_spectrumPlot->yAxis2->setSubTickPen(QPen(Qt::transparent));  // 隐藏子刻度
    m_spectrumPlot->xAxis->setTickLabelColor(Qt::white);
    m_spectrumPlot->yAxis->setTickLabelColor(Qt::white);
    m_spectrumPlot->xAxis2->setTickLabelColor(Qt::white);
    m_spectrumPlot->yAxis2->setTickLabelColor(Qt::white);
    m_spectrumPlot->xAxis->setLabelColor(Qt::white);
    m_spectrumPlot->yAxis->setLabelColor(Qt::white);

    // 设置刻度标签显示
    m_spectrumPlot->xAxis->setTickLabels(true);   // X轴显示频率刻度
    m_spectrumPlot->xAxis2->setTickLabels(false);  // 顶部X轴不显示刻度
    m_spectrumPlot->yAxis->setTickLabels(true);    // Y轴显示dB刻度
    m_spectrumPlot->yAxis2->setTickLabels(false);  // 右侧Y轴不显示刻度

    // 网格
    m_spectrumPlot->xAxis->grid()->setVisible(true);
    m_spectrumPlot->yAxis->grid()->setVisible(true);
    m_spectrumPlot->xAxis->grid()->setPen(QPen(QColor(50, 50, 50), 0.5));
    m_spectrumPlot->yAxis->grid()->setPen(QPen(QColor(50, 50, 50), 0.5));

    // 坐标轴标签 - X轴为频率，Y轴为dB
    m_spectrumPlot->xAxis->setLabel("Frequency (kHz)");
    m_spectrumPlot->yAxis->setLabel("dB");
    m_spectrumPlot->xAxis2->setLabelColor(Qt::white);
    m_spectrumPlot->yAxis2->setLabelColor(Qt::white);

    // X轴范围（频率kHz，初始默认值，将由setSampleRate()动态更新）
    m_spectrumPlot->xAxis->setRange(0, 125);

    // Y轴范围（dB，归一化后的40~110）
    m_spectrumPlot->yAxis->setRange(40, 110);

    // 初始化颜色梯度（与瀑布图一致：Jet渐变）
    m_colorGradient = QCPColorGradient::gpJet;

    // 创建单条纯色Graph（不使用渐变色）
    m_spectrumGraph = m_spectrumPlot->addGraph(m_spectrumPlot->xAxis, m_spectrumPlot->yAxis);
    m_spectrumGraph->setPen(QPen(QColor(0, 255, 255), 1.5));  // 青色线条
    m_spectrumGraph->setLineStyle(QCPGraph::lsLine);
    m_spectrumGraph->setVisible(true);

    // 设置X轴（频率）刻度
    QSharedPointer<QCPAxisTicker> freqTicker(new QCPAxisTicker);
    freqTicker->setTickCount(6);
    m_spectrumPlot->xAxis->setTicker(freqTicker);

    // 设置Y轴（dB）刻度
    QSharedPointer<QCPAxisTicker> dbTicker(new QCPAxisTicker);
    dbTicker->setTickCount(6);
    m_spectrumPlot->yAxis->setTicker(dbTicker);

    // 连接轴，确保上下、左右轴范围同步
    connect(m_spectrumPlot->xAxis, SIGNAL(rangeChanged(QCPRange)), m_spectrumPlot->xAxis2, SLOT(setRange(QCPRange)));
    connect(m_spectrumPlot->yAxis, SIGNAL(rangeChanged(QCPRange)), m_spectrumPlot->yAxis2, SLOT(setRange(QCPRange)));
}

// void VLFSensorWidget::buildLogFreqIndex()
// {
//     // 参考D:\qtpro\dest\mainwindow.cpp第572-608行的实现
//     int ny = m_colorMapFreqCount;
//     if (ny <= 0) return;

//     double freqMin = m_freqMinKHz;
//     double freqMax = m_freqMaxKHz;
//     if (freqMin <= 0) {
//         freqMin = 0;
//     }

//     double logFreqMin = std::log10(freqMin);
//     double logFreqMax = std::log10(freqMax);
//     double denom = (ny - 1) > 0 ? (ny - 1) : 1;

//     m_logFreqIndex.resize(ny);

//     // 构建从对数Y坐标到线性频率索引的映射
//     // 对于colorMap的每个Y像素位置，计算其在对数空间中对应的频率，
//     // 然后找到该频率在原始线性频率数组中的索引
//     for (int y = 0; y < ny; ++y) {
//         // 计算对数空间中的频率
//         double logFreq = logFreqMin + y * (logFreqMax - logFreqMin) / denom;
//         double freqVal = std::pow(10.0, logFreq);

//         // 将频率转换为线性索引
//         // 原始数据的频率范围是m_freqMinKHz到m_freqMaxKHz，共ny个点
//         double ratio = (freqVal - m_freqMinKHz) / (m_freqMaxKHz - m_freqMinKHz);
//         int freqIndex = static_cast<int>(ratio * (ny - 1));
//         freqIndex = qBound(0, freqIndex, ny - 1);

//         m_logFreqIndex[y] = freqIndex;
//     }
// }
void VLFSensorWidget::buildLogFreqIndex()
{
    int ny = m_colorMapFreqCount;
    if (ny <= 0) return;

    // ✅ 对数显示范围（Y轴上显示的范围，根据采样率动态计算）
    double displayMin = 1.0;      // 1 kHz
    // 动态计算displayMax：不超过Nyquist频率，并向上取整到10的幂次
    // 例如：125kHz -> 100kHz, 2000kHz -> 1000kHz
    double nyquistKHz = m_freqMaxKHz;  // Nyquist频率
    double displayMax = std::pow(10.0, std::floor(std::log10(nyquistKHz)));
    // 如果Nyquist频率明显大于当前10的幂次，则使用更大的范围
    if (nyquistKHz > displayMax * 5.0) {
        displayMax *= 10.0;
    }
    displayMax = std::min(displayMax, nyquistKHz);  // 不超过Nyquist频率

    // ✅ 原始数据的频率范围（用于索引映射）
    double dataMin = m_freqMinKHz;    // 0.1 kHz（或其他最小频率）
    double dataMax = m_freqMaxKHz;    // Nyquist频率（根据采样率动态设置）

    double logFreqMin = std::log10(displayMin);
    double logFreqMax = std::log10(displayMax);
    double denom = (ny - 1) > 0 ? (ny - 1) : 1;

    m_logFreqIndex.resize(ny);

    for (int y = 0; y < ny; ++y) {
        // 在对数显示空间中计算频率（1~displayMax kHz）
        double logFreq = logFreqMin + y * (logFreqMax - logFreqMin) / denom;
        double freqVal = std::pow(10.0, logFreq);

        // ✅ 关键修复: 将显示频率映射到原始数据索引
        // 原始数据是线性分布在 dataMin~dataMax 范围内的
        double ratio = (freqVal - dataMin) / (dataMax - dataMin);
        int freqIndex = static_cast<int>(ratio * (ny - 1));
        freqIndex = qBound(0, freqIndex, ny - 1);

        m_logFreqIndex[y] = freqIndex;
    }

// qDebug() << "[" << m_channelName << "] 对数模式频率范围: " << displayMin << "~" << displayMax << "kHz (Nyquist=" << nyquistKHz << "kHz)";
}

void VLFSensorWidget::onScaleButtonClicked()
{
    m_isLogScale = !m_isLogScale;

    if (m_isLogScale) {
        // 当前切换到对数模式，按钮显示"线性坐标轴"（表示点击后切换到线性）
        m_scaleButton->setText("线性坐标轴");
        m_scaleButton->setStyleSheet(R"(
            QPushButton {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #f39c12, stop:1 #e74c3c);
                color: white;
                border: none;
                border-radius: 4px;
                font-size: 11pt;
                font-weight: bold;
            }
            QPushButton:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #d68910, stop:1 #c0392b);
            }
            QPushButton:pressed {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #b9770e, stop:1 #922b21);
            }
        )");
        buildLogFreqIndex();  // 构建对数索引映射表
        applyLogScale();
    } else {
        // 当前切换到线性模式，按钮显示"对数坐标轴"（表示点击后切换到对数）
        m_scaleButton->setText("对数坐标轴");
        m_scaleButton->setStyleSheet(R"(
            QPushButton {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #3498db, stop:1 #1abc9c);
                color: white;
                border: none;
                border-radius: 4px;
                font-size: 11pt;
                font-weight: bold;
            }
            QPushButton:hover {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #2980b9, stop:1 #16a085);
            }
            QPushButton:pressed {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                    stop:0 #1a5276, stop:1 #0e6655);
            }
        )");
        applyLinearScale();
    }

    // 从缓存重绘瀑布图（切换时立即更新瀑布图内容）
    redrawColorMapFromCache();

    // 重新应用频谱数据
    if (!m_lastFreq.isEmpty() && !m_lastSpectrum.isEmpty()) {
        setSpectrumData(m_lastFreq, m_lastSpectrum);
    }

    m_spectrogramPlot->replot();
    m_spectrumPlot->replot();
}

void VLFSensorWidget::applyLinearScale()
{
    // 瀑布图Y轴：线性刻度
    QSharedPointer<QCPAxisTicker> linearTicker(new QCPAxisTicker);
    linearTicker->setTickCount(6);
    m_spectrogramPlot->yAxis->setTicker(linearTicker);
    m_spectrogramPlot->yAxis->setScaleType(QCPAxis::stLinear);
    m_spectrogramPlot->yAxis->setRange(m_freqMinKHz, m_freqMaxKHz);

    // colorMap数据范围（线性）
    m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange), QCPRange(m_freqMinKHz, m_freqMaxKHz));

    // 频谱图Y轴：线性刻度（dB值，40~110）
    QSharedPointer<QCPAxisTicker> spectrumYTicker(new QCPAxisTicker);
    spectrumYTicker->setTickCount(6);
    m_spectrumPlot->yAxis->setTicker(spectrumYTicker);
    m_spectrumPlot->yAxis->setScaleType(QCPAxis::stLinear);
    m_spectrumPlot->yAxis->setRange(40, 110);  // dB范围

    // 频谱图X轴：始终保持线性（频率，0~Nyquist频率）
    m_spectrumPlot->xAxis->setRange(0, m_freqMaxKHz);
}
void VLFSensorWidget::applyLogScale()
{
    QSharedPointer<QCPAxisTickerLog> logTicker(new QCPAxisTickerLog);
    logTicker->setLogBase(10);
    logTicker->setSubTickCount(0);
    m_spectrogramPlot->yAxis->setTicker(logTicker);
    m_spectrogramPlot->yAxis->setScaleType(QCPAxis::stLogarithmic);

    // ✅ 确保显示范围与buildLogFreqIndex中的范围一致
    double displayMin = 1;
    double displayMax = 1000;
    m_spectrogramPlot->yAxis->setRange(displayMin, displayMax);

    m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange),
                                 QCPRange(displayMin, displayMax));

    // 频谱图Y轴：对数刻度（dB值，1~1000，显示为0 1 10 100 1000）
    QSharedPointer<QCPAxisTickerLog> spectrumYLogTicker(new QCPAxisTickerLog);
    spectrumYLogTicker->setLogBase(10);
    spectrumYLogTicker->setSubTickCount(0);
    m_spectrumPlot->yAxis->setTicker(spectrumYLogTicker);
    m_spectrumPlot->yAxis->setScaleType(QCPAxis::stLogarithmic);
    m_spectrumPlot->yAxis->setRange(1, 1000);  // 对数范围：1~1000

    // 频谱图X轴：始终保持线性（频率，0~Nyquist频率）
    m_spectrumPlot->xAxis->setRange(0, m_freqMaxKHz);
}
// void VLFSensorWidget::applyLogScale()
// {
//     const double kMinPos = 1e-3;
// double yMin = std::max(m_freqMinKHz, kMinPos);
//     // 瀑布图Y轴：对数刻度
//     QSharedPointer<QCPAxisTickerLog> logTicker(new QCPAxisTickerLog);
//     logTicker->setLogBase(10);
//     logTicker->setSubTickCount(0);  // 不显示子刻度
//     m_spectrogramPlot->yAxis->setTicker(logTicker);
//     // m_spectrogramPlot->yAxis->setScaleType(QCPAxis::stLogarithmic);
//     // m_spectrogramPlot->yAxis->setRange(m_freqMinKHz, m_freqMaxKHz);
//     m_spectrogramPlot->yAxis->setScaleType(QCPAxis::stLogarithmic);
//     m_spectrogramPlot->yAxis->setRange(yMin, m_freqMaxKHz);
//     // colorMap数据范围
//     // m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange), QCPRange(m_freqMinKHz, m_freqMaxKHz));
//     m_colorMap->data()->setRange(QCPRange(0, m_displayTimeRange), QCPRange(yMin, m_freqMaxKHz));

//     // 频谱图X轴：对数刻度
//     QSharedPointer<QCPAxisTickerLog> spectrumLogTicker(new QCPAxisTickerLog);
//     spectrumLogTicker->setLogBase(10);
//     spectrumLogTicker->setSubTickCount(0);  // 不显示子刻度
//     m_spectrumPlot->xAxis->setTicker(spectrumLogTicker);
//     m_spectrumPlot->xAxis->setScaleType(QCPAxis::stLogarithmic);
//     m_spectrumPlot->xAxis->setRange(m_freqMinKHz, m_freqMaxKHz);
//     m_spectrumPlot->xAxis->setRangeReversed(true);  // 保持反转
// }

void VLFSensorWidget::updateTimeAxis(const QDateTime &timestamp)
{
    if (!timestamp.isValid()) return;

    // ✅ 网络模式：使用NetworkTimeAxisTicker显示真实时间戳
    if (m_spectrogramPlot && m_spectrogramPlot->xAxis) {
        // 创建NetworkTimeAxisTicker并设置为X轴刻度器
        QSharedPointer<NetworkTimeAxisTicker> ticker(new NetworkTimeAxisTicker(timestamp, m_displayTimeRange));
        m_spectrogramPlot->xAxis->setTicker(ticker);

// qDebug() << "[VLFSensorWidget] 更新时间轴，起始时间戳:" << timestamp.toString("yyyy-MM-dd HH:mm:ss.zzz")
    //                  << "显示范围:" << m_displayTimeRange << "秒";

        // 强制重绘
        m_spectrogramPlot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void VLFSensorWidget::setDisplayTimeRange(double seconds)
{
    if (seconds <= 0) return;

    m_displayTimeRange = seconds;

    // 更新时间轴范围
    if (m_spectrogramPlot) {
        m_spectrogramPlot->xAxis->setRange(0, seconds);
    }

    // 如果你有 colorMap，也同步一下
    if (m_colorMap) {
        m_colorMap->data()->setKeyRange(QCPRange(0, seconds));
    }
}
void VLFSensorWidget::setSampleRate(double fsHz)
{
    // 频率单位：kHz；显示范围：0.1 kHz ~ Nyquist
    m_freqMinKHz = 0.1;
    m_freqMaxKHz = std::max(m_freqMinKHz, fsHz / 2000.0); // fs/2 (Hz) => kHz

    // 根据当前刻度模式刷新坐标轴和colorMap范围
    if (m_isLogScale) {
        applyLogScale();
    } else {
        applyLinearScale();
    }

    m_spectrogramPlot->replot();
    m_spectrumPlot->replot();
}



void VLFSensorWidget::initializeColorMapSize(int timeSlots, int freqCount)
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

    m_spectrogramPlot->replot();
}

void VLFSensorWidget::setColorMapColumn(int timeSlot, const QVector<double> &freqValues, const QVector<double> &spectrum)
{
    if (freqValues.size() != spectrum.size() || freqValues.isEmpty()) {
        return;
    }

    int ny = m_colorMapFreqCount;
    int dataSize = qMin(freqValues.size(), ny);

    // 缓存原始数据（始终保存线性模式的原始数据）
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
        // 参考D:\qtpro\dest\mainwindow.cpp第696-713行
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

void VLFSensorWidget::setColorMapColumnRaw(int timeSlot, const double* spectrum, int freqCount)
{
    if (timeSlot < 0 || timeSlot >= m_colorMap->data()->keySize()) {
// qDebug() << "[DROP] timeSlot out of range:" << timeSlot;
        return;
    }
    if (!spectrum || freqCount <= 0) {
        return;
    }

    int ny = m_colorMapFreqCount;
    int dataSize = qMin(freqCount, ny);

    // 缓存原始数据（始终保存线性模式的原始数据）
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

void VLFSensorWidget::setSpectrumData(const QVector<double> &freq, const QVector<double> &spectrum)
{
    if (freq.size() != spectrum.size() || freq.isEmpty()) {
        return;
    }

    // 缓存原始数据
    m_lastFreq = freq;
    m_lastSpectrum = spectrum;

    // 只使用单条纯色线，不使用渐变色
    if (m_spectrumGraph) {
        // 先按频率排序（确保数据按X轴有序）
        QVector<QPair<double, double>> pairs;  // (freq, spectrum)
        for (int i = 0; i < freq.size(); ++i) {
            pairs.append(qMakePair(freq[i], spectrum[i]));
        }
        std::sort(pairs.begin(), pairs.end(), [](const QPair<double,double>& a, const QPair<double,double>& b) {
            return a.first < b.first;  // 按频率升序
        });

        QVector<double> sortedFreq, sortedSpectrum;
        for (const auto& p : pairs) {
            sortedFreq.append(p.first);
            sortedSpectrum.append(p.second);
        }

        // 直接使用排序后的数据（X=频率，Y=dB）
        m_spectrumGraph->setData(sortedFreq, sortedSpectrum);
        m_spectrumGraph->setVisible(true);
    }
}

void VLFSensorWidget::replotSpectrogram()
{
    m_spectrogramPlot->replot();
}

void VLFSensorWidget::replotSpectrum()
{
    m_spectrumPlot->replot();
}

void VLFSensorWidget::redrawColorMapFromCache()
{
    // 从缓存的原始数据重绘整个瀑布图
    int nx = m_colorMapTimeSlots;
    int ny = m_colorMapFreqCount;

    if (m_rawSpectrumCache.size() != nx) {
        return;
    }

    if (m_isLogScale) {
        // 对数模式：使用预计算的索引映射
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
        // 线性模式：直接使用原始数据
        for (int x = 0; x < nx; ++x) {
            const QVector<double> &spectrum = m_rawSpectrumCache[x];
            int dataSize = qMin(spectrum.size(), ny);
            for (int f = 0; f < dataSize; ++f) {
                m_colorMap->data()->setCell(x, f, spectrum[f]);
            }
        }
    }
}

void VLFSensorWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 不再需要旋转相关的对齐代码
}

void VLFSensorWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 不再需要旋转相关的对齐代码
}

void VLFSensorWidget::clearColorMap()
{
    if (!m_colorMap) {
        return;
    }

    // 清空colorMap的所有数据，使用40（蓝色）而不是100（深红色）
    for (int x = 0; x < m_colorMapTimeSlots; ++x) {
        for (int y = 0; y < m_colorMapFreqCount; ++y) {
            m_colorMap->data()->setCell(x, y, 40);
        }
    }

    // 清空原始数据缓存
    m_rawSpectrumCache.clear();
    m_rawSpectrumCache.resize(m_colorMapTimeSlots);

// qDebug() << "[" << m_channelName << "] 图表已清空（蓝色背景）";
}

void VLFSensorWidget::setAbsoluteTimeMode(const QVector<SecondMark> &marks)
{
    if (!m_colorMap || marks.isEmpty()) {
// qWarning() << "[" << m_channelName << "] 无法切换到绝对时间模式：colorMap未初始化或时间标记为空";
        return;
    }

    // 创建绝对时间刻度器
    m_absoluteTimeTicker = QSharedPointer<AbsoluteTimeAxisTicker>::create(marks);

    // 应用到X轴
    m_colorMap->keyAxis()->setTicker(m_absoluteTimeTicker);

    // 设置X轴范围为整秒数（0, 1, 2, 3...）
    m_colorMap->keyAxis()->setRange(0, marks.size() - 1);

    // 刷新显示
    m_spectrogramPlot->replot();

// qDebug() << "[" << m_channelName << "] 已切换到绝对时间模式，时间标记数:" << marks.size();
}
void VLFSensorWidget::setRelativeTimeMode()
{
    if (!m_colorMap) {
// qWarning() << "[" << m_channelName << "] 无法切换到相对时间模式：colorMap未初始化";
        return;
    }

    // 移除绝对时间刻度器，恢复默认刻度器
    m_absoluteTimeTicker.reset();
    m_colorMap->keyAxis()->setTicker(QSharedPointer<QCPAxisTicker>::create());

    // 恢复X轴范围（使用displayTimeRange或默认值）
    if (m_displayTimeRange > 0) {
        m_colorMap->keyAxis()->setRange(0, m_displayTimeRange);
    } else {
        m_colorMap->keyAxis()->setRange(0, 10);  // 默认10秒
    }

    // 刷新显示
    m_spectrogramPlot->replot();

// qDebug() << "[" << m_channelName << "] 已切换到相对时间模式";
}
