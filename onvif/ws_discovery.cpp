#include "ws_discovery.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include "xml.h"

namespace obs_onvif {

namespace {

struct WinsockInitializer {
	WinsockInitializer()
	{
		WSADATA data{};
		WSAStartup(MAKEWORD(2, 2), &data);
	}
	~WinsockInitializer() { WSACleanup(); }
};

WinsockInitializer &EnsureWinsock()
{
	static WinsockInitializer init;
	return init;
}

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

// "dn:NetworkVideoTransmitter" -> "NetworkVideoTransmitter".
std::string LocalTypeName(const std::string &token)
{
	const size_t colon = token.find(':');
	return colon == std::string::npos ? token : token.substr(colon + 1);
}

// Address family for a textual host (IPv4 literal, IPv6 literal, hostname).
int FamilyForHost(const std::string &host)
{
	if (host.find(':') != std::string::npos)
		return AF_INET6;
	return AF_INET;
}

// IPv4 addresses of up, multicast-capable, non-loopback interfaces. The probe
// is sent on each so cameras are reached regardless of which adapter is the
// OS default route (VPNs / virtual adapters frequently hijack it).
std::vector<uint32_t> MulticastInterfaces()
{
	EnsureWinsock();
	std::vector<uint32_t> out;
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
		return out;
	DWORD bytes = 0;
	INTERFACE_INFO infos[32];
	if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, nullptr, 0, infos,
		     sizeof infos, &bytes, nullptr, nullptr) == 0) {
		const size_t n = bytes / sizeof(INTERFACE_INFO);
		for (size_t i = 0; i < n; ++i) {
			const sockaddr_in *addr = reinterpret_cast<const sockaddr_in *>(
				&infos[i].iiAddress);
			const ULONG flags = infos[i].iiFlags;
			if ((flags & IFF_UP) && (flags & IFF_MULTICAST) &&
			    !(flags & IFF_LOOPBACK))
				out.push_back(addr->sin_addr.s_addr);
		}
	}
	closesocket(s);
	return out;
}

// Multicast group address as an in_addr.
in_addr DiscoveryGroupAddr()
{
	in_addr a{};
	inet_pton(AF_INET, kDiscoveryGroup, &a);
	return a;
}

// Device entries nested in the SOAP Body: ProbeMatch lives under a
// ProbeMatches wrapper; Hello/Bye sit directly under Body.
std::vector<const tinyxml2::XMLElement *>
EntryElements(const tinyxml2::XMLElement *body)
{
	std::vector<const tinyxml2::XMLElement *> entries;
	const tinyxml2::XMLElement *matches = xml::Child(body, "ProbeMatches");
	if (matches) {
		for (const tinyxml2::XMLElement *el :
		     xml::Children(matches, "ProbeMatch"))
			entries.push_back(el);
		return entries;
	}
	for (const char *name : {"Hello", "Bye"}) {
		if (const tinyxml2::XMLElement *el = xml::Child(body, name)) {
			entries.push_back(el);
			return entries;
		}
	}
	return entries;
}

} // namespace

std::string BuildProbe(const std::string &messageId)
{
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	       "<soap:Envelope "
	       "xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\"\n"
	       " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"\n"
	       " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"\n"
	       " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\n"
	       " <soap:Header>\n"
	       "  <wsa:MessageID>" +
	       messageId +
	       "</wsa:MessageID>\n"
	       "  <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>\n"
	       "  <wsa:Action>"
	       "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>\n"
	       " </soap:Header>\n"
	       " <soap:Body>\n"
	       "  <d:Probe>\n"
	       "   <d:Types>dn:NetworkVideoTransmitter</d:Types>\n"
	       "  </d:Probe>\n"
	       " </soap:Body>\n"
	       "</soap:Envelope>";
}

bool ParseDiscoveryResponse(const std::string &xml,
			    std::vector<DiscoveredDevice> &out)
{
	tinyxml2::XMLDocument doc;
	if (!xml::Parse(xml, doc))
		return false;
	const tinyxml2::XMLElement *env = doc.RootElement();
	if (!env)
		return false;

	std::string relatesTo;
	const tinyxml2::XMLElement *header = xml::Child(env, "Header");
	if (header)
		relatesTo = xml::ChildText(header, "RelatesTo");

	const tinyxml2::XMLElement *body = xml::Child(env, "Body");
	if (!body)
		return false;

	// Classify the message: ProbeMatches wraps ProbeMatch entries; Hello
	// and Bye sit directly under Body (and may appear in a ProbeMatch-style
	// envelope from loose implementations, so Body-level wins).
	DiscoveryMsgType type = DiscoveryMsgType::Unknown;
	if (xml::Child(body, "ProbeMatches"))
		type = DiscoveryMsgType::ProbeMatches;
	else if (xml::Child(body, "Bye"))
		type = DiscoveryMsgType::Bye;
	else if (xml::Child(body, "Hello"))
		type = DiscoveryMsgType::Hello;

	bool found = false;
	for (const tinyxml2::XMLElement *el : EntryElements(body)) {
		DiscoveredDevice dev;
		dev.relatesTo = relatesTo;
		dev.type = type;
		dev.xaddrs = SplitTokens(xml::ChildText(el, "XAddrs"));
		for (const std::string &t :
		     SplitTokens(xml::ChildText(el, "Types"))) {
			dev.types.push_back(LocalTypeName(t));
		}
		dev.scopes = xml::ChildText(el, "Scopes");
		dev.uuid = xml::ChildText(el, "Address");
		out.push_back(std::move(dev));
		found = true;
	}
	return found;
}

