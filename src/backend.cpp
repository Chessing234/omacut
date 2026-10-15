#include "backend.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QTextStream>
#include <QVariantMap>

#include <cstdio>
#include <memory>
#include <utility>

#include "filepicker.h"
#include "portalfilepicker.h"
#include "thumbprovider.h"

namespace {
const QString kDefaultAccent = QStringLiteral("#FFD60A");

QString omarchyCurrentDir() {
    return QDir::homePath() + QStringLiteral("/.local/state/omarchy/current");
}

QString omarchyColorsPath() {
    return omarchyCurrentDir() + QStringLiteral("/theme/colors.toml");
}

QString mp4PathFor(const QString &path) {
    const QFileInfo file(path);
    if (file.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0)
        return path;

    const QString baseName = file.completeBaseName().isEmpty()
        ? file.fileName()
        : file.completeBaseName();
    return file.dir().filePath(baseName + QStringLiteral(".mp4"));
}

bool replaceWithTemp(const QString &tmpPath, const QString &outPath) {
    const QByteArray tmpName = QFile::encodeName(tmpPath);
    const QByteArray outName = QFile::encodeName(outPath);
    return std::rename(tmpName.constData(), outName.constData()) == 0;
}
}

Backend::Backend(ThumbProvider *provider, QObject *parent)
    : Backend(provider, new PortalFilePicker(), parent) {}

Backend::Backend(ThumbProvider *provider, FilePicker *filePicker, QObject *parent)
    : QObject(parent), m_provider(provider), m_filePicker(filePicker),
      m_themeAccent(kDefaultAccent) {
    if (!m_filePicker->parent())
        m_filePicker->setParent(this);
    wireFilePicker();

    // Follow omarchy theme switches live. The theme lives behind a symlink that
    // gets swapped, so the reload also re-arms the watch paths every time.
    const auto themeChanged = [this] {
        watchTheme();
        loadThemeAccent();
    };
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, themeChanged);
    connect(&m_timeline, &Timeline::changed, this, &Backend::saveSidecar);
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, themeChanged);
    watchTheme();
    loadThemeAccent();
}

Backend::~Backend() = default;

void Backend::wireFilePicker() {
    connect(m_filePicker, &FilePicker::openSelected, this, [this](const QUrl &url) {
        if (std::exchange(m_adding, false))
            addVideo(url, m_addAt);
        else
            load(url);
    });
    connect(m_filePicker, &FilePicker::exportSelected, this, [this](const QUrl &url, int scaleHeight) {
        exportClips(url, m_exportDialogClips, scaleHeight);
    });
    connect(m_filePicker, &FilePicker::failed, this, &Backend::loadError);
}

void Backend::setBusy(bool busy) {
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

QString Backend::accentFromColorsFile(const QString &path, const QString &fallback) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return fallback;

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || line.left(equals).trimmed() != QStringLiteral("accent"))
            continue;

        QString value = line.mid(equals + 1).trimmed();

        // A value can carry a trailing comment. Inside quotes the '#' belongs
        // to the string; outside them it starts one -- except the leading '#'
        // of a bare hex colour, which is the value itself.
        if (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))) {
            const QChar quote = value.front();
            const int closing = value.indexOf(quote, 1);
            value = closing > 0 ? value.mid(1, closing - 1) : value.mid(1);
        } else {
            const int comment =
                value.indexOf(QLatin1Char('#'), value.startsWith(QLatin1Char('#')) ? 1 : 0);
            if (comment >= 0)
                value = value.left(comment).trimmed();
        }

        return QColor::fromString(value).isValid() ? value : fallback;
    }
    return fallback;
}

QString Backend::foregroundFor(const QString &color) {
    const QColor parsed = QColor::fromString(color);
    if (!parsed.isValid())
        return QStringLiteral("black");
    const double luminance = 0.299 * parsed.redF()
        + 0.587 * parsed.greenF() + 0.114 * parsed.blueF();
    return luminance < 0.5 ? QStringLiteral("white") : QStringLiteral("black");
}

QString Backend::themeAccentForeground() const {
    return foregroundFor(m_themeAccent);
}

void Backend::loadThemeAccent() {
    const QString accent = accentFromColorsFile(omarchyColorsPath(), kDefaultAccent);
    if (accent == m_themeAccent)
        return;
    m_themeAccent = accent;
    emit themeAccentChanged();
}

void Backend::watchTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = omarchyCurrentDir();
    const QString themeDir = currentDir + QStringLiteral("/theme");
    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFileInfo::exists(omarchyColorsPath()))
        m_themeWatcher.addPath(omarchyColorsPath());
}

bool Backend::isVideoPath(const QString &path) const {
    const QString target = QFileInfo(path).canonicalFilePath();
    for (const Video &video : m_videos) {
        if (QFileInfo(video.info.path).canonicalFilePath() == target)
            return true;
    }
    return false;
}

