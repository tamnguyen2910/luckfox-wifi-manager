#include "wifimanager.h"
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDebug>
#include <algorithm>
#include <QRegularExpression>
#include <QCryptographicHash>
#include <QByteArray>
#include <QMessageAuthenticationCode>
#include <QTime>
#include <QThread>
#include <unistd.h> // for ::rename, ::chown (POSIX)
#include <sys/stat.h> // for ::chmod, ::stat (P1 security: config 0600)
#include <cerrno> // for errno in chmod failure logs
#include <cstring> // for strerror

namespace {
// Timestamped log prefix for the debug/action log lines, e.g.
//   [14:03:22.415][ACT] Connect requested ...
// Correlates with the [UI] lines written by main.qml.
QString logTime()
{
    return QTime::currentTime().toString("HH:mm:ss.zzz");
}
} // namespace

WifiManager::WifiManager(QObject *parent)
    : QObject(parent)
    , m_isScanning(false)
    , m_isConnected(false)
    , m_connectedSSID()
    , m_connectedIP()
    , m_connectionStatus("Disconnected")
    , m_scanResults()
    , m_wifiScanProcess(nullptr)
    , m_currentSSID()
    , m_currentScanOutput()
    , m_connectAttemptCounter(0)
    , m_statusPollTimer()
    , m_pollingInterval(2000)
    , m_maxConnectWaitSeconds(15)
    , m_scanTimeoutTimer()
    , m_connectTimeoutTimer()
    , m_isConnecting(false)
    , m_forgetMode(false)
    , m_ssidToForget()
    , m_ipCheckProcess(nullptr)
    , m_ssidCheckProcess(nullptr)
    , m_pendingIpAddress()
    , m_pendingSsid()
    , m_checkStep(0)
    , m_dhcpProcess(nullptr)
    , m_wpaCLIBuffer()
    , m_autoScanTimer(nullptr)
    , m_handshakeFailCount(0)
    , m_seenFourWay(false)
    , m_fourWayConsecutive(0)
    , m_sawAssociating(false)
{
    // Initialize scan process (async, non-blocking)
    m_wifiScanProcess = new QProcess(this);
    m_wifiScanProcess->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(m_wifiScanProcess, &QProcess::readyReadStandardOutput,
                     this, &WifiManager::onWifiScanReadyRead);
    QObject::connect(m_wifiScanProcess, &QProcess::readyReadStandardError,
                     this, [this]() {
        qWarning() << "[WifiManager] iw stderr:" << QString::fromUtf8(m_wifiScanProcess->readAllStandardError()).trimmed();
    });
    QObject::connect(m_wifiScanProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, &WifiManager::onWifiScanFinished);

    // Initialize WPA CLI process (async, non-blocking)
    m_wpaCLIProcess = new QProcess(this);
    m_wpaCLIProcess->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(m_wpaCLIProcess, &QProcess::readyReadStandardOutput,
                     this, &WifiManager::onWpaCLIReadyRead);
    QObject::connect(m_wpaCLIProcess, &QProcess::readyReadStandardError,
                     this, [this]() {
        qWarning() << "[WifiManager] wpa_cli stderr:" << QString::fromUtf8(m_wpaCLIProcess->readAllStandardError()).trimmed();
    });
    QObject::connect(m_wpaCLIProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, &WifiManager::onWpaCLIFinished);

    // Initialize IP check process (async)
    m_ipCheckProcess = new QProcess(this);
    m_ipCheckProcess->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(m_ipCheckProcess, &QProcess::readyReadStandardError,
                     this, [this]() {
        qWarning() << "[WifiManager] ip stderr:" << QString::fromUtf8(m_ipCheckProcess->readAllStandardError()).trimmed();
    });
    QObject::connect(m_ipCheckProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                         // P1: process finished (any path) — disarm the hang-guard
                         m_checkTimeoutTimer.stop();
                         // Only process result if process exited normally (not killed/crashed)
                         if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                             qDebug() << "[WifiManager] ip check process failed:" << exitCode << exitStatus;
                             // If we were in the middle of checking, abort this check cycle
                             if (m_checkStep == 1) {
                                 m_checkStep = 0;
                                 m_pendingIpAddress.clear();
                             }
                             return;
                         }
                         if (m_checkStep == 1) {
                             // Parse IPv4 address from output using regex — robust against
                             // format variations (with/without /prefix, extra whitespace,
                             // inet6 lines). Old code split on spaces and assumed the
                             // prefix was always present, breaking on different `ip`
                             // output layouts.
                             QString output = QString::fromUtf8(m_ipCheckProcess->readAllStandardOutput());
                             static const QRegularExpression ipRe(
                                 "\\binet\\s+(\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3})(?:/\\d{1,2})?");
                             QRegularExpressionMatch match = ipRe.match(output);
                             if (match.hasMatch()) {
                                 m_pendingIpAddress = match.captured(1);
                                 qDebug() << "[WifiManager] Parsed IP:" << m_pendingIpAddress;
                             } else {
                                 qDebug() << "[WifiManager] No IPv4 address found on wlan0";
                             }
                             m_checkStep = 2; // Move to waiting for SSID check
                             startSsidCheck();
                         }
                     });

    // Initialize SSID check process (async)
    m_ssidCheckProcess = new QProcess(this);
    m_ssidCheckProcess->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(m_ssidCheckProcess, &QProcess::readyReadStandardError,
                     this, [this]() {
        qWarning() << "[WifiManager] wpa_cli status stderr:" << QString::fromUtf8(m_ssidCheckProcess->readAllStandardError()).trimmed();
    });
    QObject::connect(m_ssidCheckProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                         // P1: process finished (any path) — disarm the hang-guard
                         m_checkTimeoutTimer.stop();
                         // Only process result if process exited normally (not killed/crashed)
                         if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                             qDebug() << "[WifiManager] ssid check process failed:" << exitCode << exitStatus;
                             // If we were in the middle of checking, abort this check cycle
                             if (m_checkStep == 2) {
                                 m_checkStep = 0;
                                 m_pendingSsid.clear();
                             }
                             return;
                         }
                         // Parse SSID, wpa_state and ip_address from wpa_cli status output.
                         QString output = QString::fromUtf8(m_ssidCheckProcess->readAllStandardOutput());
                         const QStringList lines = output.split('\n');
                         bool completed = false;
                         QString currentWpaState;
                         QString currentSsid;
                         QString currentIp;
                         for (const QString &line : lines) {
                             const QString t = line.trimmed();
                             if (t.startsWith("ssid="))
                                 currentSsid = t.mid(5);
                             else if (t.startsWith("wpa_state=")) {
                                 currentWpaState = t.mid(10);
                                 if (currentWpaState == "COMPLETED")
                                     completed = true;
                             } else if (t.startsWith("ip_address=")) {
                                 currentIp = t.mid(11);
                             }
                         }

                         // Handshake failure tracking for wrong password detection.
                         // wpa_supplicant returns to DISCONNECTED/INACTIVE/SCANNING
                         // right after 4WAY_HANDSHAKE when credentials are wrong.
                         //
                         // Counting is driven by m_seenFourWay (not m_lastWpaState):
                         // the 2s poll can miss the brief 4WAY_HANDSHAKE window on a
                         // fast MIC failure, so once we've SEEN a 4WAY_HANDSHAKE, the
                         // next failure-state counts as one failed AP. With a
                         // multi-BSSID network, supplicant retries each AP of the same
                         // SSID — each failed AP increments the count, and reaching
                         // >=3 within one connect attempt is treated as wrong password.
                         if (m_isConnecting && !currentWpaState.isEmpty()) {
                             m_lastWpaState = currentWpaState; // for specific error messages
                             // Track if we ever associated with the AP (or reached 4WAY).
                             // If we did but never completed, it's wrong password
                             // (the AP is present but rejected the handshake).
                             if (currentWpaState == "ASSOCIATING" ||
                                 currentWpaState == "4WAY_HANDSHAKE")
                                 m_sawAssociating = true;
                             if (currentWpaState == "4WAY_HANDSHAKE") {
                                 m_seenFourWay = true;
                                 // Fast wrong-password detection: stuck in 4WAY for
                                 // >=3 consecutive polls (~6s) means the AP is rejecting
                                 // the handshake — no need to wait for the DISCONNECTED
                                 // transition (which the 2s poll can miss entirely).
                                 ++m_fourWayConsecutive;
                                 if (m_fourWayConsecutive >= 3) {
                                     qDebug() << "[WifiManager] Stuck in 4WAY_HANDSHAKE for"
                                              << m_fourWayConsecutive << "polls — treating as wrong password";
                                     m_handshakeFailCount = 3; // triggers fast-fail on next poll
                                 }
                             } else {
                                 m_fourWayConsecutive = 0;
                             }
                             if (m_seenFourWay &&
                                 (currentWpaState == "DISCONNECTED" ||
                                  currentWpaState == "INACTIVE" ||
                                  currentWpaState == "SCANNING" ||
                                  currentWpaState == "ASSOCIATING")) {
                                 m_handshakeFailCount++;
                                 m_seenFourWay = false; // re-arm on the next 4WAY_HANDSHAKE
                                 qDebug() << "[WifiManager] Handshake failed (AP retry), count:"
                                          << m_handshakeFailCount;
                             }
                             if (currentWpaState == "COMPLETED") {
                                 m_handshakeFailCount = 0;
                                 m_seenFourWay = false;
                                 m_fourWayConsecutive = 0;
                             }
                         }

                         if (m_checkStep == 2) {
                             // Async (non-connecting) check flow — IP came from `ip addr show`.
                             m_pendingSsid = currentSsid;
                             // Only declare connected when wpa_supplicant fully authenticated
                             if (!completed)
                                 m_pendingSsid.clear();
                             finalizeConnectionCheck(m_pendingIpAddress, m_pendingSsid);
                             m_checkStep = 0;
                         } else if (m_isConnecting) {
                             // CONNECTING flow — detected wpa_state=COMPLETED on wpa_cli status.
                             //
                             // IMPORTANT: during a connect flow we are now using the
                             // `m_isConnecting` branch instead of `m_checkStep == 2`, because
                             // `m_checkStep` was never set to 2 during the connect flow.
                             // This fix was the root cause of the "stuck at Connecting..." bug.
                             qDebug() << "[WifiManager] ssid check during connect: state="
                                      << currentWpaState << "ssid=" << currentSsid << "ip=" << currentIp;
                             if (completed && !currentSsid.isEmpty()) {
                                 // COMPLETED on the TARGET network — normal success path.
                                 if (currentSsid == m_currentSSID) {
                                     m_otherNetworkCount = 0;
                                     m_otherNetworkSsid.clear();
                                     const QString ipForConnect = !currentIp.isEmpty()
                                                                      ? currentIp
                                                                      : m_pendingIpAddress;
                                     finalizeConnectionCheck(ipForConnect, currentSsid);
                                 } else {
                                     // COMPLETED on a DIFFERENT network than the target:
                                     // wpa_supplicant has auto-rolled back to a saved network
                                     // (the new AP failed associate/auth and supplicant fell
                                     // back to the last good one). Treat as a failed connect
                                     // that ended on the previous network.
                                     if (m_otherNetworkSsid == currentSsid) {
                                         m_otherNetworkCount++;
                                     } else {
                                         m_otherNetworkSsid = currentSsid;
                                         m_otherNetworkCount = 1;
                                     }
                                     // Require 2 consecutive polls (~4s) to avoid a false
                                     // positive from a transient stale COMPLETED during a
                                     // legitimate network switch.
                                     if (m_otherNetworkCount >= 2) {
                                         qDebug() << "[WifiManager] Auto-rollback detected: supplicant on"
                                                  << currentSsid << "after failing to connect to" << m_currentSSID;
                                         m_lastError = "Could not connect to " + m_currentSSID
                                                       + " -- rolled back to " + currentSsid;
                                         emit lastErrorChanged();
                                         m_otherNetworkSsid.clear();
                                         m_otherNetworkCount = 0;
                                         // Finalize as connected to the rolled-back network
                                         const QString ipForConnect = !currentIp.isEmpty()
                                                                          ? currentIp
                                                                          : m_pendingIpAddress;
                                         finalizeConnectionCheck(ipForConnect, currentSsid);
                                     }
                                 }
                             } else if (currentSsid != m_currentSSID) {
                                 // Not completed, and not the target — reset the counter
                                 m_otherNetworkCount = 0;
                                 m_otherNetworkSsid.clear();
                             }
                         }
                     });

    // DHCP renewal process (async) — triggers fresh lease when switching networks
    m_dhcpProcess = new QProcess(this);
    m_dhcpProcess->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(m_dhcpProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
                         if (exitStatus != QProcess::NormalExit) {
                             qDebug() << "[WifiManager] DHCP process killed, aborting renewal";
                             m_renewalPending = false;
                             return;
                         }
                         qDebug() << "[WifiManager] DHCP renewal finished, exit code:" << exitCode;
                         if (exitCode == 0) {
                             // Fresh lease acquired — safe to check the connection now.
                             qDebug() << "[WifiManager] DHCP renewal OK, checking connection";
                             m_renewalPending = false;
                             m_renewalRetries = 0;
                             startStatusPolling();
                         } else {
                             // No lease yet — retry up to 5 times
                             if (m_renewalRetries < 5) {
                                 qDebug() << "[WifiManager] DHCP renewal failed (exit" << exitCode
                                          << "), retrying...";
                                 m_renewalRetries++;
                                 startDhcpRenewal();
                             } else {
                                 qDebug() << "[WifiManager] DHCP renewal gave up after 5 retries";
                                 m_renewalPending = false;
                                 m_renewalRetries = 0;
                                 startStatusPolling(); // try anyway, maybe static IP
                             }
                         }
                     });

    // Status polling timer
    m_statusPollTimer.setInterval(m_pollingInterval);
    QObject::connect(&m_statusPollTimer, &QTimer::timeout,
                     this, &WifiManager::onStatusPollingTimeout);

    // Scan timeout timer
    m_scanTimeoutTimer.setSingleShot(true);
    QObject::connect(&m_scanTimeoutTimer, &QTimer::timeout,
                     this, &WifiManager::onScanTimeout);

    // Connect timeout timer
    m_connectTimeoutTimer.setSingleShot(true);
    QObject::connect(&m_connectTimeoutTimer, &QTimer::timeout,
                     this, &WifiManager::onConnectTimeout);

    // Global connect timeout (90s) — prevents stuck "Connecting..." forever
    // if wpa_supplicant/ip/wpa_cli hangs. Covers entire state machine.
    m_connectTotalTimer.setSingleShot(true);
    m_connectTotalTimer.setInterval(90000);
    QObject::connect(&m_connectTotalTimer, &QTimer::timeout,
                     this, &WifiManager::onConnectTotalTimeout);

    // Check initial connection state (trigger async check)
    QTimer::singleShot(500, this, [this]() {
        if (interfaceExists()) {
            startAsyncConnectionCheck();
        }
    });

    // Initialize check timeout timer (P1: 5s guard for ip addr show / wpa_cli status hangs)
    m_checkTimeoutTimer.setInterval(kCheckTimeoutMs);
    m_checkTimeoutTimer.setSingleShot(true);
    QObject::connect(&m_checkTimeoutTimer, &QTimer::timeout, this, &WifiManager::onCheckTimeout);

    // Load saved networks from config for remember-password feature
    loadSavedNetworks();

    // Auto-reconnect to strongest saved network on startup (RF-04).
    // Wait for first scan completion, then connect to the saved network
    // with the best signal strength. Skip if already connected.
    QTimer::singleShot(1500, this, [this]() {
        if (m_isConnected) {
            qDebug() << "[WifiManager] Startup: already connected, skipping auto-reconnect";
            return;
        }
        if (interfaceExists() && interfaceIsUp() && !m_isScanning) {
            scan();
        }
    });

    // Watchdog: poll wlan0 existence every 2s.
    // With debounce of 2 consecutive down checks, a down lasting >= 5s
    // is detected (~4s), showing "Interface lost" and enabling auto-reconnect.
    m_interfaceWatchdog = new QTimer(this);
    m_interfaceWatchdog->setInterval(2000);
    QObject::connect(m_interfaceWatchdog, &QTimer::timeout,
                     this, &WifiManager::onInterfaceWatchdogTimeout);
    m_interfaceWatchdog->start();

    // Periodic auto-scan so the WiFi list always refreshes (even when
    // connected, and after auto-reconnect). Guards inside the timer
    // callback skip while a connect/DHCP is in progress.
    startAutoScan();

    // Debug: headless input simulation — polls /tmp/wifi_sim_cmd every 500ms
    m_simTimer = new QTimer(this);
    m_simTimer->setInterval(500);
    connect(m_simTimer, &QTimer::timeout, this, &WifiManager::pollSimCommands);
    m_simTimer->start();
    qDebug() << logTime() << "[WifiManager] Sim command polling started (500ms interval)";
}

