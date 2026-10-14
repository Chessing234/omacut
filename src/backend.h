#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include "ffmpeg.h"
#include "timeline.h"

class ThumbProvider;
class FilePicker;

// The bridge between QML and the ffmpeg/ffprobe layer. Holds the loaded
// videos and the clips cut from them, and drives export.
class Backend : public QObject {
    Q_OBJECT
    // The first video, which names the project and sets the export's frame.
    Q_PROPERTY(QUrl source READ source NOTIFY infoChanged)
    // Each video a clip can come from, by source index: {url, thumbKey}.
    Q_PROPERTY(QVariantList videos READ videoList NOTIFY videosChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeAccentChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeAccentChanged)
    Q_PROPERTY(QObject *timeline READ timeline CONSTANT)

public:
    explicit Backend(ThumbProvider *provider, QObject *parent = nullptr);
    explicit Backend(ThumbProvider *provider, FilePicker *filePicker,
                     QObject *parent = nullptr);
    ~Backend() override;

    QUrl source() const { return m_videos.isEmpty() ? QUrl() : m_videos.first().url; }
    QVariantList videoList() const;
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeAccentForeground() const;
    Timeline *timeline() { return &m_timeline; }

    // The accent from an omarchy colors.toml, or the fallback when the file is
    // missing or holds no usable accent — which is what keeps omacut working on
    // distros without omarchy themes.
    static QString accentFromColorsFile(const QString &path, const QString &fallback);
    // "black" or "white", whichever stays legible on the given color.
    static QString foregroundFor(const QString &color);

    // Start over with a video.
    Q_INVOKABLE bool load(const QUrl &url);
    // Add a video as a clip after the one under t, sequence seconds.
    Q_INVOKABLE bool addVideo(const QUrl &url, double t);

    // Open native desktop file dialogs.
    Q_INVOKABLE void openVideoDialog();
    Q_INVOKABLE void addVideoDialog(double t);
    // Exports the clips as they are when the dialog opens.
    Q_INVOKABLE void exportDialog();

    // Suggested "<name>_trimmed.mp4" target next to the source.
    Q_INVOKABLE QUrl suggestedExportUrl() const;

    // Write the clips to dst. A non-zero scaleHeight downscales the shorter
    // side to that size.
    void exportClips(const QUrl &dst, const edit::Clips &clips, int scaleHeight = 0);

    // The downscale heights worth offering for a source: only ones strictly
    // below the source's shorter side, so exports never upscale.
    static QList<int> exportHeights(int width, int height);

signals:
    void infoChanged();
    void videosChanged();
    void busyChanged();
    void statusChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void loadError(const QString &message);

private:
    void setBusy(bool busy);
    void setStatus(const QString &status);
    void failExport(const QString &tmpPath, const QString &message);
    void wireFilePicker();
    void loadThemeAccent();
    void watchTheme();

    struct Video {
        ffmpeg::VideoInfo info;
        QUrl url;
        int thumbKey = 0;
    };
    // Probes the video and registers its frames with the thumbnail provider.
    bool probeVideo(const QUrl &url, Video *video);

    ThumbProvider *m_provider;
    FilePicker *m_filePicker;
    Timeline m_timeline;
    QList<Video> m_videos;
    int m_nextThumbKey = 0;
    // The open dialog adds a video at this sequence time, rather than starting over.
    bool m_adding = false;
    double m_addAt = 0.0;
    edit::Clips m_exportDialogClips;
    bool m_busy = false;
    QString m_status;
    QString m_themeAccent;
    QFileSystemWatcher m_themeWatcher;
};
