import std;
import terminal;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

void test_style_disabled() {
    check(terminal::style("hello", terminal::StyleRole::accent, false) == "hello",
          "style disabled returns plain text");
}

void test_status_cell_width() {
    check(terminal::status_cell(terminal::StatusKind::ok, false) == "OK     ",
          "ok status cell padded");
    check(terminal::status_cell(terminal::StatusKind::timeout, false) == "TIMEOUT",
          "timeout status cell preserves width");
}

void test_key_value() {
    check(terminal::key_value("target", "native") == "  target     native",
          "key value uses stable label column");
}

void test_stage() {
    check(terminal::stage("build", 4, 5) == "[4/5] build",
          "stage formats indexed title");
    check(terminal::stage("build", 4, 5, "app (apps/app)") ==
              "[4/5] [app (apps/app)] build",
          "stage formats context inline");
}

void test_progress_frame() {
    auto frame = terminal::format_progress_frame({
        .done = 12,
        .total = 56,
        .percent = 21,
        .label = "build",
    }, 0, false);
    check(frame == "  RUN     [|] [12/56 21%] build",
          "progress frame uses status cell and spinner");

    auto detailed = terminal::format_progress_frame({
        .label = "discover",
        .detail = "  locked: github.com/misut/tomlcpp v0.4.0 (12345678)",
    }, 0, false);
    check(detailed ==
              "  RUN     [|] discover...\n"
              "  locked: github.com/misut/tomlcpp v0.4.0 (12345678)",
          "progress frame renders transient detail on the next line");
}

void test_progress_frame_with_detail_lines() {
    auto frame = terminal::format_progress_frame({
        .done = 12,
        .total = 56,
        .percent = 21,
        .label = "build",
        .detail_lines = {"Building CXX object foo.o", "Linking app"},
    }, 0, false);
    check(frame == "  RUN     [|] [12/56 21%] build\n"
                   "    Building CXX object foo.o\n"
                   "    Linking app",
          "progress frame appends detail lines");
}

std::string active_char(char ch) {
    return std::format("\x1b[1m\x1b[36m{}\x1b[0m", ch);
}

std::string dim_char(char ch) {
    return std::format("\x1b[2m{}\x1b[0m", ch);
}

void test_shimmer_label() {
    check(terminal::shimmer_label("build", 0, false) == "build",
          "shimmer label falls back when color is disabled");

    auto first = terminal::shimmer_label("build", 0, true);
    check(first == active_char('b') + active_char('u') + dim_char('i') +
                       dim_char('l') + dim_char('d'),
          "shimmer label highlights from the left");

    auto later = terminal::shimmer_label("build", 3, true);
    check(later == dim_char('b') + dim_char('u') + dim_char('i') +
                       active_char('l') + active_char('d'),
          "shimmer label moves right");
}

int main() {
    std::println("test_terminal:");
    test_style_disabled();
    test_status_cell_width();
    test_key_value();
    test_stage();
    test_progress_frame();
    test_progress_frame_with_detail_lines();
    test_shimmer_label();

    if (failures > 0) {
        std::println("test_terminal: {} FAILED", failures);
        return 1;
    }
    std::println("test_terminal: all passed");
    return 0;
}
