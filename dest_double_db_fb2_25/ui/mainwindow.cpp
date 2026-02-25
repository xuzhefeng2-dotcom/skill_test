#include "mainwindow.h"
#include <QDialog> 
#include <QVBoxLayout> 
#include <QHBoxLayout> 
#include <QLabel> 
#include <QPushButton>
#include <QMenuBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QDebug>
#include <QFileInfo>
#include <cmath>
#include <QThread>
#include <QApplication>   // 或 <QCoreApplication>
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , currentMode(MODE_UDP_DUAL_CLIENT)
    , serverRunning(false)
    , udpDualServer(nullptr)
    , udpDualClient(nullptr)
    , localFileReader(nullptr)
    , webSocketClient(nullptr)
    , fftPlotTimer(nullptr)
    , spectrumPlotTimer(nullptr)
    , currentTimeSlot(0)
    , colorMapInitialized(false)
    , m_drawRefreshInterval(50) 
{
    setupUI();
    setupMenus();


    qRegisterMetaType<SpectrogramBatch>("SpectrogramBatch");

    // 创建WebSocket客户端
    webSocketClient = new WebSocketClient(this);
    connect(webSocketClient, &WebSocketClient::connected, this, &MainWindow::onWebSocketConnected);
    connect(webSocketClient, &WebSocketClient::disconnected, this, &MainWindow::onWebSocketDisconnected);
    connect(webSocketClient, &WebSocketClient::ackReceived, this, &MainWindow::onWebSocketAckReceived);
    connect(webSocketClient, &WebSocketClient::commandACKReceived, this, &MainWindow::onWebSocketCommandAckReceived);  // 新信号，带数据负载
    connect(webSocketClient, &WebSocketClient::statusReceived, this, &MainWindow::onWebSocketStatusReceived);
    connect(webSocketClient, &WebSocketClient::error, this, &MainWindow::onWebSocketError);

    // WebSocket：程序启动自动连接；断线则持续重连直到连接成功
    m_wsReconnectTimer = new QTimer(this);
    m_wsReconnectTimer->setInterval(m_wsReconnectIntervalMs);
    m_wsReconnectTimer->setSingleShot(false);
    connect(m_wsReconnectTimer, &QTimer::timeout, this, [this]() {
        if (!webSocketClient) return;
        if (webSocketClient->isConnected()) {
            m_wsReconnectTimer->stop();
            return;
        }
// qDebug() << "[主窗口] WebSocket未连接，尝试重连:" << m_wsUrl;
        webSocketClient->connectToServer(m_wsUrl);
    });

    // 立即尝试首次连接
    webSocketClient->connectToServer(m_wsUrl);
    m_wsReconnectTimer->start();

    // 创建UDP双通道客户端并移入专用线程（onSeparatedFrames + processBatchFFT 不占主线程，提升处理速度）
    udpDualClient = new UDPClientReceiver(nullptr);
    m_receiverThread = new QThread(this);
    udpDualClient->moveToThread(m_receiverThread);
    connect(m_receiverThread, &QThread::finished, udpDualClient, &QObject::deleteLater);
    m_receiverThread->start();

    // ✅ 注释掉信号驱动更新，只使用定时器驱动（与副本项目一致）
    // connect(udpDualClient, &UDPClientReceiver::batchReady, this, &MainWindow::onClientBatchReady);
    connect(udpDualClient, &UDPClientReceiver::statsUpdated, this, &MainWindow::onClientStatsUpdated);
    connect(udpDualClient, &UDPClientReceiver::error, [this](const QString &error) {
    connect(udpDualClient, &UDPClientReceiver::spectrumTripletReady,
            this,          &MainWindow::onSpectrumTripletReady,
            Qt::QueuedConnection);
        // qCritical() << "[UDP客户端错误]" << error;
        statusConnectionLabel->setText("状态: 错误");
    });

    // 创建时间文件解析器
    timeFileParser = new TimeFileParser(this);
    connect(timeFileParser, &TimeFileParser::parseFinished, this, &MainWindow::onTimeFileParseFinished);
    connect(timeFileParser, &TimeFileParser::error, [this](const QString &error) {
// qCritical() << "[时间文件解析错误]" << error;
    });

    // ✅ 创建时间戳加载器（在子线程中执行）
    timeStampLoaderThread = new QThread(this);
    timeStampLoader = new TimeStampLoader(nullptr);  // 不设置parent，避免线程冲突
    timeStampLoader->moveToThread(timeStampLoaderThread);
    timeStampLoaderThread->start();

    connect(timeStampLoader, &TimeStampLoader::loadFinished, this, &MainWindow::onTimeDataLoaded);
    connect(timeStampLoader, &TimeStampLoader::progressChanged, this, [this](int seconds, int lines) {
        statusConnectionLabel->setText(QString("状态: 正在加载时间数据... (%1秒/%2行)").arg(seconds).arg(lines));
    });
    connect(timeStampLoader, &TimeStampLoader::error, [this](const QString &error) {
// qCritical() << "[时间戳加载错误]" << error;
    });

    // 创建FFT绘图更新定时器（用于瀑布图流式刷新）
    fftPlotTimer = new QTimer(this);
    // 不在这里connect，而是在各模式启动时动态连接

    // 创建频谱图独立刷新定时器（每100ms更新一次）
    spectrumPlotTimer = new QTimer(this);
    connect(spectrumPlotTimer, &QTimer::timeout, this, &MainWindow::updateSpectrumPlots);

    // ✅ 创建异步频谱渲染定时器（50ms间隔，处理渲染队列）
    m_spectrumRenderTimer = new QTimer(this);
    m_spectrumRenderTimer->setInterval(50);  // 50ms渲染一次，不阻塞批次处理
    connect(m_spectrumRenderTimer, &QTimer::timeout, this, &MainWindow::processSpectrumRenderQueue);
    m_spectrumRenderTimer->start();
    // 程序启动默认客户端模式（不再提供服务器/绘图模式）
    switchToUDPDualClientMode();

    // 设置默认信息面板值
    infoPanel->setLatitude("N31°14'22\"");
    infoPanel->setLongitude("E121°28'01\"");
    infoPanel->setAltitude("45m");
    infoPanel->setMagneticDeclination("-6.2° W");
    infoPanel->setSystemTemperature(35.2);
    infoPanel->setNoiseFloor(-112);
    // 创建绘制定时器
    m_spectrumDrawTimer = new QTimer(this);
    m_spectrumDrawTimer->setInterval(m_drawRefreshInterval);
    connect(m_spectrumDrawTimer, &QTimer::timeout, this, &MainWindow::onDrawTimerTimeout);
    // 注意：定时器在UDP连接后才启动
}

MainWindow::~MainWindow()
{
    // 确保子线程退出
    if (plotWorker) {
        QMetaObject::invokeMethod(plotWorker, "stop", Qt::QueuedConnection);
    }
    if (plotThread) {
        plotThread->quit();
        plotThread->wait();
        plotThread->deleteLater();
        plotThread = nullptr;
        plotWorker = nullptr;
    }

    if (localFileReader) {
        QMetaObject::invokeMethod(localFileReader, "stopProcessing", Qt::QueuedConnection);
    }
    if (localFileThread) {
        localFileThread->quit();
        localFileThread->wait();
        localFileThread->deleteLater();
        localFileThread = nullptr;
    }
    if (localFileReader) {
        localFileReader->deleteLater();
        localFileReader = nullptr;
    }

    // ✅ 清理时间戳加载器线程
    if (timeStampLoaderThread) {
        timeStampLoaderThread->quit();
        timeStampLoaderThread->wait();
        timeStampLoaderThread->deleteLater();
        timeStampLoaderThread = nullptr;
    }
    if (timeStampLoader) {
        timeStampLoader->deleteLater();
        timeStampLoader = nullptr;
    }

    // 接收管线线程：quit 后 udpDualClient 由 deleteLater 在线程内释放
    if (m_receiverThread) {
        m_receiverThread->quit();
        m_receiverThread->wait(3000);
        m_receiverThread->deleteLater();
        m_receiverThread = nullptr;
    }
    udpDualClient = nullptr;
}

