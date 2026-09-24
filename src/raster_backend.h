#pragma once

#include <QRectF>
#include <QString>
#include "crop_plan.h"

namespace Poppler { class Document; }

// Export only rendered pixels from the selected region into a new PDF.
// The source PDF's original page streams and resources are never copied.
bool export_raster_pdf(const Poppler::Document &document, const QString &output,
                       const QVector<CropJob> &jobs, int dpi, int rotation,
                       QString &error);
