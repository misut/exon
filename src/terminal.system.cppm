module;
#include <cstdio>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

export module terminal.system;
import std;
import reporting;
import terminal;

export namespace terminal::system {

struct ProgressSource {
    std::function<std::optional<terminal::ProgressSnapshot>()> poll;
};

enum class LiveProgressRenderMode {
    disabled,
    carriage_return,
    vt,
};

void enable_vt_on_windows();
bool no_color_requested();
bool stdout_is_terminal();
bool stderr_is_terminal();
bool stdout_is_tty();
bool stderr_is_tty();
bool stdout_hyperlinks_enabled();
bool stdout_unicode_enabled();
std::size_t terminal_width();

class LiveProgressRenderer {
public:
    explicit LiveProgressRenderer(LiveProgressRenderMode mode
#if defined(_WIN32)
                                  ,
                                  HANDLE handle = nullptr,
                                  DWORD original_console_mode = 0,
                                  bool restore_console_mode = false
#endif
    );
    ~LiveProgressRenderer();

    LiveProgressRenderer(LiveProgressRenderer const&) = delete;
    LiveProgressRenderer& operator=(LiveProgressRenderer const&) = delete;

    bool active() const;
    void start(ProgressSource source);
    void refresh();
    void stop();

private:
    void render_once();
    void clear_line();
    void clear_rendered_frame();

    LiveProgressRenderMode mode_ = LiveProgressRenderMode::disabled;
    ProgressSource source_{};
    std::thread thread_;
    std::atomic<bool> stop_requested_ = false;
    std::mutex render_mutex_;
    std::size_t spinner_index_ = 0;
    std::size_t last_width_ = 0;
    std::size_t last_line_count_ = 0;
    bool cursor_hidden_ = false;
    std::chrono::steady_clock::time_point started_{};
#if defined(_WIN32)
    HANDLE handle_ = nullptr;
    DWORD original_console_mode_ = 0;
    bool restore_console_mode_ = false;
#endif
};

std::unique_ptr<LiveProgressRenderer> make_live_progress_renderer();

} // namespace terminal::system

namespace terminal::system {

bool no_color_requested() {
    auto const* value = std::getenv("NO_COLOR");
    return value != nullptr && value[0] != '\0';
}

bool env_is_set(std::string_view name) {
    auto key = std::string{name};
    auto const* value = std::getenv(key.c_str());
    return value != nullptr && value[0] != '\0';
}

bool term_is_dumb() {
    auto const* value = std::getenv("TERM");
    return value != nullptr && std::string_view{value} == "dumb";
}

bool github_actions() {
    return env_is_set("GITHUB_ACTIONS");
}

bool generic_ci() {
    return env_is_set("CI");
}

bool color_enabled_for_stdout() {
    auto setting = reporting::resolve_capability(reporting::current_options.color,
                                                 "EXON_COLOR");
    if (setting == reporting::CapabilitySetting::always)
        return true;
    if (setting == reporting::CapabilitySetting::never)
        return false;
    return stdout_is_terminal() && !no_color_requested() && !term_is_dumb();
}

bool color_enabled_for_stderr() {
    auto setting = reporting::resolve_capability(reporting::current_options.color,
                                                 "EXON_COLOR");
    if (setting == reporting::CapabilitySetting::always)
        return true;
    if (setting == reporting::CapabilitySetting::never)
        return false;
    return stderr_is_terminal() && !no_color_requested() && !term_is_dumb();
}

bool progress_enabled_for_stdout() {
    if (reporting::current_options.output == reporting::OutputMode::json)
        return false;
    if (github_actions())
        return false;

    auto setting = reporting::resolve_capability(reporting::current_options.progress,
                                                 "EXON_PROGRESS");
    if (setting == reporting::CapabilitySetting::always)
        return stdout_is_terminal();
    if (setting == reporting::CapabilitySetting::never)
        return false;

    return stdout_is_terminal() && !generic_ci() && !no_color_requested() && !term_is_dumb();
}

bool stdout_hyperlinks_enabled() {
    auto setting = reporting::resolve_capability(reporting::current_options.hyperlinks,
                                                 "EXON_HYPERLINKS");
    if (setting == reporting::CapabilitySetting::always)
        return true;
    if (setting == reporting::CapabilitySetting::never)
        return false;
    return stdout_is_terminal() && !term_is_dumb();
}

bool stdout_unicode_enabled() {
    auto setting = reporting::resolve_capability(reporting::current_options.unicode,
                                                 "EXON_UNICODE");
    if (setting == reporting::CapabilitySetting::always)
        return true;
    if (setting == reporting::CapabilitySetting::never)
        return false;
    return stdout_is_terminal() && !term_is_dumb();
}

#if defined(_WIN32)

void enable_vt_on_windows() {
    auto enable = [](DWORD stream) {
        auto handle = GetStdHandle(stream);
        if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
            return;
        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode))
            return;
        (void)SetConsoleMode(handle, mode | ENABLE_PROCESSED_OUTPUT |
                                         ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    };
    enable(STD_OUTPUT_HANDLE);
    enable(STD_ERROR_HANDLE);
}

