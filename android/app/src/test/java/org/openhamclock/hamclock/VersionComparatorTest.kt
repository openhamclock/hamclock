package org.openhamclock.hamclock

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class VersionComparatorTest {

    @Test
    fun testParseStable() {
        val v1 = VersionComparator.parse("4.32")
        assertNotNull(v1)
        assertEquals(4, v1!!.major)
        assertEquals(32, v1.minor)
        assertEquals(0, v1.patch)
        assertNull(v1.beta)

        val v2 = VersionComparator.parse("v4.32.0")
        assertNotNull(v2)
        assertEquals(4, v2!!.major)
        assertEquals(32, v2.minor)
        assertEquals(0, v2.patch)
        assertNull(v2.beta)

        val v3 = VersionComparator.parse("V4.32.1")
        assertNotNull(v3)
        assertEquals(4, v3!!.major)
        assertEquals(32, v3.minor)
        assertEquals(1, v3.patch)
        assertNull(v3.beta)
    }

    @Test
    fun testParseBeta() {
        val v1 = VersionComparator.parse("4.32b00")
        assertNotNull(v1)
        assertEquals(4, v1!!.major)
        assertEquals(32, v1.minor)
        assertEquals(0, v1.patch)
        assertEquals(0, v1.beta)

        val v2 = VersionComparator.parse("v4.32b01.0")
        assertNotNull(v2)
        assertEquals(4, v2!!.major)
        assertEquals(32, v2.minor)
        assertEquals(0, v2.patch)
        assertEquals(1, v2.beta)
    }

    @Test
    fun testParseInvalidAndEdge() {
        assertNull(VersionComparator.parse("edge"))
        assertNull(VersionComparator.parse(""))
        assertNull(VersionComparator.parse(null))
        assertNull(VersionComparator.parse("invalid"))
    }

    @Test
    fun testIsNewerStableFlow() {
        // Same version
        assertFalse(VersionComparator.isNewer("4.32", "4.32"))
        assertFalse(VersionComparator.isNewer("v4.32.0", "4.32"))
        assertFalse(VersionComparator.isNewer("4.32", "v4.32.0"))

        // Older version
        assertFalse(VersionComparator.isNewer("4.32", "4.31"))
        assertFalse(VersionComparator.isNewer("v4.32.1", "4.32.0"))

        // Newer version
        assertTrue(VersionComparator.isNewer("4.32", "4.33"))
        assertTrue(VersionComparator.isNewer("v4.32.0", "4.33.0"))
        assertTrue(VersionComparator.isNewer("4.32.0", "4.32.1"))
        assertTrue(VersionComparator.isNewer("3.99", "4.00"))

        // Stable users should NOT get beta updates
        assertFalse(VersionComparator.isNewer("4.32", "4.33b00"))
    }

    @Test
    fun testIsNewerBetaFlow() {
        // Newer beta
        assertTrue(VersionComparator.isNewer("4.32b00", "4.32b01"))
        assertTrue(VersionComparator.isNewer("v4.32b00.0", "v4.32b01.0"))

        // Final stable of current beta
        assertTrue(VersionComparator.isNewer("4.32b00", "4.32"))
        assertTrue(VersionComparator.isNewer("v4.32b01.0", "4.32.0"))

        // Older beta
        assertFalse(VersionComparator.isNewer("4.32b01", "4.32b00"))
        assertFalse(VersionComparator.isNewer("4.32", "4.32b00"))
    }

    @Test
    fun testEdgeBuild() {
        // Dev builds return false to avoid unintended prompts
        assertFalse(VersionComparator.isNewer("edge", "4.33"))
    }
}
