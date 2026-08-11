// ONVIF Control settings dock (obs/dock.cpp). Qt-only; added to the module by
// CMake when ENABLE_QT is set.
//
// Four tabs wired to the public ABI (obs_onvif_get_abi) and the per-scene-
// collection store:
//   Cameras  — snapshot of known cameras (name / online / XAddr)
//   Sources  — RTSP Media Sources in the scene tree ↔ camera + auto-apply
//   Presets  — full preset lifecycle for the selected camera
//   Policy   — default live-output apply policy (persisted)
// PTZ pad + camera Config (Image/Stream/OSD/Network) panels are M3f follow-up
// work (they need the capabilities cache from the M2→M3 discovery bridge).
#include "dock.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QObject>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <map>

#include "obs-onvif.h"
#include "obs_apply.h"
#include "obs_mapping.h"
#include "store.h"

namespace obs_onvif::glue {
namespace {

using namespace obs_onvif::registry;

const char *kDockId = "obs-onvif_control_dock";

obs_cast_abi_t *Abi()
{
	return obs_onvif_get_abi();
}

// Module-locale string; obs_module_text returns the key itself when absent.
QString String(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString FromUtf8(const std::string &s)
{
	return QString::fromUtf8(s.c_str());
}

// Fills a camera combo with the ABI camera table (id as user data). When
// `onlineOnly` is set, offline cameras are skipped (PTZ operates live).
void PopulateCameraCombo(QComboBox *combo, bool onlineOnly)
{
	const QString prev = combo->currentData().toString();
	combo->blockSignals(true);
	combo->clear();
	obs_cast_abi_t *abi = Abi();
	if (abi) {
		obs_cast_camera_info_t *arr = nullptr;
		int n = 0;
		if (abi->get_camera_list(&arr, &n) == 0 && arr) {
			for (int i = 0; i < n; ++i) {
				if (onlineOnly && !arr[i].online)
					continue;
				combo->addItem(FromUtf8(arr[i].name),
					       FromUtf8(arr[i].camera_id));
			}
			if (abi->release_camera_list)
				abi->release_camera_list(arr);
		}
	}
	const int idx = combo->findData(prev);
	if (idx >= 0)
		combo->setCurrentIndex(idx);
	combo->blockSignals(false);
}

// -- Cameras tab -------------------------------------------------------------

class CamerasWidget : public QWidget {
public:
	explicit CamerasWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		table_ = new QTableWidget(0, 3, this);
		table_->setHorizontalHeaderLabels(
			{QStringLiteral("Name"), String("Dock.Camera.Online"),
			 QStringLiteral("XAddr")});
		table_->horizontalHeader()->setStretchLastSection(true);
		table_->verticalHeader()->setVisible(false);
		table_->setSelectionBehavior(QAbstractItemView::SelectRows);
		table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

		auto *refresh = new QPushButton(String("Dock.Refresh"), this);
		connect(refresh, &QPushButton::clicked, this,
			&CamerasWidget::Refresh);

		auto *layout = new QVBoxLayout(this);
		layout->addWidget(table_);
		layout->addWidget(refresh, 0, Qt::AlignLeft);

		Refresh();
	}

private:
	void Refresh()
	{
		obs_cast_abi_t *abi = Abi();
		table_->setRowCount(0);
		if (!abi)
			return;
		obs_cast_camera_info_t *cams = nullptr;
		int n = 0;
		if (abi->get_camera_list(&cams, &n) != 0 || !cams)
			return;
		table_->setRowCount(n);
		for (int i = 0; i < n; ++i) {
			auto *name = new QTableWidgetItem(FromUtf8(cams[i].name));
			auto *online = new QTableWidgetItem(
				cams[i].online ? String("Dock.Yes")
					       : String("Dock.No"));
			auto *addr =
				new QTableWidgetItem(FromUtf8(cams[i].xaddr));
			table_->setItem(i, 0, name);
			table_->setItem(i, 1, online);
			table_->setItem(i, 2, addr);
		}
		if (abi->release_camera_list)
			abi->release_camera_list(cams);
	}

	QTableWidget *table_;
};

// -- Sources tab -------------------------------------------------------------

struct CameraOption {
	std::string id;
	std::string name;
};

class SourcesWidget : public QWidget {
public:
	explicit SourcesWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		table_ = new QTableWidget(0, 4, this);
		table_->setHorizontalHeaderLabels(
			{QStringLiteral("Source"), QStringLiteral("URL"),
			 String("Dock.Source.Camera"),
			 String("Dock.Source.Auto")});
		table_->horizontalHeader()->setStretchLastSection(true);
		table_->verticalHeader()->setVisible(false);
		table_->setSelectionBehavior(QAbstractItemView::SelectRows);

