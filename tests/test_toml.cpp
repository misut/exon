import std;
import toml;

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

void test_parse_error() {
    bool caught = false;
    try {
        toml::parse("invalid = ");
    } catch (toml::ParseError const&) {
        caught = true;
    }
    check(caught, "parse error on invalid input");
}

int main() {
    test_key_value();
    test_table();
    test_array();
    test_array_of_tables();
    test_string_escapes();
    test_comments();
    test_parse_error();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_toml: all passed");
    return 0;
}
