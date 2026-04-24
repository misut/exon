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
}

int main() {
    std::println("test_terminal:");
    test_style_disabled();
    test_status_cell_width();
    test_key_value();
    test_stage();
    test_progress_frame();

    if (failures > 0) {
        std::println("test_terminal: {} FAILED", failures);
        return 1;
    }
    std::println("test_terminal: all passed");
    return 0;
}
