package org.openhamclock.hamclock

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream

class MdnsResponderTest {

    private val myIp = byteArrayOf(192.toByte(), 168.toByte(), 1.toByte(), 42.toByte())
    private val otherIp = byteArrayOf(192.toByte(), 168.toByte(), 1.toByte(), 99.toByte())

    private fun buildQuery(host: String, qtype: Int = MdnsResponder.TYPE_A, id: Int = 0): ByteArray {
        val baos = ByteArrayOutputStream()
        val dos = DataOutputStream(baos)
        dos.writeShort(id)
        dos.writeShort(0x0000)
        dos.writeShort(1)
        dos.writeShort(0)
        dos.writeShort(0)
        dos.writeShort(0)
        for (label in host.split(".")) {
            val b = label.toByteArray(Charsets.UTF_8)
            dos.writeByte(b.size)
            dos.write(b)
        }
        dos.writeByte(0)
        dos.writeShort(qtype)
        dos.writeShort(MdnsResponder.CLASS_IN)
        dos.flush()
        return baos.toByteArray()
    }

    private fun buildAaaaResponse(host: String): ByteArray {
        val baos = ByteArrayOutputStream()
        val dos = DataOutputStream(baos)
        dos.writeShort(0)
        dos.writeShort(0x8400)
        dos.writeShort(0)
        dos.writeShort(1)
        dos.writeShort(0)
        dos.writeShort(0)
        for (label in host.split(".")) {
            val b = label.toByteArray(Charsets.UTF_8)
            dos.writeByte(b.size)
            dos.write(b)
        }
        dos.writeByte(0)
        dos.writeShort(28) // AAAA
        dos.writeShort(MdnsResponder.CLASS_IN)
        dos.writeInt(120)
        dos.writeShort(16)
        dos.write(ByteArray(16) { 1 })
        dos.flush()
        return baos.toByteArray()
    }

    @Test
    fun testNormalizeBaseName() {
        assertEquals("hamclock", MdnsResponder.normalizeBaseName(null))
        assertEquals("hamclock", MdnsResponder.normalizeBaseName(""))
        assertEquals("hamclock", MdnsResponder.normalizeBaseName("   "))
        assertEquals("shack", MdnsResponder.normalizeBaseName("  SHACK  "))
        assertEquals("shack", MdnsResponder.normalizeBaseName("shack.local"))
        assertEquals("hamclock", MdnsResponder.normalizeBaseName(".local"))
    }

    @Test
    fun testCompressedDnsNameParsing() {
        // Header (12 bytes) + "hamclock.local\0" at offset 12 + pointer (0xC0 0x0C)
        val baos = ByteArrayOutputStream()
        val dos = DataOutputStream(baos)
        dos.write(ByteArray(12))
        dos.writeByte(8); dos.writeBytes("hamclock")
        dos.writeByte(5); dos.writeBytes("local")
        dos.writeByte(0)
        val ptrOffset = baos.size()
        dos.writeByte(0xC0)
        dos.writeByte(12)
        dos.flush()

        val bytes = baos.toByteArray()
        val parsed = MdnsResponder.readDnsName(bytes, bytes.size, ptrOffset)
        assertNotNull(parsed)
        assertEquals("hamclock.local", parsed!!.first)
        assertEquals(ptrOffset + 2, parsed.second)
    }

    @Test
    fun testProbeThenAnnounceNoConflict() {
        val events = mutableListOf<Pair<MdnsResponder.MdnsState, String>>()
        val session = MdnsResponder.MdnsSession("hamclock", listOf(myIp)) { s, h ->
            events.add(s to h)
        }
        var t = 1000L
        session.start(t)
        assertEquals("hamclock.local", session.currentHost())
        assertEquals(MdnsResponder.MdnsState.PROBING, session.publicState())

        // Too early (before INITIAL_PROBE_DELAY_MS)
        assertTrue(session.step(t).isEmpty())

        // 3 probes
        t += MdnsResponder.INITIAL_PROBE_DELAY_MS
        for (i in 0 until MdnsResponder.PROBE_COUNT) {
            val pkts = session.step(t)
            assertEquals(1, pkts.size)
            val parsed = MdnsResponder.parsePacket(pkts[0], pkts[0].size)
            assertNotNull(parsed)
            assertFalse(parsed!!.isResponse)
            assertEquals(2, parsed.questions.size)
            assertEquals(1, parsed.records.size)
            t += MdnsResponder.PROBE_INTERVAL_MS
        }

        // Still probing until 250ms after 3rd probe, then transitions & sends 1st announcement
        val announce1 = session.step(t)
        assertEquals(1, announce1.size)
        assertTrue(session.announced)
        assertEquals(MdnsResponder.MdnsState.ANNOUNCED, session.publicState())
        assertEquals(
            listOf(
                MdnsResponder.MdnsState.PROBING to "hamclock.local",
                MdnsResponder.MdnsState.ANNOUNCED to "hamclock.local"
            ),
            events
        )

        // 2nd announcement
        t += MdnsResponder.ANNOUNCE_INTERVAL_MS
        val announce2 = session.step(t)
        assertEquals(1, announce2.size)
        assertEquals(MdnsResponder.MdnsSession.InternalState.RUNNING, session.internalState)
    }

