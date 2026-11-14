package org.openhamclock.hamclock

import android.content.Context
import android.net.wifi.WifiManager
import android.os.SystemClock
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.DataOutputStream
import java.net.DatagramPacket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.MulticastSocket
import java.net.NetworkInterface
import java.util.concurrent.atomic.AtomicBoolean

class MdnsResponder(private val context: Context) {

    enum class MdnsState {
        OFF,
        PROBING,
        ANNOUNCED
    }

    data class DnsQuestion(val name: String, val type: Int, val qclass: Int)
    data class DnsRecord(val name: String, val type: Int, val ttl: Long, val rdata: ByteArray)
    data class ParsedPacket(
        val id: Int,
        val isResponse: Boolean,
        val questions: List<DnsQuestion>,
        val records: List<DnsRecord>
    )

    internal class MdnsSession(
        rawBaseName: String,
        private var localIps: List<ByteArray>,
        private val onStatusChanged: ((MdnsState, String) -> Unit)? = null
    ) {
        enum class InternalState {
            PROBING,
            ANNOUNCING,
            RUNNING
        }

        val baseName: String = normalizeBaseName(rawBaseName)
        var suffix: Int = 1
            private set
        var internalState: InternalState = InternalState.PROBING
            private set
        var announced: Boolean = false
            private set
        private var sentCount: Int = 0
        private var nextActionAtMs: Long = 0L

        fun currentHost(): String = if (suffix > 1) "$baseName-$suffix.local" else "$baseName.local"

        fun publicState(): MdnsState = if (announced) MdnsState.ANNOUNCED else MdnsState.PROBING

        fun start(nowMs: Long) {
            suffix = 1
            announced = false
            internalState = InternalState.PROBING
            sentCount = 0
            nextActionAtMs = nowMs + INITIAL_PROBE_DELAY_MS
            onStatusChanged?.invoke(MdnsState.PROBING, currentHost())
        }

        fun updateLocalIps(newIps: List<ByteArray>) {
            localIps = newIps
        }

        fun step(nowMs: Long): List<ByteArray> {
            val primaryIp = localIps.firstOrNull() ?: return emptyList()
            if (nowMs < nextActionAtMs) return emptyList()

            return when (internalState) {
                InternalState.PROBING -> {
                    if (sentCount < PROBE_COUNT) {
                        sentCount++
                        nextActionAtMs = nowMs + PROBE_INTERVAL_MS
                        listOf(buildProbePacket(currentHost(), primaryIp))
                    } else {
                        internalState = InternalState.ANNOUNCING
                        sentCount = 0
                        nextActionAtMs = nowMs
                        // Immediately send the first announcement
                        sendAnnouncementStep(nowMs, primaryIp)
                    }
                }
                InternalState.ANNOUNCING -> {
                    sendAnnouncementStep(nowMs, primaryIp)
                }
                InternalState.RUNNING -> emptyList()
            }
        }

        private fun sendAnnouncementStep(nowMs: Long, primaryIp: ByteArray): List<ByteArray> {
            val pkt = buildResponsePacket(currentHost(), primaryIp, ttl = DEFAULT_TTL)
            sentCount++
            if (!announced) {
                announced = true
                onStatusChanged?.invoke(MdnsState.ANNOUNCED, currentHost())
            }
            if (sentCount >= ANNOUNCE_COUNT) {
                internalState = InternalState.RUNNING
            } else {
                nextActionAtMs = nowMs + ANNOUNCE_INTERVAL_MS
            }
            return listOf(pkt)
        }

        fun buildGoodbyeIfAnnounced(): ByteArray? {
            if (!announced) return null
            val primaryIp = localIps.firstOrNull() ?: return null
            return buildResponsePacket(currentHost(), primaryIp, ttl = 0)
        }

        /**
         * Handles an incoming mDNS packet.
         * Returns Pair(packetToSend, sendUnicastToSender).
         */
        fun handleIncomingPacket(
            buf: ByteArray,
            length: Int,
            srcPort: Int,
            nowMs: Long
        ): Pair<ByteArray?, Boolean> {
            val packet = parsePacket(buf, length) ?: return Pair(null, false)
            if (packet.isResponse) {
                val goodbye = checkConflicts(packet.records, nowMs)
                return Pair(goodbye, false)
            }

            if (internalState == InternalState.PROBING) {
                return Pair(null, false)
            }

            val host = currentHost()
            val matching = packet.questions.filter {
                it.name.equals(host, ignoreCase = true) && (it.type == TYPE_A || it.type == TYPE_ANY)
            }
            if (matching.isEmpty()) return Pair(null, false)

            val primaryIp = localIps.firstOrNull() ?: return Pair(null, false)
            val legacyUnicast = srcPort != MDNS_PORT
            val response = buildResponsePacket(
                hostFqdn = host,
                ipBytes = primaryIp,
                ttl = DEFAULT_TTL,
                id = if (legacyUnicast) packet.id else 0,
                questions = if (legacyUnicast) packet.questions else emptyList(),
                legacyUnicast = legacyUnicast
            )
            return Pair(response, legacyUnicast)
        }

        private fun checkConflicts(records: List<DnsRecord>, nowMs: Long): ByteArray? {
            val host = currentHost()
            for (rec in records) {
                if (!rec.name.equals(host, ignoreCase = true)) continue
                val isOurs = when {
                    rec.type == TYPE_A -> {
                        rec.rdata.size == 4 && localIps.any { it.contentEquals(rec.rdata) }
                    }
                    internalState == InternalState.PROBING -> {
                        // While probing, any other record (e.g. AAAA, NSEC) for this host means it is taken
                        false
                    }
                    else -> true
                }
                if (!isOurs) {
                    val goodbye = buildGoodbyeIfAnnounced()
                    announced = false
                    suffix = if (suffix >= MAX_SUFFIX) 1 else suffix + 1
                    internalState = InternalState.PROBING
                    sentCount = 0
                    nextActionAtMs = nowMs + CONFLICT_RETRY_DELAY_MS
                    onStatusChanged?.invoke(MdnsState.PROBING, currentHost())
                    return goodbye
                }
            }
            return null
        }
    }

