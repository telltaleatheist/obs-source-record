#include "sync-start.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

/* ---------- start batches ---------- */

struct pending_start {
	obs_output_t *output;
	void (*started)(void *param, bool ok);
	void *param;
};

struct batch {
	os_event_t *go;
	volatile long ready;
	volatile long refs;
	long total;
};

struct start_job {
	struct batch *batch;
	struct pending_start item;
};

/* How long a Record press waits for every expected recording to be ready to
 * start before starting the ones that are; a source that never gets there
 * must not hold up the rest. */
#define SYNC_START_WAIT_MS 3000

static pthread_mutex_t pending_mutex;
static DARRAY(struct pending_start) pending;
static int (*expected_fn)(void);
static long expected;       /* recordings the current Record press will start */
static long generation;     /* bumps on every Record press */

/* ---------- timing report ---------- */

struct timed_file {
	char *name;
	char *path;
	obs_output_t *output;
	uint64_t first_frame_ns; /* render time of the file's first video frame; 0 until seen */
	bool missing_time;       /* first video packet had no timing info */
};

static pthread_mutex_t report_mutex;
static DARRAY(struct timed_file *) files;
static struct timed_file master;
static obs_output_t *master_output;

static void on_packet(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time, void *param)
{
	UNUSED_PARAMETER(output);
	struct timed_file *f = param;
	if (pkt->type != OBS_ENCODER_VIDEO)
		return;
	pthread_mutex_lock(&report_mutex);
	if (!f->first_frame_ns && !f->missing_time) {
		if (pkt_time && pkt_time->cts)
			f->first_frame_ns = pkt_time->cts;
		else
			f->missing_time = true;
	}
	pthread_mutex_unlock(&report_mutex);
}

static void start_job_thread_fn(struct start_job *job)
{
	struct batch *b = job->batch;
	obs_output_initialize_encoders(job->item.output, 0);
	if (os_atomic_inc_long(&b->ready) == b->total)
		os_event_signal(b->go);
	os_event_wait(b->go);
	bool ok = obs_output_start(job->item.output);
	job->item.started(job->item.param, ok);
	obs_output_release(job->item.output);
	if (os_atomic_dec_long(&b->refs) == 0) {
		os_event_destroy(b->go);
		bfree(b);
	}
	bfree(job);
}

static void *start_job_thread(void *data)
{
	os_set_thread_name("source-record sync start");
	start_job_thread_fn(data);
	return NULL;
}

/* Takes everything pending and starts it as one batch. Called with
 * pending_mutex held; the starts themselves happen on worker threads. */
static void launch_batch_locked(const char *why)
{
	DARRAY(struct pending_start) items;
	da_init(items);
	da_move(items, pending);
	expected = 0; /* anything enqueued after this starts on its own */

	if (!items.num)
		return;
	struct batch *b = bzalloc(sizeof(struct batch));
	os_event_init(&b->go, OS_EVENT_TYPE_MANUAL);
	b->total = (long)items.num;
	b->refs = (long)items.num;
	blog(LOG_INFO, "[source-record] %s: %ld recordings", why, b->total);

	for (size_t i = 0; i < items.num; i++) {
		struct start_job *job = bzalloc(sizeof(struct start_job));
		job->batch = b;
		job->item = items.array[i];
		pthread_t thread;
		if (pthread_create(&thread, NULL, start_job_thread, job) == 0) {
			pthread_detach(thread);
		} else {
			/* No thread: start inline. It cannot wait for the others here, so
			 * count it ready first; the batch still releases when all arrive. */
			blog(LOG_ERROR, "[source-record] could not create a start thread; starting inline");
			if (os_atomic_inc_long(&b->ready) == b->total)
				os_event_signal(b->go);
			bool ok = obs_output_start(job->item.output);
			job->item.started(job->item.param, ok);
			obs_output_release(job->item.output);
			if (os_atomic_dec_long(&b->refs) == 0) {
				os_event_destroy(b->go);
				bfree(b);
			}
			bfree(job);
		}
	}
	da_free(items);
}

static void *watchdog_thread(void *data)
{
	long gen = (long)(intptr_t)data;
	os_set_thread_name("source-record sync start watchdog");
	os_sleep_ms(SYNC_START_WAIT_MS);
	pthread_mutex_lock(&pending_mutex);
	if (gen == generation && pending.num) {
		blog(LOG_WARNING, "[source-record] only %zu of %ld recordings were ready after %d ms",
		     pending.num, expected, SYNC_START_WAIT_MS);
		launch_batch_locked("starting the recordings that are ready");
	}
	pthread_mutex_unlock(&pending_mutex);
	return NULL;
}

void sync_start_enqueue(obs_output_t *output, const char *name, const char *path, void (*started)(void *param, bool ok),
			void *param)
{
	struct timed_file *f = bzalloc(sizeof(struct timed_file));
	f->name = bstrdup(name);
	f->path = bstrdup(path);
	f->output = obs_output_get_ref(output);
	obs_output_add_packet_callback(output, on_packet, f);
	pthread_mutex_lock(&report_mutex);
	da_push_back(files, &f);
	pthread_mutex_unlock(&report_mutex);

	struct pending_start item = {obs_output_get_ref(output), started, param};
	pthread_mutex_lock(&pending_mutex);
	da_push_back(pending, &item);
	if ((long)pending.num >= expected)
		launch_batch_locked("synchronized start");
	pthread_mutex_unlock(&pending_mutex);
}

/* ---------- report ---------- */