bool stdout_is_terminal() {
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return false;
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
}

bool stderr_is_terminal() {
    auto handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return false;
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
}

bool stdout_is_tty() {
    return color_enabled_for_stdout();
}

bool stderr_is_tty() {
    return color_enabled_for_stderr();
}

std::size_t terminal_width() {
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return 0;
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(handle, &info))
        return 0;
    return static_cast<std::size_t>(
        std::max<SHORT>(0, info.srWindow.Right - info.srWindow.Left + 1));
}

#else

void enable_vt_on_windows() {}

bool stdout_is_terminal() {
    return ::isatty(STDOUT_FILENO) != 0;
}

bool stderr_is_terminal() {
    return ::isatty(STDERR_FILENO) != 0;
}

bool stdout_is_tty() {
    return color_enabled_for_stdout();
}

bool stderr_is_tty() {
    return color_enabled_for_stderr();
}

std::size_t terminal_width() {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 || size.ws_col == 0)
        return 0;
    return static_cast<std::size_t>(size.ws_col);
}

#endif

namespace progress_detail {

void write_raw(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

std::string fit_line_to_terminal_width(std::string text, std::size_t width) {
    if (width == 0 || text.size() <= width)
        return text;
    if (width <= 3)
        return text.substr(0, width);
    text.resize(width - 3);
    text += "...";
    return text;
}

std::string fit_to_terminal_width(std::string text) {
    auto width = terminal_width();
    if (width == 0)
        return text;

    auto out = std::string{};
    auto start = std::size_t{0};
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        auto last = end == std::string::npos;
        auto line = last ? text.substr(start) : text.substr(start, end - start);
        out += fit_line_to_terminal_width(std::move(line), width);
        if (last)
            break;
        out.push_back('\n');
        start = end + 1;
    }
    return out;
}

std::size_t rendered_line_count(std::string_view text) {
    if (text.empty())
        return 1;
    return 1 + static_cast<std::size_t>(std::ranges::count(text, '\n'));
}

std::size_t rendered_last_line_width(std::string_view text) {
    auto last_newline = text.rfind('\n');
    if (last_newline == std::string_view::npos)
        return text.size();
    return text.size() - last_newline - 1;
}

} // namespace progress_detail

LiveProgressRenderer::LiveProgressRenderer(LiveProgressRenderMode mode
#if defined(_WIN32)
                                           ,
                                           HANDLE handle,
                                           DWORD original_console_mode,
                                           bool restore_console_mode
#endif
                                           )
    : mode_(mode)
#if defined(_WIN32)
    , handle_(handle)
    , original_console_mode_(original_console_mode)
    , restore_console_mode_(restore_console_mode)
#endif
{}

LiveProgressRenderer::~LiveProgressRenderer() {
    stop();
#if defined(_WIN32)
    if (restore_console_mode_ && handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
        SetConsoleMode(handle_, original_console_mode_);
        restore_console_mode_ = false;
    }
#endif
}

bool LiveProgressRenderer::active() const {
    return mode_ != LiveProgressRenderMode::disabled;
}

void LiveProgressRenderer::start(ProgressSource source) {
    if (!active() || thread_.joinable())
        return;
    source_ = std::move(source);
    started_ = std::chrono::steady_clock::now();
    stop_requested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] {
        while (!stop_requested_.load(std::memory_order_relaxed)) {
            render_once();
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    });
}

