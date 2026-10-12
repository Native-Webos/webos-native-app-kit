// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example C: a networked app. It browses the public TVmaze catalogue over HTTPS and shows what a
// TV app has to get right around the network:
//
//   ┌──────────┬──────────────────────────────────────────────────┐
//   │ Home     │  Updated just now · 250 shows                    │  status: live, saved copy, offline
//   │ Browse   │  Top rated     [poster][poster][poster]...       │  rails built from page 0
//   │ Favourites│ Most popular  [poster][poster][poster]...       │
//   │ About    │  Recently updated ...                            │
//   └──────────┴──────────────────────────────────────────────────┘
//
//   * every request and every DB8 call runs on the worker pool; JSON is parsed there too, so a
//     370 KB page never stalls a frame; results come back through the main dispatcher and are
//     dropped if the screen that asked has gone (async::Lifetime);
//   * Browse is a paged grid: focusing the last two rows fetches the next page (404 = end);
//   * a details screen fetches seasons and cast; Back while it loads cancels the transfer;
//   * failures become one sentence a viewer can act on, with Try again; transient ones (network,
//     timeouts, 5xx) were already retried by HttpClient with backoff;
//   * the most popular shows are saved in DB8 after each refresh; at the next launch they are shown
//     at once and replaced when the network answers, or kept (marked "saved copy") when it does not;
//   * favourites live in DB8 too, written with compare-and-set so two writers cannot lose an update;
//   * returning to the app refreshes a catalogue older than 15 minutes.
//
// Launch params (ares-launch --params):
//   {"api":"http://HOST:PORT/catalogue"}  another server with the TVmaze routes (tests/integration/
//                                         http_server.py serves synthetic ones); default TVmaze
//   {"wnkMetrics":true}                   frame-timing overlay (also the Blue key)
//
// Scripted runs (host): WNK_SHOWS_KEYS="Right,Return" and WNK_SHOWS_SHOT=/path/out.bmp (see wnk/app).
#include "catalogue.hpp"
#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/ui/collection.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/ui.hpp"
#include "wnk/ui/widgets.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace wnk::ui;
using shows::Show;
namespace async = wnk::async;
namespace net = wnk::net;
namespace storage = wnk::storage;

constexpr const char *kFallbackAppId = "org.wnk.examples.shows";
constexpr const char *kCat = "shows";
constexpr const char *kDefaultApi = "https://api.tvmaze.com";
constexpr const char *kSavedKey = "catalogue.v1";
constexpr const char *kFavouritesKey = "favourites.v1";
constexpr size_t kSavedShows = 150;        // ~25 KB in DB8, well under the 64 KiB value limit
constexpr size_t kMaxFavourites = 200;
constexpr size_t kRailLength = 30;
constexpr int kGridColumns = 6;
constexpr int64_t kStaleSeconds = 15 * 60;
constexpr int64_t kPageRetrySeconds = 10;  // after a failed page, wait before focus asks again
const wnk::Size kRailCell{220, 372};       // 210x295 posters at 220 wide (309) plus two text lines
const wnk::Size kGridCell{220, 372};
constexpr float kPosterAspect = 295.0f / 210.0f;

int64_t now_s() { return static_cast<int64_t>(std::time(nullptr)); }

// ---- service: everything that blocks ---------------------------------------------------------------

// How a request ended, in terms the UI needs.
struct Outcome {
    bool ok = false;
    net::NetError error = net::NetError::None;
    long status = 0;
    std::string bad_data;  // the reply arrived but is not what the API returns
    double seconds = 0;
    int attempts = 0;

    std::string message() const {
        return bad_data.empty() ? shows::describe_failure(error, status)
                                : "The catalogue sent a reply the app cannot read.";
    }
    std::string summary() const {
        char s[160];
        if (!bad_data.empty())
            std::snprintf(s, sizeof s, "unreadable reply (%s)", bad_data.c_str());
        else if (error != net::NetError::None)
            std::snprintf(s, sizeof s, "%s after %d attempt(s)", net::error_name(error), attempts);
        else
            std::snprintf(s, sizeof s, "HTTP %ld after %d attempt(s)", status, attempts);
        return s;
    }
};

struct PageResult {
    Outcome outcome;
    int page = 0;
    bool end = false;  // past the last page
    std::vector<Show> shows;
};

struct DetailsResult {
    Outcome outcome;
    shows::Details details;
};

struct SavedState {
    storage::Status catalogue = storage::Status::Unavailable;
    storage::Status favourites = storage::Status::Unavailable;
    std::vector<Show> shows;
    int64_t saved_at = 0;
    std::vector<Show> favourite_shows;
};

struct FavouriteResult {
    storage::Status status = storage::Status::Unavailable;
    std::vector<Show> favourites;  // as stored, when status == Ok
    int conflicts = 0;
};

