#include "ws_discovery.h"

#include <cstdio>
#include <cstring>

#include <winsock2.h>
#include <ws2tcpip.h>

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
	       "   <d:Types>dn:NetworkVideoTransmitter d:Device</d:Types>\n"
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

	bool found = false;
	for (const char *entry : {"ProbeMatch", "Hello", "Bye"}) {
		for (const tinyxml2::XMLElement *el : xml::Children(body, entry)) {
			DiscoveredDevice dev;
			dev.relatesTo = relatesTo;
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
		ip_mreq mreq{};
		inet_pton(AF_INET, kDiscoveryGroup, &mreq.imr_multiaddr);
		mreq.imr_interface.s_addr = htonl(INADDR_ANY);
		if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
			       (const char *)&mreq, sizeof mreq) == SOCKET_ERROR) {
			closesocket(sock);
			return -1;
		}
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