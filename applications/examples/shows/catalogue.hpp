// SPDX-License-Identifier: GPL-3.0-or-later
//
// Data side of Example C, kept free of UI and threads so the host tests can drive it directly.
//
// The catalogue is the public TVmaze API (https://www.tvmaze.com/api, no key, CC BY-SA data):
//   GET {base}/shows?page=N                       250 shows per page, ordered by id; 404 past the end
//   GET {base}/shows/ID?embed[]=seasons&embed[]=cast
// A launch parameter can point {base} at another server with the same routes (the host tests use
// tests/integration/http_server.py, which serves synthetic shows under /catalogue).
//
// Everything parsed here comes from the network: missing or mistyped fields become defaults, never
// exceptions, and strings are only ever shown as text.
#pragma once

#include "wnk/net/http.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shows {

struct Show {
    int id = 0;
    std::string name;
    std::string image;      // medium portrait poster (https), or empty
    float rating = 0;       // 0..10; 0 means not rated
    int weight = 0;         // TVmaze popularity, 0..100
    int year = 0;           // premiere year, 0 if unknown
    int64_t updated = 0;    // unix seconds of the last change to the entry
};

struct Season {
    int number = 0;
    int episodes = 0;       // 0 when not announced
    std::string premiere;   // YYYY-MM-DD or empty
};

struct CastMember {
    std::string person;
    std::string character;
};

struct Details {
    Show show;
    std::string summary;    // plain text, HTML removed
    std::string status;     // "Running", "Ended", ...
    std::string network;    // network or streaming service
    std::string language;
    std::string premiered, ended;
    int runtime = 0;        // minutes
    std::vector<std::string> genres;
    std::vector<Season> seasons;
    std::vector<CastMember> cast;
};

// URLs. `base` has no trailing slash; it is validated once by valid_base().
bool valid_base(const std::string &base);
std::string page_url(const std::string &base, int page);
std::string details_url(const std::string &base, int id);

// Parsers return false with *why set when the document is not what the API returns (not JSON, wrong
// top-level type). Entries without an id or a name are skipped rather than failing the page.
bool parse_show_list(std::string_view json, std::vector<Show> *out, std::string *why);
bool parse_details(std::string_view json, Details *out, std::string *why);

// TVmaze summaries are HTML fragments. Tags are dropped, <p> and <br> become line breaks, the common
// named entities and numeric references are decoded, and runs of whitespace collapse.
std::string strip_html(std::string_view html);

// One sentence a viewer can act on, for a failed request. `status` is used when error == None.
std::string describe_failure(wnk::net::NetError error, long status);
// True for failures where trying again later can help (network, timeouts, 5xx).
bool is_transient(wnk::net::NetError error, long status);

// Compact form of a list of shows for the key/value store. Entries are dropped from the end until
// the encoding fits in max_bytes, so a growing list cannot fail the save with TooLarge. *kept, if
// given, receives the number of shows that made it in.
std::string encode_shows(const std::vector<Show> &shows, int64_t saved_at, size_t max_bytes,
                         size_t *kept = nullptr);
bool decode_shows(std::string_view text, std::vector<Show> *out, int64_t *saved_at);

// Adds `show` to the front of `list` (on) or removes it (off). At most `max` entries are kept; the
// oldest additions fall off the end.
void set_favourite(std::vector<Show> *list, const Show &show, bool on, size_t max);
bool contains(const std::vector<Show> &list, int id);

// Rails on the home screen. Stable: equal keys keep API order.
std::vector<Show> top_rated(const std::vector<Show> &all, size_t n);
std::vector<Show> most_popular(const std::vector<Show> &all, size_t n);
std::vector<Show> recently_updated(const std::vector<Show> &all, size_t n);
// Shows whose name contains every word of `query`, ignoring ASCII case, in catalogue order; all of
// them for a blank query.
std::vector<Show> search_by_name(const std::vector<Show> &all, std::string_view query);

// "just now", "5 min ago", "3 h ago", "2 days ago"; "" for a time in the future (wrong TV clock).
std::string age_text(int64_t seconds);
// "8.4" or "" when not rated; "2013 · 8.4" style subtitle for cards.
std::string card_subtitle(const Show &s);

}  // namespace shows