QVariantList Backend::videoList() const {
    QVariantList result;
    for (const Video &video : m_videos)
        result.append(QVariantMap{{QStringLiteral("url"), video.url},
                                  {QStringLiteral("thumbKey"), video.thumbKey}});
    return result;
}

bool Backend::probeVideo(const QUrl &url, Video *video) {
    // Only absolute local paths reach ffprobe: "file:-report" would be read
    // as an option, and "file:http://..." as a network address.
    const QString path = url.toLocalFile();
    if (!url.isLocalFile() || !QDir::isAbsolutePath(path)) {
        emit loadError(QStringLiteral("Only local files can be opened."));
        return false;
    }
    video->info = ffmpeg::probe(path);
    if (!video->info.ok) {
        emit loadError(video->info.error);
        return false;
    }
    video->url = url;
    video->thumbKey = ++m_nextThumbKey;
    m_provider->setVideo(video->thumbKey, video->info.path);
    return true;
}

bool Backend::load(const QUrl &url) {
    // A running export marks its clips exported when it finishes, which
    // must not land on a different set of videos.
    if (m_busy)
        return false;

    Video video;
    if (!probeVideo(url, &video))
        return false;

    QByteArray savedEdit;
    QFile sidecar(sidecarPath(video.info.path));
    if (sidecar.open(QIODevice::ReadOnly))
        savedEdit = sidecar.readAll();

    m_videos = {video};
    m_timeline.reset(video.info.duration);
    edit::Clips restored;
    if (clipsFromSidecar(savedEdit, video.info.path, video.info.duration, &restored))
        m_timeline.replace(restored);
    emit videosChanged();
    emit infoChanged();
    return true;
}

bool Backend::addVideo(const QUrl &url, double t) {
    if (m_busy)
        return false;
    if (m_videos.isEmpty())
        return load(url);

    Video video;
    if (!probeVideo(url, &video))
        return false;

    m_videos.append(video);
    emit videosChanged();
    m_timeline.addSource(video.info.duration, t);
    return true;
}

void Backend::openVideoDialog() {
    m_adding = false;
    m_filePicker->openVideo();
}

void Backend::addVideoDialog(double t) {
    m_adding = !m_videos.isEmpty();
    m_addAt = t;
    m_filePicker->openVideo();
}

void Backend::exportDialog() {
    if (m_videos.isEmpty())
        return;

    m_exportDialogClips = m_timeline.clips();
    const ffmpeg::VideoInfo &frame = m_videos.value(m_exportDialogClips.value(0).source).info;
    m_filePicker->exportVideo(suggestedExportUrl(), exportHeights(frame.width, frame.height));
}

QList<int> Backend::exportHeights(int width, int height) {
    const int shortSide = qMin(width, height);
    QList<int> heights;
    for (const int candidate : {1080, 720}) {
        if (shortSide > candidate)
            heights << candidate;
    }
    return heights;
}

QString Backend::sidecarPath(const QString &videoPath) {
    const QFileInfo file(videoPath);
    return file.dir().filePath(file.completeBaseName() + QStringLiteral(".omacut.json"));
}

