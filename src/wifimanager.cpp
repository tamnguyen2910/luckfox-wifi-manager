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
#include <unistd.h> // for ::rename (POSIX)

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
    , m_maxConnectWaitSeconds(25)
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
    , m_wpaCLIBuffer()
    , m_dhcpProcess(nullptr)
    , m_autoScanTimer(nullptr)
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
                         if (m_checkStep == 2) {
                             // Parse SSID and wpa_state from wpa_cli status output
                             QString output = QString::fromUtf8(m_ssidCheckProcess->readAllStandardOutput());
                             const QStringList lines = output.split('\n');
                             bool completed = false;
                             for (const QString &line : lines) {
                                 const QString t = line.trimmed();
                                 if (t.startsWith("ssid="))
                                     m_pendingSsid = t.mid(5);
                                 else if (t.startsWith("wpa_state=COMPLETED"))
                                     completed = true;
                             }
                             // Only declare connected when wpa_supplicant fully authenticated
                             if (!completed)
                                 m_pendingSsid.clear();
                             // Complete connection check with parsed data
                             finalizeConnectionCheck(m_pendingIpAddress, m_pendingSsid);
                             m_checkStep = 0;
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

    // Check initial connection state (trigger async check)
    QTimer::singleShot(500, this, [this]() {
        if (interfaceExists()) {
            startAsyncConnectionCheck();
        }
    });

    // Load saved networks from config for remember-password feature
    loadSavedNetworks();
}

WifiManager::~WifiManager()
{
    m_statusPollTimer.stop();
    m_scanTimeoutTimer.stop();
    m_connectTimeoutTimer.stop();
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

    m_isScanning = true;
    emit isScanningChanged();

    m_currentScanOutput.clear();
    m_scanResults.clear();

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
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);
    m_scanTimeoutTimer.stop();

    parseWifiScanOutput(m_currentScanOutput);

    m_isScanning = false;
    emit isScanningChanged();
    emit scanResultsChanged();
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

void WifiManager::connectToNetwork(const QString &ssid, const QString &password)
{
    // Kill both wpa_cli and scan process to avoid race conditions
    if (m_wpaCLIProcess->state() != QProcess::NotRunning)
        m_wpaCLIProcess->kill();
    if (m_wifiScanProcess->state() != QProcess::NotRunning)
        m_wifiScanProcess->kill();

    if (!interfaceExists()) {
        m_connectionStatus = "Failed: wlan0 not found";
        emit connectionStateChanged();
        qDebug() << "[WifiManager] ERROR: wlan0 interface does not exist";
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
    emit connectionStateChanged();

    qDebug() << "[WifiManager] Connecting to:" << ssid;

    // Step 1: Write wpa_supplicant.conf
    if (!writeWpaSupplicantConfig(ssid, cleanPassword)) {
        m_isConnecting = false;
        m_connectionStatus = "Failed to write config";
        emit connectionStateChanged();
        return;
    }
    loadSavedNetworks(); // refresh in-memory saved list

    // Store password for async steps
    m_pendingPassword = cleanPassword;
    
    // Start async connection state machine
    m_connectStep = 1; // add_network
    startAddNetwork();
}

void WifiManager::disconnectFromNetwork()
{
    // Use wpa_cli to disconnect gracefully — preserves saved networks
    QProcess discProc;
    discProc.start("wpa_cli", QStringList() << "-i" << "wlan0" << "disconnect");
    discProc.waitForFinished(3000);

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
    qDebug() << "[WifiManager] Disconnected from current network";

    // Refresh scan results sau disconnect (fix B2)
    updateSavedFlags();
    emit scanResultsChanged();
}

bool WifiManager::writeWpaSupplicantConfig(const QString &ssid, const QString &password)
{
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
        // Check if password is already a hex PSK (64 hex chars — e.g. reconnecting from saved)
        bool alreadyHex = isHexPskString(password);
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

void WifiManager::onWpaCLIFinished(int exitCode, QProcess::ExitStatus exitStatus)
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
    // Always renew DHCP first when switching networks — wpa_supplicant
    // only handles 802.11, not DHCP. Without renewal, wlan0 keeps the
    // OLD AP's lease and the SAME IP shows up for every network.
    m_renewalRetries = 0;
    qDebug() << "[WifiManager] beginConnectPolling: starting DHCP renewal";
    startDhcpRenewal();
}

void WifiManager::onStatusPollingTimeout()
{
    m_connectAttemptCounter++;

    // Max wait: ~25 seconds (13 polls at 2s interval)
    if (m_connectAttemptCounter > m_maxConnectWaitSeconds / (m_pollingInterval / 1000)) {
        stopStatusPolling();
        m_connectionStatus = "Connection failed";
        m_isConnecting = false;
        m_renewalPending = false; // give up on DHCP too
        emit connectionStateChanged();
        qDebug() << "[WifiManager] Connection timeout after"
                 << m_maxConnectWaitSeconds << "seconds";
        return;
    }

    // If we're waiting on DHCP to assign a fresh IP (switched networks),
    // run the renewal before checking — otherwise the old lease's IP
    // would be reported for the new network.
    if (m_renewalPending && m_dhcpProcess->state() == QProcess::NotRunning) {
        qDebug() << "[WifiManager] Poll: DHCP renewal pending, retrying...";
        if (m_renewalRetries++ < 5) {
            m_renewalPending = false; // let handleDhcpRenewalFinished re-arm
            startDhcpRenewal();
            return;
        }
        qDebug() << "[WifiManager] Poll: DHCP renewal gave up after retries";
        m_renewalPending = false;
    }

    // Trigger async check instead of blocking check
    startAsyncConnectionCheck();
}

// ==================== CONNECT STATE MACHINE ====================

void WifiManager::startAddNetwork()
{
    if (m_connectStep != 1) return;
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
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
        << "set_network" << m_pendingNetworkId << "ssid" << ('"' + m_currentSSID + '"'));
    qDebug() << "[WifiManager] startSetSsid -> set_network id ssid";
}

