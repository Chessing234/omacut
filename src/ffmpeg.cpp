#include "ffmpeg.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

namespace ffmpeg {

namespace {
// Upper bound on a synchronous probe so a hung/unresponsive file (e.g. a stalled
// network mount) can't freeze the caller — load() runs probe() on the UI thread.
constexpr int kProbeTimeoutMs = 15000;
// Poll granularity while waiting on a thumbnail child, so cancellation is prompt.
constexpr int kThumbPollMs = 50;
}

QString toolPath(const QString &tool) {
    return QStandardPaths::findExecutable(tool);
}

VideoInfo probe(const QString &path) {
    VideoInfo info;
    info.path = path;

    const QString ffprobe = toolPath("ffprobe");
    if (ffprobe.isEmpty()) {
        info.error = "`ffprobe` was not found on your PATH. Install ffmpeg.";
        return info;
    }

    QProcess proc;
    proc.start(ffprobe, {
        "-v", "error",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        path,
    });
    if (!proc.waitForFinished(kProbeTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(-1);
        info.error = "ffprobe timed out reading this file.";
        return info;
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        info.error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (info.error.isEmpty())
            info.error = "ffprobe failed.";
        return info;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
    const QJsonObject root = doc.object();
    QJsonObject stream;
    for (const QJsonValue &value : root.value("streams").toArray()) {
        const QJsonObject candidate = value.toObject();
        const QString type = candidate.value("codec_type").toString();
        if (type == "video" && stream.isEmpty())
            stream = candidate;
        else if (type == "audio")
            info.audio = true;
    }
    if (stream.isEmpty()) {
        info.error = "No video stream found in this file.";
        return info;
    }

    info.width = stream.value("width").toInt();
    info.height = stream.value("height").toInt();
    // Phones record sideways and say how to turn it upright, in a display
    // matrix or the older rotate tag.
    int rotation = stream.value("tags").toObject().value("rotate").toString().toInt();
    for (const QJsonValue &sideData : stream.value("side_data_list").toArray()) {
        if (sideData.toObject().contains("rotation"))
            rotation = sideData.toObject().value("rotation").toInt();
    }
    // Sized as displayed: widened for non-square pixels, like PAL's 16:15.
    const QStringList sar = stream.value("sample_aspect_ratio").toString().split(':');
    if (sar.size() == 2 && sar[0].toInt() > 0 && sar[1].toInt() > 0)
        info.width = qRound(double(info.width) * sar[0].toInt() / sar[1].toInt());
    if (qAbs(rotation) % 180 == 90)
        std::swap(info.width, info.height);

    // Duration can live on the stream or on the container.
    QString durationStr = stream.value("duration").toString();
    if (durationStr.isEmpty())
        durationStr = root.value("format").toObject().value("duration").toString();
    if (durationStr.isEmpty()) {
        info.error = "Could not determine the video duration.";
        return info;
    }

    info.duration = durationStr.toDouble();

    info.ok = info.duration > 0.0;
    if (!info.ok)
        info.error = "Video has a zero or invalid duration.";
    return info;
}

QImage thumbnail(const QString &path, double time, int height,
                 const std::atomic<bool> *cancel) {
    const QString ffmpeg = toolPath("ffmpeg");
    if (ffmpeg.isEmpty())
        return {};

    QProcess proc;
    proc.start(ffmpeg, {
        "-loglevel", "error",
        "-ss", QString::number(qMax(time, 0.0), 'f', 3),
        "-i", path,
        "-frames:v", "1",
        "-vf", QString("scale=-1:%1").arg(height),
        "-f", "image2pipe",
        "-vcodec", "mjpeg",
        "pipe:1",
    });

    // Poll instead of waitForFinished(-1) so a cancel request can kill the child
    // promptly — otherwise the std::future destructor in ThumbWorker would block
    // the UI thread until ffmpeg finishes on its own.
    while (!proc.waitForFinished(kThumbPollMs)) {
        if (proc.state() == QProcess::NotRunning)
            break;  // failed to start, or exited between polls
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            proc.kill();
            proc.waitForFinished(-1);
            return {};
        }
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};

    const QByteArray data = proc.readAllStandardOutput();
    QImage img;
    img.loadFromData(data, "JPEG");
    return img;
}

QStringList trimArgs(const QList<Segment> &segments, const QString &dst,
                     int width, int height, int scaleHeight) {
    bool audio = false;
    bool mixed = false;
    for (const Segment &segment : segments) {
        audio = audio || segment.audio;
        mixed = mixed || segment.path != segments.first().path;
    }

    // Machine-readable progress on stdout (errors stay on stderr), so the UI
    // can show how far along the encode is.
    QStringList args = {"-y", "-loglevel", "error", "-progress", "pipe:1"};
    // One input per segment: -ss before -i seeks fast and -t bounds it, so
    // only what survives the cut is decoded. The concat filter joins them.
    QString graph;
    QString inputs;
    for (int i = 0; i < segments.size(); ++i) {
        const Segment &segment = segments[i];
        const QString length = QString::number(qMax(segment.end - segment.start, 0.0), 'f', 3);
        args << "-ss" << QString::number(segment.start, 'f', 3) << "-t" << length
             << "-i" << segment.path;
        if (!mixed) {
            inputs += QString("[%1:v:0]").arg(i) + (audio ? QString("[%1:a:0]").arg(i) : QString());
            continue;
        }
        // Concat needs one frame size and one audio format throughout. Pixels
        // are squared first, so non-square video keeps its shape; -1 centres
        // the padding; libx264 needs even sides.
        graph += QString("[%1:v:0]scale='trunc(iw*sar/2)*2':ih,setsar=1,"
                         "scale=%2:%3:force_original_aspect_ratio=decrease,"
                         "pad=%2:%3:-1:-1,setsar=1,format=yuv420p[v%1];")
                     .arg(i).arg(width / 2 * 2).arg(height / 2 * 2);
        if (segment.audio)
            graph += QString("[%1:a:0]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[a%1];").arg(i);
        else if (audio)
            graph += QString("anullsrc=r=48000:cl=stereo,atrim=duration=%2,aformat=sample_fmts=fltp[a%1];")
                         .arg(i).arg(length);
        inputs += QString("[v%1]").arg(i) + (audio ? QString("[a%1]").arg(i) : QString());
    }
    graph += inputs + QString("concat=n=%1:v=1:a=%2").arg(segments.size()).arg(audio ? 1 : 0);
    // Cap the shorter side, judged on the decoded (rotation-applied) frame, so
    // portrait and landscape both keep their aspect ratio. -2 keeps the other
    // side divisible by two, which libx264 requires.
    if (scaleHeight > 0)
        graph += QString("[joined]%1;[joined]scale='if(gt(iw,ih),-2,%2)':'if(gt(iw,ih),%2,-2)'[v]")
                     .arg(audio ? "[a]" : "").arg(scaleHeight);
    else
        graph += QString("[v]") + (audio ? "[a]" : "");
    args << "-filter_complex" << graph << "-map" << "[v]";
    if (audio)
        args << "-map" << "[a]" << "-c:a" << "aac";
    // +faststart puts the moov atom up front so shared clips start playing
    // before they finish downloading.
    args << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "18"
         << "-movflags" << "+faststart"
         << dst;
    return args;
}

}  // namespace ffmpeg