void LiveProgressRenderer::refresh() {
    if (!active())
        return;
    render_once();
}

void LiveProgressRenderer::stop() {
    if (thread_.joinable()) {
        stop_requested_.store(true, std::memory_order_relaxed);
        thread_.join();
    }
    clear_line();
}

void LiveProgressRenderer::render_once() {
    auto render_lock = std::lock_guard{render_mutex_};
    if (!source_.poll)
        return;

    auto progress = source_.poll();
    if (!progress)
        return;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_);
    progress->elapsed = elapsed;
    if (progress->total > 0 && progress->done > 0 && elapsed.count() > 0) {
        progress->rate = static_cast<double>(progress->done) /
                         (static_cast<double>(elapsed.count()) / 1000.0);
        auto remaining = progress->total - progress->done;
        if (remaining > 0 && progress->rate > 0.0) {
            auto seconds = static_cast<double>(remaining) / progress->rate;
            progress->remaining = std::chrono::milliseconds{
                static_cast<long long>(seconds * 1000.0)};
        }
    }

    auto text = terminal::format_progress_frame(*progress, spinner_index_++,
                                                stdout_is_tty());
    text = progress_detail::fit_to_terminal_width(std::move(text));
    auto const line_count = progress_detail::rendered_line_count(text);
    if (mode_ == LiveProgressRenderMode::vt) {
        if (!cursor_hidden_) {
            progress_detail::write_raw("\x1b[?25l");
            cursor_hidden_ = true;
        }
        if (last_line_count_ == 0)
            progress_detail::write_raw("\r\x1b[2K");
        else
            clear_rendered_frame();
        progress_detail::write_raw(text);
        std::fflush(stdout);
        last_width_ = progress_detail::rendered_last_line_width(text);
        last_line_count_ = line_count;
        return;
    }

    auto text_line = text;
    if (auto newline = text_line.find('\n'); newline != std::string::npos)
        text_line.resize(newline);
    progress_detail::write_raw("\r");
    progress_detail::write_raw(text_line);
    if (text_line.size() < last_width_) {
        auto padding = std::string(last_width_ - text_line.size(), ' ');
        progress_detail::write_raw(padding);
    }
    std::fflush(stdout);
    last_width_ = text_line.size();
    last_line_count_ = 1;
}

void LiveProgressRenderer::clear_rendered_frame() {
    if (last_line_count_ == 0)
        return;

    if (mode_ == LiveProgressRenderMode::vt || last_line_count_ > 1) {
        progress_detail::write_raw("\r\x1b[2K");
        for (std::size_t line = 1; line < last_line_count_; ++line)
            progress_detail::write_raw("\x1b[1A\r\x1b[2K");
        last_line_count_ = 0;
        last_width_ = 0;
        return;
    }

    progress_detail::write_raw("\r");
    auto padding = std::string(last_width_, ' ');
    progress_detail::write_raw(padding);
    progress_detail::write_raw("\r");
    last_line_count_ = 0;
    last_width_ = 0;
}

void LiveProgressRenderer::clear_line() {
    if (!active())
        return;

    if (mode_ == LiveProgressRenderMode::vt) {
        clear_rendered_frame();
        if (cursor_hidden_)
            progress_detail::write_raw("\x1b[?25h");
        std::fflush(stdout);
        cursor_hidden_ = false;
        last_line_count_ = 0;
        return;
    }

    clear_rendered_frame();
    std::fflush(stdout);
}

std::unique_ptr<LiveProgressRenderer> make_live_progress_renderer() {
#if defined(_WIN32)
    if (!progress_enabled_for_stdout())
        return {};

    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return {};

    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode))
        return {};

    auto vt_mode = mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (SetConsoleMode(handle, vt_mode)) {
        return std::make_unique<LiveProgressRenderer>(LiveProgressRenderMode::vt, handle,
                                                      mode, true);
    }
    return std::make_unique<LiveProgressRenderer>(LiveProgressRenderMode::carriage_return);
#else
    if (!progress_enabled_for_stdout())
        return {};
    return std::make_unique<LiveProgressRenderer>(LiveProgressRenderMode::vt);
#endif
}

} // namespace terminal::system
