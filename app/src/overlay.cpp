// SPDX-License-Identifier: GPL-3.0-or-later
#include "wnk/app/overlay.hpp"

#include <algorithm>
#include <cstdio>

namespace wnk::app {
namespace {

constexpr float kWidth = 600, kPad = 20, kGraphH = 120, kGraphMaxMs = 50;
const render::Color kPanel = render::Color::hex(0x000000E8);
const render::Color kGood = render::Color::hex(0x5FD38DFF);
const render::Color kLate = render::Color::hex(0xFFC145FF);
const render::Color kDropped = render::Color::hex(0xFF5C5CFF);
const render::Color kGuide = render::Color::hex(0xFFFFFF40);

}  // namespace

void draw_metrics_overlay(render::Renderer &r, const ui::Theme &theme, Size canvas, const OverlayData &d) {
    const render::TextStyle text{theme.regular, 22, theme.text};
    const render::TextStyle head{theme.bold, 24, theme.text};
    const float lh = r.line_height(text);
    const int text_lines = 5 + (d.images ? 1 : 0) + (d.pool ? 1 : 0);
    const Rect panel{canvas.w - kWidth - 24, 24, kWidth, kPad * 2 + lh * text_lines + kGraphH + 16};
    r.fill_rect(panel, kPanel, 12);

    float y = panel.y + kPad;
    auto line = [&](const render::TextStyle &s, const char *str) {
        r.draw_text(s, {panel.x + kPad, y + r.ascent(s)}, str);
        y += lh;
    };
    char buf[200];
    const diagnostics::FrameSummary f = d.frames != nullptr ? d.frames->summary() : diagnostics::FrameSummary{};
    std::snprintf(buf, sizeof buf, "%.1f fps  vsync %.1f  p95 %.1f  max %.1f ms  late %zu/%zu", f.fps(),
                  f.interval_median, f.interval_p95, f.interval_max, f.late, f.frames);
    line(head, buf);

    // Frame interval graph, oldest on the left; guides at one and two 60 Hz frames.
    const Rect g{panel.x + kPad, y + 4, kWidth - 2 * kPad, kGraphH};
    y += kGraphH + 16;
    auto y_for = [&g](float ms) { return g.bottom() - std::min(ms, kGraphMaxMs) / kGraphMaxMs * g.h; };
    r.fill_rect(g, render::Color::hex(0xFFFFFF10));
    if (d.frames != nullptr && d.frames->size() > 0) {
        const size_t n = d.frames->size();
        const float bw = g.w / static_cast<float>(diagnostics::FrameMetrics::kCapacity);
        const float x0 = g.right() - bw * static_cast<float>(n);
        const float period = f.interval_median > 0 ? f.interval_median : 1000.0f / 60;
        for (size_t i = 0; i < n; ++i) {
            const float ms = d.frames->at(i).interval;
            const render::Color c = ms > period * 2.5f ? kDropped : ms > f.late_threshold ? kLate : kGood;
            const float top = y_for(ms);
            r.fill_rect({x0 + bw * static_cast<float>(i), top, std::max(1.0f, bw - 0.5f), g.bottom() - top}, c);
        }
    }
    // Guides at one and two measured vsync periods.
    const float period = f.interval_median > 0 ? f.interval_median : 1000.0f / 60;
    for (float guide : {period, period * 2}) r.fill_rect({g.x, y_for(guide), g.w, 1.5f}, kGuide);

    std::snprintf(buf, sizeof buf, "max ms  events %.1f  update %.1f  draw %.1f  finish %.1f  swap %.1f", f.events_max,
                  f.update_max, f.draw_max, f.finish_max, f.swap_max);
    line(text, buf);
    std::snprintf(buf, sizeof buf, "draw calls %u   quads %u   textures %u   glyphs %u", d.render.draw_calls, d.render.quads,
                  d.render.textures, d.render.glyphs_cached);
    line(text, buf);
    std::snprintf(buf, sizeof buf, "views %zu   focus nodes %zu   layouts %llu", d.ui.views, d.ui.focus_nodes,
                  static_cast<unsigned long long>(d.ui.layouts));
    line(text, buf);
    if (d.images) {
        const auto &is = *d.images;
        std::snprintf(buf, sizeof buf, "images %.1f MB (%zu)  hits mem %llu disk %llu  downloads %llu  in flight %zu",
                      is.memory_bytes / 1048576.0, is.memory_entries, static_cast<unsigned long long>(is.memory_hits),
                      static_cast<unsigned long long>(is.disk_hits), static_cast<unsigned long long>(is.downloads),
                      is.in_flight);
        line(text, buf);
    }
    if (d.pool) {
        const auto &p = *d.pool;
        std::snprintf(buf, sizeof buf, "workers %zu   queued %zu (max %zu)   running %zu   done %llu", p.threads, p.queued,
                      p.max_queued, p.running, static_cast<unsigned long long>(p.completed));
        line(text, buf);
    }
    std::snprintf(buf, sizeof buf, "rss %.1f MB   state %s   frames %llu", d.memory.rss_bytes / 1048576.0, d.state.c_str(),
                  static_cast<unsigned long long>(d.frames != nullptr ? d.frames->total() : 0));
    line(text, buf);
}

}  // namespace wnk::app
