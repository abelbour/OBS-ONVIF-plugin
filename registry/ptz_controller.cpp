#include "ptz_controller.h"

#include <utility>

namespace obs_onvif::registry {

PtzController::PtzController(Executor exec, PtzSettings settings)
	: exec_(std::move(exec)), settings_(settings)
{
	thread_ = std::thread([this] { Loop(); });
}

PtzController::~PtzController()
{
	Shutdown();
}

void PtzController::SetSettings(const PtzSettings &s)
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		settings_ = s;
	}
	cv_.notify_all();
}

void PtzController::Move(const std::string &cameraId, double pan, double tilt,
			 double zoom)
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		CamState &s = states_[cameraId];
		s.held = true;
		s.heldCmd = PtzCommand{pan, tilt, zoom, false,
				       settings_.moveTimeoutSeconds};
		// Re-arm dispatch: an explicit Move() always (re)issues the
		// command, even if it is byte-identical to the last one. The loop
		// clears this flag when it picks, so without a fresh Move() the
		// same held command is never re-dispatched.
		s.hasDispatched = false;
		s.stopPending = false;
	}
	cv_.notify_all();
}

void PtzController::Stop(const std::string &cameraId)
{
	bool abortInFlight = false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		CamState &s = states_[cameraId];
		s.held = false;         // release: no further moves
		s.stopPending = true;   // Stop dispatches before any queued move
		s.heldCmd = {};
		s.hasDispatched = false;
		abortInFlight = settings_.stopImmediate;
	}
	if (abortInFlight) {
		if (obs_onvif::AbortHandle *a =
			    activeAbort_.load(std::memory_order_acquire))
			a->Signal();
	}
	cv_.notify_all();
}

void PtzController::WaitIdle()
{
	std::unique_lock<std::mutex> lk(mu_);
	idleCv_.wait(lk, [&] { return dispatching_ == 0 && !HasWork(); });
}

void PtzController::Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		shutdown_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

bool PtzController::HasWork() const
{
	for (const auto &kv : states_) {
		const CamState &s = kv.second;
		if (s.stopPending)
			return true;
		if (s.held && (!s.hasDispatched ||
			       !(s.heldCmd == s.lastDispatched)))
			return true;
	}
	return false;
}

void PtzController::Loop()
{
	using namespace std::chrono;
	steady_clock::time_point lastMove{};
	for (;;) {
		PtzCommand cmd;
		std::string cameraId;
		PtzSettings settings;
		bool have = false;
		{
			std::unique_lock<std::mutex> lk(mu_);
			cv_.wait(lk, [&] { return shutdown_.load() || HasWork(); });
			if (shutdown_.load() && !HasWork())
				return;
			settings = settings_;

			// Movement floor between dispatches. A queued Stop preempts
			// the floor so it lands promptly; otherwise movements are
			// spaced at least minIntervalMs apart.
			if (lastMove != steady_clock::time_point{}) {
				const auto nextSlot =
					lastMove +
					milliseconds(settings.minIntervalMs);
				while (steady_clock::now() < nextSlot) {
					bool stopPending = false;
					for (const auto &kv : states_)
						if (kv.second.stopPending) {
							stopPending = true;
							break;
						}
					if (stopPending)
						break;
					cv_.wait_for(
						lk,
						nextSlot -
							steady_clock::now());
				}
			}

			// Stop has priority over moves.
			for (auto &kv : states_) {
				if (kv.second.stopPending) {
					cameraId = kv.first;
					kv.second.stopPending = false;
					kv.second.held = false;
					kv.second.hasDispatched = false;
					cmd = PtzCommand{};
					cmd.stop = true;
					have = true;
					break;
				}
			}
			if (!have) {
				for (auto &kv : states_) {
					CamState &s = kv.second;
					if (s.held &&
					    (!s.hasDispatched ||
					     !(s.heldCmd ==
					       s.lastDispatched))) {
						cameraId = kv.first;
						cmd = s.heldCmd;
						s.lastDispatched = cmd;
						s.hasDispatched = true;
						have = true;
						break;
					}
				}
			}
			if (have && !cmd.stop)
				lastMove = steady_clock::now();
			if (have)
				++dispatching_;
		}
		if (!have)
			continue;

		Dispatch(cameraId, cmd, settings);

		bool idleNow = false;
		{
			std::lock_guard<std::mutex> lk(mu_);
			if (--dispatching_ == 0 && !HasWork())
				idleNow = true;
		}
		if (idleNow)
			idleCv_.notify_all();
	}
}

void PtzController::Dispatch(const std::string &cameraId,
			     const PtzCommand &cmd, const PtzSettings &settings)
{
	obs_onvif::AbortHandle abort;
	if (settings.stopImmediate)
		activeAbort_.store(&abort, std::memory_order_release);

	std::string err;
	if (cmd.stop) {
		exec_(cameraId, cmd, abort, err);
		if (settings.stopImmediate)
			activeAbort_.store(nullptr, std::memory_order_release);
		return;
	}

	// Bounded moves re-fire while the caller keeps this command current so
	// motion is continuous until release (the ContinuousMove Timeout bounds
	// each dispatch).
	for (;;) {
		exec_(cameraId, cmd, abort, err);
		if (settings.moveTimeoutSeconds == 0)
			break; // move until Stop: a single dispatch is enough
		bool refire = false;
		{
			std::unique_lock<std::mutex> lk(mu_);
			const bool changed = cv_.wait_for(
				lk,
				std::chrono::seconds(settings.moveTimeoutSeconds),
				[&] {
					const auto it = states_.find(cameraId);
					if (it == states_.end() ||
					    !it->second.held)
						return true; // released
					if (it->second.stopPending)
						return true; // Stop queued
					return !(it->second.heldCmd == cmd);
				});
			if (!changed) {
				const auto it = states_.find(cameraId);
				refire = it != states_.end() &&
					 it->second.held &&
					 (it->second.heldCmd == cmd) &&
					 !it->second.stopPending;
			}
		}
		if (!refire)
			break;
	}

	if (settings.stopImmediate)
		activeAbort_.store(nullptr, std::memory_order_release);
}

} // namespace obs_onvif::registry
