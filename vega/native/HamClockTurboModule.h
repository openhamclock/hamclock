#pragma once

#include <string>

namespace hamclock {
namespace vega {

class HamClockTurboModule {
public:
    static bool startEngine(const std::string &dataDir,
                            int restPort = 8080,
                            int rwPort = 8081,
                            int roPort = 8082,
                            const std::string &backendHost = "");
    static bool stopEngine();
    static bool isEngineRunning();
    static std::string getEngineStatus();
};

} // namespace vega
} // namespace hamclock
