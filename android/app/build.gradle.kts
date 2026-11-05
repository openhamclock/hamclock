import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.zip.ZipFile

plugins {
    alias(libs.plugins.android.application)
}

fun getGitVersion(): Pair<String, Boolean> {
    val envTag = (System.getenv("APP_VERSION") ?: System.getenv("TAG"))?.trim()
    if (!envTag.isNullOrEmpty()) {
        return Pair(envTag, true)
    }
    return try {
        val process = ProcessBuilder("git", "describe", "--exact-match", "--tags")
            .directory(rootDir)
            .redirectErrorStream(true)
            .start()
        val tag = process.inputStream.bufferedReader().readText().trim()
        val exitCode = process.waitFor()
        if (exitCode == 0 && tag.isNotEmpty() && !tag.startsWith("fatal")) {
            Pair(tag, true)
        } else {
            Pair("edge", false)
        }
    } catch (e: Exception) {
        Pair("edge", false)
    }
}

fun getGitCommitCount(): Int {
    return try {
        val process = ProcessBuilder("git", "rev-list", "--count", "HEAD")
            .directory(rootDir)
            .redirectErrorStream(true)
            .start()
        val output = process.inputStream.bufferedReader().readText().trim()
        val exitCode = process.waitFor()
        if (exitCode == 0 && output.isNotEmpty()) {
            output.toInt()
        } else {
            1
        }
    } catch (e: Exception) {
        1
    }
}

fun hasLocalChanges(): Boolean {
    if (System.getenv("CI") == "true") {
        return false
    }
    return try {
        val process = ProcessBuilder("git", "diff-index", "--quiet", "HEAD", "--")
            .directory(rootDir)
            .start()
        process.waitFor() != 0
    } catch (e: Exception) {
        false
    }
}

val (gitTag, onTag) = getGitVersion()
val appVersion = if (onTag) gitTag else "edge"

// Refuse to build on a tag if there are local uncommitted changes
if (onTag && hasLocalChanges()) {
    throw GradleException(
        """
        
        ========================================================================
        ERROR: Cannot build release tag '$gitTag' with uncommitted local changes.
        Git tags must be built from a clean repository state matching the tag.
        Please commit, stash, or revert local changes before building tag '$gitTag'.
        ========================================================================
        """.trimIndent()
    )
}

// If on a tag, update ESPHamClock/version.cpp with stripped version (e.g. v4.02.1 -> 4.02)
if (onTag) {
    var hcTag = gitTag.removePrefix("v").removePrefix("V")
    if (hcTag.contains(".")) {
        hcTag = hcTag.substringBeforeLast(".")
    }
    val versionFile = file("${rootDir}/../ESPHamClock/version.cpp")
    if (versionFile.exists()) {
        val content = versionFile.readText()
        val updated = content.replace(Regex("""(hc_version\s*=\s*")[^"]+(")"""), "$1$hcTag$2")
        if (content != updated) {
            versionFile.writeText(updated)
        }
    }
}


// Automatically restore ESPHamClock/version.cpp at the end of the build so no git artifacts remain
val restoreVersionCppTask = tasks.register("restoreVersionCpp") {
    doLast {
        val versionFile = file("${rootDir}/../ESPHamClock/version.cpp")
        if (versionFile.exists()) {
            ProcessBuilder("git", "restore", versionFile.absolutePath)
                .directory(rootDir)
                .start()
                .waitFor()
        }
    }
}

tasks.matching {
    it.name.startsWith("assemble") ||
    it.name.startsWith("bundle") ||
    it.name.startsWith("buildCMake")
}.configureEach {
    finalizedBy(restoreVersionCppTask)
}

