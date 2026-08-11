#include <string>
#include <vector>

#include "apply.h"
#include "check.h"

using namespace obs_onvif::registry;

static void TestRewriteInjectsCredentials()
{
	CHECK_EQ(RewriteSourceUrl("", "rtsp://10.0.0.2:554/Streaming/Channels/101",
				  "admin:pass"),
		 std::string(
			 "rtsp://admin:pass@10.0.0.2:554/Streaming/Channels/101"));
}

static void TestRewritePreservesOldCredentials()
{
	// No explicit creds: fall back to the old URL's embedded userinfo.
	CHECK_EQ(RewriteSourceUrl("rtsp://cam:oldpw@10.0.0.1:554/Streaming/Channels/101",
				  "rtsp://10.0.0.2:554/Streaming/Channels/101", ""),
		 std::string(
			 "rtsp://cam:oldpw@10.0.0.2:554/Streaming/Channels/101"));
}

static void TestRewriteKeepsNewUrlUserinfo()
{
	// The stream URI already carries its own userinfo -> never replaced.
	CHECK_EQ(RewriteSourceUrl("rtsp://old:old@10.0.0.1/x",
				  "rtsp://who:xyz@10.0.0.2/y", "admin:pass"),
		 std::string("rtsp://who:xyz@10.0.0.2/y"));
}

static void TestRewritePassThrough()
{
	CHECK_EQ(RewriteSourceUrl("", "rtsp://10.0.0.2:554/Streaming/Channels/101",
				  ""),
		 std::string("rtsp://10.0.0.2:554/Streaming/Channels/101"));
	CHECK_EQ(RewriteSourceUrl("rtsp://host/x", "", "admin:pass"),
		 std::string());
}

static void TestUrlEncodeUserinfo()
{
	CHECK_EQ(UrlEncodeUserinfo("admin:p@ss w/&"),
		 std::string("admin:p%40ss%20w%2F&"));
	CHECK_EQ(UrlEncodeUserinfo("plain-user"),
		 std::string("plain-user"));
}

static SourceMapping Mapping(const std::string &source,
			     const std::string &camera,
			     bool auto_apply)
{
	SourceMapping m;
	m.collection_uuid = "col-1";
	m.source_name = source;
	m.camera_id = camera;
	m.profileToken = "profile1";
	m.auto_apply = auto_apply;
	return m;
}

static void TestAskNoOutputAutoApplies()
{
	ApplyPolicy p;
	p.TrackMappings({Mapping("CAM-101", "sn:1", true)});
	p.TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/false, "admin:pass", rw) ==
	      ApplyDecision::AppliedNow);
	CHECK_EQ(rw.size(), size_t(1));
	CHECK_EQ(rw[0].new_url,
		 std::string(
			 "rtsp://admin:pass@10.0.0.2:554/Streaming/Channels/101"));
	CHECK(!p.HasPending());
	CHECK(!p.HasDeferred());
}

static void TestAskWhileLivePromptsThenApplies()
{
	ApplyPolicy p;
	p.TrackMappings({Mapping("CAM-101", "sn:1", true)});
	p.TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::Prompted);
	CHECK_EQ(rw.size(), size_t(0));
	CHECK(p.HasPending());
	CHECK_EQ(p.PendingCamera(), std::string("sn:1"));

	CHECK(p.ApplyPending(rw) == ApplyDecision::AppliedNow);
	CHECK_EQ(rw.size(), size_t(1));
	CHECK(!p.HasPending());
}

static void TestAlwaysAutoAppliesWhileLive()
{
	ApplyPolicy p;
	p.SetDefaultPolicy(ApplyPolicyChoice::Always);
	p.TrackMappings({Mapping("CAM-101", "sn:1", true)});
	p.TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::AppliedNow);
	CHECK_EQ(rw.size(), size_t(1));
	CHECK(!p.HasPending());
}

static void TestIgnoreDrops()
{
	ApplyPolicy p;
	p.SetDefaultPolicy(ApplyPolicyChoice::Ignore);
	p.TrackMappings({Mapping("CAM-101", "sn:1", true)});
	p.TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::Ignored);
	CHECK_EQ(rw.size(), size_t(0));
}

static void TestPromptIgnoreRemembered()
{
	ApplyPolicy p;
	p.TrackMappings({Mapping("CAM-101", "sn:1", true)});
	p.TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::Prompted);
	CHECK(p.IgnorePending(/*remember=*/true) == ApplyDecision::Ignored);
	CHECK(!p.HasPending());
	// The remembered choice governs future incidents.
	CHECK(p.PolicyFor("sn:1") == ApplyPolicyChoice::Ignore);
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::Ignored);
}

static void TestPromptTimeoutDefersThenAppliesOnIdle()
{
	ApplyPolicy p;
	p.TrackMappings({Mapping("CAM-101", "sn:1", true)});
	p.TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::Prompted);
	CHECK(p.OnPromptTimeout() == ApplyDecision::Deferred);
	CHECK(!p.HasPending());
	CHECK(p.HasDeferred());
	CHECK_EQ(p.DeferredCamera(), std::string("sn:1"));

	// Outputs go inactive -> the deferred incident is auto-applied.
	CHECK(p.OnOutputsIdle(rw));
	CHECK_EQ(rw.size(), size_t(1));
	CHECK_EQ(rw[0].new_url,
		 std::string(
			 "rtsp://admin:pass@10.0.0.2:554/Streaming/Channels/101"));
	CHECK(!p.HasDeferred());
	// Nothing deferred -> idle does nothing.
	CHECK(!p.OnOutputsIdle(rw));
}

static void TestNonAutoApplyMappingSkipped()
{
	ApplyPolicy p;
	p.TrackMappings({Mapping("CAM-101", "sn:1", false)});
	p.TrackSourceUrl("CAM-101", "rtsp://10.0.0.1:554/Streaming/Channels/101");

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2:554/Streaming/Channels/101",
			   /*output_active=*/false, "admin:pass", rw) ==
	      ApplyDecision::Ignored);
	CHECK_EQ(rw.size(), size_t(0));
}

static void TestNoMappingsNothingToDo()
{
	ApplyPolicy p;
	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::Ignored);
	CHECK_EQ(rw.size(), size_t(0));
}

static void TestForgetSource()
{
	ApplyPolicy p;
	p.TrackMappings({Mapping("CAM-101", "sn:1", true)});
	p.ForgetSource("CAM-101");
	CHECK(p.PolicyFor("sn:1") == ApplyPolicyChoice::Ask);

	std::vector<SourceRewrite> rw;
	CHECK(p.OnIpChange("sn:1", "rtsp://10.0.0.2/Streaming/Channels/101",
			   /*output_active=*/true, "admin:pass", rw) ==
	      ApplyDecision::Ignored);
	CHECK_EQ(rw.size(), size_t(0));
}

int main()
{
	TestRewriteInjectsCredentials();
	TestRewritePreservesOldCredentials();
	TestRewriteKeepsNewUrlUserinfo();
	TestRewritePassThrough();
	TestUrlEncodeUserinfo();
	TestAskNoOutputAutoApplies();
	TestAskWhileLivePromptsThenApplies();
	TestAlwaysAutoAppliesWhileLive();
	TestIgnoreDrops();
	TestPromptIgnoreRemembered();
	TestPromptTimeoutDefersThenAppliesOnIdle();
	TestNonAutoApplyMappingSkipped();
	TestNoMappingsNothingToDo();
	TestForgetSource();
	RUN_TESTS("apply");
}