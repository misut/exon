module;
#include <cstdio>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

export module reporting.system;
import std;
import core;
import reporting;

export namespace reporting::system {

ProcessResult run_process(core::ProcessSpec const& spec, StreamMode mode);
using OutputObserver = std::function<void(std::string_view chunk, bool stderr_stream)>;
ProcessResult run_process(core::ProcessSpec const& spec, StreamMode mode,
                          OutputObserver observer);

void enable_vt_on_windows();
bool stdout_is_tty();
bool stderr_is_tty();

enum class LiveProgressRenderMode {
    disabled,
    carriage_return,
    vt,
};

std::string format_progress_frame(ProgressSnapshot const& snapshot,
                                  std::size_t frame_index);

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

} // namespace reporting::system

namespace reporting::system {

namespace tty_detail {

bool no_color_requested() {
    auto const* value = std::getenv("NO_COLOR");
    return value != nullptr && value[0] != '\0';
}

} // namespace tty_detail

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
    if (tty_detail::no_color_requested())
        return false;
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return false;
    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
}

bool stderr_is_tty() {
    if (tty_detail::no_color_requested())
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
    if (tty_detail::no_color_requested())
        return false;
    return ::isatty(STDOUT_FILENO) != 0;
}

bool stderr_is_tty() {
    if (tty_detail::no_color_requested())
        return false;
    return ::isatty(STDERR_FILENO) != 0;
}

#endif

namespace detail {

#if defined(_WIN32)
std::wstring widen(std::string_view text) {
    if (text.empty())
        return {};
    auto size = MultiByteToWideChar(CP_ACP, 0, text.data(),
                                    static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        throw std::runtime_error("failed to convert command line to UTF-16");

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), size);
    return wide;
}

std::wstring quote_windows_arg(std::string_view arg) {
    bool needs_quotes = arg.empty();
    for (char ch : arg) {
        if (ch == ' ' || ch == '\t' || ch == '"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes)
        return widen(arg);

    std::wstring wide = L"\"";
    std::size_t backslashes = 0;
    auto raw = widen(arg);
    for (wchar_t ch : raw) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"')
            wide.append(backslashes * 2 + 1, L'\\');
        else if (backslashes > 0)
            wide.append(backslashes, L'\\');

        backslashes = 0;
        wide.push_back(ch);
    }
    if (backslashes > 0)
        wide.append(backslashes * 2, L'\\');
    wide.push_back(L'"');
    return wide;
}

std::wstring make_command_line(core::ProcessSpec const& spec) {
    std::wstring command = quote_windows_arg(spec.program);
    for (auto const& arg : spec.args) {
        command.push_back(L' ');
        command += quote_windows_arg(arg);
    }
    return command;
}

void append_and_maybe_tee(std::string& sink, char const* data, std::size_t size,
                          bool tee, bool stderr_stream,
                          OutputObserver const* observer = nullptr) {
    sink.append(data, size);
    if (observer && *observer)
        (*observer)(std::string_view{data, size}, stderr_stream);
    if (!tee || size == 0)
        return;

    auto handle = GetStdHandle(stderr_stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
        return;

    auto remaining = static_cast<DWORD>(size);
    auto cursor = data;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(handle, cursor, remaining, &written, nullptr) || written == 0)
            break;
        remaining -= written;
        cursor += written;
    }
}

void drain_pipe(HANDLE pipe, std::string& sink, bool tee, bool stderr_stream,
                OutputObserver const* observer = nullptr) {
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE)
                break;
            throw std::runtime_error(std::format(
                "failed to read child output (GetLastError={})", error));
        }
        if (read == 0)
            break;
        append_and_maybe_tee(sink, buffer.data(), read, tee, stderr_stream, observer);
    }
}
#else
void set_nonblocking(int fd) {
    auto flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        throw std::runtime_error(std::format("fcntl(F_GETFL) failed ({})", errno));
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error(std::format("fcntl(F_SETFL) failed ({})", errno));
}

