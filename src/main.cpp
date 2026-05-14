#include <cstdio> // stdout/stderr/_IONBF macros (not exported by import std;)
import std;
import commands;
import reporting.system;

int main(int argc, char* argv[]) {
    // Disable stdout/stderr buffering so prints from exon appear in the
    // correct order relative to child-process output from std::system.
    // Without this, on Windows block-buffered stdout leaves "fetching..."
    // / "building..." etc. queued up until after cmake/ninja have finished
    // writing to the terminal.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    reporting::system::enable_vt_on_windows();

    if (argc < 2) {
        std::print("{}", commands::usage_text());
        return 1;
    }

    std::string_view command{argv[1]};

    if (command == "version" || command == "--version" || command == "-v")
        return commands::cmd_version();
    if (command == "init")
        return commands::cmd_init(argc, argv);
    if (command == "new")
        return commands::cmd_new(argc, argv);
    if (command == "info")
        return commands::cmd_info();
    if (command == "status" || command == "doctor")
        return commands::cmd_status(argc, argv);
    if (command == "build")
        return commands::cmd_build(argc, argv);
    if (command == "dist")
        return commands::cmd_dist(argc, argv);
    if (command == "check")
        return commands::cmd_check(argc, argv);
    if (command == "run")
        return commands::cmd_run(argc, argv);
    if (command == "debug")
        return commands::cmd_debug(argc, argv);
    if (command == "test")
        return commands::cmd_test(argc, argv);
    if (command == "clean")
        return commands::cmd_clean(argc, argv);
    if (command == "add")
        return commands::cmd_add(argc, argv);
    if (command == "remove")
        return commands::cmd_remove(argc, argv);
    if (command == "outdated")
        return commands::cmd_outdated(argc, argv);
    if (command == "update")
        return commands::cmd_update(argc, argv);
    if (command == "tree")
        return commands::cmd_tree(argc, argv);
    if (command == "why")
        return commands::cmd_why(argc, argv);
    if (command == "sync")
        return commands::cmd_sync(argc, argv);
    if (command == "fmt")
        return commands::cmd_fmt();

    auto rc = commands::unknown_command(command);
    std::print(std::cerr, "{}", commands::usage_text());
    return rc;
}
