// stream_liveness_test — UX-critical regression: the live-edge caret
// (maya reveal_fx) must NEVER look frozen while a model response is in
// flight. The user-visible failure this locks out: a slow model pauses
// between deltas (median ~470 ms, worst seconds) and the typewriter
// caret stops pulsing mid-sentence — reading as "the stream died".
//
// Root cause history (commits 498ec9e → a9760e7): the RAF re-arm gate in
// turn.cpp's cached_markdown_for was a fixed byte-recency timeout
// (250 ms, then 3 s). ANY fixed timeout can be out-run by a slower
// model or laggy link. The robust gate keys off m.s.is_streaming() —
// the variant-backed phase::Streaming signal — so the caret stays armed
// unconditionally while the wire is open, and drops only when the phase
// leaves Streaming.
//
// Contract asserted here, via maya::detail::animation_requested_ (the
// thread-local that request_animation_frame() sets and the run loop
// reads to schedule the next ~16 ms repaint):
//
//   1. While phase == Streaming and the message has live bytes, EVERY
//      view build re-arms the animation frame — even after a simulated
//      10-second gap with zero new bytes (out-runs any timeout anyone
//      might reintroduce).
//   2. After the stream settles (phase → Idle, streaming_text drained
//      into text), the widget finishes its ~200 ms finalize ramp and
//      then STOPS re-arming — the idle loop must return to zero wakes
//      (the other half of the contract: no frozen caret, but also no
//      60 fps burn at idle).
//
// If this test fails on (1), someone re-introduced a timeout race —
// the "stream looks dead" bug is back. If it fails on (2), the caret
// never disarms and idle CPU burns.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <maya/app/app.hpp>            // maya::detail::animation_requested_
#include <maya/render/canvas.hpp>
#include <maya/render/renderer.hpp>
#include <maya/style/theme.hpp>
#include <maya/widget/thread.hpp>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "  FAIL [%s:%d] %s\n",                    \
                         __FILE__, __LINE__, (msg));                       \
        }                                                                  \
    } while (0)

// Render the CONVERSATION region only (frozen + live tail) and report
// whether any widget in it requested an animation frame during build.
// Mirrors the run loop: clear the flag, build + paint, read the flag.
//
// Deliberately NOT the full AppLayout: the composer's idle cursor blink
// (maya composer.hpp) re-arms the animation frame on every idle build,
// which would mask the markdown caret's disarm in test (2). The caret
// under test lives in the thread region; probe exactly that.
static bool frame_requests_animation(const Model& m,
                                     int width = 100, int height = 40) {
    maya::detail::animation_requested_ = false;

    auto root = maya::Thread{agentty::ui::thread_config(m)}.build();

    maya::StylePool pool;
    maya::Canvas canvas(width, height, &pool);
    canvas.clear();
    maya::render_tree(root, canvas, pool, maya::theme::dark, true);

    return maya::detail::animation_requested_;
}