WifiManager::~WifiManager()
{
    m_statusPollTimer.stop();
    m_scanTimeoutTimer.stop();
    m_connectTimeoutTimer.stop();
    m_connectTotalTimer.stop();
    if (m_interfaceWatchdog)
        m_interfaceWatchdog->stop();
    if (m_wifiScanProcess->state() != QProcess::NotRunning)
        m_wifiScanProcess->kill();
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill();
    if (m_ipCheckProcess->state() != QProcess::NotRunning)
        m_ipCheckProcess->kill();
    if (m_ssidCheckProcess->state() != QProcess::NotRunning)
        m_ssidCheckProcess->kill();
    if (m_dhcpProcess && m_dhcpProcess->state() != QProcess::NotRunning)
        m_dhcpProcess->kill();
    if (m_simTimer)
        m_simTimer->stop();
    if (m_recoveryProcess && m_recoveryProcess->state() != QProcess::NotRunning)
        m_recoveryProcess->kill();
}

// ==================== SIM COMMANDS (headless input) ====================

void WifiManager::pollSimCommands()
{
    QFile cmdFile("/tmp/wifi_sim_cmd");
    if (!cmdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return; // no command file
    }
    QString cmd = QString::fromUtf8(cmdFile.readAll()).trimmed();
    cmdFile.close();

    if (!cmd.isEmpty()) {
        // Remove command file after reading (single-shot)
        QFile::remove("/tmp/wifi_sim_cmd");
        qDebug() << "[SIM] Received command:" << cmd;
        execSimCommand(cmd);
    }
}

void WifiManager::execSimCommand(const QString &cmd)
{
    const QStringList parts = cmd.split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) return;

    const QString action = parts[0].toLower();

    if (action == "scan") {
        qDebug() << "[ACT] Sim: scan";
        scan();
    } else if (action == "connect" && parts.size() >= 3) {
        QString ssid = parts[1];
        QString password = parts.mid(2).join(' ');
        qDebug() << "[ACT] Sim: connect to" << ssid << "(pwd len:" << password.length() << ")";
        connectToNetwork(ssid, password);
    } else if (action == "connectsaved" && parts.size() >= 2) {
        QString ssid = parts[1];
        qDebug() << "[ACT] Sim: connect saved" << ssid;
        connectSaved(ssid);
    } else if (action == "forget" && parts.size() >= 2) {
        QString ssid = parts[1];
        qDebug() << "[ACT] Sim: forget" << ssid;
        forgetNetwork(ssid);
    } else if (action == "disconnect") {
        qDebug() << "[ACT] Sim: disconnect";
        disconnectFromNetwork();
    } else if (action == "status") {
        logStatus();
    } else if (action == "log") {
        logStatus();
    } else {
        qWarning() << "[SIM] Unknown command:" << cmd;
        qDebug() << "[SIM] Available: scan | connect <ssid> <password> | connectsaved <ssid> | forget <ssid> | disconnect | status";
    }
}

void WifiManager::logStatus()
{
    qDebug() << "[STA] Status dump:";
    qDebug() << "  m_isScanning:" << m_isScanning;
    qDebug() << "  m_isConnected:" << m_isConnected;
    qDebug() << "  m_isConnecting:" << m_isConnecting;
    qDebug() << "  m_connectedSSID:" << m_connectedSSID;
    qDebug() << "  m_connectedIP:" << m_connectedIP;
    qDebug() << "  m_connectionStatus:" << m_connectionStatus;
    qDebug() << "  m_connectStep:" << m_connectStep;
    qDebug() << "  m_handshakeFailCount:" << m_handshakeFailCount;
    qDebug() << "  m_seenFourWay:" << m_seenFourWay;
    qDebug() << "  m_renewalPending:" << m_renewalPending;
    qDebug() << "  m_scanResults.count:" << m_scanResults.size();
    qDebug() << "  m_savedSSIDs:" << m_savedSSIDs.values();
}

// ==================== SCAN ====================

void WifiManager::scan()
{
    if (m_isScanning)
        return;

    if (!interfaceExists()) {
        m_connectionStatus = "Failed: wlan0 not found";
        emit connectionStateChanged();
        return;
    }

    if (!interfaceIsUp()) {
        m_connectionStatus = "Failed: wlan0 is down";
        emit connectionStateChanged();
        qDebug() << "[WifiManager] Scan aborted: wlan0 is down";
        return;
    }

    // Ensure wpa_supplicant is alive before scanning — if the control
    // socket is missing, wpa_cli won't work. Start it if needed.
    ensureWpaSupplicant();

    m_isScanning = true;
    emit isScanningChanged();

    m_currentScanOutput.clear();
    // NOTE: don't clear m_scanResults here — if this scan fails, the UI keeps
    // the previous results instead of flashing an empty list. parseWifiScanOutput
    // replaces the list wholesale on success anyway.

    // Start scan with dedicated timeout
    m_scanTimeoutTimer.start(5000); // 5s max for scan
    m_wifiScanProcess->start("iw", QStringList() << "dev" << "wlan0" << "scan");
}

void WifiManager::onWifiScanReadyRead()
{
    m_currentScanOutput += QString::fromUtf8(m_wifiScanProcess->readAllStandardOutput());
}

