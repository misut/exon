import phenotype;

namespace docs {

// exon's docs site is non-interactive — no buttons, no text fields,
// no state mutations — so the message DSL is degenerate: empty State,
// empty Msg, no-op update.
struct State {};
struct Msg {};

void update(State&, Msg) {}

void view(State const&) {
    using namespace phenotype;
    layout::scaffold(
        // Hero
        [&] {
            widget::text("exon");
            widget::text("A modern C++ package manager inspired by Cargo");
        },
        // Content
        [&] {
            // Features
            layout::column([&] {
                widget::text("Features");
                layout::list_items([&] {
                    layout::item("C++23 modules (import std;)");
                    layout::item("Automatic CMakeLists.txt generation");
                    layout::item("Git-based dependency management with lockfile");
                    layout::item("WebAssembly cross-compilation (wasm32-wasi)");
                    layout::item("Workspace support for monorepos");
                    layout::item("vcpkg integration");
                    layout::item("Platform-conditional dependencies");
                });
            });

            // Quick Start
            layout::column([&] {
                widget::text("Quick Start");
                widget::text("Install exon, create a project, and run it:");
                widget::code(
                    "# Install via mise\n"
                    "mise use -g vfox:misut/mise-exon@latest\n"
                    "\n"
                    "# Create a new project\n"
                    "exon init hello\n"
                    "cd hello\n"
                    "\n"
                    "# Build and run\n"
                    "exon run"
                );
            });

            layout::divider();

            // Commands
            layout::column([&] {
                widget::text("Commands");
                layout::row([&] { widget::code("exon init [name]");     widget::text("Create a new project"); });
                layout::row([&] { widget::code("exon build");           widget::text("Build the project"); });
                layout::row([&] { widget::code("exon run");             widget::text("Build and run"); });
                layout::row([&] { widget::code("exon test");            widget::text("Build and run tests"); });
                layout::row([&] { widget::code("exon check");           widget::text("Check syntax without linking"); });
                layout::row([&] { widget::code("exon add <pkg> <ver>"); widget::text("Add a dependency"); });
                layout::row([&] { widget::code("exon remove <pkg>");    widget::text("Remove a dependency"); });
                layout::row([&] { widget::code("exon update");          widget::text("Update dependencies"); });
                layout::row([&] { widget::code("exon sync");            widget::text("Sync CMakeLists.txt"); });
                layout::row([&] { widget::code("exon clean");           widget::text("Remove build artifacts"); });
                layout::row([&] { widget::code("exon fmt");             widget::text("Format source files"); });
            });
        },
        // Footer
        [&] {
            layout::row(
                [&] { widget::text("Built with "); },
                [&] { widget::link("phenotype", "https://github.com/misut/phenotype"); },
                [&] { widget::text(" · "); },
                [&] { widget::link("GitHub", "https://github.com/misut/exon"); }
            );
        }
    );
}

} // namespace docs

int main() {
    phenotype::run<docs::State, docs::Msg>(docs::view, docs::update);
    return 0;
}
