import QtQuick

Window {
    id: root

    width: 1280
    height: 720
    visible: true
    color: "#101418"
    title: qsTr("Mini Surgeon Console")

    // The teleop core's state, and the hand controller. main.cpp finds it by objectName and gives
    // it the core. The UI shows what the core decided and passes requests on; it decides nothing.
    TeleopModel {
        id: teleop

        objectName: "teleop"
    }

    // Faults on purpose (testing): the toggles at the right of the status bar.
    FaultInjector {
        id: faults

        objectName: "faults"
    }

    // A flat button: enabled only when the action would be accepted.
    component PanelButton: Rectangle {
        id: button

        property string label
        property bool active: true
        signal clicked

        implicitHeight: 34
        radius: 4
        color: !active ? "#232b33" : (mouse.pressed ? "#3a4652" : "#2d3740")
        border.color: active ? "#4b5a68" : "#2d3740"

        Text {
            anchors.centerIn: parent
            text: button.label
            color: button.active ? "#e6edf3" : "#5b6876"
            font.pixelSize: 14
        }
        MouseArea {
            id: mouse

            anchors.fill: parent
            enabled: button.active
            onClicked: button.clicked()
        }
    }

    // Keyboard, as on a console with pedals: Space held = clutch, E engage, Esc disengage, R reset.
    Item {
        anchors.fill: parent
        focus: true

        Keys.onPressed: event => {
            if (event.isAutoRepeat) {
                return;
            }
            if (event.key === Qt.Key_Space) {
                teleop.setClutch(true);
            } else if (event.key === Qt.Key_E) {
                teleop.engage();
            } else if (event.key === Qt.Key_Escape) {
                teleop.disengage();
            } else if (event.key === Qt.Key_R) {
                teleop.reset();
            }
        }
        Keys.onReleased: event => {
            if (!event.isAutoRepeat && event.key === Qt.Key_Space) {
                teleop.setClutch(false);
            }
        }
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
        // The frame says who drives the arm: green while the hand does, red in fault.
        border.width: 3
        border.color: teleop.state === "engaged" ? "#2ea043" : (teleop.state === "fault" ? "#da3633" : "#1b2229")

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

    // M4: the teleop state and controls, then the arm link and the video timing.
    Rectangle {
        id: sidePanel

        anchors {
            top: parent.top
            right: parent.right
            bottom: statusBar.top
            margins: 12
        }
        width: 300
        color: "#1b2229"
        radius: 6

        Column {
            id: panel

            anchors {
                fill: parent
                margins: 14
            }
            spacing: 8

            // The state, impossible to miss. In fault: why, and what the operator can do.
            Rectangle {
                width: panel.width
                height: stateColumn.implicitHeight + 16
                radius: 4
                color: {
                    switch (teleop.state) {
                    case "engaged": return "#1f6f3a";
                    case "paused": return "#6b5310";
                    case "fault": return "#8e1f1f";
                    default: return "#2d3740";
                    }
                }

                Column {
                    id: stateColumn

                    anchors {
                        left: parent.left
                        right: parent.right
                        verticalCenter: parent.verticalCenter
                        margins: 10
                    }
                    spacing: 2

                    Text {
                        text: {
                            switch (teleop.state) {
                            case "engaged": return qsTr("ENGAGED - hand drives the arm");
                            case "paused": return qsTr("PAUSED - clutch held, arm holds");
                            case "fault": return qsTr("FAULT");
                            case "idle": return qsTr("Idle - arm holds");
                            default: return qsTr("Teleop not available (no arm link)");
                            }
                        }
                        color: "white"
                        font.pixelSize: 16
                        font.bold: teleop.state === "fault" || teleop.state === "engaged"
                    }
                    Text {
                        visible: teleop.state === "fault"
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: qsTr("%1 (%2 s ago). The arm is holding.").arg(teleop.fault)
                                                                        .arg(teleop.faultAgeSeconds.toFixed(0))
                        color: "white"
                        font.pixelSize: 14
                    }
                    Text {
                        visible: teleop.state === "fault" || (teleop.state === "idle" && !teleop.engageAllowed)
                        width: parent.width
                        wrapMode: Text.Wrap
                        text: teleop.present.length > 0
                              ? qsTr("Still present: %1").arg(teleop.present.join(", "))
                              : qsTr("Cause cleared: press Reset (R).")
                        color: "#f0d0d0"
                        font.pixelSize: 13
                    }
                }
            }

            Row {
                spacing: 6

                PanelButton {
                    width: (panel.width - 12) / 3
                    label: qsTr("Engage (E)")
                    active: teleop.engageAllowed
                    onClicked: teleop.engage()
                }
                PanelButton {
                    width: (panel.width - 12) / 3
                    label: qsTr("Disengage")
                    active: teleop.state === "engaged" || teleop.state === "paused"
                    onClicked: teleop.disengage()
                }
                PanelButton {
                    width: (panel.width - 12) / 3
                    label: qsTr("Reset (R)")
                    active: teleop.resetAllowed
                    onClicked: teleop.reset()
                }
            }

            // Held, not toggled: letting go of a pedal must always end the clutch.
            PanelButton {
                width: panel.width
                label: teleop.clutch ? qsTr("CLUTCH HELD") : qsTr("Hold for clutch (Space)")
                active: true
                color: teleop.clutch ? "#6b5310" : (clutchMouse.pressed ? "#3a4652" : "#2d3740")

                MouseArea {
                    id: clutchMouse

                    anchors.fill: parent
                    onPressed: teleop.setClutch(true)
                    onReleased: teleop.setClutch(false)
                    onCanceled: teleop.setClutch(false)
                }
            }

            Text {
                visible: teleop.refusal !== ""
                width: panel.width
                wrapMode: Text.Wrap
                text: qsTr("Refused: %1").arg(teleop.refusal)
                color: "#f0b429"
                font.pixelSize: 13
            }

            // The hand controller stand-in: drag to move the hand (joints 1 and 2).
            Rectangle {
                id: pad

                width: panel.width
                height: 150
                radius: 4
                color: "#11161b"
                border.color: "#2d3740"

                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: 1
                    color: "#2d3740"
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 1
                    height: parent.height
                    color: "#2d3740"
                }
                Rectangle {
                    width: 16
                    height: 16
                    radius: 8
                    x: (teleop.handX + 1) / 2 * (pad.width - width)
                    y: (1 - (teleop.handY + 1) / 2) * (pad.height - height)
                    color: teleop.clutch ? "#f0b429" : (teleop.state === "engaged" ? "#3fb950" : "#9aa7b4")
                }
                Text {
                    anchors {
                        left: parent.left
                        bottom: parent.bottom
                        margins: 6
                    }
                    text: teleop.demo ? qsTr("hand: scripted (--hand-demo)") : qsTr("hand: drag here")
                    color: "#5b6876"
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !teleop.demo
                    function move(mouse) {
                        teleop.setHand(mouse.x / width * 2 - 1, 1 - mouse.y / height * 2);
                    }
                    onPressed: mouse => move(mouse)
                    onPositionChanged: mouse => move(mouse)
                }
            }

            // M3: the link to arm_sim. Red when the arm's heartbeats stop: the arm then holds on
            // its own (REQ-SAF-001), and the operator has to know why.
            Text {
                text: qsTr("Arm link: %1").arg(teleop.link)
                color: teleop.link === "OK" ? "#3fb950" : (teleop.link === "lost" ? "#f85149" : "#9aa7b4")
                font.pixelSize: 15
            }
            Text {
                visible: teleop.armState !== ""
                text: qsTr("Arm: %1%2").arg(teleop.armState)
                                       .arg(teleop.joints.length > 1
                                            ? " · j1 %1 · j2 %2 rad".arg(teleop.joints[0].toFixed(2))
                                                                    .arg(teleop.joints[1].toFixed(2))
                                            : "")
                color: teleop.armState === "holding" ? "#f0b429" : "#9aa7b4"
                font.pixelSize: 15
            }

            // Video timing (REQ-VID-002). Here, not over the picture: nothing should cover
            // the surgeon's view (and the test pattern's barcode must stay readable).
            Item {
                width: 1
                height: 4
            }
            Text {
                text: qsTr("Video")
                color: "#e6edf3"
                font.pixelSize: 16
            }
            Text {
                width: panel.width
                text: qsTr("Source: %1").arg(video.sourceStatus)
                wrapMode: Text.Wrap
                color: "#9aa7b4"
                font.pixelSize: 14
            }
            Text {
                // Capture to present: from the camera's timestamp until the frame was handed
                // to the OS for display. The panel itself adds a little more (glass-to-glass).
                text: video.latencySamples > 0
                      ? qsTr("Latency p50 / p99: %1 / %2 ms").arg(video.latencyP50Ms.toFixed(1))
                                                             .arg(video.latencyP99Ms.toFixed(1))
                      : qsTr("Latency: measuring…")
                color: "#9aa7b4"
                font.pixelSize: 14
            }
            Text {
                text: qsTr("Render %1 fps · video %2 fps · dropped %3").arg(video.renderFps.toFixed(0))
                                                                       .arg(video.videoFps.toFixed(0))
                                                                       .arg(video.framesDropped)
                color: "#9aa7b4"
                font.pixelSize: 14
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
            text: qsTr("M4 teleoperation · v%1 · Space clutch · E engage · Esc disengage · R reset")
                      .arg(Qt.application.version)
            color: "#6b7a89"
            font.pixelSize: 13
        }

        // The fault injector, for testing the safety chain by hand. Red while a fault is injected.
        Row {
            anchors {
                verticalCenter: parent.verticalCenter
                right: parent.right
                rightMargin: 12
            }
            spacing: 6
            visible: faults.available

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("inject:")
                color: "#6b7a89"
                font.pixelSize: 13
            }
            Repeater {
                model: ["link", "corrupt", "hand", "video", "fault"]

                Rectangle {
                    required property string modelData
                    readonly property bool on: faults[modelData]

                    width: chip.implicitWidth + 16
                    height: 22
                    radius: 11
                    color: on ? "#8e1f1f" : "#1b2229"
                    border.color: on ? "#da3633" : "#2d3740"

                    Text {
                        id: chip

                        anchors.centerIn: parent
                        text: parent.modelData
                        color: parent.on ? "white" : "#9aa7b4"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: faults[parent.modelData] = !parent.on
                    }
                }
            }
        }
    }
}
