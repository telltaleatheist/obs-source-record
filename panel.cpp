// Recording panel: a dock listing every Source Record filter next to the main
// recording, MultiCorder-style. The master button starts and stops the main
// recording; ticked sources start and stop with it (record_mode "Recording"),
// each row can also be started or stopped on its own, and every row shows
// whether it is writing, for how long, how much, and how many frames dropped.

#include "panel-api.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QStringList>

#include <cstring>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <map>
#include <string>
#include <vector>

namespace {

QString FormatDuration(uint64_t frames)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || !ovi.fps_num)
		return QStringLiteral("--:--:--");
	uint64_t secs = frames * ovi.fps_den / ovi.fps_num;
	return QString::asprintf("%02llu:%02llu:%02llu", (unsigned long long)(secs / 3600),
				 (unsigned long long)(secs / 60 % 60), (unsigned long long)(secs % 60));
}

QString FormatBytes(uint64_t bytes)
{
	if (bytes >= 1024ull * 1024 * 1024)
		return QString::asprintf("%.2f GB", bytes / (1024.0 * 1024 * 1024));
	return QString::asprintf("%.0f MB", bytes / (1024.0 * 1024));
}

bool Confirm(QWidget *parent, const char *text)
{
	return QMessageBox::question(parent, obs_module_text("Panel.ConfirmTitle"), obs_module_text(text),
				     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

// What audio goes into a Source Record file, from the filter's settings.
QString FilterAudio(obs_source_t *filter)
{
	obs_data_t *settings = obs_source_get_settings(filter);
	QString text;
	if (!obs_data_get_bool(settings, "different_audio")) {
		obs_source_t *parent = obs_filter_get_parent(filter);
		text = QString(obs_module_text("Panel.AudioOwn")).arg(QString::fromUtf8(parent ? obs_source_get_name(parent) : "?"));
	} else {
		long long track = obs_data_get_int(settings, "audio_track");
		if (track == -1)
			text = obs_module_text("Panel.AudioAllTracks");
		else if (track > 0)
			text = QString(obs_module_text("Panel.AudioTrack")).arg(track);
		else
			text = QString::fromUtf8(obs_data_get_string(settings, "audio_source"));
	}
	obs_data_release(settings);
	return text;
}

// The main recording's audio tracks and which inputs feed each.
QString MainAudio()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return QString();
	bool advanced = strcmp(config_get_string(cfg, "Output", "Mode") ? config_get_string(cfg, "Output", "Mode") : "",
			       "Advanced") == 0;
	long long mask = advanced ? config_get_int(cfg, "AdvOut", "RecTracks") : 1;
	struct Ctx {
		uint32_t track;
		QStringList names;
	};
	QStringList lines;
	for (uint32_t t = 0; t < MAX_AUDIO_MIXES; t++) {
		if (!(mask & (1ll << t)))
			continue;
		Ctx ctx{t, {}};
		obs_enum_sources(
			[](void *data, obs_source_t *source) {
				auto c = static_cast<Ctx *>(data);
				if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) && !obs_source_muted(source) &&
				    (obs_source_get_audio_mixers(source) & (1u << c->track)))
					c->names << QString::fromUtf8(obs_source_get_name(source));
				return true;
			},
			&ctx);
		QString trackName;
		if (advanced) {
			const char *n = config_get_string(cfg, "AdvOut", QString("Track%1Name").arg(t + 1).toUtf8().constData());
			if (n && *n)
				trackName = QString::fromUtf8(n);
		}
		lines << QString(obs_module_text("Panel.MainTrack")).arg(t + 1).arg(trackName.isEmpty() ? QString() : " (" + trackName + ")")
				 .arg(ctx.names.isEmpty() ? obs_module_text("Panel.AudioNothing") : ctx.names.join(", "));
	}
	return lines.join(QChar('\n'));
}

struct Row {
	QToolButton *expand;
	QLabel *audio; // the expanded line: what audio goes into this file
	QCheckBox *armed;
	QLabel *dot;
	QLabel *name;
	QLabel *time;
	QLabel *size;
	QLabel *dropped;
	QPushButton *button; // null for the main recording row
};

class RecordingPanel : public QWidget {
public:
	RecordingPanel()
	{
		auto outer = new QVBoxLayout(this);
		outer->setContentsMargins(4, 4, 4, 4);

		// Same layout as the Multiple output dock: Start all | Stop all, then one row per recording.
		auto top = new QHBoxLayout();
		startAll_ = new QPushButton(obs_module_text("Panel.StartAll"), this);
		stopAll_ = new QPushButton(obs_module_text("Panel.StopAll"), this);
		top->addWidget(startAll_);
		top->addWidget(stopAll_);
		outer->addLayout(top);

		auto scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		auto body = new QWidget(scroll);
		grid_ = new QGridLayout(body);
		grid_->setAlignment(Qt::AlignTop);
		grid_->setColumnStretch(3, 1);
		scroll->setWidget(body);
		outer->addWidget(scroll, 1);

		mainRow_ = AddRow(obs_module_text("Panel.MainRecording"), true);
		mainRow_.armed->setChecked(true);
		mainRow_.armed->setEnabled(false);
		mainRow_.armed->setToolTip(obs_module_text("Panel.MainTip"));

		QObject::connect(startAll_, &QPushButton::clicked, [this]() { StartAll(); });
		QObject::connect(stopAll_, &QPushButton::clicked, [this]() { StopAll(); });
		QObject::connect(mainRow_.button, &QPushButton::clicked, [this]() {
			if (obs_frontend_recording_active())
				StopAll();
			else
				StartAll();
		});

		// The frontend's outputs do not exist while modules are loading, so the
		// panel starts polling only once OBS has finished loading (and stops on exit).
		timer_.setInterval(500);
		QObject::connect(&timer_, &QTimer::timeout, [this]() { Refresh(); });
		obs_frontend_add_event_callback(OnFrontendEvent, this);
	}

