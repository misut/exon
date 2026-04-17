export module lock.system;
import std;
import cppx.fs;
import cppx.fs.system;
import lock;

export namespace lock::system {

namespace detail {

std::string read_file(std::filesystem::path const& path) {
    auto text = cppx::fs::system::read_text(path);
    if (!text)
        throw std::runtime_error(std::format(
            "failed to read {} ({})", path.string(), cppx::fs::to_string(text.error())));
    return *text;
}

} // namespace detail

LockFile load(std::string_view path) {
    auto file_path = std::filesystem::path{path};
    if (!std::filesystem::exists(file_path))
        return {};
    return lock::parse(detail::read_file(file_path));
}

void save(LockFile const& lf, std::string_view path) {
    auto result = cppx::fs::system::write_if_changed({
        .path = std::filesystem::path{path},
        .content = lock::render(lf),
    });
    if (!result) {
        throw std::runtime_error(std::format(
            "failed to write {} ({})", path, cppx::fs::to_string(result.error())));
    }
}

} // namespace lock::system
