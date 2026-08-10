#pragma once

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "tinyxml2/tinyxml2.h"

namespace obs_onvif {
namespace xml {

// Parses XML text. Returns false (with an error recorded on the document)
// when the input is not well-formed.
bool Parse(const std::string &text, tinyxml2::XMLDocument &doc);

// Element's local name without its namespace prefix ("trt:Fault" -> "Fault").
const char *LocalName(const tinyxml2::XMLElement *el);

// Direct child element whose local name matches; nullptr when absent.
const tinyxml2::XMLElement *Child(const tinyxml2::XMLElement *parent,
				  const char *localName);

// All direct child elements with the given local name.
std::vector<const tinyxml2::XMLElement *> Children(
	const tinyxml2::XMLElement *parent, const char *localName);

// Text content of an element ("" when there is none).
std::string TextOf(const tinyxml2::XMLElement *el);

// Text of the first direct child with the given local name ("" if missing).
std::string ChildText(const tinyxml2::XMLElement *parent, const char *localName);

// Walks a path of local names starting at parent, returning the last element.
const tinyxml2::XMLElement *Descendant(
	const tinyxml2::XMLElement *parent,
	std::initializer_list<const char *> path);

// Text at the end of a Descendant path ("" if the path is absent).
std::string DescendantText(const tinyxml2::XMLElement *parent,
			   std::initializer_list<const char *> path);

// Builds a SOAP envelope. `namespaces` are (prefix, uri) pairs serialized onto
// the Envelope element; `header`/`body` are raw XML fragments that reference
// those prefixes. `soapNs` defaults to the SOAP 1.1 envelope namespace.
std::string Envelope(const std::string &header, const std::string &body,
		     const std::vector<std::pair<std::string, std::string>>
			     &namespaces,
		     const char *soapNs =
			     "http://schemas.xmlsoap.org/soap/envelope/");

} // namespace xml
} // namespace obs_onvif