void MainWindow::setupUI()
{
    setWindowTitle("甚低频采集系统");
    setMinimumSize(1400, 900);
    resize(2000, 930);  // 设置初始窗口大小

    // 设置深色主题
    setStyleSheet(R"(
        QMainWindow {
            background-color: #1a1a1a;
        }
        QMenuBar {
            background-color: #2d2d2d;
            color: #e0e0e0;
        }
        QMenuBar::item:selected {
            background-color: #3a3a3a;
        }
        QMenu {
            background-color: #2d2d2d;
            color: #e0e0e0;
            border: 1px solid #3a3a3a;
        }
        QMenu::item:selected {
            background-color: #3a3a3a;
        }
        QStatusBar {
            background-color: #2d2d2d;
            color: #e0e0e0;
        }
        QLabel {
            color: #e0e0e0;
        }
    )");

    centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    mainLayout = new QHBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 左侧信息面板
    infoPanel = new InfoPanelWidget();
    mainLayout->addWidget(infoPanel);
    
    // 创建主显示区域（新布局：上方三个频谱图 + 下方一个瀑布图）
    displayArea = new QWidget();
    displayArea->setStyleSheet("background-color: #1a1a1a;");
    QVBoxLayout *displayLayout = new QVBoxLayout(displayArea);
    displayLayout->setContentsMargins(5, 5, 5, 5);
    displayLayout->setSpacing(5);

    // 上方区域：三个频谱图 + 刷新间隔选择
    QWidget *topArea = new QWidget();
    QVBoxLayout *topLayout = new QVBoxLayout(topArea);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(5);

    // 频谱图上方：开始采集 / 开始显示（从信息面板移出）
    QHBoxLayout *controlBtnLayout = new QHBoxLayout();
    m_collectButtonTop = new QPushButton("开始采集");
    m_displayButtonTop = new QPushButton("开始显示");
    const QString controlBtnStyle = R"(
        QPushButton {
            background-color: #5DADE2;
            color: white;
            border: 1px solid #4A9DD1;
            border-radius: 4px;
            padding: 8px 14px;
            font-weight: bold;
            min-width: 110px;
        }
        QPushButton:hover {
            background-color: #6DBDF2;
        }
        QPushButton:pressed {
            background-color: #4A9DD1;
        }
        QPushButton:disabled {
            background-color: #444;
            border: 1px solid #555;
            color: #999;
        }
    )";
    m_collectButtonTop->setStyleSheet(controlBtnStyle);
    m_displayButtonTop->setStyleSheet(controlBtnStyle);
    m_collectButtonTop->setEnabled(true);
    m_displayButtonTop->setEnabled(true);

    connect(m_collectButtonTop, &QPushButton::clicked, this, [this]() {
        if (!webSocketClient || !webSocketClient->isConnected()) {
// qWarning() << "[警告] WebSocket未连接，无法发送采集命令";
            statusBar()->showMessage("WebSocket未连接，无法开始采集");
            return;
        }
        if (!m_collecting) {
            onSendStartCommand();
            m_collecting = true;
            m_collectButtonTop->setText("停止采集");
        } else {
            onSendStopCollectCommand();
            m_collecting = false;
            m_collectButtonTop->setText("开始采集");
        }
    });
        connect(m_displayButtonTop, &QPushButton::clicked, this, [this]() {
        if (!webSocketClient || !webSocketClient->isConnected()) {
// qWarning() << "[警告] WebSocket未连接，无法发送显示命令";
            statusBar()->showMessage("WebSocket未连接，无法开始显示");
            return;
        }
        if (!m_displaying) {
            onSendDisplayCommand();
            m_displaying = true;
            m_displayButtonTop->setText("停止显示");
        } else {
            onSendStopDisplayCommand();
            m_displaying = false;
            m_displayButtonTop->setText("开始显示");
        }
    });
    // connect(m_collectButtonTop, &QPushButton::clicked, this, [this]() {
    //     if (!m_collecting) {
    //             QHostAddress serverIp("127.0.0.1");   // 和 Go 客户端里的 serverIP 一致
    //             quint16 serverPort = 5555;            // 和 serverPort 一致
    //             QByteArray reg("register");
    //             qint64 sent = m_socket->writeDatagram(reg, serverIp, serverPort);
    //             qDebug() << "[UdpReceiverWorker] 发送注册包到"
    //             << serverIp.toString() << ":" << serverPort
    //             << "size=" << sent;
    //         m_collecting = true;
    //         m_collectButtonTop->setText("停止采集");
    //     } else {
    //         //onSendStopCollectCommand();
    //         m_collecting = false;
    //         m_collectButtonTop->setText("开始采集");
    //     }
    // });
    // connect(m_displayButtonTop, &QPushButton::clicked, this, [this]() {
    //     if (!webSocketClient || !webSocketClient->isConnected()) {
    //         qWarning() << "[警告] WebSocket未连接，无法发送显示命令";
    //         statusBar()->showMessage("WebSocket未连接，无法开始显示");
    //         return;
    //     }
    //     if (!m_displaying) {
    //         onSendDisplayCommand();
    //         m_displaying = true;
    //         m_displayButtonTop->setText("停止显示");
    //     } else {
    //         onSendStopDisplayCommand();
    //         m_displaying = false;
    //         m_displayButtonTop->setText("开始显示");
    //     }
    // });



    // 刷新间隔选择控件
    QHBoxLayout *refreshLayout = new QHBoxLayout();
    QLabel *refreshLabel = new QLabel("频谱刷新间隔:");
    refreshLabel->setStyleSheet("color: #e0e0e0; font-weight: bold;");
    spectrumRefreshCombo = new QComboBox();
    spectrumRefreshCombo->addItem("50 ms", 50);
    spectrumRefreshCombo->addItem("100 ms", 100);
    spectrumRefreshCombo->addItem("200 ms", 200);
    spectrumRefreshCombo->addItem("500 ms", 500);  // 默认
    spectrumRefreshCombo->addItem("1 s", 1000);
    spectrumRefreshCombo->setCurrentIndex(3);  // 默认500ms
    spectrumRefreshCombo->setStyleSheet(R"(
        QComboBox {
            background-color: #3a3a3a;
            color: #e0e0e0;
            border: 1px solid #555;
            border-radius: 3px;
            padding: 4px;
            min-width: 80px;
        }
        QComboBox::drop-down {
            border: none;
        }
        QComboBox::down-arrow {
            image: none;
            border-left: 4px solid transparent;
            border-right: 4px solid transparent;
            border-top: 6px solid #e0e0e0;
            margin-right: 5px;
        }
        QComboBox QAbstractItemView {
            background-color: #3a3a3a;
            color: #e0e0e0;
            selection-background-color: #4a90d9;
            selection-color: white;
            border: 1px solid #555;
        }
    )");
    connect(spectrumRefreshCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(onSpectrumRefreshIntervalChanged(int)));
    // ✅ 顶栏：左侧刷新率，右侧按钮（保持距离）
    QHBoxLayout *topBarLayout = new QHBoxLayout();
    topBarLayout->setContentsMargins(0, 0, 0, 0);
    topBarLayout->setSpacing(10);
    
    // 左边弹簧：把整体推向中间
    topBarLayout->addStretch();
    
    topBarLayout->addWidget(refreshLabel);
    topBarLayout->addWidget(spectrumRefreshCombo);
    
    // combo 和按钮之间保持距离
    topBarLayout->addSpacing(24);
    
    topBarLayout->addWidget(m_collectButtonTop);
    topBarLayout->addSpacing(12);
    topBarLayout->addWidget(m_displayButtonTop);
    
    // 右边弹簧：对称，保证居中
    topBarLayout->addStretch();
    
    topLayout->addLayout(topBarLayout);

    // 三个频谱图横向排列
    QHBoxLayout *spectrumLayout = new QHBoxLayout();
    spectrumLayout->setSpacing(5);

    spectrumWidgetNS = new SpectrumWidget("CH-A", "N-S", this);
    spectrumWidgetNS->setSampleRate(m_sampleRate);
    connect(spectrumWidgetNS, &SpectrumWidget::clicked, this, &MainWindow::onSpectrumWidgetClicked);

    spectrumWidgetEW = new SpectrumWidget("CH-B", "E-W", this);
    spectrumWidgetEW->setSampleRate(m_sampleRate);
    connect(spectrumWidgetEW, &SpectrumWidget::clicked, this, &MainWindow::onSpectrumWidgetClicked);

    spectrumWidgetTD = new SpectrumWidget("CH-C", "T-D", this);
    spectrumWidgetTD->setSampleRate(m_sampleRate);
    connect(spectrumWidgetTD, &SpectrumWidget::clicked, this, &MainWindow::onSpectrumWidgetClicked);

    spectrumLayout->addWidget(spectrumWidgetNS, 1);
    spectrumLayout->addWidget(spectrumWidgetEW, 1);
    spectrumLayout->addWidget(spectrumWidgetTD, 1);
    topLayout->addLayout(spectrumLayout, 1);

    displayLayout->addWidget(topArea, 1);
    spectrogramWidget = nullptr; 


    // 默认选中NS通道
    m_currentChannel = "CH-A";
    spectrumWidgetNS->setSelected(true);
    //m_specDialogWidget->setCurrentChannel("CH-A", "N-S");

    mainLayout->addWidget(displayArea, 1);

    // 创建状态栏
    QStatusBar *statusBarWidget = this->statusBar();


    // 右侧：模式和状态
    statusModeLabel = new QLabel("模式: 绘图模式");
    statusConnectionLabel = new QLabel("状态: 待机");
    statusConnectionLabel->setStyleSheet("color: gray;");
    if (infoPanel) infoPanel->enableAutoTimeUpdate(true);

    statusBarWidget->addPermanentWidget(statusModeLabel);
    statusBarWidget->addPermanentWidget(statusConnectionLabel);
}
void MainWindow::onSpectrumTripletReady(const SpectrumResult &ns,
    const SpectrumResult &ew,
    const SpectrumResult &td)
{
auto drawOne = [](const SpectrumResult &r, SpectrumWidget *w) {
if (!w || !r.isValid || r.freq.isEmpty() || r.spectrum.isEmpty())
return;

int F = r.freqCount;
int T = r.timeCount;
if (F <= 0 || T <= 0) return;

int offset = (T - 1) * F;            // 取最后一列
QVector<double> mag(F);
for (int i = 0; i < F; ++i) {
mag[i] = r.spectrum[offset + i];
}

w->setSpectrumData(r.freq, mag);
w->replot();
};

drawOne(ns, spectrumWidgetNS);
drawOne(ew, spectrumWidgetEW);
drawOne(td, spectrumWidgetTD);   // 如果暂时没有 TD，可以先注释掉这行
}
void MainWindow::setupMenus()
{
    QMenuBar *menuBar = this->menuBar();

    // ========== 控制菜单 ==========
    QMenu *controlMenu = menuBar->addMenu("控制(&T)");

    // 仅保留UDP客户端控制（程序默认客户端模式）
    startDualClientAction = controlMenu->addAction("启动接收");
    connect(startDualClientAction, &QAction::triggered, this, &MainWindow::onStartDualClientReceiving);

    stopDualClientAction = controlMenu->addAction("停止接收");
    connect(stopDualClientAction, &QAction::triggered, this, &MainWindow::onStopDualClientReceiving);


    // ========== 视图菜单 ==========
    QMenu *viewMenu = menuBar->addMenu("视图(&V)");

    toggleInfoPanelAction = viewMenu->addAction("显示信息面板(&I)");
    toggleInfoPanelAction->setCheckable(true);
    toggleInfoPanelAction->setChecked(true);
    connect(toggleInfoPanelAction, &QAction::triggered, this, &MainWindow::onToggleInfoPanel);

    autoClearPlotAction = viewMenu->addAction("自动清空图表(&A)");
    autoClearPlotAction->setCheckable(true);
    autoClearPlotAction->setChecked(false);  // 默认不自动清空
    connect(autoClearPlotAction, &QAction::triggered, this, &MainWindow::onAutoClearPlotToggled);

    viewMenu->addSeparator();

    // 截图功能
    QAction *captureScreenshotAction = viewMenu->addAction("截取频谱图(&S)");
    connect(captureScreenshotAction, &QAction::triggered, this, &MainWindow::onCaptureSpectrumScreenshots);


    // ========== 设置菜单 ==========
    QMenu *settingsMenu = menuBar->addMenu("设置(&S)");

    // 采样率设置（原：视图菜单）
    QMenu *sampleRateMenu = settingsMenu->addMenu("采样率设置(&R)");

    QActionGroup *sampleRateGroup = new QActionGroup(this);
    sampleRateGroup->setExclusive(true);

    QAction *sampleRate4MAction = sampleRateMenu->addAction("4 MHz");
    sampleRate4MAction->setCheckable(true);

    QAction *sampleRate250kAction = sampleRateMenu->addAction("250 kHz");
    sampleRate250kAction->setCheckable(true);

    sampleRateGroup->addAction(sampleRate4MAction);
    sampleRateGroup->addAction(sampleRate250kAction);

    // 默认选中（根据当前 m_sampleRate）
    if (m_sampleRate >= 1000000.0) sampleRate4MAction->setChecked(true);
    else sampleRate250kAction->setChecked(true);

    connect(sampleRate4MAction, &QAction::triggered, this, &MainWindow::setSampleRate4M);
    connect(sampleRate250kAction, &QAction::triggered, this, &MainWindow::setSampleRate250k);

    // 通道参数（原：视图菜单）
    QMenu *channelDisplayMenu = settingsMenu->addMenu("通道参数(&C)");

    channelEWAction = channelDisplayMenu->addAction("EW通道 (东西)");
    channelEWAction->setCheckable(true);
    channelEWAction->setChecked(true);
    connect(channelEWAction, &QAction::triggered, this, [this](bool checked) {
        m_channelEWEnabled = checked;
        updateChannelVisibility();
    });

    channelNSAction = channelDisplayMenu->addAction("NS通道 (南北)");
    channelNSAction->setCheckable(true);
    channelNSAction->setChecked(true);
    connect(channelNSAction, &QAction::triggered, this, [this](bool checked) {
        m_channelNSEnabled = checked;
        updateChannelVisibility();
    });

    channelTDAction = channelDisplayMenu->addAction("TD通道 (垂直)");
    channelTDAction->setCheckable(true);
    channelTDAction->setChecked(true);
    connect(channelTDAction, &QAction::triggered, this, [this](bool checked) {
        m_channelTDEnabled = checked;
        updateChannelVisibility();
    });

    settingsMenu->addSeparator();

    // 状态栏订阅周期（原：控制菜单）
    QMenu *statusSubscribeMenu = settingsMenu->addMenu("状态栏订阅周期(&B)");

    subscribeStatus1sAction = statusSubscribeMenu->addAction("1秒周期");
    connect(subscribeStatus1sAction, &QAction::triggered, this, &MainWindow::onSubscribeStatus1s);

    subscribeStatus5sAction = statusSubscribeMenu->addAction("5秒周期");
    connect(subscribeStatus5sAction, &QAction::triggered, this, &MainWindow::onSubscribeStatus5s);

    subscribeStatus10sAction = statusSubscribeMenu->addAction("10秒周期");
    connect(subscribeStatus10sAction, &QAction::triggered, this, &MainWindow::onSubscribeStatus10s);

    // ========== 帮助菜单 ==========
    QMenu *helpMenu = menuBar->addMenu("帮助(&H)");

    QAction *aboutAction = helpMenu->addAction("关于(&A)");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);

    updateMenusForMode();


}

void MainWindow::updateMenusForMode()
{
    // 程序默认客户端模式，仅保留“启动接收/停止接收”
    if (startDualClientAction) startDualClientAction->setEnabled(true);
    if (stopDualClientAction) stopDualClientAction->setEnabled(false);
}

void MainWindow::updateStatusBar()
{
    // 默认客户端模式
    statusConnectionLabel->setText("状态: 客户端待机");
    statusConnectionLabel->setStyleSheet("color: gray;");
}

void MainWindow::updateInfoPanel()
{
    // 信息面板的模式显示已移除，这里不再更新模式文案
}

void MainWindow::onOpenFile()
{
    QMessageBox::information(this, "提示", "当前版本默认客户端模式，不提供绘图/服务器模式文件打开。");
}

void MainWindow::onExit()
{
    close();
}

void MainWindow::setSampleRate250k()
{
    m_sampleRate = 250000.0;
    if (localFileReader) localFileReader->setSampleRate(m_sampleRate);
    if (spectrumWidgetNS) spectrumWidgetNS->setSampleRate(m_sampleRate);
    if (spectrumWidgetEW) spectrumWidgetEW->setSampleRate(m_sampleRate);
    if (spectrumWidgetTD) spectrumWidgetTD->setSampleRate(m_sampleRate);
    if (m_specDialogWidget) m_specDialogWidget->setSampleRate(m_sampleRate);
    statusBar()->showMessage(QString("采样率已切换为 250 kHz"));
}

void MainWindow::setSampleRate4M()
{
    m_sampleRate = 4000000.0;
    if (localFileReader) localFileReader->setSampleRate(m_sampleRate);
    if (spectrumWidgetNS) spectrumWidgetNS->setSampleRate(m_sampleRate);
    if (spectrumWidgetEW) spectrumWidgetEW->setSampleRate(m_sampleRate);
    if (spectrumWidgetTD) spectrumWidgetTD->setSampleRate(m_sampleRate);
    if (m_specDialogWidget) m_specDialogWidget->setSampleRate(m_sampleRate);
    statusBar()->showMessage(QString("采样率已切换为 4 MHz"));
}


void MainWindow::switchToUDPDualServerMode()
{
    currentMode = MODE_UDP_DUAL_SERVER;
    statusModeLabel->setText("模式: UDP双通道服务器");

    // 切换模式时清空图表
    if (m_specDialogWidget) {
        m_specDialogWidget->clearColorMap();
    }
// qDebug() << "[主窗口] 切换到UDP双通道服务器模式，已清空图表";

    updateMenusForMode();
    updateStatusBar();
    updateInfoPanel();
}

void MainWindow::switchToUDPDualClientMode()
{
    currentMode = MODE_UDP_DUAL_CLIENT;
    statusModeLabel->setText("模式: UDP双通道客户端");
    updateMenusForMode();
    updateStatusBar();
    updateInfoPanel();
}

void MainWindow::switchToLocalFileMode()
{
    currentMode = MODE_LOCAL_FILE;
    statusModeLabel->setText("模式: 绘图模式");
    updateMenusForMode();
    updateStatusBar();
    updateInfoPanel();
}

void MainWindow::onToggleInfoPanel()
{
    if (infoPanel->isVisible()) {
        infoPanel->hide();
        toggleInfoPanelAction->setChecked(false);
    } else {
        infoPanel->show();
        toggleInfoPanelAction->setChecked(true);
    }
}

void MainWindow::onAutoClearPlotToggled(bool checked)
{
    // 新架构：只有一个大瀑布图需要设置自动清空
    // 自动清空逻辑在onSpectrogramBatchReady中处理
// qDebug() << "[主窗口] 自动清空图表:" << (checked ? "开启" : "关闭");
}

void MainWindow::onCaptureSpectrumScreenshots()
{
    // 生成时间戳文件名
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");

    int savedCount = 0;

    // 截取NS频谱图
    if (spectrumWidgetNS && spectrumWidgetNS->isVisible()) {
        QString filename = QString("%1_ns.png").arg(timestamp);
        if (saveSpectrumWidgetScreenshot(spectrumWidgetNS, filename)) {
            savedCount++;
// qDebug() << "[截图] 已保存NS频谱图:" << filename;
        }
    }

    // 截取EW频谱图
    if (spectrumWidgetEW && spectrumWidgetEW->isVisible()) {
        QString filename = QString("%1_ew.png").arg(timestamp);
        if (saveSpectrumWidgetScreenshot(spectrumWidgetEW, filename)) {
            savedCount++;
// qDebug() << "[截图] 已保存EW频谱图:" << filename;
        }
    }

    // 截取TD频谱图
    if (spectrumWidgetTD && spectrumWidgetTD->isVisible()) {
        QString filename = QString("%1_td.png").arg(timestamp);
        if (saveSpectrumWidgetScreenshot(spectrumWidgetTD, filename)) {
            savedCount++;
// qDebug() << "[截图] 已保存TD频谱图:" << filename;
        }
    }

    // 显示提示信息
    if (savedCount > 0) {
        statusConnectionLabel->setText(QString("状态: 已保存 %1 张频谱图截图").arg(savedCount));
        statusConnectionLabel->setStyleSheet("color: green;");
// qDebug() << "[截图] 成功保存" << savedCount << "张频谱图截图";
    } else {
        statusConnectionLabel->setText("状态: 没有可见的频谱图");
        statusConnectionLabel->setStyleSheet("color: orange;");
// qWarning() << "[截图] 没有可见的频谱图可以截取";
    }
}

