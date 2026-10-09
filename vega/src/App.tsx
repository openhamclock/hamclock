import React, { useEffect, useState, useRef } from "react";
import {
  View,
  Text,
  StyleSheet,
  ActivityIndicator,
  Modal,
  TextInput,
  TouchableOpacity,
  BackHandler,
  TVEventHandler,
} from "react-native";
import { WebView } from "@amazon-devices/webview";
import AsyncStorage from "@react-native-async-storage/async-storage";
import { generateRemoteInjectionScript } from "./RemoteNav";

// Native TurboModule Interface
import { NativeModules } from "react-native";
const { HamClockTurboModule } = NativeModules;

const PREF_BACKEND_HOST = "hamclock_backend_host";
const PREF_USE_LOCAL_ENGINE = "hamclock_use_local_engine";

export const App = () => {
  const [loading, setLoading] = useState<boolean>(true);
  const [statusMessage, setStatusMessage] = useState<string>("Initializing HamClock engine...");
  const [webUrl, setWebUrl] = useState<string>("http://127.0.0.1:8080");
  const [settingsVisible, setSettingsVisible] = useState<boolean>(false);
  const [backendInput, setBackendInput] = useState<string>("");
  const [useLocalEngine, setUseLocalEngine] = useState<boolean>(true);

  const webViewRef = useRef<any>(null);
  const tvEventHandler = useRef<any>(null);

  useEffect(() => {
    // Fire TV remote event listener
    tvEventHandler.current = new TVEventHandler();
    tvEventHandler.current.enable(null, (cmp: any, evt: any) => {
      if (evt && evt.eventType) {
        if (evt.eventType === "menu" || evt.eventType === "playPause") {
          setSettingsVisible((prev) => !prev);
        }
      }
    });

    const backAction = () => {
      if (settingsVisible) {
        setSettingsVisible(false);
        return true;
      }
      return false;
    };

    const backHandler = BackHandler.addEventListener("hardwareBackPress", backAction);

    initializeEngine();

    return () => {
      backHandler.remove();
      if (tvEventHandler.current) {
        tvEventHandler.current.disable();
      }
    };
  }, [settingsVisible]);

  const initializeEngine = async () => {
    try {
      const savedUseLocal = await AsyncStorage.getItem(PREF_USE_LOCAL_ENGINE);
      const savedBackend = await AsyncStorage.getItem(PREF_BACKEND_HOST);

      const isLocal = savedUseLocal !== "false";
      setUseLocalEngine(isLocal);
      if (savedBackend) {
        setBackendInput(savedBackend);
      }

      if (!isLocal && savedBackend) {
        setStatusMessage(`Connecting to remote host ${savedBackend}...`);
        setWebUrl(`http://${savedBackend}`);
        setLoading(false);
        return;
      }

      // Start native C++ TurboModule engine
      setStatusMessage("Starting local HamClock daemon...");
      if (HamClockTurboModule && HamClockTurboModule.startEngine) {
        const dataDir = "/data/data/org.openhamclock.hamclock/files";
        await HamClockTurboModule.startEngine(dataDir, 8080, 8081, 8082, savedBackend || "");
      }

      // Wait for local HTTP/WebSocket port
      setStatusMessage("Waiting for web service on 127.0.0.1:8080...");
      await pollUntilReady("http://127.0.0.1:8080/get_sys.txt", 15);

      setWebUrl("http://127.0.0.1:8080");
      setLoading(false);
    } catch (err: any) {
      setStatusMessage(`Startup notice: ${err?.message || "Loading default localhost..."}`);
      setWebUrl("http://127.0.0.1:8080");
      setLoading(false);
    }
  };

  const pollUntilReady = async (url: string, maxAttempts: number): Promise<boolean> => {
    for (let i = 0; i < maxAttempts; i++) {
      try {
        const res = await fetch(url);
        if (res.ok) return true;
      } catch {
        // Port not ready yet, wait and retry
      }
      await new Promise((r) => setTimeout(r, 1000));
    }
    return false;
  };

  const saveSettings = async () => {
    await AsyncStorage.setItem(PREF_USE_LOCAL_ENGINE, useLocalEngine ? "true" : "false");
    await AsyncStorage.setItem(PREF_BACKEND_HOST, backendInput.trim());
    setSettingsVisible(false);
    setLoading(true);
    initializeEngine();
  };

  return (
    <View style={styles.container}>
      {loading ? (
        <View style={styles.loadingContainer}>
          <Text style={styles.titleText}>HamClock for Vega OS</Text>
          <ActivityIndicator size="large" color="#4dabf7" style={styles.spinner} />
          <Text style={styles.statusText}>{statusMessage}</Text>
        </View>
      ) : (
        <WebView
          ref={webViewRef}
          source={{ uri: webUrl }}
          style={styles.webView}
          hasTVPreferredFocus={true}
          javaScriptEnabled={true}
          domStorageEnabled={true}
          injectedJavaScript={generateRemoteInjectionScript()}
        />
      )}

      {/* Backend / Settings Dialog (Opened via Menu or Play/Pause) */}
      <Modal visible={settingsVisible} transparent animationType="fade">
        <View style={styles.modalBackdrop}>
          <View style={styles.modalContent}>
            <Text style={styles.modalTitle}>HamClock Settings</Text>

            <TouchableOpacity
              style={styles.button}
              onPress={() => setUseLocalEngine(!useLocalEngine)}
            >
              <Text style={styles.buttonText}>
                Mode: {useLocalEngine ? "Embedded Local Engine" : "Remote Backend"}
              </Text>
            </TouchableOpacity>

            {!useLocalEngine && (
              <TextInput
                style={styles.input}
                placeholder="192.168.1.100:8080"
                placeholderTextColor="#888"
                value={backendInput}
                onChangeText={setBackendInput}
              />
            )}

            <View style={styles.modalButtons}>
              <TouchableOpacity style={[styles.button, styles.saveButton]} onPress={saveSettings}>
                <Text style={styles.buttonText}>Save & Apply</Text>
              </TouchableOpacity>
              <TouchableOpacity
                style={[styles.button, styles.cancelButton]}
                onPress={() => setSettingsVisible(false)}
              >
                <Text style={styles.buttonText}>Close</Text>
              </TouchableOpacity>
            </View>
          </View>
        </View>
      </Modal>
    </View>
  );
};

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: "#000000",
  },
  webView: {
    flex: 1,
    backgroundColor: "#000000",
  },
  loadingContainer: {
    flex: 1,
    justifyContent: "center",
    alignItems: "center",
    backgroundColor: "#111111",
  },
  titleText: {
    fontSize: 28,
    fontWeight: "bold",
    color: "#ffffff",
    marginBottom: 20,
  },
  statusText: {
    fontSize: 16,
    color: "#aaaaaa",
    marginTop: 15,
  },
  spinner: {
    marginVertical: 10,
  },
  modalBackdrop: {
    flex: 1,
    backgroundColor: "rgba(0,0,0,0.85)",
    justifyContent: "center",
    alignItems: "center",
  },
  modalContent: {
    width: 500,
    padding: 30,
    backgroundColor: "#1e1e1e",
    borderRadius: 8,
    borderWidth: 1,
    borderColor: "#333333",
  },
  modalTitle: {
    fontSize: 22,
    fontWeight: "bold",
    color: "#ffffff",
    marginBottom: 20,
  },
  input: {
    backgroundColor: "#2a2a2a",
    color: "#ffffff",
    padding: 12,
    borderRadius: 4,
    marginVertical: 15,
    fontSize: 16,
  },
  modalButtons: {
    flexDirection: "row",
    justifyContent: "space-between",
    marginTop: 20,
  },
  button: {
    backgroundColor: "#333333",
    padding: 12,
    borderRadius: 4,
    alignItems: "center",
  },
  saveButton: {
    backgroundColor: "#1971c2",
    flex: 1,
    marginRight: 10,
  },
  cancelButton: {
    backgroundColor: "#495057",
    flex: 1,
    marginLeft: 10,
  },
  buttonText: {
    color: "#ffffff",
    fontSize: 16,
    fontWeight: "600",
  },
});
