#ifndef WIFIMANAGER_H
#define WIFIMANAGER_H

#include <QObject>
#include <QVariantList>
#include <QTimer>
#include <QProcess>
#include <QMap>
#include <QSet>
#include <QMutex>

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

    // Scan parsing
    void parseWifiScanOutput(const QString &output);

    // Connection state machine helpers
    void startAddNetwork();
    void handleAddNetworkFinished();
    void startSetSsid();
    void handleSetSsidFinished();
    void startSetPskOrKeyMgmt();
    void handleSetPskOrKeyMgmtFinished();
    void startSaveConfig();
    void handleSaveConfigFinished();
    void startSelectNetwork();
    void handleSelectNetworkFinished();
    void handleFallbackReconfigureFinished();
    void handleFallbackReassociateFinished();

    // Async connection check
    void startAsyncConnectionCheck();
    void startIpCheck();
    void startSsidCheck();
    void finalizeConnectionCheck(const QString &ipAddress, const QString &ssid);

    // Helpers
    bool interfaceExists();
    bool interfaceIsUp();
    bool writeWpaSupplicantConfig(const QString &ssid, const QString &password);
    static QString generateHexPsk(const QString &ssid, const QString &password);
    static bool isHexPskString(const QString &str);
    void updateDetailedStatus(const QString &wpaState);

    // Status polling
    void startStatusPolling();
    void stopStatusPolling();
    void beginConnectPolling();

    // Interface watchdog
    void startInterfaceWatchdog();
    void stopInterfaceWatchdog();
    void onInterfaceWatchdogTimeout();

    // Saved networks
    void loadSavedNetworks();
    QString readSavedPassword(const QString &ssid) const;
    void updateSavedFlags();

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

    // DHCP renewal state (used when switching networks so the old AP's
    // lease is never displayed as the new network's IP)
    QProcess *m_dhcpProcess = nullptr;
    bool m_renewalPending = false;
    int m_renewalRetries = 0;
    void startDhcpRenewal();

    // Buffer for wpa_cli output
    QString m_wpaCLIBuffer;

    // Saved network SSIDs (loaded from /etc/wpa_supplicant.conf)
    QSet<QString> m_savedSSIDs;

    // Interface watchdog — poll wlan0 existence and wpa_cli health
    QTimer *m_interfaceWatchdog = nullptr;
    int m_watchdogDownCount = 0; // consecutive down checks (debounce)
    QString m_lastKnownSSID;     // SSID before interface loss, for auto-reconnect

    // Config file access mutex (serialize read/write to /etc/wpa_supplicant.conf)
    mutable QMutex m_configMutex;

    // Auto scan timer
    QTimer *m_autoScanTimer = nullptr;

    // Connection state machine
    int m_connectStep = 0; // 0=idle, 1=add_network, 2=set_ssid, 3=set_psk_or_keymgmt, 4=save_config, 5=select_network, 6=fallback_reconfigure, 7=fallback_reassociate
    QString m_pendingNetworkId;
    QString m_pendingPassword;
};

#endif // WIFIMANAGER_H
