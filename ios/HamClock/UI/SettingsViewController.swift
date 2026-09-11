//
//  SettingsViewController.swift
//  HamClock
//
//  Dark-themed Backend and Network settings modal.
//

import UIKit

protocol SettingsViewControllerDelegate: AnyObject {
    func settingsDidRequestRestart(forceSetup: Bool, countdown: Bool)
    func settingsDidChangeBackendHost(_ host: String)
    func settingsDidChangeAllowExternal(_ allow: Bool)
    func settingsDidChangeMdnsName(_ name: String)
}

class SettingsViewController: UIViewController {

    weak var delegate: SettingsViewControllerDelegate?

    private let scrollView = UIScrollView()
    private let contentView = UIStackView()

    private let hostTextField = UITextField()
    private let mdnsTextField = UITextField()
    private let allowExternalSwitch = UISwitch()
    private let qrSegmentedControl = UISegmentedControl(items: ["Live Clock (:8081)", "Antennas (:8080)"])
    private let qrImageView = UIImageView()
    private let qrInfoLabel = UILabel()

    override func viewDidLoad() {
        super.viewDidLoad()
        setupUI()
        loadCurrentSettings()
    }

    private func setupUI() {
        title = "HamClock Settings"
        view.backgroundColor = UIColor(red: 0.1, green: 0.1, blue: 0.12, alpha: 1.0)

        navigationController?.navigationBar.barTintColor = UIColor(red: 0.15, green: 0.15, blue: 0.18, alpha: 1.0)
        navigationController?.navigationBar.titleTextAttributes = [.foregroundColor: UIColor.white]

        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: self, action: #selector(saveAndDismiss))
        navigationItem.leftBarButtonItem = UIBarButtonItem(barButtonSystemItem: .cancel, target: self, action: #selector(cancelDismiss))

        scrollView.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scrollView)

        contentView.axis = .vertical
        contentView.spacing = 20
        contentView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.addSubview(contentView)

        NSLayoutConstraint.activate([
            scrollView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.bottomAnchor),

            contentView.topAnchor.constraint(equalTo: scrollView.topAnchor, constant: 20),
            contentView.leadingAnchor.constraint(equalTo: scrollView.leadingAnchor, constant: 20),
            contentView.trailingAnchor.constraint(equalTo: scrollView.trailingAnchor, constant: -20),
            contentView.bottomAnchor.constraint(equalTo: scrollView.bottomAnchor, constant: -20),
            contentView.widthAnchor.constraint(equalTo: scrollView.widthAnchor, constant: -40)
        ])

        // Backend Host Section
        let hostHeader = createSectionHeader(text: "Backend Server Host")
        contentView.addArrangedSubview(hostHeader)

        hostTextField.backgroundColor = UIColor(red: 0.2, green: 0.2, blue: 0.24, alpha: 1.0)
        hostTextField.textColor = .white
        hostTextField.layer.cornerRadius = 8
        hostTextField.layer.borderWidth = 1.0
        hostTextField.layer.borderColor = UIColor(white: 1.0, alpha: 0.2).cgColor
        hostTextField.placeholder = "e.g., https://hamclock.org (Leave empty for default)"
        hostTextField.autocapitalizationType = .none
        hostTextField.autocorrectionType = .no
        hostTextField.leftView = UIView(frame: CGRect(x: 0, y: 0, width: 10, height: 40))
        hostTextField.leftViewMode = .always
        hostTextField.heightAnchor.constraint(equalToConstant: 44).isActive = true
        contentView.addArrangedSubview(hostTextField)

        // Allow External LAN Access Section
        let lanStack = UIStackView()
        lanStack.axis = .horizontal
        lanStack.alignment = .center
        lanStack.distribution = .equalSpacing

        let lanLabel = UILabel()
        lanLabel.text = "Allow Local Network Access (LAN)"
        lanLabel.textColor = .white
        lanLabel.font = .systemFont(ofSize: 16, weight: .medium)