void WifiManager::handleSetSsidFinished()
{
    if (m_connectStep != 2) return;
    m_wpaCLIBuffer.clear();
    m_connectStep = 3; // psk_or_key_mgmt
    startSetPskOrKeyMgmt();
}

void WifiManager::startSetPskOrKeyMgmt()
{
    if (m_connectStep != 3) return;
    
    if (m_pendingPassword.isEmpty()) {
        // Open network
        m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
            << "set_network" << m_pendingNetworkId << "key_mgmt" << "NONE");
        qDebug() << "[WifiManager] startSetPskOrKeyMgmt -> set_network key_mgmt NONE";
    } else if (isHexPskString(m_pendingPassword)) {
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
    m_wpaCLIBuffer.clear();
    m_connectStep = 4; // save_config
    startSaveConfig();
}

void WifiManager::startSaveConfig()
{
    if (m_connectStep != 4) return;
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0" << "save_config");
    qDebug() << "[WifiManager] startSaveConfig -> save_config";
}

void WifiManager::handleSaveConfigFinished()
{
    if (m_connectStep != 4) return;
    m_wpaCLIBuffer.clear();
    m_connectStep = 5; // select_network
    startSelectNetwork();
}

void WifiManager::startSelectNetwork()
{
    if (m_connectStep != 5) return;
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0"
        << "select_network" << m_pendingNetworkId);
    qDebug() << "[WifiManager] startSelectNetwork -> select_network";
}