bool MainWindow::saveSpectrumWidgetScreenshot(SpectrumWidget* widget, const QString& filename)
{
    if (!widget) return false;

    // 获取widget的截图（包含整个widget，包括坐标轴）
    QPixmap pixmap = widget->grab();

    // 保存到当前目录
    bool success = pixmap.save(filename, "PNG");

    if (!success) {
// qWarning() << "[截图] 保存失败:" << filename;
    }

    return success;
}

// ✅ 更新通道显示/隐藏
void MainWindow::updateChannelVisibility()
{
    // 新架构：控制三个小频谱图的显示/隐藏
    if (spectrumWidgetEW) {
        spectrumWidgetEW->setVisible(m_channelEWEnabled);
    }
    if (spectrumWidgetNS) {
        spectrumWidgetNS->setVisible(m_channelNSEnabled);
    }
    if (spectrumWidgetTD) {
        spectrumWidgetTD->setVisible(m_channelTDEnabled);
    }

    // 通知LocalFileReader当前启用的通道
    if (localFileReader) {
        localFileReader->setEnabledChannels(m_channelEWEnabled, m_channelNSEnabled, m_channelTDEnabled);
    }

// qDebug() << "[主窗口] 通道显示状态 - EW:" << m_channelEWEnabled
    //              << "NS:" << m_channelNSEnabled
    //              << "TD:" << m_channelTDEnabled;
}

// 从视图菜单获取通道配置字符串
QString MainWindow::getChannelConfigFromView()
{
    // 通道配置格式：第一位=EW，第二位=NS，第三位=TD
    // 例如："110" = EW+NS启用，TD禁用
    QString config;
    config += m_channelEWEnabled ? '1' : '0';
    config += m_channelNSEnabled ? '1' : '0';
    config += m_channelTDEnabled ? '1' : '0';

// qDebug() << "[主窗口] 从视图菜单获取通道配置:" << config;
    return config;
}

// ========== 绘图文件模式 ==========



void MainWindow::onOpenLocalFile()
{
    QString filePath = QFileDialog::getOpenFileName(
        this,
        "打开数据文件",
        "",
        "Data Files (*.bin *.dat );;COS Files (*.bin);;DAT Files (*.dat);;All Files (*)"
    );

    if (filePath.isEmpty()) {
        return;
    }

    currentFilePath = filePath;

    // 启动本地文件处理线程（LocalFileReader在子线程中打开/读取/FFT）
    if (!localFileThread) {
        localFileThread = new QThread(this);
        localFileReader->moveToThread(localFileThread);
        localFileThread->start();
    }

    // 让LocalFileReader在它自己的线程里打开文件（成功/失败通过信号回传）
    QMetaObject::invokeMethod(localFileReader, "openFileAsync",
                              Qt::QueuedConnection,
                              Q_ARG(QString, filePath));

    statusBar()->showMessage("已打开: " + filePath);
}



void MainWindow::onLocalFileOpened(const QString &fileName, int totalFrames)
{
    // 录制文件信息显示已移除
// qDebug() << "[主窗口] 绘图文件已打开:" << fileName << ", 总帧数:" << totalFrames;
}

void MainWindow::onStartLocalProcessing()
{
    if (!localFileReader || localFileReader->filePath().isEmpty()) {
// qWarning() << "[警告] 请先打开 COS 文件";
        return;
    }

    // 重置时间槽索引
    currentTimeSlot = 0;

    // 初始化colorMap大小
    // 根据文件大小/通道数/采样率动态计算总时长，并让X轴正好铺满（文件时长固定，显示列数固定以提升速度）
    const double durationSec = localFileReader->recordingDurationSeconds();

    // 固定显示 2000 列（避免 4MHz 时列数暴涨导致绘制/处理极慢）
    const int timeSlots = 2000;

    // 反推一个分析步进（样本点），让 FFT 段数大致落在 timeSlots 附近
    qint64 fileBytes = localFileReader->fileSizeBytes();
    if (fileBytes <= 0) {
        QFileInfo fi(localFileReader->filePath());
        fileBytes = fi.exists() ? fi.size() : 0;
    }

    // 计算启用的通道数
    int channels = 0;
    if (m_channelEWEnabled) channels++;
    if (m_channelNSEnabled) channels++;
    if (m_channelTDEnabled) channels++;

    const qint64 samplesPerChannel = (channels > 0) ? (fileBytes / (channels * 3LL)) : 0; // 3字节/通道

    int analysisHop = FFTProcessor::hopSize(); // 默认 2048
    const int fftSize = 4096;
    if (samplesPerChannel > fftSize && timeSlots > 1) {
        const qint64 numerator = samplesPerChannel - fftSize;
        analysisHop = static_cast<int>((numerator + (timeSlots - 2)) / (timeSlots - 1)); // ceil
        analysisHop = qMax(analysisHop, FFTProcessor::hopSize());
    }
    localFileReader->setAnalysisHop(analysisHop);

    // ✅ 保存hop_size用于位置计算
    m_currentHopSize = analysisHop;
// qDebug() << "[主窗口] 本地文件模式 - hop_size:" << m_currentHopSize;
// qDebug() << "[主窗口] 文件样本数:" << samplesPerChannel << "，时间槽数:" << timeSlots;

    int freqCount = 2048;
    m_timeSlots = timeSlots;

    // 动态设置时间轴范围（只需设置大瀑布图）
    if (m_specDialogWidget) {
        m_specDialogWidget->setDisplayTimeRange(durationSec);
        m_specDialogWidget->initializeColorMapSize(timeSlots, freqCount);

        // ✅ 在初始化完成后更新时间轴起始时间（使用当前时间）
        QDateTime startTime = QDateTime::currentDateTime();
        m_specDialogWidget->updateTimeAxis(startTime);
// qDebug() << "[主窗口] 本地文件模式 - 时间轴起始时间:" << startTime.toString("yyyy-MM-dd HH:mm:ss");
    }
    colorMapInitialized = true;

    // 开始处理（在LocalFileReader线程中）
    QMetaObject::invokeMethod(localFileReader, "startProcessing", Qt::QueuedConnection);

    // 启动子线程绘图数据准备器（主线程仅负责绘制）
    if (plotThread) {
        if (plotWorker) {
            QMetaObject::invokeMethod(plotWorker, "stop", Qt::QueuedConnection);
        }
        plotThread->quit();
        plotThread->wait();
        plotThread->deleteLater();
        plotThread = nullptr;
        plotWorker = nullptr;
    }
    plotThread = new QThread(this);
    plotWorker = new PlotWorker();
    plotWorker->setBuffers(&localFileReader->getNSFFTBuffer(), &localFileReader->getEWFFTBuffer(), &localFileReader->getTDFFTBuffer());
    plotWorker->setTimeParameters(durationSec, timeSlots);  // ✅ 设置时间参数
    plotWorker->reset(0);
    plotWorker->moveToThread(plotThread);
    connect(plotThread, &QThread::started, plotWorker, &PlotWorker::start);
    connect(plotThread, &QThread::finished, plotWorker, &QObject::deleteLater);
    connect(plotWorker, &PlotWorker::batchReady, this, &MainWindow::onSpectrogramBatchReady, Qt::QueuedConnection);
    plotThread->start();

    // 启动瀑布图定时刷新（50ms，只负责replot，数据由PlotWorker准备）
    fftPlotTimer->stop();
    disconnect(fftPlotTimer, &QTimer::timeout, nullptr, nullptr);  // 断开旧连接
    connect(fftPlotTimer, &QTimer::timeout, this, &MainWindow::updateLocalFFTPlots);  // 连接到本地文件刷新函数
    fftPlotTimer->start(50);  // 流式处理：每50ms刷新一次瀑布图，保证流畅

    // ✅ 初始化基于位置的频谱刷新参数
    int refreshIntervalMs = spectrumRefreshCombo->itemData(spectrumRefreshCombo->currentIndex()).toInt();
    m_spectrumRefreshInterval = refreshIntervalMs / 1000.0;  // 转换为秒
    m_nextSpectrumRefreshTime = m_spectrumRefreshInterval;   // 第一个刷新点
    m_spectrumRefreshCount = 0;  // 重置计数器
// qDebug() << "[主窗口] 基于位置的频谱刷新已启动，间隔:" << m_spectrumRefreshInterval << "秒";

    // 频谱图定时器已废弃，改为基于位置刷新
    // int refreshInterval = spectrumRefreshCombo->itemData(spectrumRefreshCombo->currentIndex()).toInt();
    // spectrumPlotTimer->start(refreshInterval);

    startLocalAction->setEnabled(false);
    pauseLocalAction->setEnabled(true);
    stopLocalAction->setEnabled(true);
    updateStatusBar();
    updateInfoPanel();

// qDebug() << "[主窗口] 绘图文件处理已启动（线程化）";
}



void MainWindow::onStopLocalProcessing()
{
    if (!localFileReader) return;

    QMetaObject::invokeMethod(localFileReader, "stopProcessing", Qt::QueuedConnection);

    fftPlotTimer->stop();
    // spectrumPlotTimer->stop();  // 已废弃，改为基于位置刷新

    // 停止子线程绘图数据准备器
    if (plotWorker) {
        QMetaObject::invokeMethod(plotWorker, "stop", Qt::QueuedConnection);
    }
    if (plotThread) {
        plotThread->quit();
        plotThread->wait();
        plotThread->deleteLater();
        plotThread = nullptr;
        plotWorker = nullptr;
    }

    startLocalAction->setEnabled(true);
    pauseLocalAction->setEnabled(false);
    stopLocalAction->setEnabled(false);
    updateStatusBar();
    updateInfoPanel();

// qDebug() << "[主窗口] 绘图文件处理已停止（线程化）";
}


void MainWindow::onPauseLocalProcessing()
{
    if (!localFileReader) return;

    if (localFileReader->isPaused()) {
        localFileReader->resumeProcessing();
        pauseLocalAction->setText("暂停");
    } else {
        localFileReader->pauseProcessing();
        pauseLocalAction->setText("继续");
    }
    updateStatusBar();
    updateInfoPanel();
}

void MainWindow::onLocalProgressChanged(double percent)
{
    // 录制进度显示已移除

    // 更新状态栏显示当前时长
    if (localFileReader) {
        double currentSec = (double)localFileReader->currentFrame() * 5000 / m_sampleRate;
        int hours = (int)(currentSec / 3600);
        int minutes = (int)((currentSec - hours * 3600) / 60);
        int seconds = (int)(currentSec - hours * 3600 - minutes * 60);
        QString currentDuration = QString("%1:%2:%3")
                                      .arg(hours, 2, 10, QChar('0'))
                                      .arg(minutes, 2, 10, QChar('0'))
                                      .arg(seconds, 2, 10, QChar('0'));
        statusBar()->showMessage(QString("处理进度: %1% | 时长: %2").arg(percent, 0, 'f', 1).arg(currentDuration));
    }
}

void MainWindow::onLocalProcessingFinished()
{
    fftPlotTimer->stop();
    // spectrumPlotTimer->stop();  // 已废弃

    // ✅ 播放结束时强制最后一次频谱刷新（确保与瀑布图同步）
// qDebug() << "[主窗口] 播放结束，强制最后一次频谱刷新";
    updateSpectrumPlots();

    startLocalAction->setEnabled(true);
    pauseLocalAction->setEnabled(false);
    stopLocalAction->setEnabled(false);
    pauseLocalAction->setText("暂停/继续");
    updateStatusBar();
    updateInfoPanel();

    statusConnectionLabel->setText("状态: 处理完成");
    statusConnectionLabel->setStyleSheet("color: blue;");

// qDebug() << "[主窗口] 绘图文件处理完成";
}

// void MainWindow::updateLocalFFTPlots()
// {
//     if (!localFileReader) return;

//     RingBuffer<SpectrumResult>& nsBuffer = localFileReader->getNSFFTBuffer();
//     RingBuffer<SpectrumResult>& ewBuffer = localFileReader->getEWFFTBuffer();

//     if (nsBuffer.isEmpty() || ewBuffer.isEmpty()) {
//         return;
//     }

//     SpectrumResult nsResult;
//     if (!nsBuffer.tryPop(nsResult)) {
//         return;
//     }

//     SpectrumResult ewResult;
//     if (!ewBuffer.tryPop(ewResult)) {
//         nsBuffer.push(std::move(nsResult));
//         return;
//     }

//     if (!nsResult.isValid || !ewResult.isValid || (hasTDResult && !tdResult.isValid)) {
//         return;
//     }

//     // 更新NS通道瀑布图数据（使用新接口，自动处理线性/对数映射）
//     for (int t = 0; t < nsResult.timeCount; ++t) {
//         int timeSlot = (currentTimeSlot + t) ;
//         QVector<double> columnSpectrum(nsResult.freqCount);
//         for (int f = 0; f < nsResult.freqCount; ++f) {
//             int index = f * nsResult.timeCount + t;
//             columnSpectrum[f] = nsResult.spectrum[index];
//         }
//         monitorWidgetNS->setColorMapColumn(timeSlot, nsResult.freq, columnSpectrum);
//     }

//     // 更新EW通道瀑布图数据
//     for (int t = 0; t < ewResult.timeCount; ++t) {
//         int timeSlot = (currentTimeSlot + t) ;
//         QVector<double> columnSpectrum(ewResult.freqCount);
//         for (int f = 0; f < ewResult.freqCount; ++f) {
//             int index = f * ewResult.timeCount + t;
//             columnSpectrum[f] = ewResult.spectrum[index];
//         }
//         monitorWidgetEW->setColorMapColumn(timeSlot, ewResult.freq, columnSpectrum);
//     }

