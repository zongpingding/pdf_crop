#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The rectangle is normalized to the visible, rotated page shown in the UI.
// page_index is zero-based. When all_pages is false, the output has one page.
int export_vector_pdf(const char *input_path, const char *output_path,
                      int page_index, int all_pages,
                      float x0, float y0, float x1, float y1,
                      char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
