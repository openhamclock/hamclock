//
//  HamClockViewController.swift
//  HamClock
//
//  Main container view controller hosting WKWebView and native controls.
//

import UIKit
import WebKit

class HamClockViewController: UIViewController, WKNavigationDelegate, WKUIDelegate,
                              HamClockBridgeDelegate, LocationServiceDelegate, SettingsViewControllerDelegate {

    private var webView: WKWebView!
    private let activityIndicator = UIActivityIndicatorView(style: .large)
    private let statusLabel = UILabel()
    private let settingsButton = UIButton(type: .system)

    // Error recovery card components
    private let errorContainer = UIView()
    private let errorIcon = UIImageView()
    private let errorTitleLabel = UILabel()
    private let errorMessageLabel = UILabel()
    private let restartButton = UIButton(type: .system)
    private let errorSettingsButton = UIButton(type: .system)

    private let liveWebPort: Int32 = 8081
    private let roPort: Int32 = 8082
    private let restPort: Int32 = 8080

    private var pollTimer: Timer?
    private var isConnected = false
    private var connectAttemptCount = 0
    private let maxConnectAttempts = 24 // 24 * 0.5s = 12 seconds

    override var prefersStatusBarHidden: Bool {
        return true
    }

    override var prefersHomeIndicatorAutoHidden: Bool {
        return true
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black

        setupWebView()
        setupOverlayUI()
        setupRecoveryUI()
        setupBridge()

        LocationService.shared.delegate = self
        LocationService.shared.requestLocationPermission()

        startEngine(forceSetup: false, countdown: false)
    }

    private func setupWebView() {
        let config = WKWebViewConfiguration()
        config.allowsInlineMediaPlayback = true
        config.mediaTypesRequiringUserActionForPlayback = []
        config.preferences.javaScriptCanOpenWindowsAutomatically = true

        // Viewport and canvas auto-scaling script with pinch-to-zoom enabled
        let source = """
        var meta = document.createElement('meta');
        meta.name = 'viewport';
        meta.content = 'width=device-width, initial-scale=1.0, maximum-scale=5.0, user-scalable=yes, viewport-fit=cover';
        document.getElementsByTagName('head')[0].appendChild(meta);
        document.body.style.backgroundColor = '#000';
        document.body.style.margin = '0';
        document.body.style.padding = '0';
        """
        let script = WKUserScript(source: source, injectionTime: .atDocumentEnd, forMainFrameOnly: true)
        config.userContentController.addUserScript(script)

        webView = WKWebView(frame: view.bounds, configuration: config)
        webView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        webView.navigationDelegate = self
        webView.uiDelegate = self
        webView.isOpaque = false
        webView.backgroundColor = .black
        webView.scrollView.isScrollEnabled = true
        webView.scrollView.bounces = true
        webView.scrollView.minimumZoomScale = 1.0
        webView.scrollView.maximumZoomScale = 5.0
        webView.scrollView.contentInsetAdjustmentBehavior = .never
        view.addSubview(webView)
    }

    private func setupOverlayUI() {
        activityIndicator.color = .systemCyan
        activityIndicator.translatesAutoresizingMaskIntoConstraints = false
        activityIndicator.startAnimating()
        view.addSubview(activityIndicator)

        statusLabel.text = "Starting HamClock engine..."
        statusLabel.textColor = .white
        statusLabel.font = .systemFont(ofSize: 16, weight: .medium)
        statusLabel.textAlignment = .center
        statusLabel.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(statusLabel)

        settingsButton.setImage(UIImage(systemName: "gearshape.fill"), for: .normal)
        settingsButton.tintColor = .white
        settingsButton.backgroundColor = UIColor(white: 0.0, alpha: 0.7)
        settingsButton.layer.cornerRadius = 20
        settingsButton.layer.borderWidth = 1.0
        settingsButton.layer.borderColor = UIColor(white: 1.0, alpha: 0.4).cgColor
        settingsButton.translatesAutoresizingMaskIntoConstraints = false
        settingsButton.addTarget(self, action: #selector(settingsTapped), for: .touchUpInside)

        let panGesture = UIPanGestureRecognizer(target: self, action: #selector(handleButtonPan(_:)))
        settingsButton.addGestureRecognizer(panGesture)
        view.addSubview(settingsButton)

        let twoFingerTap = UITapGestureRecognizer(target: self, action: #selector(settingsTapped))
        twoFingerTap.numberOfTouchesRequired = 2
        twoFingerTap.numberOfTapsRequired = 2
        view.addGestureRecognizer(twoFingerTap)

        NSLayoutConstraint.activate([
            activityIndicator.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            activityIndicator.centerYAnchor.constraint(equalTo: view.centerYAnchor, constant: -20),

            statusLabel.topAnchor.constraint(equalTo: activityIndicator.bottomAnchor, constant: 16),
            statusLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),

            settingsButton.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            settingsButton.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -12),
            settingsButton.widthAnchor.constraint(equalToConstant: 40),
            settingsButton.heightAnchor.constraint(equalToConstant: 40)
        ])
    }

    private func setupRecoveryUI() {
        errorContainer.backgroundColor = UIColor(red: 0.12, green: 0.12, blue: 0.15, alpha: 0.96)
        errorContainer.layer.cornerRadius = 16
        errorContainer.layer.borderWidth = 1.0
        errorContainer.layer.borderColor = UIColor(white: 1.0, alpha: 0.2).cgColor
        errorContainer.layer.shadowColor = UIColor.black.cgColor
        errorContainer.layer.shadowOpacity = 0.5
        errorContainer.layer.shadowOffset = CGSize(width: 0, height: 4)
        errorContainer.layer.shadowRadius = 8
        errorContainer.translatesAutoresizingMaskIntoConstraints = false
        errorContainer.isHidden = true
        errorContainer.alpha = 0.0
        view.addSubview(errorContainer)

        let errorCardStack = UIStackView()
        errorCardStack.axis = .vertical
        errorCardStack.alignment = .center
        errorCardStack.spacing = 14
        errorCardStack.translatesAutoresizingMaskIntoConstraints = false
        errorContainer.addSubview(errorCardStack)

        errorIcon.image = UIImage(systemName: "exclamationmark.triangle.fill")
        errorIcon.tintColor = .systemOrange
        errorIcon.contentMode = .scaleAspectFit
        errorIcon.translatesAutoresizingMaskIntoConstraints = false
        errorIcon.heightAnchor.constraint(equalToConstant: 44).isActive = true
        errorIcon.widthAnchor.constraint(equalToConstant: 44).isActive = true
        errorCardStack.addArrangedSubview(errorIcon)

        errorTitleLabel.text = "HamClock Engine Not Responding"
        errorTitleLabel.textColor = .white
        errorTitleLabel.font = .systemFont(ofSize: 18, weight: .bold)
        errorTitleLabel.textAlignment = .center
        errorCardStack.addArrangedSubview(errorTitleLabel)

        errorMessageLabel.text = "The background HamClock engine failed to respond on port \(liveWebPort). Check settings or restart the engine."
        errorMessageLabel.textColor = UIColor(white: 0.8, alpha: 1.0)
        errorMessageLabel.font = .systemFont(ofSize: 14, weight: .regular)
        errorMessageLabel.textAlignment = .center
        errorMessageLabel.numberOfLines = 0
        errorCardStack.addArrangedSubview(errorMessageLabel)

        let buttonStack = UIStackView()
        buttonStack.axis = .horizontal
        buttonStack.spacing = 12
        buttonStack.distribution = .fillEqually
        buttonStack.translatesAutoresizingMaskIntoConstraints = false

        restartButton.setTitle("Restart Engine", for: .normal)
        restartButton.setTitleColor(.white, for: .normal)
        restartButton.backgroundColor = UIColor(red: 0.2, green: 0.45, blue: 0.8, alpha: 1.0)
        restartButton.titleLabel?.font = .systemFont(ofSize: 15, weight: .semibold)
        restartButton.layer.cornerRadius = 8
        restartButton.heightAnchor.constraint(equalToConstant: 42).isActive = true
        restartButton.addTarget(self, action: #selector(restartEngineTapped), for: .touchUpInside)

        errorSettingsButton.setTitle("HamClock Settings", for: .normal)
        errorSettingsButton.setTitleColor(.white, for: .normal)
        errorSettingsButton.backgroundColor = UIColor(white: 0.25, alpha: 1.0)
        errorSettingsButton.titleLabel?.font = .systemFont(ofSize: 15, weight: .semibold)
        errorSettingsButton.layer.cornerRadius = 8
        errorSettingsButton.heightAnchor.constraint(equalToConstant: 42).isActive = true
        errorSettingsButton.addTarget(self, action: #selector(settingsTapped), for: .touchUpInside)

        buttonStack.addArrangedSubview(restartButton)
        buttonStack.addArrangedSubview(errorSettingsButton)
        errorCardStack.addArrangedSubview(buttonStack)

        NSLayoutConstraint.activate([
            errorContainer.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            errorContainer.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            errorContainer.widthAnchor.constraint(lessThanOrEqualToConstant: 420),
            errorContainer.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 24),
            errorContainer.trailingAnchor.constraint(lessThanOrEqualTo: view.trailingAnchor, constant: -24),

            errorCardStack.topAnchor.constraint(equalTo: errorContainer.topAnchor, constant: 20),
            errorCardStack.leadingAnchor.constraint(equalTo: errorContainer.leadingAnchor, constant: 20),
            errorCardStack.trailingAnchor.constraint(equalTo: errorContainer.trailingAnchor, constant: -20),
            errorCardStack.bottomAnchor.constraint(equalTo: errorContainer.bottomAnchor, constant: -20),

            buttonStack.widthAnchor.constraint(equalTo: errorCardStack.widthAnchor)
        ])
    }

    @objc private func handleButtonPan(_ gesture: UIPanGestureRecognizer) {
        let translation = gesture.translation(in: view)
        if let btn = gesture.view {
            btn.center = CGPoint(x: btn.center.x + translation.x, y: btn.center.y + translation.y)
        }
        gesture.setTranslation(.zero, in: view)
    }

    private func setupBridge() {
        HamClockBridge.shared().delegate = self
        let defaults = UserDefaults.standard
        let allow = defaults.bool(forKey: "allow_external_access")
        HamClockBridge.shared().setAllowExternalAccess(allow)
    }

    private func startEngine(forceSetup: Bool, countdown: Bool) {
        let paths = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)
        let dataDir = paths[0].path

        let defaults = UserDefaults.standard
        let backendHost = defaults.string(forKey: "backend_host") ?? ""
        let mdnsName = defaults.string(forKey: "mdns_name") ?? "HamClock"

        let hasLoc = LocationService.shared.hasLocation
        let lat = LocationService.shared.lastLatitude ?? 0.0
        let lng = LocationService.shared.lastLongitude ?? 0.0

        BonjourService.shared.registerServices(name: mdnsName, restPort: restPort, webPort: liveWebPort)

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            HamClockBridge.shared().startDaemon(withDataDir: dataDir,
                                               rwPort: self.liveWebPort,
                                               roPort: self.roPort,
                                               restPort: self.restPort,
                                               backendHost: backendHost.isEmpty ? nil : backendHost,
                                               hasLocation: hasLoc,
                                               lat: lat,
                                               lng: lng,
                                               forceSetup: forceSetup,
                                               countdownSetup: countdown)
            DispatchQueue.main.async {
                self.startConnectingToEngine()
            }
        }
    }

    private func startConnectingToEngine() {
        pollTimer?.invalidate()
        isConnected = false
        connectAttemptCount = 0

        errorContainer.isHidden = true
        errorContainer.alpha = 0.0
        activityIndicator.startAnimating()
        activityIndicator.isHidden = false
        statusLabel.isHidden = false
        statusLabel.text = "Connecting to HamClock..."

        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.checkEngineReadiness()
        }
    }

    private func checkEngineReadiness() {
        guard let url = URL(string: "http://127.0.0.1:\(liveWebPort)/live.html") else { return }

        connectAttemptCount += 1
        if connectAttemptCount > maxConnectAttempts {
            DispatchQueue.main.async { [weak self] in
                self?.showRecoveryUI()
            }
            return
        }

        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        request.timeoutInterval = 1.0

        let task = URLSession.shared.dataTask(with: request) { [weak self] (_, response, error) in
            if let httpResp = response as? HTTPURLResponse, httpResp.statusCode == 200 {
                DispatchQueue.main.async {
                    self?.onEngineReady(url: url)
                }
            } else if let self = self, !HamClockBridge.shared().isDaemonRunning() && self.connectAttemptCount >= 6 {
                // If daemon thread already exited prematurely, fail fast
                DispatchQueue.main.async {
                    self.showRecoveryUI()
                }
            }
        }
        task.resume()
    }

    private func showRecoveryUI() {
        guard !isConnected else { return }
        pollTimer?.invalidate()
        pollTimer = nil
        activityIndicator.stopAnimating()
        activityIndicator.isHidden = true
        statusLabel.isHidden = true

        errorContainer.isHidden = false
        UIView.animate(withDuration: 0.25) {
            self.errorContainer.alpha = 1.0
        }
    }

    @objc private func restartEngineTapped() {
        UIView.animate(withDuration: 0.2, animations: {
            self.errorContainer.alpha = 0.0
        }) { _ in
            self.errorContainer.isHidden = true
            self.startEngine(forceSetup: false, countdown: false)
        }
    }

    private func onEngineReady(url: URL) {
        guard !isConnected else { return }
        isConnected = true
        pollTimer?.invalidate()
        pollTimer = nil

        errorContainer.isHidden = true
        errorContainer.alpha = 0.0
        statusLabel.text = "Loading interface..."
        webView.load(URLRequest(url: url))
    }

    // MARK: - WKNavigationDelegate

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        activityIndicator.stopAnimating()
        activityIndicator.isHidden = true
        statusLabel.isHidden = true
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        statusLabel.text = "Load failed: \(error.localizedDescription)"
        if !isConnected {
            showRecoveryUI()
        }
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        statusLabel.text = "Connection failed: \(error.localizedDescription)"
        if !isConnected {
            showRecoveryUI()
        }
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        NSLog("[HamClockViewController] WebContent process terminated")
        isConnected = false
        showRecoveryUI()
    }

    // MARK: - HamClockBridgeDelegate

    func hamclockExitRequested() {
        NSLog("[HamClockViewController] Exit requested by native engine")
        exit(0)
    }

    func hamclockRestartRequested(minusK: Bool) {
        NSLog("[HamClockViewController] Restart requested (minusK=\(minusK))")
        startEngine(forceSetup: false, countdown: !minusK)
    }

    func hamclockOpenURL(_ url: URL) {
        UIApplication.shared.open(url, options: [:], completionHandler: nil)
    }

    func hamclockGetClipboardText() -> String {
        return UIPasteboard.general.string ?? ""
    }

    // MARK: - LocationServiceDelegate

    func locationServiceDidUpdateLocation(latitude: Double, longitude: Double) {
        NSLog("[HamClockViewController] GPS location updated: \(latitude), \(longitude)")
    }

    func locationServiceDidFail(with error: Error) {
        NSLog("[HamClockViewController] Location error: \(error.localizedDescription)")
    }

    // MARK: - Settings

    @objc private func settingsTapped() {
        let settingsVC = SettingsViewController()
        settingsVC.delegate = self
        let nav = UINavigationController(rootViewController: settingsVC)
        nav.modalPresentationStyle = .formSheet
        present(nav, animated: true)
    }

    func settingsDidRequestRestart(forceSetup: Bool, countdown: Bool) {
        startEngine(forceSetup: forceSetup, countdown: countdown)
    }

    func settingsDidChangeBackendHost(_ host: String) {
        NSLog("[HamClockViewController] Backend host changed: \(host)")
    }

    func settingsDidChangeAllowExternal(_ allow: Bool) {
        HamClockBridge.shared().setAllowExternalAccess(allow)
    }

    func settingsDidChangeMdnsName(_ name: String) {
        BonjourService.shared.registerServices(name: name, restPort: restPort, webPort: liveWebPort)
    }
}