		auto *refresh = new QPushButton(String("Dock.Refresh"), this);
		auto *save = new QPushButton(String("Dock.Sources.Save"), this);
		connect(refresh, &QPushButton::clicked, this,
			&SourcesWidget::Refresh);
		connect(save, &QPushButton::clicked, this,
			&SourcesWidget::Save);

		auto *buttons = new QHBoxLayout();
		buttons->addWidget(refresh);
		buttons->addWidget(save);
		buttons->addStretch();

		auto *layout = new QVBoxLayout(this);
		layout->addWidget(table_);
		layout->addLayout(buttons);

		Refresh();
	}

private:
	void CleanCellWidgets()
	{
		for (int i = 0; i < table_->rowCount(); ++i) {
			table_->removeCellWidget(i, 2);
			table_->removeCellWidget(i, 3);
		}
	}

	void Refresh()
	{
		std::vector<SourceMapping> prev;
		if (!SceneCollection().empty()) {
			Store store(ConfigDir());
			CollectionState cs;
			if (store.LoadCollection(SceneCollection(), cs))
				prev = cs.mappings;
		}
		const std::vector<RtspSource> sources = DiscoverRtspSources();

		std::vector<CameraOption> cams;
		obs_cast_abi_t *abi = Abi();
		if (abi) {
			obs_cast_camera_info_t *arr = nullptr;
			int n = 0;
			if (abi->get_camera_list(&arr, &n) == 0 && arr) {
				for (int i = 0; i < n; ++i)
					cams.push_back(
						{arr[i].camera_id ? arr[i].camera_id
								  : "",
						 arr[i].name ? arr[i].name : ""});
				if (abi->release_camera_list)
					abi->release_camera_list(arr);
			}
		}

		CleanCellWidgets();
		table_->setRowCount((int)sources.size());
		for (size_t i = 0; i < sources.size(); ++i) {
			const int row = (int)i;
			table_->setItem(row, 0,
					new QTableWidgetItem(
						FromUtf8(sources[i].name)));
			table_->setItem(row, 1,
					new QTableWidgetItem(
						FromUtf8(sources[i].url)));

			auto *combo = new QComboBox(table_);
			combo->addItem(String("Dock.Source.None"));
			for (const auto &c : cams)
				combo->addItem(FromUtf8(c.name), FromUtf8(c.id));
			table_->setCellWidget(row, 2, combo);

			bool autoApply = true;
			for (const auto &m : prev) {
				if (m.source_name == sources[i].name) {
					const int idx = combo->findData(
						FromUtf8(m.camera_id));
					if (idx >= 0)
						combo->setCurrentIndex(idx);
					autoApply = m.auto_apply;
					break;
				}
			}
			auto *check = new QCheckBox(table_);
			check->setChecked(autoApply);
			table_->setCellWidget(row, 3, check);
		}
	}

	void Save()
	{
		const std::string uuid = SceneCollection();
		if (uuid.empty()) {
			obs_log(LOG_WARNING,
				"obs-onvif: no active scene collection; "
				"cannot save source mappings");
			return;
		}
		Store store(ConfigDir());
		CollectionState cs;
		cs.uuid = uuid;
		cs.display_name = uuid;
		(void)store.LoadCollection(uuid, cs); // keep scene_presets
		const std::vector<SourceMapping> prev = cs.mappings;

		cs.mappings.clear();
		for (int row = 0; row < table_->rowCount(); ++row) {
			auto *combo = qobject_cast<QComboBox *>(
				table_->cellWidget(row, 2));
			if (!combo)
				continue;
			const QString cam = combo->currentData().toString();
			if (cam.isEmpty())
				continue;
			auto *nameItem = table_->item(row, 0);
			if (!nameItem)
				continue;
			SourceMapping m;
			m.collection_uuid = uuid;
			m.source_name = nameItem->text().toStdString();
			m.camera_id = cam.toStdString();
			for (const auto &old : prev) {
				if (old.source_name == m.source_name) {
					m.profileToken = old.profileToken;
					break;
				}
			}
			auto *check = qobject_cast<QCheckBox *>(
				table_->cellWidget(row, 3));
			m.auto_apply = !check || check->isChecked();
			cs.mappings.push_back(m);
		}
		if (!store.SaveCollection(cs)) {
			obs_log(LOG_ERROR,
				"obs-onvif: failed to persist source mappings");
			return;
		}
		ReseedApplyState();
	}

