import std;
import lock;
import lock.system;

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

std::string read_file_bytes(std::filesystem::path const& path) {
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("failed to read {}", path.string()));
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void write_file_bytes(std::filesystem::path const& path, std::string_view content) {
    auto file = std::ofstream(path, std::ios::binary);
    if (!file)
        throw std::runtime_error(std::format("failed to write {}", path.string()));
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file)
        throw std::runtime_error(std::format("failed to write {}", path.string()));
}

void test_empty_lockfile() {
    lock::LockFile lf;
    check(lf.packages.empty(), "empty lockfile has no packages");
    check(!lf.contains("foo", "1.0.0"), "empty lockfile contains nothing");
    check(lf.find("foo", "1.0.0") == nullptr, "empty lockfile find returns nullptr");
}

void test_add_or_update() {
    lock::LockFile lf;

    lf.add_or_update({.name = "pkg-a", .package = "alpha", .version = "1.0.0", .commit = "aaa"});
    check(lf.packages.size() == 1, "one package after add");
    check(lf.contains("pkg-a", "1.0.0"), "contains pkg-a");
    check(lf.find("pkg-a", "1.0.0")->commit == "aaa", "commit is aaa");
    check(lf.find("pkg-a", "1.0.0")->package == "alpha", "package is alpha");

    // update existing
    lf.add_or_update({.name = "pkg-a", .package = "alpha2", .version = "1.0.0", .commit = "bbb"});
    check(lf.packages.size() == 1, "still one package after update");
    check(lf.find("pkg-a", "1.0.0")->commit == "bbb", "commit updated to bbb");
    check(lf.find("pkg-a", "1.0.0")->package == "alpha2", "package updated to alpha2");

    // add different version
    lf.add_or_update({.name = "pkg-a", .version = "2.0.0", .commit = "ccc"});
    check(lf.packages.size() == 2, "two packages for different versions");
    check(lf.find("pkg-a", "2.0.0")->commit == "ccc", "v2 commit is ccc");

    // add different package
    lf.add_or_update({.name = "pkg-b", .version = "0.1.0", .commit = "ddd"});
    check(lf.packages.size() == 3, "three packages total");
}

void test_save_writes_canonical_lf_bytes() {
    auto tmp = std::filesystem::temp_directory_path() / "exon_test_lock_lf.toml";

    lock::LockFile lf;
    lf.add_or_update({
        .name = "github.com/user/repo",
        .package = "repo",
        .version = "1.0.0",
        .commit = "abc123",
    });
    lf.add_or_update({
        .name = "github.com/other/lib",
        .package = "lib",
        .version = "0.2.0",
        .commit = "def456",
    });

    auto rendered = lock::render(lf);
    lock::system::save(lf, tmp.string());
    auto saved = read_file_bytes(tmp);

    check(saved == rendered, "save writes canonical lockfile bytes");
    check(saved.find('\r') == std::string::npos, "save does not write CRLF line endings");

    std::filesystem::remove(tmp);
}

void test_load_save_load_roundtrip() {
    auto tmp = std::filesystem::temp_directory_path() / "exon_test_lock_roundtrip.toml";

    lock::LockFile expected;
    expected.add_or_update({
        .name = "github.com/user/repo",
        .package = "repo",
        .version = "1.0.0",
        .commit = "abc123",
    });
    expected.add_or_update({
        .name = "github.com/other/lib",
        .package = "lib",
        .version = "0.2.0",
        .commit = "def456",
    });

    auto rendered = lock::render(expected);
    write_file_bytes(tmp, rendered);

    auto loaded = lock::system::load(tmp.string());
    check(loaded.packages.size() == 2, "loaded 2 packages");
    check(loaded.contains("github.com/user/repo", "1.0.0"), "loaded contains repo");
    check(loaded.find("github.com/user/repo", "1.0.0")->commit == "abc123", "loaded commit");
    check(loaded.find("github.com/user/repo", "1.0.0")->package == "repo", "loaded package");
    check(loaded.contains("github.com/other/lib", "0.2.0"), "loaded contains lib");
    check(loaded.find("github.com/other/lib", "0.2.0")->commit == "def456", "loaded lib commit");

    lock::system::save(loaded, tmp.string());
    auto saved = read_file_bytes(tmp);
    check(saved == rendered, "load/save preserves canonical lockfile bytes");
    check(saved.find('\r') == std::string::npos, "load/save roundtrip keeps LF bytes");

    auto reloaded = lock::system::load(tmp.string());
    check(reloaded.packages.size() == 2, "reloaded 2 packages");
    check(reloaded.contains("github.com/user/repo", "1.0.0"), "reloaded contains repo");
    check(reloaded.find("github.com/user/repo", "1.0.0")->commit == "abc123",
          "reloaded repo commit");
    check(reloaded.find("github.com/user/repo", "1.0.0")->package == "repo",
          "reloaded repo package");
    check(reloaded.contains("github.com/other/lib", "0.2.0"), "reloaded contains lib");
    check(reloaded.find("github.com/other/lib", "0.2.0")->commit == "def456",
          "reloaded lib commit");

    std::filesystem::remove(tmp);
}

