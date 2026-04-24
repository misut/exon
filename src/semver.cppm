export module semver;
import std;

export namespace semver {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease;

    bool is_prerelease() const { return !prerelease.empty(); }

    std::strong_ordering operator<=>(Version const& other) const;
    bool operator==(Version const& other) const {
        return major == other.major && minor == other.minor && patch == other.patch &&
               prerelease == other.prerelease;
    }
};

namespace detail {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

bool is_digits(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

std::optional<int> parse_int(std::string_view s) {
    if (!is_digits(s))
        return std::nullopt;
    int value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size())
        return std::nullopt;
    return value;
}

std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto pos = s.find(delim, start);
        auto end = pos == std::string_view::npos ? s.size() : pos;
        parts.push_back(s.substr(start, end - start));
        if (pos == std::string_view::npos)
            break;
        start = pos + 1;
    }
    return parts;
}

bool wildcard(std::string_view part) {
    return part == "*" || part == "x" || part == "X";
}

bool prerelease_ident_is_valid(std::string_view ident) {
    if (ident.empty())
        return false;
    for (auto ch : ident) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-')
            return false;
    }
    return true;
}

bool prerelease_is_valid(std::string_view prerelease) {
    if (prerelease.empty())
        return false;
    for (auto ident : split(prerelease, '.')) {
        if (!prerelease_ident_is_valid(ident))
            return false;
    }
    return true;
}

int compare_prerelease(std::string_view lhs, std::string_view rhs) {
    if (lhs.empty() && rhs.empty())
        return 0;
    if (lhs.empty())
        return 1;
    if (rhs.empty())
        return -1;

    auto lhs_parts = split(lhs, '.');
    auto rhs_parts = split(rhs, '.');
    auto count = std::min(lhs_parts.size(), rhs_parts.size());
    for (std::size_t i = 0; i < count; ++i) {
        auto l = lhs_parts[i];
        auto r = rhs_parts[i];
        auto l_num = parse_int(l);
        auto r_num = parse_int(r);
        if (l_num && r_num) {
            if (*l_num < *r_num)
                return -1;
            if (*l_num > *r_num)
                return 1;
            continue;
        }
        if (l_num && !r_num)
            return -1;
        if (!l_num && r_num)
            return 1;
        if (l < r)
            return -1;
        if (l > r)
            return 1;
    }
    if (lhs_parts.size() < rhs_parts.size())
        return -1;
    if (lhs_parts.size() > rhs_parts.size())
        return 1;
    return 0;
}

} // namespace detail

