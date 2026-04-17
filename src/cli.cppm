export module cli;
import std;

export namespace cli {

// --- Usage formatter ---

struct Entry {
    std::string syntax;
    std::string description;
};

struct Section {
    std::string name;
    std::vector<Entry> entries;
};

std::string usage(std::string_view program, std::vector<Section> sections) {
    std::string out;
    out += std::format("usage: {} <command> [args]\n", program);

    for (auto const& sec : sections) {
        out += std::format("\n{}:\n", sec.name);

        // compute alignment: max syntax width that fits under cap
        constexpr std::size_t cap = 40;
        std::size_t align = 0;
        for (auto const& e : sec.entries) {
            if (e.syntax.size() <= cap && e.syntax.size() > align)
                align = e.syntax.size();
        }

        for (auto const& e : sec.entries) {
            if (e.description.empty()) {
                out += std::format("    {}\n", e.syntax);
                continue;
            }
            if (e.syntax.size() > align) {
                // wrap: syntax on one line, description indented on next
                out += std::format("    {}\n", e.syntax);
                out += std::format("{}   {}\n", std::string(4 + align, ' '), e.description);
            } else {
                auto pad = align - e.syntax.size();
                out += std::format("    {}{}   {}\n", e.syntax, std::string(pad, ' '), e.description);
            }
        }
    }
    return out;
}

// --- Argument parser ---

struct Flag { std::string name; };
struct Option { std::string name; };
struct ListOption { std::string name; };
using ArgDef = std::variant<Flag, Option, ListOption>;

struct Args {
    std::set<std::string> flags_;
    std::map<std::string, std::string> options_;
    std::map<std::string, std::vector<std::string>> lists_;
    std::vector<std::string> positional_;

    bool has(std::string_view name) const {
        return flags_.contains(std::string{name});
    }

    std::string_view get(std::string_view name) const {
        auto it = options_.find(std::string{name});
        return it != options_.end() ? std::string_view{it->second} : std::string_view{};
    }

    std::vector<std::string> const& get_list(std::string_view name) const {
        static std::vector<std::string> const empty;
        auto it = lists_.find(std::string{name});
        return it != lists_.end() ? it->second : empty;
    }

    std::vector<std::string> const& positional() const {
        return positional_;
    }
};

Args parse(int argc, char* argv[], int from, std::vector<ArgDef> defs) {
    // build lookup: name → index
    std::map<std::string, std::size_t> lookup;
    for (std::size_t i = 0; i < defs.size(); ++i)
        std::visit([&](auto const& d) { lookup[d.name] = i; }, defs[i]);

    Args result;
    int i = from;
    while (i < argc) {
        auto a = std::string_view{argv[i]};
        if (a == "--") {
            for (++i; i < argc; ++i)
                result.positional_.emplace_back(argv[i]);
            break;
        }
        if (a.starts_with("--")) {
            auto it = lookup.find(std::string{a});
            if (it == lookup.end())
                throw std::runtime_error(std::format("unknown flag '{}'", a));
            auto& def = defs[it->second];
            if (std::holds_alternative<Flag>(def)) {
                result.flags_.insert(std::string{a});
                ++i;
            } else if (std::holds_alternative<Option>(def)) {
                if (i + 1 >= argc)
                    throw std::runtime_error(std::format("{} requires a value", a));
                result.options_[std::string{a}] = argv[i + 1];
                i += 2;
            } else {
                // ListOption: CSV split
                if (i + 1 >= argc)
                    throw std::runtime_error(std::format("{} requires a value", a));
                auto& vec = result.lists_[std::string{a}];
                auto list = std::string_view{argv[i + 1]};
                std::size_t start = 0;
                while (start < list.size()) {
                    auto comma = list.find(',', start);
                    auto end = (comma == std::string_view::npos) ? list.size() : comma;
                    auto tok = list.substr(start, end - start);
                    while (!tok.empty() && tok.front() == ' ')
                        tok.remove_prefix(1);
                    while (!tok.empty() && tok.back() == ' ')
                        tok.remove_suffix(1);
                    if (!tok.empty())
                        vec.emplace_back(tok);
                    start = end + 1;
                }
                i += 2;
            }
        } else {
            result.positional_.emplace_back(a);
            ++i;
        }
    }
    return result;
}

} // namespace cli