//     // 记录最后绘制的时间槽对应的时间戳（文件回放模式）
//     if (nsResult.timestamp.isValid()) {
//         lastDrawnTimestamp = nsResult.timestamp;
//     }

//     // 更新右侧实时频谱图数据（使用最后一列数据，由各自定时器独立刷新）
//     if (nsResult.timeCount > 0 && nsResult.freqCount > 0) {
//         QVector<double> lastNSSpectrum(nsResult.freqCount);
//         QVector<double> lastEWSpectrum(ewResult.freqCount);
//         int lastT = nsResult.timeCount - 1;

//         for (int f = 0; f < nsResult.freqCount; ++f) {
//             lastNSSpectrum[f] = nsResult.spectrum[f * nsResult.timeCount + lastT];
//         }
//         for (int f = 0; f < ewResult.freqCount; ++f) {
//             lastEWSpectrum[f] = ewResult.spectrum[f * ewResult.timeCount + lastT];
//         }

//         // 只更新数据，不直接刷新（频谱图由100ms定时器统一刷新）
//         monitorWidgetNS->setSpectrumData(nsResult.freq, lastNSSpectrum);
//         monitorWidgetEW->setSpectrumData(ewResult.freq, lastEWSpectrum);
//     }
//     currentTimeSlot += nsResult.timeCount;   // 只累加，不做任何“回绕检测/时间戳推算”

//     // int oldTimeSlot = currentTimeSlot;
//     // currentTimeSlot = (currentTimeSlot + nsResult.timeCount);

//     // // 检测时间槽回绕（画完一轮），如果启用了自动清空，则清空图表
//     // if (oldTimeSlot > currentTimeSlot) {
//     //     // 发生了回绕，说明画完了一轮（从0到999）
//     //     if (autoClearPlotAction && autoClearPlotAction->isChecked()) {
//     //         monitorWidgetNS->clearColorMap();
//     //         monitorWidgetEW->clearColorMap();
//     //         qDebug() << "[主窗口-本地文件] 检测到时间槽回绕，自动清空图表";
//     //     }
//     // }

//     // // 🔍 详细诊断：打印时间槽和时间戳信息
//     // qDebug() << "[时间轴诊断]"
//     //          << "oldTimeSlot=" << oldTimeSlot
//     //          << "currentTimeSlot=" << currentTimeSlot
//     //          << "timeCount=" << nsResult.timeCount
//     //          << "nsResult.timestamp=" << nsResult.timestamp.toString("HH:mm:ss.zzz")
//     //          << "lastDrawnTimestamp=" << lastDrawnTimestamp.toString("HH:mm:ss.zzz");

//     // // 检测第一个数据包到达：更新时间轴为第一个数据包的起始时间
//     // if (oldTimeSlot == 0 && nsResult.timestamp.isValid()) {
//     //     // 计算第一个时间点的时间戳（nsResult.timestamp是最后一个时间点）
//     //     // 每个时间点间隔 = 10秒 / 1000个时间槽 = 0.01秒 = 10毫秒
//     //     int msOffset = (nsResult.timeCount - 1) * 10;
//     //     QDateTime firstTimestamp = nsResult.timestamp.addMSecs(-msOffset);
//     //     monitorWidgetNS->updateTimeAxis(firstTimestamp);
//     //     monitorWidgetEW->updateTimeAxis(firstTimestamp);
        //if (monitorWidgetTD) monitorWidgetTD->updateTimeAxis(firstTimestamp);
//     //     qDebug() << "[文件模式] 第一次绘制 - 时间轴起始=" << firstTimestamp.toString("yyyy-MM-dd HH:mm:ss.zzz");
//     // }
//     // // 检测是否循环回到开头（填满1000个时间槽后）
//     // else if (oldTimeSlot > currentTimeSlot) {
//     //     // ✅ 使用 lastDrawnTimestamp（与副本项目一致）
//     //     if (lastDrawnTimestamp.isValid()) {
//     //         monitorWidgetNS->updateTimeAxis(lastDrawnTimestamp);
//     //         monitorWidgetEW->updateTimeAxis(lastDrawnTimestamp);
//     //         qDebug() << "[文件模式] 循环检测 - 时间轴起始=" << lastDrawnTimestamp.toString("yyyy-MM-dd HH:mm:ss.zzz");
//     //     } else {
//     //         // 回退方案：使用当前时间
//     //         QDateTime currentTime = QDateTime::currentDateTime();
//     //         monitorWidgetNS->updateTimeAxis(currentTime);
//     //         monitorWidgetEW->updateTimeAxis(currentTime);
//     //         qDebug() << "[文件模式] 时间轴更新为当前时间:" << currentTime.toString("yyyy-MM-dd HH:mm:ss.zzz");
//     //     }
//     // }

//     // 两个瀑布图同时刷新
//     monitorWidgetNS->replotSpectrogram();
//     monitorWidgetEW->replotSpectrogram();
// }
void MainWindow::updateLocalFFTPlots()
{
    // 流式处理：定时器只负责replot，数据写入由onSpectrogramBatchReady完成
    // 这样实现真正的流式显示，数据不断写入，定时器定期刷新显示
    if (m_specDialogWidget && m_specDialog && m_specDialog->isVisible()) {
        m_specDialogWidget->replot();
    }
}


// ========== UDP服务器模式 ==========

void MainWindow::onOpenCosFileDualServer()
{
    QString filePath = QFileDialog::getOpenFileName(
        this,
        "打开 COS 文件",
        "",
        "COS Files (*.cos);;All Files (*)"
    );

    if (filePath.isEmpty()) {
        return;
    }

    currentFilePath = filePath;
    if (!udpDualServer->openCosFile(filePath)) {
// qCritical() << "[文件打开失败]" << filePath;
        return;
    }

    statusBar()->showMessage("已打开: " + filePath);
}

void MainWindow::onStartDualServerSending()
{
    if (currentFilePath.isEmpty()) {
// qWarning() << "[警告] 请先打开 COS 文件";
        return;
    }

    if (!udpDualServer->initialize(45454)) {
        return;
    }

    udpDualServer->setClientAddress(QHostAddress::LocalHost, 45456);
    udpDualServer->startSending();
    serverRunning = true;

    startDualServerAction->setEnabled(false);
    stopDualServerAction->setEnabled(true);
    updateStatusBar();
    updateInfoPanel();

// qDebug() << "[主窗口] UDP双通道服务器已启动";
}

void MainWindow::onStopDualServerSending()
{
    udpDualServer->stopSending();
    serverRunning = false;

    startDualServerAction->setEnabled(true);
    stopDualServerAction->setEnabled(false);
    updateStatusBar();
    updateInfoPanel();

// qDebug() << "[主窗口] UDP双通道服务器已停止";
}

// ========== UDP客户端模式 ==========

void MainWindow::onStartDualClientReceiving()
{
    // 以下在接收管线线程执行，与主线程绘制解耦
    QMetaObject::invokeMethod(udpDualClient, "setUseNewPacketFormat", Qt::BlockingQueuedConnection, Q_ARG(bool, true));
    QMetaObject::invokeMethod(udpDualClient, "setChannelConfig", Qt::BlockingQueuedConnection, Q_ARG(QString, getChannelConfigFromView()));
    QMetaObject::invokeMethod(udpDualClient, "setSampleRate", Qt::BlockingQueuedConnection, Q_ARG(double, m_sampleRate));
    bool initOk = false;
    QMetaObject::invokeMethod(udpDualClient, "initialize", Qt::BlockingQueuedConnection, Q_RETURN_ARG(bool, initOk), Q_ARG(quint16, (quint16)45456));
    if (!initOk) {
        return;
    }

    currentTimeSlot = 0;
    // ✅ 不在启动时设置时间轴，等待第一个数据包的真实时间戳
    // 时间轴会在 onSpectrogramBatchReady() 中首次收到有效时间戳时自动更新

	int freqCount = 2048;

	// ✅ 确保 hopSize 有值（UDP模式下你当前代码没有给 m_currentHopSize 赋值）
	// 用 FFTProcessor 默认 hopSize（你工程里一般是 2048）
	if (m_currentHopSize <= 0) {
		m_currentHopSize = FFTProcessor::hopSize();
	}

	// 固定 10 秒窗口
	const double displaySec = 10.0;

	// dt=每列对应秒数
	double dt = static_cast<double>(m_currentHopSize) / m_sampleRate;

	// 防御：避免 dt=0 或异常
	if (dt <= 0.0) {
		dt = 2048.0 / 4000000.0; // 兜底：按 4MHz/2048
	}

	// 计算 10 秒需要的列数，并做合理范围钳制
	int timeSlots = static_cast<int>(displaySec / dt);
	if (timeSlots < 64) timeSlots = 64;        // 太小会不好看/不好用
	if (timeSlots > 4096) timeSlots = 4096;    // 太大性能会炸

	m_timeSlots = timeSlots;

	// ✅ 只初始化一次
	if (m_specDialogWidget) {
		m_specDialogWidget->setDisplayTimeRange(displaySec);
		m_specDialogWidget->initializeColorMapSize(timeSlots, freqCount);
	}

	colorMapInitialized = true;

    QMetaObject::invokeMethod(udpDualClient, "startReceiving", Qt::QueuedConnection);
    // 启动子线程绘图数据准备器（主线程仅负责绘制）
    if (plotThread) {
        // 防止重复启动
        plotThread->quit();
        plotThread->wait();
        plotThread->deleteLater();
        plotThread = nullptr;
        plotWorker = nullptr;
    }
    // plotThread = new QThread(this);
    // plotWorker = new PlotWorker();
    // plotWorker->setBuffers(&udpDualClient->getNSFFTBuffer(), &udpDualClient->getEWFFTBuffer(), &udpDualClient->getTDFFTBuffer());
    // plotWorker->setTimeParameters(displaySec, timeSlots);  // ✅ 设置时间参数
    // plotWorker->reset(0);
    // plotWorker->moveToThread(plotThread);
    // connect(plotThread, &QThread::started, plotWorker, &PlotWorker::start);
    // connect(plotThread, &QThread::finished, plotWorker, &QObject::deleteLater);
    // connect(plotWorker, &PlotWorker::batchReady, this, &MainWindow::onSpectrogramBatchReady, Qt::QueuedConnection);
    // plotThread->start();

    // 停止旧的主线程瀑布图定时器（避免在GUI线程做数据整理）
    disconnect(fftPlotTimer, &QTimer::timeout, nullptr, nullptr);  // 断开旧连接
    // connect(fftPlotTimer, &QTimer::timeout, this, &MainWindow::updateSpectrogramPlots);  // 连接到瀑布图重绘函数
    // fftPlotTimer->start(50);  // 每50ms重绘一次瀑布图
    fftPlotTimer->stop();
    // ✅ 初始化基于位置的频谱刷新参数
    int refreshIntervalMs = spectrumRefreshCombo->itemData(spectrumRefreshCombo->currentIndex()).toInt();
    m_spectrumRefreshInterval = refreshIntervalMs / 1000.0;  // 转换为秒
    m_nextSpectrumRefreshTime = m_spectrumRefreshInterval;   // 第一个刷新点
    m_spectrumRefreshCount = 0;  // 重置计数器
// qDebug() << "[主窗口] 基于位置的频谱刷新已启动，间隔:" << m_spectrumRefreshInterval << "秒";

    // 频谱图定时器已废弃，改为基于位置刷新
    // int refreshInterval = spectrumRefreshCombo->itemData(spectrumRefreshCombo->currentIndex()).toInt();
    // spectrumPlotTimer->start(refreshInterval);

    startDualClientAction->setEnabled(false);
    stopDualClientAction->setEnabled(true);
    statusConnectionLabel->setText("状态: 客户端接收中");
    statusConnectionLabel->setStyleSheet("color: green;");

// qDebug() << "[主窗口] UDP双通道客户端已启动";
	connect(udpDualClient, &UDPClientReceiver::packetConfigChanged,
			this, &MainWindow::onPacketConfigChanged,
			Qt::QueuedConnection);
    // 连接绘制信号
    connect(udpDualClient, &UDPClientReceiver::drawableSpectrumReady,
        this, &MainWindow::onDrawableSpectrumReady, Qt::QueuedConnection);

    // 清空频谱二维矩阵，按界面刷新频率定时取一帧绘制（与老代码一致）
    m_spectrumMatrixNS.clear();
    m_spectrumMatrixEW.clear();
    m_spectrumMatrixTD.clear();
    m_spectrumMatrixFreq.clear();
    // 重置统计计数器
    m_totalDrawCount = 0;
    m_emptyQueueCount = 0;
    
    // 启动绘制定时器（间隔由界面“刷新频率”决定，见 onSpectrumRefreshIntervalChanged）
    m_spectrumDrawTimer->start();
    
    qDebug() << "[MainWindow] 频谱绘制定时器已启动，刷新间隔:" << m_drawRefreshInterval << "ms";
}
void MainWindow::onDrawableSpectrumReady(quint64 frameIndex)
{
    // 可选：记录日志或更新UI状态
    // qDebug() << "[MainWindow] 收到第" << frameIndex << "帧的绘制通知";
}
void MainWindow::onDrawTimerTimeout()
{
    if (!udpDualClient || !udpDualClient->isReceiving()) {
        return;
    }

    // 1) 排空绘制队列，把每批 DrawableSpectrum 按时间列追加到二维矩阵（与老代码一致：存时间×频率）
    auto appendResultToMatrix = [this](const SpectrumResult &r, QVector<QVector<double>> &matrix) {
        if (!r.isValid || r.freq.isEmpty() || r.spectrum.isEmpty()) return;
        int F = r.freqCount;
        int T = r.timeCount;
        if (F <= 0 || T <= 0) return;
        const QVector<double> &src = r.spectrum;
        for (int t = 0; t < T; ++t) {
            QVector<double> col(F);
            int offset = t * F;
            for (int i = 0; i < F; ++i)
                col[i] = src[offset + i];
            matrix.append(col);
            while (matrix.size() > m_spectrumMatrixMaxColumns)
                matrix.removeFirst();
        }
    };

    UDPClientReceiver::DrawableSpectrum drawable;
    while (udpDualClient->popDrawableSpectrum(drawable)) {
        if (drawable.hasNS) {
            appendResultToMatrix(drawable.nsSpectrum, m_spectrumMatrixNS);
            if (m_spectrumMatrixFreq.isEmpty() && !drawable.nsSpectrum.freq.isEmpty())
                m_spectrumMatrixFreq = drawable.nsSpectrum.freq;
        }
        if (drawable.hasEW) {
            appendResultToMatrix(drawable.ewSpectrum, m_spectrumMatrixEW);
            if (m_spectrumMatrixFreq.isEmpty() && !drawable.ewSpectrum.freq.isEmpty())
                m_spectrumMatrixFreq = drawable.ewSpectrum.freq;
        }
        if (drawable.hasTD) {
            appendResultToMatrix(drawable.tdSpectrum, m_spectrumMatrixTD);
            if (m_spectrumMatrixFreq.isEmpty() && !drawable.tdSpectrum.freq.isEmpty())
                m_spectrumMatrixFreq = drawable.tdSpectrum.freq;
        }
    }

    // 2) 按界面刷新频率定时取一帧：取二维矩阵的最后一列（最新一帧）绘制
    if (m_spectrumMatrixFreq.isEmpty()) return;

    if (!m_spectrumMatrixNS.isEmpty() && spectrumWidgetNS) {
        spectrumWidgetNS->setSpectrumData(m_spectrumMatrixFreq, m_spectrumMatrixNS.last());
        spectrumWidgetNS->replot();
    }
    if (!m_spectrumMatrixEW.isEmpty() && spectrumWidgetEW) {
        spectrumWidgetEW->setSpectrumData(m_spectrumMatrixFreq, m_spectrumMatrixEW.last());
        spectrumWidgetEW->replot();
    }
    if (!m_spectrumMatrixTD.isEmpty() && spectrumWidgetTD) {
        spectrumWidgetTD->setSpectrumData(m_spectrumMatrixFreq, m_spectrumMatrixTD.last());
        spectrumWidgetTD->replot();
    }
    m_totalDrawCount++;
}
// void MainWindow::onDrawTimerTimeout()
// {
//     if (!udpDualClient || !udpDualClient->isReceiving()) {
//         return;
//     }
    
