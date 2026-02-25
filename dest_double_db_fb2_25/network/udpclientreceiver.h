#ifndef UDPCLIENTRECEIVER_H
#define UDPCLIENTRECEIVER_H

#include <QObject>
#include <QUdpSocket>
#include <QElapsedTimer>
#include <QTimer>
#include <QVector>
#include <QDateTime>
#include <QTimeZone>
#include <QThread>
#include "../core/ringbuffer.h"
#include "../core/reorderbuffer.h"
#include "../core/channelseparator.h"
#include "../processing/fftprocessor.h"
#include "../processing/fftworker.h"
#include "packetqueue.h"
#include "packetworker.h"
#include "udpreceiverworker.h"
#include "../protocol/packetformat.h"
#include "../protocol/framereassembler.h"

// 接收统计信息
struct ReceiveStats {
    qint64 receiveStartTime;     // 接收开始时间(ms)
    qint64 lastFetchTime;        // 最后一次取出时间(ms)
    int totalFramesReceived;     // 总接收帧数
    int totalPacketsReceived;    // 总接收包数
    int lostPackets;             // 丢包数
    int outOfOrderPackets;       // 乱序包数
    int duplicatePackets;        // 重复包数
    int resyncCount;             // 重同步次数
    int fetchCount;              // 取出次数
    int fftProcessCount;         // FFT处理次数

    // [诊断] PacketQueue 统计
    int queueCurrent;            // 当前队列深度
    int queueMaxDepth;           // 队列峰值深度
    quint64 queueDropped;        // 队列丢弃数
    quint64 queueEnqueued;       // 队列累计入队
    quint64 queueDequeued;       // 队列累计出队

    // [诊断] readyRead drain 统计
    quint64 slowDrains;          // 慢 drain 次数（>2ms）
    double readyReadMaxDrainMs;  // 最大 drain 耗时 (ms)
    quint64 readyReadTotalDrains;// 总 drain 次数
    quint64 readyReadTotalPackets;// 总 drain 读取包数
    int readyReadMaxBatch;       // 单次 drain 最大包数
};

/**
 * @brief UDP客户端接收器（扩展版 - play.md协议）
 *
 * 帧格式：[帧头4B][帧类型1B][帧序号4B][通道数1B][采样率1B][时间8B][数据936B]
 * 总包大小：955字节（19字节包头 + 936字节数据）
 *
 * 功能:
 * 1. 接收UDP数据包(955字节格式)
 * 2. 使用ReorderBuffer处理乱序
 * 3. 使用ChannelSeparator分离EW和NS通道
 * 4. 存入ringbuffer
 * 5. UDP心跳检测（帧类型=0，数据全为0）
 * 6. 支持通道配置（通道数2或3）
 * 7. 支持采样率切换（0=250kHz，1=4MHz）
 */
class UDPClientReceiver : public QObject
{
    Q_OBJECT
public:
    explicit UDPClientReceiver(QObject *parent = nullptr);
    ~UDPClientReceiver();

    // 初始化接收器（Q_INVOKABLE 供跨线程 invokeMethod 调用）
    Q_INVOKABLE bool initialize(quint16 port);

    // 开始接收
    Q_INVOKABLE void startReceiving();

    // 停止接收
    Q_INVOKABLE void stopReceiving();

    // 获取原始数据ringbuffer引用（24位数据存储为quint32）
    RingBuffer<QVector<quint32>>& getEWDataBuffer() { return m_ewDataBuffer; }
    RingBuffer<QVector<quint32>>& getNSDataBuffer() { return m_nsDataBuffer; }
    RingBuffer<QVector<quint32>>& getTDDataBuffer() { return m_tdDataBuffer; }

    // 获取FFT结果ringbuffer引用
    RingBuffer<SpectrumResult>& getEWFFTBuffer() { return m_fftWorker->getEWFFTBuffer(); }
    RingBuffer<SpectrumResult>& getNSFFTBuffer() { return m_fftWorker->getNSFFTBuffer(); }
    RingBuffer<SpectrumResult>& getTDFFTBuffer() { return m_fftWorker->getTDFFTBuffer(); }

    // 获取统计信息
    ReceiveStats getStats() const { return m_stats; }