	QTableWidget *table_;
};

// -- Presets tab -------------------------------------------------------------

struct PresetEntry {
	std::string token;
	std::string name;
};

class PresetsWidget : public QWidget {
public:
	explicit PresetsWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		cameras_ = new QComboBox(this);
		auto *cameraRow = new QHBoxLayout();
		cameraRow->addWidget(new QLabel(String("Dock.Presets.Camera"),
						this));
		cameraRow->addWidget(cameras_, 1);
		connect(cameras_, &QComboBox::currentIndexChanged, this,
			[this](int) { RefreshPresets(); });

		list_ = new QListWidget(this);
		list_->setSelectionMode(QAbstractItemView::SingleSelection);

		auto *refresh = new QPushButton(String("Dock.Refresh"), this);
		auto *goTo = new QPushButton(String("Dock.Presets.Goto"), this);
		auto *save = new QPushButton(String("Dock.Presets.Save"), this);
		auto *rename = new QPushButton(String("Dock.Presets.Rename"),
					       this);
		auto *remove = new QPushButton(String("Dock.Presets.Delete"),
					       this);

		connect(refresh, &QPushButton::clicked, this,
			[this]() { RefreshPresets(); });
		connect(goTo, &QPushButton::clicked, this,
			[this]() { GotoSelected(); });
		connect(save, &QPushButton::clicked, this,
			[this]() { SaveCurrent(); });
		connect(rename, &QPushButton::clicked, this,
			[this]() { RenameSelected(); });
		connect(remove, &QPushButton::clicked, this,
			[this]() { DeleteSelected(); });

		auto *buttons = new QHBoxLayout();
		buttons->addWidget(refresh);
		buttons->addWidget(goTo);
		buttons->addWidget(save);
		buttons->addWidget(rename);
		buttons->addWidget(remove);
		buttons->addStretch();

		auto *bindScene = new QPushButton(
			String("Dock.Presets.BindScene"), this);
		auto *clearScene = new QPushButton(
			String("Dock.Presets.ClearScene"), this);
		status_ = new QLabel(this);
		status_->setWordWrap(true);
		connect(bindScene, &QPushButton::clicked, this,
			[this]() { BindToCurrentScene(); });
		connect(clearScene, &QPushButton::clicked, this,
			[this]() { ClearSceneBinding(); });

		auto *binding = new QHBoxLayout();
		binding->addWidget(bindScene);
		binding->addWidget(clearScene);
		binding->addStretch();

		auto *layout = new QVBoxLayout(this);
		layout->addLayout(cameraRow);
		layout->addWidget(list_, 1);
		layout->addLayout(buttons);
		layout->addLayout(binding);
		layout->addWidget(status_);

		RefreshCameras();
	}

private:
	QString CurrentCameraId() const
	{
		return cameras_->currentData().toString();
	}

	void RefreshCameras()
	{
		const QString prev = CurrentCameraId();
		cameras_->blockSignals(true);
		cameras_->clear();
		obs_cast_abi_t *abi = Abi();
		if (abi) {
			obs_cast_camera_info_t *arr = nullptr;
			int n = 0;
			if (abi->get_camera_list(&arr, &n) == 0 && arr) {
				for (int i = 0; i < n; ++i)
					cameras_->addItem(
						FromUtf8(arr[i].name),
						FromUtf8(arr[i].camera_id));
				if (abi->release_camera_list)
					abi->release_camera_list(arr);
			}
		}
		const int idx = cameras_->findData(prev);
		if (idx >= 0)
			cameras_->setCurrentIndex(idx);
		cameras_->blockSignals(false);
		RefreshPresets();
	}