void WifiManager::handleSelectNetworkFinished()
{
    if (m_connectStep != 5) return;
    m_wpaCLIBuffer.clear();
    m_connectStep = 0; // done
    m_connectTimeoutTimer.start(12000);
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

void WifiManager::handleDhcpRenewalFinished(int exitCode)
{
    if (exitCode == 0) {
        // Fresh lease acquired — safe to check the connection now. The IP
        // on wlan0 is guaranteed to be the NEW AP's, not the old one's.
        qDebug() << "[WifiManager] DHCP renewal OK, checking connection";
        m_renewalPending = false;
        startStatusPolling();
    } else {
        // No lease yet — leave m_renewalPending set so the poll loop retries.
        qDebug() << "[WifiManager] DHCP renewal failed (exit" << exitCode
                 << "), retrying on next poll";
    }
}

void WifiManager::startIpCheck()
{
    // Prevent race: don't start if already running
    if (m_ipCheckProcess->state() != QProcess::NotRunning) {
        qDebug() << "[WifiManager] IP check already running, skipping";
        return;
    }
    m_ipCheckProcess->start("ip", QStringList() << "-4" << "addr" << "show" << "wlan0");
}

void WifiManager::startSsidCheck()
{
    // Prevent race: don't start if already running
    if (m_ssidCheckProcess->state() != QProcess::NotRunning) {
        qDebug() << "[WifiManager] SSID check already running, skipping";
        return;
    }
    m_ssidCheckProcess->start("wpa_cli", QStringList() << "-i" << "wlan0" << "status");
}

void WifiManager::finalizeConnectionCheck(const QString &ipAddress, const QString &ssid)
{
    if (!ipAddress.isEmpty() && !ssid.isEmpty()) {
        // While actively connecting to a target SSID, ignore the old connection
        // until wpa_supplicant actually switches over
        if (m_isConnecting && ssid != m_currentSSID) {
            qDebug() << "[WifiManager] Still on old network:" << ssid << "— waiting for" << m_currentSSID;
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
        if (!wasConnected || oldSSID != ssid || oldIP != ipAddress) {
            qDebug() << "[WifiManager] Connected:" << ssid << "@" << ipAddress;
            emit isConnectedChanged();
            emit connectedInfoChanged();
        }
        emit connectionStateChanged();
        stopStatusPolling();
        // refresh scan results để cập nhật connected flag ngay lập tức (fix B1)
        updateSavedFlags();
        // cập nhật trong list ngay lập tức
        emit scanResultsChanged();

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
            bool wasConnected = m_isConnected;
            m_isConnected = false;
            m_connectedSSID.clear();
            m_connectedIP.clear();
            m_connectionStatus = "Disconnected";
            if (wasConnected) emit isConnectedChanged();
            emit connectionStateChanged();
        }
        // If connecting, keep polling until timeout
    }
}

// ==================== FORGET ====================

void WifiManager::forgetNetwork(const QString &ssid)
{
    m_forgetMode = true;
    m_ssidToForget = ssid;

    // Read current config, filter out the target network using shared helper
    QStringList headerLines;
    QList<ConfigBlock> networkBlocks;
    if (!readWpaSupplicantConfig("/etc/wpa_supplicant.conf", headerLines, networkBlocks, ssid)) {
        qWarning() << "[WifiManager] ERROR: Cannot read wpa_supplicant.conf during forget";
        m_connectionStatus = "Failed: Cannot read config";
        emit connectionStateChanged();
        return;
    }

    // Write back config without the forgotten network using shared helper
    if (!writeWpaSupplicantConfig("/etc/wpa_supplicant.conf", headerLines, networkBlocks)) {
        qWarning() << "[WifiManager] ERROR: Cannot write wpa_supplicant.conf during forget";
        m_connectionStatus = "Failed: Cannot write config";
        emit connectionStateChanged();
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

        // Actually disconnect the WiFi interface (not just local state)
        QProcess discProc;
        discProc.start("wpa_cli", QStringList() << "-i" << "wlan0" << "disconnect");
        discProc.waitForFinished(3000);
    }

    // Reconfigure wpa_supplicant with the updated config
    m_wpaCLIProcess->start("wpa_cli", QStringList() << "-i" << "wlan0" << "reconfigure");

    qDebug() << "[WifiManager] Forgot network:" << ssid;

    // Refresh saved-network list and UI flags
    loadSavedNetworks();
    updateSavedFlags();
    emit scanResultsChanged(); // refresh list view ngay lập tức (fix B2)
}

// ==================== SAVED NETWORKS ====================

void WifiManager::loadSavedNetworks()
{
    m_savedSSIDs.clear();
    QFile confFile("/etc/wpa_supplicant.conf");
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

QString WifiManager::readSavedPassword(const QString &ssid) const
{
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
    qDebug() << "[WifiManager] Connecting to saved network:" << ssid;
    connectToNetwork(ssid, password);
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
            if (!m_isScanning) {
                scan(); // Scan even when connected to refresh signal strength
            }
        });
    }
    m_autoScanTimer->start(10000);
    // Initial scan
    scan();
}

void WifiManager::stopAutoScan()
{
    if (m_autoScanTimer) {
        m_autoScanTimer->stop();
    }
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
        m_connectionStatus = "Connection timeout";
        m_isConnecting = false;
        m_isConnected = false;          // Reset connected flag
        m_connectedSSID.clear();
        m_connectedIP.clear();
        emit connectionStateChanged();
        emit isConnectedChanged();
        emit connectedInfoChanged();
        stopStatusPolling(); // also stop polling
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

    qDebug() << "[WifiManager] Atomic write ok:" << path;
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

// Update detailed connection status from wpa_state
void WifiManager::updateDetailedStatus(const QString &wpaState)
{
    if (wpaState.isEmpty()) {
        m_lastDetailedStatus = "Unknown";
        emit detailedStatusChanged();
        return;
    }

    // Map wpa_supplicant states to user-friendly descriptions
    QString friendlyState;
    if (wpaState == "SCANNING") {
        friendlyState = "Scanning for networks...";
    } else if (wpaState == "DISCONNECTED") {
        friendlyState = "Disconnected";
    } else if (wpaState == "INACTIVE") {
        friendlyState = "Inactive";
    } else if (wpaState == "INTERFACE_DISABLED") {
        friendlyState = "Interface disabled";
    } else if (wpaState == "ASSOCIATING") {
        friendlyState = "Associating with access point...";
    } else if (wpaState == "ASSOCIATED") {
        friendlyState = "Associated, starting authentication...";
    } else if (wpaState == "4WAY_HANDSHAKE") {
        friendlyState = "Authenticating (4-way handshake)...";
    } else if (wpaState == "GROUP_HANDSHAKE") {
        friendlyState = "Completing group handshake...";
    } else if (wpaState == "COMPLETED") {
        friendlyState = "Connected";
    } else if (wpaState == "DORMANT") {
        friendlyState = "Dormant (waiting for AP)";
    } else {
        friendlyState = wpaState; // Fallback: show raw state
    }

    if (m_lastDetailedStatus != friendlyState) {
        m_lastDetailedStatus = friendlyState;
        emit detailedStatusChanged();
    }
}