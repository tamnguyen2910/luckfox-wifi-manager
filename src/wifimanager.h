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
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
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
    QString lastError() const { return m_lastError; }
    QVariantList scanResults() const { return m_scanResults; }

    // Public slots (Q_INVOKABLE for QML)
    Q_INVOKABLE void scan();
    Q_INVOKABLE void connectToNetwork(const QString &ssid, const QString &password, bool passwordIsHex = false);
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
    void lastErrorChanged();
    void scanResultsChanged();

private slots:
    void onWifiScanReadyRead();
    void onWifiScanFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onWpaCLIReadyRead();
    void onWpaCLIFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onStatusPollingTimeout();
    void onScanTimeout();
    void onConnectTimeout();
    void onConnectTotalTimeout();
    void onCheckTimeout(); // P1: 5s guard for ip addr show / wpa_cli status hangs

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

    // Ensure wpa_supplicant is running (start it if the control socket is missing)
    static void ensureWpaSupplicant();

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
    static bool isValidSSID(const QString &ssid);

    // Unified teardown for a failed/aborted connect flow
    void abortConnect(const QString &reason);

    // Debug: headless input simulation (reads /tmp/wifi_sim_cmd)
    void pollSimCommands();
    void execSimCommand(const QString &cmd);
    void logStatus();

    // Status polling
    void startStatusPolling();
    void stopStatusPolling();
    void beginConnectPolling();

    // Interface watchdog
    void startInterfaceWatchdog();
    void stopInterfaceWatchdog();
    void onInterfaceWatchdogTimeout();
    void startInterfaceRecovery();

    // Auto-scan
    void resumeAutoScan();

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
    QString m_lastError; // transient error message shown in the error bar (RF-47)

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
    QTimer m_connectTimeoutTimer; // fallback path hang-guard (12s)
    QTimer m_connectTotalTimer;   // global connect timeout (90s)
    QTimer m_checkTimeoutTimer;   // P1: 5s guard for ip addr show / wpa_cli status hangs
    static constexpr int kCheckTimeoutMs = 5000;
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

    // Interface recovery process (up wlan0, restart wpa_supplicant)
    QProcess *m_recoveryProcess = nullptr;
    int m_recoveryAttempts = 0;
    bool m_recoveryInProgress = false;

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
    // Adaptive auto-scan (RF-03): keep the previous connected-network signal to
    // detect stability/movement and adjust the scan interval.
    int m_lastConnectedSignal = 0; // dBm from previous scan for connected network
    int m_scanStableCount = 0;     // consecutive scans with stable signal (< 6 dBm change)
    int m_autoScanInterval = 60000; // current interval (ms), 30s–180s

    // Fallback: remember last good connected SSID so if a new connect attempt
    // fails or times out, we can auto-roll back to the previous network.
    QString m_previousSSID;       // last known good connected SSID before a new connect
    bool m_autoRollbackPending = false;

    // Connection state machine
    int m_connectStep = 0; // 0=idle, 1=add_network, 2=set_ssid, 3=set_psk_or_keymgmt, 4=save_config, 5=select_network, 6=fallback_reconfigure, 7=fallback_reassociate
    QString m_pendingNetworkId;
    QString m_pendingPassword;
    int m_flowToken = 0;   // Unique token per connect flow, prevents stale callbacks
    bool m_passwordIsHex = false; // True when password was saved as hex PSK (from a "hex PSK" block), false when saved as plaintext (passphrase)
    bool m_pendingPasswordWasHex = false; // Source-of-truth flag: what the USER entered (hex PSK vs passphrase)

    // Handshake failure tracking (for wrong password detection).
    // m_seenFourWay: once wpa_cli status shows 4WAY_HANDSHAKE, the next
    // failure-state (DISCONNECTED/INACTIVE/SCANNING/ASSOCIATING) counts as one
    // AP handshake failure. Robust against the 2s poll missing the brief
    // 4WAY_HANDSHAKE window.
    int m_handshakeFailCount = 0;
    bool m_seenFourWay = false;
    int m_fourWayConsecutive = 0; // consecutive 4WAY_HANDSHAKE polls during connect (fast wrong-password detection)
    bool m_sawAssociating = false; // ever saw ASSOCIATING/4WAY during this connect (AP reachable → wrong password on timeout)
    QString m_lastWpaState;   // last observed wpa_state during connecting (for specific error messages)
    bool m_autoConnectDone = false; // startup auto-connect to strongest saved network ran (RF-04)

    // Auto-rollback detection (RF-42): if during a connect the status shows
    // COMPLETED on a DIFFERENT network than the target, wpa_supplicant already
    // fell back to a saved network (new AP failed to associate/auth). We track
    // consecutive polls to avoid transient false-positives during a switch.
    QString m_otherNetworkSsid;
    int m_otherNetworkCount = 0;

    // Debug: headless input simulation poll timer
    QTimer *m_simTimer = nullptr;
};

#endif // WIFIMANAGER_H
