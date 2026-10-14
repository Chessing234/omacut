#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <atomic>


// Thin wrappers around the ffmpeg/ffprobe command-line tools.
namespace ffmpeg {

struct VideoInfo {
    QString path;
    double duration = 0.0;  // seconds
    // As displayed: swapped for video rotated a quarter turn.
    int width = 0;
    int height = 0;
    bool audio = false;
    bool ok = false;
    QString error;
};

// Probe a file for a usable video stream and duration (runs ffprobe).
VideoInfo probe(const QString &path);

// Grab a single frame at `time` seconds, scaled to `height` px.
// Returns a null QImage on failure. If `cancel` is set and flips to true while
// the ffmpeg child is running, the child is killed and a null QImage returned.
QImage thumbnail(const QString &path, double time, int height = 90,
                 const std::atomic<bool> *cancel = nullptr);

// ffprobe arguments listing the video packet times up to `time` seconds,
// from far enough back to find the frame on screen there.
QStringList shownFrameArgs(const QString &path, double time);
// The frame on screen at `time`: the last listed packet time not after it.
// `time` itself when none is listed.
double shownFrameTime(const QByteArray &packetTimes, double time);
// ffmpeg arguments that write the frame starting at `frameTime` seconds to
// `pngPath`, full size, upright and with square pixels.
QStringList frameArgs(const QString &path, double frameTime, const QString &pngPath);

// A stretch [start, end] seconds of one video, as exported.
struct Segment {
    QString path;
    double start = 0.0;
    double end = 0.0;
    bool audio = false;
};

// Build the ffmpeg argument list that writes the segments, back to back, to
// dst. Cuts are frame-accurate and re-encoded with libx264/aac. Segments from
// different videos are fitted into width x height, letterboxed, with silence
// for any without audio. A non-zero scaleHeight downscales so the shorter
// side becomes scaleHeight (1080p of a portrait video is 1080 wide), always
// preserving the aspect ratio.
QStringList trimArgs(const QList<Segment> &segments, const QString &dst,
                     int width, int height, int scaleHeight = 0);

// Locate a tool on PATH; returns empty string if missing.
QString toolPath(const QString &tool);

}  // namespace ffmpeg