    // 获取最新数据包时间戳（东八区本地时间）
    QDateTime getLastPacketTimestamp() const { return m_lastPacketTimestamp; }

    // 清空缓冲区
    void clear();

    // 重置整个数据管线（原始数据 + FFT处理器 + FFT结果）
    void resetPipeline();

    // 设置是否使用帧内分包模式
    void setUseSubPacketMode(bool enable) { m_useSubPacketMode = enable; }

    // 设置是否使用新的数据包格式（955字节格式）
    Q_INVOKABLE void setUseNewPacketFormat(bool enable) { m_useNewPacketFormat = enable; }

    // play.md协议扩展
    Q_INVOKABLE void setChannelConfig(const QString& channelConfig);  // 设置通道配置（三位二进制）
    void setUDPHeartbeatTimeout(int timeoutMs);           // 设置UDP心跳超时（默认5000ms）

    // 设置采样率（应用到FFT处理器）
    Q_INVOKABLE void setSampleRate(double fsHz);

    // 设置分析步进（应用到FFT处理器）
    void setAnalysisHop(int hop);

    // STFT数据保存
    bool saveSTFTDataToCSV(const QString &outputDir, const QString &fileBaseName);

    // 绘制队列数据结构（公开，供MainWindow使用）
    struct DrawableSpectrum {
        quint64 frameIndex;         // 触发绘制时的帧索引
        SpectrumResult ewSpectrum;  // EW通道FFT结果
        SpectrumResult nsSpectrum;  // NS通道FFT结果
        SpectrumResult tdSpectrum;  // TD通道FFT结果（可选）
        bool hasEW;
        bool hasNS;
        bool hasTD;
    };
    
    // 获取绘制队列中的数据（由MainWindow调用）
    bool popDrawableSpectrum(DrawableSpectrum& result);
    
    // 设置绘制帧间隔
    void setDrawFrameInterval(int interval) { m_drawFrameInterval = interval; }
    
    // 检查是否正在接收
    bool isReceiving() const { return m_isReceiving; }
signals:
    void drawableSpectrumReady(quint64 frameIndex); 
    void spectrumTripletReady(const SpectrumResult &ns,
        const SpectrumResult &ew,
        const SpectrumResult &td);  // 新增转发信号
    void dataReceived(int frameCount);
    void batchReady(int frameCount);  // 每满5帧发出信号

    void error(const QString &message);
    void statsUpdated(const ReceiveStats &stats);
    void udpHeartbeatReceived();      // 收到UDP心跳
    void udpTimeout();                // UDP超时
    void udpLinkLost();               // UDP链路丢失
    void dataLoopRestarted();         // 数据循环重启（时间戳回到起点）
   void packetConfigChanged(int channelCount, double sampleRateHz);
private slots:
    void checkAndFetchBatch();  // 检查并取出一批数据
    void processBatchFFT();     // 批量处理FFT（定时器触发）
    void onOrderedPacketReady(quint32 packetNumber, const QByteArray& data);  // 重排序包就绪
    void checkUDPTimeout();     // 检测UDP超时
    void onPacketParsed(const ParsedPacket& packet);  // 已废弃，仅保留兼容；实际使用 onSeparatedFrames
    void onSeparatedFrames(const SeparatedFrame& frame);  // 通道已在 worker 线程分离，主线程只 push/统计
    void onHeartbeat(quint32 seq);
    void onPacketsLost(quint32 count);
    void onRecvWorkerInitialized(bool success);  // UDP接收 worker 初始化完成

    private:
    // 帧索引追踪（用于按帧绘制）
    quint64 m_receivedFrameIndex;  // 接收到的累积帧索引（从1开始）
    quint64 m_lastDrawFrameIndex;  // 上次触发绘制的帧索引
    int m_drawFrameInterval;       // 绘制帧间隔（默认100）
    
    // 绘制队列（存储待绘制的FFT结果）
    QQueue<DrawableSpectrum> m_drawQueue;  // 绘制队列
    QMutex m_drawQueueMutex;               // 队列保护
    int m_maxDrawQueueSize;                // 队列最大长度（默认10）
    quint16 m_port;
    bool m_isReceiving;

