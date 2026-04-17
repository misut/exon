export module core;
import std;
import cppx.fs;
import cppx.process;

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

using ProcessSpec = cppx::process::ProcessSpec;
using ProcessResult = cppx::process::ProcessResult;
using TextWrite = cppx::fs::TextWrite;

struct ProcessStep {
    ProcessSpec spec;
    std::string label;
};

struct FileWrite {
    TextWrite text;
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
