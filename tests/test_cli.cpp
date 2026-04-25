import std;
import cli;
import commands;
import reporting;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

// helper: simulate argv for cli::parse
cli::Args test_parse(std::vector<std::string> const& args, std::vector<cli::ArgDef> defs) {
    std::vector<char*> ptrs;
    for (auto& a : args)
        ptrs.push_back(const_cast<char*>(a.c_str()));
    return cli::parse(static_cast<int>(ptrs.size()), ptrs.data(), 0, std::move(defs));
}

std::string read_text(std::filesystem::path const& path) {
    auto in = std::ifstream{path};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// --- usage tests ---

void test_usage_alignment() {
    auto out = cli::usage("tool", {
        cli::Section{"commands", {
            {"short",      "short command"},
            {"very-long-command", "long command"},
        }},
    });
    // both descriptions should start at the same column
    auto line1 = out.find("short");
    auto desc1 = out.find("short command");
    auto line2 = out.find("very-long-command");
    auto desc2 = out.find("long command");
    // description column offsets relative to their line should be equal
    auto col1 = desc1 - out.rfind('\n', desc1);
    auto col2 = desc2 - out.rfind('\n', desc2);
    check(col1 == col2, "descriptions aligned at same column");
}

void test_usage_wrap() {
    auto out = cli::usage("tool", {
        cli::Section{"commands", {
            {"short",  "desc a"},
            {"this-is-a-very-very-very-very-very-long-syntax-that-exceeds-40-chars", "desc b"},
        }},
    });
    // long syntax should be on its own line, description on the next
    auto long_pos = out.find("this-is-a-very");
    auto desc_pos = out.find("desc b");
    check(long_pos != std::string::npos, "long syntax present");
    check(desc_pos != std::string::npos, "wrapped description present");
    // the description should be on a DIFFERENT line than the syntax
    auto nl_between = out.find('\n', long_pos);
    check(nl_between < desc_pos, "description on next line after long syntax");
}

void test_usage_empty_description() {
    auto out = cli::usage("tool", {
        cli::Section{"info", {
            {"llvm, cmake, ninja", ""},
        }},
    });
    check(out.find("llvm, cmake, ninja") != std::string::npos, "entry with empty desc rendered");
    // no trailing spaces after the entry (just newline)
    auto pos = out.find("llvm, cmake, ninja");
    auto nl = out.find('\n', pos);
    auto line = out.substr(pos, nl - pos);
    check(line == "llvm, cmake, ninja", "no trailing padding for empty desc");
}

// --- parse tests ---

void test_parse_flag() {
    auto args = test_parse({"--release"}, {cli::Flag{"--release"}});
    check(args.has("--release"), "flag --release set");
    check(!args.has("--debug"), "flag --debug not set");
}

void test_parse_option() {
    auto args = test_parse({"--target", "wasm"}, {cli::Option{"--target"}});
    check(args.get("--target") == "wasm", "option --target = wasm");
    check(args.get("--missing").empty(), "missing option returns empty");
}

void test_parse_list_option() {
    auto args = test_parse({"--features", "a, b ,c"}, {cli::ListOption{"--features"}});
    auto& list = args.get_list("--features");
    check(list.size() == 3, "list has 3 items");
    check(list[0] == "a", "list[0] = a");
    check(list[1] == "b", "list[1] = b (trimmed)");
    check(list[2] == "c", "list[2] = c (trimmed)");
}

void test_parse_positional() {
    auto args = test_parse({"foo", "bar"}, {});
    auto& pos = args.positional();
    check(pos.size() == 2, "2 positional args");
    check(pos[0] == "foo", "pos[0] = foo");
    check(pos[1] == "bar", "pos[1] = bar");
}

void test_parse_separator() {
    auto args = test_parse({"--release", "--", "--not-a-flag"}, {cli::Flag{"--release"}});
    check(args.has("--release"), "flag before -- is parsed");
    auto& pos = args.positional();
    check(pos.size() == 1, "1 positional after --");
    check(pos[0] == "--not-a-flag", "positional preserved as-is");
}

void test_parse_unknown_flag() {
    bool threw = false;
    try {
        test_parse({"--unknown"}, {cli::Flag{"--release"}});
    } catch (std::runtime_error const& e) {
        threw = true;
        check(std::string_view{e.what()}.find("unknown flag") != std::string_view::npos,
              "error mentions unknown flag");
    }
    check(threw, "unknown flag throws");
}

void test_parse_missing_value() {
    bool threw = false;
    try {
        test_parse({"--target"}, {cli::Option{"--target"}});
    } catch (std::runtime_error const& e) {
        threw = true;
        check(std::string_view{e.what()}.find("requires a value") != std::string_view::npos,
              "error mentions missing value");
    }
    check(threw, "missing value throws");
}

void test_parse_mixed() {
    auto args = test_parse(
        {"--release", "--target", "wasm", "foo", "--features", "x,y"},
        {cli::Flag{"--release"}, cli::Option{"--target"}, cli::ListOption{"--features"}});
    check(args.has("--release"), "mixed: flag");
    check(args.get("--target") == "wasm", "mixed: option");
    check(args.get_list("--features").size() == 2, "mixed: list");
    check(args.positional().size() == 1, "mixed: positional");
    check(args.positional()[0] == "foo", "mixed: positional value");
}

void test_parse_debugger_option_with_separator() {
    auto args = test_parse(
        {"--debugger", "lldb", "--", "--flag", "value"},
        {cli::Option{"--debugger"}});
    check(args.get("--debugger") == "lldb", "debugger option parsed");
    check(args.positional().size() == 2, "debugger separator keeps positionals");
    check(args.positional()[0] == "--flag", "debugger positional flag preserved");
    check(args.positional()[1] == "value", "debugger positional value preserved");
}

void test_commands_usage_lists_debug() {
    auto usage = commands::usage_text();
    check(usage.find(
              "debug [--release] [--debugger auto|lldb|gdb|devenv|cdb|<path>] "
              "[--member <name>] [--exclude x,y] [-- <args...>]") !=
              std::string::npos,
          "usage lists debug command");
}

void test_commands_usage_lists_human_output_mode() {
    auto usage = commands::usage_text();
    check(usage.find("[--output human|json|wrapped|raw]") != std::string::npos,
          "usage lists json output mode");
    check(usage.find("[--color auto|always|never]") != std::string::npos,
          "usage lists color capability");
    check(usage.find("status [--output human|json]") != std::string::npos,
          "usage lists status command");
    check(usage.find("doctor [--output human|json]") != std::string::npos,
          "usage lists doctor alias");
    check(usage.find("add [--dev] <pkg> <ver> [--features a,b] "
                     "[--no-default-features]") != std::string::npos,
          "usage lists git dependency feature options");
    check(usage.find("add [--dev] --cmake <name> --repo <url> --tag <tag> "
                     "--targets <targets> [--option K=V] [--shallow false]") !=
              std::string::npos,
          "usage lists raw CMake dependency options");
    check(usage.find("outdated [pkg...] [--member a,b] [--exclude x,y] "
                     "[--output human|json]") != std::string::npos,
          "usage lists outdated command");
    check(usage.find("update [pkg...] [--dry-run] [--precise <version>] "
                     "[--member a,b] [--exclude x,y]") != std::string::npos,
          "usage lists update package options");
    check(usage.find("tree [--member a,b] [--exclude x,y] [--dev] "
                     "[--features] [--output human|json]") != std::string::npos,
          "usage lists tree command");
    check(usage.find("why <pkg> [--member a,b] [--exclude x,y] [--dev] "
                     "[--output human|json]") != std::string::npos,
          "usage lists why command");
    check(usage.find("[--output human|json|wrapped|raw] [--show-output failed|all|none]") !=
              std::string::npos,
          "usage lists json output mode for test");
    check(usage.find("aarch64-linux-android") != std::string::npos,
          "usage lists Android target");
}

void test_commands_reporting_defaults() {
    auto options = commands::parse_reporting_options();
    check(options.output == reporting::OutputMode::human,
          "commands default output mode is human");
    check(options.show_output == reporting::ShowOutput::failed,
          "commands default show-output is failed");
    check(!options.color.has_value(), "commands default color is env/default");
}

void test_commands_reporting_capabilities() {
    auto options = commands::parse_reporting_options(
        "json", "none", "always", "never", "auto", "always");
    check(options.output == reporting::OutputMode::json,
          "commands parse json output mode");
    check(options.show_output == reporting::ShowOutput::none,
          "commands parse show output none");
    check(options.color == reporting::CapabilitySetting::always,
          "commands parse color always");
    check(options.progress == reporting::CapabilitySetting::never,
          "commands parse progress never");
    check(options.unicode == reporting::CapabilitySetting::auto_detect,
          "commands parse unicode auto");
    check(options.hyperlinks == reporting::CapabilitySetting::always,
          "commands parse hyperlinks always");
}

void test_commands_suggest_unknown_command() {
    auto suggestion = commands::suggest_command("udpate");
    check(suggestion && *suggestion == "update",
          "commands suggest close command names");
    check(!commands::suggest_command("zzzzzz").has_value(),
          "commands ignore distant command names");
}

void test_readme_output_docs_match_usage() {
    auto readme = read_text(std::filesystem::current_path() / "README.md");
    check(readme.find(
              "`exon run [--release] [--target <t>] [--member <name>] [args]`") !=
              std::string::npos,
          "README documents run args");
    check(readme.find(
              "`exon build [--release] [--target <t>] [--member a,b] [--exclude x,y] "
              "[--output human\\|json\\|wrapped\\|raw]") != std::string::npos,
          "README documents build json output mode");
    check(readme.find(
              "`exon test [--release] [--target <t>] [--member a,b] [--exclude x,y] "
              "[--timeout <sec>] [--output human\\|json\\|wrapped\\|raw] "
              "[--show-output failed\\|all\\|none]`") != std::string::npos,
          "README documents test json output mode");
    check(readme.find("`exon status [--output human\\|json]`") != std::string::npos,
          "README documents status command");
    check(readme.find(
              "`exon add [--dev] --git <repo> --version <v> --subdir <dir> [--name <n>]`") !=
              std::string::npos,
          "README documents git subdir add name override");
    check(readme.find(
              "`exon add [--dev] <pkg> <ver> [--features a,b] [--no-default-features]`") !=
              std::string::npos,
          "README documents git dependency feature options");
    check(readme.find(
              "`exon add [--dev] --cmake <name> --repo <url> --tag <tag> --targets <targets> [--option K=V] [--shallow false]`") !=
              std::string::npos,
          "README documents raw CMake dependency options");
    check(readme.find("[dependencies.cmake.glfw]") != std::string::npos,
          "README documents raw CMake dependency table");
    check(readme.find("commit hash is the most reproducible choice") !=
              std::string::npos,
          "README documents reproducible CMake dependency refs");
    check(readme.find(
              "`exon outdated [pkg...] [--member a,b] [--exclude x,y] [--output human\\|json]`") !=
              std::string::npos,
          "README documents outdated command");
    check(readme.find(
              "`exon update [pkg...] [--dry-run] [--precise <version>] [--member a,b] [--exclude x,y]`") !=
              std::string::npos,
          "README documents update package options");
    check(readme.find(
              "`exon tree [--member a,b] [--exclude x,y] [--dev] [--features] [--output human\\|json]`") !=
              std::string::npos,
          "README documents tree command");
    check(readme.find(
              "`exon why <pkg> [--member a,b] [--exclude x,y] [--dev] [--output human\\|json]`") !=
              std::string::npos,
          "README documents why command");
    check(readme.find("`exon version`") != std::string::npos,
          "README documents version command");
    check(readme.find("`os` = `linux`, `macos`, `windows`, `wasi`, `android`") !=
              std::string::npos,
          "README documents known platform OS values");
    check(readme.find("`arch` =\n`x86_64`, `aarch64`, `wasm32`") !=
              std::string::npos,
          "README documents known platform arch values");
    check(readme.find("exon build --target aarch64-linux-android") !=
              std::string::npos,
          "README documents Android build target");
    check(readme.find("exon test --target aarch64-linux-android") !=
              std::string::npos,
          "README documents Android build-only tests");
    check(readme.find("default to `human` output") != std::string::npos,
          "README documents human default");
    check(readme.find("interactive terminal") != std::string::npos,
          "README documents interactive live progress");
    check(readme.find("active phase label shimmers from left to right") !=
              std::string::npos,
          "README documents progress label shimmer");
    check(readme.find("bright-white highlight") != std::string::npos,
          "README documents bright-white progress highlight");
    check(readme.find("latest CMake/Ninja output lines") != std::string::npos,
          "README documents live tool output tail");
    check(readme.find("dim styling") != std::string::npos,
          "README documents dim live output preview");
    check(readme.find("When stdout is not a TTY") != std::string::npos,
          "README documents non-tty fallback");
    check(readme.find("NO_COLOR=1") != std::string::npos,
          "README documents NO_COLOR override");
    check(readme.find("EXON_PROGRESS=never") != std::string::npos,
          "README documents progress capability env");
    check(readme.find("JSON Lines") != std::string::npos,
          "README documents JSON Lines output");
    check(readme.find("[4/5] [hello (apps/hello)] build") != std::string::npos,
          "README documents workspace member stage labels");
    check(readme.find("`wrapped` adds the same command framing while still showing the underlying "
                      "CMake/Ninja/test output") != std::string::npos,
          "README documents wrapped behavior");
    check(readme.find("`raw` keeps exon wrapping to a minimum") != std::string::npos,
          "README documents raw behavior");
    check(readme.find("`exon test --show-output failed` (default)") != std::string::npos,
          "README documents failed-only default");
    check(readme.find("[--output raw\\|wrapped]") == std::string::npos,
          "README removes raw wrapped-only command docs");
    check(readme.find("default to a wrapped console") == std::string::npos,
          "README removes wrapped default wording");
}

int main() {
    std::println("test_cli:");

    test_usage_alignment();
    test_usage_wrap();
    test_usage_empty_description();
    test_parse_flag();
    test_parse_option();
    test_parse_list_option();
    test_parse_positional();
    test_parse_separator();
    test_parse_unknown_flag();
    test_parse_missing_value();
    test_parse_mixed();
    test_parse_debugger_option_with_separator();
    test_commands_usage_lists_debug();
    test_commands_usage_lists_human_output_mode();
    test_commands_reporting_defaults();
    test_commands_reporting_capabilities();
    test_commands_suggest_unknown_command();
    test_readme_output_docs_match_usage();

    if (failures > 0) {
        std::println("test_cli: {} FAILED", failures);
        return 1;
    }
    std::println("test_cli: all passed");
    return 0;
}