//     // 从UDPClientReceiver的绘制队列取数据
//     UDPClientReceiver::DrawableSpectrum drawable;
//     if (!udpDualClient->popDrawableSpectrum(drawable)) {
//         m_emptyQueueCount++;
//         if (m_emptyQueueCount % 100 == 0) {
//             qDebug() << "[绘制定时器] 队列为空，累计空队列次数:" << m_emptyQueueCount 
//                      << "成功绘制次数:" << m_totalDrawCount;
//         }
//         return;  // 队列为空，无数据可绘制
//     }
    
//     // 绘制EW通道
//     if (drawable.hasEW && drawable.ewSpectrum.isValid && spectrumWidgetEW) {
//         int F = drawable.ewSpectrum.freqCount;
//         int T = drawable.ewSpectrum.timeCount;
//         if (T > 0 && F > 0) {
//             QVector<double> freqData = drawable.ewSpectrum.freq;
//             QVector<double> specData(F, -1e9);  // 初始化很小值
    
//             const QVector<double> &src = drawable.ewSpectrum.spectrum;
//             // 对这一批 T 帧做 max，突出尖刺
//             for (int t = 0; t < T; ++t) {
//                 int offset = t * F;
//                 for (int i = 0; i < F; ++i) {
//                     specData[i] = std::max(specData[i], src[offset + i]);
//                 }
//             }
    
//             spectrumWidgetEW->setSpectrumData(freqData, specData);
//             spectrumWidgetEW->replot();
//         }
//     }
    
//     // 绘制NS通道
//     if (drawable.hasNS && drawable.nsSpectrum.isValid && spectrumWidgetNS) {
//         int F = drawable.nsSpectrum.freqCount;
//         int T = drawable.nsSpectrum.timeCount;
//         if (T > 0 && F > 0) {
//             QVector<double> freqData = drawable.nsSpectrum.freq;
//             QVector<double> specData(F, -1e9);
    
//             const QVector<double> &src = drawable.nsSpectrum.spectrum;
//             for (int t = 0; t < T; ++t) {
//                 int offset = t * F;
//                 for (int i = 0; i < F; ++i) {
//                     specData[i] = std::max(specData[i], src[offset + i]);
//                 }
//             }
    
//             spectrumWidgetNS->setSpectrumData(freqData, specData);
//             spectrumWidgetNS->replot();
//         }
//     }
    
//     // 绘制TD通道
//     if (drawable.hasTD && drawable.tdSpectrum.isValid && spectrumWidgetTD) {
//         int freqCount = drawable.tdSpectrum.freqCount;
//         int timeCount = drawable.tdSpectrum.timeCount;
        
//         if (timeCount > 0 && freqCount > 0) {
//             QVector<double> freqData = drawable.tdSpectrum.freq;
//             QVector<double> specData(freqCount);
//             int lastColOffset = (timeCount - 1) * freqCount;
            
//             for (int i = 0; i < freqCount; i++) {
//                 specData[i] = drawable.tdSpectrum.spectrum[lastColOffset + i];
//             }
            
//             spectrumWidgetTD->setSpectrumData(freqData, specData);
//             spectrumWidgetTD->replot();
//         }
//     }
    
//     // 统计成功绘制次数
//     m_totalDrawCount++;
    
//     // 每10次成功绘制打印一次统计信息
//     if (m_totalDrawCount % 10 == 0) {
//         qDebug() << "[绘制统计] 成功绘制:" << m_totalDrawCount << "次"
//                  << "| 当前帧索引:" << drawable.frameIndex
//                  << "| EW:" << (drawable.hasEW ? "有" : "无")
//                  << "| NS:" << (drawable.hasNS ? "有" : "无")
//                  << "| TD:" << (drawable.hasTD ? "有" : "无")
//                  << "| 空队列次数:" << m_emptyQueueCount;
//     }
    
//     // 可选：更新信息面板显示当前帧索引
//     // infoPanel->setCurrentFrameIndex(drawable.frameIndex);
// }
void MainWindow::onStopDualClientReceiving()
{
        // 停止绘制定时器
        if (m_spectrumDrawTimer) {
            m_spectrumDrawTimer->stop();
        }
    QMetaObject::invokeMethod(udpDualClient, "stopReceiving", Qt::BlockingQueuedConnection);
    fftPlotTimer->stop();
    // spectrumPlotTimer->stop();  // 已废弃，改为基于位置刷新

    // ✅ 停止时强制最后一次频谱刷新（确保显示最新数据）
// qDebug() << "[主窗口] UDP接收停止，强制最后一次频谱刷新";
    updateSpectrumPlots();

    // 停止子线程绘图数据准备器
    if (plotWorker) {
        QMetaObject::invokeMethod(plotWorker, "stop", Qt::QueuedConnection);
    }
    if (plotThread) {
        plotThread->quit();
        plotThread->wait();
        plotThread->deleteLater();
        plotThread = nullptr;
        plotWorker = nullptr;
    }

    startDualClientAction->setEnabled(true);
    stopDualClientAction->setEnabled(false);
    statusConnectionLabel->setText("状态: 已停止");
    statusConnectionLabel->setStyleSheet("color: gray;");

// qDebug() << "[主窗口] UDP双通道客户端已停止";
}

// void MainWindow::onSaveSTFTData()
// {
//     // 生成默认的输出目录和文件名
//     QString outputDir = QDir::currentPath() + "/STFT_Output";
//     QString fileBaseName;

//     bool success = false;

//     // 根据当前模式调用不同的保存逻辑
//     if (currentMode == MODE_LOCAL_FILE) {
//         if (!localFileReader) {
//             qWarning() << "[主窗口] 本地文件读取器未初始化";
//             return;
//         }

//         // 使用当前打开的文件名作为基础名
//         QString fileName = localFileReader->recordingFileName();
//         if (fileName.isEmpty()) {
//             fileBaseName = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
//         } else {
//             // 移除文件扩展名
//             QFileInfo fileInfo(fileName);
//             fileBaseName = fileInfo.completeBaseName();
//         }

//         // qDebug() << "[主窗口] 开始保存STFT数据（本地文件模式）...";
//         // qDebug() << "   文件名:" << fileName;
//         // qDebug() << "   输出目录:" << outputDir;
//         // qDebug() << "   文件基础名:" << fileBaseName;

//         //success = localFileReader->saveSTFTDataToCSV(outputDir, fileBaseName);

//     } else if (currentMode == MODE_UDP_DUAL_CLIENT) {
//         if (!udpDualClient) {
//             qWarning() << "[主窗口] UDP客户端未初始化";
//             return;
//         }

//         fileBaseName = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");

//         qDebug() << "[主窗口] 开始保存STFT数据（UDP客户端模式）...";
//         qDebug() << "   输出目录:" << outputDir;
//         qDebug() << "   文件基础名:" << fileBaseName;

//         success = udpDualClient->saveSTFTDataToCSV(outputDir, fileBaseName);

//     } else {
//         qWarning() << "[主窗口] 当前模式不支持保存STFT数据";
//         return;
//     }

//     // 显示结果
//     if (success) {
//         qInfo() << "[主窗口] ✅ STFT数据保存成功！";
//         statusConnectionLabel->setText("STFT数据已保存");
//         statusConnectionLabel->setStyleSheet("color: green;");
//     } else {
//         qWarning() << "[主窗口] ❌ STFT数据保存失败";
//         statusConnectionLabel->setText("STFT数据保存失败");
//         statusConnectionLabel->setStyleSheet("color: red;");
//     }
// }

// ========== 其他槽函数 ==========

void MainWindow::onAbout()
{
    qInfo() << "关于: UDP 双通道频谱数据采集传输系统";
    qInfo() << "功能:";
    qInfo() << "- 绘图文件: 直接读取COS文件进行频谱分析";
    qInfo() << "- 服务器: 读取COS文件, 通过UDP发送数据";
    qInfo() << "- 客户端: 接收UDP数据, 实时显示频谱";
    qInfo() << "版本: 2.0";
}

void MainWindow::onServerPacketSent(int frameCount, quint32 sequence)
{
    Q_UNUSED(frameCount)
    Q_UNUSED(sequence)
}

void MainWindow::onClientBatchReady(int frameCount)
{
    // ✅ 改为与副本项目一致：空函数，仅定时器驱动UI更新
    Q_UNUSED(frameCount);
}

void MainWindow::onClientStatsUpdated(const ReceiveStats &stats)
{
    QString statusText = QString("接收: %1帧 | 丢包: %2 | FFT: %3次")
                             .arg(stats.totalFramesReceived)
                             .arg(stats.lostPackets)
                             .arg(stats.fftProcessCount);
    statusConnectionLabel->setText(statusText);
    statusConnectionLabel->setStyleSheet("color: green;");
}

// ========== 旧的FFT更新函数（已废弃，保留空实现用于兼容）==========
// 新架构使用 PlotWorker + onSpectrogramBatchReady 代替此函数
void MainWindow::updateFFTPlots()
{
    // 此函数已废弃，不再使用
    // 新架构中数据处理流程：
    // 1. PlotWorker 在子线程中从 RingBuffer 读取数据并整理
    // 2. 通过 onSpectrogramBatchReady 信号传递给主线程
    // 3. 主线程更新小频谱图和大瀑布图
    // 4. 定时器只负责 replot，不再直接处理数据
}

void MainWindow::onSpectrogramBatchReady(const SpectrogramBatch &batch)
{
    //qDebug() << "[BATCH]" << batch.channel << batch.startSlot << batch.count << batch.freqCount;

    if (batch.count <= 0 || batch.freqCount <= 0) return;

    // 时间显示：优先使用数据包内UTC时间；未收到数据前由InfoPanel内部定时器显示当前UTC
    if (batch.timestamp.isValid() && infoPanel) {
        infoPanel->enableAutoTimeUpdate(false);
        infoPanel->setUTCTime(batch.timestamp);
    }


    // ✅ 新逻辑：缓存batch，等三个通道都到齐后再同步绘制
    if (batch.channel == PLOT_CH_NS) {
        m_pendingBatchNS = batch;
        m_hasPendingNS = true;
        // 缓存最新的频谱数据
        m_latestSpectrumNS_freq = batch.freq;
        m_latestSpectrumNS_data = batch.lastSpectrum;
        m_hasNewSpectrumNS = true;
    } else if (batch.channel == PLOT_CH_EW) {
        m_pendingBatchEW = batch;
        m_hasPendingEW = true;
        // 缓存最新的频谱数据
        m_latestSpectrumEW_freq = batch.freq;
        m_latestSpectrumEW_data = batch.lastSpectrum;
        m_hasNewSpectrumEW = true;
    } else if (batch.channel == PLOT_CH_TD) {
        m_pendingBatchTD = batch;
        m_hasPendingTD = true;
        // 缓存最新的频谱数据
        m_latestSpectrumTD_freq = batch.freq;
        m_latestSpectrumTD_data = batch.lastSpectrum;
        m_hasNewSpectrumTD = true;
    }
    // // === 方案 A：在这里按 slot 间隔更新右侧频谱图 ===

    // // 1. 取得这个 batch 的 slot 范围
    // const int startSlot = batch.startSlot;      // 这个 batch 第一个slot的全局索引
    // const int slotCount = batch.count;      // 这个 batch 有多少列
    // const int endSlot   = startSlot + slotCount;

    // // 2. 如果这是第一次进来，初始化 m_nextSpectrumSlotToDraw
    // if (m_nextSpectrumSlotToDraw <= 0) {
    //     // 比如从第100个slot开始画，你也可以从 startSlot 开始
    //     m_nextSpectrumSlotToDraw = startSlot + m_spectrumSlotInterval;
    // }

    // // 3. 在当前 batch 范围内，找所有 >= m_nextSpectrumSlotToDraw 的slot
    // //    比如 m_nextSpectrumSlotToDraw = 310，那么在 [startSlot, endSlot) 里
    // //    找到 310, 410, 510 ... 这些点（可能只有一个，也可能有多个）
    // for (int globalSlot = m_nextSpectrumSlotToDraw;
    //      globalSlot < endSlot;
    //      globalSlot += m_spectrumSlotInterval)
    // {
    //     int localIndex = globalSlot - startSlot;  // 换算到 batch 内部的列索引

    //     if (localIndex < 0 || localIndex >= slotCount) {
    //         continue; // 不在当前 batch 范围内，跳过
    //     }

    //     // 4. 使用已有的辅助函数，从这一列更新三通道频谱图
    //     updateSpectrumPlotsFromBatch(batch, localIndex);

    //     // 这里就是“在 globalSlot 这个位置，绘制一次频谱”
    //     // 你可以加一点打印看看：
    //     // qDebug() << "[Spectrum] 绘制slot" << globalSlot << " (batch localIndex =" << localIndex << ")";
    // }

    // // 5. 更新下次要绘制的 slot：
    // //    如果当前 batch 已经越过了 m_nextSpectrumSlotToDraw，
    // //    就把它推进到 >= endSlot 的第一个“整间隔点”
    // if (endSlot > m_nextSpectrumSlotToDraw) {
    //     int delta = endSlot - m_nextSpectrumSlotToDraw;
    //     int steps = (delta + m_spectrumSlotInterval - 1) / m_spectrumSlotInterval; // 向上取整步数
    //     m_nextSpectrumSlotToDraw += steps * m_spectrumSlotInterval;
    // }
    // 尝试同步绘制三个通道
    tryDrawSynchronizedBatches();
}

