export module reporting;
import std;

export namespace reporting {

enum class OutputMode {
    human,
    raw,
    wrapped,
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
    std::string_view label;
};

struct ProgressSource {
    std::function<std::optional<ProgressSnapshot>()> poll;
};

std::optional<OutputMode> parse_output_mode(std::string_view value) {
    if (value == "human")
        return OutputMode::human;
    if (value == "raw")
        return OutputMode::raw;
    if (value == "wrapped")
        return OutputMode::wrapped;
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
    case OutputMode::raw:
        return "raw";
    case OutputMode::wrapped:
        return "wrapped";
    }
    return "raw";
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
        return StreamMode::capture;
    case OutputMode::raw:
        return StreamMode::passthrough;
    case OutputMode::wrapped:
        return StreamMode::tee;
    }
    return StreamMode::passthrough;
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

} // namespace reporting
