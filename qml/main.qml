import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

ApplicationWindow {
    id: app
    width: 800; height: 480; visible: true
    color: "#1a1a2e"

    /* ===== TOP BAR ===== */
    Rectangle {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left; anchors.right: parent.right
        height: 70; color: "#0f3460"; z: 10

        ColumnLayout {
            anchors.left: parent.left; anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter; spacing: 2
            Text {
                text: "WiFi Manager"
                color: "#00d2ff"; font.pixelSize: 20; font.bold: true
            }
            Text {
                text: wifiManager.connectionStatus === "Connected"
                    ? wifiManager.connectedSSID + " — " + wifiManager.connectedIP
                    : wifiManager.connectionStatus
                color: wifiManager.connectionStatus === "Connected" ? "#00e676" : "#e0e0e0"
                font.pixelSize: 13
            }
        }

        /* ===== SCAN BUTTON WITH SPINNER ===== */
        Rectangle {
            id: scanBtn
            anchors.right: parent.right; anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            width: 110; height: 40; radius: 8
            color: scanMouse.pressed ? "#0090b0"
                 : (wifiManager.isScanning ? "#007090" : "#00d2ff")
            Behavior on color { ColorAnimation { duration: 120 } }

            Row {
                anchors.centerIn: parent; spacing: 8
                Canvas {
                    id: scanSpinner
                    width: 20; height: 20
                    visible: wifiManager.isScanning
                    property real angle: 0
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.save()
                        ctx.translate(width/2, height/2)
                        ctx.rotate(angle * Math.PI / 180)
                        ctx.strokeStyle = "#1a1a2e"
                        ctx.lineWidth = 2.5; ctx.lineCap = "round"
                        ctx.beginPath()
                        ctx.arc(0, 0, 7, 0, Math.PI * 1.4)
                        ctx.stroke()
                        ctx.restore()
                    }
                    Connections {
                        target: wifiManager
                        function onIsScanningChanged() {
                            if (wifiManager.isScanning) scanAnim.start()
                            else { scanAnim.stop(); scanSpinner.angle = 0; scanSpinner.requestPaint() }
                        }
                    }
                    RotationAnimation on angle {
                        id: scanAnim
                        from: 0; to: 360; duration: 800
                        loops: Animation.Infinite; running: false
                    }
                    onAngleChanged: requestPaint()
                }
                Text {
                    text: wifiManager.isScanning ? "Scanning..." : "Scan"
                    color: "#1a1a2e"; font.pixelSize: 15; font.bold: true
                }
            }

            MouseArea {
                id: scanMouse
                anchors.fill: parent
                onClicked: { if (!wifiManager.isScanning) wifiManager.scan() }
            }

            SequentialAnimation {
                id: scanDonePulse
                NumberAnimation { target: scanBtn; property: "scale"; from: 1.0; to: 1.08; duration: 120 }
                NumberAnimation { target: scanBtn; property: "scale"; from: 1.08; to: 1.0; duration: 120 }
            }
            Connections {
                target: wifiManager
                function onScanResultsChanged() { scanDonePulse.start() }
            }
        }
    }

    /* ===== CONNECTION ERROR BAR ===== */
    Item {
        id: errorBar
        anchors.top: topBar.bottom
        anchors.left: parent.left; anchors.right: parent.right
        height: visible ? 36 : 0
        visible: false; z: 9; clip: true
        Rectangle {
            anchors.fill: parent; color: "#b71c1c"
            Text {
                anchors.centerIn: parent
                text: wifiManager.connectionStatus === "Connection failed" ? "Connection failed"
                    : wifiManager.connectionStatus === "Connection timeout" ? "Connection timed out"
                    : wifiManager.connectionStatus === "Scan failed" ? "Scan failed" : "Error"
                color: "white"; font.pixelSize: 14; font.bold: true
            }
        }
        Behavior on height { NumberAnimation { duration: 200 } }
        function show() { visible = true; hideTimer.restart() }
        Timer { id: hideTimer; interval: 4000; onTriggered: errorBar.visible = false }
    }
    Connections {
        target: wifiManager
        function onConnectionStateChanged() {
            if (wifiManager.connectionStatus === "Connection failed" ||
                wifiManager.connectionStatus === "Connection timeout" ||
                wifiManager.connectionStatus === "Scan failed")
                errorBar.show()
        }
    }

    /* ===== SCANNING INDICATOR BAR ===== */
    Item {
        id: scanInd
        anchors.top: errorBar.bottom
        anchors.left: parent.left; anchors.right: parent.right
        height: wifiManager.isScanning ? 40 : 0
        visible: height > 0; clip: true
        Rectangle {
            anchors.fill: parent; color: "#16213e"
            Row {
                anchors.centerIn: parent; spacing: 8
                Repeater {
                    model: 3
                    Rectangle {
                        width: 8; height: 8; radius: 4; color: "#00d2ff"
                        SequentialAnimation on opacity {
                            loops: Animation.Infinite
                            PauseAnimation { duration: index * 250 }
                            NumberAnimation { from: 0.3; to: 1.0; duration: 400 }
                            NumberAnimation { from: 1.0; to: 0.3; duration: 400 }
                            PauseAnimation { duration: (2 - index) * 250 }
                        }
                    }
                }
                Text { text: "Scanning..."; color: "#00d2ff"; font.pixelSize: 14 }
            }
        }
        Behavior on height { NumberAnimation { duration: 200 } }
    }

    /* ===== WIFI LIST ===== */
    ListView {
        id: wifiList
        anchors.top: scanInd.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left; anchors.right: parent.right
        spacing: 6; clip: true
        model: wifiManager.scanResults

        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 1500

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
            contentItem: Rectangle {
                implicitWidth: 4; radius: 2
                color: "#00d2ff"; opacity: 0.5
            }
        }

        delegate: ItemDelegate {
            id: del
            width: wifiList.width - 24
            height: 64
            anchors.horizontalCenter: parent ? parent.horizontalCenter : undefined
            padding: 0

            background: Rectangle {
                radius: 10
                color: del.down || del.highlighted ? "#0f3460" : "#16213e"
                border.color: del.down || del.highlighted ? "#00d2ff" : "#2a2a4e"
                border.width: 1
            }

            contentItem: Row {
                anchors.fill: parent
                anchors.leftMargin: 12; anchors.rightMargin: 12
                spacing: 10

                Canvas {
                    width: 30; height: 22
                    anchors.verticalCenter: parent.verticalCenter
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        var lvl = modelData.signalLevel
                        var colors = ["#ff1744", "#ff6d00", "#ffab00", "#00e676"]
                        for (var i = 0; i < 4; i++) {
                            ctx.fillStyle = (i < lvl) ? colors[i] : "#3a3a5e"
                            ctx.fillRect(i * 7, height - (6 + i * 4), 5, 6 + i * 4)
                        }
                    }
                    Component.onCompleted: requestPaint()
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        text: modelData.ssid
                        color: "#e0e0e0"; font.pixelSize: 15
                        font.bold: modelData.connected
                        elide: Text.ElideRight
                        width: wifiList.width - 160
                    }
                    Row { spacing: 6
                        Text {
                            text: modelData.secured ? "Secured" : "Open"
                            color: modelData.secured ? "#ffab00" : "#4a4a5e"
                            font.pixelSize: 11
                        }
                        Text {
                            text: modelData.signal + " dBm"
                            color: "#808080"; font.pixelSize: 11
                        }
                    }
                }

                Item { Layout.fillWidth: true; width: 1 }

                /* Right section: ✓ for connected, Forget button for saved */
                Item {
                    width: 60
                    height: parent.height

                    Text {
                        anchors.centerIn: parent
                        text: "✓"; color: "#00e676"
                        font.pixelSize: 18; font.bold: true
                        visible: modelData.connected
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: 54; height: 26; radius: 6
                        visible: modelData.saved && !modelData.connected
                        color: forgetMa.pressed ? "#7a0000" : "#b71c1c"
                        Behavior on color { ColorAnimation { duration: 80 } }
                        Text {
                            anchors.centerIn: parent
                            text: "Forget"; color: "white"
                            font.pixelSize: 11; font.bold: true
                        }
                        MouseArea {
                            id: forgetMa; anchors.fill: parent
                            onPressed: mouse.accepted = true
                            onClicked: wifiManager.forgetNetwork(modelData.ssid)
                        }
                    }
                }
            }

            onClicked: {
                if (modelData.connected) {
                    // Already connected — do nothing
                } else if (modelData.saved) {
                    // Has saved credentials — auto-connect, no password needed
                    wifiManager.connectSaved(modelData.ssid)
                } else if (modelData.secured) {
                    passwordDialog.ssid = modelData.ssid
                    passwordDialog.input = ""
                    passwordDialog.visible = true
                } else {
                    wifiManager.connectToNetwork(modelData.ssid, "")
                }
            }
        }

        /* empty state */
        Item {
            anchors.fill: parent
            visible: wifiManager.scanResults.length === 0 && !wifiManager.isScanning
            Text {
                anchors.centerIn: parent
                text: "No WiFi networks found"
                color: "#808080"; font.pixelSize: 16
            }
        }
    }

    /* ===== PASSWORD DIALOG WITH VIRTUAL KEYBOARD ===== */
    Item {
        id: passwordDialog
        anchors.fill: parent
        visible: false; z: 200
        property string ssid: ""
        property string input: ""
        property bool showPwd: false
        property bool shiftActive: false

        function keyPress(k) {
            if (k === "BS") { input = input.slice(0, -1) }
            else if (k === "CLR") { input = ""; shiftActive = false; }
            else if (k === "SHIFT") { shiftActive = !shiftActive; }
            else if (input.length < 64) {
                if (shiftActive) {
                    input += (k.length === 1 && k >= 'a' && k <= 'z') ? k.toUpperCase() : k;
                    shiftActive = false;
                } else {
                    input += k;
                }
            }
        }

        function show(ssid) {
            this.ssid = ssid;
            input = "";
            showPwd = false;
            shiftActive = false;
            passwordDialog.visible = true;
        }

        function hide() {
            passwordDialog.visible = false;
            input = "";
            showPwd = false;
            shiftActive = false;
        }

        /* overlay */
        Rectangle {
            anchors.fill: parent; color: "#000000"; opacity: 0.5
            MouseArea { anchors.fill: parent; onClicked: passwordDialog.hide() }
        }

        /* dialog card - centered */
        Rectangle {
            id: dlg
            anchors.centerIn: parent
            width: 500; height: 430; radius: 16
            color: "#16213e"; border.color: "#00d2ff"; border.width: 2

            Text {
                id: dlgTitle
                anchors.top: parent.top; anchors.topMargin: 16
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Connect to WiFi"
                color: "#00d2ff"; font.pixelSize: 22; font.bold: true
            }

            Text {
                id: ssidLabel
                anchors.top: dlgTitle.bottom; anchors.topMargin: 6
                anchors.horizontalCenter: parent.horizontalCenter
                text: passwordDialog.ssid
                color: "#e0e0e0"; font.pixelSize: 16
            }

            /* Password display field */
            Rectangle {
                id: pwdFieldBg
                anchors.top: ssidLabel.bottom; anchors.topMargin: 10
                anchors.horizontalCenter: parent.horizontalCenter
                width: 420; height: 56; radius: 10
                color: "#0f3460"; border.color: "#00d2ff"; border.width: 2

                Text {
                    id: pwdDots
                    anchors.centerIn: parent
                    text: passwordDialog.input.length === 0 ? "Enter password (8-64 chars)"
                        : (passwordDialog.showPwd ? passwordDialog.input
                            : (new Array(passwordDialog.input.length).fill('•').join('')))
                    color: passwordDialog.input.length === 0 ? "#505070" : "#e0e0e0"
                    font.pixelSize: 20; font.family: "DejaVu Sans Mono"
                }
            }

            /* Show password toggle + char counter */
            Row {
                id: toggleRow
                anchors.top: pwdFieldBg.bottom; anchors.topMargin: 10
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12

                Rectangle {
                    width: 24; height: 24; radius: 6
                    color: passwordDialog.showPwd ? "#00d2ff" : "transparent"
                    border.color: "#505070"; border.width: 1
                    Text {
                        anchors.centerIn: parent
                        text: passwordDialog.showPwd ? "✓" : ""
                        color: "#1a1a2e"; font.pixelSize: 14; font.bold: true
                    }
                    MouseArea { anchors.fill: parent; onClicked: passwordDialog.showPwd = !passwordDialog.showPwd }
                }
                Text { text: "Show password"; color: "#808080"; font.pixelSize: 12 }

                Item { width: 1 }

                Text {
                    id: charCounter
                    text: passwordDialog.input.length + " / 64"
                    color: passwordDialog.input.length >= 8 ? "#00e676" : "#ff6d00"
                    font.pixelSize: 12; font.bold: true
                }
            }

            /* Virtual keyboard grid */
            Grid {
                id: keysGrid
                anchors.top: toggleRow.bottom; anchors.topMargin: 8
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 11; spacing: 5

                // Row 1: numbers + backspace
                Repeater {
                    model: ["1","2","3","4","5","6","7","8","9","0","⌫"]
                    delegate: Rectangle {
                        width: 40; height: 40; radius: 8
                        color: ma.pressed ? "#4a4a5e" : "#2a2a4e"
                        border.color: "#3a3a5e"; border.width: 1
                        Text { anchors.centerIn: parent; text: modelData; color: "white"; font.pixelSize: 14; font.bold: true }
                        MouseArea {
                            id: ma; anchors.fill: parent
                            onClicked: {
                                if (modelData === "⌫") passwordDialog.keyPress("BS")
                                else passwordDialog.keyPress(modelData)
                            }
                        }
                    }
                }

                // Row 2: q-p
                Repeater {
                    model: ["q","w","e","r","t","y","u","i","o","p","-"]
                    delegate: Rectangle {
                        width: 40; height: 40; radius: 8
                        color: ma2.pressed ? "#4a4a5e" : "#2a2a4e"
                        border.color: "#3a3a5e"; border.width: 1
                        Text { anchors.centerIn: parent; text: modelData; color: "white"; font.pixelSize: 14; font.bold: true }
                        MouseArea {
                            id: ma2; anchors.fill: parent
                            onClicked: passwordDialog.keyPress(modelData)
                        }
                    }
                }

                // Row 3: a-l + @ + shift
                Repeater {
                    model: ["a","s","d","f","g","h","j","k","l","@","⇧"]
                    delegate: Rectangle {
                        width: 40; height: 40; radius: 8
                        property bool isShiftKey: modelData === "⇧"
                        color: (isShiftKey && passwordDialog.shiftActive) ? "#00d2ff"
                             : (ma3.pressed ? "#4a4a5e" : "#2a2a4e")
                        border.color: (isShiftKey && passwordDialog.shiftActive) ? "#00d2ff" : "#3a3a5e"
                        border.width: (isShiftKey && passwordDialog.shiftActive) ? 2 : 1
                        Behavior on color { ColorAnimation { duration: 80 } }
                        Text {
                            anchors.centerIn: parent; text: modelData
                            color: (isShiftKey && passwordDialog.shiftActive) ? "#1a1a2e" : "white"
                            font.pixelSize: 14; font.bold: true
                        }
                        MouseArea {
                            id: ma3; anchors.fill: parent
                            onClicked: {
                                if (isShiftKey) passwordDialog.keyPress("SHIFT")
                                else passwordDialog.keyPress(modelData)
                            }
                        }
                    }
                }

                // Row 4: z-m + . + _ + ! + shift
                Repeater {
                    model: ["z","x","c","v","b","n","m",".","_","!","⇧"]
                    delegate: Rectangle {
                        width: 40; height: 40; radius: 8
                        property bool isShiftKey: modelData === "⇧"
                        color: (isShiftKey && passwordDialog.shiftActive) ? "#00d2ff"
                             : (ma4.pressed ? "#4a4a5e" : "#2a2a4e")
                        border.color: (isShiftKey && passwordDialog.shiftActive) ? "#00d2ff" : "#3a3a5e"
                        border.width: (isShiftKey && passwordDialog.shiftActive) ? 2 : 1
                        Behavior on color { ColorAnimation { duration: 80 } }
                        Text {
                            anchors.centerIn: parent; text: modelData
                            color: (isShiftKey && passwordDialog.shiftActive) ? "#1a1a2e" : "white"
                            font.pixelSize: 14; font.bold: true
                        }
                        MouseArea {
                            id: ma4; anchors.fill: parent
                            onClicked: {
                                if (isShiftKey) passwordDialog.keyPress("SHIFT")
                                else passwordDialog.keyPress(modelData)
                            }
                        }
                    }
                }

            }

            /* Action buttons */
            Row {
                anchors.top: keysGrid.bottom; anchors.topMargin: 8
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8

                Rectangle {
                    width: 110; height: 40; radius: 8
                    color: ma5.pressed ? "#4a4a5e" : "#ff6d00"
                    Text { anchors.centerIn: parent; text: "CLEAR"; color: "white"; font.pixelSize: 12; font.bold: true }
                    MouseArea { id: ma5; anchors.fill: parent; onClicked: passwordDialog.keyPress("CLR") }
                }
                Rectangle {
                    width: 110; height: 40; radius: 8
                    color: ma6.pressed ? "#4a4a5e" : "#2a2a4e"
                    border.color: "#3a3a5e"; border.width: 1
                    Text { anchors.centerIn: parent; text: "␣"; color: "white"; font.pixelSize: 16; font.bold: true }
                    MouseArea { id: ma6; anchors.fill: parent; onClicked: passwordDialog.keyPress(" ") }
                }
                Rectangle {
                    width: 110; height: 40; radius: 8
                    color: (passwordDialog.input.length >= 8) ? (ma7.pressed ? "#0090b0" : "#00d2ff") : "#4a4a5e"
                    Text {
                        anchors.centerIn: parent; text: "CONNECT"
                        color: passwordDialog.input.length >= 8 ? "#1a1a2e" : "#808080"
                        font.pixelSize: 12; font.bold: true
                    }
                    MouseArea {
                        id: ma7; anchors.fill: parent
                        onClicked: {
                            if (passwordDialog.input.length >= 8) {
                                wifiManager.connectToNetwork(passwordDialog.ssid, passwordDialog.input)
                                passwordDialog.hide()
                            }
                        }
                    }
                }
                Rectangle {
                    width: 110; height: 40; radius: 8
                    color: ma8.pressed ? "#4a4a5e" : "#b71c1c"
                    Text { anchors.centerIn: parent; text: "CANCEL"; color: "white"; font.pixelSize: 12; font.bold: true }
                    MouseArea { id: ma8; anchors.fill: parent; onClicked: passwordDialog.hide() }
                }
            }
        }
    }

    Component.onCompleted: wifiManager.scan()
}