#pragma once

#include <QQuickPaintedItem>
#include <QImage>
#include <QUrl>

#include <QtQml/qqmlregistration.h>

/**
 * 绘制一个真正的圆形头像，避免 QML Rectangle 的矩形 clip 露出图片四角。
 */
class CircularImage : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit CircularImage(QQuickItem *parent = nullptr);

    QUrl source() const;
    void setSource(const QUrl &source);

    void paint(QPainter *painter) override;

signals:
    void sourceChanged();

private:
    void reload();

    QUrl m_source;
    QImage m_image;
};
