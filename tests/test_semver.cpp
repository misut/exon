import std;
import semver;

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

    auto v5 = semver::parse("1.2.3-alpha.1+build.5");
    check(v5.major == 1, "parse prerelease major");
    check(v5.prerelease == "alpha.1", "parse prerelease identifier");
}

void test_compare() {
    check(semver::parse("1.0.0") < semver::parse("2.0.0"), "1.0.0 < 2.0.0");
    check(semver::parse("1.0.0") < semver::parse("1.1.0"), "1.0.0 < 1.1.0");
    check(semver::parse("1.0.0") < semver::parse("1.0.1"), "1.0.0 < 1.0.1");
    check(semver::parse("1.0.0") == semver::parse("1.0.0"), "1.0.0 == 1.0.0");
    check(semver::parse("2.0.0") > semver::parse("1.9.9"), "2.0.0 > 1.9.9");
    check(semver::parse("1.0.0-alpha") < semver::parse("1.0.0"),
          "prerelease sorts before stable");
    check(semver::parse("1.0.0-alpha.2") < semver::parse("1.0.0-alpha.10"),
          "numeric prerelease identifiers compare numerically");
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

    auto r3 = semver::parse_range("^0.0.3");
    check(semver::satisfies(semver::parse("0.0.3"), r3), "^0.0.3 satisfies 0.0.3");
    check(!semver::satisfies(semver::parse("0.0.4"), r3), "^0.0.3 rejects 0.0.4");
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

    auto r2 = semver::parse_range(">=1.0.0 <2.0.0");
    check(semver::satisfies(semver::parse("1.5.0"), r2),
          "space-separated comparisons satisfy 1.5.0");
    check(!semver::satisfies(semver::parse("2.0.0"), r2),
          "space-separated comparisons reject 2.0.0");
}

void test_bare_version() {
    // bare 1.2.3 → ^1.2.3
    auto r = semver::parse_range("1.2.3");
    check(semver::satisfies(semver::parse("1.2.3"), r), "bare 1.2.3 satisfies 1.2.3");
    check(semver::satisfies(semver::parse("1.9.0"), r), "bare 1.2.3 satisfies 1.9.0");
    check(!semver::satisfies(semver::parse("2.0.0"), r), "bare 1.2.3 rejects 2.0.0");
}

void test_exact_range() {
    auto r = semver::parse_range("=1.2.3");
    check(semver::satisfies(semver::parse("1.2.3"), r), "exact satisfies equal version");
    check(!semver::satisfies(semver::parse("1.2.4"), r), "exact rejects patch bump");
}

void test_wildcard_range() {
    auto any = semver::parse_range("*");
    check(semver::satisfies(semver::parse("9.9.9"), any), "wildcard * accepts any stable");

    auto major = semver::parse_range("1.*");
    check(semver::satisfies(semver::parse("1.9.0"), major), "1.* accepts 1.x");
    check(!semver::satisfies(semver::parse("2.0.0"), major), "1.* rejects 2.x");

    auto minor = semver::parse_range("1.2.x");
    check(semver::satisfies(semver::parse("1.2.9"), minor), "1.2.x accepts patch");
    check(!semver::satisfies(semver::parse("1.3.0"), minor), "1.2.x rejects next minor");
}

void test_prerelease_range() {
    auto stable_req = semver::parse_range("1.0.0");
    check(!semver::satisfies(semver::parse("1.1.0-alpha.1"), stable_req),
          "stable requirement rejects prerelease candidates");

    auto prerelease_req = semver::parse_range(">=1.0.0-alpha.1, <1.0.0");
    check(semver::satisfies(semver::parse("1.0.0-alpha.2"), prerelease_req),
          "prerelease requirement accepts prerelease candidates");
}

int main() {
    test_parse();
    test_compare();
    test_caret_range();
    test_tilde_range();
    test_explicit_range();
    test_bare_version();
    test_exact_range();
    test_wildcard_range();
    test_prerelease_range();

    if (failures > 0) {
        std::println("{} test(s) failed", failures);
        return 1;
    }
    std::println("test_semver: all passed");
    return 0;
}
