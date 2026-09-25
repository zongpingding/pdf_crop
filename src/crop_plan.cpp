// SPDX-License-Identifier: AGPL-3.0-or-later
#include "crop_plan.h"

#include <poppler-qt6.h>

#include <QImage>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

bool parsePageRange(const QString &expression, int pageCount,
                    QVector<int> &pages, QString &error) {
    pages.clear();
    if (pageCount < 1 || expression.trimmed().isEmpty()) {
        error = QObject::tr("Enter a page range such as 1-5,8,2x+1.");
        return false;
    }
    QSet<int> selected;
    const QRegularExpression progression(QStringLiteral("^(\\d+)\\s*[xX]\\s*\\+\\s*(\\d+)$"));
    for (const QString &part : expression.split(QLatin1Char(','))) {
        const QString token = part.trimmed();
        const auto match = progression.match(token);
        if (match.hasMatch()) {
            bool stepOk = false, offsetOk = false;
            const qint64 step = match.captured(1).toLongLong(&stepOk);
            const qint64 offset = match.captured(2).toLongLong(&offsetOk);
            if (!stepOk || !offsetOk || step < 1 || step > pageCount ||
                offset > pageCount - step) {
                error = QObject::tr("Invalid page range: %1").arg(expression);
                return false;
            }
            for (qint64 page = step + offset; page <= pageCount; page += step)
                selected.insert(int(page - 1));
            continue;
        }
        const int dash = token.indexOf(QLatin1Char('-'));
        bool firstOk = false;
        const int first = (dash < 0 ? token : token.left(dash)).toInt(&firstOk);
        bool lastOk = true;
        const int last = dash < 0 ? first :
                         (token.mid(dash + 1).isEmpty() ? pageCount :
                          token.mid(dash + 1).toInt(&lastOk));
        if (!firstOk || !lastOk || first < 1 || last < first || last > pageCount ||
            (dash >= 0 && token.indexOf(QLatin1Char('-'), dash + 1) >= 0)) {
            error = QObject::tr("Invalid page range: %1").arg(expression);
            return false;
        }
        for (int page = first; page <= last; ++page) selected.insert(page - 1);
    }
    for (int page = 0; page < pageCount; ++page)
        if (selected.contains(page)) pages.append(page);
    return true;
}

bool trimSelections(const Poppler::Document &document, const QVector<int> &pages,
                    const QVector<QRectF> &selections, double paddingMm,
                    QVector<QRectF> &trimmed, QString &error) {
    if (pages.isEmpty() || selections.isEmpty() || paddingMm < 0) {
        error = QObject::tr("Select an area before trimming margins.");
        return false;
    }
    trimmed = selections;
    QVector<QRectF> bounds(selections.size());
    QVector<bool> found(selections.size(), false);
    for (int pageIndex : pages) {
        auto page = document.page(pageIndex);
        if (!page) {
            error = QObject::tr("Cannot read page %1.").arg(pageIndex + 1);
            return false;
        }
        const QSizeF points = page->pageSizeF();
        if (points.width() * points.height() > 40000000.0) {
            error = QObject::tr("Page %1 is too large to inspect for margins.").arg(pageIndex + 1);
            return false;
        }
        const QImage image = page->renderToImage(72, 72).convertToFormat(QImage::Format_ARGB32);
        if (image.isNull()) {
            error = QObject::tr("Cannot render page %1.").arg(pageIndex + 1);
            return false;
        }
        for (int i = 0; i < selections.size(); ++i) {
            const QRectF region = selections.at(i).intersected(QRectF(0, 0, 1, 1));
            const int left = std::clamp(int(std::floor(region.left() * image.width())), 0, image.width());
            const int top = std::clamp(int(std::floor(region.top() * image.height())), 0, image.height());
            const int right = std::clamp(int(std::ceil(region.right() * image.width())), 0, image.width());
            const int bottom = std::clamp(int(std::ceil(region.bottom() * image.height())), 0, image.height());
            int minX = right, minY = bottom, maxX = left - 1, maxY = top - 1;
            for (int y = top; y < bottom; ++y) {
                const auto *scanline = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                for (int x = left; x < right; ++x) {
                    const QRgb pixel = scanline[x];
                    if (qAlpha(pixel) < 16) continue;
                    if (qRed(pixel) >= 245 && qGreen(pixel) >= 245 && qBlue(pixel) >= 245)
                        continue;
                    minX = std::min(minX, x); minY = std::min(minY, y);
                    maxX = std::max(maxX, x); maxY = std::max(maxY, y);
                }
            }
            if (maxX < minX) continue;
            const double padX = paddingMm / 25.4 * 72.0 / points.width();
            const double padY = paddingMm / 25.4 * 72.0 / points.height();
            QRectF content(QPointF(std::max(region.left(), double(minX) / image.width() - padX),
                                   std::max(region.top(), double(minY) / image.height() - padY)),
                           QPointF(std::min(region.right(), double(maxX + 1) / image.width() + padX),
                                   std::min(region.bottom(), double(maxY + 1) / image.height() + padY)));
            bounds[i] = found[i] ? bounds[i].united(content) : content;
            found[i] = true;
        }
    }
    for (int i = 0; i < selections.size(); ++i)
        if (found[i]) trimmed[i] = bounds[i];
    return true;
}

QVector<QRectF> splitForDevice(const QRectF &selection, const QSizeF &pagePoints,
                               double widthToHeight) {
    QVector<QRectF> result;
    if (selection.isEmpty() || pagePoints.isEmpty() || widthToHeight <= 0) return result;
    const double physicalRatio = selection.width() * pagePoints.width() /
                                 (selection.height() * pagePoints.height());
    if (!std::isfinite(physicalRatio) ||
        physicalRatio / widthToHeight > 100 || widthToHeight / physicalRatio > 100)
        return result;
    // A near match is more useful as one page than two almost identical pages.
    if (physicalRatio / widthToHeight < 1.2 &&
        widthToHeight / physicalRatio < 1.2) {
        result.append(selection);
    } else if (physicalRatio > widthToHeight) {
        const int columns = std::max(1, int(std::ceil(physicalRatio / widthToHeight)));
        const double width = selection.height() * pagePoints.height() * widthToHeight /
                             pagePoints.width();
        for (int column = 0; column < columns; ++column) {
            const double x = std::min(selection.left() + column * width,
                                      selection.right() - width);
            result.append(QRectF(x, selection.top(), width, selection.height()));
        }
    } else {
        const int rows = std::max(1, int(std::ceil(widthToHeight / physicalRatio)));
        const double height = selection.width() * pagePoints.width() /
                              (widthToHeight * pagePoints.height());
        for (int row = 0; row < rows; ++row) {
            const double y = std::min(selection.top() + row * height,
                                      selection.bottom() - height);
            result.append(QRectF(selection.left(), y, selection.width(), height));
        }
    }
    return result;
}