void WifiManager::onWifiScanFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // If we are not in scanning state, this finished signal is from a process
    // that was killed intentionally (e.g., by connectToNetwork). Ignore it.
    if (!m_isScanning) {
        m_scanTimeoutTimer.stop();
        return;
    }

    m_scanTimeoutTimer.stop();

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        // Scan process failed or was killed unexpectedly.
        m_isScanning = false;
        m_connectionStatus = "Scan failed";
        emit isScanningChanged();
        emit connectionStateChanged();
        qWarning() << "[WifiManager] iw scan failed:" << exitCode << exitStatus;
        return;
    }

    // Successful scan: parse the output and update results.
    parseWifiScanOutput(m_currentScanOutput);

    // Adaptive auto-scan (RF-03): track connected network signal to adjust interval
    if (m_isConnected && !m_connectedSSID.isEmpty()) {
        const int oldInterval = m_autoScanInterval;
        // Find the connected network in the new scan results
        for (const QVariant &item : m_scanResults) {
            QVariantMap map = item.toMap();
            if (map.value("ssid").toString() == m_connectedSSID) {
                int currentSignal = map.value("signal").toInt(); // dBm
                if (m_lastConnectedSignal != 0) {
                    int delta = qAbs(currentSignal - m_lastConnectedSignal);
                    if (delta <= 5) {
                        // Stable signal — increase interval (cap at 180s)
                        m_scanStableCount++;
                        if (m_scanStableCount >= 2) {
                            m_autoScanInterval = qMin(m_autoScanInterval + 30000, 180000);
                        }
                    } else if (delta >= 15) {
                        // Movement detected — drop to 30s
                        m_autoScanInterval = 30000;
                        m_scanStableCount = 0;
                    } else {
                        // Moderate change — decrease interval slightly (floor 60s)
                        m_autoScanInterval = qMax(m_autoScanInterval - 10000, 60000);
                        m_scanStableCount = 0;
                    }
                }
                m_lastConnectedSignal = currentSignal;
                qDebug() << "[WifiManager] Adaptive scan: connected signal" << currentSignal
                         << "dBm, interval" << m_autoScanInterval << "ms";
                break;
            }
        }
        // Apply the new interval immediately: restart the running timer so the
        // countdown uses the adjusted value instead of the previous one.
        if (m_autoScanInterval != oldInterval && m_autoScanTimer && m_autoScanTimer->isActive()) {
            m_autoScanTimer->start(m_autoScanInterval);
        }
    }

    m_isScanning = false;
    emit isScanningChanged();
    emit scanResultsChanged();

    // RF-04: startup auto-connect to strongest saved network.
    // Runs once, after the first scan completes (triggered by the startup
    // singleShot in the constructor). scanResults are sorted by signal,
    // so pick the first saved network we encounter.
    if (!m_autoConnectDone && !m_isConnected && !m_isConnecting && !m_scanResults.isEmpty()) {
        m_autoConnectDone = true;
        QString strongestSaved;
        int bestSignal = -100;
        for (const QVariant &item : m_scanResults) {
            QVariantMap map = item.toMap();
            const QString s = map.value("ssid").toString();
            const int sig = map.value("signal").toInt();
            if (m_savedSSIDs.contains(s) && sig > bestSignal) {
                bestSignal = sig;
                strongestSaved = s;
            }
        }
        if (!strongestSaved.isEmpty()) {
            qDebug() << "[WifiManager] Startup auto-connect: strongest saved network"
                     << strongestSaved << "@" << bestSignal << "dBm";
            connectSaved(strongestSaved);
        } else {
            qDebug() << "[WifiManager] Startup auto-connect: no saved network in range";
        }
    }
}

void WifiManager::parseWifiScanOutput(const QString &output)
{
    // Parse iw scan output
    QList<QVariantMap> networks;
    QVariantMap current;

    const QStringList lines = output.split('\n');
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();

        // BSS line: new network entry
        if (trimmed.startsWith("BSS ")) {
            if (!current.isEmpty()) {
                // Finalize secured for previous network before appending
                bool hasPrivacy = current.value("hasPrivacy", false).toBool();
                bool hasKeyMgmt = current.value("hasKeyMgmt", false).toBool();
                current["secured"] = hasPrivacy || hasKeyMgmt;
                current.remove("hasPrivacy");
                current.remove("hasKeyMgmt");
                networks.append(current);
            }
            current.clear();
        }
        // SSID
        else if (trimmed.startsWith("SSID: ")) {
            QString ssid = trimmed.mid(6);
            current["ssid"] = ssid;
        }
        // Signal strength
        else if (trimmed.startsWith("signal: ")) {
            QString signalStr = trimmed.mid(8); // bỏ "signal: "
            QRegularExpression re("-?\\d+(\\.\\d+)?"); // số âm nguyên hoặc thập phân
            QRegularExpressionMatch match = re.match(signalStr);
            if (match.hasMatch()) {
                bool ok;
                double signalDbm = match.captured(0).toDouble(&ok);
                if (ok) {
                    current["signal"] = static_cast<int>(signalDbm);
                    int level = 0;
                    if (signalDbm >= -50) level = 4;
                    else if (signalDbm >= -60) level = 3;
                    else if (signalDbm >= -70) level = 2;
                    else if (signalDbm >= -80) level = 1;
                    current["signalLevel"] = level;
                } else {
                    qWarning() << "[WifiManager] Failed to convert signal to double:" << signalStr;
                    current["signal"] = -100;
                    current["signalLevel"] = 0;
                }
            } else {
                qWarning() << "[WifiManager] No number found in signal line:" << signalStr;
                current["signal"] = -100;
                current["signalLevel"] = 0;
            }
        }
        // Capability line (board uses "capability: ESS Privacy ...")
        else if (trimmed.startsWith("capability: ")) {
            bool hasPrivacy = trimmed.contains("Privacy", Qt::CaseInsensitive);
            current["hasPrivacy"] = hasPrivacy;
        }
        // RSN/WPA sections indicate WPA/WPA2 security
        else if (trimmed.startsWith("RSN:") || trimmed.startsWith("WPA:")) {
            current["hasKeyMgmt"] = true;
        }
    }
    // Don't forget the last entry
    if (!current.isEmpty()) {
        // Finalize secured for last network
        bool hasPrivacy = current.value("hasPrivacy", false).toBool();
        bool hasKeyMgmt = current.value("hasKeyMgmt", false).toBool();
        current["secured"] = hasPrivacy || hasKeyMgmt;
        // Remove temporary keys
        current.remove("hasPrivacy");
        current.remove("hasKeyMgmt");
        networks.append(current);
    }

    // Build QVariantList for QML
    QVariantList results;
    // Sort by signal strength (strongest first)
    std::sort(networks.begin(), networks.end(),
        [](const QVariantMap &a, const QVariantMap &b) {
            return a["signal"].toInt() > b["signal"].toInt();
        });

    for (const QVariantMap &net : networks) {
        const QString itemSsid = net.value("ssid", "").toString();
        // Skip hidden/empty SSID entries (hidden networks)
        if (itemSsid.isEmpty()) {
            continue;
        }
        QVariantMap item;
        item["ssid"] = itemSsid;
        item["signal"] = net.value("signal", -100).toInt();
        item["signalLevel"] = net.value("signalLevel", 0).toInt();
        item["secured"] = net.value("secured", false).toBool();
        item["saved"]     = m_savedSSIDs.contains(itemSsid);
        item["connected"] = (itemSsid == m_connectedSSID && m_isConnected);
        results.append(item);
    }

    m_scanResults = results;
}

// ==================== CONNECT ====================

void WifiManager::connectToNetwork(const QString &ssid, const QString &password, bool passwordIsHex)
{
    // Abort any in-flight connect/forget state machine first.
    // Kill both wpa_cli and scan process to avoid race conditions.
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill();
    if (m_wifiScanProcess->state() != QProcess::NotRunning)
        m_wifiScanProcess->kill();

    // P0 fix: bump flow token — any callback from a previous connect flow
    // will see the mismatch and be discarded (stale-callback guard).
    ++m_flowToken;

    // P0 fix (SHIFT bug): use the explicit source-of-truth parameter.
    // - User-typed password (QML calls connectToNetwork without 3rd arg):
    //   default false → ALWAYS treated as passphrase, even if it looks like
    //   64 hex chars (SHIFT bug fix).
    // - connectSaved() passes true when the stored psk is hex → sent as hex PSK.
    m_passwordIsHex = passwordIsHex;

    // RF-42 auto-rollback: remember the currently connected network BEFORE
    // resetting state, so if this connect attempt fails we can reconnect to it.
    // Only meaningful when we were connected to a different, saved network.
    if (m_isConnected && !m_connectedSSID.isEmpty()) {
        m_previousSSID = m_connectedSSID;
        qDebug() << "[WifiManager] Rollback target saved:" << m_previousSSID;
    }

    m_wpaCLIBuffer.clear();       // discard stale output from killed process
    m_connectStep = 0;            // reset state machine
    m_isConnecting = false;
    m_isConnected = false;        // reset stale connected flag
    m_connectTimeoutTimer.stop(); // cancel any pending connect timeout
    m_connectTotalTimer.stop();   // cancel any pending global connect timeout
    stopStatusPolling();

    // Pause auto-scan while connecting — scanning would disturb wpa_supplicant
    // and could cause connection failures.
    if (m_autoScanTimer && m_autoScanTimer->isActive()) {
        m_autoScanTimer->stop();
        qDebug() << "[WifiManager] Auto-scan paused for connect";
    }

    if (!interfaceExists()) {
        m_connectionStatus = "Failed: wlan0 not found";
        emit connectionStateChanged();
        qDebug() << "[WifiManager] ERROR: wlan0 interface does not exist";
        return;
    }

    // Validate SSID: max 32 bytes (802.11), no control chars, no config injection
    if (!isValidSSID(ssid)) {
        m_connectionStatus = "Invalid SSID";
        emit connectionStateChanged();
        qDebug() << "[WifiManager] ERROR: Invalid SSID:" << ssid;
        return;
    }

    // Validate password: trim and reject if empty
    QString cleanPassword = password.trimmed();
    if (cleanPassword.isEmpty() && !password.isEmpty()) {
        qDebug() << "[WifiManager] Password contains only whitespace, treating as empty";
        cleanPassword.clear();
    }

    m_currentSSID = ssid;
    m_isConnecting = true;
    m_forgetMode = false;
    m_connectionStatus = "Connecting...";
    // Reset handshake failure tracking for this connect attempt
    m_handshakeFailCount = 0;
    m_seenFourWay = false;
    m_fourWayConsecutive = 0;
    m_lastWpaState.clear();

    // Ensure wpa_supplicant is alive before connecting — this was the root
    // cause of "Failed to connect to non-global ctrl_ifname" errors after
    // wpa_supplicant was killed by config-file deletion.
    ensureWpaSupplicant();

    m_connectTotalTimer.start(); // global 90s timeout for whole connect flow
    emit connectionStateChanged();

    qDebug() << "[WifiManager] Connecting to:" << ssid;

    // NOTE: Do NOT write wpa_supplicant.conf yet. That is deferred to
    // finalizeConnectionCheck (success path) so a network with the wrong
    // password never ends up persisted to disk. The running wpa_supplicant
    // config (add_network / set_network below) is temporary and removed on
    // failure by the postConnectCleanup in abortConnect().
    m_pendingPassword = cleanPassword;

    // Start async connection state machine
    m_connectStep = 1; // add_network
    startAddNetwork();
}

