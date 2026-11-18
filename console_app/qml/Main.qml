import QtQuick

Window {
    id: root

    width: 1280
    height: 720
    visible: true
    color: "#101418"
    title: qsTr("Mini Surgeon Console")

    // M2: the endoscope video, drawn by VideoItem (a QQuickRhiItem). M5 adds the HUD overlay.
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

        // The endoscope view. main.cpp finds it by objectName and gives it the frame source.
        VideoItem {
            id: video

            objectName: "video"
            anchors {
                fill: parent
                margins: 6
            }
        }

        // The software renderer has no GPU API, so it cannot draw a QQuickRhiItem: the video
        // area would stay black, which looks like a dead camera. Say what is going on instead.
        Text {
            anchors.centerIn: parent
            width: parent.width - 48
            visible: video.GraphicsInfo.api === GraphicsInfo.Software
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("No video: the software renderer is active, and video needs a GPU graphics backend "
                       + "(Direct3D, Vulkan, Metal or OpenGL).")
            color: "#f0b429"
            font.pixelSize: 18
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
            text: qsTr("M2 video · v%1").arg(Qt.application.version)
            color: "#6b7a89"
            font.pixelSize: 13
        }
    }
}
