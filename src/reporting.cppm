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

} // namespace reporting