// Runs requests and store calls on the pool and posts results to the main thread. Worker lambdas
// capture the client and the store, never `this`, so a task that outlives the shutdown deadline does
// not reach into a destroyed delegate.
class Service {
 public:
    Service(wnk::app::Context &ctx, std::string base)
        : pool_(ctx.pool()), main_(ctx.dispatcher()), store_(ctx.store()), base_(std::move(base)) {
        if (auto http = ctx.http()) {
            client_ = std::make_shared<net::HttpClient>(http, pool_, main_);
            transport_ = http->describe();
        }
    }

    bool online() const { return client_ != nullptr; }
    bool has_store() const { return store_ != nullptr; }
    const std::string &base() const { return base_; }
    const std::string &transport() const { return transport_; }
    const char *store_name() const { return store_ != nullptr ? store_->backend_name() : "none"; }

    void fetch_page(int page, async::CancellationToken token, async::Lifetime::Weak owner,
                    std::function<void(PageResult)> done) {
        auto client = client_;
        const std::string url = shows::page_url(base_, page);
        async::run_then_post<PageResult>(
            pool_, main_, owner, token,
            [client, url, page](const async::CancellationToken &t) {
                PageResult r;
                r.page = page;
                const net::HttpResponse res = get(*client, url, t, &r.outcome);
                if (page > 0 && res.error == net::NetError::None && res.status == 404) {
                    r.end = true;  // TVmaze answers 404 past the last page
                    r.outcome.ok = true;
                } else if (r.outcome.ok && !shows::parse_show_list(res.body, &r.shows, &r.outcome.bad_data)) {
                    r.outcome.ok = false;
                }
                return r;
            },
            std::move(done));
    }

    void fetch_details(int id, async::CancellationToken token, async::Lifetime::Weak owner,
                       std::function<void(DetailsResult)> done) {
        auto client = client_;
        const std::string url = shows::details_url(base_, id);
        async::run_then_post<DetailsResult>(
            pool_, main_, owner, token,
            [client, url, id](const async::CancellationToken &t) {
                DetailsResult r;
                const net::HttpResponse res = get(*client, url, t, &r.outcome);
                if (res.error == net::NetError::Cancelled) {
                    // Nobody will see this result; the log shows that the transfer really stopped.
                    WNK_LOGI(kCat, "details %d: request cancelled after %.2f s", id, res.total_seconds);
                } else if (r.outcome.ok && !shows::parse_details(res.body, &r.details, &r.outcome.bad_data)) {
                    r.outcome.ok = false;
                }
                return r;
            },
            std::move(done));
    }

    void load_saved(async::Lifetime::Weak owner, std::function<void(SavedState)> done) {
        storage::KeyValueStore *store = store_;
        async::run_then_post<SavedState>(
            pool_, main_, owner, {},
            [store](const async::CancellationToken &) {
                SavedState s;
                if (store == nullptr) return s;
                storage::Entry e;
                s.catalogue = store->get(kSavedKey, &e);
                if (s.catalogue == storage::Status::Ok && !shows::decode_shows(e.value, &s.shows, &s.saved_at))
                    s.catalogue = storage::Status::Corrupt;
                s.favourites = store->get(kFavouritesKey, &e);
                if (s.favourites == storage::Status::Ok && !shows::decode_shows(e.value, &s.favourite_shows, nullptr))
                    s.favourites = storage::Status::Corrupt;
                return s;
            },
            std::move(done));
    }

    // Fire and forget: a failed save only costs the offline copy.
    void save_catalogue(std::vector<Show> list) {
        storage::KeyValueStore *store = store_;
        if (store == nullptr) return;
        pool_.submit([store, list = std::move(list)](const async::CancellationToken &) {
            size_t kept = 0;
            const std::string value = shows::encode_shows(list, now_s(), store->max_value_bytes(), &kept);
            const storage::PutResult r = store->put(kSavedKey, value);
            WNK_LOGI(kCat, "saved copy: %zu shows, %zu bytes: %s", kept, value.size(), storage::status_name(r.status));
        });
    }

    // Read, change, write with the revision that was read; on Conflict (another writer got in between)
    // start again from a fresh read.
    void set_favourite(const Show &show, bool on, async::Lifetime::Weak owner, std::function<void(FavouriteResult)> done) {
        storage::KeyValueStore *store = store_;
        async::run_then_post<FavouriteResult>(
            pool_, main_, owner, {},
            [store, show, on](const async::CancellationToken &) {
                FavouriteResult r;
                if (store == nullptr) return r;
                for (int attempt = 0; attempt < 4; ++attempt) {
                    storage::Entry e;
                    const storage::Status st = store->get(kFavouritesKey, &e);
                    if (st != storage::Status::Ok && st != storage::Status::NotFound) {
                        r.status = st;
                        return r;
                    }
                    std::vector<Show> list;
                    if (st == storage::Status::Ok) shows::decode_shows(e.value, &list, nullptr);
                    shows::set_favourite(&list, show, on, kMaxFavourites);
                    const uint64_t expected = st == storage::Status::Ok ? e.revision : storage::kMustNotExist;
                    const storage::PutResult put = store->put(
                        kFavouritesKey, shows::encode_shows(list, now_s(), store->max_value_bytes()), expected);
                    r.status = put.status;
                    if (put.status == storage::Status::Conflict) {
                        ++r.conflicts;
                        continue;
                    }
                    if (put.status == storage::Status::Ok) r.favourites = std::move(list);
                    return r;
                }
                return r;
            },
            std::move(done));
    }

