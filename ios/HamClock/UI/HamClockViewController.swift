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

    private let liveWebPort: Int32 = 8081
    private let roPort: Int32 = 8082
    private let restPort: Int32 = 8080

    private var pollTimer: Timer?
    private var isConnected = false

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

        var request = URLRequest(url: url)
        request.httpMethod = "GET"
        request.timeoutInterval = 1.0

        let task = URLSession.shared.dataTask(with: request) { [weak self] (_, response, error) in
            if let httpResp = response as? HTTPURLResponse, httpResp.statusCode == 200 {
                DispatchQueue.main.async {
                    self?.onEngineReady(url: url)
                }
            }
        }
        task.resume()
    }

    private func onEngineReady(url: URL) {
        guard !isConnected else { return }
        isConnected = true
        pollTimer?.invalidate()
        pollTimer = nil

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
