// agentty::provider — active-provider selection (process-global).

#include "agentty/provider/selection.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

#include "agentty/provider/registry.hpp"

namespace agentty::provider {

namespace {
Selection g_active{};

std::string env_or_empty(std::string_view name) {
    if (name.empty()) return {};
    const char* v = std::getenv(std::string{name}.c_str());
    return (v && *v) ? std::string{v} : std::string{};
}
} // namespace

Selection parse_selection(std::string_view spec) {
    Selection s;
    // Registry-driven: an empty spec or any preset whose kind is Anthropic
    // routes to the Anthropic transport. Everything else (OpenAI-family
    // presets AND raw "host[:port]" custom endpoints) goes through the
    // OpenAI-compatible transport with the matching Endpoint.
    const ProviderPreset* p = spec.empty() ? preset_for(default_provider_id())
                                            : preset_for(spec);
    if (p && p->kind == Kind::Anthropic) {
        s.kind = Kind::Anthropic;
        return s;
    }
    s.kind = Kind::OpenAI;
    s.openai_endpoint = openai::Endpoint::from_spec(
        spec.empty() ? default_provider_id() : spec);
    return s;
}

auth::AuthHeader resolve_auth_for(std::string_view spec,
                                  const auth::AuthHeader& anthropic_creds,
                                  std::string_view cli_key) {
    const ProviderPreset* p = spec.empty() ? preset_for(default_provider_id())
                                            : preset_for(spec);

    // Anthropic (or an unknown spec that parsed to Anthropic): use the creds
    // resolved from `agentty login`.
    if ((p && p->kind == Kind::Anthropic)
        || (!p && parse_selection(spec).kind == Kind::Anthropic)) {
        return anthropic_creds;
    }

    // Local backends need no key.
    if (p && p->auth == AuthStyle::None)
        return auth::AuthHeader{auth::ApiKeyHeader{std::string{}}};

    // OpenAI-family (preset or custom host): bearer key from --key, then the
    // preset's env-var chain. Custom hosts (no preset) fall back to
    // OPENAI_API_KEY, the de-facto default for OpenAI-compatible servers.
    std::string key{cli_key};
    if (key.empty() && p) {
        for (auto env : p->auth_env) {
            key = env_or_empty(env);
            if (!key.empty()) break;
        }
    }
    if (key.empty())
        key = env_or_empty("OPENAI_API_KEY");
    return auth::AuthHeader{auth::ApiKeyHeader{std::move(key)}};
}

void select(Selection s) { g_active = std::move(s); }

const Selection& active() { return g_active; }

} // namespace agentty::provider