QByteArray Backend::sidecarJson(const QString &source, const edit::Clips &clips) {
    QJsonArray stored;
    for (const edit::Clip &clip : clips) {
        stored.append(QJsonObject{{QStringLiteral("source"), clip.source},
                                  {QStringLiteral("in"), clip.in},
                                  {QStringLiteral("out"), clip.out}});
    }
    const QJsonObject root{{QStringLiteral("version"), 1},
                           {QStringLiteral("source"), source},
                           {QStringLiteral("clips"), stored}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool Backend::clipsFromSidecar(const QByteArray &data, const QString &source, double duration,
                               edit::Clips *clips) {
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    if (root.value(QStringLiteral("version")).toInt() != 1
            || root.value(QStringLiteral("source")).toString() != source)
        return false;
    edit::Clips parsed;
    for (const QJsonValue &value : root.value(QStringLiteral("clips")).toArray()) {
        const QJsonObject clip = value.toObject();
        if (clip.value(QStringLiteral("source")).toInt() != 0)
            return false;
        const double in = clip.value(QStringLiteral("in")).toDouble();
        const double out = clip.value(QStringLiteral("out")).toDouble();
        if (!(out - in >= edit::minimumClip) || in < 0 || out > duration + 0.001)
            return false;
        parsed.append({0, in, out});
    }
    if (parsed.isEmpty())
        return false;
    *clips = parsed;
    return true;
}

void Backend::saveSidecar() {
    if (m_videos.size() != 1)
        return;
    const QString path = sidecarPath(m_videos.first().info.path);
    if (!m_timeline.unexported()) {
        QFile::remove(path);
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(sidecarJson(m_videos.first().info.path, m_timeline.clips()));
    file.commit();
}

QUrl Backend::suggestedExportUrl() const {
    if (m_videos.isEmpty())
        return {};
    const QFileInfo src(m_videos.first().info.path);
    const QString target = src.dir().filePath(src.completeBaseName() + "_trimmed.mp4");
    return QUrl::fromLocalFile(target);
}

void Backend::exportClips(const QUrl &dst, const edit::Clips &clips, int scaleHeight) {
    if (m_videos.isEmpty() || m_busy)
        return;

    QList<ffmpeg::Segment> segments;
    // The first clip that still has its video sets the frame.
    int frameSource = -1;
    for (const edit::Clip &clip : edit::merged(clips)) {
        if (clip.source < 0 || clip.source >= m_videos.size())
            continue;
        if (frameSource < 0)
            frameSource = clip.source;
        const ffmpeg::VideoInfo &info = m_videos[clip.source].info;
        segments.append({info.path, clip.in, clip.out, info.audio});
    }
    const double clipLen = edit::duration(clips);
    if (segments.isEmpty() || clipLen <= 0.0) {
        emit exportFailed("The selected clip has no length.");
        return;
    }

    // Forcing the .mp4 suffix can redirect the write to a file the save
    // dialog never asked the user about overwriting — refuse rather than
    // silently replace it.
    const QString selectedPath = dst.toLocalFile();
    const QString outPath = mp4PathFor(selectedPath);
    if (outPath != selectedPath && QFileInfo::exists(outPath)) {
        emit exportFailed(QStringLiteral("%1 already exists.")
                              .arg(QFileInfo(outPath).fileName()));
        return;
    }

    const QString ffmpegBin = ffmpeg::toolPath("ffmpeg");
    if (ffmpegBin.isEmpty()) {
        emit exportFailed("`ffmpeg` was not found on your PATH.");
        return;
    }

    setBusy(true);
    setStatus(QStringLiteral("Exporting 0%"));

    // Encode to a sibling temp file and atomically replace the target only after
    // success, so failed/cancelled exports preserve any existing file.
    const QString tmpPath = outPath + QStringLiteral(".omacut-part.mp4");
    QFile::remove(tmpPath);
    const ffmpeg::VideoInfo &frame = m_videos[frameSource].info;
    const QStringList args = ffmpeg::trimArgs(segments, tmpPath, frame.width, frame.height, scaleHeight);

    auto *proc = new QProcess(this);
    auto completed = std::make_shared<bool>(false);

    // ffmpeg -progress writes key=value blocks to stdout as it encodes;
    // out_time_us against the kept length gives the percentage.
    auto progressBuf = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this, proc, progressBuf, clipLen, completed] {
                progressBuf->append(proc->readAllStandardOutput());
                int newline;
                while ((newline = progressBuf->indexOf('\n')) >= 0) {
                    const QByteArray line = progressBuf->left(newline).trimmed();
                    progressBuf->remove(0, newline + 1);
                    if (*completed || !line.startsWith("out_time_us="))
                        continue;
                    bool ok = false;
                    const double outSecs = line.mid(line.indexOf('=') + 1).toLongLong(&ok) / 1e6;
                    if (!ok)
                        continue;
                    const int percent = qBound(0, qRound(outSecs / clipLen * 100.0), 100);
                    setStatus(QStringLiteral("Exporting %1%").arg(percent));
                }
            });

    connect(proc, &QProcess::finished, this,
            [this, proc, outPath, tmpPath, completed, clips](int code, QProcess::ExitStatus exitStatus) {
                if (*completed)
                    return;
                *completed = true;
                const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
                proc->deleteLater();
                if (exitStatus != QProcess::NormalExit || code != 0) {
                    failExport(tmpPath, err.isEmpty() ? QStringLiteral("ffmpeg trim failed.") : err);
                    return;
                }
                if (!replaceWithTemp(tmpPath, outPath)) {
                    failExport(tmpPath, QStringLiteral("Could not write the exported file."));
                    return;
                }
                setBusy(false);
                setStatus(QString());
                // Written over one of its own videos: the clips' times point
                // into a file that's gone, so start over from what was saved.
                if (!isVideoPath(outPath)) {
                    m_timeline.markExported(clips);
                } else if (!load(QUrl::fromLocalFile(outPath))) {
                    emit exportDone(outPath);
                    emit loadError(QStringLiteral("Saved, but couldn't reopen it, so the clips no longer match the file."));
                    return;
                }
                emit exportDone(outPath);
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, tmpPath, completed](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *completed)
                    return;
                *completed = true;
                const QString err = proc->errorString();
                proc->deleteLater();
                failExport(tmpPath, err.isEmpty() ? QStringLiteral("Could not start ffmpeg.") : err);
            });
    proc->start(ffmpegBin, args);
}

void Backend::failExport(const QString &tmpPath, const QString &message) {
    setBusy(false);
    setStatus(QString());
    QFile::remove(tmpPath);
    emit exportFailed(message);
}
