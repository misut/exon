import phenotype;
using namespace phenotype;

void DocsApp() {
    Scaffold(
        // topBar
        [&] {
            Text("exon");
            Text("A modern C++ package manager inspired by Cargo");
        },
        // content
        [&] {
            // Features
            Column([&] {
                Text("Features");
                ListItems([&] {
                    Item("C++23 modules (import std;)");
                    Item("Automatic CMakeLists.txt generation");
                    Item("Git-based dependency management with lockfile");
                    Item("WebAssembly cross-compilation (wasm32-wasi)");
                    Item("Workspace support for monorepos");
                    Item("vcpkg integration");
                    Item("Platform-conditional dependencies");
                });
            });

            // Quick Start
            Column([&] {
                Text("Quick Start");
                Text("Install exon, create a project, and run it:");
                Code(
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

            Divider();

            // Commands
            Column([&] {
                Text("Commands");
                Row([&] { Code("exon init [name]");     Text("Create a new project"); });
                Row([&] { Code("exon build");           Text("Build the project"); });
                Row([&] { Code("exon run");             Text("Build and run"); });
                Row([&] { Code("exon test");            Text("Build and run tests"); });
                Row([&] { Code("exon check");           Text("Check syntax without linking"); });
                Row([&] { Code("exon add <pkg> <ver>"); Text("Add a dependency"); });
                Row([&] { Code("exon remove <pkg>");    Text("Remove a dependency"); });
                Row([&] { Code("exon update");          Text("Update dependencies"); });
                Row([&] { Code("exon sync");            Text("Sync CMakeLists.txt"); });
                Row([&] { Code("exon clean");           Text("Remove build artifacts"); });
                Row([&] { Code("exon fmt");             Text("Format source files"); });
            });
        },
        // bottomBar
        [&] {
            Row(
                [&] { Text("Built with "); },
                [&] { Link("phenotype", "https://github.com/misut/phenotype"); },
                [&] { Text(" · "); },
                [&] { Link("GitHub", "https://github.com/misut/exon"); }
            );
        }
    );
}

int main() {
    express(DocsApp);
    return 0;
}