    void forget_saved(async::Lifetime::Weak owner, std::function<void(storage::Status)> done) {
        storage::KeyValueStore *store = store_;
        async::run_then_post<storage::Status>(
            pool_, main_, owner, {},
            [store](const async::CancellationToken &) {
                if (store == nullptr) return storage::Status::Unavailable;
                storage::Status a = store->remove(kSavedKey);
                storage::Status b = store->remove(kFavouritesKey);
                if (a == storage::Status::NotFound) a = storage::Status::Ok;
                if (b == storage::Status::NotFound) b = storage::Status::Ok;
                return a != storage::Status::Ok ? a : b;
            },
            std::move(done));
    }

 private:
    static net::HttpResponse get(net::HttpClient &client, const std::string &url, const async::CancellationToken &t,
                                 Outcome *o) {
        net::HttpRequest req;
        req.url = url;
        req.headers.emplace_back("Accept", "application/json");
        net::HttpResponse res = client.perform_with_retry(req, t);
        o->ok = res.ok();
        o->error = res.error;
        o->status = res.status;
        o->seconds = res.total_seconds;
        o->attempts = res.attempts;
        return res;
    }

    async::WorkerPool &pool_;
    async::MainDispatcher &main_;
    storage::KeyValueStore *store_;
    std::shared_ptr<net::HttpClient> client_;
    std::string base_;
    std::string transport_;
};

// ---- widgets ---------------------------------------------------------------------------------------

std::unique_ptr<Card> make_card(const Show &s, std::string name) {
    auto card = std::make_unique<Card>(s.name, s.image, shows::card_subtitle(s));
    card->image_aspect = kPosterAspect;
    card->name = std::move(name);
    return card;
}

// Sidebar entry (as in Example B): the D-pad selects on focus, the pointer selects on click.
class NavItem : public Button {
 public:
    NavItem(std::string text, std::function<void(Ui &)> on_select) : Button(std::move(text)), on_select_(on_select) {
        set_align(Align::Stretch, Align::Start);
        set_on_click(std::move(on_select));
    }
    void on_focus_changed(Ui &ui, bool focused) override {
        if (focused && on_select_ && ui.focus_source() != FocusSource::Pointer) on_select_(ui);
    }
    bool selected = false;

 protected:
    void draw(Ui &ui) override {
        Button::draw(ui);
        if (!selected) return;
        const wnk::Rect f = frame();
        ui.renderer().fill_rect({f.x + 14, f.y + f.h * 0.3f, 6, f.h * 0.4f}, ui.fade(ui.theme().accent), 3);
    }

 private:
    std::function<void(Ui &)> on_select_;
};

// ---- details ---------------------------------------------------------------------------------------

// Asks the home screen to change a favourite; `after` runs with the new state if `requester` is alive.
using FavouriteToggle =
    std::function<void(const Show &, bool on, async::Lifetime::Weak requester, std::function<void(bool)> after)>;

class DetailsScreen : public Screen {
 public:
    DetailsScreen(Service &service, Show show, bool favourite, FavouriteToggle toggle)
        : Screen(show.name), service_(service), show_(std::move(show)), favourite_(favourite),
          toggle_(std::move(toggle)) {
        name = "details";
        set_padding({100, 80, 100, 60});
        auto &row = add<Row>(64);
        auto &poster = row.add<ImageView>(show_.image);
        poster.set_size(420, 420 * kPosterAspect).set_align(Align::Start, Align::Start);
        poster.set_background(std::nullopt, 16);
        auto &scroll = row.add<ScrollView>();
        scroll.set_width(Dim::fill());
        auto &col = scroll.add<Column>(22);
        col.add<Label>(show_.name, TextRole::Title).set_max_lines(2);
        meta_ = &col.add<Label>(shows::card_subtitle(show_));
        meta_->set_muted(true);
        genres_ = &col.add<Label>();
        genres_->set_muted(true);
        auto &buttons = col.add<Row>(24);
        buttons.set_padding({0, 12, 0, 12});
        fav_ = &buttons.add<Button>(fav_text());
        fav_->name = "details.fav";
        fav_->set_on_click([this](Ui &) { toggle_favourite(); });
        retry_ = &buttons.add<Button>("Try again", [this](Ui &) { load(); });
        retry_->name = "details.retry";
        retry_->set_visible(false);
        busy_ = &col.add<Row>(20);
        busy_->add<Spinner>(48);
        busy_->add<Label>("Loading details…").set_muted(true);
        error_ = &col.add<Label>();
        error_->set_max_lines(2).set_visible(false);
        summary_ = &col.add<Label>();
        summary_->set_max_lines(7);
        seasons_title_ = &col.add<Label>("Seasons");
        seasons_title_->set_bold(true).set_visible(false);
        seasons_ = &col.add<Label>();
        seasons_->set_max_lines(3).set_muted(true);
        cast_title_ = &col.add<Label>("Cast");
        cast_title_->set_bold(true).set_visible(false);
        cast_ = &col.add<Label>();
        cast_->set_max_lines(3).set_muted(true);
    }

