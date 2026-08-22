import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

ApplicationWindow {
    id: app
    width: 800; height: 480; visible: true
    color: theme.bg

    Theme { id: theme }

    function uiLog(msg) {
        var t = new Date().toISOString().substr(11, 12)
        console.log("[UI][" + t + "] " + msg)
    }

    // Signal helpers — thresholds match the bar-canvas coloring
    function sigLevel(s) { return s >= -50 ? 4 : s >= -60 ? 3 : s >= -70 ? 2 : s >= -80 ? 1 : 0 }
    function sigColor(lv) {
        // Keep text color consistent with the bars on the same card:
        // lvl 1 = Weak (red), 2 = Fair (amber), 3 = Good (amber), 4 = Excellent (green)
        if (lv >= 4) return theme.green
        if (lv === 3) return theme.amber
        if (lv === 2) return theme.amber
        if (lv === 1) return theme.red
        return theme.textDim
    }
    function qualLabel(lv) {
        return lv >= 4 ? "Excellent" : lv === 3 ? "Good" : lv === 2 ? "Fair" : lv === 1 ? "Weak" : "No signal"
    }

    /* ───────────────── HEADER ───────────────── */
    Rectangle {
        id: header
        anchors.top: parent.top; width: parent.width; height: 64
        color: theme.overlay; z: 20

        // slim accent signature line
        Rectangle {
            anchors.top: parent.top; width: parent.width; height: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: theme.accent }
                GradientStop { position: 0.35; color: theme.accent }
                GradientStop { position: 1.0; color: "transparent" }
            }
            opacity: 0.85
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.border }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18; anchors.rightMargin: 16
            spacing: 12

            // Brand mark — wifi glyph tile
            Rectangle {
                Layout.preferredWidth: 42; Layout.preferredHeight: 42; radius: theme.rMd
                color: wifiManager.isConnected ? theme.greenSoft : theme.accentSoft
                border.color: wifiManager.isConnected ? theme.greenBorder : theme.borderAccent; border.width: 1
                Canvas {
                    anchors.fill: parent
                    property bool connected: wifiManager.isConnected
                    readonly property color glyph: connected ? theme.green : theme.accent
                    onConnectedChanged: requestPaint()
                    onGlyphChanged: requestPaint()
                    Component.onCompleted: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                        var cx = width / 2, cy = height * 0.58
                        ctx.strokeStyle = glyph; ctx.lineWidth = 2.2; ctx.lineCap = "round"
                        ctx.beginPath(); ctx.arc(cx, cy, 12, Math.PI * 1.18, Math.PI * 1.82); ctx.stroke()
                        ctx.beginPath(); ctx.arc(cx, cy, 8,  Math.PI * 1.18, Math.PI * 1.82); ctx.stroke()
                        ctx.beginPath(); ctx.arc(cx, cy, 4.2, Math.PI * 1.18, Math.PI * 1.82); ctx.stroke()
                        ctx.fillStyle = glyph; ctx.beginPath(); ctx.arc(cx, cy, 2.0, 0, Math.PI * 2); ctx.fill()
                    }
                }
            }

            // Title + live status line
            ColumnLayout { spacing: 2; Layout.fillWidth: true
                Text { text: "WiFi Manager"; color: theme.text1; font.pixelSize: 17; font.bold: true; font.family: theme.fontFamily }
                RowLayout { spacing: 6; Layout.fillWidth: true
                    Rectangle {
                        width: 7; height: 7; radius: 3.5
                        color: wifiManager.isConnected ? theme.green : (wifiManager.isScanning ? theme.accent : theme.textDim)
                        SequentialAnimation on opacity {
                            running: wifiManager.isScanning && !wifiManager.isConnected; loops: Animation.Infinite
                            NumberAnimation { from: 1; to: 0.35; duration: 500 }
                            NumberAnimation { from: 0.35; to: 1; duration: 500 }
                        }
                    }
                    Text { visible: wifiManager.isConnected
                        text: wifiManager.connectedSSID + " · " + wifiManager.connectedIP
                        color: theme.green; font.pixelSize: 12; font.bold: true
                        font.family: theme.fontFamily; elide: Text.ElideRight }
                    Text { visible: !wifiManager.isConnected; Layout.fillWidth: true
                        text: wifiManager.isScanning ? "Scanning nearby networks…" : "Not connected — tap Scan"
                        color: wifiManager.isScanning ? theme.accentHi : theme.text3
                        font.pixelSize: 12; font.family: theme.fontFamily; elide: Text.ElideRight }
                }
            }

            // Network count chip
            Rectangle {
                visible: !wifiManager.isConnected && wifiManager.scanResults.length > 0
                implicitHeight: 26; implicitWidth: cntTxt.width + 18; radius: theme.rPill
                color: theme.surface2; border.color: theme.border; border.width: 1
                Text { id: cntTxt; anchors.centerIn: parent
                    text: wifiManager.scanResults.length + " found"
                    color: theme.text2; font.pixelSize: 11; font.bold: true; font.family: theme.fontFamily }
            }

            // Primary action — Scan
            Rectangle {
                id: scanBtn
                Layout.preferredWidth: 118; Layout.preferredHeight: 40; radius: theme.rPill
                Layout.alignment: Qt.AlignVCenter
                color: scanMa.pressed ? theme.accentDk : theme.accent
                opacity: wifiManager.isScanning ? 0.92 : 1
                Behavior on opacity { NumberAnimation { duration: 150 } }

                RowLayout { anchors.centerIn: parent; spacing: 7
                    Canvas {
                        id: spinnerCv
                        width: 16; height: 16; visible: wifiManager.isScanning
                        property real angle: 0
                        onPaint: {
                            var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                            ctx.translate(width / 2, height / 2); ctx.rotate(angle * Math.PI / 180)
                            ctx.strokeStyle = theme.ink; ctx.lineWidth = 2.3; ctx.lineCap = "round"
                            ctx.beginPath(); ctx.arc(0, 0, 5.2, 0, Math.PI * 1.45); ctx.stroke()
                            ctx.beginPath(); ctx.moveTo(3.5, -3.5); ctx.lineTo(5.2, -2); ctx.lineTo(4, 0); ctx.stroke()
                        }
                        RotationAnimation on angle { id: scanAnim; from: 0; to: 360; duration: 700; loops: Animation.Infinite; running: false }
                        Connections { target: wifiManager; function onIsScanningChanged() { scanAnim.running = wifiManager.isScanning; if (scanAnim.running) spinnerCv.requestPaint() } }
                        onAngleChanged: requestPaint()
                    }
                    Text { text: wifiManager.isScanning ? "Scanning" : "Scan"; color: theme.ink; font.pixelSize: 14; font.bold: true; font.family: theme.fontFamily }
                }
                MouseArea { id: scanMa; anchors.fill: parent
                    onClicked: {
                        if (wifiManager.isScanning) app.uiLog("Scan skipped — busy")
                        else { app.uiLog("Tap Scan"); wifiManager.scan(); scanPulse.start() }
                    }
                }
                SequentialAnimation { id: scanPulse
                    NumberAnimation { target: scanBtn; property: "scale"; from: 1; to: 1.05; duration: 110; easing.type: Easing.OutQuad }
                    NumberAnimation { target: scanBtn; property: "scale"; from: 1.05; to: 1; duration: 130; easing.type: Easing.OutQuad }
                }
            }

            // Theme toggle — canvas sun/moon (emoji-free, DejaVu-safe) — last item, flush right at end of header
            Rectangle {
                Layout.preferredWidth: 40; Layout.preferredHeight: 40; radius: theme.rPill
                Layout.alignment: Qt.AlignVCenter
                color: themeMa.pressed ? theme.surface3 : theme.surface2
                border.color: theme.border; border.width: 1
                Canvas {
                    anchors.centerIn: parent; width: 20; height: 20
                    property bool isDark: theme.dark
                    onIsDarkChanged: requestPaint()
                    Component.onCompleted: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                        var cx = width / 2, cy = height / 2
                        ctx.fillStyle = theme.text2; ctx.strokeStyle = theme.text2
                        if (isDark) {
                            // crescent moon
                            ctx.beginPath(); ctx.arc(cx, cy, 7, 0, Math.PI * 2); ctx.fill()
                            ctx.globalCompositeOperation = "destination-out"
                            ctx.beginPath(); ctx.arc(cx + 3.4, cy - 2.4, 6.2, 0, Math.PI * 2); ctx.fill()
                            ctx.globalCompositeOperation = "source-over"
                        } else {
                            // sun: core + 8 rays
                            ctx.beginPath(); ctx.arc(cx, cy, 3.6, 0, Math.PI * 2); ctx.fill()
                            ctx.lineWidth = 1.6; ctx.lineCap = "round"
                            for (var i = 0; i < 8; i++) {
                                var a = i * Math.PI / 4
                                ctx.beginPath()
                                ctx.moveTo(cx + Math.cos(a) * 5.6, cy + Math.sin(a) * 5.6)
                                ctx.lineTo(cx + Math.cos(a) * 7.6, cy + Math.sin(a) * 7.6)
                                ctx.stroke()
                            }
                        }
                    }
                }
                MouseArea { id: themeMa; anchors.fill: parent
                    onClicked: { theme.dark = !theme.dark; app.uiLog("Theme → " + (theme.dark ? "Dark" : "Light")) } }
            }
        }
    }

    /* ───────────────── ERROR TOAST ───────────────── */
    Item {
        id: errorBar
        anchors.top: header.bottom; width: parent.width
        height: visible ? 42 : 0; visible: false; z: 15; clip: true
        Behavior on height { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        Rectangle {
            anchors.fill: parent; color: theme.redSoft
            Rectangle { width: 3; height: parent.height; color: theme.red }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.red; opacity: 0.35 }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 18; anchors.rightMargin: 16; spacing: 10
                Rectangle { width: 22; height: 22; radius: theme.rPill; color: theme.red
                    Text { anchors.centerIn: parent; text: "!"; color: theme.ink; font.pixelSize: 13; font.bold: true } }
                Text {
                    Layout.fillWidth: true; maximumLineCount: 1; elide: Text.ElideRight
                    text: wifiManager.lastError.length > 0 ? wifiManager.lastError
                        : wifiManager.connectionStatus === "Wrong password" ? "Wrong password"
                        : wifiManager.connectionStatus === "Invalid SSID" ? "Invalid network name"
                        : wifiManager.connectionStatus === "Connection failed" ? "Connection failed"
                        : wifiManager.connectionStatus === "Connection timeout" ? "Connection timed out"
                        : wifiManager.connectionStatus === "Scan failed" ? "Scan failed" : "Error"
                    color: theme.redText; font.pixelSize: 13; font.bold: true; font.family: theme.fontFamily
                }
                Text { text: "✕"; color: theme.redTextHi; font.pixelSize: 14
                    MouseArea { anchors.fill: parent; anchors.margins: -10; onClicked: errorBar.visible = false } }
            }
        }
        function show() { visible = true; hideTimer.restart() }
        Timer { id: hideTimer; interval: 4600; onTriggered: errorBar.visible = false }
    }
    Connections {
        target: wifiManager
        function onLastErrorChanged() { if (wifiManager.lastError.length > 0) errorBar.show() }
        function onConnectionStateChanged() {
            if (wifiManager.lastError.length > 0) errorBar.show()
            else if (["Wrong password","Invalid SSID","Connection failed","Connection timeout","Scan failed"].indexOf(wifiManager.connectionStatus) >= 0) errorBar.show()
        }
    }

    /* ───────────────── SCANNING STRIP ───────────────── */
    Item {
        id: scanInd
        anchors.top: errorBar.bottom; width: parent.width
        height: wifiManager.isScanning ? 36 : 0; visible: height > 0; clip: true; z: 14
        Behavior on height { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Rectangle {
            anchors.fill: parent; color: theme.bgSubtle
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: theme.border }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 20; anchors.rightMargin: 16; spacing: 8
                Repeater { model: 3
                    Rectangle { width: 6; height: 6; radius: 3; color: theme.accent
                        SequentialAnimation on opacity { loops: Animation.Infinite
                            PauseAnimation { duration: index * 200 }
                            NumberAnimation { from: 0.3; to: 1; duration: 420; easing.type: Easing.InOutQuad }
                            NumberAnimation { from: 1; to: 0.3; duration: 420; easing.type: Easing.InOutQuad }
                            PauseAnimation { duration: (2 - index) * 200 }
                        }
                    }
                }
                Text { text: "Scanning for networks…"; color: theme.accentHi; font.pixelSize: 12; font.bold: true; font.family: theme.fontFamily }
                Item { Layout.fillWidth: true }
                Text { text: "wlan0 · 2.4 GHz"; color: theme.text3; font.pixelSize: 11; font.family: theme.fontMono; visible: parent.width > 500 }
            }
        }
    }

    /* ───────────────── NETWORK LIST ───────────────── */
    ListView {
        id: wifiList
        anchors.top: scanInd.bottom; anchors.bottom: footer.top
        width: parent.width; spacing: 10; clip: true; model: wifiManager.scanResults
        boundsBehavior: Flickable.StopAtBounds; flickDeceleration: 1800
        leftMargin: 14; rightMargin: 14; topMargin: 10; bottomMargin: 10
        add: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 220 }
            NumberAnimation { property: "y"; from: 10; to: 0; duration: 220; easing.type: Easing.OutCubic }
        }
        displaced: Transition { NumberAnimation { properties: "x,y"; duration: 220; easing.type: Easing.OutCubic } }

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle { implicitWidth: 4; radius: 2; color: theme.accent; opacity: 0.5 }
        }

        delegate: Rectangle {
            id: card
            width: wifiList.width - 28; height: 74; radius: theme.rMd
            color: modelData.connected ? theme.greenSoft : (cardMa.pressed ? theme.surface2 : theme.surface)
            border.color: modelData.connected ? theme.greenBorder : (cardMa.containsMouse ? theme.borderHi : theme.border)
            border.width: 1
            Behavior on border.color { ColorAnimation { duration: 120 } }

            // connected edge marker
            Rectangle { visible: modelData.connected; x: 0; y: 10; width: 3; height: parent.height - 20; radius: 1.5; color: theme.green }
            // hairline sheen for depth (no shadows on software renderer)
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: theme.ink; opacity: modelData.connected ? 0.07 : 0.04 }

            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 12; spacing: 12

                // signal tile
                Rectangle {
                    Layout.preferredWidth: 44; Layout.preferredHeight: 44; radius: theme.rSm
                    color: modelData.connected ? theme.greenSoft : theme.surface2
                    border.color: modelData.connected ? theme.greenBorder : theme.border; border.width: 1
                    Canvas {
                        anchors.fill: parent; anchors.margins: 6
                        property int lvl: modelData.signalLevel
                        property bool themeTick: theme.dark
                        onLvlChanged: requestPaint()
                        onThemeTickChanged: requestPaint()
                        Component.onCompleted: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                            var cols = [theme.red, theme.amber, theme.amber, theme.green]
                            var bc = lvl > 0 ? cols[lvl - 1] : theme.border
                            for (var i = 0; i < 4; i++) {
                                var h = 6 + i * 4.5
                                ctx.fillStyle = (i < lvl) ? bc : theme.border
                                ctx.globalAlpha = (i < lvl) ? 1 : 0.38
                                var x = i * 7.2, y = height - h, w = 5, r = 1.4
                                ctx.beginPath()
                                ctx.moveTo(x + r, y); ctx.lineTo(x + w - r, y); ctx.quadraticCurveTo(x + w, y, x + w, y + r)
                                ctx.lineTo(x + w, y + h - r); ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h)
                                ctx.lineTo(x + r, y + h); ctx.quadraticCurveTo(x, y + h, x, y + h - r)
                                ctx.lineTo(x, y + r); ctx.quadraticCurveTo(x, y, x + r, y); ctx.closePath(); ctx.fill()
                            }
                            ctx.globalAlpha = 1
                        }
                    }
                    // saved marker dot
                    Rectangle {
                        anchors.top: parent.top; anchors.right: parent.right; anchors.margins: -3
                        width: 10; height: 10; radius: 5; color: theme.accent
                        border.color: modelData.connected ? theme.greenSoft : theme.surface; border.width: 2
                        visible: modelData.saved
                    }
                }

                // identity + meta pills
                ColumnLayout {
                    spacing: 4; Layout.fillWidth: true; Layout.maximumWidth: 430
                    Text {
                        text: modelData.ssid
                        color: modelData.connected ? theme.green : theme.text1
                        font.pixelSize: 15; font.bold: true; font.family: theme.fontFamily
                        elide: Text.ElideRight; Layout.fillWidth: true
                    }
                    RowLayout { spacing: 6
                        Rectangle {
                            radius: theme.rPill; implicitWidth: secTxt.width + 16; implicitHeight: 20
                            color: modelData.secured ? theme.amberSoft : theme.bgSubtle
                            border.color: modelData.secured ? theme.amberBorder : theme.border; border.width: 1
                            Text { id: secTxt; anchors.centerIn: parent
                                text: modelData.secured ? "WPA · Secured" : "Open"
                                color: modelData.secured ? theme.amber : theme.text3
                                font.pixelSize: 11; font.bold: true; font.family: theme.fontFamily }
                        }
                        Text { text: modelData.signal + " dBm"; color: sigColor(modelData.signalLevel)
                            font.pixelSize: 11; font.bold: true; font.family: theme.fontMono }
                        Rectangle {
                            radius: theme.rPill; implicitWidth: svTxt.width + 16; implicitHeight: 20
                            visible: modelData.saved && !modelData.connected
                            color: theme.accentSoft; border.color: theme.borderAccent; border.width: 1
                            Text { id: svTxt; anchors.centerIn: parent; text: "Saved"
                                color: theme.accentHi; font.pixelSize: 11; font.bold: true; font.family: theme.fontFamily }
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                // right rail — fixed slots keep columns aligned across cards
                RowLayout { spacing: 8
                    ColumnLayout { spacing: 1; visible: !modelData.connected
                        Text { text: qualLabel(modelData.signalLevel); color: sigColor(modelData.signalLevel)
                            font.pixelSize: 11; font.bold: true; font.family: theme.fontFamily
                            horizontalAlignment: Text.AlignRight; Layout.fillWidth: true }
                    }
                    // forget slot — reserved width so the rail never shifts
                    Item { Layout.preferredWidth: 66; Layout.preferredHeight: 32
                        Rectangle {
                            anchors.fill: parent; radius: theme.rSm
                            visible: modelData.saved && !modelData.connected
                            color: forgetMa.pressed ? theme.redSoft : "transparent"
                            border.color: theme.red; border.width: 1
                            Text { anchors.centerIn: parent; text: "Forget"; color: theme.red
                                font.pixelSize: 11; font.bold: true; font.family: theme.fontFamily }
                            MouseArea { id: forgetMa; anchors.fill: parent
                                onClicked: { app.uiLog("Forget " + modelData.ssid); wifiManager.forgetNetwork(modelData.ssid) } }
                        }
                    }
                    Rectangle {
                        Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: theme.rPill
                        color: modelData.connected ? theme.green : (cardMa.pressed ? theme.surface3 : theme.surface2)
                        border.color: modelData.connected ? theme.green : theme.border; border.width: 1
                        Text { anchors.centerIn: parent
                            text: modelData.connected ? "✓" : "›"
                            color: modelData.connected ? theme.ink : theme.text2
                            font.pixelSize: modelData.connected ? 13 : 16; font.bold: true }
                    }
                }
            }

            MouseArea { id: cardMa; anchors.fill: parent; hoverEnabled: true
                onClicked: {
                    if (modelData.connected) app.uiLog("Tap " + modelData.ssid + " — already connected")
                    else if (modelData.saved) { app.uiLog("Tap " + modelData.ssid + " — connect saved"); wifiManager.connectSaved(modelData.ssid) }
                    else if (modelData.secured) { app.uiLog("Tap " + modelData.ssid + " — password dialog"); passwordDialog.show(modelData.ssid, modelData.signal) }
                    else { app.uiLog("Tap " + modelData.ssid + " — open connect"); wifiManager.connectToNetwork(modelData.ssid, "") }
                }
            }
        }

        // Empty state
        Item {
            anchors.fill: parent
            visible: wifiManager.scanResults.length === 0 && !wifiManager.isScanning
            ColumnLayout {
                anchors.centerIn: parent; spacing: 10; width: parent.width - 80
                Canvas {
                    Layout.alignment: Qt.AlignHCenter; width: 84; height: 84
                    property bool themeTick: theme.dark
                    onThemeTickChanged: requestPaint()
                    Component.onCompleted: requestPaint()
                    onPaint: {
                        var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                        var cx = width / 2, cy = height * 0.56
                        ctx.strokeStyle = theme.borderHi; ctx.lineWidth = 2.6; ctx.lineCap = "round"
                        ctx.globalAlpha = 0.9
                        ctx.beginPath(); ctx.arc(cx, cy, 26, Math.PI * 1.12, Math.PI * 1.88); ctx.stroke()
                        ctx.globalAlpha = 0.6
                        ctx.beginPath(); ctx.arc(cx, cy, 18, Math.PI * 1.12, Math.PI * 1.88); ctx.stroke()
                        ctx.globalAlpha = 0.35
                        ctx.beginPath(); ctx.arc(cx, cy, 9.5, Math.PI * 1.12, Math.PI * 1.88); ctx.stroke()
                        ctx.globalAlpha = 1
                        ctx.fillStyle = theme.text3; ctx.beginPath(); ctx.arc(cx, cy, 3.2, 0, Math.PI * 2); ctx.fill()
                        ctx.fillStyle = theme.border; ctx.beginPath(); ctx.arc(cx, cy, 1.4, 0, Math.PI * 2); ctx.fill()
                    }
                }
                Text { Layout.alignment: Qt.AlignHCenter; text: "No networks nearby"; color: theme.text1; font.pixelSize: 16; font.bold: true; font.family: theme.fontFamily }
                Text { Layout.alignment: Qt.AlignHCenter; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; Layout.fillWidth: true
                    text: "Make sure WiFi is enabled, then tap Scan to discover available networks."
                    color: theme.text3; font.pixelSize: 12; font.family: theme.fontFamily }
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter; Layout.topMargin: 8
                    implicitWidth: 148; implicitHeight: 40; radius: theme.rPill
                    color: esMa.pressed ? theme.accentDk : theme.accent
                    Text { anchors.centerIn: parent; text: "Scan now"; color: theme.ink; font.pixelSize: 13; font.bold: true; font.family: theme.fontFamily }
                    MouseArea { id: esMa; anchors.fill: parent; onClicked: wifiManager.scan() }
                }
            }
        }
    }

    /* ───────────────── FOOTER ───────────────── */
    Rectangle {
        id: footer
        anchors.bottom: parent.bottom; width: parent.width; height: 54
        color: theme.overlay; z: 18
        Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: theme.border }

        RowLayout {
            anchors.fill: parent; anchors.leftMargin: 14; anchors.rightMargin: 14; spacing: 10

            Rectangle {
                implicitWidth: footTxt.width + 18; implicitHeight: 26; radius: theme.rPill
                color: theme.surface2; border.color: theme.border; border.width: 1
                visible: !wifiManager.isConnected
                Text { id: footTxt; anchors.centerIn: parent
                    text: wifiManager.scanResults.length + " network" + (wifiManager.scanResults.length === 1 ? "" : "s")
                    color: theme.text3; font.pixelSize: 12; font.family: theme.fontFamily }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: 100; Layout.preferredHeight: 36; radius: theme.rSm
                color: footScanMa.pressed ? theme.surface3 : theme.surface2
                border.color: theme.border; border.width: 1
                visible: !wifiManager.isConnected
                Text { anchors.centerIn: parent; text: "↻  Rescan"; color: theme.text2; font.pixelSize: 12; font.bold: true; font.family: theme.fontFamily }
                MouseArea { id: footScanMa; anchors.fill: parent; onClicked: wifiManager.scan() }
            }

            Rectangle {
                Layout.preferredWidth: 136; Layout.preferredHeight: 36; radius: theme.rSm
                color: discMa.pressed ? theme.redSoft : "transparent"
                border.color: theme.red; border.width: 1
                visible: wifiManager.isConnected
                RowLayout { anchors.centerIn: parent; spacing: 7
                    Canvas {
                        width: 13; height: 13
                        property bool themeTick: theme.dark
                        onThemeTickChanged: requestPaint()
                        Component.onCompleted: requestPaint()
                        onPaint: {
                            var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                            var cx = width / 2, cy = height / 2 + 1
                            ctx.strokeStyle = theme.red; ctx.lineWidth = 1.7; ctx.lineCap = "round"
                            ctx.beginPath(); ctx.moveTo(cx, 0.5); ctx.lineTo(cx, 5.5); ctx.stroke()
                            ctx.beginPath(); ctx.arc(cx, cy, 4.6, -Math.PI / 3, Math.PI + Math.PI / 3); ctx.stroke()
                        }
                    }
                    Text { text: "Disconnect"; color: theme.text1; font.pixelSize: 12; font.bold: true; font.family: theme.fontFamily }
                }
                MouseArea { id: discMa; anchors.fill: parent; onClicked: { app.uiLog("Tap Disconnect footer"); wifiManager.disconnectFromNetwork() } }
            }
        }
    }

    /* ───────────────── PASSWORD SHEET ───────────────── */
    Item {
        id: passwordDialog
        anchors.fill: parent; visible: false; z: 200
        property string ssid: ""
        property string input: ""
        property bool showPwd: false
        property bool shiftActive: false
        property int signal: 0

        function keyPress(k) {
            if (k === "BS") input = input.slice(0, -1)
            else if (k === "CLR") { input = ""; shiftActive = false }
            else if (k === "SHIFT") shiftActive = !shiftActive
            else if (input.length < 64) {
                if (shiftActive) { input += (k.length === 1 && k >= 'a' && k <= 'z') ? k.toUpperCase() : k; shiftActive = false }
                else input += k
            }
        }
        function show(s, sig) { ssid = s; signal = (sig !== undefined) ? sig : 0; input = ""; showPwd = false; shiftActive = false; visible = true; dlgAnim.restart() }
        function hide() { visible = false; input = ""; showPwd = false; shiftActive = false }

        Rectangle {
            anchors.fill: parent; color: theme.shadow; opacity: 0.62
            MouseArea { anchors.fill: parent; onClicked: passwordDialog.hide() }
        }

        Rectangle {
            id: dlg
            anchors.centerIn: parent; width: 560
            height: contentCol.implicitHeight + 36
            radius: theme.rXl
            color: theme.overlay; border.color: theme.borderHi; border.width: 1
            opacity: 0; scale: 0.96

            ParallelAnimation {
                id: dlgAnim
                NumberAnimation { target: dlg; property: "opacity"; from: 0; to: 1; duration: 220; easing.type: Easing.OutCubic }
                NumberAnimation { target: dlg; property: "scale"; from: 0.96; to: 1; duration: 260; easing.type: Easing.OutCubic }
            }

            // accent signature line
            Rectangle { x: 20; y: 0; width: parent.width - 40; height: 2; radius: 1; color: theme.accent }

            ColumnLayout {
                id: contentCol
                x: 18; y: 18; width: parent.width - 36; spacing: 9

                // header
                RowLayout { spacing: 12; Layout.fillWidth: true
                    Rectangle {
                        Layout.preferredWidth: 38; Layout.preferredHeight: 38; radius: theme.rSm
                        color: theme.surface2; border.color: theme.border; border.width: 1
                        Canvas {
                            anchors.fill: parent; anchors.margins: 9
                            property bool themeTick: theme.dark
                            onThemeTickChanged: requestPaint()
                            Component.onCompleted: requestPaint()
                            onPaint: {
                                var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                                ctx.strokeStyle = theme.accent; ctx.fillStyle = theme.accent
                                ctx.lineWidth = 1.8; ctx.lineCap = "round"
                                // shackle
                                ctx.beginPath(); ctx.arc(width / 2, height * 0.42, width * 0.26, Math.PI, 0); ctx.stroke()
                                // body
                                var bw = width * 0.72, bh = height * 0.46, bx = (width - bw) / 2, by = height * 0.42, r = 2
                                ctx.beginPath()
                                ctx.moveTo(bx + r, by); ctx.lineTo(bx + bw - r, by); ctx.quadraticCurveTo(bx + bw, by, bx + bw, by + r)
                                ctx.lineTo(bx + bw, by + bh - r); ctx.quadraticCurveTo(bx + bw, by + bh, bx + bw - r, by + bh)
                                ctx.lineTo(bx + r, by + bh); ctx.quadraticCurveTo(bx, by + bh, bx, by + bh - r)
                                ctx.lineTo(bx, by + r); ctx.quadraticCurveTo(bx, by, bx + r, by); ctx.closePath(); ctx.fill()
                                // keyhole
                                ctx.fillStyle = theme.overlay
                                ctx.beginPath(); ctx.arc(width / 2, by + bh * 0.45, 1.6, 0, Math.PI * 2); ctx.fill()
                            }
                        }
                    }
                    ColumnLayout { spacing: 2; Layout.fillWidth: true
                        Text { text: "Enter password"; color: theme.text1; font.pixelSize: 17; font.bold: true; font.family: theme.fontFamily }
                        RowLayout { spacing: 6; Layout.fillWidth: true
                            Canvas {
                                width: 20; height: 14
                                property int lvl: sigLevel(passwordDialog.signal)
                                property bool themeTick: theme.dark
                                onLvlChanged: requestPaint()
                                onThemeTickChanged: requestPaint()
                                Component.onCompleted: requestPaint()
                                onPaint: {
                                    var ctx = getContext("2d"); ctx.clearRect(0, 0, width, height)
                                    var cols = [theme.red, theme.amber, theme.amber, theme.green]
                                    var bc = lvl > 0 ? cols[lvl - 1] : theme.border
                                    for (var i = 0; i < 4; i++) {
                                        var h = 3 + i * 2.8
                                        ctx.fillStyle = (i < lvl) ? bc : theme.border
                                        ctx.globalAlpha = (i < lvl) ? 1 : 0.4
                                        ctx.fillRect(i * 5, height - h, 3.5, h)
                                    }
                                    ctx.globalAlpha = 1
                                }
                            }
                            Text { text: passwordDialog.ssid; color: theme.text2; font.pixelSize: 13; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true; font.family: theme.fontFamily }
                            Text { text: passwordDialog.signal + " dBm"; color: theme.text3; font.pixelSize: 11; font.family: theme.fontMono }
                        }
                    }
                    Rectangle {
                        Layout.preferredWidth: 32; Layout.preferredHeight: 32; radius: theme.rSm
                        color: closeMa.pressed ? theme.surface3 : theme.surface2
                        border.color: theme.border; border.width: 1
                        Text { anchors.centerIn: parent; text: "✕"; color: theme.text2; font.pixelSize: 13 }
                        MouseArea { id: closeMa; anchors.fill: parent; onClicked: passwordDialog.hide() }
                    }
                }

                // weak-signal advisory
                Rectangle {
                    radius: theme.rSm; visible: passwordDialog.signal !== 0 && passwordDialog.signal < -75
                    color: theme.redSoft; border.color: theme.redBorder; border.width: 1
                    Layout.fillWidth: true; implicitHeight: 34
                    RowLayout {
                        anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 12; spacing: 8
                        Rectangle { width: 18; height: 18; radius: theme.rPill; color: theme.red
                            Text { anchors.centerIn: parent; text: "!"; color: theme.ink; font.pixelSize: 11; font.bold: true } }
                        Text { Layout.fillWidth: true; text: "Weak signal — connection may be unstable"
                            color: theme.weakText; font.pixelSize: 12; font.family: theme.fontFamily }
                    }
                }

                // password field
                Rectangle {
                    radius: theme.rMd; Layout.fillWidth: true; implicitHeight: 52
                    color: theme.bg
                    border.color: passwordDialog.input.length > 0 ? theme.borderAccent : theme.border
                    border.width: passwordDialog.input.length > 0 ? 1.4 : 1
                    Behavior on border.color { ColorAnimation { duration: 150 } }
                    Text {
                        anchors.centerIn: parent; width: parent.width - 32
                        horizontalAlignment: Text.AlignHCenter; elide: Text.ElideMiddle; maximumLineCount: 1
                        text: passwordDialog.input.length === 0 ? "Enter password  (8–64 characters)"
                             : (passwordDialog.showPwd ? passwordDialog.input : (new Array(passwordDialog.input.length + 1).join("●")))
                        color: passwordDialog.input.length === 0 ? theme.text3 : theme.text1
                        font.pixelSize: passwordDialog.input.length === 0 ? 12 : (passwordDialog.showPwd ? 13 : 16)
                        font.family: passwordDialog.input.length === 0 ? theme.fontFamily : theme.fontMono
                    }
                }

                // show toggle + counter
                RowLayout { spacing: 8; Layout.fillWidth: true
                    Rectangle {
                        width: 20; height: 20; radius: 5
                        color: passwordDialog.showPwd ? theme.accent : "transparent"
                        border.color: passwordDialog.showPwd ? theme.accent : theme.borderHi; border.width: 1
                        Text { anchors.centerIn: parent; text: passwordDialog.showPwd ? "✓" : ""; color: theme.ink; font.pixelSize: 11; font.bold: true }
                        MouseArea { anchors.fill: parent; anchors.margins: -8; onClicked: passwordDialog.showPwd = !passwordDialog.showPwd }
                    }
                    Text { text: "Show password"; color: theme.text2; font.pixelSize: 12; font.family: theme.fontFamily }
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        radius: theme.rPill; implicitWidth: cntTxt2.width + 16; implicitHeight: 22
                        color: passwordDialog.input.length >= 8 ? theme.greenSoft : theme.amberSoft
                        border.color: passwordDialog.input.length >= 8 ? theme.greenBorder : theme.amberBorder; border.width: 1
                        Text { id: cntTxt2; anchors.centerIn: parent
                            text: passwordDialog.input.length + " / 64"
                            color: passwordDialog.input.length >= 8 ? theme.green : theme.amber
                            font.pixelSize: 11; font.bold: true; font.family: theme.fontMono }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: theme.border }

                // keyboard — 42x40 keys, 11 columns
                Grid {
                    Layout.alignment: Qt.AlignHCenter
                    columns: 11; columnSpacing: 5; rowSpacing: 5

                    Repeater { model: ["1","2","3","4","5","6","7","8","9","0","⌫"]
                        delegate: Rectangle {
                            property string txt: modelData
                            property bool isBksp: modelData === "⌫"
                            width: 42; height: 40; radius: theme.rSm
                            color: kbMa1.pressed ? theme.surface : (isBksp ? theme.surface3 : theme.surface2)
                            border.color: theme.border; border.width: 1
                            Text { anchors.centerIn: parent; text: txt; color: theme.text1
                                font.pixelSize: txt.length > 1 ? 15 : 14; font.bold: true; font.family: theme.fontFamily }
                            MouseArea { id: kbMa1; anchors.fill: parent
                                onClicked: passwordDialog.keyPress(isBksp ? "BS" : txt) }
                        }
                    }
                    Repeater { model: ["q","w","e","r","t","y","u","i","o","p","-"]
                        delegate: Rectangle {
                            property string txt: modelData
                            width: 42; height: 40; radius: theme.rSm
                            color: kbMa2.pressed ? theme.surface3 : theme.surface2
                            border.color: theme.border; border.width: 1
                            Text { anchors.centerIn: parent; text: txt; color: theme.text1; font.pixelSize: 14; font.bold: true; font.family: theme.fontFamily }
                            MouseArea { id: kbMa2; anchors.fill: parent; onClicked: passwordDialog.keyPress(txt) }
                        }
                    }
                    Repeater { model: ["a","s","d","f","g","h","j","k","l","@","⇧"]
                        delegate: Rectangle {
                            property string txt: modelData
                            property bool isShift: modelData === "⇧"
                            width: 42; height: 40; radius: theme.rSm
                            color: isShift && passwordDialog.shiftActive ? theme.accent : (kbMa3.pressed ? theme.surface3 : theme.surface2)
                            border.color: isShift && passwordDialog.shiftActive ? theme.accent : theme.border; border.width: 1
                            Text { anchors.centerIn: parent; text: txt
                                color: isShift && passwordDialog.shiftActive ? theme.ink : theme.text1
                                font.pixelSize: 14; font.bold: true; font.family: theme.fontFamily }
                            MouseArea { id: kbMa3; anchors.fill: parent
                                onClicked: passwordDialog.keyPress(isShift ? "SHIFT" : txt) }
                        }
                    }
                    Repeater { model: ["z","x","c","v","b","n","m",".","_","!","?"]
                        delegate: Rectangle {
                            property string txt: modelData
                            width: 42; height: 40; radius: theme.rSm
                            color: kbMa4.pressed ? theme.surface3 : theme.surface2
                            border.color: theme.border; border.width: 1
                            Text { anchors.centerIn: parent; text: txt; color: theme.text1; font.pixelSize: 14; font.bold: true; font.family: theme.fontFamily }
                            MouseArea { id: kbMa4; anchors.fill: parent; onClicked: passwordDialog.keyPress(txt) }
                        }
                    }
                }

                // actions
                RowLayout { Layout.alignment: Qt.AlignHCenter; spacing: 8
                    Rectangle {
                        width: 92; height: 40; radius: theme.rSm
                        color: clrMa.pressed ? theme.surface3 : "transparent"
                        border.color: theme.border; border.width: 1
                        Text { anchors.centerIn: parent; text: "CLEAR"; color: theme.text2; font.pixelSize: 12; font.bold: true; font.family: theme.fontFamily }
                        MouseArea { id: clrMa; anchors.fill: parent; onClicked: passwordDialog.keyPress("CLR") }
                    }
                    Rectangle {
                        width: 68; height: 40; radius: theme.rSm
                        color: spMa.pressed ? theme.surface3 : "transparent"
                        border.color: theme.border; border.width: 1
                        Text { anchors.centerIn: parent; text: "Space"; color: theme.text2; font.pixelSize: 12; font.bold: true; font.family: theme.fontFamily }
                        MouseArea { id: spMa; anchors.fill: parent; onClicked: passwordDialog.keyPress(" ") }
                    }
                    Rectangle {
                        width: 138; height: 40; radius: theme.rSm
                        color: passwordDialog.input.length >= 8 ? (okMa.pressed ? theme.accentDk : theme.accent) : theme.surface2
                        border.color: passwordDialog.input.length >= 8 ? theme.accent : theme.border; border.width: 1
                        Text { anchors.centerIn: parent; text: "CONNECT"
                            color: passwordDialog.input.length >= 8 ? theme.ink : theme.textDim
                            font.pixelSize: 12; font.bold: true; font.family: theme.fontFamily }
                        MouseArea { id: okMa; anchors.fill: parent; enabled: passwordDialog.input.length >= 8
                            onClicked: { wifiManager.connectToNetwork(passwordDialog.ssid, passwordDialog.input); passwordDialog.hide() } }
                    }
                    Rectangle {
                        width: 92; height: 40; radius: theme.rSm
                        color: cxMa.pressed ? theme.redSoft : "transparent"
                        border.color: theme.red; border.width: 1
                        Text { anchors.centerIn: parent; text: "CANCEL"; color: theme.red; font.pixelSize: 12; font.bold: true; font.family: theme.fontFamily }
                        MouseArea { id: cxMa; anchors.fill: parent; onClicked: passwordDialog.hide() }
                    }
                }
            }
        }
    }

    Component.onCompleted: wifiManager.scan()
}
