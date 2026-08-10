#include "xml.h"

#include <cstring>

namespace obs_onvif {
namespace xml {

bool Parse(const std::string &text, tinyxml2::XMLDocument &doc)
{
	return doc.Parse(text.data(), text.size()) == tinyxml2::XML_SUCCESS;
}

const char *LocalName(const tinyxml2::XMLElement *el)
{
	const char *name = el->Name();
	if (!name)
		return "";
	const char *colon = std::strchr(name, ':');
	return colon ? colon + 1 : name;
}

const tinyxml2::XMLElement *Child(const tinyxml2::XMLElement *parent,
				  const char *localName)
{
	if (!parent)
		return nullptr;
	for (const tinyxml2::XMLElement *el = parent->FirstChildElement();
	     el; el = el->NextSiblingElement()) {
		if (std::strcmp(LocalName(el), localName) == 0)
			return el;
	}
	return nullptr;
}

std::vector<const tinyxml2::XMLElement *> Children(
	const tinyxml2::XMLElement *parent, const char *localName)
{
	std::vector<const tinyxml2::XMLElement *> out;
	if (!parent)
		return out;
	for (const tinyxml2::XMLElement *el = parent->FirstChildElement();
	     el; el = el->NextSiblingElement()) {
		if (std::strcmp(LocalName(el), localName) == 0)
			out.push_back(el);
	}
	return out;
}

std::string TextOf(const tinyxml2::XMLElement *el)
{
	if (!el)
		return "";
	const char *text = el->GetText();
	return text ? text : "";
}

std::string ChildText(const tinyxml2::XMLElement *parent, const char *localName)
{
	return TextOf(Child(parent, localName));
}

const tinyxml2::XMLElement *Descendant(
	const tinyxml2::XMLElement *parent,
	std::initializer_list<const char *> path)
{
	const tinyxml2::XMLElement *el = parent;
	for (const char *name : path) {
		el = Child(el, name);
		if (!el)
			break;
	}
	return el;
}

std::string DescendantText(const tinyxml2::XMLElement *parent,
			   std::initializer_list<const char *> path)
{
	return TextOf(Descendant(parent, path));
}

std::string Envelope(const std::string &header, const std::string &body,
		     const std::vector<std::pair<std::string, std::string>>
			     &namespaces,
		     const char *soapNs)
{
	std::string xmlns;
	for (const auto &ns : namespaces)
		xmlns += " xmlns:" + ns.first + "=\"" + ns.second + "\"";

	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	       "<soap:Envelope xmlns:soap=\"" +
	       soapNs + "\"" + xmlns +
	       ">\n"
	       "<soap:Header>" +
	       header +
	       "</soap:Header>\n"
	       "<soap:Body>" +
	       body +
	       "</soap:Body>\n"
	       "</soap:Envelope>";
}

} // namespace xml
} // namespace obs_onvif