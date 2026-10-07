#pragma once

/* Synchronized start for recordings that follow the main Record button.
 *
 * Upstream starts each source's recording one after another, each one first
 * initializing its own video encoder and spawning its own muxer, so the files
 * begin up to a second or more apart. Here every recording started by the
 * same press of Record is collected into one batch; all of their encoders are
 * initialized in parallel, and once every one is ready they are all started
 * at the same moment.
 *
 * Each batch also records the render time of the first frame in every file,
 * and of the main recording's first frame. When recording stops, a
 * "<main recording name>.sync.json" is written next to the source recordings
 * giving each file's exact start offset from the main recording, in
 * nanoseconds and in frames, so the files can be aligned frame-exactly.
 */

#include <obs.h>

/* `count_expected` returns how many recordings the Record button is about to
 * start; the batch waits for exactly that many (or for SYNC_START_WAIT_MS). */
void sync_start_init(int (*count_expected)(void));
void sync_start_free(void);

/* Queue `output` to start with the current batch. `started` is called (from a
 * worker thread) with the result of obs_output_start. `name` is how the file
 * is labelled in the sync report and `path` the file being written. */
void sync_start_enqueue(obs_output_t *output, const char *name, const char *path, void (*started)(void *param, bool ok),
			void *param);

/* Forwarded from the module's frontend event callback. */
void sync_start_frontend_event(int event);
