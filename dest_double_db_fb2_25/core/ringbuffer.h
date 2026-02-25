#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <QVector>
#include <QMutex>
#include <QWaitCondition>

/**
 * @brief 线程安全的环形缓冲区
 *
 * 用于在生产者-消费者模式中存储数据
 * @tparam T 数据类型
 */
template<typename T>
class RingBuffer
{
public:
    explicit RingBuffer(int capacity = 100)
        : m_capacity(capacity)
        , m_readIndex(0)
        , m_writeIndex(0)
        , m_size(0)
    {
        m_buffer.resize(capacity);
    }

    ~RingBuffer() = default;

    /**
     * @brief 写入数据到缓冲区
     * @param data 要写入的数据
     * @return 是否成功写入
     */
    bool push(const T &data)
    {
        QMutexLocker locker(&m_mutex);

        if (m_size >= m_capacity) {
            // 缓冲区满，覆盖最老的数据
            m_buffer[m_writeIndex] = data;
            m_writeIndex = (m_writeIndex + 1) % m_capacity;
            m_readIndex = (m_readIndex + 1) % m_capacity;  // 读指针也向前移动
            m_condition.wakeOne();
            return true;
        }

        m_buffer[m_writeIndex] = data;
        m_writeIndex = (m_writeIndex + 1) % m_capacity;
        m_size++;
        m_condition.wakeOne();
        return true;
    }

    /**
     * @brief 写入数据到缓冲区(移动语义) - 性能优化,避免深拷贝
     * @param data 要移动的数据
     * @return 是否成功写入
     */
    bool push(T &&data)
    {
        QMutexLocker locker(&m_mutex);

        if (m_size >= m_capacity) {
            // 缓冲区满，覆盖最老的数据
            m_buffer[m_writeIndex] = std::move(data);
            m_writeIndex = (m_writeIndex + 1) % m_capacity;
            m_readIndex = (m_readIndex + 1) % m_capacity;  // 读指针也向前移动
            m_condition.wakeOne();
            return true;
        }

        m_buffer[m_writeIndex] = std::move(data);
        m_writeIndex = (m_writeIndex + 1) % m_capacity;
        m_size++;
        m_condition.wakeOne();
        return true;
    }

    /**
     * @brief 从缓冲区读取数据（阻塞）
     * @param data 输出参数，读取的数据
     * @param timeout 超时时间（毫秒），-1表示永久等待
     * @return 是否成功读取
     */
    bool pop(T &data, int timeout = -1)
    {
        QMutexLocker locker(&m_mutex);

        // 等待数据可用
        while (m_size == 0) {
            if (timeout < 0) {
                m_condition.wait(&m_mutex);
            } else {
                if (!m_condition.wait(&m_mutex, timeout)) {
                    return false;  // 超时
                }
            }
        }

        data = m_buffer[m_readIndex];
        m_readIndex = (m_readIndex + 1) % m_capacity;
        m_size--;
        return true;
    }

    /**
     * @brief 非阻塞写入
     * @param data 要写入的数据
     * @return 是否成功写入（false表示缓冲区已满）
     */
    bool tryPush(const T &data)
    {
        QMutexLocker locker(&m_mutex);

        if (m_size >= m_capacity) {
            return false;  // 缓冲区满，返回失败
        }

        m_buffer[m_writeIndex] = data;
        m_writeIndex = (m_writeIndex + 1) % m_capacity;
        m_size++;
        m_condition.wakeOne();
        return true;
    }

    /**
     * @brief 移动语义写入（避免深拷贝，性能优化）
     * @param data 要移动的数据
     * @return 是否成功写入
     */
    bool tryPushMove(T &&data)
    {
        QMutexLocker locker(&m_mutex);

        if (m_size >= m_capacity) {
            return false;  // 缓冲区满，返回失败
        }

        m_buffer[m_writeIndex] = std::move(data);  // 使用移动语义，避免拷贝
        m_writeIndex = (m_writeIndex + 1) % m_capacity;
        m_size++;
        m_condition.wakeOne();
        return true;
    }

    /**
     * @brief 非阻塞读取
     * @param data 输出参数，读取的数据
     * @return 是否成功读取
     */
    bool tryPop(T &data)
    {
        QMutexLocker locker(&m_mutex);

        if (m_size == 0) {
            return false;
        }

        data = m_buffer[m_readIndex];
        m_readIndex = (m_readIndex + 1) % m_capacity;
        m_size--;
        return true;
    }

    /**
     * @brief 清空缓冲区
     */
    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_readIndex = 0;
        m_writeIndex = 0;
        m_size = 0;
    }

    /**
     * @brief 获取当前缓冲区大小
     */
    int size() const
    {
        QMutexLocker locker(&m_mutex);
        return m_size;
    }

    /**
     * @brief 是否为空
     */
    bool isEmpty() const
    {
        QMutexLocker locker(&m_mutex);
        return m_size == 0;
    }

    /**
     * @brief 是否已满
     */
    bool isFull() const
    {
        QMutexLocker locker(&m_mutex);
        return m_size >= m_capacity;
    }

    /**
     * @brief 获取容量
     */
    int capacity() const
    {
        return m_capacity;
    }

private:
    QVector<T> m_buffer;
    int m_capacity;
    int m_readIndex;
    int m_writeIndex;
    int m_size;
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
};

#endif // RINGBUFFER_H
