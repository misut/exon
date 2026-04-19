import std;
import foundation;

namespace {

std::string local_profile() {
#if defined(WORKSPACE_PROFILE_RELEASE)
    return "release";
#elif defined(WORKSPACE_PROFILE_DEBUG)
    return "debug";
#else
    return "default";
#endif
}

}

int main() {
    std::println("profile from workspace.build.* -> {}", local_profile());
    std::println("{}", foundation::member_label("report", "app"));
    std::println("{}", foundation::shared_defaults());
    std::println("{}", foundation::dependency_path("report", "foundation"));
    return 0;
}
