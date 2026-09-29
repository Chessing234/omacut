#pragma once

#include <QObject>
#include <QVariantList>

#include "edit.h"

// The editable clips of the loaded video, with undo. A gesture (a drag) is one
// undo step. Shared by the real backend and the QML tests' stand-in, so both
// edit the same way.
class Timeline : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList clips READ clipList NOTIFY changed)
    Q_PROPERTY(double keptDuration READ keptDuration NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    // Cut from the source, and not exported as it stands.
    Q_PROPERTY(bool unexported READ unexported NOTIFY changed)

public:
    explicit Timeline(QObject *parent = nullptr) : QObject(parent) {}

    edit::Clips clips() const { return m_clips; }
    QVariantList clipList() const;
    double keptDuration() const { return edit::keptDuration(m_clips); }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool unexported() const;

    // A freshly loaded video: one clip spanning all of it, and no history.
    void reset(double duration);
    void markExported(const edit::Clips &clips);

    // The clip containing t, or -1 in a gap.
    Q_INVOKABLE int clipAt(double t) const;
    // The gap before clip n; the clip count is the tail after the last clip.
    Q_INVOKABLE int gapAt(double t) const;
    // Where playback continues from t: t itself inside a clip, else the next
    // clip's start; -1 past the last clip.
    Q_INVOKABLE double playableFrom(double t) const;
    // The nearest clip edge before (direction < 0) or after t, else the first
    // or last edge.
    Q_INVOKABLE double edgeFrom(double t, int direction) const;

    Q_INVOKABLE void split(double time);
    Q_INVOKABLE void setClip(int index, double start, double end);
    // Moves one edge of a clip toward t, stopping a minimum clip short of the other.
    Q_INVOKABLE void moveEdge(int index, bool start, double t);
    // Moves an edge of the clip under t to it; in a gap, the neighbouring clip grows.
    Q_INVOKABLE void trimTo(double t, bool start);
    Q_INVOKABLE void removeClip(int index);
    // Removes the clip under t; in a gap, restores it.
    Q_INVOKABLE void removeOrRestoreAt(double t);
    Q_INVOKABLE void restoreGap(int gap);
    // Joins clip index with the next one, restoring whatever lay between them.
    Q_INVOKABLE void joinClips(int index);
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    void changed();

private:
    void apply(edit::Clips next);

    double m_duration = 0.0;
    edit::Clips m_clips;
    edit::Clips m_exported;
    QList<edit::Clips> m_undo;
    QList<edit::Clips> m_redo;
    bool m_gesture = false;
    bool m_gestureRecorded = false;
};
