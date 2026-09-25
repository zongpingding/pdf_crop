// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pdf_backend.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

static bool near(float actual, float expected) {
    return std::abs(actual - expected) < 0.0001f;
}

static bool checkBox(const pdf_page_box &box, float x0, float y0,
                     float x1, float y1) {
    return box.available && near(box.x0, x0) && near(box.y0, y0) &&
           near(box.x1, x1) && near(box.y1, y1);
}

static bool exportedMediaBox(const QByteArray &path, fz_rect expected) {
    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    pdf_document *doc = NULL;
    fz_var(doc);
    bool good = false;
    fz_try(ctx) {
        doc = pdf_open_document(ctx, path.constData());
        if (pdf_count_pages(ctx, doc) == 1) {
            pdf_obj *page = pdf_lookup_page_obj(ctx, doc, 0);
            const fz_rect box = pdf_to_rect(ctx, pdf_dict_get(ctx, page, PDF_NAME(MediaBox)));
            good = near(box.x0, expected.x0) && near(box.y0, expected.y0) &&
                   near(box.x1, expected.x1) && near(box.y1, expected.y1);
        }
    }
    fz_catch(ctx) { good = false; }
    if (doc) pdf_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return good;
}

int main() {
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString path = directory.filePath(QStringLiteral("boxes.pdf"));
    const QByteArray objects[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] "
        "/CropBox [50 100 550 700] /BleedBox [40 90 560 710] "
        "/TrimBox [100 150 500 650] /ArtBox [200 300 400 500] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] "
        "/CropBox [50 100 550 700] /TrimBox [100 150 500 650] /Rotate 90 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] >>"
    };
    QByteArray pdf("%PDF-1.4\n");
    QList<int> offsets;
    for (int i = 0; i < 5; ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = pdf.size();
    pdf += "xref\n0 6\n0000000000 65535 f \n";
    for (int offset : offsets)
        pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" +
           QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(pdf) != pdf.size()) return 1;
    file.close();

    pdf_page_box boxes[3] = {};
    char error[1024] = {};
    const QByteArray filename = QFile::encodeName(path);
    if (!read_pdf_page_boxes(filename.constData(), 0, boxes, error, sizeof(error))) {
        std::fprintf(stderr, "%s\n", error);
        return 1;
    }
    if (!checkBox(boxes[0], 0, 0, 1, 1) ||
        !checkBox(boxes[1], .1f, 1.f / 12.f, .9f, 11.f / 12.f) ||
        !checkBox(boxes[2], .3f, 1.f / 3.f, .7f, 2.f / 3.f)) {
        std::fprintf(stderr, "Unrotated PDF page boxes have incorrect bounds\n");
        for (const auto &box : boxes)
            std::fprintf(stderr, "%d %.5f %.5f %.5f %.5f\n", box.available,
                         box.x0, box.y0, box.x1, box.y1);
        return 1;
    }
    if (!read_pdf_page_boxes(filename.constData(), 1, boxes, error, sizeof(error))) return 1;
    if (boxes[0].available || boxes[2].available ||
        !checkBox(boxes[1], 1.f / 12.f, .1f, 11.f / 12.f, .9f)) {
        std::fprintf(stderr, "Rotated PDF page box has incorrect bounds\n");
        return 1;
    }
    if (!read_pdf_page_boxes(filename.constData(), 2, boxes, error, sizeof(error))) return 1;
    if (boxes[0].available || boxes[1].available || boxes[2].available) {
        std::fprintf(stderr, "Missing page boxes should be unavailable\n");
        return 1;
    }
    const pdf_crop_job jobs[] = {
        {0, .1f, 1.f / 12.f, .9f, 11.f / 12.f},
        {1, 1.f / 12.f, .1f, 11.f / 12.f, .9f}
    };
    for (int i = 0; i < 2; ++i) {
        const QByteArray output = QFile::encodeName(
            directory.filePath(QStringLiteral("crop-%1.pdf").arg(i)));
        if (!export_vector_pdf(filename.constData(), output.constData(), &jobs[i],
                               1, 0, error, sizeof(error)) ||
            !exportedMediaBox(output, {100, 150, 500, 650})) {
            std::fprintf(stderr, "Exported page box is incorrect: %s\n", error);
            return 1;
        }
    }
    return 0;
}