// void MainWindow::onSpectrogramBatchReady(const SpectrogramBatch &batch)
// {
//     // 移除调试输出，避免刷屏影响性能
//     // qDebug() << "[UI]"
//     //          << "thread=" << QThread::currentThread()
//     //          << "id=" << (quintptr)QThread::currentThreadId()
//     //          << "appThread=" << qApp->thread();
//     if (batch.count <= 0 || batch.freqCount <= 0) return;

//     VLFSensorWidget* w = nullptr;
//     if (batch.channel == PLOT_CH_NS) w = monitorWidgetNS;
//     else if (batch.channel == PLOT_CH_EW) w = monitorWidgetEW;
//     else if (batch.channel == PLOT_CH_TD) w = monitorWidgetTD;

//     if (!w) return;

//     const int F = batch.freqCount;
//     const int T = batch.count;

//     // 绘制：主线程仅负责把数据写入colorMap（UI线程安全）
//     const double* base = batch.data.constData();
//     for (int i = 0; i < T; ++i) {
//         int slot = batch.startSlot + i;
//         if (m_timeSlots > 0) {
//             slot = slot % m_timeSlots;
//         }
//         const double* col = base + i * F;
//         w->setColorMapColumnRaw(slot, col, F);
//     }

//     // 右侧频谱：每批用最后一列更新一次
//     if (!batch.freq.isEmpty() && batch.lastSpectrum.size() == F) {
//         w->setSpectrumData(batch.freq, batch.lastSpectrum);
//     }

//     // 时间轴：以NS为准，首次slot=0时更新
//     if (batch.channel == PLOT_CH_NS && batch.startSlot == 0 && batch.timestamp.isValid()) {
//         monitorWidgetNS->updateTimeAxis(batch.timestamp);
//         monitorWidgetEW->updateTimeAxis(batch.timestamp);
//         if (monitorWidgetTD) monitorWidgetTD->updateTimeAxis(batch.timestamp);
//     }

//     // 推进currentTimeSlot（以NS为准）
//     if (batch.channel == PLOT_CH_NS) {
//         currentTimeSlot = batch.startSlot + batch.count;

//         // 流式处理优化：不再基于列数触发replot，改为由定时器控制
//         // 只在最后一列时强制replot一次，确保显示完整
//         if (currentTimeSlot >= m_timeSlots - 1) {
//             monitorWidgetNS->replotSpectrogram();
//             monitorWidgetEW->replotSpectrogram();
//             if (monitorWidgetTD) monitorWidgetTD->replotSpectrogram();
//         }
//     }
// }


// ✅ 处理异步频谱渲染队列（定时器触发，50ms间隔）
void MainWindow::processSpectrumRenderQueue()
{
    if (m_spectrumRenderQueue.isEmpty()) {
        return;  // 队列为空，无需处理
    }

    // ✅ 智能队列管理：如果积压过多，进入追赶模式
    const int MAX_QUEUE_SIZE = 20;  // 最大队列大小
    const int CATCHUP_SIZE = 5;     // 追赶模式保留数量

    if (m_spectrumRenderQueue.size() > MAX_QUEUE_SIZE) {
        // qDebug() << "[频谱渲染] ⚠️ 队列积压过多(" << m_spectrumRenderQueue.size()
        //          << ")，进入追赶模式，清理旧数据";

        // 只保留最新的几个数据
        while (m_spectrumRenderQueue.size() > CATCHUP_SIZE) {
            m_spectrumRenderQueue.dequeue();  // 丢弃旧数据
        }

        // qDebug() << "[频谱渲染] 追赶完成，队列剩余:" << m_spectrumRenderQueue.size();
    }

    // 从队列取出一个频谱数据
    SpectrumData spectrumData = m_spectrumRenderQueue.dequeue();

    // 根据通道缓存频谱数据
    if (spectrumData.channel == PLOT_CH_NS) {
        m_latestSpectrumNS_freq = spectrumData.freq;
        m_latestSpectrumNS_data = spectrumData.data;
        m_hasNewSpectrumNS = true;
    } else if (spectrumData.channel == PLOT_CH_EW) {
        m_latestSpectrumEW_freq = spectrumData.freq;
        m_latestSpectrumEW_data = spectrumData.data;
        m_hasNewSpectrumEW = true;
    } else if (spectrumData.channel == PLOT_CH_TD) {
        m_latestSpectrumTD_freq = spectrumData.freq;
        m_latestSpectrumTD_data = spectrumData.data;
        m_hasNewSpectrumTD = true;
    }

    // 调用现有的更新函数进行绘制
    QElapsedTimer renderTimer;
    renderTimer.start();

    updateSpectrumPlots();

    qint64 renderTime = renderTimer.elapsed();
    if (renderTime > 10) {  // 只记录超过10ms的渲染
        //qDebug() << "[频谱渲染] 耗时:" << renderTime << "ms";
    }

    // 如果队列还有数据，显示剩余数量
    if (!m_spectrumRenderQueue.isEmpty() && m_spectrumRenderQueue.size() % 10 == 0) {
        //qDebug() << "[频谱渲染] 队列剩余:" << m_spectrumRenderQueue.size() << "个待渲染";
    }
}

// ✅ 从批次中提取特定槽位的频谱数据并加入渲染队列
void MainWindow::updateSpectrumPlotsFromBatch(const SpectrogramBatch &batch, int slotIndexInBatch)
{
    if (slotIndexInBatch < 0 || slotIndexInBatch >= batch.count) {
// qWarning() << "[主窗口] 槽位索引越界:" << slotIndexInBatch << "（批次大小:" << batch.count << "）";
        return;
    }

    const int F = batch.freqCount;

    // 从批次数据中提取目标槽位的频谱
    // 数据布局：[slot0_freq0, slot0_freq1, ..., slot1_freq0, slot1_freq1, ...]
    const double* base = batch.data.constData();
    const double* spectrumAtSlot = base + slotIndexInBatch * F;

    // 将提取的频谱数据复制到QVector
    QVector<double> extractedSpectrum(F);
    for (int i = 0; i < F; ++i) {
        extractedSpectrum[i] = spectrumAtSlot[i];
    }

    // ✅ 创建频谱数据并加入渲染队列（异步处理，不阻塞）
    SpectrumData spectrumData;
    spectrumData.freq = batch.freq;
    spectrumData.data = extractedSpectrum;
    spectrumData.channel = batch.channel;

    m_spectrumRenderQueue.enqueue(spectrumData);
}

void MainWindow::updateSpectrumPlots()
{
    // ✅ 定时器触发时，更新所有有新数据的通道（一次性批量更新，确保同步）
    // 先收集所有需要更新的通道
    bool hasAnyUpdate = false;

    if (spectrumWidgetNS && m_hasNewSpectrumNS && !m_latestSpectrumNS_freq.isEmpty()) {
        hasAnyUpdate = true;
    }
    if (spectrumWidgetEW && m_hasNewSpectrumEW && !m_latestSpectrumEW_freq.isEmpty()) {
        hasAnyUpdate = true;
    }
    if (spectrumWidgetTD && m_hasNewSpectrumTD && !m_latestSpectrumTD_freq.isEmpty()) {
        hasAnyUpdate = true;
    }

    if (!hasAnyUpdate) {
        return;  // 没有任何新数据，跳过本次更新
    }

    // 批量更新所有有新数据的通道（同一时刻更新，确保视觉同步）
    if (spectrumWidgetNS && m_hasNewSpectrumNS && !m_latestSpectrumNS_freq.isEmpty()) {
        spectrumWidgetNS->setSpectrumData(m_latestSpectrumNS_freq, m_latestSpectrumNS_data);
        m_hasNewSpectrumNS = false;
    }

    if (spectrumWidgetEW && m_hasNewSpectrumEW && !m_latestSpectrumEW_freq.isEmpty()) {
        spectrumWidgetEW->setSpectrumData(m_latestSpectrumEW_freq, m_latestSpectrumEW_data);
        m_hasNewSpectrumEW = false;
    }

    if (spectrumWidgetTD && m_hasNewSpectrumTD && !m_latestSpectrumTD_freq.isEmpty()) {
        spectrumWidgetTD->setSpectrumData(m_latestSpectrumTD_freq, m_latestSpectrumTD_data);
        m_hasNewSpectrumTD = false;
    }

    // 统一replot（确保同步刷新）
    if (spectrumWidgetNS) spectrumWidgetNS->replot();
    if (spectrumWidgetEW) spectrumWidgetEW->replot();
    if (spectrumWidgetTD) spectrumWidgetTD->replot();
}

void MainWindow::updateSpectrogramPlots()
{
    // 瀑布图刷新（50ms定时器触发）
    if (m_specDialogWidget && m_specDialog && m_specDialog->isVisible()) {
        m_specDialogWidget->replot();
    }
}

// 频谱图点击切换
// 频谱图点击切换
void MainWindow::onSpectrumWidgetClicked(const QString &channelName)
{
// qDebug() << "[主窗口] 频谱图点击切换到通道:" << channelName;

    // 取消所有频谱图的选中状态
    if (spectrumWidgetNS) spectrumWidgetNS->setSelected(false);
    if (spectrumWidgetEW) spectrumWidgetEW->setSelected(false);
    if (spectrumWidgetTD) spectrumWidgetTD->setSelected(false);

    // 设置当前选中的通道
    m_currentChannel = channelName;

    // ✅ 弹窗：确保存在
    ensureSpectrogramDialog();
    if (!m_specDialog || !m_specDialogWidget) return;

    // 确定缓存数据
    QVector<QVector<double>> *cachedData = nullptr;

    // 高亮选中的频谱图并更新“弹窗瀑布图”的标题/通道
    if (channelName == "CH-A") {
        if (spectrumWidgetNS) spectrumWidgetNS->setSelected(true);
        m_specDialogWidget->setCurrentChannel("CH-A", "N-S");
        cachedData = &m_cachedSpectrogramNS;

        // ✅ 更新自定义黑色标题栏
        if (auto titleLabel = m_specDialog->findChild<QLabel*>("specTitleLabel")) {
            titleLabel->setText("[CH-A] VLF Magnetic (N-S) - Spectrogram");
        }

    } else if (channelName == "CH-B") {
        if (spectrumWidgetEW) spectrumWidgetEW->setSelected(true);
        m_specDialogWidget->setCurrentChannel("CH-B", "E-W");
        cachedData = &m_cachedSpectrogramEW;

        // ✅ 更新自定义黑色标题栏
        if (auto titleLabel = m_specDialog->findChild<QLabel*>("specTitleLabel")) {
            titleLabel->setText("[CH-B] VLF Magnetic (E-W) - Spectrogram");
        }

    } else if (channelName == "CH-C") {
        if (spectrumWidgetTD) spectrumWidgetTD->setSelected(true);
        m_specDialogWidget->setCurrentChannel("CH-C", "T-D");
        cachedData = &m_cachedSpectrogramTD;

        // ✅ 更新自定义黑色标题栏
        if (auto titleLabel = m_specDialog->findChild<QLabel*>("specTitleLabel")) {
            titleLabel->setText("[CH-C] VLF Magnetic (T-D) - Spectrogram");
        }
    }

    // ✅ 显示并置顶弹窗
    if (!m_specDialog->isVisible()) {
        m_specDialog->show();
    }
    m_specDialog->raise();
    m_specDialog->activateWindow();

    // ✅ 切换通道时：如果有缓存，就分块重绘到“弹窗瀑布图”（避免阻塞UI）
    if (cachedData && !cachedData->isEmpty()) {
        const int timeSlots = cachedData->size();
        const int freqCount = cachedData->at(0).size();
        const int chunkSize = 100;

        QTimer *chunkTimer = new QTimer(this);
        chunkTimer->setSingleShot(false);
        int currentSlot = 0;

        connect(chunkTimer, &QTimer::timeout, this,
                [this, cachedData, timeSlots, freqCount, chunkSize, chunkTimer, currentSlot]() mutable {
            if (!m_specDialogWidget) { // 弹窗被销毁/异常情况
                chunkTimer->stop();
                chunkTimer->deleteLater();
                return;
            }

            int endSlot = qMin(currentSlot + chunkSize, timeSlots);

            for (int slot = currentSlot; slot < endSlot; ++slot) {
                const QVector<double> &columnData = cachedData->at(slot);
                if (columnData.size() == freqCount) {
                    m_specDialogWidget->setColorMapColumnRaw(slot, columnData.constData(), freqCount);
                }
            }

            currentSlot = endSlot;

            if (currentSlot >= timeSlots) {
                chunkTimer->stop();
                chunkTimer->deleteLater();
                m_specDialogWidget->replot();
// qDebug() << "[主窗口] 瀑布图缓存重绘完成（弹窗）";
            }
        });

        chunkTimer->start(0);
    }
}