    @Test
    fun testConflictDuringProbingRenamesToDash2() {
        val events = mutableListOf<Pair<MdnsResponder.MdnsState, String>>()
        val session = MdnsResponder.MdnsSession("hamclock", listOf(myIp)) { s, h ->
            events.add(s to h)
        }
        var t = 1000L
        session.start(t)
        t += MdnsResponder.INITIAL_PROBE_DELAY_MS
        assertEquals(1, session.step(t).size) // 1st probe

        // Another device responds with hamclock.local -> otherIp
        val conflictPkt = MdnsResponder.buildResponsePacket("hamclock.local", otherIp)
        val (reply, _) = session.handleIncomingPacket(conflictPkt, conflictPkt.size, MdnsResponder.MDNS_PORT, t + 10)
        assertNull(reply) // Not announced yet, so no goodbye packet
        assertEquals(2, session.suffix)
        assertEquals("hamclock-2.local", session.currentHost())
        assertFalse(session.announced)

        // Advance through 3 probes for hamclock-2.local
        t += 10 + MdnsResponder.CONFLICT_RETRY_DELAY_MS
        for (i in 0 until MdnsResponder.PROBE_COUNT) {
            assertEquals(1, session.step(t).size)
            t += MdnsResponder.PROBE_INTERVAL_MS
        }
        assertEquals(1, session.step(t).size) // 1st announcement for hamclock-2.local
        assertTrue(session.announced)
        assertEquals("hamclock-2.local", session.currentHost())
        assertEquals(MdnsResponder.MdnsState.ANNOUNCED to "hamclock-2.local", events.last())
    }

    @Test
    fun testAaaaResponseConflictsWhileProbing() {
        val session = MdnsResponder.MdnsSession("hamclock", listOf(myIp))
        val t = 1000L
        session.start(t)
        val aaaa = buildAaaaResponse("hamclock.local")
        session.handleIncomingPacket(aaaa, aaaa.size, MdnsResponder.MDNS_PORT, t + 50)
        assertEquals(2, session.suffix)
        assertEquals("hamclock-2.local", session.currentHost())
    }

    @Test
    fun testOwnIpResponseDoesNotConflict() {
        val session = MdnsResponder.MdnsSession("hamclock", listOf(myIp))
        val t = 1000L
        session.start(t)
        val ownPkt = MdnsResponder.buildResponsePacket("hamclock.local", myIp)
        session.handleIncomingPacket(ownPkt, ownPkt.size, MdnsResponder.MDNS_PORT, t + 30)
        assertEquals(1, session.suffix)
        assertEquals("hamclock.local", session.currentHost())
    }

    @Test
    fun testConflictWhileAnnouncedSendsGoodbyeAndRenames() {
        val session = MdnsResponder.MdnsSession("hamclock", listOf(myIp))
        var t = 1000L
        session.start(t)
        t += MdnsResponder.INITIAL_PROBE_DELAY_MS
        repeat(MdnsResponder.PROBE_COUNT) {
            session.step(t)
            t += MdnsResponder.PROBE_INTERVAL_MS
        }
        session.step(t) // announced
        assertTrue(session.announced)

        val conflictPkt = MdnsResponder.buildResponsePacket("hamclock.local", otherIp)
        val (goodbye, unicast) = session.handleIncomingPacket(conflictPkt, conflictPkt.size, MdnsResponder.MDNS_PORT, t + 50)
        assertNotNull(goodbye)
        assertFalse(unicast)
        val parsedGoodbye = MdnsResponder.parsePacket(goodbye!!, goodbye.size)!!
        assertEquals(0L, parsedGoodbye.records[0].ttl)
        assertEquals("hamclock.local", parsedGoodbye.records[0].name)
        assertEquals(2, session.suffix)
        assertEquals("hamclock-2.local", session.currentHost())
        assertFalse(session.announced)
    }

    @Test
    fun testQueryIgnoredWhileProbingAndAnsweredAfterAnnounced() {
        val session = MdnsResponder.MdnsSession("hamclock", listOf(myIp))
        var t = 1000L
        session.start(t)

        // Query while probing -> ignored
        val q = buildQuery("hamclock.local")
        assertNull(session.handleIncomingPacket(q, q.size, MdnsResponder.MDNS_PORT, t + 10).first)

        // Conflict -> hamclock-2.local, finish probing
        val conflict = MdnsResponder.buildResponsePacket("hamclock.local", otherIp)
        t += 20
        session.handleIncomingPacket(conflict, conflict.size, MdnsResponder.MDNS_PORT, t)
        t += MdnsResponder.CONFLICT_RETRY_DELAY_MS
        repeat(MdnsResponder.PROBE_COUNT) {
            session.step(t)
            t += MdnsResponder.PROBE_INTERVAL_MS
        }
        session.step(t) // announced hamclock-2.local

        // Query for hamclock.local -> should NOT answer!
        assertNull(session.handleIncomingPacket(q, q.size, MdnsResponder.MDNS_PORT, t + 10).first)

        // Query for hamclock-2.local -> answered
        val q2 = buildQuery("HAMCLOCK-2.local", id = 0x1234)
        val (resp, unicast) = session.handleIncomingPacket(q2, q2.size, 54321, t + 10)
        assertNotNull(resp)
        assertTrue(unicast)
        val parsed = MdnsResponder.parsePacket(resp!!, resp.size)!!
        assertEquals(0x1234, parsed.id)
        assertEquals(1, parsed.questions.size)
        assertEquals(1, parsed.records.size)
        assertEquals("hamclock-2.local", parsed.records[0].name)
        assertArrayEquals(myIp, parsed.records[0].rdata)
        assertTrue(parsed.records[0].ttl <= 10L)
    }
}