void WifiManager::disconnectFromNetwork()
{
    // Use wpa_cli to disconnect gracefully — preserves saved networks.
    // Async (startDetached) so the UI never freezes.
    QProcess::startDetached("wpa_cli", QStringList() << "-i" << "wlan0" << "disconnect");

    // Release DHCP lease to avoid IP conflicts after reconnecting
    QProcess::startDetached("udhcpc", QStringList() << "-i" << "wlan0" << "-R");

    m_isConnected = false;
    m_connectedSSID.clear();
    m_connectedIP.clear();
    m_connectionStatus = "Disconnected";
    emit isConnectedChanged();
    emit connectedInfoChanged();
    emit connectionStateChanged();

    stopStatusPolling();
    m_connectTimeoutTimer.stop();
    m_connectTotalTimer.stop();
    qDebug() << "[WifiManager] Disconnected from current network";

    // Refresh scan results sau disconnect (fix B2)
    updateSavedFlags();
    emit scanResultsChanged();
}

bool WifiManager::writeWpaSupplicantConfig(const QString &ssid, const QString &password)
{
    // Serialize config file access with concurrent forget operations
    QMutexLocker locker(&m_configMutex);

    // Use the shared helper to read existing config, skipping the old block for this SSID
    QStringList headerLines;
    QList<ConfigBlock> networkBlocks;
    readWpaSupplicantConfig("/etc/wpa_supplicant.conf", headerLines, networkBlocks, ssid);

    // Build new network block with hex PSK for secure storage
    ConfigBlock newBlock;
    QTextStream ns(&newBlock.content);
    ns << "network={\n";
    ns << "    ssid=\"" << ssid << "\"\n";
    if (!password.isEmpty()) {
        // P0 fix (SHIFT bug): use the source-of-truth flag, NOT content
        // guessing. connectToNetwork() (user-typed) sets false; connectSaved()
        // (read back from config) sets true only when the stored psk was hex.
        bool alreadyHex = m_passwordIsHex;
        if (!alreadyHex) {
            // Normal password → convert to hex PSK via PBKDF2-SHA1
            QString hexPsk = generateHexPsk(ssid, password);
            if (!hexPsk.isEmpty()) {
                // generateHexPsk returns "psk=hexstring", write it FULLY (with psk= prefix)
                ns << "    " << hexPsk << "\n";
                qDebug() << "[WifiManager] Saved hex PSK for" << ssid;
            } else {
                // Fallback to plaintext if PBKDF2 fails
                qWarning() << "[WifiManager] hex PSK generation failed, storing plaintext";
                ns << "    psk=\"" << password << "\"\n";
            }
        } else {
            // Already hex PSK — write it directly (with psk= prefix)
            ns << "    psk=" << password << "\n";
            qDebug() << "[WifiManager] Saved hex PSK (reconnect) for" << ssid;
        }
        ns << "    key_mgmt=WPA-PSK\n";
    } else {
        ns << "    key_mgmt=NONE\n";
    }
    ns << "    priority=999\n";
    ns << "}\n";
    newBlock.hasSSID = true;
    newBlock.ssid = ssid;

    // Prepend the new block so it gets priority
    networkBlocks.prepend(newBlock);

    // Ensure essential global settings exist
    bool hasCtrlInterface = false, hasApScan = false, hasUpdateConfig = false;
    for (const QString &h : headerLines) {
        QString t = h.trimmed();
        if (t.startsWith("ctrl_interface=")) hasCtrlInterface = true;
        else if (t.startsWith("ap_scan=")) hasApScan = true;
        else if (t.startsWith("update_config=")) hasUpdateConfig = true;
    }
    if (!hasCtrlInterface) headerLines.prepend("ctrl_interface=/var/run/wpa_supplicant");
    if (!hasApScan) headerLines.prepend("ap_scan=1");
    if (!hasUpdateConfig) headerLines.prepend("update_config=1");

    // Write back via shared helper
    return writeWpaSupplicantConfig("/etc/wpa_supplicant.conf", headerLines, networkBlocks);
}

void WifiManager::onWpaCLIReadyRead()
{
    m_wpaCLIBuffer += QString::fromUtf8(m_wpaCLIProcess->readAllStandardOutput());
    qDebug() << "[WifiManager] wpa_cli output:" << m_wpaCLIBuffer.trimmed();
}

void WifiManager::onWpaCLIFinished(int /*exitCode*/, QProcess::ExitStatus exitStatus)
{
    m_connectTimeoutTimer.stop();

    // If the process was killed (e.g. new connect started), don't run state
    // machine on stale output — the new connect owns the state machine now.
    if (exitStatus != QProcess::NormalExit) {
        m_wpaCLIBuffer.clear();
        return;
    }

    if (m_forgetMode) {
        // Only update UI to "Disconnected" if the forgotten network was the active one
        if (m_ssidToForget == m_connectedSSID) {
            m_connectionStatus = "Disconnected";
            emit connectionStateChanged();
        }
        m_wpaCLIBuffer.clear();
        m_forgetMode = false;
        m_ssidToForget.clear();
        return;
    }

    // Connection state machine
    if (m_connectStep == 1) {
        handleAddNetworkFinished();
        return;
    } else if (m_connectStep == 2) {
        handleSetSsidFinished();
        return;
    } else if (m_connectStep == 3) {
        handleSetPskOrKeyMgmtFinished();
        return;
    } else if (m_connectStep == 4) {
        handleSaveConfigFinished();
        return;
    } else if (m_connectStep == 5) {
        handleSelectNetworkFinished();
        return;
    } else if (m_connectStep == 6 || m_connectStep == 7) {
        // fallback_reconfigure or fallback_reassociate
        if (m_connectStep == 6) {
            handleFallbackReconfigureFinished();
        } else {
            handleFallbackReassociateFinished();
        }
        return;
    }

}

// ==================== STATUS POLLING ====================

void WifiManager::startStatusPolling()
{
    m_connectAttemptCounter = 0;
    m_statusPollTimer.start();
    onStatusPollingTimeout(); // Immediate first check
}

void WifiManager::stopStatusPolling()
{
    m_statusPollTimer.stop();
}

void WifiManager::beginConnectPolling()
{
    // Start status polling FIRST so wpa_state is checked immediately,
    // even before DHCP finishes. This is critical for fast-fail on wrong
    // password: if DHCP blocks until lease expires, the wrong-password
    // detection (4WAY_HANDSHAKE fast-fail) would be delayed by the DHCP
    // retry cycle (~5 × 8s = 40s), making the "wrong password" error
    // appear extremely late.
    m_renewalRetries = 0;
    qDebug() << "[WifiManager] beginConnectPolling: starting status polling + DHCP renewal";
    startStatusPolling();
    startDhcpRenewal();
}

void WifiManager::onStatusPollingTimeout()
{
    m_connectAttemptCounter++;

    // Fast fail: if handshake failed 3+ times, it's almost certainly wrong password.
    // This catches it in ~6-8s instead of waiting for 25s/90s timeouts.
    if (m_isConnecting && m_handshakeFailCount >= 3) {
        qDebug() << "[WifiManager] Fast-fail: wrong password detected (handshake failed" << m_handshakeFailCount << "times)";
        abortConnect(QString("Wrong password -- authentication failed with %1")
                         .arg(m_currentSSID));
        return;
    }

    // Fallback fast-fail: AIC driver may pass through 4WAY_HANDSHAKE so briefly
    // the 2s poll misses it entirely. Detect "no progress": if we've polled
    // >=5 times (~10s) and wpa_state never reached COMPLETED but the network
    // was successfully associated (i.e. last state is 4WAY_HANDSHAKE or the
    // driver shows connect attempts), it's almost certainly wrong password.
    // This avoids the generic "connection timed out" after 15s.
    if (m_isConnecting && m_connectAttemptCounter >= 5 &&
        m_lastWpaState == "4WAY_HANDSHAKE") {
        qDebug() << "[WifiManager] No progress in 4WAY_HANDSHAKE after 5 polls — wrong password";
        abortConnect(QString("Wrong password -- authentication failed with %1")
                         .arg(m_currentSSID));
        return;
    }

    // Max wait: ~15 seconds (8 polls at 2s interval)
    if (m_connectAttemptCounter > m_maxConnectWaitSeconds / (m_pollingInterval / 1000)) {
        qDebug() << "[WifiManager] Connection timeout after"
                 << m_maxConnectWaitSeconds << "seconds";
        // Generate specific error message based on observed wpa_state
        QString errorMsg;
        if (m_sawAssociating) {
            // We associated with the AP but never completed auth — the AP is
            // present and reachable, so the failure is the handshake → wrong
            // password. (On this AIC hardware the 4WAY window is so brief the
            // 2s poll often never catches it; "ever saw ASSOCIATING" is the
            // reliable discriminator between wrong password and not-in-range.)
            errorMsg = QString("Wrong password -- authentication failed with %1")
                          .arg(m_currentSSID);
        } else {
            errorMsg = QString("Cannot find %1 -- network not in range")
                          .arg(m_currentSSID);
        }
        abortConnect(errorMsg);
        return;
    }

    if (m_renewalPending && m_dhcpProcess->state() == QProcess::NotRunning) {
        qDebug() << "[WifiManager] Poll: DHCP renewal pending, retrying...";
        if (m_renewalRetries++ < 5) {
            m_renewalPending = false; // lambda will re-arm
            startDhcpRenewal();
            // Don't return — keep polling wpa_state so fast-fail still works
        } else {
            qDebug() << "[WifiManager] Poll: DHCP renewal gave up after retries";
            m_renewalPending = false;
        }
    }

    // While connecting, poll wpa_cli status to detect both wrong password
    // AND successful connection. The old code required m_pendingSsid to be
    // non-empty, but m_pendingSsid is only set by startAsyncConnectionCheck()
    // which runs when NOT connecting — so the SSID check was never started
    // during the connect flow, leaving the UI stuck at "Connecting...".
    if (m_isConnecting) {
        if (m_ssidCheckProcess->state() == QProcess::NotRunning)
            startSsidCheck();
    } else {
        // Not connecting — normal async check
        startAsyncConnectionCheck();
    }
}

// ==================== CONNECT STATE MACHINE ====================

void WifiManager::startAddNetwork()
{
    if (m_connectStep != 1) return;
    // P0: kill any existing wpa_cli process to ensure isolation between steps
    if (m_wpaCLIProcess->state() != QProcess::NotRunning) {
        m_wpaCLIProcess->kill();
    }
    m_wpaCLIBuffer.clear();
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0" << "add_network");
    qDebug() << "[WifiManager] startAddNetwork -> calling wpa_cli add_network";
}

void WifiManager::handleAddNetworkFinished()
{
    if (m_connectStep != 1) return;

    // IMPORTANT: read from m_wpaCLIBuffer, NOT readAllStandardOutput() —
    // readyRead has already consumed the process stdout into the buffer.
    QString output = m_wpaCLIBuffer.trimmed();
    m_wpaCLIBuffer.clear();
    bool ok;
    int netId = output.toInt(&ok);

    if (!ok || netId < 0) {
        qDebug() << "[WifiManager] add_network failed:" << output << "— fallback reconfigure";
        m_connectTimeoutTimer.start(12000);
        m_wpaCLIProcess->start("sh", QStringList() << "-c"
            << "wpa_cli -i wlan0 reconfigure && wpa_cli -i wlan0 reassociate");
        m_connectStep = 7; // fallback_reassociate
        return;
    }

    qDebug() << "[WifiManager] add_network ID:" << netId;
    m_pendingNetworkId = QString::number(netId);
    m_connectStep = 2; // set_ssid
    startSetSsid();
}

