package org.openhamclock.hamclock

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AutoStartHelperTest {

    @Test
    fun testNonTvAlwaysSupported() {
        // Non-TV devices (phones, tablets) allow start on boot regardless of API level
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = false, sdkInt = 21, isRootedOrCustom = false))
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = false, sdkInt = 28, isRootedOrCustom = false))
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = false, sdkInt = 29, isRootedOrCustom = false))
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = false, sdkInt = 34, isRootedOrCustom = false))
    }

    @Test
    fun testOlderTvSupported() {
        // TV devices running Android 9 or older (e.g. Fire OS 5, 6, 7) support start on boot
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 21, isRootedOrCustom = false))
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 25, isRootedOrCustom = false))
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 28, isRootedOrCustom = false))
    }

    @Test
    fun testModernTvNotSupportedUnlessRooted() {
        // Modern TV devices (Chromecast with Google TV, Fire OS 8, Android TV 10+)
        // strictly block background activity starts from BOOT_COMPLETED
        assertFalse(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 29, isRootedOrCustom = false))
        assertFalse(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 31, isRootedOrCustom = false))
        assertFalse(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 34, isRootedOrCustom = false))

        // Hypothetically rooted or custom ROM TV environments are permitted
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 29, isRootedOrCustom = true))
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 31, isRootedOrCustom = true))
        assertTrue(AutoStartHelper.isStartOnBootSupported(isTv = true, sdkInt = 34, isRootedOrCustom = true))
    }
}