    ~DetailsScreen() override {
        // Leaving before the reply: stop the transfer instead of letting it finish for nobody.
        if (loading_) {
            WNK_LOGI(kCat, "details %d: closed while loading, cancelling the request", show_.id);
            cancel_.cancel();
        }
    }

    void on_show(Ui &ui) override {
        Screen::on_show(ui);
        if (!requested_) load();
    }

 private:
    std::string fav_text() const { return favourite_ ? "Remove from favourites" : "Add to favourites"; }

    void load() {
        requested_ = true;
        if (!service_.online()) {
            show_error(shows::describe_failure(net::NetError::Unsupported, 0));
            return;
        }
        loading_ = true;
        busy_->set_visible(true);
        error_->set_visible(false);
        retry_->set_visible(false);
        const int id = show_.id;
        service_.fetch_details(id, cancel_.token(), life_.weak(), [this, id](DetailsResult r) {
            loading_ = false;
            busy_->set_visible(false);
            if (!r.outcome.ok) {
                WNK_LOGW(kCat, "details %d: %s", id, r.outcome.summary().c_str());
                show_error(r.outcome.message());
                return;
            }
            WNK_LOGI(kCat, "details %d loaded in %.2f s: %zu seasons, %zu cast", id, r.outcome.seconds,
                     r.details.seasons.size(), r.details.cast.size());
            fill(r.details);
        });
    }

    void show_error(const std::string &message) {
        busy_->set_visible(false);
        error_->set_text(message).set_visible(true);
        retry_->set_visible(true);
    }

    void fill(const shows::Details &d) {
        std::vector<std::string> parts;
        std::string years = d.premiered.size() >= 4 ? d.premiered.substr(0, 4) : "";
        if (!years.empty() && d.ended.size() >= 4 && d.ended.compare(0, 4, years) != 0) years += "–" + d.ended.substr(0, 4);
        else if (!years.empty() && d.status == "Running") years += "–";
        for (const std::string &p : {years, d.network, d.runtime > 0 ? std::to_string(d.runtime) + " min" : "", d.status})
            if (!p.empty()) parts.push_back(p);
        if (d.show.rating > 0) {
            char r[24];
            std::snprintf(r, sizeof r, "Rated %.1f", static_cast<double>(d.show.rating));
            parts.emplace_back(r);
        }
        meta_->set_text(join(parts, " · "));
        genres_->set_text(join(d.genres, ", "));
        summary_->set_text(d.summary.empty() ? "No summary yet." : d.summary);
        if (!d.seasons.empty()) {
            int episodes = 0;
            std::vector<std::string> per;
            for (const shows::Season &s : d.seasons) {
                episodes += s.episodes;
                std::string t = "S" + std::to_string(s.number);
                if (s.episodes > 0) t += " " + std::to_string(s.episodes) + " ep";
                if (s.premiere.size() >= 4) t += " (" + s.premiere.substr(0, 4) + ")";
                per.push_back(std::move(t));
            }
            std::string head = std::to_string(d.seasons.size()) + (d.seasons.size() == 1 ? " season" : " seasons");
            if (episodes > 0) head += ", " + std::to_string(episodes) + " episodes";
            seasons_->set_text(head + "\n" + join(per, " · "));
            seasons_title_->set_visible(true);
        }
        if (!d.cast.empty()) {
            std::vector<std::string> names;
            for (size_t i = 0; i < d.cast.size() && i < 12; ++i)
                names.push_back(d.cast[i].character.empty() ? d.cast[i].person
                                                            : d.cast[i].person + " as " + d.cast[i].character);
            cast_->set_text(join(names, ", "));
            cast_title_->set_visible(true);
        }
    }

    void toggle_favourite() {
        if (saving_) return;  // one write at a time from this button
        saving_ = true;
        toggle_(show_, !favourite_, life_.weak(), [this](bool now_favourite) {
            saving_ = false;
            favourite_ = now_favourite;
            fav_->set_text(fav_text());
        });
    }

    static std::string join(const std::vector<std::string> &v, const char *sep) {
        std::string out;
        for (const std::string &s : v) out += (out.empty() ? "" : sep) + s;
        return out;
    }

    Service &service_;
    Show show_;
    bool favourite_;
    FavouriteToggle toggle_;
    async::CancellationSource cancel_;
    async::Lifetime life_;
    bool requested_ = false, loading_ = false, saving_ = false;
    Label *meta_ = nullptr, *genres_ = nullptr, *error_ = nullptr, *summary_ = nullptr;
    Label *seasons_title_ = nullptr, *seasons_ = nullptr, *cast_title_ = nullptr, *cast_ = nullptr;
    Button *fav_ = nullptr, *retry_ = nullptr;
    Row *busy_ = nullptr;
};