// 刷新间隔改变
void MainWindow::onSpectrumRefreshIntervalChanged(int index)
{
    int intervalMs = spectrumRefreshCombo->itemData(index).toInt();
// qDebug() << "[主窗口] 频谱图刷新间隔改变为:" << intervalMs << "ms";

    // ✅ 清空渲染队列（避免旧数据积压）
    m_spectrumRenderQueue.clear();

    // ✅ 动态调整渲染定时器速度（应该比刷新间隔快10倍）
    int renderIntervalMs = qMax(2, intervalMs / 10);  // 最小2ms
    m_spectrumRenderTimer->setInterval(renderIntervalMs);
// qDebug() << "[主窗口] 渲染定时器间隔已调整为:" << renderIntervalMs << "ms（刷新间隔的1/10）";

    // ✅ 更新基于位置的刷新间隔（秒）
    m_spectrumRefreshInterval = intervalMs / 1000.0;

    // 重置下一个刷新点（从当前位置开始计算）
    // 使用当前时间槽位置和hop_size计算当前播放时间
    double currentPlaybackTime = (currentTimeSlot * m_currentHopSize) / m_sampleRate;
    m_nextSpectrumRefreshTime = currentPlaybackTime + m_spectrumRefreshInterval;

// qDebug() << "[主窗口] 基于位置的刷新间隔已更新为:" << m_spectrumRefreshInterval << "秒";
// qDebug() << "[主窗口] 当前播放位置:" << currentPlaybackTime << "秒，下一个刷新点:" << m_nextSpectrumRefreshTime << "秒";
m_drawRefreshInterval = intervalMs;              // 记录当前刷新间隔
if (m_spectrumDrawTimer) {
    m_spectrumDrawTimer->setInterval(m_drawRefreshInterval);
}
    statusBar()->showMessage(QString("频谱图刷新间隔已设置为 %1 ms").arg(intervalMs));
}

// ========== WebSocket控制 ==========

void MainWindow::onConnectWebSocket()
{
    QString url = "ws://localhost:8080/ws";  // 默认本地服务器
    webSocketClient->connectToServer(url);
// qDebug() << "[主窗口] 正在连接WebSocket:" << url;
}

void MainWindow::onDisconnectWebSocket()
{
    webSocketClient->disconnectFromServer();
// qDebug() << "[主窗口] 已断开WebSocket连接";
}

void MainWindow::onSendStartCommand()
{
    if (!webSocketClient->isConnected()) {
// qWarning() << "[警告] 请先连接WebSocket服务器";
        return;
    }

    // 从视图菜单获取通道配置并发送COLLECT命令（携带采样率）
    QString channelConfig = getChannelConfigFromView();
    webSocketClient->sendCollectCommand(channelConfig, m_sampleRate);
// qDebug() << "[主窗口] 已发送COLLECT命令，通道配置:" << channelConfig << "采样率:" << m_sampleRate;
}

void MainWindow::onSendDisplayCommand()
{
    if (!webSocketClient->isConnected()) {
// qWarning() << "[警告] 请先连接WebSocket服务器";
        return;
    }

    // 从视图菜单获取通道配置并发送DISPLAY命令
    QString channelConfig = getChannelConfigFromView();
    webSocketClient->sendDisplayCommand(channelConfig);
// qDebug() << "[主窗口] 已发送DISPLAY命令，通道配置:" << channelConfig;
}

void MainWindow::onSendStopCollectCommand()
{
    if (!webSocketClient->isConnected()) {
// qWarning() << "[警告] 请先连接WebSocket服务器";
        return;
    }

    webSocketClient->sendStopCollectCommand();
// qDebug() << "[主窗口] 已发送STOP_COLLECT命令";
}

void MainWindow::onSendStopDisplayCommand()
{
    if (!webSocketClient->isConnected()) {
// qWarning() << "[警告] 请先连接WebSocket服务器";
        return;
    }

    webSocketClient->sendStopDisplayCommand();
// qDebug() << "[主窗口] 已发送STOP_DISPLAY命令";
}

void MainWindow::onWebSocketConnected()
{
// qDebug() << "[主窗口] WebSocket已连接";
    statusConnectionLabel->setText("状态: WebSocket已连接");
    statusConnectionLabel->setStyleSheet("color: green;");

    // 停止重连定时器
    if (m_wsReconnectTimer) m_wsReconnectTimer->stop();
    if (m_collectButtonTop) m_collectButtonTop->setEnabled(true);
    if (m_displayButtonTop) m_displayButtonTop->setEnabled(true);

    // 已移到信息面板按钮
    // connectWebSocketAction->setEnabled(false);
    // disconnectWebSocketAction->setEnabled(true);
    // if (sendStartCommandAction) sendStartCommandAction->setEnabled(true);
    // if (sendDisplayCommandAction) sendDisplayCommandAction->setEnabled(true);
    // if (sendStopCollectCommandAction) sendStopCollectCommandAction->setEnabled(true);
    // if (sendStopDisplayCommandAction) sendStopDisplayCommandAction->setEnabled(true);
    // subscribeStatus1sAction->setEnabled(true);
    // subscribeStatus5sAction->setEnabled(true);
    // subscribeStatus10sAction->setEnabled(true);
}

void MainWindow::onWebSocketDisconnected()
{
// qDebug() << "[主窗口] WebSocket已断开";
    statusConnectionLabel->setText("状态: WebSocket已断开");
    statusConnectionLabel->setStyleSheet("color: gray;");

    // 开始持续重连
    if (m_wsReconnectTimer && !m_wsReconnectTimer->isActive()) m_wsReconnectTimer->start();
    if (m_collectButtonTop) m_collectButtonTop->setEnabled(false);
    if (m_displayButtonTop) m_displayButtonTop->setEnabled(false);
    // 断线时复位按钮状态
    m_collecting = false;
    m_displaying = false;
    if (m_collectButtonTop) m_collectButtonTop->setText("开始采集");
    if (m_displayButtonTop) m_displayButtonTop->setText("开始显示");

    // 已移到信息面板按钮
    // connectWebSocketAction->setEnabled(true);
    // disconnectWebSocketAction->setEnabled(false);
    // if (sendStartCommandAction) sendStartCommandAction->setEnabled(false);
    // if (sendDisplayCommandAction) sendDisplayCommandAction->setEnabled(false);
    // if (sendStopCollectCommandAction) sendStopCollectCommandAction->setEnabled(false);
    // if (sendStopDisplayCommandAction) sendStopDisplayCommandAction->setEnabled(false);
    // subscribeStatus1sAction->setEnabled(false);
    // subscribeStatus5sAction->setEnabled(false);
    // subscribeStatus10sAction->setEnabled(false);
}

void MainWindow::onWebSocketAckReceived(const QString &cmd, int code, const QString &message)
{
// qDebug() << "[主窗口] 收到ACK:" << cmd << "code=" << code << "msg=" << message;

    if (code == 0) {
        statusConnectionLabel->setText(QString("状态: %1成功").arg(cmd == "start" ? "START" : "DISPLAY"));
        statusConnectionLabel->setStyleSheet("color: green;");
    } else {
        statusConnectionLabel->setText(QString("状态: %1失败 - %2").arg(cmd).arg(message));
        statusConnectionLabel->setStyleSheet("color: red;");
// qWarning() << "[命令失败]" << cmd << ":" << message;
    }
}

void MainWindow::onWebSocketCommandAckReceived(const QString &cmd, int code, const QString &msg, const QJsonObject &data)
{
// qDebug() << "[主窗口] 收到命令ACK:" << cmd << "code=" << code << "msg=" << msg;

    if (code != 0) {
        statusConnectionLabel->setText(QString("状态: %1失败 - %2").arg(cmd).arg(msg));
        statusConnectionLabel->setStyleSheet("color: red;");
// qWarning() << "[命令失败]" << cmd << ":" << msg;
        return;
    }

    // 处理COLLECT命令的ACK（包含起始时间戳和文件时长）
    if (cmd == "collect") {
        // 解析起始时间戳
        QString startTimestampStr = data["start_timestamp"].toString();
        double duration = data["duration"].toDouble();

        if (startTimestampStr.isEmpty() || duration <= 0) {
// qWarning() << "[主窗口] COLLECT ACK缺少时间信息: start_timestamp=" << startTimestampStr << "duration=" << duration;
            return;
        }

        // 解析时间戳字符串（格式：2026-01-27 23:33:02）
        QDateTime startTime = QDateTime::fromString(startTimestampStr, "yyyy-MM-dd HH:mm:ss");
        if (!startTime.isValid()) {
// qWarning() << "[主窗口] 无效的起始时间戳:" << startTimestampStr;
            return;
        }

// qDebug() << "[主窗口] 收到COLLECT ACK - 起始时间:" << startTime.toString("yyyy-MM-dd HH:mm:ss")
    //                  << "文件时长:" << duration << "秒";

        // ✅ 关键修复：根据文件时长计算并设置 analysisHop（与本地文件模式一致）
        const int timeSlots = 2032;  // 与UDP客户端启动时的设置一致
        const int fftSize = 4096;
        const qint64 totalSamples = static_cast<qint64>(duration * m_sampleRate);

        int analysisHop = FFTProcessor::hopSize(); // 默认 2048
        int idealHopSize = analysisHop;  // 用于位置计算的理想hop_size（不受最小值限制）

        if (totalSamples > fftSize && timeSlots > 1) {
            const qint64 numerator = totalSamples - fftSize;
            idealHopSize = static_cast<int>((numerator + (timeSlots - 2)) / (timeSlots - 1));
            analysisHop = qMax(idealHopSize, FFTProcessor::hopSize());  // FFT处理用的hop_size（有最小值限制）
        }

        // 设置UDP客户端的analysisHop（确保FFT输出列数匹配显示宽度）
        if (udpDualClient) {
            udpDualClient->setAnalysisHop(analysisHop);
// qDebug() << "[主窗口] 已设置UDP客户端 analysisHop=" << analysisHop
    //                      << "（总样本数=" << totalSamples << "，预期列数=" << timeSlots << "）";
        }

        // ✅ 保存理想hop_size用于位置计算（不受最小值限制，确保位置准确）
        m_currentHopSize = idealHopSize;
// qDebug() << "[主窗口] UDP模式 - 位置计算hop_size:" << m_currentHopSize << "（FFT处理hop_size:" << analysisHop << "）";

        // 设置显示时间范围（使用文件实际时长）
        if (m_specDialogWidget) {
            m_specDialogWidget->setDisplayTimeRange(duration);
            m_specDialogWidget->updateTimeAxis(startTime);
        }

        statusConnectionLabel->setText(QString("状态: COLLECT成功 (时长: %1秒)").arg(duration, 0, 'f', 1));
        statusConnectionLabel->setStyleSheet("color: green;");
    } else {
        // 其他命令的ACK
        statusConnectionLabel->setText(QString("状态: %1成功").arg(cmd));
        statusConnectionLabel->setStyleSheet("color: green;");
    }
}

void MainWindow::onWebSocketStatusReceived(const QJsonObject &statusData)
{
    // 更新InfoPanel显示
    infoPanel->updateFromStatusData(statusData);
}

void MainWindow::onWebSocketError(const QString &message)
{
// qCritical() << "[WebSocket错误]" << message;
    statusConnectionLabel->setText("状态: WebSocket错误");
    statusConnectionLabel->setStyleSheet("color: red;");
}

// ========== 状态栏订阅 ==========

void MainWindow::onSubscribeStatus1s()
{
    if (!webSocketClient->isConnected()) {
// qWarning() << "[警告] 请先连接WebSocket服务器";
        return;
    }

    webSocketClient->sendStatusSubscribe(1);
// qDebug() << "[主窗口] 已订阅状态栏，周期: 1秒";
}

void MainWindow::onSubscribeStatus5s()
{
    if (!webSocketClient->isConnected()) {
// qWarning() << "[警告] 请先连接WebSocket服务器";
        return;
    }

    webSocketClient->sendStatusSubscribe(5);
// qDebug() << "[主窗口] 已订阅状态栏，周期: 5秒";
}

void MainWindow::onSubscribeStatus10s()
{
    if (!webSocketClient->isConnected()) {
// qWarning() << "[警告] 请先连接WebSocket服务器";
        return;
    }

    webSocketClient->sendStatusSubscribe(10);
// qDebug() << "[主窗口] 已订阅状态栏，周期: 10秒";
}

// ========== 时间文件解析 ==========

void MainWindow::onTimeFileParseFinished(bool success, const QString &message)
{
    if (success) {
        statusConnectionLabel->setText("状态: 时间文件解析完成");
        statusConnectionLabel->setStyleSheet("color: green;");
// qDebug() << "[主窗口] 时间文件解析成功:" << message;
    } else {
        statusConnectionLabel->setText("状态: 时间文件解析失败");
        statusConnectionLabel->setStyleSheet("color: red;");
// qCritical() << "[主窗口] 时间文件解析失败:" << message;
    }
}
void MainWindow::onLoadTimeData()
{
    // 1. 选择FPGA时间文件（.dat）
    QString inputFilePath = QFileDialog::getOpenFileName(
        this,
        "加载时间数据",
        "",
        "Time Files (*.dat);;All Files (*)"
    );

    if (inputFilePath.isEmpty()) {
        return;
    }

    // 2. 生成输出文件路径（在输入文件同目录下，加txt_前缀）
    QFileInfo fileInfo(inputFilePath);
    QString outputDir = fileInfo.absolutePath();
    QString baseName = fileInfo.completeBaseName();  // 不带扩展名的文件名
    QString outputFilePath = outputDir + "/txt_" + baseName + ".txt";

    // 3. 显示状态信息
    statusConnectionLabel->setText("状态: 正在解析时间文件...");
    statusConnectionLabel->setStyleSheet("color: orange;");

// qDebug() << "[主窗口] 开始解析时间文件:" << inputFilePath;
// qDebug() << "[主窗口] 输出文件:" << outputFilePath;

    // 4. 调用解析器（同步解析）
    bool parseSuccess = timeFileParser->parseTimeFile(inputFilePath, outputFilePath);

    if (!parseSuccess) {
        statusConnectionLabel->setText("状态: 时间文件解析失败");
        statusConnectionLabel->setStyleSheet("color: red;");
// qCritical() << "[主窗口] 时间文件解析失败";
        return;
    }

    // 5. 解析成功后，自动加载时间数据
    statusConnectionLabel->setText("状态: 正在加载时间数据...");
    statusConnectionLabel->setStyleSheet("color: orange;");

// qDebug() << "[主窗口] 时间文件解析成功，开始加载时间数据:" << outputFilePath;

    // 6. 异步调用TimeStampLoader提取整秒时刻（在子线程执行）
    QMetaObject::invokeMethod(timeStampLoader, "loadAsync",
                              Qt::QueuedConnection,
                              Q_ARG(QString, outputFilePath));

    // 注意：loadFinished信号会自动触发onTimeDataLoaded槽函数
}

