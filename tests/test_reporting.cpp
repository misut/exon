import std;
import reporting;

int failures = 0;

void check(bool cond, std::string_view msg) {
    if (!cond) {
        std::println(std::cerr, "  FAIL: {}", msg);
        ++failures;
    }
}

void test_parse_output_mode() {
    check(reporting::parse_output_mode("raw") == reporting::OutputMode::raw,
          "parse output raw");
    check(reporting::parse_output_mode("wrapped") == reporting::OutputMode::wrapped,
          "parse output wrapped");
    check(!reporting::parse_output_mode("human").has_value(),
          "parse output rejects human");
    check(!reporting::parse_output_mode("verbose").has_value(),
          "parse output rejects unknown");
}

void test_parse_show_output() {
    check(reporting::parse_show_output("failed") == reporting::ShowOutput::failed,
          "parse show_output failed");
    check(reporting::parse_show_output("all") == reporting::ShowOutput::all,
          "parse show_output all");
    check(reporting::parse_show_output("none") == reporting::ShowOutput::none,
          "parse show_output none");
    check(!reporting::parse_show_output("summary").has_value(),
          "parse show_output rejects unknown");
}

void test_stream_mode_for_output_mode() {
    check(reporting::stream_mode_for(reporting::OutputMode::raw) ==
              reporting::StreamMode::passthrough,
          "raw uses passthrough");
    check(reporting::stream_mode_for(reporting::OutputMode::wrapped) ==
              reporting::StreamMode::tee,
          "wrapped uses tee");
}

void test_should_show_output() {
    check(reporting::should_show_output(reporting::ShowOutput::all, false),
          "show_output all shows passing output");
    check(reporting::should_show_output(reporting::ShowOutput::failed, true),
          "show_output failed shows failure output");
    check(!reporting::should_show_output(reporting::ShowOutput::failed, false),
          "show_output failed hides passing output");
    check(!reporting::should_show_output(reporting::ShowOutput::none, true),
          "show_output none hides failure output");
}

void test_format_duration() {
    check(reporting::format_duration(std::chrono::milliseconds{42}) == "42ms",
          "format short duration");
    check(reporting::format_duration(std::chrono::milliseconds{1250}) == "1.25s",
          "format second duration");
}

int main() {
    std::cout << "test_reporting:\n";
    test_parse_output_mode();
    test_parse_show_output();
    test_stream_mode_for_output_mode();
    test_should_show_output();
    test_format_duration();

    if (failures > 0) {
        std::println("test_reporting: {} FAILED", failures);
        return 1;
    }

    std::cout << "test_reporting: all passed\n";
    return 0;
}
