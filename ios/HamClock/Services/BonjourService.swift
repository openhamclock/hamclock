//
//  BonjourService.swift
//  HamClock
//
//  Bonjour / mDNS service advertiser for local network discovery.
//

import Foundation

class BonjourService: NSObject, NetServiceDelegate {
    static let shared = BonjourService()

    private var hamclockService: NetService?
    private var httpService: NetService?

    func registerServices(name: String, restPort: Int32 = 8080, webPort: Int32 = 8081) {
        stopServices()

        let serviceName = name.isEmpty ? "HamClock" : name

        // Register _hamclock._tcp on REST port
        hamclockService = NetService(domain: "local.", type: "_hamclock._tcp.", name: serviceName, port: restPort)
        hamclockService?.delegate = self
        let txtDict: [String: Data] = [
            "version": "1.0".data(using: .utf8) ?? Data(),
            "web_port": "\(webPort)".data(using: .utf8) ?? Data()
        ]
        hamclockService?.setTXTRecord(NetService.data(fromTXTRecord: txtDict))
        hamclockService?.publish()

        // Register _http._tcp on Live Web port
        httpService = NetService(domain: "local.", type: "_http._tcp.", name: "\(serviceName)-Web", port: webPort)
        httpService?.delegate = self
        httpService?.publish()

        NSLog("[BonjourService] Published mDNS services as '\(serviceName)' (REST: \(restPort), Web: \(webPort))")
    }

    func stopServices() {
        hamclockService?.stop()
        hamclockService = nil
        httpService?.stop()
        httpService = nil
    }

    func netServiceDidPublish(_ sender: NetService) {
        NSLog("[BonjourService] Successfully published: \(sender.name) (\(sender.type))")
    }

    func netService(_ sender: NetService, didNotPublish errorDict: [String : NSNumber]) {
        NSLog("[BonjourService] Failed to publish \(sender.name): \(errorDict)")
    }
}
