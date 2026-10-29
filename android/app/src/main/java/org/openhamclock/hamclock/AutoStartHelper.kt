package org.openhamclock.hamclock

import android.app.ActivityManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.Log

data class SpecialSettingInfo(
    val message: String,
    val intent: Intent
)

object AutoStartHelper {

    private const val TAG = "HamClockAutoStartHelper"

    /**
     * Checks if the device appears to be rooted or running a custom ROM.
     */
    fun isDeviceRootedOrCustom(): Boolean {
        val buildTags = Build.TAGS
        if (buildTags != null && buildTags.contains("test-keys")) {
            return true
        }

        val suPaths = arrayOf(
            "/system/app/Superuser.apk",
            "/sbin/su",
            "/system/bin/su",
            "/system/xbin/su",
            "/data/local/xbin/su",
            "/data/local/bin/su",
            "/system/sd/xbin/su",
            "/system/bin/failsafe/su",
            "/data/local/su"
        )
        for (path in suPaths) {
            try {
                if (java.io.File(path).exists()) {
                    return true
                }
            } catch (_: Exception) {
            }
        }
        return false
    }

    /**
     * Determines whether "Start on boot" is supported on this platform.
     *
     * Standard TV platforms (Android TV, Google TV, Fire OS 8) running Android 10+ (API 29+)
     * strictly block background activity launches from BOOT_COMPLETED.
     * We allow it for:
     * - Any non-TV platform (phones, tablets)
     * - Any platform running Android 9 or older (API < 29, such as Fire OS 5/6/7)
     * - Hypothetically rooted or custom ROM environments
     */
    fun isStartOnBootSupported(
        isTv: Boolean,
        sdkInt: Int = Build.VERSION.SDK_INT,
        isRootedOrCustom: Boolean = isDeviceRootedOrCustom()
    ): Boolean {
        if (!isTv || sdkInt < Build.VERSION_CODES.Q) {
            return true
        }
        return isRootedOrCustom
    }

    /**
     * Checks if the current OS environment has special background or auto-start
     * requirements that may prevent BOOT_COMPLETED from launching HamClock.
     *
     * Returns a SpecialSettingInfo with user message and actionable intent if detected,
     * or null if the device is a standard Android environment without restrictions.
     */
    fun getSpecialSetting(context: Context): SpecialSettingInfo? {
        // 1. Check if the Android OS has explicitly marked this app as background restricted (Android 9+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            if (activityManager?.isBackgroundRestricted == true) {
                val appDetailsIntent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
                    data = Uri.fromParts("package", context.packageName, null)
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                }
                return SpecialSettingInfo(
                    context.getString(R.string.background_restricted_notice),
                    appDetailsIntent
                )
            }
        }

        // 2. Query PackageManager for known OEM proprietary auto-start management components
        val oemIntentCandidates = listOf(
            // Xiaomi / MIUI / HyperOS
            Intent().setComponent(ComponentName("com.miui.securitycenter", "com.miui.permcenter.autostart.AutoStartManagementActivity")),
            // Huawei / Honor
            Intent().setComponent(ComponentName("com.huawei.systemmanager", "com.huawei.systemmanager.startupmgr.ui.StartupNormalAppListActivity")),
            Intent().setComponent(ComponentName("com.huawei.systemmanager", "com.huawei.systemmanager.optimize.process.ProtectActivity")),
            // Oppo / Realme / ColorOS
            Intent().setComponent(ComponentName("com.coloros.safecenter", "com.coloros.safecenter.permission.startup.StartupAppListActivity")),
            Intent().setComponent(ComponentName("com.coloros.safecenter", "com.coloros.safecenter.startupapp.StartupAppListActivity")),
            // Vivo / iQOO
            Intent().setComponent(ComponentName("com.iqoo.secure", "com.iqoo.secure.ui.phoneoptimize.AddWhiteListActivity")),
            Intent().setComponent(ComponentName("com.vivo.permissionmanager", "com.vivo.permissionmanager.activity.BgStartUpManagerActivity")),
            // Asus
            Intent().setComponent(ComponentName("com.asus.mobilemanager", "com.asus.mobilemanager.autostart.AutoStartActivity")),
            // Transsion (Infinix / Tecno / Itel)
            Intent().setComponent(ComponentName("com.transsion.phonemaster", "com.transsion.phonemaster.autostart.AutoStartManagementActivity"))
        )

        val pm = context.packageManager
        for (intent in oemIntentCandidates) {
            try {
                if (pm.resolveActivity(intent, PackageManager.MATCH_DEFAULT_ONLY) != null) {
                    Log.i(TAG, "Detected OEM Auto-Start manager component: ${intent.component}")
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    return SpecialSettingInfo(
                        context.getString(R.string.autostart_oem_notice),
                        intent
                    )
                }
            } catch (e: Exception) {
                Log.w(TAG, "Error checking intent ${intent.component}: ${e.message}")
            }
        }

        // Standard device without special OEM restrictions
        return null
    }
}
