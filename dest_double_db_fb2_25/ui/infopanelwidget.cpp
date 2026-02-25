#include "infopanelwidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStorageInfo>
#include <QJsonObject>
#include <QJsonValue>

InfoPanelWidget::InfoPanelWidget(QWidget *parent)
    : QWidget(parent)
    , m_timeTimer(nullptr)
{
    setupUI();
    enableAutoTimeUpdate(true);
}

void InfoPanelWidget::setupUI()
{
    setFixedWidth(PANEL_WIDTH);
    setStyleSheet(R"(
        QWidget {
            background-color: #1e1e1e;
            font-family: "Microsoft YaHei", "SimSun", sans-serif;
        }
        QGroupBox {
            font-weight: bold;
            font-size: 12pt;
            border: 1px solid #3a3a3a;
            border-radius: 5px;
            margin-top: 5px;
            padding-top: 5px;
            background-color: #2d2d2d;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 10px;
            top: -10px;
            padding: 2px 8px;
            color: #c0c0c0;
            background-color: #2d2d2d;
        }
        QLabel {
            font-size: 11pt;
            color: #e0e0e0;
            padding: 2px;
        }
    )");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // ========== 时间组 ==========
    QGroupBox *timeGroup = createGroupBox("");
    QVBoxLayout *timeLayout = new QVBoxLayout(timeGroup);
    timeLayout->setSpacing(0);
    timeLayout->setContentsMargins(8, 8, 8, 8);
    m_utcTimeLabel = createValueLabel("UTC时间: --");
    timeLayout->addWidget(m_utcTimeLabel);
    mainLayout->addWidget(timeGroup);

    // ========== 位置组 ==========
    QGroupBox *positionGroup = createGroupBox("");
    QVBoxLayout *positionLayout = new QVBoxLayout(positionGroup);
    positionLayout->setSpacing(0);
    positionLayout->setContentsMargins(8, 8, 8, 8);

    m_latitudeLabel = createValueLabel("纬度: --");
    m_longitudeLabel = createValueLabel("经度: --");
    m_altitudeLabel = createValueLabel("高度: --");
    m_magneticDeclinationLabel = createValueLabel("磁偏角: --");

    positionLayout->addWidget(m_latitudeLabel);
    positionLayout->addWidget(m_longitudeLabel);
    positionLayout->addWidget(m_altitudeLabel);
    positionLayout->addWidget(m_magneticDeclinationLabel);
    mainLayout->addWidget(positionGroup);

    // ========== 系统组 ==========
    QGroupBox *systemGroup = createGroupBox("");
    QVBoxLayout *systemLayout = new QVBoxLayout(systemGroup);
    systemLayout->setSpacing(0);
    systemLayout->setContentsMargins(8, 8, 8, 8);
    m_temperatureLabel = createValueLabel("系统温度: --");
    m_humidityLabel = createValueLabel("湿度: --");
    systemLayout->addWidget(m_temperatureLabel);
    systemLayout->addWidget(m_humidityLabel);
    mainLayout->addWidget(systemGroup);

    // ========== 信号组 ==========
    QGroupBox *signalGroup = createGroupBox("");
    QVBoxLayout *signalLayout = new QVBoxLayout(signalGroup);
    signalLayout->setSpacing(0);
    signalLayout->setContentsMargins(8, 8, 8, 8);
    m_noiseFloorLabel = createValueLabel("底噪: --");
    m_gnssLabel = createValueLabel("GNSS:\n  GPS: --\n  BDS: --\n  GLO: --\n  GAL: --");
    signalLayout->addWidget(m_noiseFloorLabel);
    signalLayout->addWidget(m_gnssLabel);
    mainLayout->addWidget(signalGroup);

    // 添加弹簧
    mainLayout->addStretch();

    mainLayout->addStretch();
    // ===== 版本徽标（左下角）=====
    m_versionBadge = new QWidget(this);
    m_versionBadge->setObjectName("versionBadge");

    QHBoxLayout *badgeLayout = new QHBoxLayout(m_versionBadge);
    badgeLayout->setContentsMargins(10, 6, 10, 6);
    badgeLayout->setSpacing(8);

    // 小圆点（像 LED）
    QLabel *dot = new QLabel("●", m_versionBadge);
    dot->setObjectName("versionDot");

    m_versionLabel = new QLabel("FPGA v1.6.3", m_versionBadge);
    m_versionLabel->setObjectName("versionText");

    badgeLayout->addWidget(dot);
    badgeLayout->addWidget(m_versionLabel);
    badgeLayout->addStretch(0);

    // 徽标整体靠左
    m_versionBadge->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

    // 徽标样式：渐变底色 + 圆角 + 描边
    m_versionBadge->setStyleSheet(R"(
    #versionBadge {
        background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                    stop:0 #2E2E2E, stop:1 #3A3A3A);
        border: 1px solid #5C5C5C;
        border-radius: 10px;
    }
    #versionText {
        color: #F5D76E;
        font-size: 11.5pt;
        font-weight: 800;
        letter-spacing: 0.6px;
    }
    #versionDot {
        color: #7CFC98;   /* 绿色小灯 */
        font-size: 12pt;
    }
    )");

    // 放到底部
    mainLayout->addWidget(m_versionBadge, 0, Qt::AlignLeft);
}

