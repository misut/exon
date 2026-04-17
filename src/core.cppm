export module core;
import std;

export namespace core {

enum class DiagnosticSeverity {
    note,
    warning,
    error,
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::error;
    std::string message;
    std::string hint;
};

struct ProcessSpec {
    std::filesystem::path cwd;
    std::string command;
    std::optional<std::chrono::milliseconds> timeout = std::nullopt;
    std::string label;
};

struct FileWrite {
    std::filesystem::path path;
    std::string content;
    bool skip_if_unchanged = true;
    std::string success_message;
};

struct ProjectContext {
    std::filesystem::path root;
    std::filesystem::path exon_dir;
    std::filesystem::path build_dir;
    std::string profile;
    std::string target;
    bool is_wasm = false;
};

} // namespace core
