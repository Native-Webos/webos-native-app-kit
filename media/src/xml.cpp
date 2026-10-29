// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/media/xml.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace wnk::media {
namespace {

std::string strip_prefix(const std::string &name) {
    const size_t colon = name.find(':');
    return colon == std::string::npos ? name : name.substr(colon + 1);
}

void append_utf8(std::string *out, uint32_t cp) {
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
 public:
    Parser(const std::string &doc, const XmlLimits &limits) : s_(doc), limits_(limits) {}

    bool run(XmlNode *root, std::string *error) {
        if (s_.size() > limits_.max_bytes) return fail("document too large", error);
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;  // UTF-8 byte order mark
        if (!skip_misc(error)) return false;
        if (pos_ >= s_.size() || s_[pos_] != '<') return fail("no root element", error);
        if (!element(root, 0, error)) return false;
        if (!skip_misc(error)) return false;
        if (pos_ != s_.size()) return fail("content after the root element", error);
        return true;
    }

 private:
    bool fail(const char *why, std::string *error) {
        if (error_.empty()) error_ = std::string(why) + " at byte " + std::to_string(pos_);
        *error = error_;
        return false;
    }

    bool starts(const char *lit) const { return s_.compare(pos_, std::strlen(lit), lit) == 0; }

    void skip_space() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }

    // Skips to just after `end`; false if it never comes.
    bool skip_past(const char *end) {
        const size_t at = s_.find(end, pos_);
        if (at == std::string::npos) return false;
        pos_ = at + std::strlen(end);
        return true;
    }

    // Whitespace, comments, processing instructions and a DOCTYPE between top-level items.
    bool skip_misc(std::string *error) {
        for (;;) {
            skip_space();
            if (starts("<?")) {
                if (!skip_past("?>")) return fail("unterminated processing instruction", error);
            } else if (starts("<!--")) {
                if (!skip_past("-->")) return fail("unterminated comment", error);
            } else if (starts("<!DOCTYPE")) {
                const size_t gt = s_.find('>', pos_);
                const size_t bracket = s_.find('[', pos_);
                if (bracket != std::string::npos && bracket < gt) return fail("DOCTYPE with an internal subset", error);
                if (gt == std::string::npos) return fail("unterminated DOCTYPE", error);
                pos_ = gt + 1;
            } else {
                return true;
            }
        }
    }

    static bool name_char(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80;
    }

    bool name(std::string *out) {
        const size_t start = pos_;
        while (pos_ < s_.size() && name_char(s_[pos_])) ++pos_;
        if (pos_ == start) return false;
        out->assign(s_, start, pos_ - start);
        return true;
    }

