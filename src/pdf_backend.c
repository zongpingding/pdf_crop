// SPDX-License-Identifier: AGPL-3.0-or-later
// Uses MuPDF's PDF content sanitizer to discard objects wholly outside
// the selected rectangle while retaining editable text and vector graphics.
#include "pdf_backend.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    fz_rect keep;
} crop_filter_data;

static int outside_selection(fz_context *ctx, void *opaque, fz_rect bounds,
                             fz_cull_type type)
{
    (void)ctx;
    (void)type;
    crop_filter_data *data = (crop_filter_data *)opaque;
    return fz_is_empty_rect(fz_intersect_rect(bounds, data->keep));
}

static void crop_page(fz_context *ctx, pdf_document *doc, int index,
                      float x0, float y0, float x1, float y1)
{
    pdf_page *page = NULL;
    fz_var(page);
    fz_try(ctx) {
        page = pdf_load_page(ctx, doc, index);
        // MuPDF's page bounds are the visible page in its displayed rotation.
        fz_rect visible = pdf_bound_page(ctx, page, FZ_CROP_BOX);
        if (fz_is_empty_rect(visible))
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Page %d has empty bounds", index + 1);

        fz_rect selected = {
            visible.x0 + x0 * (visible.x1 - visible.x0),
            visible.y0 + y0 * (visible.y1 - visible.y0),
            visible.x0 + x1 * (visible.x1 - visible.x0),
            visible.y0 + y1 * (visible.y1 - visible.y0)
        };
        if (fz_is_empty_rect(selected))
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Selected region is empty");

        // Convert from the displayed page to the PDF's own coordinates.
        fz_matrix to_pdf;
        pdf_page_transform(ctx, page, NULL, &to_pdf);
        crop_filter_data filter_data = { fz_transform_rect(selected, to_pdf) };
        pdf_sanitize_filter_options sanitize = { 0 };
        sanitize.opaque = &filter_data;
        sanitize.culler = outside_selection;
        pdf_filter_factory filters[2] = { 0 };
        filters[0].filter = pdf_new_sanitize_filter;
        filters[0].options = &sanitize;
        pdf_filter_options options = { 0 };
        options.filters = filters;
        options.recurse = 1;
        options.instance_forms = 1;
        pdf_filter_page_contents(ctx, doc, page, &options);

        // Remove annotations fully outside the result; filter the appearance
        // streams of those that intersect it.
        pdf_annot *annot = pdf_first_annot(ctx, page);
        while (annot) {
            pdf_annot *next = pdf_next_annot(ctx, annot);
            fz_rect bounds = pdf_bound_annot(ctx, annot);
            if (fz_is_empty_rect(fz_intersect_rect(bounds, selected)))
                pdf_delete_annot(ctx, page, annot);
            else
                pdf_filter_annot_contents(ctx, doc, annot, &options);
            annot = next;
        }

        // A visible crop box alone leaves the old content in the file. The
        // sanitizer above removes wholly outside content first. Set all page
        // boxes to the selected area so the exported page has that size.
        pdf_dict_put_rect(ctx, page->obj, PDF_NAME(MediaBox), filter_data.keep);
        pdf_dict_put_rect(ctx, page->obj, PDF_NAME(CropBox), filter_data.keep);
        pdf_dict_put_rect(ctx, page->obj, PDF_NAME(BleedBox), filter_data.keep);
        pdf_dict_put_rect(ctx, page->obj, PDF_NAME(TrimBox), filter_data.keep);
        pdf_dict_put_rect(ctx, page->obj, PDF_NAME(ArtBox), filter_data.keep);
    }
    fz_always(ctx) {
        if (page)
            pdf_drop_page(ctx, page);
    }
    fz_catch(ctx)
        fz_rethrow(ctx);
}

int export_vector_pdf(const char *input_path, const char *output_path,
                      int page_index, int all_pages,
                      float x0, float y0, float x1, float y1,
                      char *error, size_t error_capacity)
{
    if (error && error_capacity)
        error[0] = '\0';
    if (!input_path || !output_path || !*input_path || !*output_path ||
        strcmp(input_path, output_path) == 0 ||
        !(x0 >= 0 && y0 >= 0 && x1 <= 1 && y1 <= 1 && x0 < x1 && y0 < y1)) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "Invalid file path or selection");
        return 0;
    }

    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!ctx) {
        if (error && error_capacity)
            snprintf(error, error_capacity, "Could not create MuPDF context");
        return 0;
    }
    pdf_document *doc = NULL;
    fz_var(doc);
    int ok = 1;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, input_path);
        if (pdf_needs_password(ctx, doc))
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Password-protected PDFs are not supported yet");
        int count = pdf_count_pages(ctx, doc);
        if (count < 1 || page_index < 0 || page_index >= count)
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Invalid PDF page number");

        if (all_pages) {
            for (int i = 0; i < count; ++i)
                crop_page(ctx, doc, i, x0, y0, x1, y1);
        } else {
            crop_page(ctx, doc, page_index, x0, y0, x1, y1);
            for (int i = count - 1; i >= 0; --i)
                if (i != page_index)
                    pdf_delete_page(ctx, doc, i);
        }

        pdf_write_options write_options = pdf_default_write_options;
        write_options.do_compress = 1;
        write_options.do_compress_images = 1;
        write_options.do_compress_fonts = 1;
        write_options.do_garbage = 3;
        pdf_save_document(ctx, doc, output_path, &write_options);
    }
    fz_catch(ctx) {
        ok = 0;
        if (error && error_capacity)
            snprintf(error, error_capacity, "%s", fz_caught_message(ctx));
    }
    if (doc)
        pdf_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return ok;
}
