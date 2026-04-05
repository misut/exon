import std;
import toml;

#if defined(_WIN32)
// Disable Windows crash dialogs so failures surface as exit codes instead of blocking UI.
extern "C" unsigned int __stdcall SetErrorMode(unsigned int);
extern "C" int _set_abort_behavior(unsigned int, unsigned int);
static int _crash_suppression = []() {
    SetErrorMode(0x0001u | 0x0002u);
    _set_abort_behavior(0, 0x1u | 0x4u);
    return 0;
}();
#endif

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

void test_key_value() {
    auto t = toml::parse(R"(
name = "hello"
version = "1.0.0"
count = 42
pi = 3.14
enabled = true
)");
    check(t.at("name").as_string() == "hello", "string value");
    check(t.at("version").as_string() == "1.0.0", "string with dots");
    check(t.at("count").as_integer() == 42, "integer");
    check(t.at("pi").as_float() > 3.13 && t.at("pi").as_float() < 3.15, "float");
    check(t.at("enabled").as_bool() == true, "bool true");
}

void test_table() {
    auto t = toml::parse(R"(
[package]
name = "test"
version = "0.1.0"

[dependencies]
"github.com/user/repo" = "1.0.0"
)");
    check(t.contains("package"), "table exists");
    check(t["package"]["name"].as_string() == "test", "nested value");
    check(t["dependencies"]["github.com/user/repo"].as_string() == "1.0.0", "quoted key");
}

void test_array() {
    auto t = toml::parse(R"(
names = ["alice", "bob", "carol"]
numbers = [1, 2, 3]
)");
    auto const& names = t.at("names").as_array();
    check(names.size() == 3, "array size");
    check(names[0].as_string() == "alice", "array[0]");
    check(names[2].as_string() == "carol", "array[2]");

    auto const& nums = t.at("numbers").as_array();
    check(nums[1].as_integer() == 2, "int array[1]");
}

void test_array_of_tables() {
    auto t = toml::parse(R"(
[[package]]
name = "foo"
version = "1.0.0"

[[package]]
name = "bar"
version = "2.0.0"
)");
    auto const& pkgs = t.at("package").as_array();
    check(pkgs.size() == 2, "array of tables size");
    check(pkgs[0].as_table().at("name").as_string() == "foo", "first table name");
    check(pkgs[1].as_table().at("version").as_string() == "2.0.0", "second table version");
}

void test_string_escapes() {
    auto t = toml::parse(R"(
basic = "hello\nworld"
literal = 'no\escape'
)");
    check(t.at("basic").as_string() == "hello\nworld", "basic string escape");
    check(t.at("literal").as_string() == "no\\escape", "literal string no escape");
}

void test_comments() {
    auto t = toml::parse(R"(
# this is a comment
name = "test" # inline comment
)");
    check(t.at("name").as_string() == "test", "value with comments");
}

// NOTE: cross-library exception catch is unreliable with add_subdirectory deps
// toml::ParseError thrown from tomlcpp cannot be caught here due to RTTI mismatch
// This is tested indirectly through exon's own error handling

int main() {
    test_key_value();
    test_table();
    test_array();
    test_array_of_tables();
    test_string_escapes();
    test_comments();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_toml: all passed");
    return 0;
}
