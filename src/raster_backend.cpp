// SPDX-License-Identifier: AGPL-3.0-or-later
#include "raster_backend.h"

#include <poppler-qt6.h>

#include <QCoreApplication>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QtMath>

bool export_raster_pdf(const Poppler::Document &document, const QString &output,
                       int first_page, int last_page_exclusive,
                       const QRectF &normalized_selection, int dpi,
                       QString &error)
{
    const QRectF selection = normalized_selection.normalized();
    if (first_page < 0 || last_page_exclusive > document.numPages() ||
        first_page >= last_page_exclusive || dpi < 72 ||
        selection.left() < 0 || selection.top() < 0 ||
        selection.right() > 1 || selection.bottom() > 1 ||
        selection.isEmpty()) {
        error = QCoreApplication::translate("RasterBackend", "Invalid page range or crop selection");
        return false;
    }

    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    {
        QPdfWriter writer(&file);
        writer.setResolution(dpi);
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        writer.setCreator(QStringLiteral("PDF Select Crop"));
        QPainter painter;

        for (int i = first_page; i < last_page_exclusive; ++i) {
            auto page = document.page(i);
            if (!page) {
                error = QCoreApplication::translate("RasterBackend", "Cannot read page %1").arg(i + 1);
                return false;
            }
            const QSizeF page_points = page->pageSizeF();
            const QSizeF crop_points(page_points.width() * selection.width(),
                                     page_points.height() * selection.height());
            if (!writer.setPageSize(QPageSize(crop_points, QPageSize::Point,
                                              QString(), QPageSize::ExactMatch))) {
                error = QCoreApplication::translate("RasterBackend", "Cannot set the size of page %1").arg(i + 1);
                return false;
            }
            const double pixels = page_points.width() * page_points.height() * dpi * dpi / (72.0 * 72.0);
            if (pixels > 40000000.0) {
                error = QCoreApplication::translate("RasterBackend", "Page %1 is too large at %2 DPI. Lower the DPI.")
                            .arg(i + 1).arg(dpi);
                return false;
            }
            const QImage full = page->renderToImage(dpi, dpi);
            if (full.isNull()) {
                error = QCoreApplication::translate("RasterBackend", "Cannot render page %1").arg(i + 1);
                return false;
            }
            const int left = qFloor(selection.left() * full.width());
            const int top = qFloor(selection.top() * full.height());
            const int right = qCeil(selection.right() * full.width());
            const int bottom = qCeil(selection.bottom() * full.height());
            const QRect cut(left, top, right - left, bottom - top);
            const QImage cropped = full.copy(cut.intersected(full.rect()));
            if (cropped.isNull()) {
                error = QCoreApplication::translate("RasterBackend", "The selection on page %1 is empty").arg(i + 1);
                return false;
            }

            if (i == first_page) {
                if (!painter.begin(&writer)) {
                    error = QCoreApplication::translate("RasterBackend", "Cannot create the output PDF");
                    return false;
                }
            } else if (!writer.newPage()) {
                error = QCoreApplication::translate("RasterBackend", "Cannot create output page %1").arg(i - first_page + 1);
                return false;
            }
            painter.drawImage(QRectF(0, 0, writer.width(), writer.height()), cropped);
        }
        painter.end();
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}
