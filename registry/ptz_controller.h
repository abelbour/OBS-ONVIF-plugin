#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "soap_client.h"

namespace obs_onvif::registry {

// One movement request. The queue is depth 1 per camera, so a move arriving
// while one is pending replaces it (latest wins, intermediates discarded).
struct PtzCommand {
	double pan = 0.0;
	double tilt = 0.0;
	double zoom = 0.0;
	bool stop = false;
	unsigned timeoutSeconds = 0; // ContinuousMove Timeout (0 = until Stop)
	bool operator==(const PtzCommand &o) const
	{
		return stop == o.stop && pan == o.pan && tilt == o.tilt &&
		       zoom == o.zoom;
	}
	bool operator!=(const PtzCommand &o) const { return !(*this == o); }
};

// Settings knobs mirrored from AppConfig (M4 §6.8, Settings → PTZ tab).
struct PtzSettings {
	bool keepalive = true;          // soap_keepalive
	bool authCache = true;          // ptz_auth_cache
	unsigned moveTimeoutSeconds = 0; // ptz_move_timeout_s (0 = until Stop)
	bool stopImmediate = true;      // ptz_stop_mode == "immediate"
	unsigned minIntervalMs = 75;    // ptz_min_interval_ms
};

// Per-camera PTZ dispatch governor (OBS-free, unit-testable). A single worker
// thread owns the SOAP dispatch for every camera:
//   * queue depth 1 per camera — latest-wins coalescing;
//   * Stop purges the pending move and dispatches before any queued move;
//   * a hard minimum interval separates movement dispatches;
//   * `stopImmediate` aborts the in-flight request so Stop lands ~1 RTT after
//     release; `queued` waits for the in-flight to finish;
//   * `moveTimeoutSeconds` bounds each ContinuousMove and the controller
//     re-fires while the caller keeps the command current (button held).
class PtzController {
public:
	// Runs on the controller thread. `abort` is owned by the controller for
	// the duration of one dispatch and can be Signaled from any thread to
	// cancel the in-flight request (immediate stop).
	using Executor = std::function<bool(const std::string &cameraId,
					    const PtzCommand &cmd,
					    obs_onvif::AbortHandle &abort,
					    std::string &err)>;

	explicit PtzController(Executor exec, PtzSettings settings = PtzSettings());
	~PtzController();
	PtzController(const PtzController &) = delete;
	PtzController &operator=(const PtzController &) = delete;

	void SetSettings(const PtzSettings &s);

	// Fire-and-forget (non-blocking) submissions from any thread.
	void Move(const std::string &cameraId, double pan, double tilt,
		  double zoom);
	void Stop(const std::string &cameraId);

	// Blocks until the queue is empty and no dispatch is in flight. Used by
	// tests to make the async dispatch deterministic.
	void WaitIdle();

	// Stops accepting new work and joins the worker thread.
	void Shutdown();

private:
	void Loop();
	bool HasWork() const;
	void Dispatch(const std::string &cameraId, const PtzCommand &cmd,
		      const PtzSettings &settings);

	Executor exec_;
	PtzSettings settings_;
	std::atomic<bool> shutdown_{false};

	struct CamState {
		bool held = false;        // a move is being held (until Stop)
		PtzCommand heldCmd;       // latest held velocity
		bool stopPending = false; // Stop requested (priority over moves)
		PtzCommand lastDispatched; // dedupe: don't re-dispatch the same
		bool hasDispatched = false; // held command
	};
	std::map<std::string, CamState> states_;

	unsigned dispatching_ = 0; // dispatches in flight (WaitIdle)
	std::atomic<obs_onvif::AbortHandle *> activeAbort_{nullptr};
	std::mutex mu_;
	std::condition_variable cv_;
	std::condition_variable idleCv_;
	std::thread thread_;
};

} // namespace obs_onvif::registry
