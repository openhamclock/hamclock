package org.openhamclock.hamclock

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.android.play.core.appupdate.AppUpdateInfo
import com.google.android.play.core.appupdate.AppUpdateManager
import com.google.android.play.core.appupdate.AppUpdateManagerFactory
import com.google.android.play.core.appupdate.AppUpdateOptions
import com.google.android.play.core.install.InstallStateUpdatedListener
import com.google.android.play.core.install.model.AppUpdateType
import com.google.android.play.core.install.model.InstallStatus
import com.google.android.play.core.install.model.UpdateAvailability
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/**
 * Handles application updates across distribution channels:
 * - Google Play Store: Performs in-app flexible background updates via AppUpdateManager.
 * - Amazon Appstore: Checks latest version against backend and prompts deep link to Amazon Appstore.
 * - Sideloaded / GitHub: Checks latest version and prompts link to GitHub releases.
 */
class AppUpdateHelper(private val activity: AppCompatActivity) {

    enum class InstallSource {
        GOOGLE_PLAY,
        AMAZON_APPSTORE,
        OTHER
    }

    companion object {
        const val REQUEST_CODE_FLEXIBLE_UPDATE = 10091
        private const val TAG = "AppUpdateHelper"
        private const val PREFS_NAME = "hamclock_prefs"
        private const val PREF_LAST_DISMISSED_VERSION = "update_dismissed_version"
        private const val PREF_LAST_DISMISSED_TIME = "update_dismissed_time"
        private const val PERIODIC_CHECK_INTERVAL_MS = 6 * 3600 * 1000L // 6 hours
        private const val INITIAL_CHECK_DELAY_MS = 15 * 1000L // 15 seconds after launch
    }

    private val executor = Executors.newSingleThreadExecutor()
    private val handler = Handler(Looper.getMainLooper())
    private var activeDialog: AlertDialog? = null

    private val appUpdateManager: AppUpdateManager? by lazy {
        try {
            AppUpdateManagerFactory.create(activity)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to initialize AppUpdateManager: ${e.message}")
            null
        }
    }

    private val installListener = InstallStateUpdatedListener { state ->
        if (state.installStatus() == InstallStatus.DOWNLOADED) {
            Log.i(TAG, "Flexible update downloaded successfully")
            showDownloadedPrompt()
        }
    }

    private val periodicCheckRunnable = object : Runnable {
        override fun run() {
            checkForUpdates()
            handler.postDelayed(this, PERIODIC_CHECK_INTERVAL_MS)
        }
    }

    fun start() {
        try {
            appUpdateManager?.registerListener(installListener)
        } catch (e: Exception) {
            Log.w(TAG, "Could not register AppUpdate install listener: ${e.message}")
        }

        // Schedule initial check with short delay so app startup is not delayed
        handler.postDelayed(periodicCheckRunnable, INITIAL_CHECK_DELAY_MS)
    }

    fun onResume() {
        // If an update was already downloaded while backgrounded, prompt on resume
        if (getInstallSource() == InstallSource.GOOGLE_PLAY) {
            try {
                appUpdateManager?.appUpdateInfo?.addOnSuccessListener { appUpdateInfo ->
                    if (appUpdateInfo.installStatus() == InstallStatus.DOWNLOADED) {
                        showDownloadedPrompt()
                    }
                }
            } catch (e: Exception) {
                Log.d(TAG, "Resume update check exception: ${e.message}")
            }
        }
    }

    fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        try {
            appUpdateManager?.unregisterListener(installListener)
        } catch (e: Exception) {
            // Ignore unregister errors on destroy
        }
        activeDialog?.dismiss()
        activeDialog = null
        executor.shutdown()
    }

    fun checkForUpdates() {
        val source = getInstallSource()
        Log.i(TAG, "Checking for updates (Install source: $source)")

        if (source == InstallSource.GOOGLE_PLAY && appUpdateManager != null) {
            checkGooglePlayUpdate()
        } else {
            checkBackendUpdate(source)
        }
    }

    private fun checkGooglePlayUpdate() {
        try {
            appUpdateManager?.appUpdateInfo?.addOnSuccessListener { appUpdateInfo ->
                if (appUpdateInfo.updateAvailability() == UpdateAvailability.UPDATE_AVAILABLE
                    && appUpdateInfo.isUpdateTypeAllowed(AppUpdateType.FLEXIBLE)
                ) {
                    Log.i(TAG, "Google Play flexible update available; starting download flow")
                    try {
                        val options = AppUpdateOptions.newBuilder(AppUpdateType.FLEXIBLE).build()
                        appUpdateManager?.startUpdateFlowForResult(
                            appUpdateInfo,
                            activity,
                            options,
                            REQUEST_CODE_FLEXIBLE_UPDATE
                        )
                    } catch (e: Exception) {
                        Log.w(TAG, "Failed to start Google Play update flow: ${e.message}")
                    }
                } else if (appUpdateInfo.installStatus() == InstallStatus.DOWNLOADED) {
                    showDownloadedPrompt()
                } else {
                    Log.i(TAG, "Google Play reports up to date (status: ${appUpdateInfo.updateAvailability()})")
                }
            }?.addOnFailureListener { e ->
                Log.i(TAG, "Google Play update query failed: ${e.message}. Falling back to backend check.")
                checkBackendUpdate(getInstallSource())
            }
        } catch (e: Exception) {
            Log.w(TAG, "Error initiating Google Play update check: ${e.message}")
            checkBackendUpdate(getInstallSource())
        }
    }

    private fun checkBackendUpdate(source: InstallSource) {
        executor.execute {
            val remoteVersion = fetchBackendVersion()
            if (remoteVersion.isNullOrBlank()) {
                Log.d(TAG, "No remote version found from backend")
                return@execute
            }

            val currentVersion = getCurrentVersionName()
            Log.d(TAG, "Current version: '$currentVersion', remote version: '$remoteVersion'")

            if (VersionComparator.isNewer(currentVersion, remoteVersion)) {
                if (isDismissedRecently(remoteVersion)) {
                    Log.i(TAG, "Update $remoteVersion was recently dismissed; skipping prompt")
                    return@execute
                }
                showUpdateAvailablePrompt(remoteVersion, source)
            } else {
                Log.d(TAG, "Running current or newer version ($currentVersion >= $remoteVersion)")
            }
        }
    }

    private fun fetchBackendVersion(): String? {
        val prefs = activity.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val configuredHost = prefs.getString("backend_host", "ohb.hamclock.app:80") ?: "ohb.hamclock.app:80"
        val hostWithoutPort = configuredHost.substringBefore(":")
        val urlsToTry = listOf(
            "https://$hostWithoutPort/ham/HamClock/version.pl",
            "http://$configuredHost/ham/HamClock/version.pl",
            "https://ohb.hamclock.app/ham/HamClock/version.pl"
        ).distinct()

        for (urlStr in urlsToTry) {
            try {
                val url = URL(urlStr)
                val conn = url.openConnection() as HttpURLConnection
                conn.connectTimeout = 8000
                conn.readTimeout = 8000
                conn.instanceFollowRedirects = true
                conn.setRequestProperty("User-Agent", "HamClock-Android/${getCurrentVersionName()}")
                if (conn.responseCode == HttpURLConnection.HTTP_OK) {
                    conn.inputStream.bufferedReader().use { reader ->
                        val firstLine = reader.readLine()?.trim()
                        if (!firstLine.isNullOrEmpty()) {
                            return firstLine
                        }
                    }
                }
            } catch (e: Exception) {
                Log.d(TAG, "Backend version fetch failed for $urlStr: ${e.message}")
            }
        }
        return null
    }

    private fun showDownloadedPrompt() {
        activity.runOnUiThread {
            if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
            activeDialog?.dismiss()

            activeDialog = AlertDialog.Builder(activity, androidx.appcompat.R.style.Theme_AppCompat_Dialog_Alert)
                .setTitle(R.string.update_downloaded_title)
                .setMessage(R.string.update_downloaded_msg)
                .setPositiveButton(R.string.update_restart_now) { _, _ ->
                    completeUpdate()
                }
                .setNegativeButton(R.string.update_later, null)
                .show()
        }
    }

    private fun showUpdateAvailablePrompt(remoteVersion: String, source: InstallSource) {
        activity.runOnUiThread {
            if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
            activeDialog?.dismiss()

            val msg = if (source == InstallSource.AMAZON_APPSTORE) {
                activity.getString(R.string.update_available_amazon_msg, remoteVersion)
            } else {
                activity.getString(R.string.update_available_github_msg, remoteVersion)
            }

            val positiveTextRes = if (source == InstallSource.AMAZON_APPSTORE) {
                R.string.update_in_appstore
            } else {
                R.string.update_view_release
            }

            activeDialog = AlertDialog.Builder(activity, androidx.appcompat.R.style.Theme_AppCompat_Dialog_Alert)
                .setTitle(R.string.update_available_title)
                .setMessage(msg)
                .setPositiveButton(positiveTextRes) { _, _ ->
                    if (source == InstallSource.AMAZON_APPSTORE) {
                        openAmazonAppstore()
                    } else {
                        openGitHubReleases()
                    }
                }
                .setNegativeButton(R.string.update_later) { _, _ ->
                    saveDismissedVersion(remoteVersion)
                }
                .setOnCancelListener {
                    saveDismissedVersion(remoteVersion)
                }
                .show()
        }
    }

    fun completeUpdate() {
        try {
            appUpdateManager?.completeUpdate()
        } catch (e: Exception) {
            Log.e(TAG, "Failed to complete in-app update: ${e.message}")
        }
    }

    private fun openAmazonAppstore() {
        val packageName = activity.packageName
        val amznUri = Uri.parse("amzn://apps/android?p=$packageName&intent=app_update")
        val webUri = Uri.parse("https://www.amazon.com/gp/mas/dl/android?p=$packageName")
        try {
            val intent = Intent(Intent.ACTION_VIEW, amznUri).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            activity.startActivity(intent)
        } catch (e: ActivityNotFoundException) {
            try {
                val webIntent = Intent(Intent.ACTION_VIEW, webUri).apply {
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                }
                activity.startActivity(webIntent)
            } catch (we: Exception) {
                Log.e(TAG, "Cannot launch web browser for Amazon Appstore", we)
            }
        }
    }

    private fun openGitHubReleases() {
        val url = "https://github.com/openhamclock/hamclock/releases/latest"
        try {
            val intent = Intent(Intent.ACTION_VIEW, Uri.parse(url)).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            activity.startActivity(intent)
        } catch (e: Exception) {
            Log.e(TAG, "Cannot open browser for GitHub releases: ${e.message}")
        }
    }

    private fun isDismissedRecently(remoteVersion: String): Boolean {
        val prefs = activity.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val lastVer = prefs.getString(PREF_LAST_DISMISSED_VERSION, null)
        val lastTime = prefs.getLong(PREF_LAST_DISMISSED_TIME, 0L)
        val now = System.currentTimeMillis()
        val oneDayMillis = 24 * 3600 * 1000L
        return lastVer == remoteVersion && (now - lastTime) < oneDayMillis
    }

    private fun saveDismissedVersion(remoteVersion: String) {
        val prefs = activity.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit()
            .putString(PREF_LAST_DISMISSED_VERSION, remoteVersion)
            .putLong(PREF_LAST_DISMISSED_TIME, System.currentTimeMillis())
            .apply()
    }

    private fun getCurrentVersionName(): String {
        return try {
            val pInfo = activity.packageManager.getPackageInfo(activity.packageName, 0)
            pInfo.versionName ?: ""
        } catch (e: Exception) {
            ""
        }
    }

    private fun getInstallSource(): InstallSource {
        val pm = activity.packageManager
        val installerPackage = try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                pm.getInstallSourceInfo(activity.packageName).installingPackageName
            } else {
                @Suppress("DEPRECATION")
                pm.getInstallerPackageName(activity.packageName)
            }
        } catch (e: Exception) {
            null
        }

        val isAmazonDevice = Build.MANUFACTURER.equals("Amazon", ignoreCase = true) ||
                Build.BRAND.equals("Amazon", ignoreCase = true) ||
                pm.hasSystemFeature("amazon.hardware.fire_tv")

        return when {
            installerPackage == "com.android.vending" -> InstallSource.GOOGLE_PLAY
            installerPackage == "com.amazon.venezia" || (isAmazonDevice && installerPackage != "com.android.vending") -> InstallSource.AMAZON_APPSTORE
            else -> InstallSource.OTHER
        }
    }
}