void write_all(int fd, char const* data, std::size_t size) {
    while (size > 0) {
        auto written = ::write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void append_and_maybe_tee(std::string& sink, char const* data, std::size_t size,
                          bool tee, bool stderr_stream,
                          OutputObserver const* observer = nullptr) {
    sink.append(data, size);
    if (observer && *observer)
        (*observer)(std::string_view{data, size}, stderr_stream);
    if (!tee || size == 0)
        return;
    write_all(stderr_stream ? STDERR_FILENO : STDOUT_FILENO, data, size);
}

int normalize_wait_status(int status) {
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}
#endif

ProcessResult run_passthrough(core::ProcessSpec const& spec) {
    auto started = std::chrono::steady_clock::now();

#if defined(_WIN32)
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                     sizeof(info))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    auto command_line = make_command_line(spec);
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    auto old_error_mode =
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    std::wstring cwd_storage;
    wchar_t const* cwd = nullptr;
    if (!spec.cwd.empty()) {
        cwd_storage = widen(spec.cwd.string());
        cwd = cwd_storage.c_str();
    }

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd,
                        &si, &pi)) {
        auto error = GetLastError();
        if (job != nullptr)
            CloseHandle(job);
        SetErrorMode(old_error_mode);
        throw std::runtime_error(
            std::format("failed to start process (GetLastError={})", error));
    }

    if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }

    auto remaining = spec.timeout
        ? static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(
              spec.timeout->count(), std::numeric_limits<DWORD>::max()))
        : INFINITE;

    auto wait_result = WaitForSingleObject(pi.hProcess, remaining);
    ProcessResult result;
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        result.exit_code = 124;
        if (job != nullptr)
            TerminateJobObject(job, 124);
        else
            TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else if (wait_result != WAIT_OBJECT_0) {
        auto error = GetLastError();
        if (job != nullptr)
            CloseHandle(job);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetErrorMode(old_error_mode);
        throw std::runtime_error(
            std::format("failed while waiting for process (GetLastError={})", error));
    } else {
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
            auto error = GetLastError();
            if (job != nullptr)
                CloseHandle(job);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SetErrorMode(old_error_mode);
            throw std::runtime_error(
                std::format("failed to read process exit code (GetLastError={})", error));
        }
        result.exit_code = static_cast<int>(exit_code);
    }

    if (job != nullptr)
        CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SetErrorMode(old_error_mode);
