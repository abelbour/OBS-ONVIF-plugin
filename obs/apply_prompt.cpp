// Apply/Defer/Ignore live-output prompt (obs/apply_prompt.cpp). Qt-only; the
// CMake glue adds this translation unit to the module when ENABLE_QT is set.
//
// Started by the apply-policy dispatch seam (obs_apply::OnCameraMoved) when a
// discovery contact resolves a moved camera while stream/record outputs are
// running. Modal-but-asynchronous (QDialog::open) so the countdown timer and
// the rest of OBS keep running. Every path resolves the pending incident
// through obs_onvif::registry::ApplyPolicy; source rewrites are applied on
// this (main) thread via obs_onvif::glue::ApplySourceRewrites.
#include "apply_prompt.h"

#include <QCheckBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-module.h>

#include "obs_apply.h"

namespace obs_onvif::glue {

namespace {

// Matches AppConfig::prompt_timeout_s (30) in registry/camera.h.
constexpr int kPromptTimeoutS = 30;

class ApplyPromptDialog : public QDialog {
public:
	explicit ApplyPromptDialog(QWidget *parent = nullptr)
		: QDialog(parent), remaining_(kPromptTimeoutS), timer_(nullptr)
	{
		setModal(true);
		setAttribute(Qt::WA_DeleteOnClose);

		auto *message = new QLabel(MessageText(), this);
		message->setWordWrap(true);

		countdown_ = new QLabel(this);
		UpdateCountdown();

		remember_ = new QCheckBox(
			obs_module_text("ApplyPrompt.Remember"), this);

		auto *applyButton = new QPushButton(
			obs_module_text("ApplyPrompt.Apply"), this);
		auto *deferButton = new QPushButton(
			obs_module_text("ApplyPrompt.Defer"), this);
		auto *ignoreButton = new QPushButton(
			obs_module_text("ApplyPrompt.Ignore"), this);
		applyButton->setDefault(true);

		connect(applyButton, &QPushButton::clicked, this,
			&ApplyPromptDialog::OnApply);
		connect(deferButton, &QPushButton::clicked, this,
			&ApplyPromptDialog::OnDefer);
		connect(ignoreButton, &QPushButton::clicked, this,
			&ApplyPromptDialog::OnIgnore);

		auto *buttons = new QHBoxLayout();
		buttons->addWidget(applyButton);
		buttons->addWidget(deferButton);
		buttons->addWidget(ignoreButton);
		buttons->addStretch();

		auto *layout = new QVBoxLayout(this);
		layout->addWidget(message);
		layout->addSpacing(6);
		layout->addWidget(countdown_, 0, Qt::AlignLeft);
		layout->addWidget(remember_);
		layout->addSpacing(6);
		layout->addLayout(buttons);

		timer_ = new QTimer(this);
		timer_->setInterval(1000);
		connect(timer_, &QTimer::timeout, this,
			&ApplyPromptDialog::OnTick);
		timer_->start();
	}

private:
	QString MessageText() const
	{
		const obs_onvif::registry::ApplyPolicy &policy =
			ApplyPolicyInstance();
		const std::string camera = policy.PendingCamera();

		QString sources;
		const std::vector<std::string> names = policy.PendingSources();
		if (names.empty()) {
			sources = obs_module_text("ApplyPrompt.NoSources");
		} else {
			for (size_t i = 0; i < names.size(); ++i) {
				if (i)
					sources += QString(", ");
				sources += QString::fromUtf8(names[i].c_str());
			}
		}

		const QString fmt = obs_module_text("ApplyPrompt.Body");
		return fmt.arg(QString::fromUtf8(camera.c_str()), sources);
	}

	void UpdateCountdown()
	{
		const QString fmt = obs_module_text("ApplyPrompt.Countdown");
		countdown_->setText(fmt.arg(remaining_));
	}

	void OnApply()
	{
		std::vector<obs_onvif::registry::SourceRewrite> rewrites;
		if (ApplyPolicyInstance().ApplyPending(rewrites) ==
			    obs_onvif::registry::ApplyDecision::AppliedNow &&
		    !rewrites.empty())
			ApplySourceRewrites(rewrites);
		close();
	}

	void OnDefer()
	{
		ApplyPolicyInstance().DeferPending();
		close();
	}

	void OnIgnore()
	{
		const bool remember = remember_->isChecked();
		ApplyPolicyInstance().IgnorePending(remember);
		close();
	}

	void OnTick()
	{
		if (--remaining_ > 0) {
			UpdateCountdown();
			return;
		}
		/* Stale prompt: auto-defer; the rewrite is re-offered once the
		 * outputs go inactive (SetOutputActive -> OnOutputsIdle). */
		ApplyPolicyInstance().OnPromptTimeout();
		close();
	}

	QCheckBox *remember_;
	QLabel *countdown_;
	int remaining_;
	QTimer *timer_;
};

ApplyPromptDialog *g_prompt = nullptr;

} // namespace

void ShowApplyPrompt()
{
	if (!ApplyPolicyInstance().HasPending())
		return;
	if (g_prompt)
		return;
	g_prompt = new ApplyPromptDialog();
	QObject::connect(g_prompt, &QObject::destroyed, []() { g_prompt = nullptr; });
	g_prompt->open();
}

} // namespace obs_onvif::glue
