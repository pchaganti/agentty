#pragma once
// agentty::app::subscribe — input → Msg routing.
//
// Pure function of Model: snapshots which modal (if any) owns the keyboard,
// then routes keys / paste / tick into the right Msg.

#include <chrono>

#include <maya/maya.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::app {

// The streaming Tick cadence — the SINGLE source of truth for how
// often the loop wakes while a turn is active.
//
// This is consumed in TWO places that MUST agree, or animation either
// wastes renders or freezes-until-keypress:
//   1. subscribe() — the `Sub::every(period, Tick{})` interval.
//   2. Program::visual_hash() — the fine-animation time bucket, which
//      must be PHASE-LOCKED to this period so the render gate advances
//      exactly once per loop wake (see program.hpp).
// Deriving both from one function makes the phase-lock structural
// rather than a hand-maintained invariant across two files.
//
// Cadence (33 ms ≈ 30 fps on DEC-2026 sync terminals for a smooth
// spinner; 100 ms ≈ 10 fps elsewhere to cut progressive-paint
// flicker; clamped to ≥ 80 ms over SSH where the wire, not local
// paint, is the bottleneck). Computed once — the inputs (terminal
// sync support, SSH env) are immutable for the session.
[[nodiscard]] std::chrono::milliseconds streaming_tick_period() noexcept;

[[nodiscard]] maya::Sub<Msg> subscribe(const Model& m);

} // namespace agentty::app
