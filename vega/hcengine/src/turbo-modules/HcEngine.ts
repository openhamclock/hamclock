import type {KeplerTurboModule} from '@amazon-devices/keplerscript-turbomodule-api';
import {TurboModuleRegistry} from '@amazon-devices/keplerscript-turbomodule-api';

/**
 * Native interface to the HamClock engine running on a thread inside the app process.
 * Structured values cross the bridge as JSON strings; use the typed wrapper in ../index.ts.
 */
export interface HcEngine extends KeplerTurboModule {
  /** Start the engine. optionsJson = EngineOptions. Returns '' on success or an error message. */
  start: (optionsJson: string) => string;
  /** Ask the engine to stop. Returns at once; poll getStatus() for state 'stopped'. */
  stop: () => void;
  /** Stop, then start again with new options ('' keeps the current ones). */
  restart: (optionsJson: string) => string;
  /** Bind to all interfaces (true) or loopback only (false). Restarts a running engine. */
  setLanAccess: (enabled: boolean) => void;
  /** EngineStatus as JSON. */
  getStatus: () => string;
  /** Last maxBytes of the engine's diagnostic log. */
  getLogTail: (maxBytes: number) => string;
}

export default TurboModuleRegistry.getEnforcing<HcEngine>('HcEngine');
