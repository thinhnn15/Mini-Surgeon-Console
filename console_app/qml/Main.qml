import QtQuick

Window {
    id: root

    width: 1280
    height: 720
    visible: true
    color: "#101418"
    title: qsTr("Mini Surgeon Console")

    // M2: the endoscope video and the HUD overlay are drawn here by a QQuickRhiItem.
    Rectangle {
        id: videoArea

        anchors {
            left: parent.left
            top: parent.top
            right: sidePanel.left
            bottom: statusBar.top
            margins: 12
        }
        color: "#1b2229"
        radius: 6

        Text {
            anchors.centerIn: parent
            text: qsTr("Video — M2")
            color: "#6b7a89"
            font.pixelSize: 22
        }
    }

    // M4: state, link and fault information come from the teleop core.
    Rectangle {
        id: sidePanel

        anchors {
            top: parent.top
            right: parent.right
            bottom: statusBar.top
            margins: 12
        }
        width: 280
        color: "#1b2229"
        radius: 6

        Column {
            anchors {
                fill: parent
                margins: 16
            }
            spacing: 12

            Text {
                text: qsTr("State: Idle")
                color: "#e6edf3"
                font.pixelSize: 18
            }
            Text {
                text: qsTr("Arm link: —")
                color: "#9aa7b4"
                font.pixelSize: 15
            }
            Text {
                text: qsTr("Faults: none")
                color: "#9aa7b4"
                font.pixelSize: 15
            }
        }
    }

    Rectangle {
        id: statusBar

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: 32
        color: "#0b0e11"

        Text {
            anchors {
                verticalCenter: parent.verticalCenter
                left: parent.left
                leftMargin: 12
            }
            text: qsTr("M1 skeleton · v%1").arg(Qt.application.version)
            color: "#6b7a89"
            font.pixelSize: 13
        }
    }
}
