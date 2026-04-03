export module semver;
import std;

export namespace semver {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;

    auto operator<=>(Version const&) const = default;
    bool operator==(Version const&) const = default;
};

Version parse(std::string_view str) {
    // strip leading 'v'
    if (!str.empty() && str[0] == 'v')
        str = str.substr(1);

    Version v;
    auto dot1 = str.find('.');
    if (dot1 == std::string_view::npos) {
        std::from_chars(str.data(), str.data() + str.size(), v.major);
        return v;
    }
    std::from_chars(str.data(), str.data() + dot1, v.major);

    auto rest = str.substr(dot1 + 1);
    auto dot2 = rest.find('.');
    if (dot2 == std::string_view::npos) {
        std::from_chars(rest.data(), rest.data() + rest.size(), v.minor);
        return v;
    }
    std::from_chars(rest.data(), rest.data() + dot2, v.minor);

    auto patch_str = rest.substr(dot2 + 1);
    std::from_chars(patch_str.data(), patch_str.data() + patch_str.size(), v.patch);
    return v;
}

std::string to_string(Version const& v) {
    return std::format("{}.{}.{}", v.major, v.minor, v.patch);
}

enum class Op { Eq, Lt, Le, Gt, Ge };

struct Constraint {
    Op op;
    Version ver;
};

struct Range {
    std::vector<Constraint> constraints;
};

namespace detail {

void skip_ws(std::string_view& s) {
    while (!s.empty() && s[0] == ' ')
        s = s.substr(1);
}

Constraint parse_constraint(std::string_view& s) {
    skip_ws(s);

    if (s.starts_with(">=")) {
        s = s.substr(2);
        skip_ws(s);
        auto comma = s.find(',');
        auto end = (comma != std::string_view::npos) ? comma : s.size();
        auto v = parse(s.substr(0, end));
        s = s.substr(end);
        return {Op::Ge, v};
    }
    if (s.starts_with("<=")) {
        s = s.substr(2);
        skip_ws(s);
        auto comma = s.find(',');
        auto end = (comma != std::string_view::npos) ? comma : s.size();
        auto v = parse(s.substr(0, end));
        s = s.substr(end);
        return {Op::Le, v};
    }
    if (s.starts_with(">")) {
        s = s.substr(1);
        skip_ws(s);
        auto comma = s.find(',');
        auto end = (comma != std::string_view::npos) ? comma : s.size();
        auto v = parse(s.substr(0, end));
        s = s.substr(end);
        return {Op::Gt, v};
    }
    if (s.starts_with("<")) {
        s = s.substr(1);
        skip_ws(s);
        auto comma = s.find(',');
        auto end = (comma != std::string_view::npos) ? comma : s.size();
        auto v = parse(s.substr(0, end));
        s = s.substr(end);
        return {Op::Lt, v};
    }
    if (s.starts_with("=")) {
        s = s.substr(1);
        skip_ws(s);
        auto comma = s.find(',');
        auto end = (comma != std::string_view::npos) ? comma : s.size();
        auto v = parse(s.substr(0, end));
        s = s.substr(end);
        return {Op::Eq, v};
    }

    // no operator prefix — just a version
    auto comma = s.find(',');
    auto end = (comma != std::string_view::npos) ? comma : s.size();
    auto v = parse(s.substr(0, end));
    s = s.substr(end);
    return {Op::Eq, v};
}

} // namespace detail

// ^1.2.3 → >=1.2.3, <2.0.0 (major != 0)
// ^0.2.3 → >=0.2.3, <0.3.0 (major == 0)
// ~1.2.3 → >=1.2.3, <1.3.0
// bare 1.2.3 → ^1.2.3
Range parse_range(std::string_view str) {
    Range r;

    // trim whitespace
    while (!str.empty() && str.front() == ' ')
        str = str.substr(1);
    while (!str.empty() && str.back() == ' ')
        str = str.substr(0, str.size() - 1);

    if (str.starts_with("^")) {
        auto v = parse(str.substr(1));
        r.constraints.push_back({Op::Ge, v});
        if (v.major != 0) {
            r.constraints.push_back({Op::Lt, {v.major + 1, 0, 0}});
        } else {
            r.constraints.push_back({Op::Lt, {0, v.minor + 1, 0}});
        }
        return r;
    }

    if (str.starts_with("~")) {
        auto v = parse(str.substr(1));
        r.constraints.push_back({Op::Ge, v});
        r.constraints.push_back({Op::Lt, {v.major, v.minor + 1, 0}});
        return r;
    }

    if (str.starts_with(">") || str.starts_with("<") || str.starts_with("=")) {
        // explicit constraints, possibly comma-separated
        while (!str.empty()) {
            detail::skip_ws(str);
            if (str.empty())
                break;
            r.constraints.push_back(detail::parse_constraint(str));
            detail::skip_ws(str);
            if (!str.empty() && str[0] == ',') {
                str = str.substr(1);
            }
        }
        return r;
    }

    // bare version → treat as caret
    auto v = parse(str);
    r.constraints.push_back({Op::Ge, v});
    if (v.major != 0) {
        r.constraints.push_back({Op::Lt, {v.major + 1, 0, 0}});
    } else {
        r.constraints.push_back({Op::Lt, {0, v.minor + 1, 0}});
    }
    return r;
}

bool satisfies(Version const& v, Range const& r) {
    for (auto const& c : r.constraints) {
        switch (c.op) {
        case Op::Eq:
            if (!(v == c.ver))
                return false;
            break;
        case Op::Lt:
            if (!(v < c.ver))
                return false;
            break;
        case Op::Le:
            if (!(v <= c.ver))
                return false;
            break;
        case Op::Gt:
            if (!(v > c.ver))
                return false;
            break;
        case Op::Ge:
            if (!(v >= c.ver))
                return false;
            break;
        }
    }
    return true;
}

} // namespace semver
