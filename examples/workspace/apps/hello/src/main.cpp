import std;
import greeting;

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
    std::println("{}", greeting::routed_message("hello"));
    return 0;
}