// ---- home ------------------------------------------------------------------------------------------

class HomeScreen : public Screen {
 public:
    HomeScreen(Service &service, Navigator &nav) : Screen("home"), service_(service), nav_(nav) {
        name = "home";
        auto &row = add<Row>(0);

        auto &sidebar = row.add<Column>(16);
        sidebar.set_width(Dim::fixed(340));
        sidebar.set_padding({40, 80, 40, 40});
        sidebar.set_focus_scope(false, true);
        sidebar.set_background(wnk::render::Color::hex(0x0A0F18FF));
        sidebar.add<Label>("Shows", TextRole::Title).set_padding({8, 0, 0, 40});

        auto &content = row.add<Column>(12);
        content.set_width(Dim::fill());
        content.set_padding({60, 50, 0, 0});
        status_ = &content.add<Label>("Loading…", TextRole::Small);
        status_->set_muted(true);
        stack_ = &content.add<View>();
        stack_->set_height(Dim::fill());

        const char *names[] = {"Home", "Browse", "Favourites", "About"};
        View *sections[] = {&build_home(), &build_browse(), &build_favourites(), &build_about()};
        for (int i = 0; i < 4; ++i) {
            sections_.push_back(sections[i]);
            auto &item = sidebar.add<NavItem>(names[i], [this, i](Ui &) { show_section(i); });
            item.name = std::string("nav.") + names[i];
            nav_items_.push_back(&item);
        }
        show_section(0);
    }

    // Saved copy first (fast, local), then the network.
    void start(Ui &ui) {
        ui_ = &ui;
        service_.load_saved(life_.weak(), [this](SavedState s) {
            if (s.favourites == storage::Status::Ok) favourites_ = std::move(s.favourite_shows);
            WNK_LOGI(kCat, "store %s: saved copy %s (%zu shows), favourites %s (%zu)", service_.store_name(),
                     storage::status_name(s.catalogue), s.shows.size(), storage::status_name(s.favourites),
                     favourites_.size());
            refresh_favourites();
            if (!s.shows.empty()) {
                WNK_LOGI(kCat, "showing saved copy of %zu shows from %s", s.shows.size(),
                         shows::age_text(now_s() - s.saved_at).c_str());
                apply(std::move(s.shows), s.saved_at, true);
            }
            refresh();
        });
    }

    void refresh() {
        refresh_.cancel();  // a newer refresh replaces an older one
        refresh_ = async::CancellationSource();
        paging_.cancel();   // a page of the old list must not be appended to the new one
        paging_ = async::CancellationSource();
        paging_busy_ = false;
        if (!service_.online()) {
            Outcome o;
            o.error = net::NetError::Unsupported;
            failed(o);
            return;
        }
        refreshing_ = true;
        offline_ = false;
        // While the error panel is up it stays, so the focused Try again button does not disappear.
        if (all_.empty() && error_->visible()) error_text_->set_text("Trying again…");
        else if (all_.empty()) show_loading();
        update_status();
        service_.fetch_page(0, refresh_.token(), life_.weak(), [this](PageResult r) {
            refreshing_ = false;
            if (r.outcome.ok && r.shows.empty()) r.outcome.bad_data = "no shows on the first page";
            if (!r.outcome.ok || !r.outcome.bad_data.empty()) {
                WNK_LOGW(kCat, "refresh failed: %s", r.outcome.summary().c_str());
                r.outcome.ok = false;
                failed(r.outcome);
                return;
            }
            WNK_LOGI(kCat, "catalogue page 0: %zu shows in %.2f s (%d attempt(s))", r.shows.size(), r.outcome.seconds,
                     r.outcome.attempts);
            pages_ = 1;
            end_ = false;
            apply(std::move(r.shows), now_s(), false);
            service_.save_catalogue(shows::most_popular(all_, kSavedShows));
        });
    }

    // Back in the foreground: a catalogue older than kStaleSeconds (or one that could not refresh) is
    // fetched again. Nothing runs while the app is in the background.
    void on_resumed() {
        if (refreshing_) return;
        if (offline_ || all_.empty() || now_s() - shown_at_ > kStaleSeconds) {
            WNK_LOGI(kCat, "back in the foreground: refreshing");
            refresh();
        } else {
            update_status();
        }
    }

    void stop() {
        refresh_.cancel();
        paging_.cancel();
    }

    void toggle_favourite(const Show &show, bool on, async::Lifetime::Weak requester, std::function<void(bool)> after) {
        const int id = show.id;
        service_.set_favourite(show, on, life_.weak(), [this, show, id, on, requester, after](FavouriteResult r) {
            if (r.status == storage::Status::Ok) {
                favourites_ = std::move(r.favourites);
                WNK_LOGI(kCat, "favourites: %zu saved (%d conflict(s))", favourites_.size(), r.conflicts);
                ui_->toast(on ? "Added to favourites" : "Removed from favourites");
            } else if (!service_.has_store()) {
                shows::set_favourite(&favourites_, show, on, kMaxFavourites);
                ui_->toast("Favourites are kept only until the app closes on this TV");
            } else {
                WNK_LOGW(kCat, "favourites: save failed: %s", storage::status_name(r.status));
                ui_->toast(std::string("Could not save favourites (") + storage::status_name(r.status) + ")");
            }
            refresh_favourites();
            if (requester.alive()) after(shows::contains(favourites_, id));
        });
    }

