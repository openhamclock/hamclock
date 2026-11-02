import { AppRegistry, LogBox } from 'react-native';
import { App } from './src/App';
import { name as appName } from './app.json';
import { ensureEngineStarted, DEFAULT_ENGINE_OPTIONS, HcEngine } from './src/engine';
import { startFetchBridge } from './src/engine/fetchBridge';

// Set true only for a Phase 1 test build: runs the engine self-test instead of the normal
// engine start (results in the device log, prefix [HCTEST]). Debug builds only: in Release
// __DEV__ is false, so Metro drops this branch and selfTest.ts from the bundle.
const ENGINE_SELFTEST = false;

// Temporary workaround for problem with nested text
// not working currently.
LogBox.ignoreAllLogs();

// Start the embedded HamClock engine as early as possible; the UI waits for it.
if (__DEV__ && ENGINE_SELFTEST) {
  require('./src/engine/selfTest').runEngineSelfTest();
} else {
  try {
    ensureEngineStarted(DEFAULT_ENGINE_OPTIONS);
  } catch (e) {
    console.error(`HcEngine start failed: ${String(e)}`);
  }
}

// HTTPS for the engine's curl panes (Sat_Alerts, Mesh_Mon): see src/engine/fetchBridge.ts.
startFetchBridge({
  engine: HcEngine,
  fetchFn: fetch,
  log: __DEV__ ? msg => console.info(msg) : undefined,
});

AppRegistry.registerComponent(appName, () => App);
