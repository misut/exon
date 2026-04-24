export module reporting;
import std;

export namespace reporting {

enum class OutputMode {
    human,
    json,
    raw,
    wrapped,
};

enum class CapabilitySetting {
    auto_detect,
    always,
    never,
};

enum class ShowOutput {
    failed,
    all,
    none,
};

enum class StreamMode {
    passthrough,
    tee,
    capture,
};

struct Options {
    OutputMode output = OutputMode::human;
    ShowOutput show_output = ShowOutput::failed;
    std::optional<CapabilitySetting> color;
    std::optional<CapabilitySetting> progress;
    std::optional<CapabilitySetting> unicode;
    std::optional<CapabilitySetting> hyperlinks;
};

struct ProcessResult {
    int exit_code = 0;
    bool timed_out = false;
    std::string stdout_text;
    std::string stderr_text;
    std::chrono::milliseconds elapsed{0};
};

struct ProgressSnapshot {
    int done = 0;
    int total = 0;
    int percent = 0;
    double rate = 0.0;
    std::chrono::milliseconds elapsed{0};
    std::chrono::milliseconds remaining{0};
    std::string_view label;
    std::string detail;
    std::vector<std::string> detail_lines;
};

struct ProgressSource {
    std::function<std::optional<ProgressSnapshot>()> poll;
};

std::optional<OutputMode> parse_output_mode(std::string_view value) {
    if (value == "human")
        return OutputMode::human;
    if (value == "json")
        return OutputMode::json;
    if (value == "raw")
        return OutputMode::raw;
    if (value == "wrapped")
        return OutputMode::wrapped;
    return std::nullopt;
}

std::optional<CapabilitySetting> parse_capability_setting(std::string_view value) {
    if (value == "auto")
        return CapabilitySetting::auto_detect;
    if (value == "always")
        return CapabilitySetting::always;
    if (value == "never")
        return CapabilitySetting::never;
    return std::nullopt;
}

std::optional<ShowOutput> parse_show_output(std::string_view value) {
    if (value == "failed")
        return ShowOutput::failed;
    if (value == "all")
        return ShowOutput::all;
    if (value == "none")
        return ShowOutput::none;
    return std::nullopt;
}

std::string_view to_string(OutputMode mode) {
    switch (mode) {
    case OutputMode::human:
        return "human";
    case OutputMode::json:
        return "json";
    case OutputMode::raw:
        return "raw";
    case OutputMode::wrapped:
        return "wrapped";
    }
    return "raw";
}

std::string_view to_string(CapabilitySetting setting) {
    switch (setting) {
    case CapabilitySetting::auto_detect:
        return "auto";
    case CapabilitySetting::always:
        return "always";
    case CapabilitySetting::never:
        return "never";
    }
    return "auto";
}

std::string_view to_string(ShowOutput mode) {
    switch (mode) {
    case ShowOutput::failed:
        return "failed";
    case ShowOutput::all:
        return "all";
    case ShowOutput::none:
        return "none";
    }
    return "failed";
}

StreamMode stream_mode_for(OutputMode mode) {
    switch (mode) {
    case OutputMode::human:
    case OutputMode::json:
        return StreamMode::capture;
    case OutputMode::raw:
        return StreamMode::passthrough;
    case OutputMode::wrapped:
        return StreamMode::tee;
    }
    return StreamMode::passthrough;
}

std::optional<CapabilitySetting> env_capability_setting(std::string_view name) {
    auto key = std::string{name};
    auto const* value = std::getenv(key.c_str());
    if (value == nullptr || value[0] == '\0')
        return std::nullopt;
    return parse_capability_setting(value);
}

CapabilitySetting resolve_capability(std::optional<CapabilitySetting> cli_setting,
                                     std::string_view env_name) {
    if (cli_setting)
        return *cli_setting;
    if (auto env_setting = env_capability_setting(env_name))
        return *env_setting;
    return CapabilitySetting::auto_detect;
}

bool should_show_output(ShowOutput mode, bool failed) {
    switch (mode) {
    case ShowOutput::all:
        return true;
    case ShowOutput::failed:
        return failed;
    case ShowOutput::none:
        return false;
    }
    return failed;
}

std::string_view test_status(ProcessResult const& result) {
    if (result.timed_out)
        return "TIMEOUT";
    if (result.exit_code == 0)
        return "PASSED";
    return "FAILED";
}

std::string format_duration(std::chrono::milliseconds elapsed) {
    auto total_ms = elapsed.count();
    if (total_ms < 1000)
        return std::format("{}ms", total_ms);

    auto seconds = static_cast<double>(total_ms) / 1000.0;
    return std::format("{:.2f}s", seconds);
}

namespace ansi {

inline constexpr std::string_view red = "\x1b[31m";
inline constexpr std::string_view yellow = "\x1b[33m";
inline constexpr std::string_view bold = "\x1b[1m";
inline constexpr std::string_view reset = "\x1b[0m";

} // namespace ansi

std::string colorize(std::string_view code, std::string_view text, bool enabled) {
    if (!enabled)
        return std::string{text};
    return std::format("{}{}{}", code, text, ansi::reset);
}

struct Diagnostic {
    enum class Severity { Error, Warning };
    Severity severity = Severity::Error;
    std::string message;
    std::string context;
    std::vector<std::string> hints;
};

std::string json_escape(std::string_view value) {
    auto out = std::string{};
    out.reserve(value.size() + 8);
    for (auto ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
            else
                out.push_back(ch);
            break;
        }
    }
    return out;
}

