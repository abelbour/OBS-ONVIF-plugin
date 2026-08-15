#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace obs_onvif {

constexpr uint16_t kDiscoveryPort = 3702;
constexpr const char *kDiscoveryGroup = "239.255.255.250";

// One enumerable multicast-capable IPv4 interface. `addr` is in network byte
// order; `ifindex` is 0 when only the legacy fallback supplied it.
struct UdpIface {
	uint32_t addr = 0;
	unsigned long ifindex = 0;
	std::string name;
};

// The kind of WS-Discovery message a parsed device entry came from. The
// discovery loop uses this to apply each entry cheaply: a Hello only refreshes
// presence for a known camera (no SOAP), a Bye marks it offline immediately,
// and only ProbeMatches (or an unknown/new-address Hello) needs full resolution.
enum class DiscoveryMsgType { Unknown, ProbeMatches, Hello, Bye };

struct DiscoveredDevice {
	std::vector<std::string> xaddrs; // whitespace-split XAddrs
	std::vector<std::string> types;  // contract type local names
					 // (e.g. "NetworkVideoTransmitter")
	std::string scopes;              // raw scopes string
	std::string uuid;                // wsa:Address endpoint uuid
	std::string relatesTo;           // probe message id (header wsa:RelatesTo)
	DiscoveryMsgType type = DiscoveryMsgType::Unknown;
	uint64_t lastSeen = 0;           // monotonic ms, managed by the caller
};

// WS-Discovery v1 (April-2005) Probe request; probes the NVT contract type for
// broad compatibility.
std::string BuildProbe(const std::string &messageId);

// WS-Discovery v1.1 (OASIS, 2009-01) Probe request. Some cameras/firmware only
// answer v1.1 probes (WCF-based tools like ODM send v1.1), so the loop sends
// both versions on every interface.
std::string BuildProbeV11(const std::string &messageId);

// WS-Discovery v1.1 Probe matching all service types (empty Types filter),
// for firmware that ignores a typed NetworkVideoTransmitter probe.
std::string BuildProbeAny(const std::string &messageId);

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

// Sends the WS-Discovery Probe on every up, multicast-capable, non-loopback
// interface. The OS default route may send multicast out the wrong adapter on
// multi-interface machines (VPN/virtual adapters), leaving cameras unreached
// even though the packet is echoed back locally. Falls back to the default
// interface when none are enumerated. Returns total bytes sent.
long SendProbeAll(intptr_t sock, const std::string &messageId);

// Human-readable list of the multicast interfaces the socket code joins and
// sends probes on (for the Diagnostics tab). Example:
// "2 multicast interface(s)\n  192.168.1.10 (idx 12) Wi-Fi".
std::string MulticastInterfaceSummary();

} // namespace obs_onvif