package org.openhamclock.hamclock

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.SharedPreferences
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import android.widget.ViewFlipper
import androidx.appcompat.app.AlertDialog
import androidx.core.content.ContextCompat
import java.io.File

/**
 * Controller and state machine managing the Vega two-page settings overlay.
 *
 * Page 1 (MAIN): Quick operational controls (status, LAN toggle, QR code hub, setup, exit).
 * Page 2 (SETTINGS): Detailed configuration (network name, backend server, overscan, autostart, background, updates).
 */
class SettingsPanelController(private val activity: MainActivity) {

    enum class Page {
        MAIN,
        SETTINGS
    }

    enum class EndpointTab {
        LIVE,
        ANTENNAS
    }

    companion object {
        private const val TAG = "SettingsPanelCtrl"
        const val PREFS_NAME = "hamclock_prefs"
        const val PREF_BACKEND_HOST = "backend_host"
        const val PREF_FORCE_SETUP = "force_setup"
        const val PREF_START_ON_BOOT = "start_on_boot"
        const val PREF_ALLOW_EXTERNAL = "allow_external_access"
        const val PREF_MDNS_NAME = "mdns_name"
        const val PREF_RUN_IN_BACKGROUND = "run_in_background"
        const val PREF_AUTO_UPDATE = "auto_update_enabled"
        const val PREF_TV_OVERSCAN = "tv_overscan"
    }

    private val prefs: SharedPreferences = activity.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    private val mainHandler = Handler(Looper.getMainLooper())

    var dialog: AlertDialog? = null
        private set

    var isShowing: Boolean = false
        private set

    var currentPage: Page = Page.MAIN
        private set

    var currentTab: EndpointTab = EndpointTab.LIVE
        private set

    private var initialBackendHost: String = ""
    private var lanAccessEnabled: Boolean = false
    private var selectedOverscan: Int = 0
    private var autoDismissRunnable: Runnable? = null

    // Hidden sync CheckBox for update listener
    private var cbAutoUpdateRef: CheckBox? = null
    private var btnUpdateOnRef: Button? = null
    private var btnUpdateOffRef: Button? = null

