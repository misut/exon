import std;
import cli;
import commands;

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
    check(usage.find("debug [--release] [--debugger auto|lldb|gdb|devenv|cdb|<path>]") !=
              std::string::npos,
          "usage lists debug command");
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

    if (failures > 0) {
        std::println("test_cli: {} FAILED", failures);
        return 1;
    }
    std::println("test_cli: all passed");
    return 0;
}