void test_render_and_parse_roundtrip() {
    lock::LockFile lf;
    lf.add_or_update({
        .name = "github.com/user/repo",
        .package = "repo",
        .version = "1.0.0",
        .commit = "abc123",
    });
    lf.add_or_update({
        .name = "github.com/misut/txn#refl",
        .package = "refl",
        .version = "0.1.0",
        .commit = "deadbeef",
        .subdir = "refl",
        .features = {"json", "yaml"},
    });

    auto rendered = lock::render(lf);
    auto parsed = lock::parse(rendered);

    check(parsed.packages.size() == 2, "render/parse roundtrip keeps package count");
    check(parsed.find("github.com/user/repo", "1.0.0") != nullptr,
          "render/parse roundtrip keeps git package");
    auto const* refl = parsed.find("github.com/misut/txn#refl", "0.1.0");
    check(refl != nullptr, "render/parse roundtrip keeps subdir package");
    check(refl && refl->package == "refl", "render/parse roundtrip keeps canonical package");
    check(refl && refl->subdir == "refl", "render/parse roundtrip keeps subdir");
    check(refl && refl->features.size() == 2, "render/parse roundtrip keeps features");
}

void test_subdir_and_features_roundtrip() {
    auto tmp = std::filesystem::temp_directory_path() / "exon_test_lock_subdir_features.toml";

    lock::LockFile expected;
    expected.add_or_update({
        .name = "github.com/misut/txn#refl",
        .package = "refl",
        .version = "0.1.0",
        .commit = "abc123",
        .subdir = "refl",
        .features = {"json", "yaml"},
    });
    expected.add_or_update({
        .name = "github.com/user/plain",
        .package = "plain",
        .version = "2.0.0",
        .commit = "xyz789",
    });

    auto rendered = lock::render(expected);
    write_file_bytes(tmp, rendered);

    auto loaded = lock::system::load(tmp.string());
    check(loaded.packages.size() == 2, "subdir/features: two entries loaded");
    auto const* refl = loaded.find("github.com/misut/txn#refl", "0.1.0");
    check(refl != nullptr, "subdir/features: composite-name entry found");
    check(refl && refl->package == "refl", "subdir/features: canonical package preserved");
    check(refl && refl->subdir == "refl", "subdir/features: subdir field preserved");
    check(refl && refl->commit == "abc123", "subdir/features: commit preserved");
    check(refl && refl->features.size() == 2, "subdir/features: feature count preserved");
    check(refl && refl->features[0] == "json", "subdir/features: first feature preserved");
    check(refl && refl->features[1] == "yaml", "subdir/features: second feature preserved");
    auto const* plain = loaded.find("github.com/user/plain", "2.0.0");
    check(plain != nullptr, "subdir/features: plain git entry found");
    check(plain && plain->package == "plain", "subdir/features: plain package preserved");
    check(plain && plain->subdir.empty(), "subdir/features: plain entry subdir empty");
    check(plain && plain->features.empty(), "subdir/features: plain entry features empty");

    lock::system::save(loaded, tmp.string());
    auto saved = read_file_bytes(tmp);
    check(saved == rendered, "subdir/features: save preserves canonical lockfile bytes");
    check(saved.find('\r') == std::string::npos, "subdir/features: save keeps LF bytes");

    auto reloaded = lock::system::load(tmp.string());
    auto const* refl_roundtrip = reloaded.find("github.com/misut/txn#refl", "0.1.0");
    check(refl_roundtrip != nullptr, "subdir/features: reloaded composite-name entry found");
    check(refl_roundtrip && refl_roundtrip->subdir == "refl",
          "subdir/features: reloaded subdir preserved");
    check(refl_roundtrip && refl_roundtrip->features.size() == 2,
          "subdir/features: reloaded features preserved");

    std::filesystem::remove(tmp);
}

void test_load_nonexistent() {
    auto lf = lock::system::load("/tmp/exon_nonexistent_lock.toml");
    check(lf.packages.empty(), "loading nonexistent file returns empty");
}

int main() {
    test_empty_lockfile();
    test_add_or_update();
    test_save_writes_canonical_lf_bytes();
    test_load_save_load_roundtrip();
    test_render_and_parse_roundtrip();
    test_subdir_and_features_roundtrip();
    test_load_nonexistent();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_lock: all passed");
    return 0;
}