static void clear_report(void)
{
	for (size_t i = 0; i < files.num; i++) {
		struct timed_file *f = files.array[i];
		obs_output_remove_packet_callback(f->output, on_packet, f);
		obs_output_release(f->output);
		bfree(f->name);
		bfree(f->path);
		bfree(f);
	}
	da_free(files);
	if (master_output) {
		obs_output_remove_packet_callback(master_output, on_packet, &master);
		obs_output_release(master_output);
		master_output = NULL;
	}
	memset(&master, 0, sizeof(master));
}

static void json_string(struct dstr *out, const char *s)
{
	dstr_cat_ch(out, '"');
	for (; s && *s; s++) {
		if (*s == '"' || *s == '\\')
			dstr_cat_ch(out, '\\');
		dstr_cat_ch(out, *s);
	}
	dstr_cat_ch(out, '"');
}

static void write_report(void)
{
	if (!files.num)
		return;
	char *master_path = obs_frontend_get_last_recording();
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	const double frame_ns = 1e9 * (double)ovi.fps_den / (double)ovi.fps_num;

	struct dstr out = {0};
	dstr_cat(&out, "{\n  \"frame_interval_ns\": ");
	dstr_catf(&out, "%.3f", frame_ns);
	dstr_cat(&out, ",\n  \"master\": {\"path\": ");
	json_string(&out, master_path ? master_path : "");
	dstr_catf(&out, ", \"first_frame_ns\": %llu},\n  \"sources\": [", (unsigned long long)master.first_frame_ns);
	for (size_t i = 0; i < files.num; i++) {
		struct timed_file *f = files.array[i];
		dstr_cat(&out, i ? ",\n    {\"name\": " : "\n    {\"name\": ");
		json_string(&out, f->name);
		dstr_cat(&out, ", \"path\": ");
		json_string(&out, f->path);
		dstr_catf(&out, ", \"first_frame_ns\": %llu", (unsigned long long)f->first_frame_ns);
		if (f->first_frame_ns && master.first_frame_ns) {
			long long offset = (long long)f->first_frame_ns - (long long)master.first_frame_ns;
			dstr_catf(&out, ", \"offset_ns\": %lld, \"offset_frames\": %lld", offset,
				  (long long)llround((double)offset / frame_ns));
		} else {
			dstr_cat(&out, ", \"offset_ns\": null, \"offset_frames\": null");
		}
		dstr_cat(&out, "}");
	}
	dstr_cat(&out, "\n  ],\n  \"note\": \"offset_frames > 0: the source file starts that many frames after the master; "
		       "its frame 0 lines up with master frame offset_frames. Source recordings are frame-exact against each "
		       "other. Measured on OBS 32.2: the main recording's picture runs one frame ahead of its timestamp "
		       "relative to source recordings, so against the main recording the picture offset is "
		       "offset_frames - 1.\"\n}\n");

	/* Next to the source recordings, named after the main recording. */
	struct dstr report_path = {0};
	const char *first = files.array[0]->path;
	const char *slash = strrchr(first, '/');
	dstr_ncopy(&report_path, first, slash ? (size_t)(slash - first + 1) : 0);
	const char *mbase = master_path ? strrchr(master_path, '/') : NULL;
	mbase = mbase ? mbase + 1 : (master_path ? master_path : "recording");
	const char *dot = strrchr(mbase, '.');
	dstr_ncat(&report_path, mbase, dot ? (size_t)(dot - mbase) : strlen(mbase));
	dstr_cat(&report_path, ".sync.json");

	if (os_quick_write_utf8_file(report_path.array, out.array, out.len, false))
		blog(LOG_INFO, "[source-record] wrote sync report %s", report_path.array);
	else
		blog(LOG_ERROR, "[source-record] could not write sync report %s", report_path.array);

	dstr_free(&report_path);
	dstr_free(&out);
	bfree(master_path);
}

void sync_start_frontend_event(int event)
{
	if (event == OBS_FRONTEND_EVENT_RECORDING_STARTING) {
		pthread_mutex_lock(&pending_mutex);
		expected = expected_fn ? expected_fn() : 0;
		long gen = ++generation;
		pthread_mutex_unlock(&pending_mutex);
		blog(LOG_INFO, "[source-record] Record pressed: %ld recordings will start together", expected);
		pthread_t thread;
		if (expected > 0 && pthread_create(&thread, NULL, watchdog_thread, (void *)(intptr_t)gen) == 0)
			pthread_detach(thread);

		pthread_mutex_lock(&report_mutex);
		clear_report();
		master_output = obs_frontend_get_recording_output();
		if (master_output)
			obs_output_add_packet_callback(master_output, on_packet, &master);
		pthread_mutex_unlock(&report_mutex);
	} else if (event == OBS_FRONTEND_EVENT_RECORDING_STOPPED) {
		pthread_mutex_lock(&report_mutex);
		write_report();
		clear_report();
		pthread_mutex_unlock(&report_mutex);
	}
}

static void frontend_cb(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	sync_start_frontend_event((int)event);
}

void sync_start_init(int (*count_expected)(void))
{
	expected_fn = count_expected;
	pthread_mutex_init(&pending_mutex, NULL);
	pthread_mutex_init(&report_mutex, NULL);
	da_init(pending);
	da_init(files);
	obs_frontend_add_event_callback(frontend_cb, NULL);
}

void sync_start_free(void)
{
	obs_frontend_remove_event_callback(frontend_cb, NULL);
	pthread_mutex_lock(&report_mutex);
	clear_report();
	pthread_mutex_unlock(&report_mutex);
	da_free(pending);
	pthread_mutex_destroy(&pending_mutex);
	pthread_mutex_destroy(&report_mutex);
}
