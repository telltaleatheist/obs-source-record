#pragma once

/* The C side of the Recording panel (panel.cpp): what the panel may read and
 * change about Source Record filters. Implemented in source-record.c. */

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of the filters, each with a reference the caller releases. */
size_t sr_filter_list(obs_source_t ***out);
void sr_filter_list_free(obs_source_t **list, size_t count);

/* The filter's file output while it exists (reference the caller releases), or NULL. */
obs_output_t *sr_filter_file_output(obs_source_t *filter);

/* record_mode values (match the filter's "record_mode" setting). */
enum sr_record_mode { SR_MODE_NONE = 0, SR_MODE_ALWAYS = 1, SR_MODE_RECORDING = 3 };
long long sr_filter_record_mode(obs_source_t *filter);
void sr_filter_set_record_mode(obs_source_t *filter, enum sr_record_mode mode);

void sr_panel_init(void);

#ifdef __cplusplus
}
#endif
