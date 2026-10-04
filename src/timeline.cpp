#include "timeline.h"

#include <QVariantMap>

#include <algorithm>

QVariantList Timeline::clipList() const {
    QVariantList result;
    for (const edit::Range &clip : m_clips)
        result.append(QVariantMap{{QStringLiteral("start"), clip.start},
                                  {QStringLiteral("end"), clip.end}});
    return result;
}

bool Timeline::unexported() const {
    return !m_clips.isEmpty() && !edit::untouched(m_clips, m_duration)
        && m_clips != m_exported;
}

void Timeline::reset(double duration) {
    m_duration = std::max(0.0, duration);
    m_clips = m_duration > 0.0 ? edit::whole(m_duration) : edit::Clips{};
    m_exported.clear();
    m_undo.clear();
    m_redo.clear();
    m_gesture = false;
    emit changed();
}

void Timeline::markExported(const edit::Clips &clips) {
    m_exported = clips;
    emit changed();
}

void Timeline::apply(edit::Clips next) {
    // Mid-drag, clips keep their indices; the gesture's end normalizes.
    if (!m_gesture)
        next = edit::normalized(next, m_duration);
    if (next == m_clips || next.isEmpty())
        return;
    if (!m_gesture || !m_gestureRecorded) {
        m_undo.append(m_clips);
        m_redo.clear();
        m_gestureRecorded = m_gesture;
    }
    m_clips = next;
    emit changed();
}

int Timeline::clipAt(double t) const {
    for (int i = 0; i < m_clips.size(); ++i) {
        if (t >= m_clips[i].start && t < m_clips[i].end)
            return i;
    }
    return -1;
}

int Timeline::gapAt(double t) const {
    int n = 0;
    while (n < m_clips.size() && m_clips[n].end <= t)
        ++n;
    return n;
}

double Timeline::playableFrom(double t) const {
    // A playhead within a frame or so of a clip's end has finished it.
    for (const edit::Range &clip : m_clips) {
        if (t < clip.end - 0.03)
            return std::max(t, clip.start);
    }
    return -1.0;
}

double Timeline::edgeFrom(double t, int direction) const {
    if (m_clips.isEmpty())
        return 0.0;
    double target = direction > 0 ? m_clips.last().end : m_clips.first().start;
    for (const edit::Range &clip : m_clips) {
        for (const double edge : {clip.start, clip.end}) {
            if (direction > 0 && edge > t + 0.01)
                return edge;
            if (direction < 0 && edge < t - 0.01)
                target = edge;
        }
    }
    return target;
}

void Timeline::split(double time) {
    for (int i = 0; i < m_clips.size(); ++i) {
        const edit::Range clip = m_clips[i];
        if (time - clip.start < edit::minimumClip || clip.end - time < edit::minimumClip)
            continue;
        edit::Clips next = m_clips;
        next[i].end = time;
        next.insert(i + 1, {time, clip.end});
        apply(next);
        return;
    }
}

void Timeline::setClip(int index, double start, double end) {
    if (index < 0 || index >= m_clips.size())
        return;
    // A clip can grow into a gap, never over its neighbours.
    const double low = index > 0 ? m_clips[index - 1].end : 0.0;
    const double high = index + 1 < m_clips.size() ? m_clips[index + 1].start : m_duration;
    start = std::clamp(start, low, high);
    end = std::clamp(end, low, high);
    if (end - start < edit::minimumClip)
        return;
    edit::Clips next = m_clips;
    next[index] = {start, end};
    apply(next);
}

void Timeline::moveEdge(int index, bool start, double t) {
    if (index < 0 || index >= m_clips.size())
        return;
    const edit::Range clip = m_clips[index];
    if (start)
        setClip(index, std::min(t, clip.end - edit::minimumClip), clip.end);
    else
        setClip(index, clip.start, std::max(t, clip.start + edit::minimumClip));
}

void Timeline::trimTo(double t, bool start) {
    int i = clipAt(t);
    if (i < 0)
        i = start ? gapAt(t) : gapAt(t) - 1;
    if (i < 0 || i >= m_clips.size())
        return;
    if (start)
        setClip(i, t, m_clips[i].end);
    else
        setClip(i, m_clips[i].start, t);
}

void Timeline::removeClip(int index) {
    if (index < 0 || index >= m_clips.size() || m_clips.size() < 2)
        return;
    edit::Clips next = m_clips;
    next.removeAt(index);
    apply(next);
}

void Timeline::removeOrRestoreAt(double t) {
    const int i = clipAt(t);
    if (i >= 0)
        removeClip(i);
    else
        restoreGap(gapAt(t));
}

void Timeline::restoreGap(int gap) {
    if (m_clips.isEmpty() || gap < 0 || gap > m_clips.size())
        return;
    if (gap == 0)
        setClip(0, 0.0, m_clips[0].end);
    else if (gap == m_clips.size())
        setClip(gap - 1, m_clips[gap - 1].start, m_duration);
    else
        joinClips(gap - 1);
}

void Timeline::joinClips(int index) {
    if (index < 0 || index + 1 >= m_clips.size())
        return;
    edit::Clips next = m_clips;
    next[index].end = next[index + 1].end;
    next.removeAt(index + 1);
    apply(next);
}

void Timeline::beginGesture() {
    if (m_clips.isEmpty())
        return;
    m_gesture = true;
    m_gestureRecorded = false;
}

void Timeline::endGesture() {
    if (!m_gesture)
        return;
    m_gesture = false;
    // Normalizing can drop a clip too short to keep; never let that empty the video.
    const edit::Clips next = edit::normalized(m_clips, m_duration);
    if (!next.isEmpty() && next != m_clips) {
        m_clips = next;
        emit changed();
    }
}

void Timeline::undo() {
    if (m_gesture || m_undo.isEmpty())
        return;
    m_redo.append(m_clips);
    m_clips = m_undo.takeLast();
    emit changed();
}

void Timeline::redo() {
    if (m_gesture || m_redo.isEmpty())
        return;
    m_undo.append(m_clips);
    m_clips = m_redo.takeLast();
    emit changed();
}
