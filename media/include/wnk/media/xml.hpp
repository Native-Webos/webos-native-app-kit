// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small non-validating XML reader for manifests (DASH MPDs): elements, attributes, text, CDATA,
// comments, processing instructions and a DOCTYPE without an internal subset; the five predefined
// entities and numeric character references. Namespace prefixes are kept in names; local() strips
// them. No external entities are ever resolved, so a hostile manifest cannot reach the file system
// or the network, and nesting and size are bounded.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace wnk::media {

struct XmlNode {
    std::string name;  // as written, with any prefix ("cenc:pssh")
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;
    std::string text;  // character data directly inside this element, concatenated
                       // (with XmlLimits::text_children, also each run as a "#text" child, in order)

    // The name without its namespace prefix.
    std::string local() const;
    // Attribute value by name (prefix ignored when `name` has none), or nullptr.
    const std::string *attribute(const std::string &name) const;
    std::string attribute_or(const std::string &name, const std::string &fallback) const;
    // First child with this local name, or nullptr.
    const XmlNode *child(const std::string &local_name) const;
    std::vector<const XmlNode *> children_named(const std::string &local_name) const;
};

struct XmlLimits {
    size_t max_bytes = 16u << 20;
    int max_depth = 64;
    size_t max_nodes = 1u << 20;
    // Mixed content (TTML's <p>a<span>b</span><br/>c</p>): every run of character data is also a
    // child named "#text" holding it, so text and elements keep their order. Counts as a node.
    bool text_children = false;
};

// Parses a whole document into its root element. False with a short reason (and the byte offset)
// on malformed input or when a limit is exceeded.
bool parse_xml(const std::string &doc, XmlNode *root, std::string *error, const XmlLimits &limits = {});

}  // namespace wnk::media
