#include "uint24.h"
#include <QDebug>

int Uint24::decode(const QByteArray& data, QVector<quint32>& output)
{
    if (data.size() % 3 != 0) {
        qWarning() << "[Uint24] 数据长度不是3的倍数:" << data.size();
        return 0;
    }

    int sampleCount = data.size() / 3;
    output.resize(sampleCount);

    const quint8* bytes = reinterpret_cast<const quint8*>(data.constData());

    for (int i = 0; i < sampleCount; i++) {
        output[i] = fromBytes(bytes + i * 3);
    }

    return sampleCount;
}

int Uint24::encode(const QVector<quint32>& input, QByteArray& output)
{
    int sampleCount = input.size();
    output.resize(sampleCount * 3);

    quint8* bytes = reinterpret_cast<quint8*>(output.data());

    for (int i = 0; i < sampleCount; i++) {
        toBytes(input[i] & MAX_VALUE, bytes + i * 3);
    }

    return output.size();
}

int Uint24::decodeToDouble(const QByteArray& data, QVector<double>& output, double offset)
{
    if (data.size() % 3 != 0) {
        qWarning() << "[Uint24] 数据长度不是3的倍数:" << data.size();
        return 0;
    }

    int sampleCount = data.size() / 3;
    output.resize(sampleCount);

    const quint8* bytes = reinterpret_cast<const quint8*>(data.constData());

    for (int i = 0; i < sampleCount; i++) {
        quint32 value = fromBytes(bytes + i * 3);
        output[i] = static_cast<double>(value) - offset;
    }

    return sampleCount;
}