std::strong_ordering Version::operator<=>(Version const& other) const {
    if (auto cmp = major <=> other.major; cmp != 0)
        return cmp;
    if (auto cmp = minor <=> other.minor; cmp != 0)
        return cmp;
    if (auto cmp = patch <=> other.patch; cmp != 0)
        return cmp;
    auto pre = detail::compare_prerelease(prerelease, other.prerelease);
    if (pre < 0)
        return std::strong_ordering::less;
    if (pre > 0)
        return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

std::optional<Version> try_parse(std::string_view str) {
    str = detail::trim(str);
    if (!str.empty() && str[0] == 'v')
        str = str.substr(1);

    auto plus = str.find('+');
    if (plus != std::string_view::npos)
        str = str.substr(0, plus);

    std::string prerelease;
    auto dash = str.find('-');
    if (dash != std::string_view::npos) {
        prerelease = std::string{str.substr(dash + 1)};
        str = str.substr(0, dash);
        if (!detail::prerelease_is_valid(prerelease))
            return std::nullopt;
    }

    auto parts = detail::split(str, '.');
    if (parts.empty() || parts.size() > 3)
        return std::nullopt;
    if (std::ranges::any_of(parts, [](auto part) { return part.empty(); }))
        return std::nullopt;

    Version v;
    if (auto major = detail::parse_int(parts[0]))
        v.major = *major;
    else
        return std::nullopt;
    if (parts.size() > 1) {
        if (auto minor = detail::parse_int(parts[1]))
            v.minor = *minor;
        else
            return std::nullopt;
    }
    if (parts.size() > 2) {
        if (auto patch = detail::parse_int(parts[2]))
            v.patch = *patch;
        else
            return std::nullopt;
    }
    v.prerelease = std::move(prerelease);
    return v;
}

Version parse(std::string_view str) {
    if (auto parsed = try_parse(str))
        return *parsed;
    throw std::runtime_error(std::format("invalid semantic version '{}'", str));
}

std::string to_string(Version const& v) {
    auto out = std::format("{}.{}.{}", v.major, v.minor, v.patch);
    if (!v.prerelease.empty())
        out += std::format("-{}", v.prerelease);
    return out;
}

enum class Op { Eq, Lt, Le, Gt, Ge };

struct Constraint {
    Op op;
    Version ver;
};

struct Range {
    std::vector<Constraint> constraints;
    bool allows_prerelease = false;
};

namespace detail {

void skip_ws(std::string_view& s) {
    while (!s.empty() && s[0] == ' ')
        s = s.substr(1);
}

bool starts_operator(std::string_view s) {
    s = trim(s);
    return s.starts_with(">") || s.starts_with("<") || s.starts_with("=");
}

std::string_view take_version_token(std::string_view& s) {
    auto end = std::size_t{0};
    while (end < s.size()) {
        if (s[end] == ',')
            break;
        if (s[end] == ' ' || s[end] == '\t') {
            auto rest = trim(s.substr(end));
            if (starts_operator(rest))
                break;
        }
        ++end;
    }
    auto token = trim(s.substr(0, end));
    s = s.substr(end);
    return token;
}

Constraint parse_constraint(std::string_view& s) {
    skip_ws(s);

    if (s.starts_with(">=")) {
        s = s.substr(2);
        skip_ws(s);
        auto v = parse(take_version_token(s));
        return {Op::Ge, v};
    }
    if (s.starts_with("<=")) {
        s = s.substr(2);
        skip_ws(s);
        auto v = parse(take_version_token(s));
        return {Op::Le, v};
    }
    if (s.starts_with(">")) {
        s = s.substr(1);
        skip_ws(s);
        auto v = parse(take_version_token(s));
        return {Op::Gt, v};
    }
    if (s.starts_with("<")) {
        s = s.substr(1);
        skip_ws(s);
        auto v = parse(take_version_token(s));
        return {Op::Lt, v};
    }
    if (s.starts_with("=")) {
        s = s.substr(1);
        skip_ws(s);
        auto v = parse(take_version_token(s));
        return {Op::Eq, v};
    }

    auto v = parse(take_version_token(s));
    return {Op::Eq, v};
}

void add_constraint(Range& r, Op op, Version ver) {
    if (ver.is_prerelease())
        r.allows_prerelease = true;
    r.constraints.push_back({op, std::move(ver)});
}

struct ParsedRequirementVersion {
    Version version;
    int specified_components = 0;
};

ParsedRequirementVersion parse_requirement_version(std::string_view s) {
    s = trim(s);
    if (!s.empty() && s.front() == 'v')
        s.remove_prefix(1);
    auto base = s;
    if (auto plus = base.find('+'); plus != std::string_view::npos)
        base = base.substr(0, plus);
    if (auto dash = base.find('-'); dash != std::string_view::npos)
        base = base.substr(0, dash);
    auto count = static_cast<int>(split(base, '.').size());
    return {.version = parse(s), .specified_components = count};
}

Version next_major(Version v) {
    v.major += 1;
    v.minor = 0;
    v.patch = 0;
    v.prerelease.clear();
    return v;
}

Version next_minor(Version v) {
    v.minor += 1;
    v.patch = 0;
    v.prerelease.clear();
    return v;
}

Version next_patch(Version v) {
    v.patch += 1;
    v.prerelease.clear();
    return v;
}

void add_caret(Range& r, Version v) {
    add_constraint(r, Op::Ge, v);
    if (v.major != 0) {
        add_constraint(r, Op::Lt, next_major(v));
    } else if (v.minor != 0) {
        add_constraint(r, Op::Lt, next_minor(v));
    } else {
        add_constraint(r, Op::Lt, next_patch(v));
    }
}

void add_tilde(Range& r, ParsedRequirementVersion parsed) {
    add_constraint(r, Op::Ge, parsed.version);
    if (parsed.specified_components <= 1)
        add_constraint(r, Op::Lt, next_major(parsed.version));
    else
        add_constraint(r, Op::Lt, next_minor(parsed.version));
}

bool add_wildcard_if_present(Range& r, std::string_view str) {
    str = trim(str);
    if (!str.empty() && str.front() == 'v')
        str.remove_prefix(1);
    if (str == "*" || str == "x" || str == "X")
        return true;

    auto parts = split(str, '.');
    if (parts.empty() || parts.size() > 3)
        return false;
    if (!std::ranges::any_of(parts, wildcard))
        return false;

    if (wildcard(parts[0]))
        return true;

    auto major = parse_int(parts[0]);
    if (!major)
        throw std::runtime_error(std::format("invalid semantic version requirement '{}'", str));

    if (parts.size() == 1 || wildcard(parts[1])) {
        Version lower{.major = *major, .minor = 0, .patch = 0};
        add_constraint(r, Op::Ge, lower);
        add_constraint(r, Op::Lt, next_major(lower));
        return true;
    }

    auto minor = parse_int(parts[1]);
    if (!minor)
        throw std::runtime_error(std::format("invalid semantic version requirement '{}'", str));

    if (parts.size() == 2 || wildcard(parts[2])) {
        Version lower{.major = *major, .minor = *minor, .patch = 0};
        add_constraint(r, Op::Ge, lower);
        add_constraint(r, Op::Lt, next_minor(lower));
        return true;
    }

    throw std::runtime_error(std::format("invalid semantic version requirement '{}'", str));
}

} // namespace detail

// ^1.2.3 → >=1.2.3, <2.0.0 (major != 0)
// ^0.2.3 → >=0.2.3, <0.3.0 (major == 0)
// ~1.2.3 → >=1.2.3, <1.3.0
// bare 1.2.3 → ^1.2.3
Range parse_range(std::string_view str) {
    Range r;

    str = detail::trim(str);
    if (str.empty() || str == "*" || str == "x" || str == "X")
        return r;

    if (str.starts_with("^")) {
        auto parsed = detail::parse_requirement_version(str.substr(1));
        detail::add_caret(r, parsed.version);
        return r;
    }

    if (str.starts_with("~")) {
        auto parsed = detail::parse_requirement_version(str.substr(1));
        detail::add_tilde(r, parsed);
        return r;
    }

    if (detail::add_wildcard_if_present(r, str))
        return r;

    if (str.starts_with(">") || str.starts_with("<") || str.starts_with("=")) {
        while (!str.empty()) {
            str = detail::trim(str);
            if (!str.empty() && str.front() == ',')
                str.remove_prefix(1);
            str = detail::trim(str);
            if (str.empty())
                break;
            auto constraint = detail::parse_constraint(str);
            detail::add_constraint(r, constraint.op, constraint.ver);
        }
        return r;
    }

    auto parsed = detail::parse_requirement_version(str);
    detail::add_caret(r, parsed.version);
    return r;
}

bool satisfies(Version const& v, Range const& r) {
    if (v.is_prerelease() && !r.allows_prerelease)
        return false;
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