struct JsonField {
    std::string key;
    std::string value;
    bool quote = true;
};

JsonField json_string(std::string key, std::string_view value) {
    return JsonField{.key = std::move(key), .value = std::string{value}, .quote = true};
}

JsonField json_number(std::string key, long long value) {
    return JsonField{.key = std::move(key), .value = std::format("{}", value), .quote = false};
}

JsonField json_bool(std::string key, bool value) {
    return JsonField{.key = std::move(key), .value = value ? "true" : "false", .quote = false};
}

void emit_json_event(std::string_view event, std::span<JsonField const> fields = {}) {
    auto out = std::format("{{\"schema\":\"exon.cli.v1\",\"event\":\"{}\"",
                           json_escape(event));
    for (auto const& field : fields) {
        out += std::format(",\"{}\":", json_escape(field.key));
        if (field.quote)
            out += std::format("\"{}\"", json_escape(field.value));
        else
            out += field.value;
    }
    out.push_back('}');
    std::println("{}", out);
}

void emit_json_event(std::string_view event, std::initializer_list<JsonField> fields) {
    emit_json_event(event, std::span<JsonField const>{fields.begin(), fields.size()});
}

int emit(Diagnostic const& diag, bool color_enabled) {
    auto label_code =
        diag.severity == Diagnostic::Severity::Error ? ansi::red : ansi::yellow;
    auto label_text =
        diag.severity == Diagnostic::Severity::Error ? "error:" : "warning:";
    auto label = color_enabled
        ? std::format("{}{}{}{}", ansi::bold, label_code, label_text, ansi::reset)
        : std::string{label_text};
    std::println(std::cerr, "{} {}", label, diag.message);
    if (!diag.context.empty())
        std::println(std::cerr, "  at {}", diag.context);
    for (auto const& hint : diag.hints)
        std::println(std::cerr, "  hint: {}", hint);
    return diag.severity == Diagnostic::Severity::Error ? 1 : 0;
}

inline thread_local std::string current_stage_context;
inline thread_local Options current_options;

class ScopedStageContext {
public:
    explicit ScopedStageContext(std::string_view context)
        : previous_(std::exchange(current_stage_context, std::string{context})) {}

    ~ScopedStageContext() {
        current_stage_context = std::move(previous_);
    }

    ScopedStageContext(ScopedStageContext const&) = delete;
    ScopedStageContext& operator=(ScopedStageContext const&) = delete;

private:
    std::string previous_;
};

class ScopedOptionsContext {
public:
    explicit ScopedOptionsContext(Options options)
        : previous_(std::exchange(current_options, std::move(options))) {}

    ~ScopedOptionsContext() {
        current_options = std::move(previous_);
    }

    ScopedOptionsContext(ScopedOptionsContext const&) = delete;
    ScopedOptionsContext& operator=(ScopedOptionsContext const&) = delete;

private:
    Options previous_;
};

} // namespace reporting