void WifiManager::startSetSsid()
{
    if (m_connectStep != 2) return;
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill(); // P0: ensure isolation between steps
    m_wpaCLIBuffer.clear();
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
        << "set_network" << m_pendingNetworkId << "ssid" << ('"' + m_currentSSID + '"'));
    qDebug() << "[WifiManager] startSetSsid -> set_network id ssid";
}

void WifiManager::handleSetSsidFinished()
{
    if (m_connectStep != 2) return;
    QString output = m_wpaCLIBuffer.trimmed();
    m_wpaCLIBuffer.clear(); // P0: consume and clear
    // wpa_cli set_network replies "OK" on success, "FAIL" on error.
    if (output != "OK") {
        qWarning() << "[WifiManager] set_network ssid FAILED:" << output;
        abortConnect("set_network ssid failed");
        return;
    }
    m_connectStep = 3; // psk_or_key_mgmt
    startSetPskOrKeyMgmt();
}

void WifiManager::startSetPskOrKeyMgmt()
{
    if (m_connectStep != 3) return;
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill(); // P0: ensure isolation between steps
    m_wpaCLIBuffer.clear();

    if (m_pendingPassword.isEmpty()) {
        // Open network
        m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
            << "set_network" << m_pendingNetworkId << "key_mgmt" << "NONE");
        qDebug() << "[WifiManager] startSetPskOrKeyMgmt -> set_network key_mgmt NONE";
    } else if (m_passwordIsHex) {
        // P0 fix (SHIFT bug): only treat as hex PSK when the flag says so —
        // never guess from the content. A user-typed 64-hex passphrase is a
        // passphrase (flag=false) and goes through the quoted branch below.
        m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
            << "set_network" << m_pendingNetworkId << "psk" << m_pendingPassword);
        qDebug() << "[WifiManager] startSetPskOrKeyMgmt -> set_network hex PSK";
    } else {
        m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
            << "set_network" << m_pendingNetworkId << "psk" << ('"' + m_pendingPassword + '"'));
        qDebug() << "[WifiManager] startSetPskOrKeyMgmt -> set_network passphrase";
    }
}

void WifiManager::handleSetPskOrKeyMgmtFinished()
{
    if (m_connectStep != 3) return;
    QString output = m_wpaCLIBuffer.trimmed();
    m_wpaCLIBuffer.clear();
    // wpa_cli set_network replies "OK" on success, "FAIL" on error.
    if (output != "OK") {
        qWarning() << "[WifiManager] set_network psk/key_mgmt FAILED:" << output;
        abortConnect("set_network psk or key_mgmt failed");
        return;
    }
    m_connectStep = 4; // save_config
    startSaveConfig();
}

void WifiManager::startSaveConfig()
{
    if (m_connectStep != 4) return;
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill(); // P0: ensure isolation between steps
    m_wpaCLIBuffer.clear();
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0" << "save_config");
    qDebug() << "[WifiManager] startSaveConfig -> save_config";
}

void WifiManager::handleSaveConfigFinished()
{
    if (m_connectStep != 4) return;
    QString output = m_wpaCLIBuffer.trimmed();
    m_wpaCLIBuffer.clear();
    // wpa_cli save_config replies "OK" on success, "FAIL" on error.
    if (output != "OK") {
        qWarning() << "[WifiManager] save_config FAILED:" << output;
        abortConnect("save_config failed");
        return;
    }
    m_connectStep = 5; // select_network
    startSelectNetwork();
}

void WifiManager::startSelectNetwork()
{
    if (m_connectStep != 5) return;
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill(); // P0: ensure isolation between steps
    m_wpaCLIBuffer.clear();
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
        << "select_network" << m_pendingNetworkId);
    qDebug() << "[WifiManager] startSelectNetwork -> select_network";
}

void WifiManager::handleSelectNetworkFinished()
{
    if (m_connectStep != 5) return;
    m_wpaCLIBuffer.clear();
    m_connectStep = 0; // done
    // NOTE: No 12s connect-timeout timer here. The DHCP renewal + status
    // polling can legitimately take up to 25s (m_maxConnectWaitSeconds).
    // A shorter timer here would fire a false "Connection timeout" while
    // a slow-but-successful connect is still in progress.
    // Renew DHCP first, then poll for connection — this makes wlan0 get a
    // fresh IP from the NEW AP instead of keeping the old network's lease.
    qDebug() << "[WifiManager] handleSelectNetworkFinished -> beginConnectPolling";
    beginConnectPolling();
}

void WifiManager::handleFallbackReconfigureFinished()
{
    if (m_connectStep != 6) return;
    m_connectStep = 0; // done
    qDebug() << "[WifiManager] handleFallbackReconfigureFinished -> beginConnectPolling";
    beginConnectPolling();
}

void WifiManager::handleFallbackReassociateFinished()
{
    if (m_connectStep != 7) return;
    m_connectStep = 0; // done
    qDebug() << "[WifiManager] handleFallbackReassociateFinished -> beginConnectPolling";
    beginConnectPolling();
}

// ==================== ASYNC CONNECTION CHECK ====================

bool WifiManager::interfaceExists()
{
    QFile ifaceFile("/sys/class/net/wlan0");
    return ifaceFile.exists();
}

void WifiManager::ensureWpaSupplicant()
{
    // If wpa_cli cannot reach the control socket, wpa_supplicant is not
    // running (or died). Start it with the current config — wpa_supplicant
    // will immediately associate to the best saved network (by priority).
    // This is a fallback for the case where the user killed wpa_supplicant
    // or it crashed; normally it is started by init scripts.
    QFile sock("/var/run/wpa_supplicant/wlan0");
    if (sock.exists())
        return; // control socket present, wpa_supplicant running

    qWarning() << "[WifiManager] wpa_supplicant control socket missing, starting it";
    QProcess::startDetached("wpa_supplicant",
        QStringList() << "-B" << "-i" << "wlan0" << "-c" << "/etc/wpa_supplicant.conf");
    // Give it a moment to initialize the control socket before use
    QThread::msleep(500);
}

