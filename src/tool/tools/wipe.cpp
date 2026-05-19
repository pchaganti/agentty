// agentty::tools::tool_wipe_memory — the "start fresh" switch for the
// learned-memory store. Truncates every record in a scope file so the
// next turn's <learned-memory> block comes up empty.
//
// Why a separate tool, not a forget-without-pattern: `forget` requires
// either `id` or `substring` precisely so a stray empty call can't
// nuke the whole store. Wipe IS that nuke — gated behind an explicit
// `confirm=true` so the model can't trigger it from a vague user
// utterance. The two-step (dry-then-confirm) interaction surfaces a
// preview count before any data is lost.
//
// Scope handling matches `remember` / `forget`: explicit `scope`
// argument selects user vs project. There is no "wipe both" — the
// model has to call twice if the user really means "forget everything,
// everywhere". That's deliberate friction.

#include "agentty/tool/memory_store.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include <format>
#include <string>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

struct WipeArgs {
    memory::Scope scope;
    bool confirm = false;
};

std::expected<WipeArgs, ToolError> parse_wipe_args(const json& j) {
    util::ArgReader ar(j);
    WipeArgs out;
    auto raw = ar.str("scope", "");
    if (raw.empty()) {
        return std::unexpected(ToolError::invalid_args(
            "wipe_memory: `scope` is required (\"user\" or \"project\"). "
            "There is no default — wiping the wrong store is destructive."));
    }
    if (raw == "global" || raw == "all") raw = "user";
    auto parsed = memory::parse_scope(raw);
    if (!parsed) {
        return std::unexpected(ToolError::invalid_args(
            "wipe_memory: `scope` must be \"user\" or \"project\" (got: " + raw + ")"));
    }
    out.scope = *parsed;
    out.confirm = ar.boolean("confirm", false);
    return out;
}

ExecResult run_wipe(const WipeArgs& a) {
    // Two-phase contract:
    //   1. Without `confirm=true` we report how many records WOULD be
    //      wiped and bail. This is the preview / dry-run path; calling
    //      `wipe_memory(scope=...)` is always safe.
    //   2. With `confirm=true` we truncate the scope file.
    auto all = memory::load_all(a.scope);
    if (!a.confirm) {
        if (all.empty()) {
            return ToolOutput{
                std::format(
                    "{} scope is already empty. Nothing to wipe.",
                    memory::to_string(a.scope)),
                std::nullopt};
        }
        std::string msg = std::format(
            "Preview: {} record(s) in {} scope would be wiped. "
            "Re-run with `confirm=true` to commit.\n",
            all.size(), memory::to_string(a.scope));
        // Show the first few so the user (via the model's reply) can
        // sanity-check what's about to vanish. Cap at 10 to keep the
        // output compact; the full store is reachable via <learned-memory>.
        std::size_t shown = 0;
        for (const auto& r : all) {
            if (shown >= 10) break;
            msg += "  - [";
            msg += r.id;
            msg += "] ";
            msg += r.text;
            msg += '\n';
            ++shown;
        }
        if (all.size() > shown) {
            msg += std::format("  \xe2\x80\xa6 and {} more\n", all.size() - shown);
        }
        return ToolOutput{std::move(msg), std::nullopt};
    }

    auto n = memory::wipe(a.scope);
    if (!n) {
        return std::unexpected(ToolError::io(std::format(
            "wipe_memory: can't resolve a writable {} memory path "
            "(no $HOME or no writable workspace root).",
            memory::to_string(a.scope))));
    }
    return ToolOutput{
        std::format(
            "Wiped {} record(s) from {} scope. The {} memory file is now empty.",
            *n, memory::to_string(a.scope), memory::to_string(a.scope)),
        std::nullopt};
}

} // namespace

ToolDef tool_wipe_memory() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"wipe_memory">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Wipe every learned-memory record in a single scope. The "
        "\"start fresh on this codebase\" switch — useful when the "
        "stored facts no longer reflect the project (rewrite, "
        "refactor, switched away from a convention) and pruning them "
        "one by one would be tedious.\n\n"
        "Two-step contract:\n"
        "  1. Call once WITHOUT `confirm` to preview how many records "
        "(and a sample of which) would be removed.\n"
        "  2. Re-call with `confirm=true` to actually wipe.\n\n"
        "`scope` is REQUIRED — there is no default. Wipe the wrong "
        "store is destructive; the explicit argument is the gate.\n\n"
        "Use `forget` for surgical removal (single id, substring "
        "match). Use `wipe_memory` only when the user wants every "
        "fact in a scope gone.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"scope"}},
        {"properties", {
            {"scope",   {{"type","string"},
                {"enum", {"user","project"}},
                {"description","Which memory file to wipe. \"user\" "
                               "removes ~/.agentty/memory.jsonl (cross-"
                               "project facts); \"project\" removes "
                               "<workspace>/.agentty/memory.jsonl (this "
                               "codebase only)."}}},
            {"confirm", {{"type","boolean"},
                {"description","Required to actually wipe. Without it "
                               "the tool returns a preview of what "
                               "would be removed. Default: false."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<WipeArgs>(parse_wipe_args, run_wipe);
    return t;
}

} // namespace agentty::tools
