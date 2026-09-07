//
//  LocationService.swift
//  HamClock
//
//  Host location coordinator for seeding HamClock DE coordinates.
//

import Foundation
import CoreLocation

protocol LocationServiceDelegate: AnyObject {
    func locationServiceDidUpdateLocation(latitude: Double, longitude: Double)
    func locationServiceDidFail(with error: Error)
}

class LocationService: NSObject, CLLocationManagerDelegate {
    static let shared = LocationService()

    weak var delegate: LocationServiceDelegate?
    private let locationManager = CLLocationManager()

    private(set) var lastLatitude: Double?
    private(set) var lastLongitude: Double?
    private(set) var hasLocation: Bool = false

    override private init() {
        super.init()
        locationManager.delegate = self
        locationManager.desiredAccuracy = kCLLocationAccuracyKilometer
    }

    func requestLocationPermission() {
        let status = locationManager.authorizationStatus
        if status == .notDetermined {
            locationManager.requestWhenInUseAuthorization()
        } else if status == .authorizedWhenInUse || status == .authorizedAlways {
            locationManager.requestLocation()
        }
    }

    func locationManagerDidChangeAuthorization(_ manager: CLLocationManager) {
        let status = manager.authorizationStatus
        if status == .authorizedWhenInUse || status == .authorizedAlways {
            manager.requestLocation()
        }
    }

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        guard let location = locations.last else { return }
        lastLatitude = location.coordinate.latitude
        lastLongitude = location.coordinate.longitude
        hasLocation = true
        delegate?.locationServiceDidUpdateLocation(latitude: location.coordinate.latitude,
                                                  longitude: location.coordinate.longitude)
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        NSLog("[LocationService] Location fetch failed: \(error.localizedDescription)")
        delegate?.locationServiceDidFail(with: error)
    }
}