bool WifiManager::interfaceIsUp()
{
    // Check admin flags first: IFF_UP (0x1) in /sys/class/net/wlan0/flags
    // This reflects whether the interface was administratively brought up,
    // regardless of operstate. operstate can be "down" even when the
    // interface is admin-UP (e.g. when wpa_supplicant died but the driver
    // is still loaded — iw scan still works in this state).
    QFile flagsFile("/sys/class/net/wlan0/flags");
    if (flagsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString flags = QString::fromUtf8(flagsFile.readAll()).trimmed();
        flagsFile.close();
        bool ok;
        unsigned long flagVal = flags.toULong(&ok, 0);
        if (ok && (flagVal & 0x1)) {
            // Admin UP — also accept operstate "up"/"unknown"/"dormant"
            QFile operFile("/sys/class/net/wlan0/operstate");
            if (operFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString state = QString::fromUtf8(operFile.readAll()).trimmed();
                operFile.close();
                // "down" with admin-UP = wpa_supplicant died, not interface lost
                // Accept all states when admin flag is UP
                return true;
            }
            // flags readable but operstate not → assume up
            return true;
        }
    }
    // Fallback: legacy operstate-only check
    QFile operFile("/sys/class/net/wlan0/operstate");
    if (!operFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QString state = QString::fromUtf8(operFile.readAll()).trimmed();
    operFile.close();
    return (state == "up" || state == "unknown" || state == "dormant");
}

void WifiManager::startAsyncConnectionCheck()
{
    if (!interfaceExists()) {
        m_connectionStatus = "Failed: wlan0 not found";
        emit connectionStateChanged();
        return;
    }
    // Prevent race condition: don't start a new check if one is already in progress
    if (m_checkStep != 0 || m_ipCheckProcess->state() != QProcess::NotRunning) {
        qDebug() << "[WifiManager] Async connection check already in progress, skipping";
        return;
    }
    m_checkStep = 1; // expecting IP
    m_pendingIpAddress.clear();
    m_pendingSsid.clear();
    startIpCheck();
}

void WifiManager::startDhcpRenewal()
{
    if (m_dhcpProcess->state() != QProcess::NotRunning) {
        qDebug() << "[WifiManager] DHCP renewal already running, skipping";
        return;
    }
    m_renewalPending = true;
    m_dhcpProcess->start("udhcpc", QStringList() << "-i" << "wlan0" << "-n" << "-q" << "-t" << "8");
    qDebug() << "[WifiManager] Started udhcpc for wlan0";
}

void WifiManager::startIpCheck()
{
    // Prevent race: don't start if already running
    if (m_ipCheckProcess->state() != QProcess::NotRunning) {
        qDebug() << "[WifiManager] IP check already running, skipping";
        return;
    }
    // P1 (A5/RF-27): arm the 5s hang-guard before launching the process
    m_checkTimeoutTimer.start();
    m_ipCheckProcess->start("ip", QStringList() << "-4" << "addr" << "show" << "wlan0");
}

void WifiManager::startSsidCheck()
{
    // Prevent race: don't start if already running
    if (m_ssidCheckProcess->state() != QProcess::NotRunning) {
        qDebug() << "[WifiManager] SSID check already running, skipping";
        return;
    }
    // P1 (A5/RF-27): arm the 5s hang-guard before launching the process
    m_checkTimeoutTimer.start();
    m_ssidCheckProcess->start("wpa_cli", QStringList() << "-i" << "wlan0" << "status");
}

// ==================== CHECK TIMEOUT (P1) ====================

void WifiManager::onCheckTimeout()
{
    // P1 (A5/RF-27/28): if ip addr show or wpa_cli status hangs for >5s,
    // kill both check processes and reset state. This prevents the UI from
    // getting stuck in "Connecting..." when the network stack is unresponsive.
    qWarning() << "[WifiManager] CHECK TIMEOUT: ip/wpa_cli status process hung, killing & resetting"
               << "(m_checkStep=" << m_checkStep << ", m_isConnecting=" << m_isConnecting << ")";

    // Kill any stuck check processes
    if (m_ipCheckProcess->state() != QProcess::NotRunning) {
        m_ipCheckProcess->kill();
        m_ipCheckProcess->waitForFinished(1000);
    }
    if (m_ssidCheckProcess->state() != QProcess::NotRunning) {
        m_ssidCheckProcess->kill();
        m_ssidCheckProcess->waitForFinished(1000);
    }

    // Reset check state
    m_checkStep = 0;
    m_pendingIpAddress.clear();
    m_pendingSsid.clear();

    // If we were in a connect flow, abort it (the user will see "Connection failed")
    if (m_isConnecting) {
        abortConnect("Status check timed out");
    }
}

// ==================== STATUS POLLING ====================

void WifiManager::finalizeConnectionCheck(const QString &ipAddress, const QString &ssid)
{
    if (!ipAddress.isEmpty() && !ssid.isEmpty()) {
        // While actively connecting to a target SSID, ignore the old connection
        // until wpa_supplicant actually switches over
        if (m_isConnecting && ssid != m_currentSSID) {
            qDebug() << "[WifiManager] Still on network:" << ssid << "— waiting for" << m_currentSSID;
            return;
        }

        bool wasConnected = m_isConnected;
        QString oldSSID = m_connectedSSID;
        QString oldIP   = m_connectedIP;
        m_isConnected = true;
        m_connectedIP = ipAddress;
        m_connectedSSID = ssid;
        m_connectionStatus = "Connected";
        m_isConnecting = false;
        // Clear any lingering error from a previous failed connect attempt,
        // so the error bar disappears on successful connection.
        // NOTE: in the auto-rollback path (ssid != m_currentSSID), lastError
        // was set to a specific "rolled back" message BEFORE this function —
        // don't clear that one.
        if (!m_lastError.isEmpty() && ssid == m_currentSSID) {
            m_lastError.clear();
            emit lastErrorChanged();
            qDebug() << "[WifiManager] Cleared lastError on successful connect";
        }
        if (!wasConnected || oldSSID != ssid || oldIP != ipAddress) {
            qDebug() << "[WifiManager] Connected:" << ssid << "@" << ipAddress;
            emit isConnectedChanged();
            emit connectedInfoChanged();
        }
        emit connectionStateChanged();
        stopStatusPolling();
        m_connectTimeoutTimer.stop(); // cancel any pending connect timeout
        m_connectTotalTimer.stop();   // connect succeeded, cancel global timeout

        // Persist the network to disk ONLY now that connect succeeded —
        // a wrong-password attempt must not be saved. Deferred write
        // (was previously written upfront in connectToNetwork).
        if (!m_savedSSIDs.contains(ssid)) {
            bool okWrite = writeWpaSupplicantConfig(ssid, m_pendingPassword);
            if (okWrite) {
                qDebug() << "[WifiManager] Persisted config for" << ssid;
                loadSavedNetworks(); // refresh in-memory saved list
                updateSavedFlags();
            } else {
                qWarning() << "[WifiManager] Failed to persist config for" << ssid;
            }
        }

        // refresh scan results để cập nhật connected flag ngay lập tức (fix B1)
        updateSavedFlags();
        // cập nhật trong list ngay lập tức
        emit scanResultsChanged();
        // Connect finished — resume auto-scan (was paused during connect)
        resumeAutoScan();

        // If just switched to a new network, schedule an IP re-check after 4s
        // in case DHCP takes longer than the polling window to update wlan0
        if (oldSSID != ssid || !wasConnected) {
            QTimer::singleShot(4000, this, [this]() {
                if (m_isConnected && !m_isConnecting)
                    startAsyncConnectionCheck();
            });
        }
    } else {
        // Not fully connected yet — just update status if in polling mode
        if (!m_isConnecting) {
            resumeAutoScan(); // connect flow over, resume auto-scan
            bool wasConnected = m_isConnected;
            m_isConnected = false;
            m_connectedSSID.clear();
            m_connectedIP.clear();
            m_connectionStatus = "Disconnected";
            if (wasConnected) emit isConnectedChanged();
            emit connectionStateChanged();
            m_connectTimeoutTimer.stop(); // no connect in flight, cancel timer
            m_connectTotalTimer.stop();
        }
        // If connecting, keep polling until timeout
    }
}

// ==================== FORGET ====================

void WifiManager::forgetNetwork(const QString &ssid)
{
    m_forgetMode = true;
    m_ssidToForget = ssid;

    // Abort any in-flight connect/forget state machine first
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill();
    m_wpaCLIBuffer.clear();
    m_connectStep = 0;
    m_isConnecting = false;
    m_connectTimeoutTimer.stop();
    m_connectTotalTimer.stop();
    stopStatusPolling();

    // Serialize config file access with concurrent connect operations
    QMutexLocker locker(&m_configMutex);

    // Read current config, filter out the target network using shared helper
    QStringList headerLines;
    QList<ConfigBlock> networkBlocks;
    if (!readWpaSupplicantConfig("/etc/wpa_supplicant.conf", headerLines, networkBlocks, ssid)) {
        qWarning() << "[WifiManager] ERROR: Cannot read wpa_supplicant.conf during forget";
        m_connectionStatus = "Failed: Cannot read config";
        emit connectionStateChanged();
        m_forgetMode = false;
        m_ssidToForget.clear();
        return;
    }

    // Write back config without the forgotten network using shared helper
    if (!writeWpaSupplicantConfig("/etc/wpa_supplicant.conf", headerLines, networkBlocks)) {
        qWarning() << "[WifiManager] ERROR: Cannot write wpa_supplicant.conf during forget";
        m_connectionStatus = "Failed: Cannot write config";
        emit connectionStateChanged();
        m_forgetMode = false;
        m_ssidToForget.clear();
        return;
    }

    // If the forgotten network was the connected one, disconnect
    if (m_connectedSSID == ssid) {
        m_isConnected = false;
        m_connectedSSID.clear();
        m_connectedIP.clear();
        m_connectionStatus = "Disconnected";
        emit isConnectedChanged();
        emit connectedInfoChanged();
        emit connectionStateChanged();
        stopStatusPolling();
        m_connectTimeoutTimer.stop();
        m_connectTotalTimer.stop();

        // Actually disconnect the WiFi interface (not just local state).
        // Async (startDetached) so the UI never freezes.
        QProcess::startDetached("wpa_cli", QStringList() << "-i" << "wlan0" << "disconnect");
    }

    // Reconfigure wpa_supplicant with the updated config.
    // Async (startDetached) — no UI freeze, no shared-buffer race with the
    // async state machine (wpa_cli is a separate daemon, not m_wpaCLIProcess).
    QProcess::startDetached("wpa_cli", QStringList() << "-i" << "wlan0" << "reconfigure");

    qDebug() << "[WifiManager] Forgot network:" << ssid;

    // Refresh saved-network list and UI flags
    loadSavedNetworks();
    updateSavedFlags();
    emit scanResultsChanged(); // refresh list view ngay lập tức (fix B2)

    m_forgetMode = false;
    m_ssidToForget.clear();
}

// ==================== SAVED NETWORKS ====================

void WifiManager::loadSavedNetworks()
{
    // NOTE: no QMutexLocker here — forgetNetwork() holds m_configMutex while
    // calling this (via connectToNetwork's writeWpaSupplicantConfig too). A
    // nested lock of the same non-recursive QMutex on one thread would deadlock.
    // The whole app runs on a single thread (Qt event loop), so the mutex is
    // only meant to serialize file access, not for thread safety.
    m_savedSSIDs.clear();

    // P1 Security (DR-19): defense-in-depth — verify config permissions on load.
    // If the file is world-readable (e.g. created by an older version without
    // chmod 0600), auto-repair it and warn.
    const QString confPath = "/etc/wpa_supplicant.conf";
    struct stat confStat;
    if (::stat(confPath.toUtf8().constData(), &confStat) == 0) {
        if (confStat.st_mode & 0077) {
            qWarning() << "[WifiManager] Config file" << confPath
                       << "has insecure permissions:" << QString::number(confStat.st_mode, 8)
                       << "— auto-repairing to 0600";
            ::chmod(confPath.toUtf8().constData(), 0600);
        }
    }

    QFile confFile(confPath);
    if (!confFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QTextStream in(&confFile);
    bool inNetwork = false;
    QString currentSSID;

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith("network={")) {
            inNetwork = true;
            currentSSID.clear();
        } else if (inNetwork) {
            if (line == "}") {
                if (!currentSSID.isEmpty())
                    m_savedSSIDs.insert(currentSSID);
                inNetwork = false;
            } else if (line.startsWith("ssid=\"")) {
                currentSSID = line.mid(6);
                if (currentSSID.endsWith("\""))
                    currentSSID.chop(1);
            }
        }
    }
    confFile.close();
    qDebug() << "[WifiManager] Saved networks:" << m_savedSSIDs;
}

void WifiManager::resumeAutoScan()
{
    // Resume auto-scan after a connect attempt finishes. Called from
    // finalizeConnectionCheck (success/fail) and onConnectTotalTimeout.
    if (m_autoScanTimer && !m_autoScanTimer->isActive()) {
        m_autoScanTimer->start(m_autoScanInterval);
        qDebug() << "[WifiManager] Auto-scan resumed (interval" << m_autoScanInterval << "ms)";
    }
    // NOTE: No immediate scan() here — the timer fires within the interval,
    // and scanning right after a B2K "saved network update" would double-scan
    // (timer fires while the scan is still running, or a scan happens twice
    // in quick succession). updateSavedFlags() already refreshes the list.
}

QString WifiManager::readSavedPassword(const QString &ssid) const
{
    // No QMutexLocker here — see loadSavedNetworks() comment. Single-threaded,
    // and connectSaved() may be called from within a locked context.
    QFile confFile("/etc/wpa_supplicant.conf");
    if (!confFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    QTextStream in(&confFile);
    bool inTarget = false;
    QString psk;

    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.startsWith("network={")) {
            inTarget = false;
            psk.clear();
        } else if (line == "}") {
            if (inTarget) {
                confFile.close();
                return psk; // empty string = open network
            }
        } else if (line.startsWith("ssid=\"")) {
            QString s = line.mid(6);
            if (s.endsWith("\"")) s.chop(1);
            inTarget = (s == ssid);
        } else if (inTarget && line.startsWith("psk=")) {
            // Handle both psk="password" and hex PSK (64 hex chars)
            if (line.startsWith("psk=\"")) {
                psk = line.mid(5);
                if (psk.endsWith("\"")) psk.chop(1);
            } else {
                // Hex PSK: psk=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
                psk = line.mid(4);
            }
        } else if (inTarget && isHexPskString(line)) {
            // LEGACY FIX: older app versions wrote the hex PSK WITHOUT the
            // "psk=" prefix (bare 64-hex line). Treat it as the PSK so saved
            // networks from old configs still connect. Connect will rewrite
            // the config with a proper psk= line, healing the file.
            psk = line;
        }
    }
    confFile.close();
    return QString();
}

void WifiManager::connectSaved(const QString &ssid)
{
    if (!m_savedSSIDs.contains(ssid)) {
        qDebug() << "[WifiManager] connectSaved: no saved credentials for" << ssid;
        return;
    }
    const QString password = readSavedPassword(ssid);
    // P0 fix (SHIFT bug): determine if the retrieved password is a hex PSK.
    // readSavedPassword always returns either a hex PSK (64 hex chars) or empty.
    const bool passwordIsHex = isHexPskString(password);
    qDebug() << "[WifiManager] Connecting to saved network:" << ssid
             << "(passwordIsHex=" << passwordIsHex << ")";
    connectToNetwork(ssid, password, passwordIsHex);
}

void WifiManager::updateSavedFlags()
{
    bool changed = false;
    for (int i = 0; i < m_scanResults.size(); ++i) {
        QVariantMap item = m_scanResults.at(i).toMap();
        const QString s = item["ssid"].toString();
        const bool savedNow     = m_savedSSIDs.contains(s);
        const bool connectedNow = (s == m_connectedSSID && m_isConnected);
        if (item["saved"].toBool() != savedNow || item["connected"].toBool() != connectedNow) {
            item["saved"]     = savedNow;
            item["connected"] = connectedNow;
            m_scanResults[i]  = item;
            changed = true;
        }
    }
    if (changed)
        emit scanResultsChanged();
}

// ==================== AUTO SCAN ====================

