// SPDX-License-Identifier: GPL-3.0-or-later
//
// Example B: DLNA media servers on the home network (wnk/media/dlna.hpp). The first screen lists the
// servers SSDP finds (or the one a description URL names); OK on a server or a folder opens it,
// OK on a video or a song starts Example D on its URL, OK on a picture shows it here.
#pragma once

#include "wnk/app/app.hpp"
#include "wnk/log.hpp"
#include "wnk/media/dlna.hpp"
#include "wnk/ui/collection.hpp"
#include "wnk/ui/navigation.hpp"
#include "wnk/ui/widgets.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace tvui_media {

using namespace wnk::ui;
using wnk::media::DlnaKind;
using wnk::media::DlnaObject;
using wnk::media::MediaServer;

constexpr const char *kMediaCat = "media";
constexpr const char *kPlayerAppId = "org.wnk.examples.player";

inline std::string clock_text(int64_t us) {
    if (us == wnk::media::kNoTime || us < 0) return {};
    const int64_t s = us / 1000000;
    char b[32];
    if (s >= 3600) {
        std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", static_cast<long long>(s / 3600), static_cast<long long>(s / 60 % 60),
                      static_cast<long long>(s % 60));
    } else {
        std::snprintf(b, sizeof b, "%lld:%02lld", static_cast<long long>(s / 60), static_cast<long long>(s % 60));
    }
    return b;
}

inline std::string row_text(const DlnaObject &o) {
    std::string t;
    switch (o.kind) {
        case DlnaKind::Container: t = "Folder  ·  "; break;
        case DlnaKind::Video: t = "Video  ·  "; break;
        case DlnaKind::Audio: t = "Music  ·  "; break;
        case DlnaKind::Image: t = "Photo  ·  "; break;
        case DlnaKind::Other: t = "Other  ·  "; break;
    }
    t += o.title.empty() ? o.id : o.title;
    if (o.kind == DlnaKind::Container && o.child_count >= 0) t += "  (" + std::to_string(o.child_count) + ")";
    if (const auto *r = o.playable()) {
        const std::string d = clock_text(r->duration_us);
        if (!d.empty()) t += "  ·  " + d;
        if (r->width > 0 && o.kind == DlnaKind::Video) t += "  ·  " + std::to_string(r->width) + "x" + std::to_string(r->height);
        if (o.kind == DlnaKind::Audio && !o.artist.empty()) t += "  ·  " + o.artist;
    }
    return t;
}

// A picture from a server, fitted to the screen.
class PictureScreen : public Screen {
 public:
    PictureScreen(const std::string &title, const std::string &url) : Screen(title) {
        name = "media.picture";
        set_padding({80, 60, 80, 60});
        auto &col = add<Column>(24);
        col.set_height(Dim::fill());
        col.add<Label>(title, TextRole::Title);
        auto &img = col.add<ImageView>(url);
        img.set_fit(ImageFit::Contain);
        img.set_width(Dim::fill());
        img.set_height(Dim::fill());
        img.set_focusable(true);  // so the screen has somewhere for focus (Back pops it)
    }
};

// The server list (no server) or one folder of a server.
class MediaListScreen : public Screen {
 public:
    // Servers on the network, or the one described at `description_url` when it is not empty.
    MediaListScreen(wnk::app::Context &ctx, Navigator &nav, std::string description_url)
        : Screen("Media servers"), ctx_(ctx), nav_(nav), description_url_(std::move(description_url)) {
        build();
        load_servers();
    }
    MediaListScreen(wnk::app::Context &ctx, Navigator &nav, std::shared_ptr<const MediaServer> server, std::string object_id,
                    std::string title)
        : Screen(std::move(title)), ctx_(ctx), nav_(nav), server_(std::move(server)), object_id_(std::move(object_id)) {
        build();
        load_folder();
    }
    ~MediaListScreen() override { cancel_.cancel(); }

 private:
    struct Row {
        std::string text;
        std::shared_ptr<const MediaServer> server;  // a server row (server list)
        DlnaObject object;                          // a folder row
    };

    void build() {
        name = "media.list";
        set_padding({100, 70, 100, 40});
        auto &col = add<Column>(20);
        col.set_height(Dim::fill());
        col.add<Label>(server_ ? server_->name + "  /  " + title() : title(), TextRole::Title);
        status_ = &col.add<Label>(server_ ? "Loading…" : "Looking for media servers…");
        status_->set_muted(true);
        list_ = &col.add<CollectionView>(Axis::Vertical, 1, wnk::Size{1600, 72}, 12);
        list_->set_width(Dim::fill());
        list_->set_height(Dim::fill());
        list_->name = "media.rows";
        list_->on_activate = [this](Ui &ui, size_t i) { open(ui, i); };
    }