    fun show(startOnTvSetup: Boolean = false, initialPage: Page = Page.MAIN) {
        if (isShowing && dialog?.isShowing == true) {
            Log.d(TAG, "Settings panel already showing; ignoring show request")
            return
        }

        val dialogView = activity.layoutInflater.inflate(R.layout.dialog_vega_settings, null)

        // Find views
        val flipper = dialogView.findViewById<ViewFlipper>(R.id.settings_flipper)

        // Page 1 (MAIN) views
        val tvEngineStatus = dialogView.findViewById<TextView>(R.id.tv_engine_status)
        val tvPortsInfo = dialogView.findViewById<TextView>(R.id.tv_ports_info)
        val tvRestPortConflict = dialogView.findViewById<TextView>(R.id.tv_rest_port_conflict)
        val llTvSetupGuide = dialogView.findViewById<LinearLayout>(R.id.ll_tv_setup_guide)
        val btnResume = dialogView.findViewById<Button>(R.id.btn_resume)
        val btnToggleLan = dialogView.findViewById<Button>(R.id.btn_toggle_lan)
        val btnGoToSettings = dialogView.findViewById<Button>(R.id.btn_go_to_settings)
        val btnOpenSetup = dialogView.findViewById<Button>(R.id.btn_open_setup)
        val btnExitApp = dialogView.findViewById<Button>(R.id.btn_exit_app)
        val llLanOffContainer = dialogView.findViewById<LinearLayout>(R.id.ll_lan_off_container)
        val llLanOnContainer = dialogView.findViewById<LinearLayout>(R.id.ll_lan_on_container)
        val btnTabLive = dialogView.findViewById<Button>(R.id.btn_tab_live)
        val btnTabAntennas = dialogView.findViewById<Button>(R.id.btn_tab_antennas)
        val tvLanTargetHint = dialogView.findViewById<TextView>(R.id.tv_lan_target_hint)
        val ivLocalAccessQr = dialogView.findViewById<ImageView>(R.id.iv_local_access_qr)
        val tvLocalUrlValue = dialogView.findViewById<TextView>(R.id.tv_local_url_value)
        val tvIpUrlValue = dialogView.findViewById<TextView>(R.id.tv_ip_url_value)
        val btnCopyLocalUrl = dialogView.findViewById<Button>(R.id.btn_copy_local_url)

        // Page 2 (SETTINGS) views
        val etMdnsName = dialogView.findViewById<EditText>(R.id.et_mdns_name)
        val tvMdnsNameHelp = dialogView.findViewById<TextView>(R.id.tv_mdns_name_help)
        val etBackendHost = dialogView.findViewById<EditText>(R.id.et_backend_host)
        val llTvOverscan = dialogView.findViewById<LinearLayout>(R.id.ll_tv_overscan)
        val btnOverscan0 = dialogView.findViewById<Button>(R.id.btn_overscan_0)
        val btnOverscan3 = dialogView.findViewById<Button>(R.id.btn_overscan_3)
        val btnOverscan5 = dialogView.findViewById<Button>(R.id.btn_overscan_5)
        val llStartOnBootSection = dialogView.findViewById<LinearLayout>(R.id.ll_start_on_boot_section)
        val cbStartOnBoot = dialogView.findViewById<CheckBox>(R.id.cb_start_on_boot)
        val llAutostartHelper = dialogView.findViewById<LinearLayout>(R.id.ll_autostart_helper)
        val tvAutostartNotice = dialogView.findViewById<TextView>(R.id.tv_autostart_notice)
        val btnOpenAutostart = dialogView.findViewById<Button>(R.id.btn_open_autostart)
        val cbRunInBackground = dialogView.findViewById<CheckBox>(R.id.cb_run_in_background)
        val btnUpdateOn = dialogView.findViewById<Button>(R.id.btn_update_on)
        val btnUpdateOff = dialogView.findViewById<Button>(R.id.btn_update_off)
        val cbAutoUpdate = dialogView.findViewById<CheckBox>(R.id.cb_auto_update)
        val btnSettingsBack = dialogView.findViewById<Button>(R.id.btn_settings_back)

        cbAutoUpdateRef = cbAutoUpdate
        btnUpdateOnRef = btnUpdateOn
        btnUpdateOffRef = btnUpdateOff

        // Load preferences
        val defaultBackend = activity.getString(R.string.backend_default)
        initialBackendHost = prefs.getString(PREF_BACKEND_HOST, defaultBackend) ?: defaultBackend
        lanAccessEnabled = if (startOnTvSetup) true else prefs.getBoolean(PREF_ALLOW_EXTERNAL, false)
        val currentMdnsName = prefs.getString(PREF_MDNS_NAME, "") ?: ""
        selectedOverscan = prefs.getInt(PREF_TV_OVERSCAN, 0)
        val currentStartOnBoot = prefs.getBoolean(PREF_START_ON_BOOT, false)
        val currentRunInBackground = prefs.getBoolean(PREF_RUN_IN_BACKGROUND, false)
        val currentAutoUpdate = prefs.getBoolean(PREF_AUTO_UPDATE, true)

        etMdnsName.setText(currentMdnsName)
        etBackendHost.setText(initialBackendHost)
        cbRunInBackground.isChecked = currentRunInBackground
        cbAutoUpdate.isChecked = currentAutoUpdate

        if (startOnTvSetup) {
            HamClockNative.setAllowExternalAccess(true)
            activity.updateMdnsService(true, currentMdnsName)
            activity.acquireWifiLock()
            llTvSetupGuide.visibility = View.VISIBLE
        } else {
            llTvSetupGuide.visibility = View.GONE
        }

        // Functions for updating UI state
        fun getEndpointTargetUrl(tab: EndpointTab): String {
            val ip = activity.getDeviceIpAddress()
            val entered = etMdnsName.text.toString().trim()
            val host = if (!ip.isNullOrEmpty()) ip else {
                if (entered.isNotEmpty()) "$entered.local" else (activity.registeredMdnsName ?: "hamclock") + ".local"
            }
            return if (tab == EndpointTab.LIVE) {
                "http://$host:${activity.actualRwPort}/live.html"
            } else {
                val port = if (activity.actualRestPort > 0) activity.actualRestPort else 8080
                "http://$host:$port/antennas.html"
            }
        }

        fun updateMdnsHelpText() {
            val name = etMdnsName.text.toString().trim()
            val displayName = if (name.isNotEmpty()) name else "hamclock"
            if (!lanAccessEnabled) {
                tvMdnsNameHelp.text = activity.getString(R.string.network_name_help_lan_off)
            } else {
                tvMdnsNameHelp.text = activity.getString(R.string.network_name_help_format, displayName, activity.actualRwPort)
            }
        }

        fun updateEngineStatusAndPorts() {
            val isRunning = HamClockNative.isDaemonRunning()
            val engineStateText = if (isRunning) "running" else "stopped"
            val versionName = try {
                activity.packageManager.getPackageInfo(activity.packageName, 0).versionName ?: "3.10"
            } catch (e: Exception) {
                "3.10"
            }
            tvEngineStatus.text = activity.getString(R.string.engine_status_format, engineStateText, versionName)

            val rwPort = activity.actualRwPort
            val restText = if (activity.actualRestPort > 0) ":${activity.actualRestPort}" else "off"
            val lanText = if (lanAccessEnabled) activity.getString(R.string.lan_status_active) else activity.getString(R.string.lan_status_loopback)
            tvPortsInfo.text = activity.getString(R.string.engine_ports_format, rwPort, restText, lanText)

            if (activity.restPortConflict) {
                tvRestPortConflict.visibility = View.VISIBLE
                tvRestPortConflict.text = if (activity.actualRestPort > 0) {
                    activity.getString(R.string.rest_port_conflict_warning, activity.actualRestPort)
                } else {
                    activity.getString(R.string.rest_port_conflict_disabled)
                }
            } else {
                tvRestPortConflict.visibility = View.GONE
            }
        }

        fun updateEndpointsAndQr() {
            btnTabLive.isSelected = (currentTab == EndpointTab.LIVE)
            btnTabAntennas.isSelected = (currentTab == EndpointTab.ANTENNAS)

            val entered = etMdnsName.text.toString().trim()
            val hostName = if (entered.isNotEmpty()) "$entered.local" else (activity.registeredMdnsName ?: "hamclock") + ".local"
            val ip = activity.getDeviceIpAddress()

            if (currentTab == EndpointTab.LIVE) {
                tvLanTargetHint.text = activity.getString(R.string.lan_scan_guidance)
                tvLocalUrlValue.text = "http://$hostName:${activity.actualRwPort}/live.html"

                val targetUrl: String
                if (!ip.isNullOrEmpty()) {
                    tvIpUrlValue.text = "or http://$ip:${activity.actualRwPort}/live.html"
                    tvIpUrlValue.visibility = View.VISIBLE
                    targetUrl = "http://$ip:${activity.actualRwPort}/live.html"
                } else {
                    tvIpUrlValue.visibility = View.GONE
                    targetUrl = "http://$hostName:${activity.actualRwPort}/live.html"
                }

                val qrBmp = HamClockNative.generateQRCodeBitmap(targetUrl, scale = 5, border = 2)
                ivLocalAccessQr.setImageBitmap(qrBmp)
                btnCopyLocalUrl.visibility = View.VISIBLE
            } else {
                tvLanTargetHint.text = activity.getString(R.string.antenna_page_guidance)
                if (activity.actualRestPort <= 0) {
                    tvLocalUrlValue.text = activity.getString(R.string.antenna_page_ports_conflict)
                    tvIpUrlValue.visibility = View.GONE
                    ivLocalAccessQr.setImageBitmap(null)
                    btnCopyLocalUrl.visibility = View.GONE
                } else {
                    tvLocalUrlValue.text = "http://$hostName:${activity.actualRestPort}/antennas.html"

                    val targetUrl: String
                    if (!ip.isNullOrEmpty()) {
                        tvIpUrlValue.text = "or http://$ip:${activity.actualRestPort}/antennas.html"
                        tvIpUrlValue.visibility = View.VISIBLE
                        targetUrl = "http://$ip:${activity.actualRestPort}/antennas.html"
                    } else {
                        tvIpUrlValue.visibility = View.GONE
                        targetUrl = "http://$hostName:${activity.actualRestPort}/antennas.html"
                    }

                    val qrBmp = HamClockNative.generateQRCodeBitmap(targetUrl, scale = 5, border = 2)
                    ivLocalAccessQr.setImageBitmap(qrBmp)
                    btnCopyLocalUrl.visibility = View.VISIBLE
                }
            }
        }

        fun updateLanCardAndToggle() {
            btnToggleLan.isSelected = lanAccessEnabled
            btnToggleLan.text = activity.getString(
                if (lanAccessEnabled) R.string.lan_access_on else R.string.lan_access_off
            )

            if (lanAccessEnabled) {
                llLanOffContainer.visibility = View.GONE
                llLanOnContainer.visibility = View.VISIBLE
                updateEndpointsAndQr()
            } else {
                llLanOffContainer.visibility = View.VISIBLE
                llLanOnContainer.visibility = View.GONE
            }
            updateEngineStatusAndPorts()
            updateMdnsHelpText()
        }

        fun updateOverscanButtons(percent: Int) {
            btnOverscan0.isSelected = (percent == 0)
            btnOverscan3.isSelected = (percent == 3)
            btnOverscan5.isSelected = (percent == 5)
        }

        fun updateUpdateButtons(enabled: Boolean) {
            btnUpdateOn.isSelected = enabled
            btnUpdateOff.isSelected = !enabled
            cbAutoUpdate.isChecked = enabled
        }

        fun checkBackendChangeAndRestart(): Boolean {
            val entered = etBackendHost.text.toString().trim()
            val newHost = if (entered.isNotEmpty()) entered else defaultBackend
            if (newHost != initialBackendHost) {
                Log.i(TAG, "Backend changed from '$initialBackendHost' to '$newHost'. Clearing cache and restarting.")
                initialBackendHost = newHost
                prefs.edit().putString(PREF_BACKEND_HOST, newHost).commit()
                val dataDir = File(activity.filesDir, "hamclock_data")
                activity.clearHamClockCache(dataDir)
                dialog?.dismiss()
                activity.restartApp()
                return true
            }
            return false
        }

        fun switchToPage(targetPage: Page) {
            if (currentPage == targetPage) return

            if (currentPage == Page.SETTINGS && targetPage == Page.MAIN) {
                if (checkBackendChangeAndRestart()) return
            }

            currentPage = targetPage
            flipper.displayedChild = if (targetPage == Page.MAIN) 0 else 1

            if (targetPage == Page.MAIN) {
                updateEngineStatusAndPorts()
                updateLanCardAndToggle()
                btnGoToSettings.post { btnGoToSettings.requestFocus() }
            } else {
                updateMdnsHelpText()
                etMdnsName.post { etMdnsName.requestFocus() }
            }
        }

        // Initialize state
        updateEngineStatusAndPorts()
        updateLanCardAndToggle()
        updateOverscanButtons(selectedOverscan)
        updateUpdateButtons(currentAutoUpdate)

        // Setup Page 1 (MAIN) Action Listeners
        btnResume.setOnClickListener {
            dialog?.dismiss()
        }

        btnToggleLan.setOnClickListener {
            lanAccessEnabled = !lanAccessEnabled
            prefs.edit().putBoolean(PREF_ALLOW_EXTERNAL, lanAccessEnabled).commit()
            HamClockNative.setAllowExternalAccess(lanAccessEnabled)
            activity.updateMdnsService(lanAccessEnabled, etMdnsName.text.toString().trim())
            if (lanAccessEnabled) {
                activity.acquireWifiLock()
            } else {
                activity.releaseWifiLock()
            }
            updateLanCardAndToggle()
        }

        btnGoToSettings.setOnClickListener {
            switchToPage(Page.SETTINGS)
        }

        btnOpenSetup.setOnClickListener {
            Log.i(TAG, "User requested HamClock Setup screen")
            prefs.edit().putBoolean(PREF_FORCE_SETUP, true).commit()
            dialog?.dismiss()
            activity.restartApp()
        }

        btnExitApp.setOnClickListener {
            Log.i(TAG, "User requested Exit HamClock from settings overlay")
            activity.isExplicitExit = true
            dialog?.dismiss()
            activity.finishAndRemoveTask()
            mainHandler.postDelayed({
                activity.killLingeringHamClockZombies()
                android.os.Process.killProcess(android.os.Process.myPid())
            }, 150)
        }

        btnTabLive.setOnClickListener {
            currentTab = EndpointTab.LIVE
            updateEndpointsAndQr()
        }

        btnTabAntennas.setOnClickListener {
            currentTab = EndpointTab.ANTENNAS
            updateEndpointsAndQr()
        }

        btnCopyLocalUrl.setOnClickListener {
            val copyUrl = getEndpointTargetUrl(currentTab)
            val clipboard = activity.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
            val clip = ClipData.newPlainText("HamClock URL", copyUrl)
            clipboard?.setPrimaryClip(clip)
            Toast.makeText(activity, activity.getString(R.string.copied_to_clipboard), Toast.LENGTH_SHORT).show()
        }

        // Setup Page 2 (SETTINGS) Action Listeners
        etMdnsName.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                val newName = s?.toString()?.trim() ?: ""
                prefs.edit().putString(PREF_MDNS_NAME, newName).commit()
                if (lanAccessEnabled) {
                    activity.updateMdnsService(true, newName)
                    updateEndpointsAndQr()
                }
                updateMdnsHelpText()
            }
            override fun afterTextChanged(s: Editable?) {}
        })

        // Overscan
        llTvOverscan.visibility = if (activity.isFireTvOrTv()) View.VISIBLE else View.GONE
        btnOverscan0.setOnClickListener {
            selectedOverscan = 0
            updateOverscanButtons(0)
            prefs.edit().putInt(PREF_TV_OVERSCAN, 0).commit()
            activity.applyOverscanMargin(0)
        }
        btnOverscan3.setOnClickListener {
            selectedOverscan = 3
            updateOverscanButtons(3)
            prefs.edit().putInt(PREF_TV_OVERSCAN, 3).commit()
            activity.applyOverscanMargin(3)
        }
        btnOverscan5.setOnClickListener {
            selectedOverscan = 5
            updateOverscanButtons(5)
            prefs.edit().putInt(PREF_TV_OVERSCAN, 5).commit()
            activity.applyOverscanMargin(5)
        }

        // Start on Boot
        val startOnBootSupported = AutoStartHelper.isStartOnBootSupported(activity.isFireTvOrTv())
        if (!startOnBootSupported) {
            llStartOnBootSection.visibility = View.GONE
        } else {
            llStartOnBootSection.visibility = View.VISIBLE
            cbStartOnBoot.isChecked = currentStartOnBoot

            val specialSetting = AutoStartHelper.getSpecialSetting(activity)
            fun updateAutostartHelperVisibility(checked: Boolean) {
                if (checked && specialSetting != null) {
                    tvAutostartNotice.text = specialSetting.message
                    btnOpenAutostart.setOnClickListener {
                        try {
                            activity.startActivity(specialSetting.intent)
                        } catch (e: Exception) {
                            Log.w(TAG, "Failed to launch special autostart intent: ${e.message}")
                        }
                    }
                    llAutostartHelper.visibility = View.VISIBLE
                } else {
                    llAutostartHelper.visibility = View.GONE
                }
            }

            updateAutostartHelperVisibility(currentStartOnBoot)
            cbStartOnBoot.setOnCheckedChangeListener { _, isChecked ->
                prefs.edit().putBoolean(PREF_START_ON_BOOT, isChecked).commit()
                updateAutostartHelperVisibility(isChecked)
            }
        }

        // Run in Background
        cbRunInBackground.setOnCheckedChangeListener { _, isChecked ->
            prefs.edit().putBoolean(PREF_RUN_IN_BACKGROUND, isChecked).commit()
        }

        // Check for Updates
        btnUpdateOn.setOnClickListener {
            val wasEnabled = prefs.getBoolean(PREF_AUTO_UPDATE, true)
            prefs.edit().putBoolean(PREF_AUTO_UPDATE, true).commit()
            updateUpdateButtons(true)
            if (!wasEnabled) {
                activity.appUpdateHelper.checkForUpdates()
            }
        }

        btnUpdateOff.setOnClickListener {
            prefs.edit().putBoolean(PREF_AUTO_UPDATE, false).commit()
            updateUpdateButtons(false)
        }

        cbAutoUpdate.setOnCheckedChangeListener { _, isChecked ->
            prefs.edit().putBoolean(PREF_AUTO_UPDATE, isChecked).commit()
            updateUpdateButtons(isChecked)
        }

        btnSettingsBack.setOnClickListener {
            switchToPage(Page.MAIN)
        }

        // Build AlertDialog
        val builder = AlertDialog.Builder(activity, R.style.Theme_HamClock_VegaDialog)
        builder.setView(dialogView)

        val dlg = builder.create()
        dialog = dlg

        dlg.setOnKeyListener { _, keyCode, event ->
            if (keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) {
                if (currentPage == Page.SETTINGS) {
                    switchToPage(Page.MAIN)
                    return@setOnKeyListener true
                }
            }
            false
        }

        dlg.setOnShowListener {
            isShowing = true
            if (initialPage == Page.SETTINGS) {
                currentPage = Page.SETTINGS
                flipper.displayedChild = 1
                etMdnsName.post { etMdnsName.requestFocus() }
            } else {
                currentPage = Page.MAIN
                flipper.displayedChild = 0
                btnResume.post { btnResume.requestFocus() }
            }
        }

        dlg.setOnDismissListener {
            isShowing = false
            autoDismissRunnable?.let { mainHandler.removeCallbacks(it) }
            autoDismissRunnable = null
            cbAutoUpdateRef = null
            btnUpdateOnRef = null
            btnUpdateOffRef = null
            activity.appUpdateHelper.onSettingsDialogDismissed()
            activity.btnSettings.clearFocus()
            activity.webView.requestFocus()

            checkBackendChangeAndRestart()
        }

        // First-run TV setup automatic dismissal upon callsign config
        if (startOnTvSetup) {
            val runnable = object : Runnable {
                override fun run() {
                    if (dlg.isShowing) {
                        if (activity.isCallsignSet()) {
                            Log.i(TAG, "Callsign configured via web setup! Automatically saving network settings and dismissing first-run TV guidance.")
                            val enteredMdns = etMdnsName.text.toString().trim()
                            prefs.edit()
                                .putBoolean(PREF_ALLOW_EXTERNAL, true)
                                .putString(PREF_MDNS_NAME, enteredMdns)
                                .putInt(PREF_TV_OVERSCAN, selectedOverscan)
                                .commit()
                            activity.applyOverscanMargin(selectedOverscan)
                            HamClockNative.setAllowExternalAccess(true)
                            activity.updateMdnsService(true, enteredMdns)
                            activity.acquireWifiLock()
                            dlg.dismiss()
                        } else {
                            mainHandler.postDelayed(this, 1000)
                        }
                    }
                }
            }
            autoDismissRunnable = runnable
            mainHandler.postDelayed(runnable, 1000)
        }

        dlg.show()

        dlg.window?.let { window ->
            window.setBackgroundDrawableResource(android.R.color.transparent)
            @Suppress("DEPRECATION")
            window.setSoftInputMode(
                WindowManager.LayoutParams.SOFT_INPUT_STATE_HIDDEN or
                WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN
            )
            val dm = activity.resources.displayMetrics
            val widthDp = dm.widthPixels / dm.density
            val isLandscape = activity.resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE

            val targetWidthPx = if (isLandscape) {
                if (widthDp >= 920) {
                    (880 * dm.density).toInt()
                } else {
                    (dm.widthPixels * 0.92).toInt()
                }
            } else {
                // Portrait: comfortable card on tablets (e.g. Pixel Tablet ~704dp) or phones (~94%)
                if (widthDp >= 600) {
                    minOf((720 * dm.density).toInt(), (dm.widthPixels * 0.88).toInt())
                } else {
                    (dm.widthPixels * 0.94).toInt()
                }
            }

            window.setLayout(
                targetWidthPx,
                WindowManager.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun dismiss() {
        dialog?.dismiss()
    }

    fun onAutoUpdateSettingChanged(enabled: Boolean) {
        cbAutoUpdateRef?.isChecked = enabled
        btnUpdateOnRef?.isSelected = enabled
        btnUpdateOffRef?.isSelected = !enabled
    }
}