#else
    auto pid = fork();
    if (pid < 0)
        throw std::runtime_error(std::format("fork() failed ({})", errno));

    if (pid == 0) {
        if (!spec.cwd.empty())
            (void)::chdir(spec.cwd.c_str());
        (void)::setsid();

        std::vector<char*> argv;
        argv.reserve(spec.args.size() + 2);
        argv.push_back(const_cast<char*>(spec.program.c_str()));
        for (auto const& arg : spec.args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        ::execvp(spec.program.c_str(), argv.data());
        _exit(127);
    }

    ProcessResult result;
    int status = 0;
    bool waited = false;
    while (!waited) {
        auto wait = waitpid(pid, &status, WNOHANG);
        if (wait == pid) {
            waited = true;
            break;
        }
        if (wait < 0 && errno != EINTR)
            throw std::runtime_error(std::format("waitpid() failed ({})", errno));

        if (spec.timeout) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (elapsed >= *spec.timeout) {
                result.timed_out = true;
                result.exit_code = 124;
                (void)::kill(-pid, SIGKILL);
                (void)::waitpid(pid, &status, 0);
                waited = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    if (!result.timed_out)
        result.exit_code = normalize_wait_status(status);
#endif

    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

ProcessResult run_captured(core::ProcessSpec const& spec, bool tee,
                           OutputObserver observer = {}) {
    auto started = std::chrono::steady_clock::now();
    ProcessResult result;

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;

    if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &sa, 0) ||
        !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        if (stdout_read) CloseHandle(stdout_read);
        if (stdout_write) CloseHandle(stdout_write);
        if (stderr_read) CloseHandle(stderr_read);
        if (stderr_write) CloseHandle(stderr_write);
        throw std::runtime_error("failed to create child output pipes");
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    PROCESS_INFORMATION pi{};

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                     sizeof(info))) {
            CloseHandle(job);
            job = nullptr;
        }
    }

    auto command_line = make_command_line(spec);
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    auto old_error_mode =
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    std::wstring cwd_storage;
    wchar_t const* cwd = nullptr;
    if (!spec.cwd.empty()) {
        cwd_storage = widen(spec.cwd.string());
        cwd = cwd_storage.c_str();
    }

    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, cwd,
                        &si, &pi)) {
        auto error = GetLastError();
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_read);
        CloseHandle(stderr_write);
        if (job != nullptr)
            CloseHandle(job);
        SetErrorMode(old_error_mode);
        throw std::runtime_error(
            std::format("failed to start process (GetLastError={})", error));
    }

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess)) {
        CloseHandle(job);
        job = nullptr;
    }

    std::thread stdout_thread([&] {
        drain_pipe(stdout_read, result.stdout_text, tee, false, &observer);
    });
    std::thread stderr_thread([&] {
        drain_pipe(stderr_read, result.stderr_text, tee, true, &observer);
    });

    auto remaining = spec.timeout
        ? static_cast<DWORD>(std::min<std::chrono::milliseconds::rep>(
              spec.timeout->count(), std::numeric_limits<DWORD>::max()))
        : INFINITE;

    auto wait_result = WaitForSingleObject(pi.hProcess, remaining);
    if (wait_result == WAIT_TIMEOUT) {
        result.timed_out = true;
        result.exit_code = 124;
        if (job != nullptr)
            TerminateJobObject(job, 124);
        else
            TerminateProcess(pi.hProcess, 124);
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else if (wait_result != WAIT_OBJECT_0) {
        auto error = GetLastError();
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        if (stdout_thread.joinable())
            stdout_thread.join();
        if (stderr_thread.joinable())
            stderr_thread.join();
        if (job != nullptr)
            CloseHandle(job);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetErrorMode(old_error_mode);
        throw std::runtime_error(
            std::format("failed while waiting for process (GetLastError={})", error));
    } else {
        DWORD exit_code = 0;
        if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
            auto error = GetLastError();
            CloseHandle(stdout_read);
            CloseHandle(stderr_read);
            if (stdout_thread.joinable())
                stdout_thread.join();
            if (stderr_thread.joinable())
                stderr_thread.join();
            if (job != nullptr)
                CloseHandle(job);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            SetErrorMode(old_error_mode);
            throw std::runtime_error(
                std::format("failed to read process exit code (GetLastError={})", error));
        }
        result.exit_code = static_cast<int>(exit_code);
    }

    CloseHandle(stdout_read);
    CloseHandle(stderr_read);
    if (stdout_thread.joinable())
        stdout_thread.join();
    if (stderr_thread.joinable())
        stderr_thread.join();

    if (job != nullptr)
        CloseHandle(job);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SetErrorMode(old_error_mode);
