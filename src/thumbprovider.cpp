#include "thumbprovider.h"

#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QRunnable>

#include <atomic>

#include "ffmpeg.h"

namespace {
constexpr int kDefaultHeight = 90;
// Enough frames for a long zoomed filmstrip, at roughly 40 KB each.
constexpr int kCacheFrames = 600;

class ThumbResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbResponse(ThumbProvider *provider, QString cacheId, QString path, double time, int height)
        : m_provider(provider), m_cacheId(std::move(cacheId)), m_path(std::move(path)),
          m_time(time), m_height(height) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    // A frame no longer on screen is skipped if it hasn't started. One already
    // being extracted finishes into the cache: the next edit likely wants it.
    // The engine still waits for finished() before deleting a cancelled response.
    void cancel() override { m_cancelled = true; }

    void run() override {
        if (!m_cancelled && !m_path.isEmpty()) {
            m_image = ffmpeg::thumbnail(m_path, m_time, m_height, m_provider->stopping());
            if (!m_image.isNull())
                m_provider->store(m_cacheId, m_image);
        }
        emit finished();
    }

private:
    ThumbProvider *m_provider;
    QString m_cacheId;
    QString m_path;
    double m_time;
    int m_height;
    QImage m_image;
    std::atomic<bool> m_cancelled = false;
};

// A cache hit answers at once, without touching the pool.
class ReadyResponse : public QQuickImageResponse {
public:
    explicit ReadyResponse(QImage image) : m_image(std::move(image)) {
        QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
    }
    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QImage m_image;
};
}  // namespace

ThumbProvider::ThumbProvider() : m_cache(kCacheFrames) {
    // Each job is an ffmpeg process; a few at a time keeps the machine responsive.
    m_pool.setMaxThreadCount(3);
}

ThumbProvider::~ThumbProvider() {
    m_stopping = true;
    m_pool.clear();
    m_pool.waitForDone();
}

void ThumbProvider::setVideo(int key, const QString &path) {
    QMutexLocker lock(&m_mutex);
    m_paths.insert(key, path);
}

QImage ThumbProvider::cached(const QString &id) const {
    QMutexLocker lock(&m_mutex);
    const QImage *image = m_cache.object(id);
    return image ? *image : QImage();
}

void ThumbProvider::store(const QString &id, const QImage &image) {
    QMutexLocker lock(&m_mutex);
    m_cache.insert(id, new QImage(image));
}

QQuickImageResponse *ThumbProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // id looks like "<video key>/<milliseconds>".
    const int key = id.section('/', 0, 0).toInt();
    const double time = id.section('/', 1, 1).toLongLong() / 1000.0;
    const int height = requestedSize.height() > 0 ? requestedSize.height() : kDefaultHeight;
    const QString cacheId = id + QLatin1Char('@') + QString::number(height);

    const QImage hit = cached(cacheId);
    if (!hit.isNull())
        return new ReadyResponse(hit);

    QString path;
    {
        QMutexLocker lock(&m_mutex);
        path = m_paths.value(key);
    }
    auto *response = new ThumbResponse(this, cacheId, path, time, height);
    m_pool.start(response);
    return response;
}