        allowExternalSwitch.onTintColor = .systemCyan
        allowExternalSwitch.addTarget(self, action: #selector(toggleExternalAccess), for: .valueChanged)

        lanStack.addArrangedSubview(lanLabel)
        lanStack.addArrangedSubview(allowExternalSwitch)
        contentView.addArrangedSubview(lanStack)

        // mDNS Name Section
        let mdnsHeader = createSectionHeader(text: "Bonjour / mDNS Device Name")
        contentView.addArrangedSubview(mdnsHeader)

        mdnsTextField.backgroundColor = UIColor(red: 0.2, green: 0.2, blue: 0.24, alpha: 1.0)
        mdnsTextField.textColor = .white
        mdnsTextField.layer.cornerRadius = 8
        mdnsTextField.layer.borderWidth = 1.0
        mdnsTextField.layer.borderColor = UIColor(white: 1.0, alpha: 0.2).cgColor
        mdnsTextField.placeholder = "HamClock"
        mdnsTextField.autocapitalizationType = .none
        mdnsTextField.autocorrectionType = .no
        mdnsTextField.leftView = UIView(frame: CGRect(x: 0, y: 0, width: 10, height: 40))
        mdnsTextField.leftViewMode = .always
        mdnsTextField.heightAnchor.constraint(equalToConstant: 44).isActive = true
        contentView.addArrangedSubview(mdnsTextField)

        // QR Code Section
        let qrHeader = createSectionHeader(text: "Web & Antenna QR Code")
        contentView.addArrangedSubview(qrHeader)

        qrSegmentedControl.selectedSegmentIndex = 0
        qrSegmentedControl.backgroundColor = UIColor(red: 0.2, green: 0.2, blue: 0.24, alpha: 1.0)
        qrSegmentedControl.selectedSegmentTintColor = .systemCyan
        qrSegmentedControl.setTitleTextAttributes([.foregroundColor: UIColor.white], for: .selected)
        qrSegmentedControl.setTitleTextAttributes([.foregroundColor: UIColor.lightGray], for: .normal)
        qrSegmentedControl.addTarget(self, action: #selector(qrSegmentChanged), for: .valueChanged)
        contentView.addArrangedSubview(qrSegmentedControl)

        qrImageView.contentMode = .scaleAspectFit
        qrImageView.layer.cornerRadius = 8
        qrImageView.clipsToBounds = true
        qrImageView.heightAnchor.constraint(equalToConstant: 180).isActive = true
        contentView.addArrangedSubview(qrImageView)

        qrInfoLabel.textColor = .lightGray
        qrInfoLabel.font = .systemFont(ofSize: 13)
        qrInfoLabel.textAlignment = .center
        qrInfoLabel.numberOfLines = 0
        qrInfoLabel.isUserInteractionEnabled = true
        let copyTap = UITapGestureRecognizer(target: self, action: #selector(copyQrUrlTapped))
        qrInfoLabel.addGestureRecognizer(copyTap)
        contentView.addArrangedSubview(qrInfoLabel)

        // Actions Section
        let actionsHeader = createSectionHeader(text: "Engine Controls")
        contentView.addArrangedSubview(actionsHeader)

        let btnSetup = createActionButton(title: "Restart & Enter Setup", color: .systemOrange)
        btnSetup.addTarget(self, action: #selector(restartSetupTapped), for: .touchUpInside)
        contentView.addArrangedSubview(btnSetup)

        let btnCountdown = createActionButton(title: "Restart with 10s Countdown", color: .systemBlue)
        btnCountdown.addTarget(self, action: #selector(restartCountdownTapped), for: .touchUpInside)
        contentView.addArrangedSubview(btnCountdown)
    }

    private func createSectionHeader(text: String) -> UILabel {
        let label = UILabel()
        label.text = text
        label.textColor = UIColor.systemCyan
        label.font = .systemFont(ofSize: 14, weight: .bold)
        return label
    }

    private func createActionButton(title: String, color: UIColor) -> UIButton {
        let button = UIButton(type: .system)
        button.setTitle(title, for: .normal)
        button.setTitleColor(.white, for: .normal)
        button.backgroundColor = color
        button.titleLabel?.font = .systemFont(ofSize: 16, weight: .semibold)
        button.layer.cornerRadius = 8
        button.heightAnchor.constraint(equalToConstant: 44).isActive = true
        return button
    }

    private func loadCurrentSettings() {
        let defaults = UserDefaults.standard
        hostTextField.text = defaults.string(forKey: "backend_host") ?? ""
        allowExternalSwitch.isOn = defaults.bool(forKey: "allow_external_access")
        mdnsTextField.text = defaults.string(forKey: "mdns_name") ?? "HamClock"

        updateQRCode()
    }

    @objc private func toggleExternalAccess() {
        updateQRCode()
    }

    @objc private func qrSegmentChanged() {
        updateQRCode()
    }

    private func getTargetUrl(isAntennas: Bool) -> String {
        let ip = getLocalIPAddress() ?? "127.0.0.1"
        return isAntennas ? "http://\(ip):8080/antennas.html" : "http://\(ip):8081/live.html"
    }

    private func updateQRCode() {
        let isAntennas = qrSegmentedControl.selectedSegmentIndex == 1
        let targetUrl = getTargetUrl(isAntennas: isAntennas)
        let title = isAntennas ? "Antenna Controls" : "Live Clock"
        qrInfoLabel.text = "\(title):\n\(targetUrl)\n(Tap to copy)"

        if let qrImage = HamClockBridge.shared().generateQRCodeImage(forText: targetUrl, scale: 6, border: 2) {
            qrImageView.image = qrImage
        }
    }

    @objc private func copyQrUrlTapped() {
        let isAntennas = qrSegmentedControl.selectedSegmentIndex == 1
        let targetUrl = getTargetUrl(isAntennas: isAntennas)
        UIPasteboard.general.string = targetUrl

        let alert = UIAlertController(title: nil, message: "URL copied to clipboard", preferredStyle: .alert)
        present(alert, animated: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            alert.dismiss(animated: true)
        }
    }

    private func getLocalIPAddress() -> String? {
        var address: String?
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0 else { return nil }
        guard let firstAddr = ifaddr else { return nil }

        for ptr in sequence(first: firstAddr, next: { $0.pointee.ifa_next }) {
            let flags = Int32(ptr.pointee.ifa_flags)
            let addr = ptr.pointee.ifa_addr.pointee
            if (flags & (IFF_UP|IFF_RUNNING|IFF_LOOPBACK)) == (IFF_UP|IFF_RUNNING) {
                if addr.sa_family == UInt8(AF_INET) {
                    var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                    if getnameinfo(ptr.pointee.ifa_addr, socklen_t(addr.sa_len),
                                   &hostname, socklen_t(hostname.count),
                                   nil, socklen_t(0), NI_NUMERICHOST) == 0 {
                        address = String(cString: hostname)
                        break
                    }
                }
            }
        }
        freeifaddrs(ifaddr)
        return address
    }

    @objc private func saveAndDismiss() {
        let defaults = UserDefaults.standard
        let host = hostTextField.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        let allow = allowExternalSwitch.isOn
        let mdns = mdnsTextField.text?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "HamClock"

        defaults.set(host, forKey: "backend_host")
        defaults.set(allow, forKey: "allow_external_access")
        defaults.set(mdns, forKey: "mdns_name")

        delegate?.settingsDidChangeBackendHost(host)
        delegate?.settingsDidChangeAllowExternal(allow)
        delegate?.settingsDidChangeMdnsName(mdns)

        dismiss(animated: true)
    }

    @objc private func cancelDismiss() {
        dismiss(animated: true)
    }

    @objc private func restartSetupTapped() {
        dismiss(animated: true) { [weak self] in
            self?.delegate?.settingsDidRequestRestart(forceSetup: true, countdown: false)
        }
    }

    @objc private func restartCountdownTapped() {
        dismiss(animated: true) { [weak self] in
            self?.delegate?.settingsDidRequestRestart(forceSetup: false, countdown: true)
        }
    }
}