    // Character data up to `end` with entities decoded.
    bool decode(size_t end, std::string *out, std::string *error) {
        while (pos_ < end) {
            const char c = s_[pos_];
            if (c != '&') {
                *out += c;
                ++pos_;
                continue;
            }
            const size_t semi = s_.find(';', pos_);
            if (semi == std::string::npos || semi > end || semi - pos_ > 12) return fail("bad entity", error);
            const std::string ent = s_.substr(pos_ + 1, semi - pos_ - 1);
            if (ent == "lt") {
                *out += '<';
            } else if (ent == "gt") {
                *out += '>';
            } else if (ent == "amp") {
                *out += '&';
            } else if (ent == "quot") {
                *out += '"';
            } else if (ent == "apos") {
                *out += '\'';
            } else if (ent.size() > 1 && ent[0] == '#') {
                char *stop = nullptr;
                const bool hex = ent[1] == 'x' || ent[1] == 'X';
                const unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), &stop, hex ? 16 : 10);
                if (stop == nullptr || *stop != '\0' || cp == 0 || cp > 0x10FFFF) return fail("bad character reference", error);
                append_utf8(out, static_cast<uint32_t>(cp));
            } else {
                return fail("unknown entity", error);  // no DTD, so nothing else is defined
            }
            pos_ = semi + 1;
        }
        return true;
    }

    // Where character data inside `node` goes: its text, or (text_children) a "#text" child, the
    // last one again when nothing came between (a comment does not split a run).
    std::string *text_run(XmlNode *node, std::string *error) {
        if (!limits_.text_children) return &node->text;
        if (node->children.empty() || node->children.back().name != "#text") {
            if (++nodes_ > limits_.max_nodes) {
                fail("too many elements", error);
                return nullptr;
            }
            node->children.emplace_back();
            node->children.back().name = "#text";
        }
        return &node->children.back().text;
    }

    bool element(XmlNode *node, int depth, std::string *error) {
        if (depth >= limits_.max_depth) return fail("nesting too deep", error);
        if (++nodes_ > limits_.max_nodes) return fail("too many elements", error);
        ++pos_;  // '<'
        if (!name(&node->name)) return fail("bad element name", error);
        for (;;) {
            skip_space();
            if (pos_ >= s_.size()) return fail("unterminated start tag", error);
            if (starts("/>")) {
                pos_ += 2;
                return true;
            }
            if (s_[pos_] == '>') {
                ++pos_;
                break;
            }
            std::string attr;
            if (!name(&attr)) return fail("bad attribute name", error);
            skip_space();
            if (pos_ >= s_.size() || s_[pos_] != '=') return fail("attribute without a value", error);
            ++pos_;
            skip_space();
            if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) return fail("unquoted attribute", error);
            const char quote = s_[pos_++];
            const size_t end = s_.find(quote, pos_);
            if (end == std::string::npos) return fail("unterminated attribute", error);
            std::string value;
            if (!decode(end, &value, error)) return false;
            pos_ = end + 1;
            node->attributes.emplace_back(std::move(attr), std::move(value));
        }
        // Content.
        for (;;) {
            if (pos_ >= s_.size()) return fail("unterminated element", error);
            if (starts("</")) {
                pos_ += 2;
                std::string close;
                if (!name(&close) || close != node->name) return fail("mismatched end tag", error);
                skip_space();
                if (pos_ >= s_.size() || s_[pos_] != '>') return fail("bad end tag", error);
                ++pos_;
                return true;
            }
            if (starts("<!--")) {
                if (!skip_past("-->")) return fail("unterminated comment", error);
            } else if (starts("<![CDATA[")) {
                pos_ += 9;
                const size_t end = s_.find("]]>", pos_);
                if (end == std::string::npos) return fail("unterminated CDATA", error);
                std::string *run = text_run(node, error);
                if (run == nullptr) return false;
                run->append(s_, pos_, end - pos_);
                if (run != &node->text) node->text.append(s_, pos_, end - pos_);
                pos_ = end + 3;
            } else if (starts("<?")) {
                if (!skip_past("?>")) return fail("unterminated processing instruction", error);
            } else if (s_[pos_] == '<') {
                node->children.emplace_back();
                if (!element(&node->children.back(), depth + 1, error)) return false;
            } else {
                size_t end = s_.find('<', pos_);
                if (end == std::string::npos) end = s_.size();
                std::string *run = text_run(node, error);
                if (run == nullptr) return false;
                const size_t from = run->size();
                if (!decode(end, run, error)) return false;
                if (run != &node->text) node->text.append(*run, from, std::string::npos);
            }
        }
    }

    const std::string &s_;
    const XmlLimits &limits_;
    size_t pos_ = 0;
    size_t nodes_ = 0;
    std::string error_;
};

}  // namespace

std::string XmlNode::local() const { return strip_prefix(name); }

const std::string *XmlNode::attribute(const std::string &n) const {
    const bool prefixed = n.find(':') != std::string::npos;
    for (const auto &[key, value] : attributes) {
        if (key == n || (!prefixed && strip_prefix(key) == n && key.compare(0, 6, "xmlns:") != 0)) return &value;
    }
    return nullptr;
}

std::string XmlNode::attribute_or(const std::string &n, const std::string &fallback) const {
    const std::string *v = attribute(n);
    return v != nullptr ? *v : fallback;
}

const XmlNode *XmlNode::child(const std::string &local_name) const {
    for (const XmlNode &c : children) {
        if (c.local() == local_name) return &c;
    }
    return nullptr;
}

std::vector<const XmlNode *> XmlNode::children_named(const std::string &local_name) const {
    std::vector<const XmlNode *> out;
    for (const XmlNode &c : children) {
        if (c.local() == local_name) out.push_back(&c);
    }
    return out;
}

bool parse_xml(const std::string &doc, XmlNode *root, std::string *error, const XmlLimits &limits) {
    *root = XmlNode{};
    return Parser(doc, limits).run(root, error);
}

}  // namespace wnk::media