// ── (1) The caret must stay armed across an arbitrarily long inter-
//        delta gap while the phase says Streaming. ──────────────────────
static void test_caret_armed_across_delta_gap() {
    std::printf("test_caret_armed_across_delta_gap\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"liveness"};
    Message u; u.role = Role::User; u.text = "explain something";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    // In-flight assistant message: first delta arrived, wire still open.
    // Body kept SHORT so the reveal cursor catches up to the live edge
    // quickly — the gap probe below must isolate the RAF gate, and
    // reveal_in_progress() (cursor still gliding backlog) arms the frame
    // independently of it, which would mask a broken gate.
    Message a; a.role = Role::Assistant;
    a.streaming_text = "Hi";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};

    // Frame right after the delta: must be armed (bytes just grew).
    CHECK(frame_requests_animation(m),
          "caret armed on the frame after a delta lands");

    // Let the typewriter reach the live edge (2 cp at 30 cps ≈ 70 ms;
    // generous deadline). Once reveal_in_progress() is false, the ONLY
    // legitimate arming source left while the wire is open is the
    // phase gate under test.
    auto& cache = m.ui.view_cache.message_md(
        m.d.current.id, m.d.current.messages.back().id);
    const auto catch_up_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (cache.streaming && cache.streaming->reveal_in_progress()
           && std::chrono::steady_clock::now() < catch_up_deadline) {
        (void)frame_requests_animation(m);   // build() advances the cursor
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
    CHECK(cache.streaming && !cache.streaming->reveal_in_progress(),
          "reveal cursor reached the live edge (test precondition)");

    // Simulate the killer gap: NO new bytes, wire still open. TWO
    // clocks must lapse for the probe to be honest:
    //   • the widget's internal recency window (maya reveal_fx,
    //     age_at_tail_ms ≤ 250 ms keeps it self-arming every build) —
    //     defeated by REAL elapsed time (sleep > 250 ms);
    //   • agentty's cache recency window (kRevealActiveMs = 3 s) —
    //     too long to sleep out in a test; backdate the stamp instead,
    //     exactly what a slow model's gap does to it.
    // Past both windows, maya's quiescent regime arms only once per
    // 33/100 ms phase bucket — NOT every frame — so consecutive
    // same-bucket builds return false unless agentty's phase gate
    // (wire_streaming_here) holds the caret armed. That's the gate
    // under test.
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    cache.last_grow_tick =
        std::chrono::steady_clock::now() - std::chrono::seconds{10};

    // Six back-to-back frames inside the gap — EVERY one must re-arm.
    // A timeout-only gate leaves same-phase-bucket frames unarmed and
    // fails here (verified by sabotaging the gate to timeout-only).
    for (int f = 0; f < 6; ++f) {
        CHECK(frame_requests_animation(m),
              "caret STILL armed mid-gap (real 400 ms + simulated 10 s, "
              "zero new bytes, phase=Streaming) — a timeout race fails here");
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    // Sanity: the gate is the phase, not the backdated clock. Flip the
    // phase to Idle while bytes remain unsettled (cancel-like edge) —
    // the reveal cursor may still glide its backlog out, so arming may
    // persist transiently, but the UNCONDITIONAL guarantee is gone.
    // (No CHECK here: transient glide is legitimate. The hard assert
    // for disarm is test_caret_disarms_after_settle.)
}

// ── (2) After settle, the caret must disarm once the finalize ramp
//        completes — idle must not burn frames forever. ────────────────
static void test_caret_disarms_after_settle() {
    std::printf("test_caret_disarms_after_settle\n");

    Model m;
    m.d.current.id = agentty::ThreadId{"liveness2"};
    Message u; u.role = Role::User; u.text = "explain something";
    m.d.current.messages.push_back(std::move(u));
    agentty::app::detail::clear_frozen(m);
    agentty::app::detail::freeze_through(m, 1);

    Message a; a.role = Role::Assistant;
    a.streaming_text = "Short reply body.";
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = agentty::phase::Streaming{agentty::phase::Active{}};
    CHECK(frame_requests_animation(m), "armed while streaming");

    // Stream finishes: reducer moves streaming_text → text, phase → Idle.
    {
        Message& back = m.d.current.messages.back();
        back.text = std::move(back.streaming_text);
        back.streaming_text.clear();
        back.pending_stream.clear();
    }
    m.s.phase = agentty::phase::Idle{};

    // The widget runs a ~200 ms finalize ramp (request_finalize(200))
    // gliding the reveal cursor to the live edge; frames during the
    // ramp legitimately re-arm. Poll until it disarms, with a hard
    // deadline far beyond the ramp — if it never disarms, the caret
    // (and 60 fps wakes) would run forever at idle.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{3};
    bool disarmed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!frame_requests_animation(m)) { disarmed = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    }
    CHECK(disarmed,
          "caret disarms after settle + finalize ramp (idle must not "
          "burn animation frames)");

    // And stays disarmed: settled fast-path must not re-arm.
    if (disarmed) {
        for (int f = 0; f < 3; ++f) {
            CHECK(!frame_requests_animation(m),
                  "caret stays disarmed on settled frames");
        }
    }
}

int main() {
    std::printf("stream_liveness_test — the caret must never look "
                "frozen while a response is in flight\n\n");

    test_caret_armed_across_delta_gap();
    test_caret_disarms_after_settle();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
