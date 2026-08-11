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
#include <QDoubleSpinBox>
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
#include <QSpinBox>
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
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
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

// Copies a QLineEdit/QComboBox text into a fixed-size ABI string buffer.
void CopyText(char *dst, size_t cap, const QString &s)
{
	if (!dst || !cap)
		return;
	const QByteArray bytes = s.toUtf8();
	const size_t n = (size_t)bytes.size() >= cap ? cap - 1 : (size_t)bytes.size();
	std::memcpy(dst, bytes.constData(), n);
	dst[n] = '\0';
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

// -- Config tab --------------------------------------------------------------

// Everything one load pass collected from the ABI for a camera.
struct ConfigSnapshot {
	bool streamOk = false;
	bool imageOk = false;
	bool netOk = false;
	bool osdOk = false;
	obs_cast_encoder_config_t enc{};
	obs_cast_encoder_options_t opt{};
	obs_cast_imaging_settings_t img{};
	obs_cast_imaging_options_t iopt{};
	std::vector<obs_cast_network_interface_t> netifs;
	std::vector<obs_cast_osd_config_t> osds;
};

class ConfigWidget : public QWidget {
public:
	explicit ConfigWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		cameras_ = new QComboBox(this);
		auto *cameraRow = new QHBoxLayout();
		cameraRow->addWidget(new QLabel(String("Dock.Config.Camera"), this));
		cameraRow->addWidget(cameras_, 1);
		connect(cameras_, &QComboBox::currentIndexChanged, this,
			[this](int) { LoadCurrent(); });

		tabs_ = new QTabWidget(this);
		tabs_->addTab(BuildImageTab(), String("Dock.Config.TabImage"));
		tabs_->addTab(BuildStreamTab(), String("Dock.Config.TabStream"));
		tabs_->addTab(BuildNetworkTab(), String("Dock.Config.TabNetwork"));
		tabs_->addTab(BuildOsdTab(), String("Dock.Config.TabOSD"));

		apply_ = new QPushButton(String("Dock.Config.Apply"), this);
		apply_->setEnabled(false);
		connect(apply_, &QPushButton::clicked, this,
			[this]() { ApplyChanges(); });

		status_ = new QLabel(this);
		status_->setWordWrap(true);

		auto *layout = new QVBoxLayout(this);
		layout->addLayout(cameraRow);
		layout->addWidget(tabs_, 1);
		layout->addWidget(apply_, 0, Qt::AlignLeft);
		layout->addWidget(status_);

		RefreshCameras();
	}

private:
	static QHBoxLayout *LabeledRow(QWidget *parent, const QString &label,
				       QWidget *field)
	{
		auto *row = new QHBoxLayout();
		row->addWidget(new QLabel(label, parent));
		row->addWidget(field, 1);
		return row;
	}

	static QDoubleSpinBox *MakeDouble(double min, double max)
	{
		auto *s = new QDoubleSpinBox();
		s->setRange(min, max);
		s->setSingleStep(1.0);
		s->setDecimals(1);
		return s;
	}

	QWidget *BuildImageTab()
	{
		imageForm_ = new QWidget(this);
		brightness_ = MakeDouble(0, 100);
		saturation_ = MakeDouble(0, 100);
		contrast_ = MakeDouble(0, 100);
		sharpness_ = MakeDouble(0, 100);
		auto *form = new QVBoxLayout(imageForm_);
		form->addLayout(LabeledRow(imageForm_, String("Dock.Config.Brightness"),
					   brightness_));
		form->addLayout(LabeledRow(imageForm_, String("Dock.Config.Saturation"),
					   saturation_));
		form->addLayout(LabeledRow(imageForm_, String("Dock.Config.Contrast"),
					   contrast_));
		form->addLayout(LabeledRow(imageForm_, String("Dock.Config.Sharpness"),
					   sharpness_));
		form->addStretch();

		imagingUnsupported_ = new QLabel(String("Dock.Config.Unsupported"));
		imagingUnsupported_->setWordWrap(true);

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addWidget(imageForm_);
		layout->addWidget(imagingUnsupported_);
		return tab;
	}

	QWidget *BuildStreamTab()
	{
		streamForm_ = new QWidget(this);
		streamInfo_ = new QLabel(streamForm_);
		streamInfo_->setWordWrap(true);
		resolutionCombo_ = new QComboBox(streamForm_);
		frameRate_ = MakeDouble(1, 30);
		bitrate_ = new QSpinBox(streamForm_);
		bitrate_->setRange(32, 8192);

		auto *form = new QVBoxLayout(streamForm_);
		form->addWidget(streamInfo_);
		form->addLayout(LabeledRow(streamForm_,
					   String("Dock.Config.Resolution"),
					   resolutionCombo_));
		form->addLayout(LabeledRow(streamForm_,
					   String("Dock.Config.FrameRate"),
					   frameRate_));
		form->addLayout(LabeledRow(streamForm_,
					   String("Dock.Config.Bitrate"),
					   bitrate_));
		form->addStretch();

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addWidget(streamForm_);
		return tab;
	}

	QWidget *BuildNetworkTab()
	{
		netForm_ = new QWidget(this);
		dhcp_ = new QCheckBox(String("Dock.Config.Dhcp"), netForm_);
		ip_ = new QLineEdit(netForm_);
		prefix_ = new QSpinBox(netForm_);
		prefix_->setRange(0, 32);

		auto *form = new QVBoxLayout(netForm_);
		form->addWidget(dhcp_);
		form->addLayout(LabeledRow(netForm_, String("Dock.Config.IP"), ip_));
		form->addLayout(LabeledRow(netForm_, String("Dock.Config.Prefix"),
					   prefix_));
		form->addStretch();

		netUnsupported_ = new QLabel(String("Dock.Config.Unsupported"));
		netUnsupported_->setWordWrap(true);
		connect(dhcp_, &QCheckBox::toggled, this, [this](bool on) {
			ip_->setEnabled(!on);
			prefix_->setEnabled(!on);
		});

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addWidget(netForm_);
		layout->addWidget(netUnsupported_);
		return tab;
	}

	QWidget *BuildOsdTab()
	{
		osdForm_ = new QWidget(this);
		osdEnabled_ = new QCheckBox(String("Dock.Config.OsdEnabled"), osdForm_);
		osdText_ = new QLineEdit(osdForm_);

		auto *form = new QVBoxLayout(osdForm_);
		form->addWidget(osdEnabled_);
		form->addLayout(LabeledRow(osdForm_, String("Dock.Config.OsdText"),
					   osdText_));
		form->addStretch();

		osdUnsupported_ = new QLabel(String("Dock.Config.Unsupported"));
		osdUnsupported_->setWordWrap(true);

		auto *tab = new QWidget(this);
		auto *layout = new QVBoxLayout(tab);
		layout->addWidget(osdForm_);
		layout->addWidget(osdUnsupported_);
		return tab;
	}

	void RefreshCameras()
	{
		PopulateCameraCombo(cameras_, /*onlineOnly=*/true);
	}

	void LoadCurrent()
	{
		const QString cam = cameras_->currentData().toString();
		apply_->setEnabled(false);
		status_->clear();
		if (cam.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();
		const int gen = ++loadGen_;
		std::thread([this, cameraId, gen]() {
			const ConfigSnapshot snap = LoadSnapshot(cameraId);
			QMetaObject::invokeMethod(
				this,
				[this, snap, gen]() { Populate(snap, gen); },
				Qt::QueuedConnection);
		}).detach();
	}

	static ConfigSnapshot LoadSnapshot(const std::string &cameraId)
	{
		ConfigSnapshot snap;
		obs_cast_abi_t *abi = obs_onvif_get_abi();
		if (!abi)
			return snap;
		const char *cam = cameraId.c_str();
		if (abi->get_encoder_config(cam, &snap.enc) == 0)
			snap.streamOk = true;
		abi->get_encoder_options(cam, &snap.opt);
		if (abi->get_imaging_settings(cam, &snap.img) == 0)
			snap.imageOk = true;
		abi->get_imaging_options(cam, &snap.iopt);
		obs_cast_network_interface_t *nifs = nullptr;
		int n = 0;
		if (abi->get_network_interfaces(cam, &nifs, &n) == 0 && nifs) {
			snap.netOk = true;
			for (int i = 0; i < n; ++i)
				snap.netifs.push_back(nifs[i]);
			if (abi->release_network_interfaces)
				abi->release_network_interfaces(nifs, n);
		}
		obs_cast_osd_config_t *osds = nullptr;
		int o = 0;
		if (abi->get_osds(cam, &osds, &o) == 0 && osds) {
			snap.osdOk = true;
			for (int i = 0; i < o; ++i)
				snap.osds.push_back(osds[i]);
			if (abi->release_osds)
				abi->release_osds(osds, o);
		}
		return snap;
	}

	void Populate(const ConfigSnapshot &snap, int gen)
	{
		if (gen != loadGen_)
			return; // a newer camera selection superseded this load
		streamOk_ = snap.streamOk;
		imageOk_ = snap.imageOk;
		netOk_ = snap.netOk;
		osdOk_ = snap.osdOk;
		const bool any = streamOk_ || imageOk_ || netOk_ || osdOk_;
		apply_->setEnabled(any);
		if (!any) {
			status_->setText(String("Dock.Config.Unsupported"));
			return;
		}

		enc_ = snap.enc;
		img_ = snap.img;
		net_ = snap.netifs.empty() ? obs_cast_network_interface_t{}
					  : snap.netifs.front();
		osd_ = snap.osds.empty() ? obs_cast_osd_config_t{}
					 : snap.osds.front();

		PopulateStream(snap);
		PopulateImage(snap);
		PopulateNetwork(snap);
		PopulateOsd(snap);
		status_->clear();
	}

	void PopulateStream(const ConfigSnapshot &snap)
	{
		streamInfo_->setText(
			QString::fromUtf8(snap.enc.encoding) + " " +
			QString::number(snap.enc.width) + "x" +
			QString::number(snap.enc.height) + " @ " +
			QString::number(snap.enc.frame_rate) + " fps, " +
			QString::number(snap.enc.bitrate) + " kbps");

		resolutions_.clear();
		resolutionCombo_->blockSignals(true);
		resolutionCombo_->clear();
		int sel = -1;
		for (int i = 0; i < snap.opt.resolution_count; ++i) {
			const int w = snap.opt.resolutions[i].width;
			const int h = snap.opt.resolutions[i].height;
			resolutions_.emplace_back(w, h);
			resolutionCombo_->addItem(
				QString::number(w) + "x" + QString::number(h), i);
			if (w == snap.enc.width && h == snap.enc.height)
				sel = i;
		}
		resolutionCombo_->setCurrentIndex(sel >= 0 ? sel : 0);
		resolutionCombo_->blockSignals(false);

		frameRate_->setRange(snap.opt.min_frame_rate > 0.0
						     ? snap.opt.min_frame_rate
						     : 1.0,
				     snap.opt.max_frame_rate > 0.0
					     ? snap.opt.max_frame_rate
					     : 30.0);
		frameRate_->setValue(snap.enc.frame_rate);
		bitrate_->setRange(snap.opt.min_bitrate > 0 ? snap.opt.min_bitrate
							   : 32,
				   snap.opt.max_bitrate > 0 ? snap.opt.max_bitrate
							    : 8192);
		bitrate_->setValue(snap.enc.bitrate);
	}

	void PopulateImage(const ConfigSnapshot &snap)
	{
		const bool supported = imageOk_ && snap.img.present;
		imageForm_->setVisible(supported);
		imagingUnsupported_->setVisible(!supported);
		if (!supported)
			return;
		brightness_->setRange(snap.iopt.min_brightness,
				      snap.iopt.max_brightness);
		brightness_->setValue(snap.img.brightness);
		saturation_->setRange(snap.iopt.min_color_saturation,
				      snap.iopt.max_color_saturation);
		saturation_->setValue(snap.img.color_saturation);
		contrast_->setRange(snap.iopt.min_contrast,
				    snap.iopt.max_contrast);
		contrast_->setValue(snap.img.contrast);
		sharpness_->setRange(snap.iopt.min_sharpness,
				     snap.iopt.max_sharpness);
		sharpness_->setValue(snap.img.sharpness);
	}

	void PopulateNetwork(const ConfigSnapshot &snap)
	{
		netForm_->setVisible(netOk_);
		netUnsupported_->setVisible(!netOk_);
		if (!netOk_)
			return;
		dhcp_->setChecked(net_.dhcp != 0);
		ip_->setText(QString::fromUtf8(net_.address));
		prefix_->setValue(net_.prefix_length);
		ip_->setEnabled(net_.dhcp == 0);
		prefix_->setEnabled(net_.dhcp == 0);
	}

	void PopulateOsd(const ConfigSnapshot &snap)
	{
		osdForm_->setVisible(osdOk_);
		osdUnsupported_->setVisible(!osdOk_);
		if (!osdOk_)
			return;
		osdEnabled_->setChecked(osd_.enabled != 0 || !snap.osds.empty());
		osdText_->setText(QString::fromUtf8(osd_.text));
		osdEnabled_->setEnabled(true);
		osdText_->setEnabled(true);
	}

	std::pair<int, int> CurrentResolution() const
	{
		const int idx = resolutionCombo_->currentIndex();
		if (idx >= 0 && idx < (int)resolutions_.size())
			return resolutions_[idx];
		return {0, 0};
	}

	void ApplyChanges()
	{
		const QString cam = cameras_->currentData().toString();
		if (cam.isEmpty())
			return;
		const std::string cameraId = cam.toStdString();

		obs_cast_encoder_config_t enc = enc_;
		const std::pair<int, int> res = CurrentResolution();
		enc.width = res.first;
		enc.height = res.second;
		enc.frame_rate = frameRate_->value();
		enc.bitrate = bitrate_->value();

		obs_cast_imaging_settings_t img = img_;
		img.present = 1;
		img.brightness = brightness_->value();
		img.color_saturation = saturation_->value();
		img.contrast = contrast_->value();
		img.sharpness = sharpness_->value();

		obs_cast_network_interface_t net = net_;
		net.dhcp = dhcp_->isChecked() ? 1 : 0;
		CopyText(net.address, sizeof net.address, ip_->text());
		net.prefix_length = prefix_->value();

		obs_cast_osd_config_t osd = osd_;
		osd.enabled = osdEnabled_->isChecked() ? 1 : 0;
		CopyText(osd.text, sizeof osd.text, osdText_->text());

		const bool doStream = streamOk_;
		const bool doImage = imageOk_ && img_.present;
		const bool doNet = netOk_;
		const bool doOsd = osdOk_;

		status_->setText(String("Dock.Config.Applying"));
		apply_->setEnabled(false);

		std::thread(
			[cameraId, enc, img, net, osd, doStream, doImage, doNet,
			 doOsd, this]() {
				bool ok = true;
				std::string err;
				obs_cast_abi_t *abi = obs_onvif_get_abi();
				const char *cam = cameraId.c_str();
				if (abi && doStream &&
				    abi->set_encoder_config(cam, &enc) != 0) {
					ok = false;
					err = "stream";
				}
				if (abi && doImage &&
				    abi->set_imaging_settings(cam, &img) != 0) {
					ok = false;
					err = "image";
				}
				if (abi && doNet &&
				    abi->set_network_interface(cam, &net) != 0) {
					ok = false;
					err = "network";
				}
				if (abi && doOsd) {
					if (osd.enabled) {
						if (abi->set_osd(cam, &osd) != 0) {
							ok = false;
							err = "osd";
						}
					} else {
						const std::string token =
							osd.token;
						if (abi->delete_osd(
							    cam, token.c_str()) !=
						    0) {
							ok = false;
							err = "osd";
						}
					}
				}
				QMetaObject::invokeMethod(
					this,
					[this, ok, err]() {
						apply_->setEnabled(true);
						status_->setText(
							ok
								? String(
									  "Dock.Config.Applied")
								: String("Dock.Config.ApplyFailed")
									  .arg(FromUtf8(err)));
						LoadCurrent();
					},
					Qt::QueuedConnection);
			})
			.detach();
	}

	QComboBox *cameras_;
	QTabWidget *tabs_;
	QPushButton *apply_;
	QLabel *status_;
	QWidget *imageForm_;
	QLabel *imagingUnsupported_;
	QDoubleSpinBox *brightness_;
	QDoubleSpinBox *saturation_;
	QDoubleSpinBox *contrast_;
	QDoubleSpinBox *sharpness_;
	QWidget *streamForm_;
	QLabel *streamInfo_;
	QComboBox *resolutionCombo_;
	QDoubleSpinBox *frameRate_;
	QSpinBox *bitrate_;
	QWidget *netForm_;
	QLabel *netUnsupported_;
	QCheckBox *dhcp_;
	QLineEdit *ip_;
	QSpinBox *prefix_;
	QWidget *osdForm_;
	QLabel *osdUnsupported_;
	QCheckBox *osdEnabled_;
	QLineEdit *osdText_;
	int loadGen_ = 0;
	std::vector<std::pair<int, int>> resolutions_;
	obs_cast_encoder_config_t enc_{};
	obs_cast_imaging_settings_t img_{};
	obs_cast_network_interface_t net_{};
	obs_cast_osd_config_t osd_{};
	bool streamOk_ = false;
	bool imageOk_ = false;
	bool netOk_ = false;
	bool osdOk_ = false;
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
	tabs->addTab(new ConfigWidget(tabs), String("Dock.TabConfig"));
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