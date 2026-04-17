export module lock.system;
import std;
import lock;

export namespace lock::system {

namespace detail {

std::string read_file(std::filesystem::path const& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("failed to read {}", path.string()));
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

} // namespace detail

LockFile load(std::string_view path) {
    auto file_path = std::filesystem::path{path};
    if (!std::filesystem::exists(file_path))
        return {};
    return lock::parse(detail::read_file(file_path));
}

void save(LockFile const& lf, std::string_view path) {
    auto file = std::ofstream(std::string{path});
    if (!file)
        throw std::runtime_error(std::format("failed to write {}", path));
    file << lock::render(lf);
}

} // namespace lock::system
