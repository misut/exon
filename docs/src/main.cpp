import phenotype;
using namespace phenotype;

namespace {

void CommandRow(str cmd, str desc) {
    Box([cmd, desc] {
        auto tr = create_element("tr");
        append_child(Scope::current()->handle, tr);
        Scope row_scope(tr);
        auto* prev = Scope::current();
        Scope::set_current(&row_scope);

        Box([cmd] {
            auto td = create_element("td");
            append_child(Scope::current()->handle, td);
            Scope td_scope(td);
            auto* p = Scope::current();
            Scope::set_current(&td_scope);
            Box([cmd] {
                auto code = create_element("code");
                set_text(code, cmd);
                append_child(Scope::current()->handle, code);
            });
            Scope::set_current(p);
        });

        Box([desc] {
            auto td = create_element("td");
            set_text(td, desc);
            append_child(Scope::current()->handle, td);
        });

        Scope::set_current(prev);
    });
}

} // namespace

void DocsApp() {
    // Header
    Box([&] {
        add_class(Scope::current()->handle, "hero");

        Box([&] {
            auto h1 = create_element("h1");
            set_text(h1, "exon");
            append_child(Scope::current()->handle, h1);
        });

        Box([&] {
            auto p = create_element("p");
            set_text(p, "A modern C++ package manager inspired by Cargo");
            add_class(p, "subtitle");
            append_child(Scope::current()->handle, p);
        });
    });

    // Main content wrapper
    Box([&] {
        add_class(Scope::current()->handle, "content");

        // Features
        Box([&] {
            auto h2 = create_element("h2");
            set_text(h2, "Features");
            append_child(Scope::current()->handle, h2);

            auto ul = create_element("ul");
            append_child(Scope::current()->handle, ul);

            char const* items[] = {
                "C++23 modules (import std;)",
                "Automatic CMakeLists.txt generation",
                "Git-based dependency management with lockfile",
                "WebAssembly cross-compilation (wasm32-wasi)",
                "Workspace support for monorepos",
                "vcpkg integration",
                "Platform-conditional dependencies",
            };
            for (auto const* item : items) {
                auto li = create_element("li");
                set_text(li, str(item, 0));
                // compute length
                unsigned int len = 0;
                while (item[len]) ++len;
                set_text(li, str(item, len));
                append_child(ul, li);
            }
        });

        // Quick Start
        Box([&] {
            auto h2 = create_element("h2");
            set_text(h2, "Quick Start");
            append_child(Scope::current()->handle, h2);

            Text("Install exon, create a project, and run it:");

            Box([&] {
                auto pre = create_element("pre");
                add_class(pre, "code-block");
                append_child(Scope::current()->handle, pre);

                auto code = create_element("code");
                set_text(code,
                    "# Install via mise\n"
                    "mise use -g vfox:misut/mise-exon@latest\n"
                    "\n"
                    "# Create a new project\n"
                    "exon init hello\n"
                    "cd hello\n"
                    "\n"
                    "# Build and run\n"
                    "exon run");
                append_child(pre, code);
            });
        });

        // Commands table
        Box([&] {
            auto h2 = create_element("h2");
            set_text(h2, "Commands");
            append_child(Scope::current()->handle, h2);

            auto table = create_element("table");
            append_child(Scope::current()->handle, table);

            // thead
            auto thead = create_element("thead");
            append_child(table, thead);
            auto thead_tr = create_element("tr");
            append_child(thead, thead_tr);
            auto th1 = create_element("th");
            set_text(th1, "Command");
            append_child(thead_tr, th1);
            auto th2 = create_element("th");
            set_text(th2, "Description");
            append_child(thead_tr, th2);

            // tbody
            auto tbody = create_element("tbody");
            append_child(table, tbody);

            struct CmdEntry { str cmd; str desc; };
            CmdEntry entries[] = {
                {"exon init [name]",    "Create a new project"},
                {"exon build",          "Build the project"},
                {"exon run",            "Build and run"},
                {"exon test",           "Build and run tests"},
                {"exon check",          "Check syntax without linking"},
                {"exon add <pkg> <ver>","Add a dependency"},
                {"exon remove <pkg>",   "Remove a dependency"},
                {"exon update",         "Update dependencies"},
                {"exon sync",           "Sync CMakeLists.txt"},
                {"exon clean",          "Remove build artifacts"},
                {"exon fmt",            "Format source files"},
            };
            for (auto const& e : entries) {
                auto tr = create_element("tr");
                append_child(tbody, tr);
                auto td1 = create_element("td");
                append_child(tr, td1);
                auto code = create_element("code");
                set_text(code, e.cmd);
                append_child(td1, code);
                auto td2 = create_element("td");
                set_text(td2, e.desc);
                append_child(tr, td2);
            }
        });
    });

    // Footer
    Box([&] {
        auto footer = create_element("footer");
        append_child(Scope::current()->handle, footer);
        auto p = create_element("p");
        set_inner_html(p,
            "Built with <a href=\"https://github.com/misut/phenotype\">phenotype</a>"
            " \xc2\xb7 <a href=\"https://github.com/misut/exon\">GitHub</a>");
        append_child(footer, p);
    });
}

int main() {
    express(DocsApp);
    return 0;
}