 private:
    void show_section(int i) {
        if (i == current_) return;
        current_ = i;
        for (size_t s = 0; s < sections_.size(); ++s) {
            sections_[s]->set_visible(static_cast<int>(s) == i);
            nav_items_[s]->selected = static_cast<int>(s) == i;
        }
        if (i == 3) update_about();
    }

    void open_details(Ui &ui, const Show &s) {
        WNK_LOGI(kCat, "open details for show %d", s.id);
        nav_.push(ui, std::make_unique<DetailsScreen>(
                          service_, s, shows::contains(favourites_, s.id),
                          [this](const Show &show, bool on, async::Lifetime::Weak requester, std::function<void(bool)> after) {
                              toggle_favourite(show, on, requester, std::move(after));
                          }));
    }

    // New list (from the network or the saved copy) for the rails and the grid.
    void apply(std::vector<Show> list, int64_t at, bool from_saved) {
        // First data while the viewer waits on "Home" in the sidebar or on Try again: move into the
        // first rail. Decided before the error panel is hidden, which would move focus by itself.
        const bool move_focus = all_.empty() && ui_ != nullptr &&
                                (ui_->is_focused(nav_items_[0]) || ui_->is_focused(retry_));
        all_ = std::move(list);
        from_saved_ = from_saved;
        shown_at_ = at;
        rail_data_[0] = shows::top_rated(all_, kRailLength);
        rail_data_[1] = shows::most_popular(all_, kRailLength);
        rail_data_[2] = shows::recently_updated(all_, kRailLength);
        for (size_t r = 0; r < rails_.size(); ++r) {
            rails_[r]->set_items(rail_data_[r].size(), [this, r](Ui &, size_t i) {
                auto card = make_card(rail_data_[r][i], "rail" + std::to_string(r) + ".item" + std::to_string(i));
                return card;
            });
            rails_[r]->on_activate = [this, r](Ui &ui, size_t i) { open_details(ui, rail_data_[r][i]); };
        }
        rails_box_->set_visible(true);
        loading_->set_visible(false);
        error_->set_visible(false);
        set_grid_items();
        update_status();
        if (move_focus) ui_->focus_within(rails_[0]);
    }

    void set_grid_items() {
        grid_->set_items(all_.size(), [this](Ui &, size_t i) { return make_card(all_[i], "grid.item" + std::to_string(i)); });
        char text[96];
        std::snprintf(text, sizeof text, "%zu shows%s", all_.size(),
                      from_saved_ ? " in the saved copy" : end_ ? ", the whole catalogue" : "");
        browse_info_->set_text(text);
    }

    void load_more() {
        if (from_saved_ || end_ || paging_busy_ || !service_.online()) return;
        if (now_s() - page_failed_at_ < kPageRetrySeconds) return;
        paging_busy_ = true;
        const int page = pages_;
        browse_info_->set_text("Loading page " + std::to_string(page + 1) + "…");
        WNK_LOGI(kCat, "loading page %d", page);
        service_.fetch_page(page, paging_.token(), life_.weak(), [this](PageResult r) {
            paging_busy_ = false;
            if (!r.outcome.ok) {
                WNK_LOGW(kCat, "page %d failed: %s", r.page, r.outcome.summary().c_str());
                page_failed_at_ = now_s();
                ui_->toast(r.outcome.message());
                set_grid_items();
                return;
            }
            if (r.end || r.shows.empty()) {
                WNK_LOGI(kCat, "page %d: end of the catalogue at %zu shows", r.page, all_.size());
                end_ = true;
            } else {
                WNK_LOGI(kCat, "page %d: %zu shows in %.2f s", r.page, r.shows.size(), r.outcome.seconds);
                all_.insert(all_.end(), r.shows.begin(), r.shows.end());
                pages_ = r.page + 1;
            }
            set_grid_items();
            update_status();
        });
    }

    void failed(const Outcome &o) {
        offline_ = true;
        if (all_.empty()) {
            WNK_LOGI(kCat, "showing error: %s", o.message().c_str());
            loading_->set_visible(false);
            rails_box_->set_visible(false);
            error_text_->set_text(o.message());
            error_->set_visible(true);
            retry_->set_visible(service_.online());
            if (service_.online() && ui_->is_focused(nav_items_[0])) ui_->focus_within(retry_);  // one OK to retry
        } else {
            WNK_LOGI(kCat, "keeping the %s list: %s", from_saved_ ? "saved" : "earlier", o.message().c_str());
            ui_->toast(o.message() + (from_saved_ ? " Showing the saved copy." : ""));
        }
        update_status();
    }

