/**
 * HcEngine Turbo Module: thin JS bridge over hcengine::EngineHost.
 */
#include "HcEngine.h"

#include "../engine/EngineHost.h"

using namespace com::amazon::kepler::turbomodule;
using hcengine::EngineHost;
using hcengine::EngineOptions;

namespace HcEngineTurboModule {

HcEngine::HcEngine() {}
HcEngine::~HcEngine() noexcept {}

std::string HcEngine::start(std::string optionsJson) {
  EngineOptions o;
  std::string err = hcengine::parseOptions(optionsJson, o);
  if (!err.empty()) return err;
  return EngineHost::instance().start(o);
}

void HcEngine::stop() { EngineHost::instance().stop(); }

std::string HcEngine::restart(std::string optionsJson) {
  if (optionsJson.empty()) return EngineHost::instance().restart(nullptr);
  EngineOptions o;
  std::string err = hcengine::parseOptions(optionsJson, o);
  if (!err.empty()) return err;
  return EngineHost::instance().restart(&o);
}

void HcEngine::setLanAccess(bool enabled) { EngineHost::instance().setLanAccess(enabled); }

std::string HcEngine::getStatus() { return EngineHost::instance().statusJson(); }

std::string HcEngine::getLogTail(double maxBytes) {
  if (maxBytes < 0) maxBytes = 0;
  if (maxBytes > 1 << 20) maxBytes = 1 << 20;
  return EngineHost::instance().logTail((size_t)maxBytes);
}

std::string HcEngine::qrMatrix(std::string text) { return hcengine::qrMatrix(text); }

std::string HcEngine::getSettings() { return EngineHost::instance().settings(); }

std::string HcEngine::updateSettings(std::string settingsJson) {
  return EngineHost::instance().updateSettings(settingsJson);
}

}  // namespace HcEngineTurboModule