void WifiManager::startAutoScan()
{
    if (!m_autoScanTimer) {
        m_autoScanTimer = new QTimer(this);
        connect(m_autoScanTimer, &QTimer::timeout, this, [this]() {
            // Skip auto-scan while connecting or doing DHCP renewal —
            // scanning during those operations would disturb wpa_supplicant
            // and could cause connection failures.
            if (m_isConnecting || m_renewalPending || m_isScanning) {
                qDebug() << "[WifiManager] Auto-scan skipped: busy";
                return;
            }
            scan();
        });
    }
    m_autoScanTimer->start(m_autoScanInterval); // adaptive interval
    // Initial scan
    scan();
}

void WifiManager::stopAutoScan()
{
    if (m_autoScanTimer) {
        m_autoScanTimer->stop();
    }
}

// ==================== INTERFACE WATCHDOG ====================

void WifiManager::startInterfaceWatchdog()
{
    if (!m_interfaceWatchdog) {
        m_interfaceWatchdog = new QTimer(this);
        m_interfaceWatchdog->setInterval(5000);
        QObject::connect(m_interfaceWatchdog, &QTimer::timeout,
                         this, &WifiManager::onInterfaceWatchdogTimeout);
    }
    m_interfaceWatchdog->start();
}

void WifiManager::stopInterfaceWatchdog()
{
    if (m_interfaceWatchdog)
        m_interfaceWatchdog->stop();
}

void WifiManager::onInterfaceWatchdogTimeout()
{
    bool ifacePresent = interfaceExists();
    bool ifaceUp = ifacePresent && interfaceIsUp();

    if (ifacePresent && ifaceUp) {
        // Interface is healthy. If it previously went down (debounced),
        // auto-reconnect to the last known network if it's saved.
        bool wasDown = (m_watchdogDownCount >= 2);
        m_watchdogDownCount = 0; // healthy, reset debounce
        m_recoveryAttempts = 0;  // healthy, reset recovery counter

        if (wasDown && !m_isConnecting) {
            if (!m_lastKnownSSID.isEmpty()) {
                QString ssid = m_lastKnownSSID;
                qDebug() << "[WifiManager] Watchdog: wlan0 back up, auto-reconnecting to"
                         << ssid;
                // Clear immediately to prevent re-trigger on next tick
                m_lastKnownSSID.clear();
                if (m_savedSSIDs.contains(ssid))
                    connectSaved(ssid);
                else
                    scan();
                return;
            }
            qDebug() << "[WifiManager] Watchdog: wlan0 back up, triggering scan";
            scan();
        }
        return;
    }

    // Interface missing or operstate down.
    //
    // Do NOT treat as lost while a connect/DHCP renewal is in progress —
    // wlan0 legitimately drops to "down" briefly when switching APs, and
    // udhcpc may be mid-renewal. The connect state machine owns wlan0
    // during that window; the watchdog must not fight it.
    if (m_isConnecting || m_renewalPending) {
        m_watchdogDownCount = 0; // reset debounce; connect flow owns state
        qDebug() << "[WifiManager] Watchdog: wlan0 down but connecting, skipping cleanup";
        return;
    }

    // Debounce: require 3 consecutive down checks (~6s) so a momentary
    // reassociate blip doesn't trigger a false "Interface lost". With 2s
    // interval, 3 checks = 6s (consistent with ~5s sleep test).
    if (++m_watchdogDownCount < 3) {
        qDebug() << "[WifiManager] Watchdog: wlan0 down (check"
                 << m_watchdogDownCount << ")";
        return;
    }

    qDebug() << "[WifiManager] Watchdog: wlan0"
             << (ifacePresent ? "is down" : "disappeared");

    if (m_isConnected) {
        // Remember which network we were on so we can auto-reconnect later
        m_lastKnownSSID = m_connectedSSID;
        qDebug() << "[WifiManager] Cleaning up lost connection state (was on"
                 << m_lastKnownSSID << ")";
        m_isConnected = false;
        m_connectedSSID.clear();
        m_connectedIP.clear();
        m_connectionStatus = "Interface lost";
        emit isConnectedChanged();
        emit connectedInfoChanged();
        emit connectionStateChanged();
        stopStatusPolling();
        m_connectTimeoutTimer.stop(); // cancel any pending connect timeout
        m_connectTotalTimer.stop();   // interface lost, abort connect
        // Kill any in-flight wpa_cli
        if (m_wpaCLIProcess->state() != QProcess::NotRunning)
            m_wpaCLIProcess->kill();
    }

    // Clear stale scan results so UI doesn't show phantom networks
    if (!m_scanResults.isEmpty()) {
        m_scanResults.clear();
        emit scanResultsChanged();
    }

    // Auto-recovery: bring the interface back up if possible.
    // Caps at 5 attempts to avoid hammering; resets when interface recovers.
    if (m_recoveryAttempts < 5 && !m_recoveryInProgress) {
        startInterfaceRecovery();
    } else if (m_recoveryAttempts >= 5 && m_watchdogDownCount % 5 == 0) {
        qWarning() << "[WifiManager] Interface recovery exhausted after"
                   << m_recoveryAttempts << "attempts";
    }
}

// ==================== INTERFACE RECOVERY ====================

void WifiManager::startInterfaceRecovery()
{
    m_recoveryInProgress = true;
    m_recoveryAttempts++;

    if (!m_recoveryProcess) {
        m_recoveryProcess = new QProcess(this);
        m_recoveryProcess->setProcessChannelMode(QProcess::SeparateChannels);
        QObject::connect(m_recoveryProcess,
                         QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            m_recoveryInProgress = false;
            if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                qDebug() << "[WifiManager] Recovery command ok, re-scanning";
                if (interfaceExists() && !m_isScanning && !m_isConnecting) {
                    scan();
                }
            } else {
                qWarning() << "[WifiManager] Recovery command failed:" << exitCode << exitStatus;
            }
        });
    }

    // If wpa_supplicant's control socket is stale, remove it first.
    // Then bring the interface up and restart wpa_supplicant if needed.
    qDebug() << "[WifiManager] Interface recovery attempt" << m_recoveryAttempts;
    m_recoveryProcess->start("sh", QStringList() << "-c"
        << "rm -f /var/run/wpa_supplicant/wlan0; "
           "ip link set wlan0 up 2>/dev/null; "
           "ps | grep -q wpa_supplicant || wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf 2>/dev/null; "
           "sleep 1; ip link set wlan0 up 2>/dev/null");
}

// ==================== TIMEOUT HANDLERS ====================

void WifiManager::onScanTimeout()
{
    if (m_isScanning) {
        qDebug() << "[WifiManager] Scan timeout, killing process";
        if (m_wifiScanProcess->state() != QProcess::NotRunning)
            m_wifiScanProcess->kill();
        m_isScanning = false;
        emit isScanningChanged();
        m_connectionStatus = "Scan failed";
        emit connectionStateChanged();
    }
}

void WifiManager::onConnectTimeout()
{
    if (m_isConnecting) {
        qDebug() << "[WifiManager] Connect timeout";
        // Generate specific error message based on last observed wpa_state
        QString errorMsg;
        if (m_lastWpaState == "ASSOCIATING") {
            errorMsg = QString("Cannot connect to %1 -- association failed (timeout)")
                          .arg(m_currentSSID);
        } else if (m_lastWpaState == "SCANNING" || m_lastWpaState == "INACTIVE") {
            errorMsg = QString("Cannot find %1 -- network not in range")
                          .arg(m_currentSSID);
        } else if (m_lastWpaState == "4WAY_HANDSHAKE") {
            errorMsg = QString("Wrong password -- authentication failed with %1")
                          .arg(m_currentSSID);
        } else {
            errorMsg = QString("Cannot connect to %1 -- connection timed out")
                          .arg(m_currentSSID);
        }
        m_connectionStatus = errorMsg;
        m_isConnecting = false;
        m_isConnected = false;          // Reset connected flag
        m_connectedSSID.clear();
        m_connectedIP.clear();
        emit connectionStateChanged();
        emit isConnectedChanged();
        emit connectedInfoChanged();
        stopStatusPolling(); // also stop polling
        m_connectTotalTimer.stop(); // cancel global timeout too
        // Also set lastError for the UI error bar
        m_lastError = errorMsg;
        emit lastErrorChanged();
    }
}

void WifiManager::onConnectTotalTimeout()
{
    if (m_isConnecting) {
        // Check if this is a handshake failure (wrong password)
        if (m_handshakeFailCount >= 3) {
            qDebug() << "[WifiManager] Wrong password detected (handshake failed" << m_handshakeFailCount << "times)";
            abortConnect(QString("Wrong password -- authentication failed with %1")
                             .arg(m_currentSSID));
        } else {
            qDebug() << "[WifiManager] Global connect timeout after 90s";
            // Generate specific error message based on last observed wpa_state
            QString errorMsg;
            if (m_lastWpaState == "ASSOCIATING") {
                errorMsg = QString("Cannot connect to %1 -- association failed (timeout)")
                              .arg(m_currentSSID);
            } else if (m_lastWpaState == "SCANNING" || m_lastWpaState == "INACTIVE") {
                errorMsg = QString("Cannot find %1 -- network not in range")
                              .arg(m_currentSSID);
            } else if (m_lastWpaState == "4WAY_HANDSHAKE") {
                errorMsg = QString("Wrong password -- authentication failed with %1")
                              .arg(m_currentSSID);
            } else {
                errorMsg = QString("Cannot connect to %1 -- connection timed out")
                              .arg(m_currentSSID);
            }
            abortConnect(errorMsg);
        }
    }
}


// ============================================================
// STATIC HELPER FUNCTIONS — Config file
// ============================================================

QString WifiManager::extractSSIDFromBlock(const QString &block)
{
    const QStringList lines = block.split('\n');
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("ssid=\"")) {
            QString ssid = trimmed.mid(6);
            if (ssid.endsWith('\"'))
                ssid.chop(1);
            return ssid;
        }
    }
    return QString();
}

bool WifiManager::blockContainsSSID(const QString &block, const QString &ssid)
{
    return extractSSIDFromBlock(block) == ssid;
}

bool WifiManager::readWpaSupplicantConfig(const QString &path,
    QStringList &headerLines, QList<ConfigBlock> &networkBlocks,
    const QString &ssidToSkip)
{
    headerLines.clear();
    networkBlocks.clear();

    QFile confFile(path);
    if (!confFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qDebug() << "[WifiManager] Cannot read" << path;
        return false;
    }

    QTextStream in(&confFile);
    bool inNetwork = false;
    QString currentBlock;

    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().startsWith("network={")) {
            inNetwork = true;
            currentBlock = line + "\n";
        } else if (inNetwork) {
            if (line.trimmed() == "}") {
                currentBlock += line + "\n";
                // Process the completed block
                if (!currentBlock.isEmpty()) {
                    QString blockSSID = extractSSIDFromBlock(currentBlock);
                    if (!ssidToSkip.isEmpty() && !blockSSID.isEmpty() && blockSSID == ssidToSkip) {
                        // Skip this block (it matches the SSID to skip)
                    } else {
                        // Keep this block
                        ConfigBlock cb;
                        cb.content = currentBlock;
                        cb.hasSSID = !blockSSID.isEmpty();
                        cb.ssid = blockSSID;
                        networkBlocks.append(cb);
                    }
                }
                inNetwork = false;
                currentBlock.clear();
            } else {
                currentBlock += line + "\n";
            }
        } else if (!line.trimmed().startsWith("network=")) {
            headerLines.append(line);
        }
    }
    confFile.close();
    return true;
}