    private var multicastSocket: MulticastSocket? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var workerThread: Thread? = null
    private val isRunning = AtomicBoolean(false)

    @Volatile
    var onStatusChanged: ((MdnsState, String) -> Unit)? = null

    fun start(hostName: String) {
        stop()

        val baseName = normalizeBaseName(hostName)

        val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
        multicastLock = wifiManager?.createMulticastLock("HamClock:MdnsLock")?.apply {
            setReferenceCounted(false)
            acquire()
            Log.i(TAG, "Acquired MulticastLock for mDNS")
        }

        isRunning.set(true)
        workerThread = Thread({
            runResponder(baseName)
        }, "MdnsResponderThread").apply {
            isDaemon = true
            start()
        }
        Log.i(TAG, "Started mDNS A-record responder for $baseName.local")
    }

    fun stop() {
        isRunning.set(false)
        try {
            multicastSocket?.close()
        } catch (e: Exception) {
            // Ignore close exceptions
        }
        multicastSocket = null

        workerThread?.interrupt()
        workerThread = null

        multicastLock?.let {
            if (it.isHeld) {
                it.release()
                Log.i(TAG, "Released MulticastLock")
            }
        }
        multicastLock = null
    }

    private fun getLocalIpv4Addresses(): List<InetAddress> {
        val result = mutableListOf<InetAddress>()
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces() ?: return result
            for (iface in interfaces) {
                if (iface.isLoopback || !iface.isUp) continue
                for (addr in iface.inetAddresses) {
                    if (!addr.isLoopbackAddress && addr is Inet4Address) {
                        result.add(addr)
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "Error getting local IP: ${e.message}")
        }
        return result
    }

    private fun runResponder(baseName: String) {
        val group = InetAddress.getByName(MDNS_GROUP)
        val buf = ByteArray(9000)

        val initialIps = getLocalIpv4Addresses().map { it.address }
        val session = MdnsSession(baseName, initialIps) { state, host ->
            Log.i(TAG, "mDNS state=$state host=$host")
            onStatusChanged?.invoke(state, host)
        }
        var lastIpCheckMs = SystemClock.elapsedRealtime()
        session.start(lastIpCheckMs)

        try {
            val socket = MulticastSocket(MDNS_PORT).apply {
                reuseAddress = true
                joinGroup(group)
                timeToLive = 255
                soTimeout = SOCKET_POLL_MS
            }
            multicastSocket = socket

            while (isRunning.get() && !socket.isClosed) {
                val now = SystemClock.elapsedRealtime()
                if (now - lastIpCheckMs > IP_REFRESH_INTERVAL_MS || session.announced.not() && getLocalIpv4Addresses().isNotEmpty()) {
                    if (now - lastIpCheckMs > IP_REFRESH_INTERVAL_MS) {
                        lastIpCheckMs = now
                        session.updateLocalIps(getLocalIpv4Addresses().map { it.address })
                    }
                }

                for (pkt in session.step(now)) {
                    val dgram = DatagramPacket(pkt, pkt.size, group, MDNS_PORT)
                    socket.send(dgram)
                }

                val packet = DatagramPacket(buf, buf.size)
                try {
                    socket.receive(packet)
                } catch (_: java.net.SocketTimeoutException) {
                    continue
                } catch (e: Exception) {
                    if (!isRunning.get() || socket.isClosed) break
                    continue
                }

                val (reply, unicast) = session.handleIncomingPacket(
                    buf = buf,
                    length = packet.length,
                    srcPort = packet.port,
                    nowMs = SystemClock.elapsedRealtime()
                )
                if (reply != null) {
                    val dstAddr = if (unicast) packet.address else group
                    val dstPort = if (unicast) packet.port else MDNS_PORT
                    socket.send(DatagramPacket(reply, reply.size, dstAddr, dstPort))
                }
            }

            if (!socket.isClosed) {
                session.buildGoodbyeIfAnnounced()?.let { goodbye ->
                    try {
                        socket.send(DatagramPacket(goodbye, goodbye.size, group, MDNS_PORT))
                    } catch (_: Exception) {
                    }
                }
            }
        } catch (e: Exception) {
            if (isRunning.get()) {
                Log.w(TAG, "mDNS socket loop exited: ${e.message}")
            }
        }
    }

    companion object {
        private const val TAG = "MdnsResponder"
        const val MDNS_GROUP = "224.0.0.251"
        const val MDNS_PORT = 5353

        const val TYPE_A = 1
        const val TYPE_PTR = 12
        const val TYPE_TXT = 16
        const val TYPE_SRV = 33
        const val TYPE_ANY = 255

        const val CLASS_IN = 1
        const val CLASS_FLUSH = 0x8000
        const val Q_UNICAST = 0x8000

        const val DEFAULT_TTL = 120
        const val MAX_SUFFIX = 20
        const val PROBE_COUNT = 3
        const val ANNOUNCE_COUNT = 2
        const val INITIAL_PROBE_DELAY_MS = 20L
        const val PROBE_INTERVAL_MS = 250L
        const val ANNOUNCE_INTERVAL_MS = 1000L
        const val CONFLICT_RETRY_DELAY_MS = 250L
        private const val SOCKET_POLL_MS = 100
        private const val IP_REFRESH_INTERVAL_MS = 5000L

        fun normalizeBaseName(raw: String?): String {
            val trimmed = raw?.trim()?.lowercase() ?: ""
            val stripped = trimmed.removeSuffix(".local").trim('.')
            return stripped.ifEmpty { "hamclock" }
        }

        fun readDnsName(buf: ByteArray, totalLen: Int, startOffset: Int): Pair<String, Int>? {
            var pos = startOffset
            var endOffset = -1
            var jumped = false
            val sb = StringBuilder()

            for (hop in 0 until 32) {
                if (pos < 0 || pos >= totalLen) return null
                val len = buf[pos].toInt() and 0xFF
                if (len == 0) {
                    if (!jumped) endOffset = pos + 1
                    return Pair(sb.toString(), endOffset)
                }
                if ((len and 0xC0) == 0xC0) {
                    if (pos + 1 >= totalLen) return null
                    val ptr = ((len and 0x3F) shl 8) or (buf[pos + 1].toInt() and 0xFF)
                    if (!jumped) endOffset = pos + 2
                    jumped = true
                    pos = ptr
                    continue
                }
                if ((len and 0xC0) != 0) return null
                pos++
                if (pos + len > totalLen) return null
                if (sb.isNotEmpty()) sb.append('.')
                sb.append(String(buf, pos, len, Charsets.UTF_8))
                pos += len
            }
            return null
        }

        fun parsePacket(buf: ByteArray, totalLen: Int): ParsedPacket? {
            if (totalLen < 12) return null
            val id = ((buf[0].toInt() and 0xFF) shl 8) or (buf[1].toInt() and 0xFF)
            val flags = ((buf[2].toInt() and 0xFF) shl 8) or (buf[3].toInt() and 0xFF)
            val isResponse = (flags and 0x8000) != 0
            val qdCount = ((buf[4].toInt() and 0xFF) shl 8) or (buf[5].toInt() and 0xFF)
            val anCount = ((buf[6].toInt() and 0xFF) shl 8) or (buf[7].toInt() and 0xFF)
            val nsCount = ((buf[8].toInt() and 0xFF) shl 8) or (buf[9].toInt() and 0xFF)
            val arCount = ((buf[10].toInt() and 0xFF) shl 8) or (buf[11].toInt() and 0xFF)

            var off = 12
            val questions = ArrayList<DnsQuestion>(qdCount.coerceAtMost(64))
            for (i in 0 until qdCount) {
                val (name, nextOff) = readDnsName(buf, totalLen, off) ?: return null
                if (nextOff + 4 > totalLen) return null
                val qtype = ((buf[nextOff].toInt() and 0xFF) shl 8) or (buf[nextOff + 1].toInt() and 0xFF)
                val qclass = ((buf[nextOff + 2].toInt() and 0xFF) shl 8) or (buf[nextOff + 3].toInt() and 0xFF)
                questions.add(DnsQuestion(name, qtype, qclass))
                off = nextOff + 4
            }

            val rrTotal = anCount + nsCount + arCount
            val records = ArrayList<DnsRecord>(rrTotal.coerceAtMost(128))
            for (i in 0 until rrTotal) {
                val (name, nextOff) = readDnsName(buf, totalLen, off) ?: return null
                if (nextOff + 10 > totalLen) return null
                val rtype = ((buf[nextOff].toInt() and 0xFF) shl 8) or (buf[nextOff + 1].toInt() and 0xFF)
                val ttl = (((buf[nextOff + 4].toLong() and 0xFF) shl 24) or
                    ((buf[nextOff + 5].toLong() and 0xFF) shl 16) or
                    ((buf[nextOff + 6].toLong() and 0xFF) shl 8) or
                    (buf[nextOff + 7].toLong() and 0xFF))
                val rdlen = ((buf[nextOff + 8].toInt() and 0xFF) shl 8) or (buf[nextOff + 9].toInt() and 0xFF)
                val rdataStart = nextOff + 10
                if (rdataStart + rdlen > totalLen) return null
                val rdata = buf.copyOfRange(rdataStart, rdataStart + rdlen)
                records.add(DnsRecord(name, rtype, ttl, rdata))
                off = rdataStart + rdlen
            }

            return ParsedPacket(id, isResponse, questions, records)
        }

        private fun writeDnsName(dos: DataOutputStream, fqdn: String) {
            val labels = fqdn.split(".")
            for (label in labels) {
                if (label.isEmpty()) continue
                val bytes = label.toByteArray(Charsets.UTF_8)
                val len = bytes.size.coerceAtMost(63)
                dos.writeByte(len)
                dos.write(bytes, 0, len)
            }
            dos.writeByte(0x00)
        }

        fun buildProbePacket(hostFqdn: String, ipBytes: ByteArray): ByteArray {
            val baos = ByteArrayOutputStream()
            val dos = DataOutputStream(baos)

            dos.writeShort(0x0000) // ID
            dos.writeShort(0x0000) // Flags: Query
            dos.writeShort(2)      // QDCOUNT
            dos.writeShort(0)      // ANCOUNT
            dos.writeShort(1)      // NSCOUNT (proposed A record)
            dos.writeShort(0)      // ARCOUNT

            writeDnsName(dos, hostFqdn)
            dos.writeShort(TYPE_ANY)
            dos.writeShort(CLASS_IN or Q_UNICAST)

            writeDnsName(dos, hostFqdn)
            dos.writeShort(TYPE_A)
            dos.writeShort(CLASS_IN or Q_UNICAST)

            writeDnsName(dos, hostFqdn)
            dos.writeShort(TYPE_A)
            dos.writeShort(CLASS_IN)
            dos.writeInt(DEFAULT_TTL)
            dos.writeShort(4)
            dos.write(ipBytes, 0, 4)

            dos.flush()
            return baos.toByteArray()
        }

        fun buildResponsePacket(
            hostFqdn: String,
            ipBytes: ByteArray,
            ttl: Int = DEFAULT_TTL,
            id: Int = 0,
            questions: List<DnsQuestion> = emptyList(),
            legacyUnicast: Boolean = false
        ): ByteArray {
            val baos = ByteArrayOutputStream()
            val dos = DataOutputStream(baos)

            dos.writeShort(if (legacyUnicast) id else 0x0000)
            dos.writeShort(0x8400) // Response + Authoritative (AA)
            dos.writeShort(if (legacyUnicast) questions.size else 0)
            dos.writeShort(1)      // ANCOUNT
            dos.writeShort(0)      // NSCOUNT
            dos.writeShort(0)      // ARCOUNT

            if (legacyUnicast) {
                for (q in questions) {
                    writeDnsName(dos, q.name)
                    dos.writeShort(q.type)
                    dos.writeShort(CLASS_IN)
                }
            }

            writeDnsName(dos, hostFqdn)
            dos.writeShort(TYPE_A)
            dos.writeShort(if (legacyUnicast) CLASS_IN else (CLASS_IN or CLASS_FLUSH))
            dos.writeInt(if (legacyUnicast) ttl.coerceAtMost(10) else ttl)
            dos.writeShort(4)
            dos.write(ipBytes, 0, 4)

            dos.flush()
            return baos.toByteArray()
        }
    }
}