#else
    int stdout_pipe[2]{-1, -1};
    int stderr_pipe[2]{-1, -1};
    if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        if (stdout_pipe[0] >= 0) close_fd(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) close_fd(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0) close_fd(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) close_fd(stderr_pipe[1]);
        throw std::runtime_error(std::format("pipe() failed ({})", errno));
    }

    auto pid = fork();
    if (pid < 0) {
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        throw std::runtime_error(std::format("fork() failed ({})", errno));
    }

    if (pid == 0) {
        close_fd(stdout_pipe[0]);
        close_fd(stderr_pipe[0]);
        if (!spec.cwd.empty())
            (void)::chdir(spec.cwd.c_str());
        (void)::setsid();
        (void)::dup2(stdout_pipe[1], STDOUT_FILENO);
        (void)::dup2(stderr_pipe[1], STDERR_FILENO);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[1]);

        std::vector<char*> argv;
        argv.reserve(spec.args.size() + 2);
        argv.push_back(const_cast<char*>(spec.program.c_str()));
        for (auto const& arg : spec.args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);

        ::execvp(spec.program.c_str(), argv.data());
        _exit(127);
    }

    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[1]);
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);

    std::array<pollfd, 2> fds{{
        {.fd = stdout_pipe[0], .events = POLLIN},
        {.fd = stderr_pipe[0], .events = POLLIN},
    }};

    int status = 0;
    bool child_exited = false;
    while (fds[0].fd >= 0 || fds[1].fd >= 0 || !child_exited) {
        auto now = std::chrono::steady_clock::now();
        if (spec.timeout &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started) >= *spec.timeout &&
            !result.timed_out && !child_exited) {
            result.timed_out = true;
            result.exit_code = 124;
            (void)::kill(-pid, SIGKILL);
        }

        auto poll_timeout = 50;
        auto ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), poll_timeout);
        if (ready < 0 && errno != EINTR) {
            close_fd(stdout_pipe[0]);
            close_fd(stderr_pipe[0]);
            throw std::runtime_error(std::format("poll() failed ({})", errno));
        }

        auto drain_fd = [&](pollfd& fd, std::string& sink, bool stderr_stream) {
            if (fd.fd < 0)
                return;
            if ((fd.revents & (POLLIN | POLLHUP)) == 0)
                return;

            std::array<char, 4096> buffer{};
            while (true) {
                auto read = ::read(fd.fd, buffer.data(), buffer.size());
                if (read > 0) {
                    append_and_maybe_tee(sink, buffer.data(), static_cast<std::size_t>(read), tee,
                                         stderr_stream, &observer);
                    continue;
                }
                if (read == 0) {
                    close_fd(fd.fd);
                    break;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    break;
                throw std::runtime_error(std::format("read() failed ({})", errno));
            }
        };

        drain_fd(fds[0], result.stdout_text, false);
        drain_fd(fds[1], result.stderr_text, true);

        if (!child_exited) {
            auto wait = waitpid(pid, &status, WNOHANG);
            if (wait == pid) {
                child_exited = true;
                if (!result.timed_out)
                    result.exit_code = normalize_wait_status(status);
            } else if (wait < 0 && errno != EINTR) {
                close_fd(stdout_pipe[0]);
                close_fd(stderr_pipe[0]);
                throw std::runtime_error(std::format("waitpid() failed ({})", errno));
            }
        }
    }
#endif

    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

} // namespace detail

ProcessResult run_process(core::ProcessSpec const& spec, StreamMode mode) {
    return run_process(spec, mode, {});
}

ProcessResult run_process(core::ProcessSpec const& spec, StreamMode mode,
                          OutputObserver observer) {
    switch (mode) {
    case StreamMode::passthrough:
        return detail::run_passthrough(spec);
    case StreamMode::tee:
        return detail::run_captured(spec, true, std::move(observer));
    case StreamMode::capture:
        return detail::run_captured(spec, false, std::move(observer));
    }
    return detail::run_passthrough(spec);
}

namespace progress_detail {

constexpr auto spinner_frames =
    std::array<std::string_view, 4>{"|", "/", "-", "\\"};

void write_raw(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

} // namespace progress_detail

std::string format_progress_frame(ProgressSnapshot const& snapshot,
                                  std::size_t frame_index) {
    auto spin = progress_detail::spinner_frames
        [frame_index % progress_detail::spinner_frames.size()];
    if (snapshot.total <= 0) {
        auto label = snapshot.label.empty() ? std::string_view{"working"}
                                            : snapshot.label;
        return std::format("  [{}] {}...", spin, label);
    }
    if (snapshot.label.empty())
        return std::format("  [{}] [{}/{} {}%]", spin, snapshot.done,
                           snapshot.total, snapshot.percent);
    return std::format("  [{}] [{}/{} {}%] {}", spin, snapshot.done,
                       snapshot.total, snapshot.percent, snapshot.label);
}

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

    auto text = format_progress_frame(*progress, spinner_index_++);
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

} // namespace reporting::system
