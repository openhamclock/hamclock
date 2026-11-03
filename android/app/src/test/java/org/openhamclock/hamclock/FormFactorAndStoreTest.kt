package org.openhamclock.hamclock

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class FormFactorAndStoreTest {

    @Test
    fun testTvDeviceDetection() {
        // Amazon Fire TV stick / cube with fire_tv hardware feature
        assertTrue(
            MainActivity.isTvDevice(
                hasFireTvFeature = true,
                model = "AFTMM",
                isLeanback = false,
                isUiTv = false
            )
        )

        // Fire TV model starting with AFT without system feature reported
        assertTrue(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "AFTS",
                isLeanback = false,
                isUiTv = false
            )
        )

        // Android TV / Google TV with Leanback feature
        assertTrue(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "SHIELD Android TV",
                isLeanback = true,
                isUiTv = false
            )
        )

        // Android TV / Google TV with UI mode television
        assertTrue(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "Chromecast",
                isLeanback = false,
                isUiTv = true
            )
        )

        // Amazon Fire Tablets (e.g. Fire HD 8 / 10, Fire 7) must NOT be detected as TV
        assertFalse(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "KFTRWI",
                isLeanback = false,
                isUiTv = false
            )
        )
        assertFalse(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "KFTUWI",
                isLeanback = false,
                isUiTv = false
            )
        )
        assertFalse(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "Fire HD 10",
                isLeanback = false,
                isUiTv = false
            )
        )

        // Standard Android phone or tablet (Samsung, Pixel, etc.)
        assertFalse(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "Pixel 8 Pro",
                isLeanback = false,
                isUiTv = false
            )
        )
        assertFalse(
            MainActivity.isTvDevice(
                hasFireTvFeature = false,
                model = "SM-X910",
                isLeanback = false,
                isUiTv = false
            )
        )
    }

    @Test
    fun testInstallSourceResolution() {
        // 1. Google Play Store install (package com.android.vending)
        assertEquals(
            AppUpdateHelper.InstallSource.GOOGLE_PLAY,
            AppUpdateHelper.determineInstallSource(
                installerPackage = "com.android.vending",
                manufacturer = "Google",
                brand = "Google",
                hasFireTvFeature = false
            )
        )

        // Google Play Store on Amazon hardware (e.g. side-installed Play Store on Fire tablet)
        assertEquals(
            AppUpdateHelper.InstallSource.GOOGLE_PLAY,
            AppUpdateHelper.determineInstallSource(
                installerPackage = "com.android.vending",
                manufacturer = "Amazon",
                brand = "Amazon",
                hasFireTvFeature = false
            )
        )

        // 2. Amazon Appstore install (package com.amazon.venezia)
        assertEquals(
            AppUpdateHelper.InstallSource.AMAZON_APPSTORE,
            AppUpdateHelper.determineInstallSource(
                installerPackage = "com.amazon.venezia",
                manufacturer = "Samsung",
                brand = "Samsung",
                hasFireTvFeature = false
            )
        )

        // Native Amazon hardware without package installer (e.g. sideloaded on Fire TV or Fire tablet)
        assertEquals(
            AppUpdateHelper.InstallSource.AMAZON_APPSTORE,
            AppUpdateHelper.determineInstallSource(
                installerPackage = null,
                manufacturer = "Amazon",
                brand = "Amazon",
                hasFireTvFeature = true
            )
        )

        // 3. Sideloaded / GitHub releases (null or generic package installer on non-Amazon device)
        assertEquals(
            AppUpdateHelper.InstallSource.OTHER,
            AppUpdateHelper.determineInstallSource(
                installerPackage = null,
                manufacturer = "Google",
                brand = "Google",
                hasFireTvFeature = false
            )
        )
        assertEquals(
            AppUpdateHelper.InstallSource.OTHER,
            AppUpdateHelper.determineInstallSource(
                installerPackage = "com.google.android.packageinstaller",
                manufacturer = "Samsung",
                brand = "Samsung",
                hasFireTvFeature = false
            )
        )
    }
}

