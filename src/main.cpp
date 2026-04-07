#include <cstdio> // stdout/stderr/_IONBF macros (not exported by import std;)
import std;
import commands;

int main(int argc, char* argv[]) {
    // Disable stdout/stderr buffering so prints from exon appear in the
    // correct order relative to child-process output from std::system.
    // Without this, on Windows block-buffered stdout leaves "fetching..."
    // / "building..." etc. queued up until after cmake/ninja have finished
    // writing to the terminal.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc < 2) {
        std::print("{}", commands::usage_text());
        return 1;
    }

    std::string_view command{argv[1]};

    if (command == "version" || command == "--version" || command == "-v")
        return commands::cmd_version();
    if (command == "init")
        return commands::cmd_init(argc, argv);
    if (command == "info")
        return commands::cmd_info();
    if (command == "build")
        return commands::cmd_build(argc, argv);
    if (command == "check")
        return commands::cmd_check(argc, argv);
    if (command == "run")
        return commands::cmd_run(argc, argv);
    if (command == "test")
        return commands::cmd_test(argc, argv);
    if (command == "clean")
        return commands::cmd_clean();
    if (command == "add")
        return commands::cmd_add(argc, argv);
    if (command == "remove")
        return commands::cmd_remove(argc, argv);
    if (command == "update")
        return commands::cmd_update();
    if (command == "sync")
        return commands::cmd_sync();
    if (command == "fmt")
        return commands::cmd_fmt();

    std::println(std::cerr, "unknown command: {}", command);
    std::print(std::cerr, "{}", commands::usage_text());
    return 1;
}