    void show_loading() {
        loading_->set_visible(true);
        error_->set_visible(false);
        rails_box_->set_visible(false);
    }

    void update_status() {
        std::string s;
        if (all_.empty()) {
            s = refreshing_ ? "Loading…" : "No catalogue";
        } else {
            const std::string age = shows::age_text(now_s() - shown_at_);
            s = (from_saved_ ? "Saved copy from " : "Updated ") + (age.empty() ? std::string("earlier") : age) + " · " +
                std::to_string(all_.size()) + " shows";
            if (refreshing_) s += " · refreshing…";
            else if (offline_) s += " · offline";
        }
        status_->set_text(s);
        if (current_ == 3) update_about();
    }

    void refresh_favourites() {
        fav_grid_->set_items(favourites_.size(),
                             [this](Ui &, size_t i) { return make_card(favourites_[i], "fav.item" + std::to_string(i)); });
        fav_empty_->set_visible(favourites_.empty());
        fav_grid_->set_visible(!favourites_.empty());
    }

    void update_about() {
        about_[0]->set_text("Catalogue: " + service_.base());
        about_[1]->set_text("HTTP: " + (service_.online() ? service_.transport() : std::string("not available")));
        about_[2]->set_text(std::string("Storage: ") + service_.store_name());
        about_[3]->set_text(std::to_string(favourites_.size()) + " favourites");
    }

    View &build_home() {
        auto &scroll = stack_->add<ScrollView>();
        scroll.name = "section.home";
        scroll.margin = 120;
        auto &col = scroll.add<Column>(20);
        col.set_padding({0, 0, 0, 60});

        loading_ = &col.add<Row>(24);
        loading_->set_padding({16, 120, 0, 0});
        loading_->add<Spinner>(64);
        loading_->add<Label>("Loading the catalogue…").set_muted(true);

        error_ = &col.add<Column>(28);
        error_->set_padding({16, 120, 80, 0});
        error_->add<Label>("Cannot load the catalogue", TextRole::Title);
        error_text_ = &error_->add<Label>();
        error_text_->set_max_lines(3);
        retry_ = &error_->add<Button>("Try again", [this](Ui &) { refresh(); });
        retry_->set_align(Align::Start, Align::Start);
        retry_->name = "home.retry";
        error_->set_visible(false);

        rails_box_ = &col.add<Column>(20);
        rails_box_->set_visible(false);
        const char *titles[] = {"Top rated", "Most popular", "Recently updated"};
        for (int r = 0; r < 3; ++r) {
            rails_box_->add<Label>(titles[r]).set_bold(true).set_padding({16, 0, 0, 0});
            auto &rail = rails_box_->add<CollectionView>(Axis::Horizontal, 1, kRailCell, 28);
            rail.set_width(Dim::fill());
            rail.name = "rail" + std::to_string(r);
            rails_.push_back(&rail);
        }
        return scroll;
    }

    View &build_browse() {
        auto &col = stack_->add<Column>(16);
        col.name = "section.browse";
        auto &head = col.add<Row>(24);
        head.add<Label>("All shows", TextRole::Title);
        browse_info_ = &head.add<Label>("", TextRole::Small);
        browse_info_->set_muted(true).set_align(Align::Start, Align::End);
        grid_ = &col.add<CollectionView>(Axis::Vertical, kGridColumns, kGridCell, 28);
        grid_->set_height(Dim::fill());
        grid_->name = "grid";
        grid_->on_activate = [this](Ui &ui, size_t i) { open_details(ui, all_[i]); };
        grid_->on_focus = [this](Ui &, size_t i) {
            if (i + 2 * kGridColumns >= all_.size()) load_more();
        };
        return col;
    }

    View &build_favourites() {
        auto &col = stack_->add<Column>(16);
        col.name = "section.favourites";
        col.add<Label>("Favourites", TextRole::Title);
        fav_empty_ = &col.add<Label>("Shows you add from their details page appear here. They are kept on this TV.");
        fav_empty_->set_max_lines(2).set_muted(true);
        fav_grid_ = &col.add<CollectionView>(Axis::Vertical, kGridColumns, kGridCell, 28);
        fav_grid_->set_height(Dim::fill());
        fav_grid_->name = "favgrid";
        fav_grid_->on_activate = [this](Ui &ui, size_t i) { open_details(ui, favourites_[i]); };
        return col;
    }

    View &build_about() {
        auto &col = stack_->add<Column>(24);
        col.name = "section.about";
        col.set_padding({16, 0, 80, 0});
        col.add<Label>("About", TextRole::Title);
        col.add<Label>("Show data and posters: TVmaze.com, CC BY-SA 4.0.").set_max_lines(2);
        for (auto &label : about_) {
            label = &col.add<Label>();
            label->set_muted(true).set_max_lines(2);
        }
        auto &buttons = col.add<Row>(24);
        buttons.set_padding({0, 20, 0, 0});
        buttons.add<Button>("Refresh now", [this](Ui &) { refresh(); }).name = "about.refresh";
        buttons.add<Button>("Forget saved data", [this](Ui &ui) { confirm_forget(ui); }).name = "about.forget";
        return col;
    }

