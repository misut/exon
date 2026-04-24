export module terminal;
import std;

export namespace terminal {

enum class StyleRole {
    accent,
    dim,
    success,
    warning,
    error,
    bold,
};

enum class StatusKind {
    run,
    ok,
    fail,
    timeout,
    skip,
};

struct ProgressSnapshot {
    int done = 0;
    int total = 0;
    int percent = 0;
    std::string_view label;
};

namespace ansi {

inline constexpr std::string_view reset = "\x1b[0m";
inline constexpr std::string_view bold = "\x1b[1m";
inline constexpr std::string_view dim = "\x1b[2m";
inline constexpr std::string_view red = "\x1b[31m";
inline constexpr std::string_view green = "\x1b[32m";
inline constexpr std::string_view yellow = "\x1b[33m";
inline constexpr std::string_view cyan = "\x1b[36m";

} // namespace ansi

std::string_view style_code(StyleRole role) {
    switch (role) {
    case StyleRole::accent:
        return ansi::cyan;
    case StyleRole::dim:
        return ansi::dim;
    case StyleRole::success:
        return ansi::green;
    case StyleRole::warning:
        return ansi::yellow;
    case StyleRole::error:
        return ansi::red;
    case StyleRole::bold:
        return ansi::bold;
    }
    return {};
}

std::string style(std::string_view text, StyleRole role, bool enabled) {
    if (!enabled)
        return std::string{text};
    return std::format("{}{}{}", style_code(role), text, ansi::reset);
}

std::string_view status_label(StatusKind status) {
    switch (status) {
    case StatusKind::run:
        return "RUN";
    case StatusKind::ok:
        return "OK";
    case StatusKind::fail:
        return "FAIL";
    case StatusKind::timeout:
        return "TIMEOUT";
    case StatusKind::skip:
        return "SKIP";
    }
    return "RUN";
}

StyleRole status_role(StatusKind status) {
    switch (status) {
    case StatusKind::run:
        return StyleRole::accent;
    case StatusKind::ok:
        return StyleRole::success;
    case StatusKind::fail:
    case StatusKind::timeout:
        return StyleRole::error;
    case StatusKind::skip:
        return StyleRole::dim;
    }
    return StyleRole::accent;
}

std::string status_cell(StatusKind status, bool color_enabled,
                        std::size_t width = 7) {
    auto label = status_label(status);
    auto padded = std::format("{:<{}}", label, width);
    return style(padded, status_role(status), color_enabled);
}

std::string key_value(std::string_view key, std::string_view value,
                      std::size_t width = 10) {
    return std::format("  {:<{}} {}", key, width, value);
}

std::string stage(std::string_view name, int index, int total,
                  std::string_view context = {}, bool color_enabled = false) {
    auto prefix = total > 0
        ? std::format("[{}/{}]", index, total)
        : std::string{"[ ]"};
    prefix = style(prefix, StyleRole::accent, color_enabled);

    if (context.empty())
        return std::format("{} {}", prefix, name);
    return std::format("{} [{}] {}", prefix, context, name);
}

std::string section(std::string_view title, bool color_enabled = false) {
    return style(title, StyleRole::bold, color_enabled);
}

std::string tail_excerpt(std::string_view text, std::size_t max_chars = 2000) {
    if (text.empty() || text.size() <= max_chars)
        return std::string{text};

    auto start = text.size() - max_chars;
    if (auto line_start = text.find('\n', start);
        line_start != std::string_view::npos && line_start + 1 < text.size()) {
        start = line_start + 1;
    }
    return std::format("...\n{}", text.substr(start));
}

std::string output_block_header(std::string_view heading,
                                bool color_enabled = false) {
    return style(std::format("---- {} ----", heading), StyleRole::dim,
                 color_enabled);
}

std::string format_progress_frame(ProgressSnapshot const& snapshot,
                                  std::size_t frame_index,
                                  bool color_enabled = false) {
    constexpr auto spinner_frames =
        std::array<std::string_view, 4>{"|", "/", "-", "\\"};
    auto spin = spinner_frames[frame_index % spinner_frames.size()];
    auto run = status_cell(StatusKind::run, color_enabled);
    if (snapshot.total <= 0) {
        auto label = snapshot.label.empty() ? std::string_view{"working"}
                                            : snapshot.label;
        return std::format("  {} [{}] {}...", run, spin, label);
    }

    if (snapshot.label.empty())
        return std::format("  {} [{}] [{}/{} {}%]", run, spin,
                           snapshot.done, snapshot.total, snapshot.percent);

    return std::format("  {} [{}] [{}/{} {}%] {}", run, spin,
                       snapshot.done, snapshot.total, snapshot.percent,
                       snapshot.label);
}

} // namespace terminal
