#include "identity.h"

#include <cstring>
#include <vector>

namespace obs_onvif {

namespace {

std::vector<std::string> SplitTokens(const std::string &s)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= s.size()) {
		size_t next = s.find_first_of(" \t\r\n", pos);
		if (next == std::string::npos)
			next = s.size();
		if (next > pos)
			out.push_back(s.substr(pos, next - pos));
		pos = next + 1;
	}
	return out;
}

} // namespace

std::string ParseScopeMac(const std::string &scopes)
{
	const char *prefix = "onvif://www.onvif.org/mac/";
	const size_t prefixLen = std::strlen(prefix);
	for (const std::string &tok : SplitTokens(scopes)) {
		if (tok.size() > prefixLen &&
		    tok.compare(0, prefixLen, prefix) == 0)
			return tok.substr(prefixLen);
	}
	return {};
}

std::string BuildFingerprint(const DeviceIdentity &id)
{
	if (!id.serialNumber.empty())
		return "sn:" + id.serialNumber;
	const std::string mac = ParseScopeMac(id.scopes);
	if (!mac.empty())
		return "mac:" + mac;
	if (!id.hardwareId.empty())
		return "hw:" + id.hardwareId;
	if (!id.uuid.empty())
		return "uuid:" + id.uuid;
	return {};
}

} // namespace obs_onvif