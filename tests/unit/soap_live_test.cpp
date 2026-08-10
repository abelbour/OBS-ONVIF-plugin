// Live SOAP round-trip test against tests/mock_onvif_server.py.
//
// usage: soap_live_test <url> <mode> <username> <password>
// modes:
//   digest         wsse digest accepted by a digest-required mock
//   digest_invalid wrong password is rejected (401 fault)
//   basic          Basic auth accepted by a basic-required mock
//   fallback       digest attempt rejected, Basic retry succeeds
//   open_fault     unknown op yields a parsed SOAP fault
//   https_open     https, self-signed cert accepted (validation off)
//   https_strict   https, validation on -> transport failure
#include <cstdlib>
#include <iostream>
#include <string>

#include "base64.h"
#include "check.h"
#include "soap_client.h"
#include "ws_security.h"
#include "xml.h"

using obs_onvif::SoapClient;
using obs_onvif::SoapRequest;
using obs_onvif::SoapResult;

namespace {

const char *kTdsNs = "http://www.onvif.org/ver10/device/wsdl";
const char *kSoapAction =
	"http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation";

std::string DigestEnvelope(const std::string &user, const std::string &pass)
{
	const auto token = obs_onvif::BuildUsernameToken(user, pass);
	return obs_onvif::xml::Envelope(obs_onvif::SecurityHeader(token),
					"<tds:GetDeviceInformation/>",
					{{"tds", kTdsNs}});
}

std::string PlainEnvelope()
{
	return obs_onvif::xml::Envelope("", "<tds:GetDeviceInformation/>",
					{{"tds", kTdsNs}});
}

std::string UnsupportedEnvelope()
{
	return obs_onvif::xml::Envelope("", "<tds:UnsupportedThing/>",
					{{"tds", kTdsNs}});
}

struct RoundTrip {
	SoapClient client;
	bool ok = false;
	SoapResult result;
};

RoundTrip Run(const SoapRequest &req)
{
	RoundTrip rt;
	rt.ok = rt.client.Send(req, rt.result);
	return rt;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 5) {
		std::cerr
			<< "usage: soap_live_test <url> <mode> <username> <password>"
			<< std::endl;
		return 2;
	}
	const std::string url = argv[1];
	const std::string mode = argv[2];
	const std::string user = argv[3];
	const std::string pass = argv[4];

	const std::string basicCred =
		obs_onvif::base64_encode(user + ":" + pass);

	if (mode == "digest" || mode == "digest_invalid") {
		SoapRequest a;
		a.url = url;
		a.body = DigestEnvelope(user, pass);
		a.soapAction = kSoapAction;
		SoapRequest b = a;
		b.body = PlainEnvelope();
		b.basicCred = basicCred;

		SoapResult r;
		CHECK(obs_onvif::SoapClient().SendWithAuthFallback(a, b, r));
		CHECK(r.transportOk);

		if (mode == "digest") {
			CHECK_EQ(r.httpStatus, 200u);
			CHECK(r.body.find("GetDeviceInformationResponse") !=
			      std::string::npos);
		} else {
			CHECK_EQ(r.httpStatus, 401u);
			CHECK(r.fault.present);
		}
	} else if (mode == "basic") {
		SoapRequest req;
		req.url = url;
		req.body = PlainEnvelope();
		req.soapAction = kSoapAction;
		req.basicCred = basicCred;

		const RoundTrip rt = Run(req);
		CHECK(rt.ok);
		CHECK(rt.result.transportOk);
		CHECK_EQ(rt.result.httpStatus, 200u);
	} else if (mode == "fallback") {
		// Server requires Basic. First attempt (digest envelope, no
		// Authorization header) is rejected; the fallback retry with
		// Basic must succeed.
		SoapRequest a;
		a.url = url;
		a.body = DigestEnvelope(user, pass);
		a.soapAction = kSoapAction;
		SoapRequest b = a;
		b.body = PlainEnvelope();
		b.basicCred = basicCred;

		SoapResult r;
		CHECK(obs_onvif::SoapClient().SendWithAuthFallback(a, b, r));
		CHECK(r.transportOk);
		CHECK_EQ(r.httpStatus, 200u);
		CHECK(r.body.find("GetDeviceInformationResponse") !=
		      std::string::npos);
	} else if (mode == "open_fault") {
		SoapRequest req;
		req.url = url;
		req.body = UnsupportedEnvelope();
		req.soapAction =
			"http://www.onvif.org/ver10/device/wsdl/UnsupportedThing";

		const RoundTrip rt = Run(req);
		CHECK(rt.ok);
		CHECK(rt.result.transportOk);
		CHECK_EQ(rt.result.httpStatus, 500u);
		CHECK(rt.result.fault.present);
		CHECK_EQ(rt.result.fault.code, "env:Server");
		CHECK_EQ(rt.result.fault.reason,
			 "Requested operation not implemented");
	} else if (mode == "https_open") {
		SoapRequest req;
		req.url = url;
		req.body = PlainEnvelope();
		req.soapAction = kSoapAction;
		req.validateCert = false;

		const RoundTrip rt = Run(req);
		CHECK(rt.ok);
		CHECK(rt.result.transportOk);
		CHECK_EQ(rt.result.httpStatus, 200u);
	} else if (mode == "https_strict") {
		SoapRequest req;
		req.url = url;
		req.body = PlainEnvelope();
		req.soapAction = kSoapAction;
		req.validateCert = true;

		const RoundTrip rt = Run(req);
		CHECK(!rt.ok); // TLS rejection -> no round trip
		CHECK(!rt.result.transportOk);
	} else {
		std::cerr << "unknown mode: " << mode << std::endl;
		return 2;
	}

	RUN_TESTS(("soap_live/" + mode).c_str());
}