    void confirm_forget(Ui &ui) {
        auto d = std::make_unique<Dialog>("Forget saved data?",
                                          "The saved copy of the catalogue and your favourites are removed from this TV.");
        d->add_button("Cancel").name = "dialog.cancel";
        d->add_button("Forget", [this](Ui &) {
            service_.forget_saved(life_.weak(), [this](storage::Status st) {
                WNK_LOGI(kCat, "forget saved data: %s", storage::status_name(st));
                if (st == storage::Status::Ok) {
                    favourites_.clear();
                    refresh_favourites();
                    update_about();
                }
                ui_->toast(st == storage::Status::Ok ? "Saved data removed"
                                                     : std::string("Could not remove saved data (") +
                                                           storage::status_name(st) + ")");
            });
        }).name = "dialog.forget";
        ui.show_dialog(std::move(d));
    }

    Service &service_;
    Navigator &nav_;
    Ui *ui_ = nullptr;
    async::Lifetime life_;
    async::CancellationSource refresh_, paging_;

    std::vector<Show> all_;          // grid order: API order (live) or popularity (saved copy)
    std::array<std::vector<Show>, 3> rail_data_;
    std::vector<Show> favourites_;
    bool from_saved_ = false, refreshing_ = false, offline_ = false, end_ = false, paging_busy_ = false;
    int pages_ = 0;
    int64_t shown_at_ = 0, page_failed_at_ = 0;

    Label *status_ = nullptr, *error_text_ = nullptr, *browse_info_ = nullptr, *fav_empty_ = nullptr;
    std::array<Label *, 4> about_{};
    View *stack_ = nullptr;
    Row *loading_ = nullptr;
    Column *error_ = nullptr, *rails_box_ = nullptr;
    Button *retry_ = nullptr;
    std::vector<CollectionView *> rails_;
    CollectionView *grid_ = nullptr, *fav_grid_ = nullptr;
    std::vector<View *> sections_;
    std::vector<NavItem *> nav_items_;
    int current_ = -1;
};

// ---- app -------------------------------------------------------------------------------------------

class ShowsApp : public wnk::app::Delegate {
 public:
    void on_start(wnk::app::Context &ctx) override {
        std::string base = kDefaultApi;
        const auto params =
            nlohmann::json::parse(ctx.launch().params_json.empty() ? "{}" : ctx.launch().params_json, nullptr, false);
        if (params.is_object() && params.contains("api") && params["api"].is_string()) {
            const std::string api = params["api"];
            if (shows::valid_base(api)) base = api;
            else WNK_LOGW(kCat, "ignoring launch param api: not an http(s) base URL without a trailing slash");
        }
        service_ = std::make_unique<Service>(ctx, base);
        WNK_LOGI(kCat, "catalogue %s, http %s, store %s", base.c_str(),
                 service_->online() ? service_->transport().c_str() : "unavailable", service_->store_name());

        Ui &ui = ctx.ui();
        nav_ = &ui.root().add<Navigator>();
        nav_->on_change = [this](Ui &u, Screen &s) {
            View *f = u.focused();
            if (nav_->depth() < depth_)
                WNK_LOGI(kCat, "back to %s focus=%s", s.title().c_str(), f != nullptr ? f->name.c_str() : "<pending>");
            depth_ = nav_->depth();
        };
        home_ = &static_cast<HomeScreen &>(nav_->push(ui, std::make_unique<HomeScreen>(*service_, *nav_)));
        home_->start(ui);
    }

    void on_lifecycle(wnk::app::Context &, const wnk::app::Lifecycle::Transition &t) override {
        if (t.from == wnk::app::State::Resuming && t.to == wnk::app::State::Running && home_ != nullptr)
            home_->on_resumed();
    }

    void on_stop(wnk::app::Context &) override {
        if (home_ != nullptr) home_->stop();
    }

 private:
    std::unique_ptr<Service> service_;
    Navigator *nav_ = nullptr;
    HomeScreen *home_ = nullptr;
    size_t depth_ = 0;
};

}  // namespace

int main(int argc, char **argv) {
    wnk::app::AppConfig config;
    config.fallback_app_id = kFallbackAppId;
    config.title = "wnk shows";
    config.script_env_prefix = "WNK_SHOWS";
    config.http = true;
    config.storage = true;
    // Poster downloads and API requests share the pool and mostly wait on the network. With the
    // default 2 workers a 0.36 s details request answered 0.98 s after OK, queued behind a
    // screenful of posters; with 4 it answered after 0.32 s (host, TVmaze, 2026-10-12).
    config.worker_threads = 4;
    config.metrics_toggle = wnk::input::Action::Blue;
    return wnk::app::run(argc, argv, config, [] { return std::make_unique<ShowsApp>(); });
}
