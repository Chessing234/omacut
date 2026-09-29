import QtQuick
import "Format.js" as Format

// The video as a row of clips over a filmstrip. Each clip has a handle at both
// ends; whatever falls outside every clip is cut. Double-click a clip to split
// it, double-click a gap or a split to join the clips around it, and hover a
// clip to remove it. All times are in seconds.
Item {
    id: root
    implicitHeight: 76

    readonly property var timeline: backend.timeline
    readonly property real durationSec: backend.duration
    readonly property var clips: timeline.clips
    property real playheadSec: 0
    property color accent: "#FFD60A"
    property color accentForeground: "black"
    property bool zoomed: false
    // Frozen at zoom time, so dragging a handle doesn't rescale the view under it.
    property real viewStartSec: 0
    property real viewEndSec: 0
    // Something is being dragged, so playback must not move the playhead.
    readonly property bool interacting: area.mode !== 0
    readonly property bool trimming: area.mode === 1

    readonly property real handleW: 14
    // The clip frames' border; the filmstrip sits exactly inside it.
    readonly property int frameW: 3
    readonly property real frameRadius: 8
    // The stretch of the video the track currently shows.
    readonly property real windowStart: zoomed ? viewStartSec : 0
    readonly property real windowEnd: zoomed ? viewEndSec : durationSec
    readonly property real windowLen: Math.max(windowEnd - windowStart, 0.001)
    readonly property color film: "#1c1c1e"

    signal scrub(real seconds)

    function xForTime(t) { return durationSec <= 0 ? 0 : ((t - windowStart) / windowLen) * width }
    function timeForX(x) {
        if (width <= 0 || durationSec <= 0) return 0
        return windowStart + Math.max(0, Math.min(1, x / width)) * windowLen
    }
    // Z frames the clip under the playhead with some slack; again zooms out.
    // If the clip changed since the last zoom, Z zooms again on it instead.
    function toggleZoom() {
        if (durationSec <= 0) return
        var i = timeline.clipAt(playheadSec)
        var from = i >= 0 ? clips[i].start : 0, to = i >= 0 ? clips[i].end : durationSec
        var slack = (to - from) / 8
        var newStart = Math.max(0, from - slack), newEnd = Math.min(durationSec, to + slack)
        if (zoomed && newStart === viewStartSec && newEnd === viewEndSec) zoomed = false
        else { viewStartSec = newStart; viewEndSec = newEnd; zoomed = true }
        backend.requestThumbs(windowStart, windowEnd)
    }

    Rectangle {
        id: track
        anchors.fill: parent; anchors.topMargin: root.frameW; anchors.bottomMargin: root.frameW
        radius: root.frameRadius - root.frameW; color: root.film; clip: true
        Item {
            anchors.fill: parent
            Repeater {
                model: backend.thumbCount
                Image {
                    // Whole-pixel edges, so neighbouring frames never leave a seam between them.
                    readonly property real slot: track.width / Math.max(backend.thumbCount, 1)
                    x: Math.round(index * slot); width: Math.round((index + 1) * slot) - x; height: track.height
                    // Bound the decoded texture to the strip height. The filmstrip
                    // has a fixed height, so this never thrashes on resize.
                    sourceSize.height: track.height
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true; cache: false
                    source: index < backend.thumbReadyCount ? "image://thumbs/" + backend.thumbRevision + "/" + index : ""
                }
            }
        }
        // Dim what's cut: everything before, between, and after the clips.
        Repeater {
            model: root.clips.length + 1
            Rectangle {
                required property int index
                readonly property real from: index === 0 ? 0 : root.clips[index - 1].end
                readonly property real to: index === root.clips.length ? root.durationSec : root.clips[index].start
                visible: to > from
                x: Math.round(root.xForTime(from)); width: Math.max(0, Math.round(root.xForTime(to)) - x); height: track.height
                color: area.gapHovered === index ? "#90000000" : "#c8000000"
            }
        }
    }

    // Beneath the clip frames, so it never draws over a handle.
    Rectangle {
        visible: root.durationSec > 0 && root.playheadSec >= root.windowStart && root.playheadSec <= root.windowEnd
        // Within a clip, keep the line between its handles, so a playhead at the clip's
        // first or last frame shows on that frame instead of hiding under the handle.
        x: {
            var px = root.xForTime(root.playheadSec) - 1
            for (var i = 0; i < root.clips.length; ++i) {
                var c = root.clips[i]
                if (root.playheadSec < c.start || root.playheadSec > c.end) continue
                var from = Math.round(root.xForTime(c.start)) + root.handleW
                var to = Math.round(root.xForTime(c.end)) - root.handleW - width
                if (to >= from) px = Math.max(from, Math.min(px, to))
                break
            }
            return Math.max(0, Math.min(root.width - width, px))
        }
        y: track.y
        width: 2; height: track.height; color: "white"
    }

    component Handle: Rectangle {
        property bool leading: true
        x: leading ? 0 : parent.width - width
        width: Math.min(root.handleW, parent.width / 2); height: parent.height
        topLeftRadius: leading ? root.frameRadius : 0; bottomLeftRadius: topLeftRadius
        topRightRadius: leading ? 0 : root.frameRadius; bottomRightRadius: topRightRadius
        color: root.accent
        Rectangle { anchors.centerIn: parent; width: 2; height: 16; radius: 1; color: root.film }
    }

    // One accent frame per clip, with its handles inside the frame. Counted
    // rather than modelled on the clips, so a drag updates frames in place.
    // Clipped, so zoomed-out-of-view clips never draw over the buttons beside it.
    Item {
        anchors.fill: parent
        clip: true
        Repeater {
            model: root.clips.length
            Item {
                id: clipItem
                required property int index
                readonly property var range: root.clips[index]
                readonly property bool hovered: area.clipHovered === index
                // Whole pixels, so handles meet the filmstrip without an antialiased seam.
                x: Math.round(root.xForTime(range.start)); width: Math.max(4, Math.round(root.xForTime(range.end)) - x)
                height: root.height
                Rectangle {
                    anchors.fill: parent
                    radius: root.frameRadius; color: "transparent"
                    border.color: root.accent; border.width: root.frameW
                }
                Handle { leading: true }
                Handle { leading: false }
                // Remove this clip. Only offered when another clip would remain.
                Rectangle {
                    id: remove
                    visible: clipItem.hovered && root.clips.length > 1 && clipItem.width > 60 && area.mode === 0
                    anchors.top: parent.top; anchors.right: parent.right
                    anchors.topMargin: 7; anchors.rightMargin: root.handleW + 4
                    width: 20; height: 20; radius: 10
                    readonly property bool hot: area.removeHovered
                    color: hot ? root.accent : "#e0202023"
                    Text {
                        anchors.centerIn: parent; anchors.verticalCenterOffset: -1
                        text: "×"; font.pixelSize: 16; font.weight: Font.DemiBold
                        color: remove.hot ? root.accentForeground : "white"
                    }
                }
            }
        }
    }

    // Hint above the timeline: the time under a dragged handle, or what a double-click will do.
    Rectangle {
        readonly property bool dragging: root.trimming
        readonly property string hint: area.gapHovered >= 0 ? "Double-click to restore"
                                      : area.splitHovered >= 0 ? "Double-click to join"
                                      : area.clipHovered >= 0 && !area.onHandle ? "Double-click to split" : ""
        visible: root.enabled && (dragging || (area.mode === 0 && area.containsMouse && !area.removeHovered && hint !== ""))
        width: label.implicitWidth + 20; height: dragging ? 32 : 26
        radius: 7; color: "#2c2c2f"
        x: Math.max(0, Math.min(root.width - width, (dragging ? root.xForTime(area.activeTime) : area.mouseX) - width / 2)); y: -height - 8
        Text {
            id: label; anchors.centerIn: parent; color: "white"
            text: parent.dragging ? Format.fmt(area.activeTime) : parent.hint
            font.pixelSize: parent.dragging ? 15 : 12
            font.family: parent.dragging ? "monospace" : Qt.application.font.family
            font.weight: parent.dragging ? Font.DemiBold : Font.Normal
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        // 0 none, 1 dragging a handle, 2 scrubbing, 3 pressing a clip's ×
        property int mode: 0
        property int clipIndex: -1
        property bool leftEdge: true
        property real activeTime: 0
        // Where the playhead was when the drag began: the drag moves the
        // playhead along, so snapping to it live would catch on itself.
        property real dragStartPlayhead: 0
        property int clipHovered: -1
        property int gapHovered: -1
        property int splitHovered: -1
        property bool onHandle: false
        property bool removeHovered: false

        // The hovered clip's × sits in its top-right corner, inside the handle.
        function onRemove(x, y) {
            if (clipHovered < 0 || root.clips.length < 2) return false
            var right = root.xForTime(root.clips[clipHovered].end) - root.handleW - 4
            var width = root.xForTime(root.clips[clipHovered].end) - root.xForTime(root.clips[clipHovered].start)
            return width > 60 && x >= right - 20 && x <= right && y >= 7 && y <= 27
        }

        // Where x lands: a handle of some clip, a split between touching clips, inside a clip, or a gap.
        function hitTest(x) {
            var best = null
            for (var i = 0; i < root.clips.length; ++i) {
                var a = root.xForTime(root.clips[i].start), b = root.xForTime(root.clips[i].end)
                var w = Math.min(root.handleW, (b - a) / 2) + 2
                if (x >= a - 3 && x <= a + w) best = { handle: true, clip: i, left: true }
                else if (x >= b - w && x <= b + 3 && !best) best = { handle: true, clip: i, left: false }
                if (best) {
                    // Between touching clips, the side of the line decides which clip is grabbed.
                    if (!best.left && i + 1 < root.clips.length && root.clips[i + 1].start === root.clips[i].end && x > b)
                        best = { handle: true, clip: i + 1, left: true }
                    var edge = best.left ? root.clips[best.clip].start : root.clips[best.clip].end
                    var neighbour = best.left ? best.clip - 1 : best.clip + 1
                    best.split = neighbour >= 0 && neighbour < root.clips.length
                        && (best.left ? root.clips[neighbour].end === edge : root.clips[neighbour].start === edge)
                        ? Math.min(best.clip, neighbour) : -1
                    return best
                }
                if (x > a && x < b) return { handle: false, clip: i, split: -1 }
            }
            return { handle: false, clip: -1, gap: root.timeline.gapAt(root.timeForX(x)), split: -1 }
        }
        // Handles catch on neighbouring clip edges and the playhead.
        function snap(t) {
            var best = t, bestDistance = 8
            function consider(s) {
                var d = Math.abs(root.xForTime(s) - root.xForTime(t))
                if (d < bestDistance) { best = s; bestDistance = d }
            }
            for (var j = 0; j < root.clips.length; ++j)
                if (j !== clipIndex) { consider(root.clips[j].start); consider(root.clips[j].end) }
            consider(dragStartPlayhead)
            return best
        }
        function seek(t) { root.playheadSec = t; root.scrub(t) }
        function hover(x, y) {
            var hit = hitTest(x)
            onHandle = hit.handle
            clipHovered = hit.clip
            splitHovered = hit.split
            gapHovered = hit.clip < 0 ? hit.gap : -1
            removeHovered = onRemove(x, y)
            cursorShape = removeHovered ? Qt.PointingHandCursor : hit.handle ? Qt.SizeHorCursor : Qt.ArrowCursor
        }

        onPositionChanged: mouse => {
            if (root.durationSec <= 0) return
            var t = root.timeForX(mouse.x)
            if (mode === 0) { hover(mouse.x, mouse.y); return }
            if (mode === 2) { seek(t); return }
            if (mode !== 1) return
            root.timeline.moveEdge(clipIndex, leftEdge, snap(t))
            activeTime = leftEdge ? root.clips[clipIndex].start : root.clips[clipIndex].end
            seek(activeTime)
        }
        onPressed: mouse => {
            if (root.durationSec <= 0) return
            if (removeHovered) { mode = 3; return }
            var hit = hitTest(mouse.x)
            if (hit.handle) {
                mode = 1; clipIndex = hit.clip; leftEdge = hit.left; dragStartPlayhead = root.playheadSec
                activeTime = leftEdge ? root.clips[clipIndex].start : root.clips[clipIndex].end
                root.timeline.beginGesture()
            } else {
                mode = 2; seek(root.timeForX(mouse.x))
            }
        }
        onReleased: {
            if (mode === 1) root.timeline.endGesture()
            if (mode === 3 && onRemove(mouseX, mouseY)) root.timeline.removeClip(clipHovered)
            mode = 0; clipIndex = -1
            hover(mouseX, mouseY)
        }
        onDoubleClicked: mouse => {
            if (onRemove(mouse.x, mouse.y)) return
            var hit = hitTest(mouse.x)
            if (hit.split >= 0) root.timeline.joinClips(hit.split)
            else if (hit.clip >= 0 && !hit.handle) root.timeline.split(root.timeForX(mouse.x))
            else if (hit.clip < 0) root.timeline.restoreGap(hit.gap)
            hover(mouse.x, mouse.y)
        }
        onExited: { clipHovered = -1; gapHovered = -1; splitHovered = -1; removeHovered = false }
    }
}
