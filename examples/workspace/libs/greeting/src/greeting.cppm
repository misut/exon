export module greeting;
import std;
import foundation;

export namespace greeting {

std::string routed_message(std::string_view member) {
    return std::format("{}\n{}\n{}",
                       foundation::member_label(member, "app"),
                       foundation::shared_defaults(),
                       foundation::dependency_path(member, "greeting -> foundation"));
}

}
