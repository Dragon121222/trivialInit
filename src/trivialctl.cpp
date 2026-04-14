#include "trivialInit/init_system.hpp"
#include <cstdio>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    std::fprintf(stderr, "trivialctl: unit file monitor\n");

    tinit::InitSystem sys;
    return sys.run_monitor();
}
