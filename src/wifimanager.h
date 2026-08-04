#ifndef WIFIMANAGER_H
#define WIFIMANAGER_H

#include <QObject>
#include <QVariantList>
#include <QTimer>
#include <QProcess>
#include <QMap>
#include <QSet>

class WifiManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool isScanning READ isScanning NOTIFY isScanningChanged)
    Q_PROPERTY(bool isConnected READ isConnected NOTIFY isConnectedChanged)
    Q_PROPERTY(QString connectedSSID READ connectedSSID NOTIFY connectedInfoChanged)
    Q_PROPERTY(QString connectedIP READ connectedIP NOTIFY connectedInfoChanged)
    Q_PROPERTY(QString connectionStatus READ connectionStatus NOTIFY connectionStateChanged)
    Q_PROPERTY(QString lastDetailedStatus READ lastDetailedStatus NOTIFY detailedStatusChanged)
    Q_PROPERTY(QVariantList scanResults READ scanResults NOTIFY scanResultsChanged)

public:
    explicit WifiManager(QObject *parent = nullptr);
    ~WifiManager();

    // Properties
    bool isScanning() const { return m_isScanning; }
    bool isConnected() const { return m_isConnected; }
    QString connectedSSID() const { return m_connectedSSID; }
    QString connectedIP() const { return m_connectedIP; }
    QString connectionStatus() const { return m_connectionStatus; }
    QString lastDetailedStatus() const { return m_lastDetailedStatus; }
    QVariantList scanResults() const { return m_scanResults; }

    // Public slots (Q_INVOKABLE for QML)
    Q_INVOKABLE void scan();
    Q_INVOKABLE void connectToNetwork(const QString &ssid, const QString &password);
    Q_INVOKABLE void connectSaved(const QString &ssid);
    Q_INVOKABLE void disconnectFromNetwork();
    Q_INVOKABLE void forgetNetwork(const QString &ssid);
    Q_INVOKABLE void startAutoScan();
    Q_INVOKABLE void stopAutoScan();

private:
    struct ConfigBlock {
        QString content;
        bool hasSSID;
        QString ssid;
    };

    // Config file helpers
    static bool readWpaSupplicantConfig(const QString &path,
        QStringList &headerLines, QList<ConfigBlock> &networkBlocks,
        const QString &ssidToSkip = QString());
    static bool writeWpaSupplicantConfig(const QString &path,
        const QStringList &headerLines, const QList<ConfigBlock> &networkBlocks);
    static QString extractSSIDFromBlock(const QString &block);
    static bool blockContainsSSID(const QString &block, const QString &ssid);
signals:
    void isScanningChanged();
    void isConnectedChanged();
    void connectedInfoChanged();
    void connectionStateChanged();
    void detailedStatusChanged();
    void scanResultsChanged();

private slots:
    void onWifiScanReadyRead();
    void onWifiScanFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onWpaCLIReadyRead();
    void onWpaCLIFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onStatusPollingTimeout();
    void onScanTimeout();
    void onConnectTimeout();

private:
    void parseWifiScanOutput(const QString &output);
    void startStatusPolling();
    void stopStatusPolling();
    void startAsyncConnectionCheck();
    void startIpCheck();
    void loadSavedNetworks();
    QString readSavedPassword(const QString &ssid) const;
    void updateSavedFlags();
    void startSsidCheck();
    void finalizeConnectionCheck(const QString &ipAddress, const QString &ssid);
    bool interfaceExists();
    bool writeWpaSupplicantConfig(const QString &ssid, const QString &password);
    static QString generateHexPsk(const QString &ssid, const QString &password);
    static bool isHexPskString(const QString &str);
    void updateDetailedStatus(const QString &wpaState);

    // UI-related members
    bool m_isScanning = false;
    bool m_isConnected = false;
    QString m_connectedSSID;
    QString m_connectedIP;
    QString m_connectionStatus = "Disconnected";
    QString m_lastDetailedStatus;

    // Scan related
    QVariantList m_scanResults;
    QProcess *m_wifiScanProcess = nullptr;
    QString m_currentSSID;
    QString m_currentScanOutput;
    int m_connectAttemptCounter = 0;

    // Connection related
    QProcess *m_wpaCLIProcess = nullptr;

    // Status polling
    QTimer m_statusPollTimer;
    int m_pollingInterval = 2000; // 2 seconds
    int m_maxConnectWaitSeconds = 25;

    // Timeouts
    QTimer m_scanTimeoutTimer;
    QTimer m_connectTimeoutTimer;
    bool m_isConnecting = false;
    bool m_forgetMode = false;
    QString m_ssidToForget;

    // Async check connection processes
    QProcess *m_ipCheckProcess = nullptr;
    QProcess *m_ssidCheckProcess = nullptr;
    QString m_pendingIpAddress;
    QString m_pendingSsid;
    int m_checkStep = 0; // 0=idle, 1=waiting ip, 2=waiting ssid

    // Buffer for wpa_cli output
    QString m_wpaCLIBuffer;

    // Saved network SSIDs (loaded from /etc/wpa_supplicant.conf)
    QSet<QString> m_savedSSIDs;

    // Cache of saved networks
    QTimer *m_autoScanTimer = nullptr;
};

#endif // WIFIMANAGER_H
