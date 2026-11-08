#pragma once
#include <QImage>
#include <QPainter>
#include <QPainterPath>

inline QImage sourceInkCoverage(const QPainterPath& scaled, const QRect& crop) {
    QImage image(crop.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(-crop.topLeft());
    painter.fillPath(scaled, Qt::black);
    painter.end();
    return image;
}

// The proposal raster uses opacity 99 for #231f20 source ink on white at threshold 170.
template<class EmitSpan>
void visitSourceInk(const QPainterPath& scaled, const QRect& crop, EmitSpan emitSpan) {
    if (crop.isEmpty()) return;
    const auto image = sourceInkCoverage(scaled, crop);
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width();) {
            while (x < image.width() && qAlpha(row[x]) < 99) ++x;
            const int start = x;
            while (x < image.width() && qAlpha(row[x]) >= 99) ++x;
            if (x > start) emitSpan(crop.x() + start, crop.y() + y, x - start);
        }
    }
}
