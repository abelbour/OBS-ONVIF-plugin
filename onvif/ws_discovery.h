#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace obs_onvif {

constexpr uint16_t kDiscoveryPort = 3702;
constexpr const char *kDiscoveryGroup = "239.255.255.250";

struct DiscoveredDevice {
	std::vector<std::string> xaddrs; // whitespace-split XAddrs
	std::vector<std::string> types;  // contract type local names
					 // (e.g. "NetworkVideoTransmitter")
	std::string scopes;              // raw scopes string
	std::string uuid;                // wsa:Address endpoint uuid
	std::string relatesTo;           // probe message id (header wsa:RelatesTo)
	uint64_t lastSeen = 0;           // monotonic ms, managed by the caller
};

// WS-Discovery v1 (April-2005) Probe request; probes BOTH the NVT and the
// generic Device contract type for broad compatibility.
std::string BuildProbe(const std::string &messageId);

// Parses a ProbeMatches/Hello/Bye response envelope into one DiscoveredDevice
// per discovered element. Version-tolerant: namespace prefixes are ignored.
// Returns true when at least one entry was produced.
bool ParseDiscoveryResponse(const std::string &xml,
			    std::vector<DiscoveredDevice> &out);

// Datagram socket surface used by the registry loop (M2) and by the loopback
// mock tests in CI. Returns the raw SOCKET as intptr_t (INVALID_SOCKET is -1).
intptr_t OpenUdpSocket(uint16_t bindPort, bool joinMulticast, bool reuseAddr);
long SendUdp(intptr_t sock, const std::string &payload, const std::string &host,
	     uint16_t port);
long RecvUdp(intptr_t sock, std::string &out, unsigned timeoutMs);
int CloseUdpSocket(intptr_t sock);

} // namespace obs_onvif