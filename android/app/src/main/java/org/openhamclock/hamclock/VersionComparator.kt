package org.openhamclock.hamclock

object VersionComparator {

    data class SemanticVersion(
        val major: Int,
        val minor: Int,
        val patch: Int,
        val beta: Int? = null // null means stable release
    )

    /**
     * Parses a version string into a SemanticVersion.
     * Supports formats like:
     * - "4.32" -> major=4, minor=32, patch=0, beta=null
     * - "v4.32.0" -> major=4, minor=32, patch=0, beta=null
     * - "4.32b00" -> major=4, minor=32, patch=0, beta=0
     * - "v4.32b01.0" -> major=4, minor=32, patch=0, beta=1
     */
    fun parse(versionStr: String?): SemanticVersion? {
        if (versionStr.isNullOrBlank()) return null
        val clean = versionStr.trim().removePrefix("v").removePrefix("V")
        if (clean.equals("edge", ignoreCase = true)) return null

        val regex = Regex("""^(\d+)\.(\d+)(?:b(\d+))?(?:\.(\d+))?.*$""")
        val match = regex.find(clean) ?: return null

        val major = match.groupValues[1].toIntOrNull() ?: return null
        val minor = match.groupValues[2].toIntOrNull() ?: return null
        val beta = if (match.groupValues[3].isNotEmpty()) {
            match.groupValues[3].toIntOrNull()
        } else null
        val patch = if (match.groupValues[4].isNotEmpty()) {
            match.groupValues[4].toIntOrNull() ?: 0
        } else 0

        return SemanticVersion(major, minor, patch, beta)
    }

    /**
     * Determines whether [remoteStr] is newer than [currentStr].
     *
     * Rules:
     * 1. If current version is invalid or "edge", returns false.
     * 2. If current is stable and remote is beta, returns false (stable users are not prompted for betas).
     * 3. If remote major.minor.patch is greater, returns true.
     * 4. If remote major.minor.patch is less, returns false.
     * 5. If major.minor.patch are equal:
     *    - If current is beta and remote is stable (e.g. current 4.32b00 vs remote 4.32), returns true.
     *    - If both are beta (e.g. 4.32b00 vs 4.32b01), returns remote.beta > current.beta.
     *    - If both are stable, returns false.
     */
    fun isNewer(currentStr: String?, remoteStr: String?): Boolean {
        val current = parse(currentStr) ?: return false
        val remote = parse(remoteStr) ?: return false

        val currentIsBeta = current.beta != null
        val remoteIsBeta = remote.beta != null

        // Stable users should never be offered beta versions
        if (!currentIsBeta && remoteIsBeta) {
            return false
        }

        if (remote.major != current.major) {
            return remote.major > current.major
        }
        if (remote.minor != current.minor) {
            return remote.minor > current.minor
        }
        if (remote.patch != current.patch) {
            return remote.patch > current.patch
        }

        // Major, minor, and patch are equal:
        if (currentIsBeta && !remoteIsBeta) {
            // Beta user upgraded to final stable release (e.g. 4.32b00 -> 4.32)
            return true
        }

        if (currentIsBeta && remoteIsBeta) {
            val curBeta = current.beta ?: 0
            val remBeta = remote.beta ?: 0
            return remBeta > curBeta
        }

        return false
    }
}