    // UDP 接收专用线程
    QThread* m_recvThread;
    UdpReceiverWorker* m_recvWorker;

    // 原始数据环形缓冲区（24位数据）
    RingBuffer<QVector<quint32>> m_ewDataBuffer;
    RingBuffer<QVector<quint32>> m_nsDataBuffer;
    RingBuffer<QVector<quint32>> m_tdDataBuffer;

    // FFT处理专用线程
    QThread* m_fftThread;
    FFTWorker* m_fftWorker;

    // 统计信息
    ReceiveStats m_stats;
    QElapsedTimer m_elapsedTimer;
    quint32 m_lastSequence;  // 上一个包的序列号
    bool m_firstPacket;      // 是否是第一个包
    bool m_everReceived;     // 是否曾经接收过数据(用于控制日志打印)

    // 临时缓存(用于累积5帧)
    int m_currentFrameCount;

    // FFT处理定时器
    QTimer *m_fftTimer;

    // 帧内分包模式支持
    bool m_useSubPacketMode;           // 是否使用帧内分包模式
    FrameReassembler m_frameReassembler;  // 帧重组器
    quint32 m_nextExpectedFrame;       // 下一个期望的帧号

    // 新格式支持
    bool m_useNewPacketFormat;         // 是否使用新的955字节格式
    QVector<quint16> m_newFormatNSBuffer;  // 新格式NS通道重组缓冲区
    QVector<quint16> m_newFormatEWBuffer;  // 新格式EW通道重组缓冲区
    int m_newFormatPacketsReceived;    // 新格式当前帧已接收的包数

    // 处理帧内分包模式的数据包
    void processSubPacket(const QByteArray &datagram);

    // 处理传统模式的数据包
    void processTraditionalPacket(const QByteArray &datagram);

    // 处理新格式的数据包（955字节）
    void processNewFormatPacket(const QByteArray &datagram);
    void processNewFormatPacketLegacy(const QByteArray &datagram);

    // 检查是否是心跳包
    bool isHeartbeatPacket(const QByteArray& datagram);

    // [诊断] 更新诊断统计字段
    void updateDiagnosticStats();

    // 解析时间戳并转换为东八区本地时间
    QDateTime parseTimestamp(const PacketTimestamp& ts);

    // 重置传统模式状态
    void resetTraditionalState();

    // 重置新格式状态
    void resetNewFormatState();

    static constexpr int SAMPLES_PER_FRAME = 5000;
    static constexpr int FRAMES_PER_BATCH = 48;   // 单批帧数：较小则延迟低、结果更频繁入队（与界面刷新率无关）
    static constexpr int RINGBUF_CAPACITY = 1280000;

    // 新格式丢包检测配置
    static constexpr qint32 PACKET_GAP_THRESHOLD = 5000;        // 包号差值阈值
    static constexpr int PACKET_ANOMALY_LOG_INTERVAL = 100;     // 异常日志打印间隔

    // play.md协议扩展成员
    ReorderBuffer* m_reorderBuffer;           // 重排序缓冲区
    QString m_currentChannelConfig;           // 当前通道配置（三位二进制）
    QTimer* m_udpTimeoutTimer;                // UDP超时检测定时器
    QElapsedTimer m_lastUDPPacketTime;        // 最后一次收到UDP包的时间
    int m_udpTimeoutMs;                       // UDP超时时间（毫秒）
    QDateTime m_lastPacketTimestamp;          // 最新数据包时间戳（东八区本地时间）
    PacketTimestamp m_firstTimestampRaw;       // 第一个数据包的原始时间戳（8字节）
    bool m_hasFirstTimestamp;                  // 是否已记录第一个时间戳
    quint64 m_packetsSinceLoopDetect;          // 上次循环检测后收到的包数

    // 新格式丢包检测状态
    quint32 m_lastNewFmtPacketNumber;         // 上一个新格式包的包号（max-last）
    bool m_hasLastNewFmtPacketNumber;         // 是否已收到第一个新格式包
    quint64 m_newFmtAnomalyCount;             // 新格式异常计数（用于限频日志）

    PacketQueue* m_packetQueue;
    PacketWorker* m_packetWorker;
    QThread* m_workerThread;
};

#endif // UDPCLIENTRECEIVER_H