intptr_t OpenUdpSocket(uint16_t bindPort, bool joinMulticast, bool reuseAddr)
{
	EnsureWinsock();

	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return -1;

	if (joinMulticast || reuseAddr) {
		BOOL optVal = TRUE;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&optVal,
			   sizeof optVal);
	}

	if (joinMulticast) {
		const in_addr group = DiscoveryGroupAddr();
		const std::vector<uint32_t> ifaces = MulticastInterfaces();
		if (ifaces.empty()) {
			ip_mreq mreq{};
			mreq.imr_multiaddr = group;
			mreq.imr_interface.s_addr = htonl(INADDR_ANY);
			setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
				   (const char *)&mreq, sizeof mreq);
		} else {
			for (uint32_t a : ifaces) {
				ip_mreq mreq{};
				mreq.imr_multiaddr = group;
				mreq.imr_interface.s_addr = a;
				setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
					   (const char *)&mreq, sizeof mreq);
			}
		}
		// Do not receive our own probes back — cuts the echo noise the
		// discovery log was filling with "parsed 0 devices".
		BOOL loop = FALSE;
		setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP,
			   (const char *)&loop, sizeof loop);
		// DHCP-sack at scale: during a network event a burst of Hello/
		// Bye announcements can arrive faster than the (SOAP-resolving)
		// loop drains them. A larger receive buffer absorbs the burst so
		// presence/TTL updates are not dropped.
		int rcvbuf = 256 * 1024;
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf,
			   sizeof rcvbuf);
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(bindPort);
	if (bind(sock, (sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
		closesocket(sock);
		return -1;
	}
	return (intptr_t)sock;
}

long SendUdp(intptr_t sock, const std::string &payload, const std::string &host,
	     uint16_t port)
{
	EnsureWinsock();

	const int family = FamilyForHost(host);
	char service[8];
	std::snprintf(service, sizeof service, "%u", (unsigned)port);

	// Resolve the peer address.
	addrinfo hints{};
	hints.ai_family = family;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_protocol = IPPROTO_UDP;
	addrinfo *resolved = nullptr;
	if (getaddrinfo(host.c_str(), service, &hints, &resolved) != 0 || !resolved)
		return -1;

	const int sent = sendto(sock, payload.data(), (int)payload.size(), 0,
				resolved->ai_addr, (int)resolved->ai_addrlen);
	freeaddrinfo(resolved);
	return sent == SOCKET_ERROR ? -1 : (long)sent;
}

long SendProbeAll(intptr_t sock, const std::string &messageId)
{
	const std::string payload = BuildProbe(messageId);
	const std::vector<uint32_t> ifaces = MulticastInterfaces();
	if (ifaces.empty())
		return SendUdp(sock, payload, kDiscoveryGroup, kDiscoveryPort);
	long total = 0;
	for (uint32_t a : ifaces) {
		const DWORD addr = a;
		setsockopt((SOCKET)sock, IPPROTO_IP, IP_MULTICAST_IF,
			   (const char *)&addr, sizeof addr);
		const long s = SendUdp(sock, payload, kDiscoveryGroup,
				      kDiscoveryPort);
		if (s > 0)
			total += s;
	}
	return total;
}

long RecvUdp(intptr_t sock, std::string &out, unsigned timeoutMs)
{
	EnsureWinsock();

	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET((SOCKET)sock, &readSet);
	timeval tv{};
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (long)((timeoutMs % 1000) * 1000);

	const int ready = select(0, &readSet, nullptr, nullptr, &tv);
	if (ready <= 0)
		return -1;

	char buf[8192];
	const int n =
		recv((SOCKET)sock, buf, (int)sizeof buf - 1, 0);
	if (n <= 0)
		return -1;
	out.assign(buf, (size_t)n);
	return (long)n;
}

int CloseUdpSocket(intptr_t sock)
{
	return closesocket((SOCKET)sock);
}

} // namespace obs_onvif