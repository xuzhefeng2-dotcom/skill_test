#ifndef INFOPANELWIDGET_H
#define INFOPANELWIDGET_H

#include <QWidget>
#include <QLabel>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QProgressBar>
#include <QTimer>
#include <QDateTime>

/**
 * @brief 左侧信息面板组件
 *
 * 显示:
 * - 时间 (本地时间)
 * - 位置 (纬度、经度、高度、磁偏角)
 * - 系统 (系统温度、湿度)
 * - 信号 (底噪、GNSS卫星信息)
 * - 工作模式
 */
class InfoPanelWidget : public QWidget
{
    Q_OBJECT
public:
    explicit InfoPanelWidget(QWidget *parent = nullptr);

    // 时间相关
    void setUTCTime(const QDateTime &time);
    void enableAutoTimeUpdate(bool enable);

    // 位置相关
    void setLatitude(const QString &lat);
    void setLongitude(const QString &lon);
    void setAltitude(const QString &alt);
    void setMagneticDeclination(const QString &decl);

    // 新增：从JSON状态数据更新位置信息
    void setGPSInfo(int degree, int minute, int decimal, bool isNorth);
    void setLongitudeInfo(int degree, int minute, int decimal, bool isEast);
    void setAltitudeInfo(int integer, int decimal);
    void setMagneticDeclinationInfo(int integer, int decimal, bool isEast);

    // 系统相关
    void setSystemTemperature(double tempC);
    void setHumidity(double percent);

    // 信号相关
    void setNoiseFloor(double dbm);
    void setGNSSSatellites(int gps, int bds, int glonass, int galileo);

    // 工作模式
    void setWorkMode(const QString &mode);

    // 新增：从JSON状态数据更新所有信息
    void updateFromStatusData(const QJsonObject &statusData);

    void setFpgaVersion(const QString &ver);
signals:

private slots:
    void updateTime();

private:
    void setupUI();
    QGroupBox* createGroupBox(const QString &title);
    QLabel* createValueLabel(const QString &text);

    // 时间组
    QLabel *m_utcTimeLabel;
    QTimer *m_timeTimer;

    // 位置组
    QLabel *m_latitudeLabel;
    QLabel *m_longitudeLabel;
    QLabel *m_altitudeLabel;
    QLabel *m_magneticDeclinationLabel;

    // 系统组
    QLabel *m_temperatureLabel;
    QLabel *m_humidityLabel;  // 新增：湿度标签

    // 信号组
    QLabel *m_noiseFloorLabel;
    QLabel *m_gnssLabel;  // GNSS卫星信息标签

    // 工作模式组
    QLabel *m_workModeLabel;

    QWidget *m_versionBadge = nullptr;
    QLabel  *m_versionLabel = nullptr;

    static const int PANEL_WIDTH = 320;
};

#endif // INFOPANELWIDGET_H
