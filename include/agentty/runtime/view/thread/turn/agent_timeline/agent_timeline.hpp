#pragma once
#include <span>
#include <maya/widget/agent_timeline.hpp>
#include "agentty/runtime/model.hpp"

namespace agentty::ui {

// Build the assistant turn's "Actions" panel config. Aggregates state
// (total/done/elapsed/category counts), picks per-category colors,
// computes title/footer, walks tool_calls into events.
//
// Takes the tool_calls by borrowed span so the tool-batch merge in
// turn_config / freeze_range / build_live_tail can synthesise a panel
// from multiple disjoint Messages' tool_calls without deep-copying a
// Message every frame.
[[nodiscard]] maya::AgentTimeline::Config agent_timeline_config(
    std::span<const ToolUse> tool_calls,
    int spinner_frame,
    maya::Color rail_color);

} // namespace agentty::ui
