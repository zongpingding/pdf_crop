// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QRectF>
#include <QString>
#include <QVector>

namespace Poppler { class Document; }

struct CropJob {
    int pageIndex = 0;
    QRectF selection;
};

bool parsePageRange(const QString &expression, int pageCount,
                    QVector<int> &pages, QString &error);
bool trimSelections(const Poppler::Document &document, const QVector<int> &pages,
                    const QVector<QRectF> &selections, double paddingMm,
                    QVector<QRectF> &trimmed, QString &error);
QVector<QRectF> splitForDevice(const QRectF &selection, const QSizeF &pagePoints,
                               double widthToHeight);
