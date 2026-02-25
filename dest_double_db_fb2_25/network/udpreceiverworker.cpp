#include "udpreceiverworker.h"
#include <QDebug>
#include <QDateTime>
#include <QThread>

UdpReceiverWorker::UdpReceiverWorker(quint16 port, PacketQueue* queue, QObject* parent)
    : QObject(parent)
    , m_socket(nullptr)
    , m_port(port)
    , m_queue(queue)
    , m_totalDrains(0)
    , m_totalPackets(0)
    , m_lastDiagPrint(0)
{
}

UdpReceiverWorker::~UdpReceiverWorker()
{
    if (m_socket) {
        m_socket->close();
        delete m_socket;
        m_socket = nullptr;
    }
}

bool UdpReceiverWorker::initialize()
{
    qDebug() << "[DIAG-BIND] initialize() 被调用"
             << "线程ID:" << QThread::currentThreadId()
             << "时间:" << QDateTime::currentDateTime().toString("HH:mm:ss.zzz");

    // 提升接收线程优先级为最高，减少被操作系统调度暂停的概率
    QThread* currentThread = QThread::currentThread();
    currentThread->setPriority(QThread::TimeCriticalPriority);
    qDebug() << "[UdpReceiverWorker] 线程优先级已设置为 TimeCriticalPriority";

    // 预分配接收缓冲区，避免每次接收都分配内存
    m_reusableBuffer.resize(MAX_DATAGRAM_SIZE);

    // 在 worker 线程里创建 socket
    m_socket = new QUdpSocket(this);

    // 先 bind，再设置缓冲区（Windows要求）
    bool bindOk = m_socket->bind(QHostAddress::Any, m_port);

    qDebug() << "[DIAG-BIND]"
             << "端口:" << m_port
             << "结果:" << (bindOk ? "成功" : "失败")
             << "本地地址:" << m_socket->localAddress().toString()
             << "本地端口:" << m_socket->localPort()
             << "错误码:" << m_socket->error()
             << "错误信息:" << m_socket->errorString();

    if (!bindOk) {
        QString errMsg = QString("无法绑定端口 %1: %2").arg(m_port).arg(m_socket->errorString());
        qCritical() << "[UdpReceiverWorker] bind 失败:" << errMsg;
        emit error(errMsg);
        emit initialized(false);
        return false;
    }
    // ✅ 在绑定成功后，从同一个 socket 发送注册包给服务器
    QHostAddress serverIp("127.0.0.1");   // 和 Go 客户端里的 serverIP 一致
    quint16 serverPort = 5555;            // 和 serverPort 一致
    QByteArray reg("register");
    qint64 sent = m_socket->writeDatagram(reg, serverIp, serverPort);
    qDebug() << "[UdpReceiverWorker] 发送注册包到"
             << serverIp.toString() << ":" << serverPort
             << "size=" << sent;



    // bind 成功后再设置 128MB 接收缓冲区（防止内核层丢包）
    const int requestedSize = 128 * 1024 * 1024;
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, requestedSize);

    // 验证实际设置的缓冲区大小（操作系统可能有上限）
    QVariant actualSizeVariant = m_socket->socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption);
    int actualSize = actualSizeVariant.toInt();

    qDebug() << "[UdpReceiverWorker] Socket接收缓冲区"
             << "请求:" << requestedSize << "字节 (" << (requestedSize / 1024 / 1024) << "MB)"
             << "实际:" << actualSize << "字节 (" << (actualSize / 1024 / 1024) << "MB)";

    if (actualSize < requestedSize / 2) {
        qWarning() << "[UdpReceiverWorker] Socket缓冲区可能不足！"
                   << "实际大小仅为请求的" << (actualSize * 100 / requestedSize) << "%"
                   << "可能导致丢包，建议检查系统限制";
    }

    // 设置低延迟优先级
    m_socket->setSocketOption(QAbstractSocket::TypeOfServiceOption, 0x10);
    
    // 连接 readyRead 信号
    connect(m_socket, &QUdpSocket::readyRead, this, &UdpReceiverWorker::onReadyRead);

    qDebug() << "[UdpReceiverWorker] Socket 已绑定到端口" << m_port
             << "（线程ID:" << QThread::currentThreadId() << "）";

    emit initialized(true);
    return true;
}

void UdpReceiverWorker::onReadyRead()
{
    int batchCount = 0;

    while (m_socket->hasPendingDatagrams()) {
        qint64 size = m_socket->pendingDatagramSize();
        if (size > MAX_DATAGRAM_SIZE) {
            // 包太大，跳过
            m_socket->readDatagram(nullptr, 0);
            continue;
        }

        // 使用预分配缓冲区，避免每次分配
        qint64 n = m_socket->readDatagram(m_reusableBuffer.data(), size);
        if (n < 8) {
            continue;
        }

        // 创建实际大小的副本并移动到队列（缓冲区会被重用）
        QByteArray datagram(m_reusableBuffer.constData(), n);
        m_queue->enqueue(std::move(datagram));
        batchCount++;
    }

    // 轻量统计
    m_totalDrains++;
    m_totalPackets += batchCount;

    // [限频诊断] 每 5 秒最多打印一次（仅在队列压力大时）
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastDiagPrint > 5000) {
        int queueDepth = m_queue->size();
        if (queueDepth > 2048) {
            m_lastDiagPrint = now;
            qWarning() << "[RecvWorker] 队列积压:" << queueDepth
                       << "本次 drain:" << batchCount << "包";
        }
    }
}
