// SPDX-License-Identifier: GPL-3.0-or-later
//
// On-screen metrics overlay: what the perf log says, live, on the TV. Drawn by the app shell after
// the UI, through the Renderer interface only. Drawing it costs a few hundred quads and two draw
// calls' worth of text, which shows up in the numbers it displays; keep that in mind when comparing
// against the perf log of a run without it.
#pragma once

#include "wnk/async/async.hpp"
#include "wnk/diagnostics/metrics.hpp"
#include "wnk/image/image.hpp"
#include "wnk/render/renderer.hpp"
#include "wnk/ui/ui.hpp"

#include <optional>
#include <string>

namespace wnk::app {

struct OverlayData {
    const diagnostics::FrameMetrics *frames = nullptr;
    render::RenderStats render;
    ui::UiStats ui;
    std::optional<image::ImageStats> images;
    std::optional<async::PoolStats> pool;
    diagnostics::ProcessMemory memory;
    std::string state;  // lifecycle state name
};

// Draws the panel in the top-right corner of the logical canvas.
void draw_metrics_overlay(render::Renderer &r, const ui::Theme &theme, Size canvas, const OverlayData &d);

}  // namespace wnk::app
