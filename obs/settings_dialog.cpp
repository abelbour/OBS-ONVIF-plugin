// Lean module settings dialog (obs/settings_dialog.cpp). Qt-only; added to
// the module by CMake when ENABLE_QT is set.
//
// The dock already covers per-camera Cameras / Sources / Presets / PTZ /
// Config / Policy; this dialog holds the global application settings that the
// dock does not expose:
//   General   — default stream quality, apply-prompt timeout, discovery
//               interval, SOAP timeout
//   Discovery — continuous Hello/Bye listener toggle, probe timeout
//   Log       — tail of the newest OBS session log + path hints
//   About     — name/version/config-directory info
#include "settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <obs-module.h>
#include <plugin-support.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

		auto *form = new QFormLayout();
		form->addRow(String("Settings.DefaultStream"), stream_);
		form->addRow(String("Settings.PromptTimeout"), promptTimeout_);
		form->addRow(String("Settings.DiscoveryInterval"), discoveryInterval_);
		form->addRow(String("Settings.SoapTimeout"), soapTimeout_);

		auto *tab = new QWidget(this);
		tab->setLayout(form);
		return tab;
	}

	QWidget *BuildDiscoveryTab()
	{
		helloEnabled_ =
			new QCheckBox(String("Settings.HelloListener"), this);
		probeTimeout_ = new QSpinBox(this);
		probeTimeout_->setRange(1, 30);
		probeTimeout_->setSuffix(" s");

		auto *form = new QFormLayout();
		form->addRow(helloEnabled_);
		form->addRow(String("Settings.ProbeTimeout"), probeTimeout_);

		auto *tab = new QWidget(this);
		tab->setLayout(form);
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
		status_->setText(store.SaveAppConfig(cfg)
					 ? String("Settings.Saved")
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
