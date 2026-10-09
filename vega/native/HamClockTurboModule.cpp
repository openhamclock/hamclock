#include "HamClockTurboModule.h"

#include <iostream>
#include <vector>
#include <string>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "HamClock.h"

extern int hamclock_main(int ac, char *av[]);

namespace hamclock {
namespace vega {

struct DaemonArgs {
    std::string dataDir;
    int restPort;
    int rwPort;
    int roPort;
    std::string backendHost;
};

static pthread_t s_daemon_thread = 0;
static bool s_daemon_running = false;
static std::string s_last_status = "idle";

static void *daemon_worker(void *arg) {
    auto *args = static_cast<DaemonArgs *>(arg);
    s_daemon_running = true;
    s_last_status = "running";

    // Ensure storage directory exists
    mkdir(args->dataDir.c_str(), 0755);

    // Build command-line arguments for hamclock_main
    std::vector<std::string> argStrings = {
        "hamclock-vega",
        "-d", args->dataDir,
        "-e", std::to_string(args->restPort),
        "-w", std::to_string(args->rwPort),
        "-r", std::to_string(args->roPort),
        "-t", "80",  // Throttle display fps to reduce CPU/thermals on TV sticks
        "-k"         // Skip splash countdown
    };

    if (!args->backendHost.empty()) {
        argStrings.push_back("-b");
        argStrings.push_back(args->backendHost);
    }

    std::vector<char *> argv;
    for (auto &str : argStrings) {
        argv.push_back(const_cast<char *>(str.c_str()));
    }
    argv.push_back(nullptr);

    int argc = static_cast<int>(argv.size() - 1);

    std::cout << "[VegaNative] Starting HamClock core with " << argc << " args..." << std::endl;
    delete args;

    int ret = hamclock_main(argc, argv.data());

    std::cout << "[VegaNative] HamClock core exited with code " << ret << std::endl;
    s_daemon_running = false;
    s_last_status = "stopped (code " + std::to_string(ret) + ")";
    return nullptr;
}

bool HamClockTurboModule::startEngine(const std::string &dataDir,
                                      int restPort,
                                      int rwPort,
                                      int roPort,
                                      const std::string &backendHost) {
    if (s_daemon_running) {
        std::cout << "[VegaNative] Engine already running." << std::endl;
        return true;
    }

    auto *args = new DaemonArgs{
        dataDir,
        restPort,
        rwPort,
        roPort,
        backendHost
    };

    int err = pthread_create(&s_daemon_thread, nullptr, daemon_worker, args);
    if (err != 0) {
        delete args;
        s_daemon_running = false;
        s_last_status = "error: pthread_create failed";
        return false;
    }

    pthread_detach(s_daemon_thread);
    return true;
}

bool HamClockTurboModule::stopEngine() {
    if (!s_daemon_running) {
        return true;
    }
    // Set exit condition
    s_daemon_running = false;
    s_last_status = "stopping";
    return true;
}

bool HamClockTurboModule::isEngineRunning() {
    return s_daemon_running;
}

std::string HamClockTurboModule::getEngineStatus() {
    return s_last_status;
}

} // namespace vega
} // namespace hamclock

// Export for Kepler / Vega OS autolinking
extern "C" __attribute__((visibility("default"))) void autoLinkVegaTurboModulesV1() {
    std::cout << "[VegaNative] HamClockTurboModule registered for Vega OS" << std::endl;
}
