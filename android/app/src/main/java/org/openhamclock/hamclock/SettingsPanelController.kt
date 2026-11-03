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
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
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
        val tvLanStatusNotice = dialogView.findViewById<TextView>(R.id.tv_lan_status_notice)

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
            prefs.edit().putBoolean(PREF_ALLOW_EXTERNAL, true).apply()
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
            val versionName = HamClockNative.getVersion().removePrefix("v").removePrefix("V")
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

            val isTv = activity.isFireTvOrTv()
            val entered = etMdnsName.text.toString().trim()
            val hostName = if (entered.isNotEmpty()) "$entered.local" else (activity.registeredMdnsName ?: "hamclock") + ".local"
            val ip = activity.getDeviceIpAddress()

            if (ip.isNullOrEmpty()) {
                tvLanStatusNotice?.visibility = View.VISIBLE
                tvLanStatusNotice?.text = activity.getString(R.string.no_network_found)
            } else {
                tvLanStatusNotice?.visibility = View.GONE
            }

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
                btnCopyLocalUrl.visibility = if (!isTv) View.VISIBLE else View.GONE
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
                    btnCopyLocalUrl.visibility = if (!isTv) View.VISIBLE else View.GONE
                }
            }
        }

        fun updateDynamicPage1Focus() {
            val isLandscape = activity.resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
            val copyVisible = btnCopyLocalUrl.visibility == View.VISIBLE

            if (isLandscape) {
                if (lanAccessEnabled) {
                    btnResume.nextFocusRightId = R.id.btn_tab_live
                    btnToggleLan.nextFocusRightId = R.id.btn_tab_live
                    btnGoToSettings.nextFocusRightId = R.id.btn_tab_live
                    btnOpenSetup.nextFocusRightId = if (copyVisible) R.id.btn_copy_local_url else R.id.btn_tab_live
                    btnExitApp.nextFocusRightId = if (copyVisible) R.id.btn_copy_local_url else R.id.btn_tab_antennas

                    btnTabLive.nextFocusDownId = if (copyVisible) R.id.btn_copy_local_url else R.id.btn_tab_live
                    btnTabAntennas.nextFocusDownId = if (copyVisible) R.id.btn_copy_local_url else R.id.btn_tab_antennas
                } else {
                    btnResume.nextFocusRightId = R.id.btn_resume
                    btnToggleLan.nextFocusRightId = R.id.btn_toggle_lan
                    btnGoToSettings.nextFocusRightId = R.id.btn_go_to_settings
                    btnOpenSetup.nextFocusRightId = R.id.btn_open_setup
                    btnExitApp.nextFocusRightId = R.id.btn_exit_app
                }
            } else {
                // Portrait
                if (lanAccessEnabled) {
                    btnExitApp.nextFocusDownId = R.id.btn_tab_live
                    btnResume.nextFocusUpId = if (copyVisible) R.id.btn_copy_local_url else R.id.btn_tab_live
                } else {
                    btnExitApp.nextFocusDownId = R.id.btn_resume
                    btnResume.nextFocusUpId = R.id.btn_exit_app
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
            updateDynamicPage1Focus()
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

        fun setupPage2FocusNavigation() {
            val isTv = activity.isFireTvOrTv()
            val startOnBootSupported = AutoStartHelper.isStartOnBootSupported(isTv)

            etMdnsName.nextFocusUpId = R.id.btn_settings_back
            btnSettingsBack.nextFocusDownId = R.id.et_mdns_name

            if (isTv) {
                etBackendHost.nextFocusDownId = R.id.btn_overscan_0
                btnOverscan0.nextFocusUpId = R.id.et_backend_host
                btnOverscan3.nextFocusUpId = R.id.et_backend_host
                btnOverscan5.nextFocusUpId = R.id.et_backend_host

                if (startOnBootSupported) {
                    btnOverscan0.nextFocusDownId = R.id.cb_start_on_boot
                    btnOverscan3.nextFocusDownId = R.id.cb_start_on_boot
                    btnOverscan5.nextFocusDownId = R.id.cb_start_on_boot
                    cbStartOnBoot.nextFocusUpId = R.id.btn_overscan_0
                } else {
                    btnOverscan0.nextFocusDownId = R.id.cb_run_in_background
                    btnOverscan3.nextFocusDownId = R.id.cb_run_in_background
                    btnOverscan5.nextFocusDownId = R.id.cb_run_in_background
                    cbRunInBackground.nextFocusUpId = R.id.btn_overscan_0
                }
            } else {
                if (startOnBootSupported) {
                    etBackendHost.nextFocusDownId = R.id.cb_start_on_boot
                    cbStartOnBoot.nextFocusUpId = R.id.et_backend_host
                } else {
                    etBackendHost.nextFocusDownId = R.id.cb_run_in_background
                    cbRunInBackground.nextFocusUpId = R.id.et_backend_host
                }
            }

            if (startOnBootSupported) {
                if (llAutostartHelper.visibility == View.VISIBLE) {
                    cbStartOnBoot.nextFocusDownId = R.id.btn_open_autostart
                    btnOpenAutostart.nextFocusUpId = R.id.cb_start_on_boot
                    btnOpenAutostart.nextFocusDownId = R.id.cb_run_in_background
                    cbRunInBackground.nextFocusUpId = R.id.btn_open_autostart
                } else {
                    cbStartOnBoot.nextFocusDownId = R.id.cb_run_in_background
                    if (isTv) {
                        cbRunInBackground.nextFocusUpId = R.id.cb_start_on_boot
                    }
                }
            }
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
                setupPage2FocusNavigation()
                etMdnsName.post { etMdnsName.requestFocus() }
            }
        }

        // Initialize state
        updateEngineStatusAndPorts()
        updateLanCardAndToggle()
        updateOverscanButtons(selectedOverscan)
        updateUpdateButtons(currentAutoUpdate)
        setupPage2FocusNavigation()

        fun applyFocusZoom(view: View, scale: Float = 1.04f, onFocus: ((Boolean) -> Unit)? = null) {
            view.setOnFocusChangeListener { v, hasFocus ->
                if (hasFocus) {
                    v.translationZ = 8f
                    v.animate().scaleX(scale).scaleY(scale).setDuration(120).start()
                } else {
                    v.translationZ = 0f
                    v.animate().scaleX(1.0f).scaleY(1.0f).setDuration(120).start()
                }
                onFocus?.invoke(hasFocus)
            }
        }

        // Setup Page 1 (MAIN) Action Listeners & Focus
        applyFocusZoom(btnResume)
        btnResume.setOnClickListener {
            dialog?.dismiss()
        }

        applyFocusZoom(btnToggleLan)
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

        applyFocusZoom(btnGoToSettings)
        btnGoToSettings.setOnClickListener {
            switchToPage(Page.SETTINGS)
        }

        applyFocusZoom(btnOpenSetup)
        btnOpenSetup.setOnClickListener {
            Log.i(TAG, "User requested HamClock Setup screen")
            prefs.edit().putBoolean(PREF_FORCE_SETUP, true).commit()
            dialog?.dismiss()
            activity.restartApp()
        }

        applyFocusZoom(btnExitApp)
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

        applyFocusZoom(btnTabLive, 1.04f) { hasFocus ->
            if (hasFocus) {
                currentTab = EndpointTab.LIVE
                updateEndpointsAndQr()
            }
        }
        btnTabLive.setOnClickListener {
            currentTab = EndpointTab.LIVE
            updateEndpointsAndQr()
        }

        applyFocusZoom(btnTabAntennas, 1.04f) { hasFocus ->
            if (hasFocus) {
                currentTab = EndpointTab.ANTENNAS
                updateEndpointsAndQr()
            }
        }
        btnTabAntennas.setOnClickListener {
            currentTab = EndpointTab.ANTENNAS
            updateEndpointsAndQr()
        }

        applyFocusZoom(btnCopyLocalUrl)
        btnCopyLocalUrl.setOnClickListener {
            val copyUrl = getEndpointTargetUrl(currentTab)
            val clipboard = activity.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
            val clip = ClipData.newPlainText("HamClock URL", copyUrl)
            clipboard?.setPrimaryClip(clip)
            Toast.makeText(activity, activity.getString(R.string.copied_to_clipboard), Toast.LENGTH_SHORT).show()
        }

        // Setup Page 2 (SETTINGS) Action Listeners & Focus
        fun getNextSettingAfterBackend(): View {
            return when {
                llTvOverscan.visibility == View.VISIBLE -> btnOverscan0
                llStartOnBootSection.visibility == View.VISIBLE -> cbStartOnBoot
                else -> cbRunInBackground
            }
        }

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

        etMdnsName.setOnEditorActionListener { v, actionId, event ->
            if (actionId == EditorInfo.IME_ACTION_DONE ||
                (event != null && event.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)
            ) {
                val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
                imm?.hideSoftInputFromWindow(v.windowToken, 0)
                v.clearFocus()
                etBackendHost.requestFocus()
                true
            } else {
                false
            }
        }

        etMdnsName.setOnKeyListener { _, keyCode, event ->
            if (event.action == KeyEvent.ACTION_DOWN) {
                when (keyCode) {
                    KeyEvent.KEYCODE_DPAD_DOWN -> {
                        etBackendHost.requestFocus()
                        return@setOnKeyListener true
                    }
                    KeyEvent.KEYCODE_DPAD_UP -> {
                        btnSettingsBack.requestFocus()
                        return@setOnKeyListener true
                    }
                }
            }
            false
        }

        etBackendHost.setOnEditorActionListener { v, actionId, event ->
            if (actionId == EditorInfo.IME_ACTION_DONE ||
                (event != null && event.keyCode == KeyEvent.KEYCODE_ENTER && event.action == KeyEvent.ACTION_DOWN)
            ) {
                val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
                imm?.hideSoftInputFromWindow(v.windowToken, 0)
                v.clearFocus()
                getNextSettingAfterBackend().requestFocus()
                true
            } else {
                false
            }
        }

        etBackendHost.setOnKeyListener { _, keyCode, event ->
            if (event.action == KeyEvent.ACTION_DOWN) {
                when (keyCode) {
                    KeyEvent.KEYCODE_DPAD_UP -> {
                        etMdnsName.requestFocus()
                        return@setOnKeyListener true
                    }
                    KeyEvent.KEYCODE_DPAD_DOWN -> {
                        getNextSettingAfterBackend().requestFocus()
                        return@setOnKeyListener true
                    }
                }
            }
            false
        }

        // Overscan
        llTvOverscan.visibility = if (activity.isFireTvOrTv()) View.VISIBLE else View.GONE
        applyFocusZoom(btnOverscan0)
        btnOverscan0.setOnClickListener {
            selectedOverscan = 0
            updateOverscanButtons(0)
            prefs.edit().putInt(PREF_TV_OVERSCAN, 0).commit()
            activity.applyOverscanMargin(0)
        }
        applyFocusZoom(btnOverscan3)
        btnOverscan3.setOnClickListener {
            selectedOverscan = 3
            updateOverscanButtons(3)
            prefs.edit().putInt(PREF_TV_OVERSCAN, 3).commit()
            activity.applyOverscanMargin(3)
        }
        applyFocusZoom(btnOverscan5)
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
            applyFocusZoom(cbStartOnBoot, 1.02f)
            applyFocusZoom(btnOpenAutostart)

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
                setupPage2FocusNavigation()
            }

            updateAutostartHelperVisibility(currentStartOnBoot)
            cbStartOnBoot.setOnCheckedChangeListener { _, isChecked ->
                prefs.edit().putBoolean(PREF_START_ON_BOOT, isChecked).commit()
                updateAutostartHelperVisibility(isChecked)
            }
        }

        // Run in Background
        applyFocusZoom(cbRunInBackground, 1.02f)
        cbRunInBackground.setOnCheckedChangeListener { _, isChecked ->
            prefs.edit().putBoolean(PREF_RUN_IN_BACKGROUND, isChecked).commit()
        }

        // Check for Updates
        applyFocusZoom(btnUpdateOn)
        btnUpdateOn.setOnClickListener {
            val wasEnabled = prefs.getBoolean(PREF_AUTO_UPDATE, true)
            prefs.edit().putBoolean(PREF_AUTO_UPDATE, true).commit()
            updateUpdateButtons(true)
            if (!wasEnabled) {
                activity.appUpdateHelper.checkForUpdates()
            }
        }

        applyFocusZoom(btnUpdateOff)
        btnUpdateOff.setOnClickListener {
            prefs.edit().putBoolean(PREF_AUTO_UPDATE, false).commit()
            updateUpdateButtons(false)
        }

        cbAutoUpdate.setOnCheckedChangeListener { _, isChecked ->
            prefs.edit().putBoolean(PREF_AUTO_UPDATE, isChecked).commit()
            updateUpdateButtons(isChecked)
        }

        applyFocusZoom(btnSettingsBack)
        btnSettingsBack.setOnClickListener {
            switchToPage(Page.MAIN)
        }

        // Build AlertDialog
        val builder = AlertDialog.Builder(activity, R.style.Theme_HamClock_VegaDialog)
        builder.setView(dialogView)

        val dlg = builder.create()
        dialog = dlg

        dlg.setOnKeyListener { _, keyCode, event ->
            if (keyCode == KeyEvent.KEYCODE_BACK) {
                if (event.action == KeyEvent.ACTION_UP) {
                    if (currentPage == Page.SETTINGS) {
                        switchToPage(Page.MAIN)
                        return@setOnKeyListener true
                    } else if (currentPage == Page.MAIN) {
                        dlg.dismiss()
                        return@setOnKeyListener true
                    }
                } else if (event.action == KeyEvent.ACTION_DOWN) {
                    return@setOnKeyListener true
                }
            }
            if (keyCode == KeyEvent.KEYCODE_MENU || keyCode == KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE) {
                if (event.action == KeyEvent.ACTION_UP) {
                    dlg.dismiss()
                    return@setOnKeyListener true
                } else if (event.action == KeyEvent.ACTION_DOWN) {
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
                                .putBoolean(PREF_ALLOW_EXTERNAL, lanAccessEnabled)
                                .putString(PREF_MDNS_NAME, enteredMdns)
                                .putInt(PREF_TV_OVERSCAN, selectedOverscan)
                                .commit()
                            activity.applyOverscanMargin(selectedOverscan)
                            HamClockNative.setAllowExternalAccess(lanAccessEnabled)
                            activity.updateMdnsService(lanAccessEnabled, enteredMdns)
                            if (lanAccessEnabled) {
                                activity.acquireWifiLock()
                            } else {
                                activity.releaseWifiLock()
                            }
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
            val heightDp = dm.heightPixels / dm.density
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

            // On devices with constrained vertical height (e.g. phones in landscape or small screens),
            // constrain dialog height to 94% of screen so the internal ScrollView can scroll smoothly.
            val targetHeightPx = if (heightDp < 560) {
                (dm.heightPixels * 0.94).toInt()
            } else {
                WindowManager.LayoutParams.WRAP_CONTENT
            }

            window.setLayout(
                targetWidthPx,
                targetHeightPx
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

