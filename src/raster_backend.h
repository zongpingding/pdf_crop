#pragma once

#include <QRectF>
#include <QString>

namespace Poppler { class Document; }

// Export only rendered pixels from the selected region into a new PDF.
// The source PDF's original page streams and resources are never copied.
bool export_raster_pdf(const Poppler::Document &document, const QString &output,
                       int first_page, int last_page_exclusive,
                       const QRectF &normalized_selection, int dpi,
                       QString &error);
