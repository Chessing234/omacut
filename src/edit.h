#pragma once

#include <QList>

// What survives of a video: an ordered list of clips from the source, which is
// never modified. Neighbouring clips may touch (a split with nothing removed);
// the gaps between them are what gets cut. Times are seconds in the source.
namespace edit {

struct Range {
    double start = 0.0;
    double end = 0.0;
    double length() const { return end - start; }
    bool operator==(const Range &other) const {
        return start == other.start && end == other.end;
    }
};
using Clips = QList<Range>;

constexpr double minimumClip = 0.1;
// Gaps narrower than this close up, so a handle dropped by a neighbour joins it.
constexpr double minimumGap = 0.05;

Clips whole(double duration);
// Clamps into the video, sorts, resolves overlaps, closes hairline gaps, and
// drops clips too short to keep.
Clips normalized(Clips clips, double duration);
// The clips with touching neighbours merged: what an export encodes.
QList<Range> kept(const Clips &clips);
double keptDuration(const Clips &clips);
bool untouched(const Clips &clips, double duration);

}  // namespace edit