void InfoPanelWidget::setFpgaVersion(const QString &ver)
{
    if (m_versionLabel) m_versionLabel->setText(ver);
}

QGroupBox* InfoPanelWidget::createGroupBox(const QString &title)
{
    QGroupBox *group = new QGroupBox(title);
    return group;
}

QLabel* InfoPanelWidget::createValueLabel(const QString &text)
{
    QLabel *label = new QLabel(text);
    label->setWordWrap(true);
    return label;
}

void InfoPanelWidget::setUTCTime(const QDateTime &time)
{
    m_utcTimeLabel->setText("UTC时间: " + time.toUTC().toString("yyyy-MM-dd HH:mm:ss"));
}

void InfoPanelWidget::enableAutoTimeUpdate(bool enable)
{
    if (enable) {
        if (!m_timeTimer) {
            m_timeTimer = new QTimer(this);
            connect(m_timeTimer, &QTimer::timeout, this, &InfoPanelWidget::updateTime);
        }
        m_timeTimer->start(1000);
        updateTime();
    } else {
        if (m_timeTimer) {
            m_timeTimer->stop();
        }
    }
}

void InfoPanelWidget::updateTime()
{
    QDateTime utcNow = QDateTime::currentDateTimeUtc();
    setUTCTime(utcNow);
}

void InfoPanelWidget::setLatitude(const QString &lat)
{
    m_latitudeLabel->setText("纬度: " + lat);
}

void InfoPanelWidget::setLongitude(const QString &lon)
{
    m_longitudeLabel->setText("经度: " + lon);
}

void InfoPanelWidget::setAltitude(const QString &alt)
{
    m_altitudeLabel->setText("高度: " + alt);
}

void InfoPanelWidget::setMagneticDeclination(const QString &decl)
{
    m_magneticDeclinationLabel->setText("磁偏角: " + decl);
}

void InfoPanelWidget::setSystemTemperature(double tempC)
{
    m_temperatureLabel->setText(QString("系统温度: %1°C").arg(tempC, 0, 'f', 1));
}

void InfoPanelWidget::setNoiseFloor(double dbm)
{
    m_noiseFloorLabel->setText(QString("底噪: %1 dBm").arg(dbm, 0, 'f', 0));
}

void InfoPanelWidget::setWorkMode(const QString &mode)
{
    // 工作模式显示已移除（仅保留接口以兼容旧调用）
    Q_UNUSED(mode);
}

// 新增方法实现

