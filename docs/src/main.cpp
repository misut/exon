import phenotype;

namespace {

// helper: write a command with string literal
unsigned int el(char const* tag) {
    unsigned int len = 0;
    while (tag[len]) ++len;
    return phenotype::create_element(tag, len);
}

void text(unsigned int h, char const* s) {
    unsigned int len = 0;
    while (s[len]) ++len;
    phenotype::set_text(h, s, len);
}

void cls(unsigned int h, char const* c) {
    unsigned int len = 0;
    while (c[len]) ++len;
    phenotype::add_class(h, c, len);
}

void style(unsigned int h, char const* prop, char const* val) {
    unsigned int plen = 0, vlen = 0;
    while (prop[plen]) ++plen;
    while (val[vlen]) ++vlen;
    phenotype::set_style(h, prop, plen, val, vlen);
}

void html(unsigned int h, char const* s) {
    unsigned int len = 0;
    while (s[len]) ++len;
    phenotype::set_inner_html(h, s, len);
}

void attr(unsigned int h, char const* key, char const* val) {
    unsigned int klen = 0, vlen = 0;
    while (key[klen]) ++klen;
    while (val[vlen]) ++vlen;
    phenotype::set_attribute(h, key, klen, val, vlen);
}

void section_heading(char const* title) {
    auto h2 = el("h2");
    text(h2, title);
    cls(h2, "section-title");
    phenotype::append_child(0, h2);
}

void code_block(char const* code) {
    auto pre = el("pre");
    auto c = el("code");
    text(c, code);
    phenotype::append_child(pre, c);
    cls(pre, "code-block");
    phenotype::append_child(0, pre);
}

void paragraph(char const* content) {
    auto p = el("p");
    text(p, content);
    phenotype::append_child(0, p);
}

void command_row(unsigned int tbody, char const* cmd, char const* desc) {
    auto tr = el("tr");
    auto td1 = el("td");
    auto code = el("code");
    text(code, cmd);
    phenotype::append_child(td1, code);
    phenotype::append_child(tr, td1);
    auto td2 = el("td");
    text(td2, desc);
    phenotype::append_child(tr, td2);
    phenotype::append_child(tbody, tr);
}

} // namespace

int main() {
    // --- Header ---
    auto header = el("header");
    cls(header, "hero");

    auto h1 = el("h1");
    text(h1, "exon");
    phenotype::append_child(header, h1);

    auto subtitle = el("p");
    text(subtitle, "A modern C++ package manager inspired by Cargo");
    cls(subtitle, "subtitle");
    phenotype::append_child(header, subtitle);

    phenotype::append_child(0, header);

    // --- Main content ---
    auto main_el = el("main");
    cls(main_el, "content");
    phenotype::append_child(0, main_el);
    phenotype::flush();

    // Use handle 0 = main from now (trick: we can't reassign 0,
    // so we flush header to body, then append rest to main_el)

    // --- Features ---
    auto features_sec = el("section");

    auto feat_h2 = el("h2");
    text(feat_h2, "Features");
    phenotype::append_child(features_sec, feat_h2);

    auto ul = el("ul");

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
        auto li = el("li");
        text(li, item);
        phenotype::append_child(ul, li);
    }

    phenotype::append_child(features_sec, ul);
    phenotype::append_child(main_el, features_sec);
    phenotype::flush();

    // --- Quick Start ---
    auto qs_sec = el("section");

    auto qs_h2 = el("h2");
    text(qs_h2, "Quick Start");
    phenotype::append_child(qs_sec, qs_h2);

    auto qs_p = el("p");
    text(qs_p, "Install exon, create a project, and run it:");
    phenotype::append_child(qs_sec, qs_p);

    auto pre = el("pre");
    auto code_el = el("code");
    text(code_el,
        "# Install via mise\n"
        "mise use -g vfox:misut/mise-exon@latest\n"
        "\n"
        "# Create a new project\n"
        "exon init hello\n"
        "cd hello\n"
        "\n"
        "# Build and run\n"
        "exon run");
    phenotype::append_child(pre, code_el);
    cls(pre, "code-block");
    phenotype::append_child(qs_sec, pre);
    phenotype::append_child(main_el, qs_sec);
    phenotype::flush();

    // --- Commands ---
    auto cmd_sec = el("section");

    auto cmd_h2 = el("h2");
    text(cmd_h2, "Commands");
    phenotype::append_child(cmd_sec, cmd_h2);

    auto table = el("table");
    auto thead = el("thead");
    auto thead_tr = el("tr");
    auto th1 = el("th");
    text(th1, "Command");
    phenotype::append_child(thead_tr, th1);
    auto th2 = el("th");
    text(th2, "Description");
    phenotype::append_child(thead_tr, th2);
    phenotype::append_child(thead, thead_tr);
    phenotype::append_child(table, thead);

    auto tbody = el("tbody");
    command_row(tbody, "exon init [name]", "Create a new project");
    command_row(tbody, "exon build", "Build the project");
    command_row(tbody, "exon run", "Build and run");
    command_row(tbody, "exon test", "Build and run tests");
    command_row(tbody, "exon check", "Check syntax without linking");
    command_row(tbody, "exon add <pkg> <ver>", "Add a dependency");
    command_row(tbody, "exon remove <pkg>", "Remove a dependency");
    command_row(tbody, "exon update", "Update dependencies");
    command_row(tbody, "exon sync", "Sync CMakeLists.txt");
    command_row(tbody, "exon clean", "Remove build artifacts");
    command_row(tbody, "exon fmt", "Format source files");
    phenotype::append_child(table, tbody);
    phenotype::append_child(cmd_sec, table);
    phenotype::append_child(main_el, cmd_sec);
    phenotype::flush();

    // --- Footer ---
    auto footer = el("footer");
    auto footer_p = el("p");
    html(footer_p,
        "Built with <a href=\"https://github.com/misut/phenotype\">phenotype</a>"
        " · <a href=\"https://github.com/misut/exon\">GitHub</a>");
    phenotype::append_child(footer, footer_p);
    phenotype::append_child(0, footer);
    phenotype::flush();

    return 0;
}
