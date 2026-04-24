module;
#include <cstdio>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

export module terminal.system;
import std;
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
bool stdout_is_tty();
bool stderr_is_tty();

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

    LiveProgressRenderMode mode_ = LiveProgressRenderMode::disabled;
    ProgressSource source_{};
    std::thread thread_;
    std::atomic<bool> stop_requested_ = false;
    std::size_t spinner_index_ = 0;
    std::size_t last_width_ = 0;
    bool cursor_hidden_ = false;
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

bool stdout_is_tty() {
    if (no_color_requested())
        return false;
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return false;
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
}

bool stderr_is_tty() {
    if (no_color_requested())
        return false;
    auto handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return false;
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
}

#else

void enable_vt_on_windows() {}

bool stdout_is_tty() {
    if (no_color_requested())
        return false;
    return ::isatty(STDOUT_FILENO) != 0;
}

bool stderr_is_tty() {
    if (no_color_requested())
        return false;
    return ::isatty(STDERR_FILENO) != 0;
}

#endif

namespace progress_detail {

void write_raw(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
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
    if (!source_.poll)
        return;

    auto progress = source_.poll();
    if (!progress)
        return;

    auto text = terminal::format_progress_frame(*progress, spinner_index_++,
                                                stdout_is_tty());
    if (mode_ == LiveProgressRenderMode::vt) {
        if (!cursor_hidden_) {
            progress_detail::write_raw("\x1b[?25l");
            cursor_hidden_ = true;
        }
        progress_detail::write_raw("\r\x1b[2K");
        progress_detail::write_raw(text);
        std::fflush(stdout);
        last_width_ = 0;
        return;
    }

    progress_detail::write_raw("\r");
    progress_detail::write_raw(text);
    if (text.size() < last_width_) {
        auto padding = std::string(last_width_ - text.size(), ' ');
        progress_detail::write_raw(padding);
    }
    std::fflush(stdout);
    last_width_ = text.size();
}

void LiveProgressRenderer::clear_line() {
    if (!active())
        return;

    if (mode_ == LiveProgressRenderMode::vt) {
        if (!cursor_hidden_)
            return;
        progress_detail::write_raw("\r\x1b[2K\x1b[?25h");
        std::fflush(stdout);
        cursor_hidden_ = false;
        return;
    }

    if (last_width_ == 0)
        return;

    progress_detail::write_raw("\r");
    auto padding = std::string(last_width_, ' ');
    progress_detail::write_raw(padding);
    progress_detail::write_raw("\r");
    std::fflush(stdout);
    last_width_ = 0;
}

std::unique_ptr<LiveProgressRenderer> make_live_progress_renderer() {
#if defined(_WIN32)
    if (!stdout_is_tty())
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
    if (!stdout_is_tty())
        return {};
    return std::make_unique<LiveProgressRenderer>(LiveProgressRenderMode::carriage_return);
#endif
}

} // namespace terminal::system