void InfoPanelWidget::setGPSInfo(int degree, int minute, int decimal, bool isNorth)
{
    // decimal是5位小数：22345 表示 22.345'
    QString formatted = QString("%1°%2'%3.%4\"%5")
        .arg(degree)
        .arg(minute, 2, 10, QChar('0'))
        .arg(decimal / 1000, 2, 10, QChar('0'))
        .arg(decimal % 1000, 3, 10, QChar('0'))
        .arg(isNorth ? "N" : "S");

    m_latitudeLabel->setText("纬度: " + formatted);
}

void InfoPanelWidget::setLongitudeInfo(int degree, int minute, int decimal, bool isEast)
{
    QString formatted = QString("%1°%2'%3.%4\"%5")
        .arg(degree)
        .arg(minute, 2, 10, QChar('0'))
        .arg(decimal / 1000, 2, 10, QChar('0'))
        .arg(decimal % 1000, 3, 10, QChar('0'))
        .arg(isEast ? "E" : "W");

    m_longitudeLabel->setText("经度: " + formatted);
}

void InfoPanelWidget::setAltitudeInfo(int integer, int decimal)
{
    QString formatted = QString("%1.%2m")
        .arg(integer)
        .arg(decimal);

    m_altitudeLabel->setText("高度: " + formatted);
}

void InfoPanelWidget::setMagneticDeclinationInfo(int integer, int decimal, bool isEast)
{
    QString formatted = QString("%1.%2° %3")
        .arg(integer)
        .arg(decimal)
        .arg(isEast ? "E" : "W");

    m_magneticDeclinationLabel->setText("磁偏角: " + formatted);
}

void InfoPanelWidget::setHumidity(double percent)
{
    m_humidityLabel->setText(QString("湿度: %1%").arg(percent, 0, 'f', 1));
}

void InfoPanelWidget::setGNSSSatellites(int gps, int bds, int glonass, int galileo)
{
    QString text = QString("GNSS:\n  GPS: %1\n  BDS: %2\n  GLO: %3\n  GAL: %4")
        .arg(gps)
        .arg(bds)
        .arg(glonass)
        .arg(galileo);

    m_gnssLabel->setText(text);
}

void InfoPanelWidget::updateFromStatusData(const QJsonObject &statusData)
{
    // 更新温度
    if (statusData.contains("temperature")) {
        setSystemTemperature(statusData["temperature"].toDouble());
    }

    // 更新湿度
    if (statusData.contains("humidity")) {
        setHumidity(statusData["humidity"].toDouble());
    }

    // 更新纬度
    if (statusData.contains("latitude")) {
        QJsonObject lat = statusData["latitude"].toObject();
        setGPSInfo(
            lat["degree"].toInt(),
            lat["minute"].toInt(),
            lat["decimal"].toInt(),
            lat["ns"].toInt() == 1
        );
    }

    // 更新经度
    if (statusData.contains("longitude")) {
        QJsonObject lon = statusData["longitude"].toObject();
        setLongitudeInfo(
            lon["degree"].toInt(),
            lon["minute"].toInt(),
            lon["decimal"].toInt(),
            lon["ew"].toInt() == 1
        );
    }

    // 更新海拔
    if (statusData.contains("altitude")) {
        QJsonObject alt = statusData["altitude"].toObject();
        setAltitudeInfo(
            alt["integer"].toInt(),
            alt["decimal"].toInt()
        );
    }

    // 更新磁偏角
    if (statusData.contains("magnetic_declination")) {
        QJsonObject mag = statusData["magnetic_declination"].toObject();
        setMagneticDeclinationInfo(
            mag["integer"].toInt(),
            mag["decimal"].toInt(),
            mag["ew"].toInt() == 1
        );
    }

    // 更新GNSS卫星信息
    if (statusData.contains("gnss_satellites")) {
        QJsonObject gnss = statusData["gnss_satellites"].toObject();
        setGNSSSatellites(
            gnss["gps"].toInt(),
            gnss["bds"].toInt(),
            gnss["glonass"].toInt(),
            gnss["galileo"].toInt()
        );
    }
}
