#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QTimer>
#include <QThread>
#include <QStatusBar>
#include <QMenu>
#include <QAction>
#include "qcustomplot.h"
#include "spectrumwidget.h"
#include "spectrogramwidget.h"
#include "infopanelwidget.h"
#include "udpserversender.h"
#include "udpclientreceiver.h"
#include "localfilereader.h"
#include "processing/plotworker.h"
#include "processing/timefileparser.h"
#include "processing/timestamploader.h"
#include "websocketclient.h"
#include <QComboBox>
#include <QDialog>
// 工作模式枚举
enum WorkMode {
    MODE_UDP_DUAL_SERVER,  // UDP双通道服务器模式
    MODE_UDP_DUAL_CLIENT,  // UDP双通道客户端模式
    MODE_LOCAL_FILE        // 本地文件读取模式
};

/**
 * @brief 主窗口
 *
 * 管理UDP双通道服务器/客户端/本地文件模式切换,显示双通道时频图
 * 左侧信息面板 + 中央频谱显示区域
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    int m_timeSlots = 0;
    // 固定10秒窗口写指针（满10秒清空重绘）
    bool m_windowInitialized = false;
    int  m_currentWriteSlot = 0;

private slots:
    // 文件菜单
    void onSpectrumTripletReady(const SpectrumResult &ns,
        const SpectrumResult &ew,
        const SpectrumResult &td);
    void onOpenFile();
    void onExit();

    // 采样率菜单
    void setSampleRate250k();
    void setSampleRate4M();

    // 模式菜单
    void switchToUDPDualServerMode();
    void switchToUDPDualClientMode();
    void switchToLocalFileMode();

    // 控制菜单(UDP双通道服务器)
    void onOpenCosFileDualServer();
    void onStartDualServerSending();
    void onStopDualServerSending();

    // 控制菜单(UDP双通道客户端)
    void onStartDualClientReceiving();
    void onStopDualClientReceiving();
    //void onSaveSTFTData();  // 保存STFT数据

    // WebSocket控制
    void onConnectWebSocket();
    void onDisconnectWebSocket();
    void onSendStartCommand();
    void onSendDisplayCommand();
    void onSendStopCollectCommand();
    void onSendStopDisplayCommand();

    // 状态栏订阅
    void onSubscribeStatus1s();
    void onSubscribeStatus5s();
    void onSubscribeStatus10s();

    // WebSocket回调
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketAckReceived(const QString &cmd, int code, const QString &message);
    void onWebSocketCommandAckReceived(const QString &cmd, int code, const QString &msg, const QJsonObject &data);  // 处理带数据负载的ACK
    void onWebSocketStatusReceived(const QJsonObject &statusData);
    void onWebSocketError(const QString &message);

    // 控制菜单(本地文件)
    void onOpenLocalFile();
    void onStartLocalProcessing();
    void onStopLocalProcessing();
    void onPauseLocalProcessing();

    // 时间文件解析
    void onTimeFileParseFinished(bool success, const QString &message);

    // 时间数据加载（合并了打开.dat文件、解析和加载三步）
    void onLoadTimeData();
    void onTimeDataLoaded(bool success, const QString &message);

    // 帮助菜单
    void onAbout();

    // 视图菜单
    void onToggleInfoPanel();
    void onAutoClearPlotToggled(bool checked);

    // 截图功能
    void onCaptureSpectrumScreenshots();

    // 数据处理
    void onServerPacketSent(int frameCount, quint32 sequence);
    void onClientBatchReady(int frameCount);
    void onClientStatsUpdated(const ReceiveStats &stats);

    // 本地文件处理
    void onLocalFileOpened(const QString &fileName, int totalFrames);
    void onLocalProgressChanged(double percent);
    void onLocalProcessingFinished();

    // FFT绘图更新
    void updateFFTPlots();
    void updateLocalFFTPlots();
    void updateSpectrumPlots();
    void updateSpectrumPlotsFromBatch(const SpectrogramBatch &batch, int slotIndexInBatch);
    void processSpectrumRenderQueue();  // 处理异步渲染队列  // 频谱图独立刷新（可配置）
    void updateSpectrogramPlots();  // 瀑布图独立刷新（50ms）
    // 子线程准备好的绘图数据
    void onSpectrogramBatchReady(const SpectrogramBatch &batch);

    // 新UI：频谱图点击切换
    void onSpectrumWidgetClicked(const QString &channelName);
    // 新UI：刷新间隔改变
    void onSpectrumRefreshIntervalChanged(int index);
    
private:
// int m_nextSpectrumSlotToDraw = 0;    // 下次要绘制的“全局slot号”
// int m_spectrumSlotInterval = 20; 
    //QTimer* m_spectrumDrawTimer;  // 频谱图绘制定时器（100ms）
    //int m_drawRefreshInterval = 100;    // 绘制刷新间隔（毫秒，默认100ms）
    quint64 m_totalDrawCount = 0;       // 成功绘制的总次数
    quint64 m_emptyQueueCount = 0;      // 队列为空的次数
    double m_sampleRate = 4000000.0;  // 默认4MHz（菜单可切换）
    // 已移到信息面板下拉框
    // QAction *sampleRate250kAction = nullptr;
    // QAction *sampleRate4MAction = nullptr;

    // ✅ 通道启用状态（默认全部启用）
    bool m_channelEWEnabled = true;
    bool m_channelNSEnabled = true;
    bool m_channelTDEnabled = true;

    QDialog *m_specDialog = nullptr;
    SpectrogramWidget *m_specDialogWidget = nullptr;
    void ensureSpectrogramDialog();

    void setupUI();
    void setupMenus();
    void updateMenusForMode();
    void updateStatusBar();
    void updateInfoPanel();
    void updateChannelVisibility();  // ✅ 更新通道显示/隐藏
    QString getChannelConfigFromView();  // 从视图菜单获取通道配置字符串
    bool saveSpectrumWidgetScreenshot(SpectrumWidget* widget, const QString& filename);  // 保存频谱图截图

    // UI组件
    QWidget *centralWidget;
    QHBoxLayout *mainLayout;
    InfoPanelWidget *infoPanel;
    QWidget *displayArea;

    // 新UI结构：上方三个小频谱图 + 下方一个大瀑布图
    SpectrumWidget *spectrumWidgetNS;
    SpectrumWidget *spectrumWidgetEW;
    SpectrumWidget *spectrumWidgetTD;
    SpectrogramWidget *spectrogramWidget;
    QComboBox *spectrumRefreshCombo;  // 频谱图刷新间隔选择
    // 频谱图上方控制按钮（由信息面板移出）
    QPushButton *m_collectButtonTop = nullptr;
    QPushButton *m_displayButtonTop = nullptr;
    bool m_collecting = false;
    bool m_displaying = false;

    // WebSocket 自动重连
    QTimer *m_wsReconnectTimer = nullptr;
    QString m_wsUrl = "ws://localhost:8189/ws";  // 修改为8189端口
    int m_wsReconnectIntervalMs = 2000;

    QString m_currentChannel;  // 当前选中的通道（"CH-A", "CH-B", "CH-C"）

    // 为每个通道缓存瀑布图数据（用于切换时重绘）
    QVector<QVector<double>> m_cachedSpectrogramNS;
    QVector<QVector<double>> m_cachedSpectrogramEW;
    QVector<QVector<double>> m_cachedSpectrogramTD;

    // 为每个通道缓存最新的频谱数据（用于同步更新）
    QVector<double> m_latestSpectrumNS_freq;
    QVector<double> m_latestSpectrumNS_data;
    QVector<double> m_latestSpectrumEW_freq;
    QVector<double> m_latestSpectrumEW_data;
    QVector<double> m_latestSpectrumTD_freq;
    QVector<double> m_latestSpectrumTD_data;

    // 标志位：记录每个通道是否有新数据待更新
    bool m_hasNewSpectrumNS = false;
    bool m_hasNewSpectrumEW = false;
    bool m_hasNewSpectrumTD = false;

    QLabel *statusModeLabel;
    QLabel *statusConnectionLabel;

    // 模式状态
    WorkMode currentMode;

    // 服务器状态
    QString currentFilePath;
    bool serverRunning;

    // 业务管理器
    UDPServerSender *udpDualServer;
    UDPClientReceiver *udpDualClient;
    LocalFileReader *localFileReader;
    WebSocketClient *webSocketClient;
    TimeFileParser *timeFileParser;
    TimeStampLoader *timeStampLoader;  // 时间戳加载器

    QTimer* m_spectrumDrawTimer;  // 频谱图绘制定时器（默认500ms）
    int m_drawRefreshInterval = 500;    // 绘制刷新间隔（毫秒，默认500ms）
    // FFT绘图定时器
    QTimer *fftPlotTimer;          // 瀑布图更新定时器（50ms）
    QTimer *spectrumPlotTimer;     // 频谱图更新定时器（已废弃，改为基于位置刷新）
    int currentTimeSlot;  // 当前时间槽索引（0-999）
    bool colorMapInitialized;  // colorMap是否已初始化大小
    QDateTime lastDrawnTimestamp;  // 最后绘制的时间槽对应的时间戳（用于时间轴更新）

    // ✅ 基于位置的频谱刷新（替代定时器）
    double m_nextSpectrumRefreshTime;   // 下一个刷新时间点（秒）
    double m_spectrumRefreshInterval;   // 刷新间隔（秒，例如0.05表示50ms）
    int m_currentHopSize;               // 当前使用的hop_size（用于位置计算）
    int m_spectrumRefreshCount;         // 频谱刷新计数器（调试用）

    // ✅ 异步频谱渲染队列
    struct SpectrumData {
        QVector<double> freq;
        QVector<double> data;
        int channel;  // PLOT_CH_NS, PLOT_CH_EW, PLOT_CH_TD
    };
    QQueue<SpectrumData> m_spectrumRenderQueue;  // 待渲染的频谱队列
    QTimer* m_spectrumRenderTimer;               // 异步渲染定时器

    // UDP 频谱绘制：二维矩阵（时间×频率），按界面刷新频率定时取一帧绘制（与老代码对齐）
    QVector<QVector<double>> m_spectrumMatrixNS;   // 每列 = 一帧频谱
    QVector<QVector<double>> m_spectrumMatrixEW;
    QVector<QVector<double>> m_spectrumMatrixTD;
    QVector<double> m_spectrumMatrixFreq;         // 频率轴（与列内长度一致）
    int m_spectrumMatrixMaxColumns = 2000;       // 最大列数，超出删最旧

    // 子线程：只做FFT结果整理/批数据准备，主线程仅绘制
    QThread *plotThread = nullptr;
    PlotWorker *plotWorker = nullptr;

    // ✅ 三通道同步绘制缓存（等三个通道都到齐后再一起绘制）
    SpectrogramBatch m_pendingBatchNS;
    SpectrogramBatch m_pendingBatchEW;
    SpectrogramBatch m_pendingBatchTD;
    bool m_hasPendingNS = false;
    bool m_hasPendingEW = false;
    bool m_hasPendingTD = false;
    int m_expectedStartSlot = -1;  // 期望的startSlot，用于同步检测
    void tryDrawSynchronizedBatches();  // 尝试绘制同步的批次
    void processSingleBatch(const SpectrogramBatch &batch,
                            SpectrumWidget* spectrumWidget,
                            const QString &channelName,
                            QVector<QVector<double>>* cachedData);  // 处理单个通道的batch

    // 本地文件处理线程（LocalFileReader + FFT + RingBuffer 生产者）
    QThread *localFileThread = nullptr;

    // 接收管线线程：UDPClientReceiver 在此线程运行，与主线程绘制解耦，提升处理速度
    QThread *m_receiverThread = nullptr;

    // ✅ 时间戳加载器线程（异步解析时间文件）
    QThread *timeStampLoaderThread = nullptr;

    // 菜单动作
    QAction *openCosFileDualServerAction;
    QAction *startDualServerAction;
    QAction *stopDualServerAction;
    QAction *startDualClientAction;
    QAction *stopDualClientAction;
    QAction *saveSTFTAction;  // 保存STFT数据动作
    QAction *openLocalFileAction;
    QAction *startLocalAction;
    QAction *stopLocalAction;
    QAction *pauseLocalAction;
    QAction *loadTimeDataAction;  // 加载时间数据（合并了打开和加载）
    QAction *toggleInfoPanelAction;
    QAction *autoClearPlotAction;

    // ✅ 通道配置菜单动作
    QAction *channelEWAction;  // EW通道开关
    QAction *channelNSAction;  // NS通道开关
    QAction *channelTDAction;  // TD通道开关

    // WebSocket菜单动作（连接/断开已移到信息面板按钮）
    // QAction *connectWebSocketAction;
    // QAction *disconnectWebSocketAction;
    QAction *subscribeStatus1sAction;
    QAction *subscribeStatus5sAction;
    QAction *subscribeStatus10sAction;
private slots:
    //void onSpectrumTripletReady(const SpectrumResult &ns,const SpectrumResult &ew,const SpectrumResult &td);
    void onDrawableSpectrumReady(quint64 frameIndex);  // 响应UDPClientReceiver的信号
    void onDrawTimerTimeout();  // 定时器触发，从队列取数据绘制
    void onPacketConfigChanged(int channelCount, double sampleRateHz);
};

#endif // MAINWINDOW_H