void MainWindow::onTimeDataLoaded(bool success, const QString &message)
{
    if (success) {
        statusConnectionLabel->setText("状态: 时间数据加载完成");
        statusConnectionLabel->setStyleSheet("color: green;");
// qDebug() << "[主窗口] 时间数据加载成功:" << message;

        // 获取提取的整秒时刻
        const QVector<SecondMark>& marks = timeStampLoader->getSecondMarks();

        if (marks.isEmpty()) {
// qWarning() << "[主窗口] 时间标记为空，无法切换到绝对时间模式";
            statusConnectionLabel->setText("状态: 时间标记为空");
            statusConnectionLabel->setStyleSheet("color: orange;");
            return;
        }

        // 切换大瀑布图到绝对时间模式
        if (m_specDialogWidget) {
            m_specDialogWidget->setAbsoluteTimeMode(marks);
// qDebug() << "[主窗口] 已切换到绝对时间模式，时间标记数:" << marks.size();
        }

        // 显示时间范围
        QDateTime startTime = timeStampLoader->getStartTime();
        QDateTime endTime = timeStampLoader->getEndTime();
        double durationSec = timeStampLoader->getDurationSeconds();

// qDebug() << "[主窗口] 时间范围:" << startTime.toString("yyyy-MM-dd HH:mm:ss")
    //                  << "~" << endTime.toString("yyyy-MM-dd HH:mm:ss")
    //                  << "，时长:" << durationSec << "秒";

        statusConnectionLabel->setText(QString("状态: 绝对时间模式 (%1s)").arg((int)durationSec));
    } else {
        statusConnectionLabel->setText("状态: 时间数据加载失败");
        statusConnectionLabel->setStyleSheet("color: red;");
// qCritical() << "[主窗口] 时间数据加载失败:" << message;
    }
}

void MainWindow::tryDrawSynchronizedBatches()
{
    // 检查是否所有启用的通道都已到齐（基于hasTD标志判断是否需要等待TD）
    bool needTD = m_hasPendingNS && m_pendingBatchNS.hasTD;
    bool allReady = m_hasPendingNS && m_hasPendingEW && (!needTD || m_hasPendingTD);

    if (!allReady) {
        return;  // 还没有全部到齐，等待
    }

    // 检查startSlot是否一致（同步检测）
    int startSlot = m_pendingBatchNS.startSlot;
    if (m_pendingBatchEW.startSlot != startSlot) {
// qDebug() << "[主窗口] 警告：NS和EW的startSlot不一致:" << startSlot << "vs" << m_pendingBatchEW.startSlot;
        // 丢弃较旧的batch，保留较新的
        if (m_pendingBatchEW.startSlot < startSlot) {
            m_hasPendingEW = false;
        } else {
            m_hasPendingNS = false;
        }
        return;
    }
    if (needTD && m_pendingBatchTD.startSlot != startSlot) {
// qDebug() << "[主窗口] 警告：TD的startSlot不一致:" << startSlot << "vs" << m_pendingBatchTD.startSlot;
        if (m_pendingBatchTD.startSlot < startSlot) {
            m_hasPendingTD = false;
        } else {
            m_hasPendingNS = false;
            m_hasPendingEW = false;
        }
        return;
    }

    // ✅ 所有通道都到齐且同步，开始绘制
    //qDebug() << "[同步绘制] 三通道同步绘制 startSlot:" << startSlot;

    // ✅ 只用NS通道来跟踪播放位置（避免重复计数）
    // 新方案：在批次内部查找所有需要刷新的槽位
    int batchStartSlot = m_pendingBatchNS.startSlot;
    int batchEndSlot = m_pendingBatchNS.startSlot + m_pendingBatchNS.count;

    // 计算批次结束时间
    double batchEndTime = (batchEndSlot * m_currentHopSize) / m_sampleRate;

    // // 查找批次内所有需要刷新的时间点
    // while (m_nextSpectrumRefreshTime < batchEndTime) {
    //     // 计算这个刷新点对应的槽位
    //     int targetSlot = (int)(m_nextSpectrumRefreshTime * m_sampleRate / m_currentHopSize);

    //     // 检查目标槽位是否在当前批次内
    //     if (targetSlot >= batchStartSlot && targetSlot < batchEndSlot) {
    //         // 计算槽位在批次中的索引
    //         int slotIndexInBatch = targetSlot - batchStartSlot;

    //         m_spectrumRefreshCount++;
    //         // qDebug() << "========================================";
    //         // qDebug() << "[频谱刷新 #" << m_spectrumRefreshCount << "]";
    //         // qDebug() << "  目标槽位:" << targetSlot << "（批次内索引:" << slotIndexInBatch << "）";
    //         // qDebug() << "  刷新时间:" << m_nextSpectrumRefreshTime << "秒";
    //         // qDebug() << "  刷新间隔:" << m_spectrumRefreshInterval << "秒";
    //         // qDebug() << "  队列大小:" << m_spectrumRenderQueue.size();
    //         // qDebug() << "========================================";

    //         // ✅ 从批次中提取目标槽位的频谱数据并加入渲染队列（不阻塞）
    //         updateSpectrumPlotsFromBatch(m_pendingBatchNS, slotIndexInBatch);
    //     }

    //     // 推进到下一个刷新点
    //     m_nextSpectrumRefreshTime += m_spectrumRefreshInterval;
    // }

    // // 1) 处理NS通道
    // processSingleBatch(m_pendingBatchNS, spectrumWidgetNS, "CH-A", &m_cachedSpectrogramNS);

    // // 2) 处理EW通道
    // processSingleBatch(m_pendingBatchEW, spectrumWidgetEW, "CH-B", &m_cachedSpectrogramEW);

    // // 3) 处理TD通道（如果有）
    // if (needTD) {
    //     processSingleBatch(m_pendingBatchTD, spectrumWidgetTD, "CH-C", &m_cachedSpectrogramTD);
    // }
    while (m_nextSpectrumRefreshTime < batchEndTime) {
        int targetSlot = (int)(m_nextSpectrumRefreshTime * m_sampleRate / m_currentHopSize);
        if (targetSlot >= batchStartSlot && targetSlot < batchEndSlot) {
            int slotIndexInBatch = targetSlot - batchStartSlot;
 
            // 三通道同步，把同一个 slot 的频谱一起丢进渲染队列
            updateSpectrumPlotsFromBatch(m_pendingBatchNS, slotIndexInBatch);
            updateSpectrumPlotsFromBatch(m_pendingBatchEW, slotIndexInBatch);
            if (needTD) {
                updateSpectrumPlotsFromBatch(m_pendingBatchTD, slotIndexInBatch);
            }
        }
        m_nextSpectrumRefreshTime += m_spectrumRefreshInterval;
    }
    // 4) 推进currentTimeSlot（以NS为准）
    currentTimeSlot = m_pendingBatchNS.startSlot + m_pendingBatchNS.count;

    // 5) 清除pending标志
    m_hasPendingNS = false;
    m_hasPendingEW = false;
    m_hasPendingTD = false;
// if (spectrumWidgetNS) spectrumWidgetNS->replot();
// if (spectrumWidgetEW) spectrumWidgetEW->replot();
// if (needTD && spectrumWidgetTD) spectrumWidgetTD->replot();
}

void MainWindow::processSingleBatch(const SpectrogramBatch &batch,
                                     SpectrumWidget* spectrumWidget,
                                     const QString &channelName,
                                     QVector<QVector<double>>* cachedData)
{
    if (!spectrumWidget || !cachedData) return;

    const int F = batch.freqCount;
    const int T = batch.count;

    // 1) 缓存所有通道的数据（用于切换时重绘）
    // 确保缓存大小足够
    if (cachedData->size() != m_timeSlots) {
        cachedData->resize(m_timeSlots);
        for (int i = 0; i < m_timeSlots; ++i) {
            (*cachedData)[i].resize(F);
            (*cachedData)[i].fill(40.0);
        }
    }

    // 缓存当前批次的数据
    const double* base = batch.data.constData();

    // 固定10秒窗口：只按接收顺序写 0..m_timeSlots-1；写满则清空并从0重绘
    for (int i = 0; i < T; ++i) {
        const double* col = base + i * F;

        // 仅在每轮开始时初始化时间轴（用首个有效时间戳）
        if (!m_windowInitialized && batch.timestamp.isValid() && batch.channel == PLOT_CH_NS) {
            if (m_specDialogWidget) m_specDialogWidget->updateTimeAxis(batch.timestamp);
            m_windowInitialized = true;
            m_currentWriteSlot = 0;
        }

        // 写满10秒 -> 清空图表与缓存，下一轮重新设置时间轴
        if (m_timeSlots > 0 && m_currentWriteSlot >= m_timeSlots) {
            if (m_specDialogWidget) m_specDialogWidget->clearColorMap();
            for (int s = 0; s < cachedData->size(); ++s) {
                (*cachedData)[s].fill(40.0);
            }
            m_currentWriteSlot = 0;
            m_windowInitialized = false;
        }

        // 缓存（用于切换通道时重绘弹窗）
        if (m_currentWriteSlot >= 0 && m_currentWriteSlot < cachedData->size()) {
            for (int f = 0; f < F; ++f) {
                (*cachedData)[m_currentWriteSlot][f] = col[f];
            }
        }

        // 只有当前选中的通道才更新大瀑布图（弹窗）
        if (channelName == m_currentChannel && m_specDialogWidget && m_specDialog && m_specDialog->isVisible()) {
            if (m_currentWriteSlot >= 0 && m_currentWriteSlot < m_timeSlots) {
                m_specDialogWidget->setColorMapColumnRaw(m_currentWriteSlot, col, F);
            }
        }

        ++m_currentWriteSlot;
    }
}
void MainWindow::ensureSpectrogramDialog()
{
    if (m_specDialog) return;

    m_specDialog = new QDialog(this);
    m_specDialog->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    m_specDialog->setAttribute(Qt::WA_DeleteOnClose, false);
    m_specDialog->resize(1200, 600);

    // 让内容贴边：只保留红框那一整块
    auto *layout = new QVBoxLayout(m_specDialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 弹窗背景/边框（想“只有内容”就 border:none）
    m_specDialog->setStyleSheet(R"(
        QDialog {
            background-color: #000000;
            border: none;
        }
    )");

    m_specDialogWidget = new SpectrogramWidget(m_specDialog);
    m_specDialogWidget->setSampleRate(m_sampleRate);
    layout->addWidget(m_specDialogWidget);

    // 保留你现有“对数/线性切换”同步频谱图
    connect(m_specDialogWidget, &SpectrogramWidget::scaleChanged, this, [this](bool isLogScale) {
        if (spectrumWidgetNS) spectrumWidgetNS->setLogScale(isLogScale, m_sampleRate);
        if (spectrumWidgetEW) spectrumWidgetEW->setLogScale(isLogScale, m_sampleRate);
        if (spectrumWidgetTD) spectrumWidgetTD->setLogScale(isLogScale, m_sampleRate);
    });

    // 内部关闭按钮：隐藏弹窗
    connect(m_specDialogWidget, &SpectrogramWidget::closeRequested, this, [this]() {
        if (m_specDialog) m_specDialog->hide();
    });
}
void MainWindow::onPacketConfigChanged(int channelCount, double sampleRateHz)
{
    // 1) 更新主窗口采样率
    m_sampleRate = sampleRateHz;

    // 2) 更新三张小频谱图坐标轴
    if (spectrumWidgetNS) spectrumWidgetNS->setSampleRate(m_sampleRate);
    if (spectrumWidgetEW) spectrumWidgetEW->setSampleRate(m_sampleRate);
    if (spectrumWidgetTD) spectrumWidgetTD->setSampleRate(m_sampleRate);

    // 3) 更新大瀑布图坐标轴
    if (m_specDialogWidget) m_specDialogWidget->setSampleRate(m_sampleRate);

    // 4) TD通道显示/隐藏（2通道时隐藏）
    bool hasTD = (channelCount == 3);

    if (!hasTD) {
        // UI隐藏TD
        if (spectrumWidgetTD) spectrumWidgetTD->hide();
    } else {
        if (spectrumWidgetTD) spectrumWidgetTD->show();
    }

    // 5) 重新计算10秒窗口 timeSlots
    if (m_currentHopSize <= 0) m_currentHopSize = FFTProcessor::hopSize();

    const double displaySec = 10.0;
    double dt = (double)m_currentHopSize / m_sampleRate;
    if (dt <= 0) dt = 2048.0 / 4000000.0;

    int timeSlots = (int)(displaySec / dt);
    if (timeSlots < 64) timeSlots = 64;
    if (timeSlots > 4096) timeSlots = 4096;

    m_timeSlots = timeSlots;

    // 6) 重新初始化瀑布图尺寸（关键！）
    if (m_specDialogWidget) {
        m_specDialogWidget->setDisplayTimeRange(displaySec);
        m_specDialogWidget->initializeColorMapSize(m_timeSlots, 2048);
    }

    // 7) 重置绘图写入位置（避免旧数据残留）
    m_currentWriteSlot = 0;
    m_windowInitialized = false;

    qDebug() << "[MainWindow] UI已根据包头更新:"
             << "channelCount=" << channelCount
             << "sampleRate=" << m_sampleRate
             << "timeSlots=" << m_timeSlots;
}