android {

    namespace = "org.openhamclock.hamclock"
    compileSdk = 36
    ndkVersion = "27.1.12297006"

    defaultConfig {
        applicationId = "org.openhamclock.hamclock"
        minSdk = 21
        targetSdk = 36
        versionCode = getGitCommitCount()
        versionName = appVersion

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++17", "-pthread")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }

        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

fun resolvePath(path: String): File {
    val expanded = if (path.startsWith("~/") || path == "~") {
        path.replaceFirst("~", System.getProperty("user.home"))
    } else {
        path
    }
    return file(expanded)
}

fun promptSecure(prompt: String): String? {
    val console = System.console()
    if (console != null) {
        val chars = console.readPassword("$prompt: ")
        return chars?.let { String(it) }?.ifEmpty { null }
    }
    print("$prompt: ")
    System.out.flush()
    val reader = BufferedReader(InputStreamReader(System.`in`))
    return reader.readLine()?.ifEmpty { null }
}

fun promptPlain(prompt: String): String? {
    val console = System.console()
    if (console != null) {
        return console.readLine("$prompt: ")?.ifEmpty { null }
    }
    print("$prompt: ")
    System.out.flush()
    val reader = BufferedReader(InputStreamReader(System.`in`))
    return reader.readLine()?.ifEmpty { null }
}

    signingConfigs {
        create("release") {
            val storeFilePath = System.getenv("ANDROID_KEYSTORE_FILE")
                ?: (project.findProperty("android.injected.signing.store.file") as? String)

            if (!storeFilePath.isNullOrEmpty()) {
                val keystoreFile = resolvePath(storeFilePath)
                if (keystoreFile.exists()) {
                    storeFile = keystoreFile

                    val keyAliasStr = System.getenv("ANDROID_KEY_ALIAS")?.trim()
                        ?: (project.findProperty("android.injected.signing.key.alias") as? String)?.trim()
                        ?: promptPlain("Enter Key Alias")?.trim()

                    val storePass = System.getenv("ANDROID_KEYSTORE_PASSWORD")?.trim()
                        ?: (project.findProperty("android.injected.signing.store.password") as? String)?.trim()
                        ?: promptSecure("Enter Keystore Password")?.trim()

                    val keyPassEnv = System.getenv("ANDROID_KEY_PASSWORD")?.trim()
                        ?: (project.findProperty("android.injected.signing.key.password") as? String)?.trim()
                    val keyPass = if (!keyPassEnv.isNullOrEmpty()) {
                        keyPassEnv
                    } else if (!storePass.isNullOrEmpty()) {
                        val prompted = promptSecure("Enter Key Password (press Enter if same as keystore)")?.trim()
                        if (prompted.isNullOrEmpty()) storePass else prompted
                    } else {
                        null
                    }

                    if (!keyAliasStr.isNullOrEmpty() && !storePass.isNullOrEmpty()) {
                        keyAlias = keyAliasStr
                        storePassword = storePass
                        keyPassword = keyPass ?: storePass
                    } else {
                        throw GradleException("Release signing error: keystore alias or password was not provided.")
                    }
                } else {
                    throw GradleException("Release keystore file not found at: ${keystoreFile.absolutePath}")
                }
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            val releaseSigning = signingConfigs.getByName("release")
            signingConfig = if (releaseSigning.storeFile != null && releaseSigning.storeFile!!.exists()) {
                releaseSigning
            } else {
                signingConfigs.getByName("debug")
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            ndk {
                debugSymbolLevel = "FULL"
            }
        }
    }


    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        viewBinding = true
    }
}

base {
    archivesName.set("org.openhamclock.hamclock-${appVersion}")
}

tasks.matching {
    it.name == "signReleaseBundle" || it.name == "signDebugBundle" ||
    it.name == "packageReleaseBundle" || it.name == "packageDebugBundle"
}.configureEach {
    val isRelease = name.contains("Release")
    val variantName = if (isRelease) "release" else "debug"
    doLast {
        val bundleDir = file("${layout.buildDirectory.get()}/outputs/bundle/$variantName")
        val defaultAab = file("$bundleDir/app-$variantName.aab")
        if (defaultAab.exists()) {
            val descriptiveAab = file("$bundleDir/org.openhamclock.hamclock-${appVersion}-${variantName}.aab")
            defaultAab.copyTo(descriptiveAab, overwrite = true)
        }
    }
}

fun verifyElf64Alignment(targetName: String, bytes: ByteArray): List<String> {
    val errors = mutableListOf<String>()
    if (bytes.size < 64) {
        errors.add("$targetName: File size (${bytes.size} bytes) is too small to be an ELF binary")
        return errors
    }
    if (bytes[0] != 0x7f.toByte() || bytes[1] != 'E'.code.toByte() ||
        bytes[2] != 'L'.code.toByte() || bytes[3] != 'F'.code.toByte()) {
        errors.add("$targetName: Not an ELF binary (magic mismatch)")
        return errors
    }
    val eiClass = bytes[4].toInt() and 0xFF
    if (eiClass != 2) {
        // 32-bit binaries (e.g. armeabi-v7a) do not require 16 KB page alignment under Google Play policy.
        return emptyList()
    }
    val eiData = bytes[5].toInt() and 0xFF
    if (eiData != 1) {
        errors.add("$targetName: Big-endian 64-bit ELF not supported")
        return errors
    }

    fun readU16(offset: Int): Int {
        val b0 = bytes[offset].toInt() and 0xFF
        val b1 = bytes[offset + 1].toInt() and 0xFF
        return (b1 shl 8) or b0
    }

    fun readU32(offset: Int): Long {
        var res = 0L
        for (i in 0..3) {
            res = res or ((bytes[offset + i].toLong() and 0xFFL) shl (i * 8))
        }
        return res
    }

    fun readU64(offset: Int): Long {
        var res = 0L
        for (i in 0..7) {
            res = res or ((bytes[offset + i].toLong() and 0xFFL) shl (i * 8))
        }
        return res
    }

    val ePhOff = readU64(32)
    val ePhEntSize = readU16(54)
    val ePhNum = readU16(56)

    var loadCount = 0
    for (i in 0 until ePhNum) {
        val entryOffset = (ePhOff + i.toLong() * ePhEntSize.toLong()).toInt()
        if (entryOffset + ePhEntSize > bytes.size) {
            errors.add("$targetName: Program header $i out of bounds")
            break
        }
        val pType = readU32(entryOffset)
        // PT_LOAD = 1
        if (pType == 1L) {
            loadCount++
            val pAlign = readU64(entryOffset + 48)
            if (pAlign < 16384L) {
                errors.add("$targetName: PT_LOAD segment $i alignment is $pAlign (0x${pAlign.toString(16)}), must be >= 16384 (0x4000) for Google Play 16 KB compliance")
            }
        }
    }
    if (loadCount == 0) {
        errors.add("$targetName: No PT_LOAD segments found in 64-bit ELF")
    }
    return errors
}

val verify16KbAlignmentTask = tasks.register("verify16KbAlignment") {
    description = "Verifies 16 KB page size alignment for all 64-bit native libraries (.so) in APK/AAB outputs and native intermediates."
    group = "verification"
    doLast {
        val buildDir = layout.buildDirectory.get().asFile
        val allErrors = mutableListOf<String>()
        var verifiedCount = 0

        // 1. Check inside all generated APKs and AABs
        val packages = mutableListOf<File>()
        val apkDir = File(buildDir, "outputs/apk")
        if (apkDir.exists()) {
            apkDir.walkTopDown().filter { it.isFile && it.extension == "apk" }.forEach { packages.add(it) }
        }
        val bundleDir = File(buildDir, "outputs/bundle")
        if (bundleDir.exists()) {
            bundleDir.walkTopDown().filter { it.isFile && it.extension == "aab" }.forEach { packages.add(it) }
        }

        for (pkg in packages) {
            try {
                ZipFile(pkg).use { zip ->
                    val entries = zip.entries()
                    while (entries.hasMoreElements()) {
                        val entry = entries.nextElement()
                        val name = entry.name
                        if (name.endsWith(".so") && (name.contains("arm64-v8a") || name.contains("x86_64"))) {
                            val bytes = zip.getInputStream(entry).use { it.readBytes() }
                            val errs = verifyElf64Alignment("${pkg.name}::$name", bytes)
                            if (errs.isNotEmpty()) {
                                allErrors.addAll(errs)
                            } else {
                                verifiedCount++
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                allErrors.add("Failed to inspect package ${pkg.name}: ${e.message}")
            }
        }

        // 2. Check standalone intermediate native libraries if present
        val intermediateDirs = listOf(
            File(buildDir, "intermediates/stripped_native_libs"),
            File(buildDir, "intermediates/merged_native_libs")
        )
        for (dir in intermediateDirs) {
            if (dir.exists()) {
                dir.walkTopDown()
                    .filter { it.isFile && it.extension == "so" && (it.parentFile?.name == "arm64-v8a" || it.parentFile?.name == "x86_64") }
                    .forEach { soFile ->
                        val bytes = soFile.readBytes()
                        val relPath = soFile.relativeTo(buildDir).path
                        val errs = verifyElf64Alignment(relPath, bytes)
                        if (errs.isNotEmpty()) {
                            allErrors.addAll(errs)
                        } else {
                            verifiedCount++
                        }
                    }
            }
        }

        if (allErrors.isNotEmpty()) {
            throw GradleException(
                """
                ========================================================================
                ERROR: 16 KB Page Alignment Verification FAILED
                The following 64-bit native libraries do not meet Google Play's 16 KB
                page size alignment requirement (all PT_LOAD segments must have Align >= 0x4000):
                ${allErrors.joinToString("\n") { "  - $it" }}
                ========================================================================
                """.trimIndent()
            )
        }

        if (verifiedCount > 0) {
            println(
                """
                ========================================================================
                [16 KB Page Alignment Verification PASSED]
                Verified $verifiedCount 64-bit native library instances across packages and intermediates.
                All PT_LOAD segments are aligned to >= 16384 bytes (0x4000).
                Google Play 16 KB page size requirement is satisfied.
                ========================================================================
                """.trimIndent()
            )
        } else {
            logger.warn("Warning: No 64-bit native libraries (.so) found to verify for 16 KB alignment. Run assemble or bundle first.")
        }
    }
}

tasks.matching {
    it.name.startsWith("assemble") || it.name.startsWith("bundle")
}.configureEach {
    finalizedBy(verify16KbAlignmentTask)
}

tasks.named<Delete>("clean") {
    delete(file(".cxx"))
}




dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)
    implementation(libs.androidx.webkit)
    implementation(libs.play.app.update)
    implementation(libs.play.app.update.ktx)
    testImplementation(libs.junit)
}
