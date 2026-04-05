import std;
import semver;

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

void test_parse() {
    auto v = semver::parse("1.2.3");
    check(v.major == 1, "parse major");
    check(v.minor == 2, "parse minor");
    check(v.patch == 3, "parse patch");

    auto v2 = semver::parse("v0.10.5");
    check(v2.major == 0, "parse v-prefix major");
    check(v2.minor == 10, "parse v-prefix minor");
    check(v2.patch == 5, "parse v-prefix patch");

    auto v3 = semver::parse("2");
    check(v3.major == 2, "parse major only");
    check(v3.minor == 0, "parse major only minor=0");

    auto v4 = semver::parse("3.1");
    check(v4.major == 3, "parse major.minor major");
    check(v4.minor == 1, "parse major.minor minor");
    check(v4.patch == 0, "parse major.minor patch=0");
}

void test_compare() {
    check(semver::parse("1.0.0") < semver::parse("2.0.0"), "1.0.0 < 2.0.0");
    check(semver::parse("1.0.0") < semver::parse("1.1.0"), "1.0.0 < 1.1.0");
    check(semver::parse("1.0.0") < semver::parse("1.0.1"), "1.0.0 < 1.0.1");
    check(semver::parse("1.0.0") == semver::parse("1.0.0"), "1.0.0 == 1.0.0");
    check(semver::parse("2.0.0") > semver::parse("1.9.9"), "2.0.0 > 1.9.9");
}

void test_caret_range() {
    // ^1.2.3 → >=1.2.3, <2.0.0
    auto r = semver::parse_range("^1.2.3");
    check(semver::satisfies(semver::parse("1.2.3"), r), "^1.2.3 satisfies 1.2.3");
    check(semver::satisfies(semver::parse("1.9.9"), r), "^1.2.3 satisfies 1.9.9");
    check(!semver::satisfies(semver::parse("2.0.0"), r), "^1.2.3 rejects 2.0.0");
    check(!semver::satisfies(semver::parse("1.2.2"), r), "^1.2.3 rejects 1.2.2");

    // ^0.2.3 → >=0.2.3, <0.3.0
    auto r2 = semver::parse_range("^0.2.3");
    check(semver::satisfies(semver::parse("0.2.3"), r2), "^0.2.3 satisfies 0.2.3");
    check(semver::satisfies(semver::parse("0.2.9"), r2), "^0.2.3 satisfies 0.2.9");
    check(!semver::satisfies(semver::parse("0.3.0"), r2), "^0.2.3 rejects 0.3.0");
}

void test_tilde_range() {
    // ~1.2.3 → >=1.2.3, <1.3.0
    auto r = semver::parse_range("~1.2.3");
    check(semver::satisfies(semver::parse("1.2.3"), r), "~1.2.3 satisfies 1.2.3");
    check(semver::satisfies(semver::parse("1.2.9"), r), "~1.2.3 satisfies 1.2.9");
    check(!semver::satisfies(semver::parse("1.3.0"), r), "~1.2.3 rejects 1.3.0");
}

void test_explicit_range() {
    auto r = semver::parse_range(">=1.0.0, <2.0.0");
    check(semver::satisfies(semver::parse("1.0.0"), r), ">=1,<2 satisfies 1.0.0");
    check(semver::satisfies(semver::parse("1.5.0"), r), ">=1,<2 satisfies 1.5.0");
    check(!semver::satisfies(semver::parse("0.9.9"), r), ">=1,<2 rejects 0.9.9");
    check(!semver::satisfies(semver::parse("2.0.0"), r), ">=1,<2 rejects 2.0.0");
}

void test_bare_version() {
    // bare 1.2.3 → ^1.2.3
    auto r = semver::parse_range("1.2.3");
    check(semver::satisfies(semver::parse("1.2.3"), r), "bare 1.2.3 satisfies 1.2.3");
    check(semver::satisfies(semver::parse("1.9.0"), r), "bare 1.2.3 satisfies 1.9.0");
    check(!semver::satisfies(semver::parse("2.0.0"), r), "bare 1.2.3 rejects 2.0.0");
}

int main() {
    test_parse();
    test_compare();
    test_caret_range();
    test_tilde_range();
    test_explicit_range();
    test_bare_version();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_semver: all passed");
    return 0;
}
