// SPDX-License-Identifier: AGPL-3.0-or-later
#include "raster_backend.h"

#include <poppler-qt6.h>

#include <QCoreApplication>
#include <QImage>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTransform>
#include <QtMath>

bool export_raster_pdf(const Poppler::Document &document, const QString &output,
                       const QVector<CropJob> &jobs, int dpi, int rotation,
                       QString &error)
{
    if (jobs.isEmpty() || dpi < 72 || rotation < 0 || rotation > 270 || rotation % 90) {
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

        for (int i = 0; i < jobs.size(); ++i) {
            const CropJob &job = jobs.at(i);
            const QRectF selection = job.selection.normalized();
            if (job.pageIndex < 0 || job.pageIndex >= document.numPages() ||
                selection.left() < 0 || selection.top() < 0 ||
                selection.right() > 1 || selection.bottom() > 1 || selection.isEmpty()) {
                error = QCoreApplication::translate("RasterBackend", "Invalid page range or crop selection");
                return false;
            }
            auto page = document.page(job.pageIndex);
            if (!page) {
                error = QCoreApplication::translate("RasterBackend", "Cannot read page %1").arg(job.pageIndex + 1);
                return false;
            }
            const QSizeF page_points = page->pageSizeF();
            QSizeF crop_points(page_points.width() * selection.width(),
                               page_points.height() * selection.height());
            if (rotation % 180) crop_points.transpose();
            if (!writer.setPageSize(QPageSize(crop_points, QPageSize::Point,
                                              QString(), QPageSize::ExactMatch))) {
                error = QCoreApplication::translate("RasterBackend", "Cannot set the size of page %1").arg(job.pageIndex + 1);
                return false;
            }
            const double pixels = crop_points.width() * crop_points.height() * dpi * dpi / (72.0 * 72.0);
            if (pixels > 40000000.0) {
                error = QCoreApplication::translate("RasterBackend", "Page %1 is too large at %2 DPI. Lower the DPI.")
                            .arg(job.pageIndex + 1).arg(dpi);
                return false;
            }
            const int fullWidth = qRound(page_points.width() * dpi / 72.0);
            const int fullHeight = qRound(page_points.height() * dpi / 72.0);
            const int left = qFloor(selection.left() * fullWidth);
            const int top = qFloor(selection.top() * fullHeight);
            const int right = qCeil(selection.right() * fullWidth);
            const int bottom = qCeil(selection.bottom() * fullHeight);
            QImage cropped = page->renderToImage(dpi, dpi, left, top,
                                                  right - left, bottom - top);
            if (cropped.isNull()) {
                error = QCoreApplication::translate("RasterBackend", "Cannot render selection on page %1").arg(job.pageIndex + 1);
                return false;
            }
            if (rotation) cropped = cropped.transformed(QTransform().rotate(rotation));

            if (i == 0) {
                if (!painter.begin(&writer)) {
                    error = QCoreApplication::translate("RasterBackend", "Cannot create the output PDF");
                    return false;
                }
            } else if (!writer.newPage()) {
                error = QCoreApplication::translate("RasterBackend", "Cannot create output page %1").arg(i + 1);
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