	void RefreshPresets()
	{
		const QString cam = CurrentCameraId();
		list_->clear();
		if (cam.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();
		std::thread([this, cameraId]() {
			obs_cast_abi_t *abi = Abi();
			std::vector<PresetEntry> entries;
			if (abi) {
				const char **names = nullptr;
				const char **tokens = nullptr;
				int n = 0;
				if (abi->list_presets(cameraId.c_str(), &names,
						      &tokens, &n) == 0) {
					for (int i = 0; i < n; ++i)
						entries.push_back(
							{tokens[i] ? tokens[i]
								   : "",
							 names[i] ? names[i]
								   : ""});
					if (abi->release_presets)
						abi->release_presets(names,
								     tokens, n);
				}
			}
			QMetaObject::invokeMethod(
				this,
				[this, entries]() { PopulatePresets(entries); },
				Qt::QueuedConnection);
		}).detach();
	}

	void PopulatePresets(const std::vector<PresetEntry> &entries)
	{
		list_->clear();
		for (const auto &e : entries) {
			auto *item = new QListWidgetItem(FromUtf8(e.name), list_);
			item->setData(Qt::UserRole, FromUtf8(e.token));
		}
	}

	QString SelectedToken() const
	{
		QListWidgetItem *item = list_->currentItem();
		return item ? item->data(Qt::UserRole).toString() : QString();
	}

	void GotoSelected()
	{
		const QString cam = CurrentCameraId();
		const QString token = SelectedToken();
		if (cam.isEmpty() || token.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();
		const std::string presetToken = token.toStdString();
		std::thread([cameraId, presetToken]() {
			obs_cast_abi_t *abi = Abi();
			if (abi)
				abi->goto_preset(cameraId.c_str(),
						 presetToken.c_str());
		}).detach();
	}

	void SaveCurrent()
	{
		const QString cam = CurrentCameraId();
		if (cam.isEmpty())
			return;
		bool ok = false;
		const QString name = QInputDialog::getText(
			this, String("Dock.Presets.Save"),
			String("Dock.Presets.SavePrompt"), QLineEdit::Normal,
			QString(), &ok);
		if (!ok || name.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();
		const std::string presetName = name.toStdString();
		std::thread([cameraId, presetName]() {
			obs_cast_abi_t *abi = Abi();
			if (abi) {
				char token[256] = {};
				abi->save_preset(cameraId.c_str(),
						 presetName.c_str(), token,
						 sizeof(token));
			}
		}).detach();
		RefreshPresets();
	}

	void RenameSelected()
	{
		const QString cam = CurrentCameraId();
		const QString token = SelectedToken();
		if (cam.isEmpty() || token.isEmpty())
			return;
		bool ok = false;
		const QString name = QInputDialog::getText(
			this, String("Dock.Presets.Rename"),
			String("Dock.Presets.RenamePrompt"), QLineEdit::Normal,
			QString(), &ok);
		if (!ok || name.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();
		const std::string presetToken = token.toStdString();
		const std::string newName = name.toStdString();
		std::thread([cameraId, presetToken, newName]() {
			obs_cast_abi_t *abi = Abi();
			if (abi)
				abi->rename_preset(cameraId.c_str(),
						   presetToken.c_str(),
						   newName.c_str());
		}).detach();
		RefreshPresets();
	}

	void DeleteSelected()
	{
		const QString cam = CurrentCameraId();
		const QString token = SelectedToken();
		if (cam.isEmpty() || token.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();
		const std::string presetToken = token.toStdString();
		std::thread([cameraId, presetToken]() {
			obs_cast_abi_t *abi = Abi();
			if (abi)
				abi->delete_preset(cameraId.c_str(),
						   presetToken.c_str());
		}).detach();
		RefreshPresets();
	}

	std::string CurrentSceneName() const
	{
		obs_source_t *scene = obs_frontend_get_current_scene();
		if (!scene)
			return {};
		const char *name = obs_source_get_name(scene);
		const std::string result = name ? name : "";
		obs_source_release(scene);
		return result;
	}

	// Binds the selected camera+preset to the current scene (fired by
	// scene_presets on scene activation). set_binding is a per-collection
	// store write — fast enough for the UI thread.
	void BindToCurrentScene()
	{
		const std::string scene = CurrentSceneName();
		const QString cam = CurrentCameraId();
		const QString token = SelectedToken();
		if (scene.empty()) {
			status_->setText(String("Dock.Presets.NoScene"));
			return;
		}
		if (cam.isEmpty() || token.isEmpty()) {
			status_->setText(String("Dock.Presets.NoPreset"));
			return;
		}
		obs_cast_abi_t *abi = Abi();
		if (abi)
			abi->set_binding(scene.c_str(), cam.toStdString().c_str(),
					 token.toStdString().c_str());
		status_->setText(
			String("Dock.Presets.Bound").arg(FromUtf8(scene)));
	}

	void ClearSceneBinding()
	{
		const std::string scene = CurrentSceneName();
		if (scene.empty()) {
			status_->setText(String("Dock.Presets.NoScene"));
			return;
		}
		obs_cast_abi_t *abi = Abi();
		if (abi)
			abi->clear_binding(scene.c_str());
		status_->setText(
			String("Dock.Presets.Cleared").arg(FromUtf8(scene)));
	}

	QComboBox *cameras_;
	QListWidget *list_;
	QLabel *status_;
};

// -- Policy tab --------------------------------------------------------------

class PolicyWidget : public QWidget {
public:
	explicit PolicyWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		auto *label = new QLabel(String("Dock.Policy.Default"), this);
		policy_ = new QComboBox(this);
		policy_->addItem(String("Dock.Policy.Ask"),
				 QVariant((int)ApplyPolicyChoice::Ask));
		policy_->addItem(String("Dock.Policy.Always"),
				 QVariant((int)ApplyPolicyChoice::Always));
		policy_->addItem(String("Dock.Policy.Ignore"),
				 QVariant((int)ApplyPolicyChoice::Ignore));

		connect(policy_, &QComboBox::currentIndexChanged, this,
			[this](int) { OnPolicyChanged(); });

		auto *row = new QHBoxLayout();
		row->addWidget(label);
		row->addWidget(policy_, 1);
		row->addStretch();

		auto *layout = new QVBoxLayout(this);
		layout->addLayout(row);
		auto *hint = new QLabel(String("Dock.Policy.Hint"), this);
		hint->setWordWrap(true);
		layout->addWidget(hint);
		layout->addStretch();

		LoadState();
	}

private:
	void LoadState()
	{
		Store store(ConfigDir());
		AppConfig cfg;
		store.LoadAppConfig(cfg);

		policy_->blockSignals(true);
		const int idx =
			policy_->findData(QVariant((int)cfg.apply_policy));
		policy_->setCurrentIndex(idx >= 0 ? idx : 0);
		policy_->blockSignals(false);

		ApplyPolicyInstance().SetDefaultPolicy(cfg.apply_policy);
		/* Replay remembered per-camera overrides into the policy. */
		std::map<std::string, ApplyPolicyChoice> policies;
		if (store.LoadCameraPolicies(policies)) {
			for (const auto &kv : policies)
				ApplyPolicyInstance().SetCameraPolicy(
					kv.first, kv.second, true);
		}
	}

	void OnPolicyChanged()
	{
		const ApplyPolicyChoice c =
			(ApplyPolicyChoice)policy_->currentData().toInt();
		ApplyPolicyInstance().SetDefaultPolicy(c);

		Store store(ConfigDir());
		AppConfig cfg;
		store.LoadAppConfig(cfg);
		cfg.apply_policy = c;
		store.SaveAppConfig(cfg);
	}

	QComboBox *policy_;
};

// -- PTZ pad -----------------------------------------------------------------

class PtzWidget : public QWidget {
public:
	explicit PtzWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		cameras_ = new QComboBox(this);
		auto *cameraRow = new QHBoxLayout();
		cameraRow->addWidget(new QLabel(String("Dock.Ptz.Camera"), this));
		cameraRow->addWidget(cameras_, 1);

		auto *pad = new QGridLayout();
		pad->setSpacing(4);
		pad->addWidget(PadButton("Dock.Ptz.Up", 0.0, -0.3, 0.0), 0, 1);
		pad->addWidget(PadButton("Dock.Ptz.Left", -0.3, 0.0, 0.0), 1, 0);
		pad->addWidget(StopButton(), 1, 1);
		pad->addWidget(PadButton("Dock.Ptz.Right", 0.3, 0.0, 0.0), 1, 2);
		pad->addWidget(PadButton("Dock.Ptz.Down", 0.0, 0.3, 0.0), 2, 1);

		auto *zoom = new QHBoxLayout();
		zoom->addWidget(PadButton("Dock.Ptz.ZoomIn", 0.0, 0.0, 0.2));
		zoom->addWidget(PadButton("Dock.Ptz.ZoomOut", 0.0, 0.0, -0.2));
		zoom->addStretch();

		auto *refresh = new QPushButton(String("Dock.Refresh"), this);
		connect(refresh, &QPushButton::clicked, this,
			[this]() { RefreshCameras(); });

		auto *layout = new QVBoxLayout(this);
		layout->addLayout(cameraRow);
		layout->addLayout(pad);
		layout->addLayout(zoom);
		layout->addWidget(refresh, 0, Qt::AlignLeft);
		layout->addStretch();

		RefreshCameras();
	}

	~PtzWidget() override
	{
		if (held_)
			held_->store(false);
	}

private:
	QPushButton *PadButton(const char *key, double pan, double tilt,
			       double zoom)
	{
		auto *b = new QPushButton(String(key), this);
		b->setMinimumHeight(28);
		connect(b, &QPushButton::pressed, this,
			[this, pan, tilt, zoom]() { StartAxis(pan, tilt, zoom); });
		connect(b, &QPushButton::released, this,
			[this]() { StopAxis(); });
		return b;
	}

	QPushButton *StopButton()
	{
		auto *b = new QPushButton(String("Dock.Ptz.Stop"), this);
		b->setMinimumHeight(28);
		connect(b, &QPushButton::clicked, this,
			[this]() { StopAxis(); });
		return b;
	}

	QString CurrentCameraId() const
	{
		return cameras_->currentData().toString();
	}

	void RefreshCameras()
	{
		PopulateCameraCombo(cameras_, /*onlineOnly=*/true);
	}

	void StartAxis(double pan, double tilt, double zoom)
	{
		const QString cam = CurrentCameraId();
		if (cam.isEmpty())
			return;
		if (held_)
			held_->store(false); // stop any prior hold
		auto held = std::make_shared<std::atomic<bool>>(true);
		held_ = held;
		const std::string cameraId = cam.toStdString();
		std::thread([held, cameraId, pan, tilt, zoom]() {
			obs_cast_abi_t *abi = obs_onvif_get_abi();
			if (!abi)
				return;
			/* Re-issue the velocity move while the button is held;
			 * the worker gives each SOAP call a short timeout. */
			while (held->load()) {
				abi->move(cameraId.c_str(), pan, tilt, zoom);
				std::this_thread::sleep_for(
					std::chrono::milliseconds(300));
			}
		}).detach();
	}

	void StopAxis()
	{
		if (held_) {
			held_->store(false);
			held_.reset();
		}
		const QString cam = CurrentCameraId();
		if (cam.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();
		std::thread([cameraId]() {
			obs_cast_abi_t *abi = obs_onvif_get_abi();
			if (abi)
				abi->stop(cameraId.c_str());
		}).detach();
	}

	QComboBox *cameras_;
	std::shared_ptr<std::atomic<bool>> held_;
};

// -- Dock lifecycle ----------------------------------------------------------

bool g_dock_loaded = false;

void ToggleDockVisibility()
{
	QMainWindow *main = static_cast<QMainWindow *>(
		obs_frontend_get_main_window());
	if (!main)
		return;
	QWidget *dock = main->findChild<QWidget *>(kDockId);
	if (!dock)
		return;
	dock->setVisible(!dock->isVisible());
	if (dock->isVisible())
		dock->raise();
}

} // namespace

void LoadDock()
{
	if (g_dock_loaded)
		return;
	auto *tabs = new QTabWidget();
	tabs->addTab(new CamerasWidget(tabs), String("Dock.TabCameras"));
	tabs->addTab(new SourcesWidget(tabs), String("Dock.TabSources"));
	tabs->addTab(new PresetsWidget(tabs), String("Dock.TabPresets"));
	tabs->addTab(new PtzWidget(tabs), String("Dock.TabPtz"));
	tabs->addTab(new PolicyWidget(tabs), String("Dock.TabPolicy"));

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Dock.Title"),
					 tabs)) {
		obs_log(LOG_WARNING,
			"obs-onvif: could not add control dock (id in use?)");
		delete tabs;
		return;
	}
	QAction *toggle = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction(
			obs_module_text("Dock.MenuItem")));
	if (toggle)
		QObject::connect(toggle, &QAction::triggered, toggle,
				 ToggleDockVisibility);
	g_dock_loaded = true;
}

void UnloadDock()
{
	if (!g_dock_loaded)
		return;
	obs_frontend_remove_dock(kDockId);
	g_dock_loaded = false;
}

} // namespace obs_onvif::glue