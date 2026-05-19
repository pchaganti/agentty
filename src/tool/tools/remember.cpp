// agentty::tools::tool_remember — persist a fact the user wants the
// agent to recall on future turns. Writes one JSONL record to either
// ~/.agentty/memory.jsonl (scope=user, cross-project) or
// <workspace>/.agentty/memory.jsonl (scope=project). The system-prompt
// builder reloads the tail of these files into <learned-memory> on
// every turn, so the model gets to see what it stored.
//
// Why this exists as a tool, not a CLAUDE.md edit: CLAUDE.md is
// human-curated and the model should respect it as authoritative. The
// JSONL memory store is the model's own scratchpad — additive,
// removable, bounded in size — for facts the *user* asked it to
// remember mid-conversation. Keeping the two stores separate avoids
// the model accidentally mutating files the human owns.

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

struct RememberArgs {
    std::string text;
    memory::Scope scope;
    bool pinned = false;
    std::vector<std::string> tags;
    std::string supersedes_id;
};

std::expected<RememberArgs, ToolError> parse_remember_args(const json& j) {
    util::ArgReader ar(j);
    auto text = ar.require_str("text");
    if (!text) {
        return std::unexpected(ToolError::invalid_args(
            "remember: `text` is required (the fact to remember, one short sentence)"));
    }
    RememberArgs out;
    out.text = std::move(*text);
    // `scope` is optional and defaults to project — the safer default;
    // global cross-project facts should be opt-in. Tolerate the common
    // synonym "global" for user-scope since some models prefer it.
    auto raw = ar.str("scope", "project");
    if (raw == "global" || raw == "all") raw = "user";
    auto parsed = memory::parse_scope(raw);
    if (!parsed) {
        return std::unexpected(ToolError::invalid_args(
            "remember: `scope` must be \"user\" or \"project\" (got: " + raw + ")"));
    }
    out.scope = *parsed;
    out.pinned = ar.boolean("pin", false);
    out.supersedes_id = ar.str("supersedes", "");
    // Tags are optional. Accept either a JSON array of strings or a
    // single comma-separated string — models that don't know the
    // schema sometimes pass "build,picker" instead of ["build","picker"].
    if (j.contains("tags")) {
        const auto& t = j["tags"];
        if (t.is_array()) {
            for (const auto& x : t) if (x.is_string()) out.tags.push_back(x.get<std::string>());
        } else if (t.is_string()) {
            std::string s = t.get<std::string>();
            std::size_t i = 0;
            while (i < s.size()) {
                std::size_t comma = s.find(',', i);
                std::string seg = s.substr(i, comma == std::string::npos ? s.size() - i : comma - i);
                if (!seg.empty()) out.tags.push_back(std::move(seg));
                if (comma == std::string::npos) break;
                i = comma + 1;
            }
        }
    }
    return out;
}

ExecResult run_remember(const RememberArgs& a) {
    memory::AppendOptions opts;
    opts.pinned        = a.pinned;
    opts.tags          = a.tags;
    opts.supersedes_id = a.supersedes_id;
    auto res = memory::append(a.scope, a.text, opts);
    if (!res.error.empty()) return std::unexpected(ToolError::io(res.error));
    std::string verb = res.deduped ? "Deduped" : "Remembered";
    std::string msg = std::format(
        "{} ({} scope, id={}): {}",
        verb, memory::to_string(a.scope), res.id, a.text);
    if (a.pinned)            msg += "  [pinned]";
    if (!a.tags.empty()) {
        msg += "  [tags:";
        for (std::size_t i = 0; i < a.tags.size(); ++i) {
            msg += (i ? "," : " ");
            msg += a.tags[i];
        }
        msg += "]";
    }
    if (!res.note.empty())   msg += "  [" + res.note + "]";
    if (res.rolled > 0) {
        msg += std::format("  [rolled {} oldest record(s) to fit cap]", res.rolled);
    }
    return ToolOutput{std::move(msg), std::nullopt};
}

} // namespace

ToolDef tool_remember() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"remember">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Persist a fact for future turns. Call this whenever the user asks "
        "you to remember something (\"remember X\", \"don't forget Y\", "
        "\"from now on Z\", \"always do W\"). The fact is written to a "
        "JSONL store and reloaded into your system prompt on every "
        "subsequent turn under <learned-memory>.\n\n"
        "scope=\"project\" (default) — stored at <workspace>/.agentty/"
        "memory.jsonl, only visible when running in this codebase.\n"
        "scope=\"user\"               — stored at ~/.agentty/memory.jsonl, "
        "visible across every project (use for facts about the user "
        "themselves, not the project).\n\n"
        "Dedup: if the fact is near-identical to an existing record in "
        "the same scope, no new line is written — the existing record's "
        "timestamp + hit count refresh instead, and tags / pin propagate. "
        "Just call `remember` with the fact; the store handles repetition.\n\n"
        "`pin=true` marks the record as cap-exempt — critical facts (build "
        "command, hard project conventions) stay put even after hundreds "
        "of subsequent appends. Pinned facts render with ★ in <learned-memory>.\n\n"
        "`tags` (array of strings, optional) groups facts by topic in the "
        "system prompt. Lower-cased + deduped automatically.\n\n"
        "`supersedes` (a record id, optional) atomically replaces an older "
        "fact — the named record is removed in the same write. Use when "
        "the user corrects a previous instruction.\n\n"
        "Keep each fact short and self-contained. If a previously-stored "
        "fact becomes wrong, use `forget` to remove it (or `supersedes` to "
        "replace it in one step).";
    t.input_schema = json{
        {"type","object"},
        {"required", {"text"}},
        {"properties", {
            {"text",  {{"type","string"},
                {"description","The fact to remember. One short sentence; "
                               "future-you reads this back without any of the "
                               "surrounding conversation."}}},
            {"scope", {{"type","string"},
                {"enum", {"user","project"}},
                {"description","\"project\" (default) is workspace-scoped; "
                               "\"user\" is cross-project (for user "
                               "preferences, identity, etc)."}}},
            {"pin",   {{"type","boolean"},
                {"description","Mark the record as pinned. Pinned records are "
                               "exempt from the FIFO cap — they stick around "
                               "across long sessions. Use sparingly, only for "
                               "facts the user has explicitly emphasised or "
                               "that are load-bearing for every turn."}}},
            {"tags",  {{"type","array"}, {"items", {{"type","string"}}},
                {"description","Optional grouping labels (e.g. [\"build\",\"picker\"]). "
                               "Lowercased + deduped automatically. Used by the "
                               "system-prompt loader to group related facts."}}},
            {"supersedes", {{"type","string"},
                {"description","An 8-char record id whose entry should be "
                               "removed atomically when this new one is written. "
                               "Use to correct an older fact in one operation."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<RememberArgs>(parse_remember_args, run_remember);
    return t;
}

} // namespace agentty::tools
