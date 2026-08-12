// PtzController unit tests (tests/unit/ptz_controller_test.cpp). OBS-free:
// a fake executor records the dispatched command stream so the governor's
// invariants are checked deterministically:
//   * queue depth 1 per camera — latest-wins coalescing, intermediates dropped;
//   * Stop purges the pending move and dispatches before any queued move;
//   * a hard minimum interval separates movement dispatches;
//   * dispatches never overlap (single worker thread);
//   * a bounded move timeout re-fires while the command stays current.
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "ptz_controller.h"

using namespace obs_onvif::registry;

namespace {

struct Dispatch {
	std::string camera;
	PtzCommand cmd;
	std::chrono::steady_clock::time_point at;
};

struct Recorder {
	mutable std::mutex mu;
	std::vector<Dispatch> dispatches;
	unsigned inFlight = 0;
	unsigned maxInFlight = 0;
	std::chrono::milliseconds sleepPer{0};

	void MarkEnter()
	{
		++inFlight;
		if (inFlight > maxInFlight)
			maxInFlight = inFlight;
	}
	void MarkExit()
	{
		--inFlight;
	}
};

bool FakeExec(Recorder &rec, const std::string &cameraId,
	      const PtzCommand &cmd, obs_onvif::AbortHandle &,
	      std::string &)
{
	rec.MarkEnter();
	{
		std::lock_guard<std::mutex> lock(rec.mu);
		rec.dispatches.push_back({cameraId, cmd,
					  std::chrono::steady_clock::now()});
	}
	if (rec.sleepPer.count() > 0)
		std::this_thread::sleep_for(rec.sleepPer);
	rec.MarkExit();
	return true;
}

bool WaitFor(const Recorder &rec, size_t n, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(rec.mu);
			if (rec.dispatches.size() >= n)
				return true;
		}
		if (std::chrono::steady_clock::now() > deadline)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

size_t CountOf(const Recorder &rec, bool stop)
{
	std::lock_guard<std::mutex> lock(rec.mu);
	size_t n = 0;
	for (const auto &d : rec.dispatches)
		if (d.cmd.stop == stop)
			++n;
	return n;
}

bool AnyCmd(const Recorder &rec, const PtzCommand &want)
{
	std::lock_guard<std::mutex> lock(rec.mu);
	for (const auto &d : rec.dispatches)
		if (d.cmd == want)
			return true;
	return false;
}

// ---------------------------------------------------------------------------

void TestCoalescing()
{
	Recorder rec;
	rec.sleepPer = std::chrono::milliseconds(20);
	PtzController ctl([&rec](const std::string &cam, const PtzCommand &cmd,
				 obs_onvif::AbortHandle &a, std::string &e) {
		return FakeExec(rec, cam, cmd, a, e);
	}, PtzSettings{});

	ctl.Move("camA", 1.0, 0.0, 0.0);
	CHECK(WaitFor(rec, 1, std::chrono::seconds(2))); // first dispatch in flight
	ctl.Move("camA", 2.0, 0.0, 0.0); // superseded by:
	ctl.Move("camA", 3.0, 0.0, 0.0); // latest wins
	CHECK(WaitFor(rec, 2, std::chrono::seconds(2)));

	PtzCommand v1{1.0, 0.0, 0.0, false, 0};
	PtzCommand v3{3.0, 0.0, 0.0, false, 0};
	CHECK(AnyCmd(rec, v1));
	CHECK(AnyCmd(rec, v3));
	CHECK(!AnyCmd(rec, PtzCommand{2.0, 0.0, 0.0, false, 0})); // discarded
	ctl.Shutdown();
}

void TestStopPurgesAndPrioritizes()
{
	Recorder rec;
	rec.sleepPer = std::chrono::milliseconds(20);
	PtzController ctl([&rec](const std::string &cam, const PtzCommand &cmd,
				 obs_onvif::AbortHandle &a, std::string &e) {
		return FakeExec(rec, cam, cmd, a, e);
	}, PtzSettings{});

	ctl.Move("camA", 1.0, 0.0, 0.0);
	CHECK(WaitFor(rec, 1, std::chrono::seconds(2))); // move in flight
	ctl.Move("camA", 9.0, 0.0, 0.0); // queued behind the in-flight move
	ctl.Stop("camA");                // must purge that queued move
	CHECK(WaitFor(rec, 2, std::chrono::seconds(2)));

	CHECK_EQ(CountOf(rec, true), size_t(1));          // stop dispatched
	CHECK(!AnyCmd(rec, PtzCommand{9.0, 0.0, 0.0, false, 0})); // purged
	CHECK(WaitFor(rec, 3, std::chrono::seconds(1)) == false);
	ctl.Shutdown();
}

void TestMinInterval()
{
	Recorder rec;
	PtzSettings settings;
	settings.minIntervalMs = 120;
	PtzController ctl([&rec](const std::string &cam, const PtzCommand &cmd,
				 obs_onvif::AbortHandle &a, std::string &e) {
		return FakeExec(rec, cam, cmd, a, e);
	}, settings);

	ctl.Move("camA", 1.0, 0.0, 0.0);
	CHECK(WaitFor(rec, 1, std::chrono::seconds(2)));
	ctl.Move("camB", 2.0, 0.0, 0.0); // second dispatch honors the floor
	CHECK(WaitFor(rec, 2, std::chrono::seconds(2)));

	std::chrono::steady_clock::time_point a, b;
	{
		std::lock_guard<std::mutex> lock(rec.mu);
		a = rec.dispatches[0].at;
		b = rec.dispatches[1].at;
	}
	const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
	CHECK(gap >= 110); // >= minIntervalMs (120) with margin
	ctl.Shutdown();
}

void TestNoOverlap()
{
	Recorder rec;
	rec.sleepPer = std::chrono::milliseconds(30);
	PtzController ctl([&rec](const std::string &cam, const PtzCommand &cmd,
				 obs_onvif::AbortHandle &a, std::string &e) {
		return FakeExec(rec, cam, cmd, a, e);
	}, PtzSettings{});

	ctl.Move("camA", 1.0, 0.0, 0.0);
	CHECK(WaitFor(rec, 1, std::chrono::seconds(2)));
	ctl.Move("camA", 2.0, 0.0, 0.0);
	ctl.Move("camB", 3.0, 0.0, 0.0);
	ctl.Move("camA", 4.0, 0.0, 0.0);
	CHECK(WaitFor(rec, 3, std::chrono::seconds(2))); // camA(2) coalesced away

	CHECK(rec.maxInFlight <= 1); // the mock-equivalent "no overlaps" guard
	ctl.Shutdown();
}

void TestBoundedRefire()
{
	Recorder rec;
	PtzSettings settings;
	settings.moveTimeoutSeconds = 1; // re-fire every second while held
	settings.minIntervalMs = 0;
	PtzController ctl([&rec](const std::string &cam, const PtzCommand &cmd,
				 obs_onvif::AbortHandle &a, std::string &e) {
		return FakeExec(rec, cam, cmd, a, e);
	}, settings);

	ctl.Move("camA", 1.0, 0.0, 0.0);
	CHECK(WaitFor(rec, 2, std::chrono::seconds(4))); // initial + one re-fire
	ctl.Stop("camA");
	CHECK(WaitFor(rec, 3, std::chrono::seconds(2))); // stop lands
	CHECK_EQ(CountOf(rec, true), size_t(1));
	ctl.Shutdown();
}

} // namespace

int main()
{
	TestCoalescing();
	TestStopPurgesAndPrioritizes();
	TestMinInterval();
	TestNoOverlap();
	TestBoundedRefire();
	RUN_TESTS("ptz_controller");
}
