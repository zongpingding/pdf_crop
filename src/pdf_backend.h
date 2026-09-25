#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Each rectangle is normalized to its source page's displayed crop box.
// One output page is produced for each job, in the supplied order.
typedef struct {
    int page_index;
    float x0, y0, x1, y1;
} pdf_crop_job;

// Explicit PDF BleedBox, TrimBox and ArtBox, in that order. Coordinates are
// normalized to the visible CropBox, clipped to [0, 1].
typedef struct {
    int available;
    float x0, y0, x1, y1;
} pdf_page_box;

int read_pdf_page_boxes(const char *input_path, int page_index,
                        pdf_page_box boxes[3], char *error, size_t error_capacity);

int export_vector_pdf(const char *input_path, const char *output_path,
                      const pdf_crop_job *jobs, size_t job_count, int rotation,
                      char *error, size_t error_capacity);

// Copy complete source pages without filtering or changing their page boxes.
// Pages must be unique, zero-based, and in ascending order.
int export_full_pages_pdf(const char *input_path, const char *output_path,
                          const int *pages, size_t page_count,
                          char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
