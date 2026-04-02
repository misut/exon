export module templates;

export namespace templates {

constexpr auto exon_toml_bin = R"toml([package]
name = ""
version = "0.1.0"
description = ""
authors = []
license = ""
type = "bin"
standard = 23

[dependencies]
)toml";

constexpr auto exon_toml_lib = R"toml([package]
name = ""
version = "0.1.0"
description = ""
authors = []
license = ""
type = "lib"
standard = 23

[dependencies]
)toml";

} // namespace templates
