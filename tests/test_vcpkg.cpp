import std;
import vcpkg;
import manifest;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

void test_json_escape() {
    check(vcpkg::json_escape("fmt") == "\"fmt\"", "escape: plain");
    check(vcpkg::json_escape("a\"b") == "\"a\\\"b\"", "escape: quote");
    check(vcpkg::json_escape("a\\b") == "\"a\\\\b\"", "escape: backslash");
    check(vcpkg::json_escape("line\nbreak") == "\"line\\nbreak\"", "escape: newline");
}

void test_render_empty() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    auto s = vcpkg::render_manifest(m);
    check(s.contains("\"name\": \"app\""), "empty: name");
    check(s.contains("\"version-string\": \"0.1.0\""), "empty: version");
    check(s.contains("\"dependencies\": []"), "empty: no deps");
}

void test_render_wildcard() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.vcpkg_deps = {{"fmt", "*"}, {"zlib", "*"}};
    auto s = vcpkg::render_manifest(m);
    // wildcard → bare string in dependencies array
    check(s.contains("\"fmt\""), "wildcard: fmt bare");
    check(s.contains("\"zlib\""), "wildcard: zlib bare");
    // no version>= for wildcard
    check(!s.contains("version>="), "wildcard: no version constraint");
}

void test_render_pinned() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.vcpkg_deps = {{"fmt", "11.0.0"}};
    auto s = vcpkg::render_manifest(m);
    check(s.contains("\"name\": \"fmt\""), "pinned: fmt object");
    check(s.contains("\"version>=\": \"11.0.0\""), "pinned: version constraint");
}

void test_render_mixed_and_dev() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.vcpkg_deps = {{"fmt", "11.0.0"}, {"zlib", "*"}};
    m.dev_vcpkg_deps = {{"gtest", "*"}};
    auto s = vcpkg::render_manifest(m);
    check(s.contains("\"fmt\""), "mixed: fmt");
    check(s.contains("\"zlib\""), "mixed: zlib");
    check(s.contains("\"gtest\""), "mixed: dev gtest included");
    check(s.contains("\"version>=\": \"11.0.0\""), "mixed: pinned fmt");
}

void test_render_dev_overridden_by_regular() {
    manifest::Manifest m;
    m.name = "app";
    m.version = "1.0.0";
    m.vcpkg_deps = {{"fmt", "11.0.0"}};
    m.dev_vcpkg_deps = {{"fmt", "*"}};
    auto s = vcpkg::render_manifest(m);
    // regular wins: only 11.0.0 constraint
    check(s.contains("\"version>=\": \"11.0.0\""), "dedup: regular wins");
    // fmt appears exactly once as a dependency entry
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = s.find("\"fmt\"", pos)) != std::string::npos) {
        ++count;
        ++pos;
    }
    check(count == 1, "dedup: fmt appears once");
}

void test_write_manifest() {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "exon_test_vcpkg_write";
    fs::remove_all(tmp);

    manifest::Manifest m;
    m.name = "app";
    m.version = "0.1.0";
    m.vcpkg_deps = {{"fmt", "*"}};

    auto out = tmp / "sub" / "vcpkg.json";
    vcpkg::write_manifest(m, out);

    check(fs::exists(out), "write: file created");
    auto f = std::ifstream{out};
    std::string content{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
    check(content.contains("\"name\": \"app\""), "write: content contains name");
    check(content.contains("\"fmt\""), "write: content contains fmt");

    fs::remove_all(tmp);
}

void test_looks_like_root() {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "exon_test_vcpkg_looks";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "scripts" / "buildsystems");

    check(!vcpkg::looks_like_vcpkg_root(tmp), "looks: missing cmake file");

    {
        auto f = std::ofstream{tmp / "scripts" / "buildsystems" / "vcpkg.cmake"};
        f << "# fake";
    }
    check(vcpkg::looks_like_vcpkg_root(tmp), "looks: with cmake file");

    fs::remove_all(tmp);
}

int main() {
    test_json_escape();
    test_render_empty();
    test_render_wildcard();
    test_render_pinned();
    test_render_mixed_and_dev();
    test_render_dev_overridden_by_regular();
    test_write_manifest();
    test_looks_like_root();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_vcpkg: all passed");
    return 0;
}
