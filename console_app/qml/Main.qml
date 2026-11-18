import QtQuick

Window {
    id: root

    width: 1280
    height: 720
    visible: true
    color: "#101418"
    title: qsTr("Mini Surgeon Console")

    // The arm link's state for the side panel. main.cpp finds it by objectName and gives it the link.
    ArmLinkModel {
        id: armLink

        objectName: "armLink"
    }

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

        // REQ-VID-003: a picture that is not live must never look live. When frames stop, the
        // last one stays on screen; this banner says so. Across the top edge, so most of the
        // last picture stays visible (it still shows where the instruments were).
        Rectangle {
            id: videoWarning

            readonly property bool frozen: video.videoState === VideoItem.Frozen
            readonly property bool waiting: video.videoState === VideoItem.Waiting

            visible: (frozen || waiting) && video.GraphicsInfo.api !== GraphicsInfo.Software
            anchors {
                left: video.left
                right: video.right
                top: video.top
            }
            height: 44
            color: frozen ? "#c62828" : "#5f4b0b"

            Text {
                anchors.centerIn: parent
                text: videoWarning.frozen
                      ? qsTr("VIDEO FROZEN - last new frame %1 s ago").arg((video.frameAgeMs / 1000).toFixed(1))
                      : qsTr("Waiting for video…")
                color: "white"
                font.pixelSize: 20
                font.bold: videoWarning.frozen
            }
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
            // M3: the link to arm_sim, from the comms thread. Red when the arm's heartbeats stop:
            // the arm then holds on its own (REQ-SAF-001), and the operator has to know why.
            Text {
                text: qsTr("Arm link: %1").arg(armLink.link)
                color: armLink.linkOk ? "#3fb950" : (armLink.link === "lost" ? "#f85149" : "#9aa7b4")
                font.pixelSize: 15
            }
            Text {
                visible: armLink.armState !== ""
                text: qsTr("Arm: %1%2").arg(armLink.armState)
                                       .arg(armLink.joints.length > 0
                                            ? " · j1 " + armLink.joints[0].toFixed(2) + " rad" : "")
                color: armLink.armState === "holding" ? "#f0b429" : "#9aa7b4"
                font.pixelSize: 15
            }
            Text {
                text: qsTr("Faults: none")
                color: "#9aa7b4"
                font.pixelSize: 15
            }

            // Video timing (REQ-VID-002). Here, not over the picture: nothing should cover
            // the surgeon's view (and the test pattern's barcode must stay readable).
            Item {
                width: 1
                height: 8
            }
            Text {
                text: qsTr("Video")
                color: "#e6edf3"
                font.pixelSize: 18
            }
            Text {
                width: parent.width
                text: qsTr("Source: %1").arg(video.sourceStatus)
                wrapMode: Text.Wrap
                color: "#9aa7b4"
                font.pixelSize: 15
            }
            Text {
                // Capture to present: from the camera's timestamp until the frame was handed
                // to the OS for display. The panel itself adds a little more (glass-to-glass).
                text: video.latencySamples > 0
                      ? qsTr("Latency p50 / p99: %1 / %2 ms").arg(video.latencyP50Ms.toFixed(1))
                                                             .arg(video.latencyP99Ms.toFixed(1))
                      : qsTr("Latency: measuring…")
                color: "#9aa7b4"
                font.pixelSize: 15
            }
            Text {
                text: qsTr("Render %1 fps · video %2 fps").arg(video.renderFps.toFixed(0))
                                                          .arg(video.videoFps.toFixed(0))
                color: "#9aa7b4"
                font.pixelSize: 15
            }
            Text {
                text: qsTr("Frames shown %1 · dropped %2").arg(video.framesShown).arg(video.framesDropped)
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
