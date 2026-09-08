#include "circular_image.h"

#include <QImageReader>
#include <QPainter>
#include <QPainterPath>

CircularImage::CircularImage(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setOpaquePainting(false);
}

QUrl CircularImage::source() const { return m_source; }

void CircularImage::setSource(const QUrl &source) {
    if (m_source == source) {
        reload();
        return;
    }
    m_source = source;
    reload();
    emit sourceChanged();
}

void CircularImage::reload() {
    m_image = QImage();
    if (!m_source.isEmpty()) {
        QImageReader reader(m_source.isLocalFile() ? m_source.toLocalFile() : m_source.toString());
        m_image = reader.read();
    }
    update();
}

void CircularImage::paint(QPainter *painter) {
    if (m_image.isNull() || width() <= 0 || height() <= 0) return;

    painter->setRenderHint(QPainter::Antialiasing);
    QPainterPath circle;
    circle.addEllipse(boundingRect());
    painter->save();
    painter->setClipPath(circle);

    const QSize targetSize = QSizeF(width(), height()).toSize();
    const QImage scaled = m_image.scaled(targetSize, Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
    const QPointF offset((width() - scaled.width()) / 2.0,
                         (height() - scaled.height()) / 2.0);
    painter->drawImage(offset, scaled);
    painter->restore();
}
