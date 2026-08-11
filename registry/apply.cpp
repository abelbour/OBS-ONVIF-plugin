#include "apply.h"

#include <algorithm>
#include <utility>

namespace obs_onvif::registry {

static const std::string kNoCamera;

// -- URL helpers -------------------------------------------------------------

static std::string ExtractUserinfo(const std::string &url)
{
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos)
		return std::string();
	const size_t hostStart = scheme + 3;
	size_t hostEnd = url.find_first_of("/?#", hostStart);
	if (hostEnd == std::string::npos)
		hostEnd = url.size();
	const std::string authority = url.substr(hostStart, hostEnd - hostStart);
	const size_t at = authority.rfind('@');
	if (at == std::string::npos)
		return std::string();
	return authority.substr(0, at);
}

static std::string InjectUserinfo(const std::string &url,
				  const std::string &userinfo)
{
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos)
		return url;
	const size_t hostStart = scheme + 3;
	std::string tail = url.substr(hostStart);
	std::string authority, rest;
	const size_t hostEnd = tail.find_first_of("/?#");
	if (hostEnd == std::string::npos) {
		authority = tail;
		rest.clear();
	} else {
		authority = tail.substr(0, hostEnd);
		rest = tail.substr(hostEnd);
	}
	const size_t at = authority.rfind('@');
	if (at != std::string::npos)
		authority = authority.substr(at + 1);
	return url.substr(0, hostStart) + userinfo + "@" + authority + rest;
}

std::string RewriteSourceUrl(const std::string &old_url,
			     const std::string &new_url,
			     const std::string &credentials)
{
	if (new_url.empty())
		return new_url;
	if (!ExtractUserinfo(new_url).empty())
		return new_url;
	if (!credentials.empty())
		return InjectUserinfo(new_url, credentials);
	if (!old_url.empty()) {
		const std::string oldUi = ExtractUserinfo(old_url);
		if (!oldUi.empty())
			return InjectUserinfo(new_url, oldUi);
	}
	return new_url;
}

std::string UrlEncodeUserinfo(const std::string &s)
{
	const char *hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(s.size());
	for (const unsigned char c : s) {
		const bool unreserved =
			(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '.' ||
			c == '_' || c == '~';
		// userinfo additionally allows ':' and the sub-delims.
		const bool allowed =
			unreserved || c == ':' || c == '!' || c == '$' ||
			c == '&' || c == '\'' || c == '(' || c == ')' ||
			c == '*' || c == '+' || c == ',' || c == ';' || c == '=';
		if (allowed) {
			out.push_back((char)c);
		} else {
			out.push_back('%');
			out.push_back(hex[(c >> 4) & 0xF]);
			out.push_back(hex[c & 0xF]);
		}
	}
	return out;
}

// -- Policy state machine ----------------------------------------------------

ApplyPolicyChoice ApplyPolicy::DefaultPolicy() const
{
	return default_policy_;
}

void ApplyPolicy::SetDefaultPolicy(ApplyPolicyChoice p)
{
	default_policy_ = p;
}

ApplyPolicyChoice ApplyPolicy::PolicyFor(const std::string &camera_id) const
{
	const auto it = camera_policy_.find(camera_id);
	return it != camera_policy_.end() ? it->second : default_policy_;
}

void ApplyPolicy::SetCameraPolicy(const std::string &camera_id,
				  ApplyPolicyChoice p, bool remember)
{
	if (remember)
		camera_policy_[camera_id] = p;
	else
		camera_policy_.erase(camera_id);
}

void ApplyPolicy::TrackSourceUrl(const std::string &source_name,
				 const std::string &current_url)
{
	source_urls_[source_name] = current_url;
}

void ApplyPolicy::TrackMappings(const std::vector<SourceMapping> &mappings)
{
	mappings_ = mappings;
}

void ApplyPolicy::ForgetSource(const std::string &source_name)
{
	source_urls_.erase(source_name);
	mappings_.erase(
		std::remove_if(mappings_.begin(), mappings_.end(),
			       [&](const SourceMapping &m) {
				       return m.source_name == source_name;
			       }),
		mappings_.end());
}

size_t ApplyPolicy::BuildRewrites(const Incident &inc,
				  std::vector<SourceRewrite> &rewrites) const
{
	rewrites.clear();
	size_t n = 0;
	for (const auto &m : mappings_) {
		if (m.camera_id != inc.camera_id || !m.auto_apply)
			continue;
		SourceRewrite rw;
		rw.mapping = m;
		rw.camera_id = m.camera_id;
		const auto it = source_urls_.find(m.source_name);
		rw.old_url = it != source_urls_.end() ? it->second
						      : std::string();
		rw.new_url = RewriteSourceUrl(rw.old_url, inc.new_stream_uri,
					      inc.credentials);
		rewrites.push_back(std::move(rw));
		++n;
	}
	return n;
}

ApplyDecision ApplyPolicy::OnIpChange(const std::string &camera_id,
				      const std::string &new_stream_uri,
				      bool output_active,
				      const std::string &credentials,
				      std::vector<SourceRewrite> &rewrites)
{
	rewrites.clear();
	if (PolicyFor(camera_id) == ApplyPolicyChoice::Ignore)
		return ApplyDecision::Ignored;

	const Incident inc{camera_id, new_stream_uri, credentials};

	if (output_active && PolicyFor(camera_id) == ApplyPolicyChoice::Ask) {
		std::vector<SourceRewrite> probe;
		if (BuildRewrites(inc, probe) == 0)
			return ApplyDecision::Ignored; // nothing to rewrite
		pending_ = inc;
		return ApplyDecision::Prompted;
	}

	return BuildRewrites(inc, rewrites) ? ApplyDecision::AppliedNow
					    : ApplyDecision::Ignored;
}

ApplyDecision ApplyPolicy::ApplyPending(std::vector<SourceRewrite> &rewrites)
{
	if (!pending_)
		return ApplyDecision::Ignored;
	const size_t n = BuildRewrites(*pending_, rewrites);
	pending_.reset();
	return n ? ApplyDecision::AppliedNow : ApplyDecision::Ignored;
}

ApplyDecision ApplyPolicy::IgnorePending(bool remember)
{
	if (pending_ && remember)
		SetCameraPolicy(pending_->camera_id, ApplyPolicyChoice::Ignore,
				true);
	pending_.reset();
	return ApplyDecision::Ignored;
}

ApplyDecision ApplyPolicy::DeferPending()
{
	if (pending_) {
		deferred_ = std::move(pending_);
		pending_.reset();
	}
	return ApplyDecision::Deferred;
}

ApplyDecision ApplyPolicy::OnPromptTimeout()
{
	return DeferPending();
}

bool ApplyPolicy::OnOutputsIdle(std::vector<SourceRewrite> &rewrites)
{
	if (!deferred_)
		return false;
	std::vector<SourceRewrite> tmp;
	const bool applied = BuildRewrites(*deferred_, tmp) != 0;
	deferred_.reset();
	if (applied)
		rewrites.swap(tmp);
	return applied;
}

bool ApplyPolicy::HasPending() const
{
	return pending_.has_value();
}

bool ApplyPolicy::HasDeferred() const
{
	return deferred_.has_value();
}

const std::string &ApplyPolicy::PendingCamera() const
{
	return pending_ ? pending_->camera_id : kNoCamera;
}

const std::string &ApplyPolicy::DeferredCamera() const
{
	return deferred_ ? deferred_->camera_id : kNoCamera;
}

} // namespace obs_onvif::registry