#include "agentty/runtime/view/view.hpp"

#include <optional>

#include <maya/element/builder.hpp>
#include <maya/widget/app_layout.hpp>

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/view/changes_strip.hpp"
#include "agentty/runtime/view/composer.hpp"
#include "agentty/runtime/view/diff_review.hpp"
#include "agentty/runtime/view/login.hpp"
#include "agentty/runtime/view/pickers.hpp"
#include "agentty/runtime/view/status_bar/status_bar.hpp"
#include "agentty/runtime/view/thread/thread.hpp"

namespace agentty::ui {

namespace {

// Pick the active overlay, if any. Login modal has highest priority —
// auth gates everything else.
std::optional<maya::Element> pick_overlay(const Model& m) {
    if (login::is_open(m.ui.login))        return login_modal(m);
    if (pick::is_open(m.ui.model_picker))  return model_picker(m);
    if (pick::is_open(m.ui.thread_list))   return thread_list(m);
    if (is_open(m.ui.command_palette))     return command_palette(m);
    if (mention_is_open(m.ui.mention_palette)) return mention_palette(m);
    if (symbol_palette_is_open(m.ui.symbol_palette)) return symbol_palette(m);
    if (pick::is_open(m.ui.diff_review))   return diff_review(m);
    if (pick::is_open(m.ui.todo.open))     return todo_modal(m);
    return std::nullopt;
}

// Bottom-inset overlay layer. maya's Overlay widget bottom-pins the
// picker to the base BOX bottom and paints a full-width bg fill over
// its whole hugging rect. The base vstack's box is content_height + 2
// (the outer bottom-padding row + the idle anti-bounce blank()), so
// opening any picker painted 2 rows the closed frame never paints —
// frame grows +2 on open, shrinks -2 on close. When the welcome screen
// already sits at/over the terminal viewport, the +2 pushes the top
// rows into native scrollback (unreclaimable), and the close-shrink
// recovery (the bobbing wordmark fails the committed-prefix match)
// strands a wordmark slice EVERY open/close cycle — "the wordmark gets
// longer with every picker".
//
// Fix: pin the overlay 2 rows ABOVE the box bottom so its painted
// extent never exceeds the base's painted extent — opening a picker
// can never change the frame height, so no rows cross the viewport
// boundary and nothing strands. The inset must sit OUTSIDE the
// bg-filled box (padding inside it gets bg-painted, which is why
// maya::Overlay's slot can't express this), so we build the z-stack
// here: same layer shape as maya::Overlay::build, plus the bottom
// inset on the justify-End wrapper.
maya::Element overlay_layer(maya::Element el) {
    return maya::vstack()
        .align_items(maya::Align::Center)
        .justify(maya::Justify::End)
        .padding(0, 0, 2, 0)(
            maya::vstack()
                .width(maya::Dimension::percent(100))
                .padding(0, 2)
                .bg(maya::Color::default_color())(std::move(el)))
        .build();
}

} // namespace

maya::Element view(const Model& m) {
    auto base = maya::AppLayout{{
        .thread        = thread_config(m),
        .changes_strip = changes_strip_config(m),
        .composer      = composer_config(m),
        .status_bar    = status_bar_config(m),
    }}.build();
    auto overlay = pick_overlay(m);
    if (!overlay) return base;
    return maya::detail::zstack({
        std::move(base),
        overlay_layer(std::move(*overlay)),
    });
}

} // namespace agentty::ui