	~RecordingPanel() override { obs_frontend_remove_event_callback(OnFrontendEvent, this); }

private:
	static void OnFrontendEvent(enum obs_frontend_event event, void *data)
	{
		auto panel = static_cast<RecordingPanel *>(data);
		if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
			panel->timer_.start();
			panel->Refresh();
		} else if (event == OBS_FRONTEND_EVENT_EXIT) {
			panel->timer_.stop();
		}
	}

	QPushButton *startAll_;
	QPushButton *stopAll_;
	QGridLayout *grid_;
	Row mainRow_{};
	int nextRow_ = 0;
	QTimer timer_;
	std::vector<std::string> shownKeys_;
	std::map<std::string, Row> rows_; // key: filter pointer as text

	Row AddRow(const QString &name, bool withButton)
	{
		Row r{};
		int y = nextRow_;
		nextRow_ += 2; // the second row is the expandable audio line
		// A labelled disclosure toggle ("Audio ▸" / "Audio ▾"), clearer than a bare arrow.
		r.expand = new QToolButton(this);
		r.expand->setAutoRaise(true);
		r.expand->setCheckable(true);
		r.expand->setToolTip(obs_module_text("Panel.AudioTip"));
		r.expand->setText(QString::fromUtf8(obs_module_text("Panel.AudioCollapsed")));
		r.audio = new QLabel(this);
		r.audio->setWordWrap(true);
		r.audio->setTextInteractionFlags(Qt::TextSelectableByMouse);
		// Secondary text in the theme's own text colour, slightly dimmed, so it reads on any theme.
		QPalette pal = r.audio->palette();
		QColor text = pal.color(QPalette::WindowText);
		text.setAlpha(190);
		pal.setColor(QPalette::WindowText, text);
		r.audio->setPalette(pal);
		r.audio->setVisible(false);
		QLabel *audio = r.audio;
		QToolButton *expand = r.expand;
		QObject::connect(r.expand, &QToolButton::toggled, [audio, expand](bool open) {
			audio->setVisible(open);
			expand->setText(QString::fromUtf8(obs_module_text(open ? "Panel.AudioExpanded" : "Panel.AudioCollapsed")));
		});
		r.armed = new QCheckBox(this);
		r.armed->setToolTip(obs_module_text("Panel.ArmedTip"));
		r.dot = new QLabel(this);
		r.name = new QLabel(name, this);
		r.time = new QLabel(this);
		r.size = new QLabel(this);
		r.dropped = new QLabel(this);
		grid_->addWidget(r.expand, y, 0);
		grid_->addWidget(r.armed, y, 1);
		grid_->addWidget(r.dot, y, 2);
		grid_->addWidget(r.name, y, 3);
		grid_->addWidget(r.time, y, 4);
		grid_->addWidget(r.size, y, 5);
		grid_->addWidget(r.dropped, y, 6);
		grid_->addWidget(r.audio, y + 1, 3, 1, 5);
		if (withButton) {
			r.button = new QPushButton(this);
			grid_->addWidget(r.button, y, 7);
		}
		return r;
	}

	void ClearSourceRows()
	{
		for (auto &[key, r] : rows_) {
			for (QWidget *w : {(QWidget *)r.expand, (QWidget *)r.audio, (QWidget *)r.armed, (QWidget *)r.dot,
					   (QWidget *)r.name, (QWidget *)r.time, (QWidget *)r.size, (QWidget *)r.dropped,
					   (QWidget *)r.button})
				if (w) {
					grid_->removeWidget(w);
					w->deleteLater();
				}
		}
		rows_.clear();
		nextRow_ = 2; // rows 0-1 are the main recording
	}

	static void ShowStatus(Row &r, obs_output_t *out)
	{
		bool active = out && obs_output_active(out);
		r.dot->setText(active ? QStringLiteral("<span style='color:#e04040'>&#9679;</span>")
				      : QStringLiteral("<span style='color:#808080'>&#9675;</span>"));
		if (active) {
			r.time->setText(FormatDuration((uint64_t)obs_output_get_total_frames(out)));
			r.size->setText(FormatBytes(obs_output_get_total_bytes(out)));
			int dropped = obs_output_get_frames_dropped(out);
			r.dropped->setText(QString(obs_module_text("Panel.Dropped")).arg(dropped));
		} else {
			r.time->setText(QString());
			r.size->setText(QString());
			r.dropped->setText(QString());
		}
	}

	static std::string KeyOf(obs_source_t *filter) { return std::to_string((uintptr_t)filter); }

	void Refresh()
	{
		obs_source_t **list = nullptr;
		size_t count = sr_filter_list(&list);

		std::vector<std::string> keys;
		for (size_t i = 0; i < count; i++)
			keys.push_back(KeyOf(list[i]));
		if (keys != shownKeys_) {
			ClearSourceRows();
			for (size_t i = 0; i < count; i++) {
				obs_source_t *parent = obs_filter_get_parent(list[i]);
				QString label = QString::fromUtf8(parent ? obs_source_get_name(parent) : "?");
				int onParent = 0;
				for (size_t j = 0; j < count; j++)
					onParent += obs_filter_get_parent(list[j]) == parent;
				if (onParent > 1)
					label += QStringLiteral(" (") + QString::fromUtf8(obs_source_get_name(list[i])) + ")";
				Row r = AddRow(label, true);
				obs_source_t *filter = list[i];
				obs_weak_source_t *weak = obs_source_get_weak_source(filter);
				QObject::connect(r.armed, &QCheckBox::toggled, [weak](bool on) {
					obs_source_t *f = obs_weak_source_get_source(weak);
					if (!f)
						return;
					long long mode = sr_filter_record_mode(f);
					if (on && mode == SR_MODE_NONE)
						sr_filter_set_record_mode(f, SR_MODE_RECORDING);
					else if (!on && mode == SR_MODE_RECORDING)
						sr_filter_set_record_mode(f, SR_MODE_NONE);
					obs_source_release(f);
				});
				QObject::connect(r.button, &QPushButton::clicked, [this, weak]() { OnRowButton(weak); });
				QObject::connect(r.armed, &QObject::destroyed, [weak]() { obs_weak_source_release(weak); });
				rows_[keys[i]] = r;
			}
			shownKeys_ = keys;
		}

		bool anyRowRunning = false;
		for (size_t i = 0; i < count; i++) {
			Row &r = rows_[keys[i]];
			obs_output_t *out = sr_filter_file_output(list[i]);
			bool active = out && obs_output_active(out);
			anyRowRunning = anyRowRunning || active;
			long long mode = sr_filter_record_mode(list[i]);
			r.armed->blockSignals(true);
			r.armed->setChecked(mode == SR_MODE_RECORDING);
			r.armed->blockSignals(false);
			r.armed->setEnabled(!active);
			r.button->setText(obs_module_text(active ? "Panel.Stop" : "Panel.Start"));
			if (r.audio->isVisible())
				r.audio->setText(QString(obs_module_text("Panel.AudioLine")).arg(FilterAudio(list[i])));
			ShowStatus(r, out);
			obs_output_release(out);
		}
		sr_filter_list_free(list, count);

		if (mainRow_.audio->isVisible())
			mainRow_.audio->setText(MainAudio());
		obs_output_t *main = obs_frontend_get_recording_output();
		ShowStatus(mainRow_, main);
		obs_output_release(main);
		bool recording = obs_frontend_recording_active();
		mainRow_.button->setText(obs_module_text(recording ? "Panel.Stop" : "Panel.Start"));
		startAll_->setEnabled(!recording);
		stopAll_->setEnabled(recording || anyRowRunning);
	}

	// The main recording plus every ticked source (they follow the main recording).
	void StartAll()
	{
		if (!obs_frontend_recording_active())
			obs_frontend_recording_start();
	}

	// Everything: the main recording, the sources following it, and any started on their own.
	void StopAll()
	{
		if (!Confirm(this, "Panel.ConfirmStopAll"))
			return;
		if (obs_frontend_recording_active())
			obs_frontend_recording_stop();
		obs_source_t **list = nullptr;
		size_t count = sr_filter_list(&list);
		for (size_t i = 0; i < count; i++) {
			if (sr_filter_record_mode(list[i]) != SR_MODE_ALWAYS)
				continue;
			// Back to "with the main recording", which is stopping, so it stops and stays ticked.
			sr_filter_set_record_mode(list[i], SR_MODE_RECORDING);
		}
		sr_filter_list_free(list, count);
	}

	void OnRowButton(obs_weak_source_t *weak)
	{
		obs_source_t *f = obs_weak_source_get_source(weak);
		if (!f)
			return;
		obs_output_t *out = sr_filter_file_output(f);
		bool active = out && obs_output_active(out);
		obs_output_release(out);
		if (active) {
			if (Confirm(this, "Panel.ConfirmStopOne"))
				sr_filter_set_record_mode(f, SR_MODE_NONE);
		} else {
			// Joining a running session follows the main recording; otherwise record on its own.
			sr_filter_set_record_mode(f, obs_frontend_recording_active() ? SR_MODE_RECORDING : SR_MODE_ALWAYS);
		}
		obs_source_release(f);
	}
};

} // namespace

extern "C" void sr_panel_init(void)
{
	auto panel = new RecordingPanel();
	if (!obs_frontend_add_dock_by_id("source-record-panel", obs_module_text("Panel.Title"), panel))
		delete panel;
}
