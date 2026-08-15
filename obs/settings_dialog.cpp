// Lean module settings dialog (obs/settings_dialog.cpp). Qt-only; added to
// the module by CMake when ENABLE_QT is set.
//
// The dock already covers per-camera Cameras / Sources / Presets / PTZ /
// Config / Policy; this dialog holds the global application settings that the
// dock does not expose:
//   General   — default stream quality, apply-prompt timeout, discovery
//               interval, SOAP timeout
//   Discovery — continuous Hello/Bye listener toggle, probe timeout
//   PTZ       — transport + motor-control knobs (keep-alive, auth cache,
//               move timeout, stop mode, min interval)
//   Log       — tail of the newest OBS session log + path hints
//   About     — name/version/config-directory info
#include "settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <obs-module.h>
#include <plugin-support.h>

#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "abi.h"
#include "obs_apply.h"
#include "store.h"

namespace obs_onvif::glue {

namespace {

using namespace obs_onvif::registry;

QString FromUtf8(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

QString String(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

std::string AppDataRoot()
{
	const char *appdata = std::getenv("APPDATA");
	return appdata ? std::string(appdata) : std::string();
}

// Best-effort tail (last ~200 lines / 200 KB) of the newest OBS session log.
std::string ReadLatestLogTail()
{
	const std::string logsDir = AppDataRoot() + "\\obs-studio\\logs";
	std::filesystem::path newest;
	std::filesystem::file_time_type latest{};
	try {
		for (const auto &entry :
		     std::filesystem::directory_iterator(logsDir)) {
			if (entry.path().extension() != ".txt")
				continue;
			const auto t = entry.last_write_time();
			if (newest.empty() || t > latest) {
				newest = entry.path();
				latest = t;
			}
		}
	} catch (const std::exception &) {
		return "Unable to locate the OBS log directory: " + logsDir;
	}
	if (newest.empty())
		return "No OBS log file found in " + logsDir;

	std::ifstream in(newest, std::ios::binary | std::ios::ate);
	if (!in)
		return "Unable to open " + newest.string();
	const std::streamsize size = in.tellg();
	const std::streamsize keep = size > 200000 ? 200000 : size;
	in.seekg(size - keep);
	std::string data((size_t)keep, '\0');
	if (keep > 0)
		in.read(&data[0], keep);
	size_t start = 0;
	int newlines = 0;
	for (size_t i = data.size(); i-- > 0;) {
		if (data[i] == '\n') {
			if (++newlines > 200) {
				start = i + 1;
				break;
			}
		}
	}
	return "==== " + newest.string() + " ====\n" + data.substr(start);
}

class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(QWidget *parent = nullptr)
		: QDialog(parent)
	{
		setWindowTitle(String("Settings.Title"));
		setModal(true);

		tabs_ = new QTabWidget(this);
		tabs_->addTab(BuildGeneralTab(), String("Settings.TabGeneral"));
		tabs_->addTab(BuildDiscoveryTab(), String("Settings.TabDiscovery"));
		tabs_->addTab(BuildPtzTab(), String("Settings.TabPtz"));
		tabs_->addTab(BuildLogTab(), String("Settings.TabLog"));
		tabs_->addTab(BuildAboutTab(), String("Settings.TabAbout"));

		status_ = new QLabel(this);
		status_->setWordWrap(true);

		auto *buttons = new QDialogButtonBox(
			QDialogButtonBox::Save | QDialogButtonBox::Close, this);
		connect(buttons, &QDialogButtonBox::accepted, this,
			[this]() { WriteSettings(); });
		connect(buttons, &QDialogButtonBox::rejected, this,
			&QDialog::reject);

		auto *layout = new QVBoxLayout(this);
		layout->addWidget(tabs_);
		layout->addWidget(status_);
		layout->addWidget(buttons);

		LoadSettings();
		logView_->setPlainText(FromUtf8(ReadLatestLogTail()));
	}

private:
	QWidget *BuildGeneralTab()
	{
		stream_ = new QComboBox(this);
		stream_->addItem(String("Settings.StreamHigh"));
		stream_->addItem(String("Settings.StreamLow"));

		promptTimeout_ = new QSpinBox(this);
		promptTimeout_->setRange(5, 300);
		promptTimeout_->setSuffix(" s");

		discoveryInterval_ = new QSpinBox(this);
		discoveryInterval_->setRange(5, 3600);
		discoveryInterval_->setSuffix(" s");

		soapTimeout_ = new QSpinBox(this);
		soapTimeout_->setRange(1, 30);
		soapTimeout_->setSuffix(" s");

		defaultUser_ = new QLineEdit(this);
		defaultPass_ = new QLineEdit(this);
		defaultPass_->setEchoMode(QLineEdit::Password);

		auto *form = new QFormLayout();
		form->addRow(String("Settings.DefaultStream"), stream_);
		form->addRow(String("Settings.PromptTimeout"), promptTimeout_);
		form->addRow(String("Settings.DiscoveryInterval"), discoveryInterval_);
		form->addRow(String("Settings.SoapTimeout"), soapTimeout_);
		form->addRow(String("Settings.DefaultUser"), defaultUser_);
		form->addRow(String("Settings.DefaultPass"), defaultPass_);
		auto *hint = new QLabel(String("Settings.DefaultCredsHint"), this);
		hint->setWordWrap(true);

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addLayout(form);
		layout->addWidget(hint);
		layout->addStretch();
		return tab;
	}

	QWidget *BuildDiscoveryTab()
	{
		helloEnabled_ =
			new QCheckBox(String("Settings.HelloListener"), this);
		probeTimeout_ = new QSpinBox(this);
		probeTimeout_->setRange(1, 30);
		probeTimeout_->setSuffix(" s");

		method_ = new QComboBox(this);
		method_->addItem(String("Settings.MethodAuto"), "auto");
		method_->addItem(String("Settings.MethodSweep"), "sweep");
		method_->addItem(String("Settings.MethodMulticast"), "multicast");

		fwHint_ = new QLabel(this);
		fwHint_->setWordWrap(true);
		fwHint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		fwHint_->setText(String("Settings.FirewallHint"));
		fwHint_->setStyleSheet(
			"padding:6px;background:#404040;border-radius:3px;");
		connect(method_, &QComboBox::currentIndexChanged, this,
			[this](int) { UpdateFirewallHint(); });

		auto *form = new QFormLayout();
		form->addRow(helloEnabled_);
		form->addRow(String("Settings.ProbeTimeout"), probeTimeout_);
		form->addRow(String("Settings.DiscoveryMethod"), method_);

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addLayout(form);
		layout->addWidget(fwHint_);
		layout->addStretch();
		return tab;
	}

	// The firewall-rule guidance applies only when multicast is used and the
	// reply path depends on an inbound UDP 3702 rule (the sweep method needs
	// neither admin nor a rule).
	void UpdateFirewallHint()
	{
		fwHint_->setVisible(
			method_->currentData().toString() == "multicast");
	}

	QWidget *BuildPtzTab()
	{
		keepalive_ = new QCheckBox(String("Settings.PtzKeepalive"), this);
		authCache_ = new QCheckBox(String("Settings.PtzAuthCache"), this);

		moveTimeout_ = new QSpinBox(this);
		moveTimeout_->setRange(0, 300);
		moveTimeout_->setSuffix(" s");
		moveTimeout_->setSpecialValueText(String("Settings.PtzUntilStop"));

		stopMode_ = new QComboBox(this);
		stopMode_->addItem(String("Settings.PtzStopImmediate"), QString("immediate"));
		stopMode_->addItem(String("Settings.PtzStopQueued"), QString("queued"));

		minInterval_ = new QSpinBox(this);
		minInterval_->setRange(0, 1000);
		minInterval_->setSuffix(" ms");

		auto *form = new QFormLayout();
		form->addRow(keepalive_);
		form->addRow(authCache_);
		form->addRow(String("Settings.PtzMoveTimeout"), moveTimeout_);
		form->addRow(String("Settings.PtzStopMode"), stopMode_);
		form->addRow(String("Settings.PtzMinInterval"), minInterval_);
		auto *hint = new QLabel(String("Settings.PtzHint"), this);
		hint->setWordWrap(true);

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addLayout(form);
		layout->addWidget(hint);
		layout->addStretch();
		return tab;
	}

	QWidget *BuildLogTab()
	{
		logView_ = new QPlainTextEdit(this);
		logView_->setReadOnly(true);
		logView_->setMaximumBlockCount(4000);

		auto *refresh = new QPushButton(String("Settings.Refresh"), this);
		connect(refresh, &QPushButton::clicked, this, [this]() {
			logView_->setPlainText(FromUtf8(ReadLatestLogTail()));
		});

		auto *layout = new QVBoxLayout();
		layout->addWidget(logView_);
		layout->addWidget(refresh, 0, Qt::AlignLeft);

		auto *tab = new QWidget(this);
		tab->setLayout(layout);
		return tab;
	}

	QWidget *BuildAboutTab()
	{
		auto *label = new QLabel(this);
		label->setWordWrap(true);
		label->setText(
			QString("<b>%1</b><br/>Version %2<br/>"
				"Config directory: <i>%3</i><br/>"
				"OBS logs: <i>%4</i><br/><br/>"
				"ONVIF camera discovery, PTZ, presets, live-output "
				"apply policy, and camera configuration for "
				"OBS Studio.")
				.arg(FromUtf8(PLUGIN_NAME))
				.arg(FromUtf8(PLUGIN_VERSION))
				.arg(FromUtf8(ConfigDir()))
				.arg(FromUtf8(AppDataRoot() + "\\obs-studio\\logs")));

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addWidget(label);
		layout->addStretch();
		return tab;
	}

	void LoadSettings()
	{
		Store store(ConfigDir());
		AppConfig cfg;
		store.LoadAppConfig(cfg);
		stream_->setCurrentIndex(
			cfg.default_stream == StreamChoice::High ? 0 : 1);
		promptTimeout_->setValue(cfg.prompt_timeout_s);
		discoveryInterval_->setValue(cfg.discovery_interval_s);
		soapTimeout_->setValue(cfg.soap_timeout_s);
		helloEnabled_->setChecked(cfg.hello_listener_enabled);
		probeTimeout_->setValue(cfg.discovery_probe_timeout_s);
		method_->setCurrentIndex(
			std::max(0, method_->findData(
					  FromUtf8(cfg.discovery_method))));
		UpdateFirewallHint();
		keepalive_->setChecked(cfg.soap_keepalive);
		authCache_->setChecked(cfg.ptz_auth_cache);
		moveTimeout_->setValue(cfg.ptz_move_timeout_s);
		stopMode_->setCurrentIndex(cfg.ptz_stop_mode == "queued" ? 1 : 0);
		minInterval_->setValue(cfg.ptz_min_interval_ms);

		// Default ONVIF credentials (Credential Vault, "obs-onvif/default").
		// Discovery uses these when a discovered camera can't be reached
		// with camera-specific or empty credentials.
		std::string secret;
		bool found = false;
		if (Store::ReadCredential(Store::DefaultCredTarget(), secret, found) &&
		    found && !secret.empty()) {
			const size_t at = secret.find(':');
			defaultUser_->setText(
				FromUtf8(secret.substr(0, at)));
			defaultPass_->setText(FromUtf8(
				at == std::string::npos
					? std::string()
					: secret.substr(at + 1)));
		}
	}

	void WriteSettings()
	{
		Store store(ConfigDir());
		AppConfig cfg;
		store.LoadAppConfig(cfg);
		cfg.default_stream = stream_->currentIndex() == 0
					     ? StreamChoice::High
					     : StreamChoice::Low;
		cfg.prompt_timeout_s = promptTimeout_->value();
		cfg.discovery_interval_s = discoveryInterval_->value();
		cfg.soap_timeout_s = soapTimeout_->value();
		cfg.hello_listener_enabled = helloEnabled_->isChecked();
		cfg.discovery_probe_timeout_s = probeTimeout_->value();
		cfg.discovery_method =
			method_->currentData().toString().toStdString();
		cfg.soap_keepalive = keepalive_->isChecked();
		cfg.ptz_auth_cache = authCache_->isChecked();
		cfg.ptz_move_timeout_s = moveTimeout_->value();
		cfg.ptz_stop_mode =
			stopMode_->currentData().toString() == "queued"
				? "queued"
				: "immediate";
		cfg.ptz_min_interval_ms = minInterval_->value();
		const bool saved = store.SaveAppConfig(cfg);

		// Persist the default credentials (empty clears them).
		const std::string user = defaultUser_->text().trimmed().toStdString();
		const std::string pass = defaultPass_->text().trimmed().toStdString();
		if (user.empty() && pass.empty()) {
			Store::DeleteCredential(Store::DefaultCredTarget());
		} else {
			Store::WriteCredential(Store::DefaultCredTarget(),
					       user + ":" + pass);
		}

		if (saved)
			obs_onvif::abi::ApplyAppConfig(cfg);
		status_->setText(saved ? String("Settings.Saved")
				       : String("Settings.SaveFailed"));
	}

	QTabWidget *tabs_;
	QLabel *status_;
	QComboBox *stream_;
	QSpinBox *promptTimeout_;
	QSpinBox *discoveryInterval_;
	QSpinBox *soapTimeout_;
	QCheckBox *helloEnabled_;
	QSpinBox *probeTimeout_;
	QComboBox *method_;
	QLabel *fwHint_;
	QCheckBox *keepalive_;
	QCheckBox *authCache_;
	QSpinBox *moveTimeout_;
	QComboBox *stopMode_;
	QSpinBox *minInterval_;
	QLineEdit *defaultUser_;
	QLineEdit *defaultPass_;
	QPlainTextEdit *logView_;
};

SettingsDialog *g_settings = nullptr;

} // namespace

void ShowSettingsDialog()
{
	if (g_settings)
		return;
	g_settings = new SettingsDialog();
	g_settings->setAttribute(Qt::WA_DeleteOnClose);
	QObject::connect(g_settings, &QObject::destroyed,
			 []() { g_settings = nullptr; });
	g_settings->open();
}

} // namespace obs_onvif::glue
