#include "trivialInit/init_system.hpp"
#include <unistd.h>
#include <cstdio>

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    // Sanity check: are we PID 1?
    if (getpid() == 1) {
        // We ARE the init system
        std::fprintf(stderr, "trivialInit: running as PID 1\n");
    } else {
        std::fprintf(stderr, "trivialInit: running as PID %d (test mode)\n", getpid());
    }

    tinit::InitSystem init;
    return init.run();
}