    void show(std::vector<Row> rows, const std::string &empty_text) {
        rows_ = std::move(rows);
        status_->set_text(rows_.empty() ? empty_text : std::to_string(rows_.size()) + (rows_.size() == 1 ? " entry" : " entries"));
        list_->set_items(rows_.size(), [this](Ui &, size_t i) {
            auto b = std::make_unique<Button>(rows_[i].text);  // no handler: OK goes to on_activate
            b->name = "media.row" + std::to_string(i);
            return b;
        });
        if (!rows_.empty()) list_->focus_index(ctx_.ui(), 0);
    }

    void load_servers() {
        auto http = ctx_.http();
        if (!http) {
            status_->set_text("No HTTP client on this device.");
            return;
        }
        struct Found {
            std::vector<MediaServer> servers;
            std::string error;
        };
        wnk::async::run_then_post<Found>(
            ctx_.pool(), ctx_.dispatcher(), life_.weak(), cancel_.token(),
            [http, url = description_url_](const wnk::async::CancellationToken &t) {
                Found f;
                if (url.empty()) {
                    f.servers = wnk::media::find_media_servers(*http, std::chrono::milliseconds(2000), t, &f.error);
                    return f;
                }
                wnk::net::UpnpDevice d;
                MediaServer s;
                if (!wnk::net::fetch_upnp_description(*http, url, &d, t, &f.error)) return f;
                if (wnk::media::media_server_from_device(d, url, &s)) {
                    f.servers.push_back(std::move(s));
                } else {
                    f.error = "not a media server";
                }
                return f;
            },
            [this](Found f) {
                WNK_LOGI(kMediaCat, "servers: %zu%s%s", f.servers.size(), f.error.empty() ? "" : " (", f.error.empty() ? "" : (f.error + ")").c_str());
                std::vector<Row> rows;
                for (auto &s : f.servers) {
                    WNK_LOGI(kMediaCat, "server %s at %s", s.name.c_str(), s.location.c_str());
                    Row r;
                    r.text = s.name;
                    r.server = std::make_shared<const MediaServer>(std::move(s));
                    rows.push_back(std::move(r));
                }
                show(std::move(rows), f.error.empty() ? "No media server answered." : "Could not reach it: " + f.error);
            });
    }

    void load_folder() {
        auto http = ctx_.http();
        if (!http) {
            status_->set_text("No HTTP client on this device.");
            return;
        }
        struct Listing {
            std::vector<DlnaObject> objects;
            std::string error;
            bool ok = false;
        };
        wnk::async::run_then_post<Listing>(
            ctx_.pool(), ctx_.dispatcher(), life_.weak(), cancel_.token(),
            [http, server = server_, id = object_id_](const wnk::async::CancellationToken &t) {
                Listing l;
                wnk::media::ContentDirectory cd(*http, *server);
                l.ok = cd.browse_all(id, &l.objects, t, &l.error, 200, 2000);
                return l;
            },
            [this](Listing l) {
                WNK_LOGI(kMediaCat, "%s: %zu objects%s%s", title().c_str(), l.objects.size(), l.ok ? "" : ": ", l.ok ? "" : l.error.c_str());
                std::vector<Row> rows;
                for (auto &o : l.objects) {
                    Row r;
                    r.text = row_text(o);
                    r.object = std::move(o);
                    rows.push_back(std::move(r));
                }
                show(std::move(rows), l.ok ? "This folder is empty." : "Could not browse: " + l.error);
                if (!l.ok && !rows_.empty()) status_->set_text(std::to_string(rows_.size()) + " entries (then: " + l.error + ")");
            });
    }

    void open(Ui &ui, size_t i) {
        if (i >= rows_.size()) return;
        const Row &r = rows_[i];
        if (r.server) {
            nav_.push(ui, std::make_unique<MediaListScreen>(ctx_, nav_, r.server, "0", "Home"));
            return;
        }
        const DlnaObject &o = r.object;
        if (o.kind == DlnaKind::Container) {
            nav_.push(ui, std::make_unique<MediaListScreen>(ctx_, nav_, server_, o.id, o.title));
            return;
        }
        const auto *res = o.playable();
        if (res == nullptr) {
            ui.toast("Nothing here this app can open");
            return;
        }
        if (o.kind == DlnaKind::Image) {
            WNK_LOGI(kMediaCat, "show image %s", res->url.c_str());
            nav_.push(ui, std::make_unique<PictureScreen>(o.title, res->url));
            return;
        }
        WNK_LOGI(kMediaCat, "play %s (%s) in %s", res->url.c_str(), res->mime_type.c_str(), kPlayerAppId);
        ctx_.launch_app(kPlayerAppId, nlohmann::json{{"uri", res->url}}.dump(), [this, alive = life_.weak()](bool ok) {
            if (!ok && alive.alive()) ctx_.ui().toast("Could not start the player (Example D installed?)");
        });
    }

    wnk::app::Context &ctx_;
    Navigator &nav_;
    std::string description_url_;
    std::shared_ptr<const MediaServer> server_;
    std::string object_id_;
    Label *status_ = nullptr;
    CollectionView *list_ = nullptr;
    std::vector<Row> rows_;
    wnk::async::CancellationSource cancel_;
    wnk::async::Lifetime life_;
};

}  // namespace tvui_media