bool WifiManager::writeWpaSupplicantConfig(const QString &path,
    const QStringList &headerLines, const QList<ConfigBlock> &networkBlocks)
{
    // Atomic write: write to temp file in same dir, fsync, then rename.
    // This prevents partial/corrupt config if power fails mid-write.
    QFileInfo fi(path);
    QString dir = fi.absolutePath();
    QString tempPath = dir + "/.wpa_supplicant.conf.tmp";

    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "[WifiManager] Cannot write temp config" << tempPath;
        return false;
    }

    QTextStream out(&tempFile);
    for (const QString &h : headerLines)
        out << h << "\n";
    for (const ConfigBlock &cb : networkBlocks)
        out << cb.content;

    // Ensure data is on disk before rename — QFile::flush() calls fsync on POSIX
    if (!tempFile.flush()) {
        qWarning() << "[WifiManager] Failed to flush temp file";
        tempFile.close();
        QFile::remove(tempPath);
        return false;
    }
    tempFile.close();

    // Atomic rename (POSIX rename guarantees atomic overwrite on Linux)
    if (::rename(tempPath.toUtf8().constData(), path.toUtf8().constData()) != 0) {
        qWarning() << "[WifiManager] Atomic rename failed:" << tempPath << "->" << path;
        QFile::remove(tempPath);
        return false;
    }

    // Optional: fsync directory to persist rename (not strictly needed on ext4/overlayfs)
    // but good practice for metadata durability.
    QFile dirFile(dir);
    if (dirFile.open(QIODevice::ReadOnly)) {
        dirFile.flush(); // fsync directory
        dirFile.close();
    }

    // P1 Security (DR-19): Set 0600 permissions on wpa_supplicant.conf
    // so the hex PSK of all saved networks is only readable by root.
    // After atomic ::rename(), the temp file's mode becomes the final
    // file's mode. We must chmod the final path to 0600 explicitly.
    if (::chmod(path.toUtf8().constData(), 0600) != 0) {
        qWarning() << "[WifiManager] chmod 0600 failed on" << path
                   << "errno:" << errno << "(" << strerror(errno) << ")";
        // Non-fatal: config still works, but warn for defense-in-depth.
    }

    // Defense-in-depth: also chown to root:root (config is written by root app,
    // wpa_supplicant daemon also runs as root on Luckfox Buildroot).
    // Verify in stat check below on load.
    const int chownResult = ::chown(path.toUtf8().constData(), 0, 0);
    if (chownResult != 0) {
        qWarning() << "[WifiManager] chown root:root failed on" << path
                   << "errno:" << errno << "(" << strerror(errno) << ")";
    }

    qDebug() << "[WifiManager] Atomic write ok (0600):" << path;
    return true;
}

// Unified teardown for failed/aborted connect flow.
// Centralizes teardown that was previously duplicated in onStatusPollingTimeout,
// onConnectTotalTimeout, and the config-write-failure path — including stopping
// the global 90s timer and pausing/resuming auto-scan consistently.
void WifiManager::abortConnect(const QString &reason)
{
    qDebug() << "[WifiManager] ABORT CONNECT:" << reason;

    m_isConnecting = false;
    m_isConnected = false;
    m_connectedSSID.clear();
    m_connectedIP.clear();
    m_connectionStatus = reason;
    // Set lastError for UI error bar (unless already set by more specific logic like auto-rollback)
    if (m_lastError.isEmpty() || m_lastError == reason) {
        m_lastError = reason;
        emit lastErrorChanged();
    }
    m_connectStep = 0;
    m_wpaCLIBuffer.clear();
    m_renewalPending = false;
    m_handshakeFailCount = 0;
    m_seenFourWay = false;
    m_fourWayConsecutive = 0;

    // Clean up the temporary network entry added during the connect flow.
    // We no longer write to disk BEFORE connect (to avoid persisting wrong
    // passwords), so there's nothing on disk to remove. However wpa_supplicant
    // may still have the transient network in its runtime config (added via
    // wpa_cli add_network). Remove it to avoid accumulating stale entries
    // in the running config over multiple failed connect attempts.
    if (!m_pendingNetworkId.isEmpty()) {
        qDebug() << "[WifiManager] postConnectCleanup: removing temporary network"
                 << m_pendingNetworkId << "from wpa_supplicant runtime config";
        QProcess::startDetached("sh", QStringList() << "-c"
            << "wpa_cli -i wlan0 remove_network " + m_pendingNetworkId);
        m_pendingNetworkId.clear();
    }

    stopStatusPolling();
    m_connectTimeoutTimer.stop();
    m_connectTotalTimer.stop();

    // Kill any in-flight processes
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill();
    if (m_dhcpProcess->state() != QProcess::NotRunning)
        m_dhcpProcess->kill();
    if (m_ipCheckProcess->state() != QProcess::NotRunning)
        m_ipCheckProcess->kill();
    if (m_ssidCheckProcess->state() != QProcess::NotRunning)
        m_ssidCheckProcess->kill();

    emit connectionStateChanged();
    emit isConnectedChanged();
    emit connectedInfoChanged();

    // Auto-rollback to a saved network on failure (RF-42).
    // If we were connected to a saved network before this connect attempt,
    // reconnect to it after 1.5s so the UI has time to show the error.
    // If we were NOT connected this session (e.g. app just restarted and the
    // user tapped connect before auto-connect finished, so m_previousSSID is
    // empty), fall back to the strongest saved network currently in view.
    QString rollbackTarget = m_previousSSID;
    m_previousSSID.clear();

    if (rollbackTarget.isEmpty()) {
        // Not previously connected this session — pick the strongest saved
        // network from the last scan (excluding the one we just failed on).
        int bestSignal = -100;
        for (const QVariant &item : m_scanResults) {
            const QVariantMap map = item.toMap();
            const QString s = map.value("ssid").toString();
            const int sig = map.value("signal").toInt();
            if (s != m_currentSSID && m_savedSSIDs.contains(s) && sig > bestSignal) {
                bestSignal = sig;
                rollbackTarget = s;
            }
        }
        if (!rollbackTarget.isEmpty()) {
            qDebug() << "[WifiManager] Rollback fallback: strongest saved network"
                     << rollbackTarget << "@" << bestSignal << "dBm";
        }
    }

    // Don't rollback if target is empty, is the network we just failed on,
    // or is no longer saved.
    if (!rollbackTarget.isEmpty() &&
        rollbackTarget != m_currentSSID &&
        m_savedSSIDs.contains(rollbackTarget)) {
        qDebug() << "[WifiManager] Auto-rollback: reconnecting to saved network" << rollbackTarget;
        // Set specific lastError for auto-rollback case (overrides generic reason)
        m_lastError = "Could not connect to " + m_currentSSID
                    + " -- rolled back to " + rollbackTarget;
        emit lastErrorChanged();
        QTimer::singleShot(1500, this, [this, rollbackTarget]() {
            if (!m_isConnecting && !m_isConnected) {
                m_connectionStatus = "Reconnecting to " + rollbackTarget;
                emit connectionStateChanged();
                connectSaved(rollbackTarget);
            }
        });
    }

    // Refresh list immediately (fixes B1/B2) and resume auto-scan
    updateSavedFlags();
    emit scanResultsChanged();
    resumeAutoScan(); // connect flow over, resume auto-scan
}

// Helper: Check if a string is already a hex PSK (64 hex characters)
bool WifiManager::isValidSSID(const QString &ssid)
{
    // Empty or whitespace-only SSID is invalid
    if (ssid.trimmed().isEmpty())
        return false;

    // 802.11 SSID max is 32 bytes (octets), not characters.
    // A multibyte UTF-8 char occupies >1 byte, so counting chars
    // would under-report and allow an over-long UTF-8 SSID that
    // wpa_supplicant rejects. Enforce the byte limit.
    if (ssid.toUtf8().size() > 32)
        return false;

    // Reject control characters (newline, tab, carriage return, ESC...).
    // A newline inside SSID would break /etc/wpa_supplicant.conf parsing
    // (config injection). Also reject starting/ending whitespace since
    // those confuse the UI and the sim-command splitter.
    for (const QChar &ch : ssid) {
        if (ch.unicode() < 0x20)
            return false; // control char (includes \n \t \r ESC)
    }
    if (ssid.startsWith(' ') || ssid.endsWith(' '))
        return false;

    return true;
}

// Helper: Check if a string is already a hex PSK (64 hex characters)
bool WifiManager::isHexPskString(const QString &str)
{
    if (str.size() != 64) return false;
    for (const QChar &ch : str) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

// Helper: PBKDF2-HMAC-SHA1 (used for WPA2 PSK generation)
// Implements the PBKDF2 algorithm as per RFC 2898, using HMAC-SHA1 via Qt.
static QByteArray pbkdf2HmacSha1(const QByteArray &password, const QByteArray &salt,
                                  int iterations, int dkLen)
{
    const int hLen = 20; // SHA-1 output length in bytes
    QByteArray derivedKey;
    derivedKey.reserve(dkLen);

    // Number of blocks needed (each block = hLen bytes)
    int blocks = (dkLen + hLen - 1) / hLen;

    for (int block = 1; block <= blocks; ++block) {
        // U_1 = HMAC-SHA1(password, salt || INT_BE(block))
        QByteArray blockSalt = salt;
        blockSalt.append((block >> 24) & 0xFF);
        blockSalt.append((block >> 16) & 0xFF);
        blockSalt.append((block >> 8) & 0xFF);
        blockSalt.append(block & 0xFF);

        QByteArray T; // T_i = U_1 ⊕ U_2 ⊕ ... ⊕ U_c
        QByteArray U = QMessageAuthenticationCode::hash(blockSalt, password,
                                                        QCryptographicHash::Sha1);

        T = U;

        for (int j = 1; j < iterations; ++j) {
            // U_j = HMAC-SHA1(password, U_{j-1})
            U = QMessageAuthenticationCode::hash(U, password,
                                                  QCryptographicHash::Sha1);
            // XOR
            for (int k = 0; k < hLen; ++k) {
                T[k] = T[k] ^ U[k];
            }
        }

        derivedKey.append(T);
    }

    return derivedKey.left(dkLen);
}

// Generate hex PSK using PBKDF2-SHA1 (standard WPA2 PSK algorithm)
// No external binary needed — implements RFC 2898 directly.
QString WifiManager::generateHexPsk(const QString &ssid, const QString &password)
{
    // WPA2-PSK = PBKDF2(HMAC-SHA1, password, ssid, 4096, 256)
    QByteArray pskRaw = pbkdf2HmacSha1(password.toUtf8(), ssid.toUtf8(), 4096, 32);

    if (pskRaw.size() != 32) {
        qWarning() << "[WifiManager] PBKDF2 generated wrong size:" << pskRaw.size();
        return QString();
    }

    // Convert to hex string (64 hex chars)
    QString hexPsk = QString::fromLatin1(pskRaw.toHex());
    if (hexPsk.size() != 64) {
        qWarning() << "[WifiManager] hex PSK wrong length:" << hexPsk.size();
        return QString();
    }

    return "psk=" + hexPsk;
}

