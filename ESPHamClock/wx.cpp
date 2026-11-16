/* This file manages two types of caches for weather and time zone:
 *   exact: WXInfo for last known DE and DX locations
 *          uses separate timeouts for wx and tz
 *   fast:  collection of WXInfo in WXTable on a fixed grid of lat/lng for approx but fast info.
 */


#include "HamClock.h"

// downloads
static const char wx_ll[] = "/wx.pl";                   // URL for requesting specific wx/time at lat/lng
static const char ww_page[] = "/worldwx/wx.txt";        // URL for the gridded world weather table


// config
#define WWXTBL_INTERVAL (45*60)                         // "fast" world wx table update interval, secs
#define MAX_WXTZ_AGE    (55*60)                         // max age of info for same location, secs

// WXInfo and exactly where it applies and when it should be updated
typedef struct {
    WXInfo info;                                        // timezone and wx for ...
    float lat_d, lng_d;                                 // this location
    bool ok;                                            // whether info is valid
    char ynot[100];                                     // or why not
    time_t next_update;                                 // next routine update of same lat/lng
} WXCache;
static WXCache de_cache, dx_cache;
static time_t next_err_update;                          // next update if net trouble, shared

/* table of world-wide info for fast general lookups by roaming cursor.
 * wwt.table is a 2d table n_cols x n_rows.
 *   width:  columns are latitude [-90,90] in steps of 180/(n_cols-1).
 *   height: rows are longitude [-180..180) in steps of 360/n_rows.
 */
typedef struct {
    WXInfo *table;                                      // malloced array of info, latitude-major order
    int n_rows, n_cols;                                 // table dimensions
    time_t next_update;                                 // next fresh
} WWTable;
static WWTable wwt;

/* bit masks of WeatherStats for DE and DX
 */
static uint16_t dewx_chmask, dxwx_chmask;

/* menu names for each WeatherStats
 */
#define X(a,b)  b,                              // expands WXSTATS to name plus comma
static const char *wxch_names[WXS_N] = {
    WXSTATS
};
#undef X




/* insure weather choice masks are defined
 */
static void initChoiceMasks(void)
{
    if (!NVReadUInt16 (NV_DEWXCHOICE, &dewx_chmask) || dewx_chmask == 0) {
        dewx_chmask = (1<<WXS_TEMP) | (1<<WXS_HUM) | (1<<WXS_WSPD) | (1<<WXS_WDIR);
        NVWriteUInt16 (NV_DEWXCHOICE, dewx_chmask);
    }
    if (!NVReadUInt16 (NV_DXWXCHOICE, &dxwx_chmask) || dxwx_chmask == 0) {
        dxwx_chmask = (1<<WXS_TEMP) | (1<<WXS_HUM) | (1<<WXS_WSPD) | (1<<WXS_WDIR);
        NVWriteUInt16 (NV_DXWXCHOICE, dxwx_chmask);
    }
}

/* convert wind direction in degs to name, return whether in range.
 */
static bool windDeg2Name (float deg, char dirname[4])
{
    const char *name;

    if (deg < 0)          name = "?";
    else if (deg < 22.5)  name = "N";
    else if (deg < 67.5)  name = "NE";
    else if (deg < 112.5) name = "E";
    else if (deg < 157.5) name = "SE";
    else if (deg < 202.5) name = "S";
    else if (deg < 247.5) name = "SW";
    else if (deg < 292.5) name = "W";
    else if (deg < 337.5) name = "NW";
    else if (deg <= 360)  name = "N";
    else                  name = "?";

    strcpy (dirname, name);

    return (dirname[0] != '?');
}

/* download world wx grid data into wwt.table every 
 */
static bool retrieveWorldWx(void)
{
    WiFiClient ww_client;
    bool ok = false;

    // reset table
    free (wwt.table);
    wwt.table = NULL;
    wwt.n_rows = wwt.n_cols = 0;

    Serial.printf ("WWX: %s\n", ww_page);

    // get
    if (ww_client.connect(backend_host, backend_port)) {
        updateClocks(false);

        // query web page
        httpHCGET (ww_client, backend_host, ww_page);

        // prep for scanning (ahead of skipping header to avoid stupid g++ goto errors)
        int line_n = 0;                         // line number
        int n_wwtable = 0;                      // entries defined found so far
        int n_wwmalloc = 0;                     // malloced room so far
        int n_lngcols = 0;                      // build up n cols of constant lng so far this block
        float del_lat = 0, del_lng = 0;         // check constant step sizes
        float prev_lat = 0, prev_lng = 0;       // for checking step sizes

        // skip response header
        if (!httpSkipHeader (ww_client)) {
            Serial.printf ("WWX: header timeout");
            goto out;
        }

        /* read file and build table. confirm regular spacing each dimension.
         * file is one line per datum, increasing lat with same lng, then lng steps at each blank line.
         * file contains lng 180 for plotting but we don't use it.
         */
        char line[100];
        while (getTCPLine (ww_client, line, sizeof(line), NULL)) {

            if (debugLevel (DEBUG_WX, 2))
                Serial.printf ("WWX: %s\n", line);

            // another line
            line_n++;

            // skip comment lines
            if (line[0] == '#')
                continue;

            // crack line:   lat     lng  temp,C     %hum    mps     dir    mmHg Wx
            float lat, lng, windir;
            WXInfo wx;
            memset (&wx, 0, sizeof(wx));
            int ns = sscanf (line, "%g %g %g %g %g %g %g %31s %d", &lat, &lng, &wx.temperature_c,
                        &wx.humidity_percent, &wx.wind_speed_mps, &windir, &wx.pressure_hPa, wx.conditions,
                        &wx.timezone);

            // skip lng 180
            if (lng == 180)
                break;

            // add and check
            if (ns == 9) {

                // confirm regular spacing
                if (n_lngcols > 0 && lng != prev_lng) {
                    Serial.printf ("WWX: irregular lng: %d x %d  lng %g != %g\n",
                                wwt.n_rows, n_lngcols, lng, prev_lng);
                    goto out;
                }
                if (n_lngcols > 1 && lat != prev_lat + del_lat) {
                    Serial.printf ("WWX: irregular lat: %d x %d    lat %g != %g + %g\n",
                                wwt.n_rows, n_lngcols,  lat, prev_lat, del_lat);
                    goto out;
                }

                // convert wind direction to name
                if (!windDeg2Name (windir, wx.wind_dir_name)) {
                    Serial.printf ("WWX: bogus wind direction: %g\n", windir);
                    goto out;
                }

                // add to wwt.table
                if (n_wwtable + 1 > n_wwmalloc)
                    wwt.table = (WXInfo *) realloc (wwt.table, (n_wwmalloc += 100) * sizeof(WXInfo));
                memcpy (&wwt.table[n_wwtable++], &wx, sizeof(WXInfo));

                // update walk
                if (n_lngcols == 0)
                    del_lng = lng - prev_lng;
                del_lat = lat - prev_lat;
                prev_lat = lat;
                prev_lng = lng;
                n_lngcols++;

            } else if (ns <= 0) {

                // blank line separates blocks of constant longitude

                // check consistency so far
                if (wwt.n_rows == 0) {
                    // we know n cols after completing the first lng block, all remaining must equal this 
                    wwt.n_cols = n_lngcols;
                } else if (n_lngcols != wwt.n_cols) {
                    Serial.printf ("WWX: inconsistent columns %d != %d after %d rows\n",
                                                n_lngcols, wwt.n_cols, wwt.n_rows);
                    goto out;
                }

                // one more wwt.table row
                wwt.n_rows++;

                // reset block stats
                n_lngcols = 0;

            } else {

                Serial.printf ("WWX: bogus line %d: %s\n", line_n, line);
                goto out;
            }
        }

        // final check
        if (wwt.n_rows != 360/del_lng || wwt.n_cols != 1 + 180/del_lat) {
            Serial.printf ("WWX: incomplete table: rows %d != 360/%g   cols %d != 1 + 180/%g\n",
                                        wwt.n_rows, del_lng,  wwt.n_cols, del_lat);
            goto out;
        }

        // yah!
        ok = true;
        Serial.printf ("WWX: fast table %d lat x %d lng\n", wwt.n_cols, wwt.n_rows);

    out:

        if (!ok) {
            // reset table
            free (wwt.table);
            wwt.table = NULL;
            wwt.n_rows = wwt.n_cols = 0;
        }

        ww_client.stop();
    }

    return (ok);
}

/* download current weather and time info for the given exact location.
 * if wxc.info is filled ok return true, else return false with short reason in wxc.ynot
 */
static bool retrieveCurrentWX (const LatLong &ll, bool is_de, WXCache &wxc)
{
    WXInfo &wxi = wxc.info;

    WiFiClient wx_client;
    char line[100];

    bool ok = false;

    // get
    if (wx_client.connect(backend_host, backend_port)) {
        updateClocks(false);

        // query web page
        snprintf (line, sizeof(line), "%s?is_de=%d&lat=%g&lng=%g", wx_ll, is_de, ll.lat_d, ll.lng_d);
        Serial.printf ("WX: %s\n", line);
        httpHCGET (wx_client, backend_host, line);

        // skip response header
        if (!httpSkipHeader (wx_client)) {
            quietStrncpy (wxc.ynot, "WX timeout", sizeof(wxc.ynot));
            goto out;
        }

        // init response 
        memset (&wxi, 0, sizeof(WXInfo));

        // crack response
        uint8_t n_found = 0;
        while (n_found < N_WXINFO_FIELDS && getTCPLine (wx_client, line, sizeof(line), NULL)) {

            if (debugLevel (DEBUG_WX, 1))
                Serial.printf ("WX: %s\n", line);

            updateClocks(false);

            // check for error message in which case abandon further search
            if (strncmp (line, "error=", 6) == 0) {
                quietStrncpy (wxc.ynot, line+6, sizeof(wxc.ynot));
                goto out;
            }

            // find start of data value after =
            char *vstart = strchr (line, '=');
            if (!vstart)
                continue;
            *vstart++ = '\0';   // eos for name and move to value

            // check for content line
            if (strcmp (line, "city") == 0) {
                quietStrncpy (wxi.city, vstart, sizeof(wxi.city));
                n_found++;
            } else if (strcmp (line, "temperature_c") == 0) {
                wxi.temperature_c = atof (vstart);
                n_found++;
            } else if (strcmp (line, "pressure_hPa") == 0) {
                wxi.pressure_hPa = atof (vstart);
                n_found++;
            } else if (strcmp (line, "pressure_chg") == 0) {
                wxi.pressure_chg = atof (vstart);
                n_found++;
            } else if (strcmp (line, "humidity_percent") == 0) {
                wxi.humidity_percent = atof (vstart);
                n_found++;
            } else if (strcmp (line, "wind_speed_mps") == 0) {
                wxi.wind_speed_mps = atof (vstart);
                n_found++;
            } else if (strcmp (line, "wind_dir_name") == 0) {
                quietStrncpy (wxi.wind_dir_name, vstart, sizeof(wxi.wind_dir_name));
                n_found++;
            } else if (strcmp (line, "clouds") == 0) {
                quietStrncpy (wxi.clouds, vstart, sizeof(wxi.clouds));
                n_found++;
            } else if (strcmp (line, "conditions") == 0) {
                quietStrncpy (wxi.conditions, vstart, sizeof(wxi.conditions));
                n_found++;
            } else if (strcmp (line, "attribution") == 0) {
                quietStrncpy (wxi.attribution, vstart, sizeof(wxi.attribution));
                n_found++;
            } else if (strcmp (line, "timezone") == 0) {
                wxi.timezone = atoi (vstart);
                n_found++;
            }

            // Serial.printf ("WX %d: %s\n", n_found, line);
        }

        if (n_found < N_WXINFO_FIELDS) {
            quietStrncpy (wxc.ynot, "Missing WX data", sizeof(wxc.ynot));
            goto out;
        }

        // ok!
        ok = true;

    } else {

        quietStrncpy (wxc.ynot, "WX connection failed", sizeof(wxc.ynot));

    }

    // clean up
out:
    wx_client.stop();
    return (ok);
}



/* display the desired weather stats in NCDXF_b or err.
 */
static void drawNCDXFBoxWx (BRB_MODE m, const WXInfo &wxi, bool ok)
{
    // consistent err msg
    static char err[] = "Err";

    // insure masks are defined
    initChoiceMasks();

    // decide mask and indentity
    const uint16_t mask = (m == BRB_SHOW_DEWX) ? dewx_chmask : dxwx_chmask;
    const char *whoami =  (m == BRB_SHOW_DEWX) ? "DE" : "DX";
    uint16_t color =      (m == BRB_SHOW_DEWX) ? DE_COLOR : DX_COLOR;

    // arrays for drawNCDXFStats()
    char values[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN];
    uint16_t colors[NCDXF_B_NFIELDS];
    char titles[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN];

    // init in case not all used
    for (int i = 0; i < NCDXF_B_NFIELDS; i++)
        values[i][0] = titles[i][0] = '\0';

    // fill arrays from mask and wxi, ignore any more than NCDXF_B_NFIELDS are set
    int n_fields = 0;
    for (int i = 0; i < WXS_N && n_fields < NCDXF_B_NFIELDS; i++) {
        // fill next set field
        if (mask & (1<<i)) {
            switch ((WeatherStats)i) {
            case WXS_TEMP:
                if (n_fields == 0) {
                    // first title includes whether DE or DX
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s Temp", whoami);
                } else
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s", wxch_names[i]);
                if (ok) {
                    float v = showTempC() ? wxi.temperature_c : CEN2FAH(wxi.temperature_c);
                    v = noNegZero (v, 1);
                    snprintf (values[n_fields], NCDXF_B_MAXLEN, "%.1f", v);
                    colors[n_fields] = color;
                } else {
                    strcpy (values[n_fields], err);
                    colors[n_fields] = RA8875_RED;
                }
                break;

            case WXS_HUM:
                if (n_fields == 0) {
                    // first title includes whether DE or DX
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s Hum", whoami);
                } else
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s", wxch_names[i]);
                if (ok) {
                    snprintf (values[n_fields], sizeof(values[1]), "%.1f", wxi.humidity_percent);
                    colors[n_fields] = color;
                } else {
                    strcpy (values[n_fields], err);
                    colors[n_fields] = RA8875_RED;
                }
                break;

            case WXS_DEW:
                if (n_fields == 0) {
                    // first title includes whether DE or DX
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s DewPt", whoami);
                } else
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s", wxch_names[i]);
                if (ok) {
                    float t = showTempC() ? wxi.temperature_c : CEN2FAH(wxi.temperature_c);
                    float d = dewPoint (t, wxi.humidity_percent);        // wants and returns user units
                    d = noNegZero (d, 1);
                    snprintf (values[n_fields], sizeof(values[1]), "%.1f", d);
                    colors[n_fields] = color;
                } else {
                    strcpy (values[n_fields], err);
                    colors[n_fields] = RA8875_RED;
                }
                break;

            case WXS_PRES:
                if (n_fields == 0) {
                    // first title includes whether DE or DX
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s Pres", whoami);
                } else
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s", wxch_names[i]);
                if (ok) {
                    if (showATMhPa())
                        snprintf (values[n_fields], sizeof(values[1]), "%.0f", wxi.pressure_hPa);
                    else
                        snprintf (values[n_fields], sizeof(values[1]), "%.2f", HPA2INHG(wxi.pressure_hPa));
                    colors[n_fields] = color;
                } else {
                    strcpy (values[n_fields], err);
                    colors[n_fields] = RA8875_RED;
                }
                break;

            case WXS_WSPD:
                if (n_fields == 0) {
                    // first title includes whether DE or DX
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s W Spd", whoami);
                } else
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s", wxch_names[i]);
                if (ok) {
                    float s = (showDistKm() ? 3.6F : 2.237F) * wxi.wind_speed_mps; // kph or mph
                    snprintf (values[n_fields], sizeof(values[1]), "%.0f", s);
                    colors[n_fields] = color;
                } else {
                    strcpy (values[n_fields], err);
                    colors[n_fields] = RA8875_RED;
                }
                break;

            case WXS_WDIR:
                if (n_fields == 0) {
                    // first title includes whether DE or DX
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s W Dir", whoami);
                } else
                    snprintf (titles[n_fields], NCDXF_B_MAXLEN, "%s", wxch_names[i]);
                if (ok) {
                    snprintf (values[n_fields], sizeof(values[1]), "%s", wxi.wind_dir_name);
                    colors[n_fields] = color;
                } else {
                    strcpy (values[n_fields], err);
                    colors[n_fields] = RA8875_RED;
                }
                break;

            case WXS_N:
                break;
            }

            // another field
            n_fields++;

        }
    }

    // show it
    drawNCDXFStats (RA8875_BLACK, titles, values, colors);
}

/* display a menu in NCDXF_b allowing choosing which wx stats to display.
 * m is expected to be BRB_SHOW_DEWX or BRB_SHOW_DXWX.
 * set NV_DEWXCHOICE or NV_DXWXCHOICE
 */
void doNCDXFWXTouch (BRB_MODE m)
{
    // decide current mask
    uint16_t &mask =     (m == BRB_SHOW_DEWX) ? dewx_chmask : dxwx_chmask;
    const char *whoami = (m == BRB_SHOW_DEWX) ? "DE" : "DX";

    // build menu
    MenuItem mitems[WXS_N] = {
        {MENU_AL1OFN, (mask & (1<<WXS_TEMP)) != 0, 1, 1, wxch_names[WXS_TEMP], 0},
        {MENU_AL1OFN, (mask & (1<<WXS_HUM))  != 0, 1, 1, wxch_names[WXS_HUM], 0},
        {MENU_AL1OFN, (mask & (1<<WXS_DEW))  != 0, 1, 1, wxch_names[WXS_DEW], 0},
        {MENU_AL1OFN, (mask & (1<<WXS_PRES)) != 0, 1, 1, wxch_names[WXS_PRES], 0},
        {MENU_AL1OFN, (mask & (1<<WXS_WSPD)) != 0, 1, 1, wxch_names[WXS_WSPD], 0},
        {MENU_AL1OFN, (mask & (1<<WXS_WDIR)) != 0, 1, 1, wxch_names[WXS_WDIR], 0},
    };

    // its box
    SBox menu_b = NCDXF_b;                      // copy, not ref!
    menu_b.x += 1;
    menu_b.y += NCDXF_b.h/8;
    menu_b.w = 0;                               // shrink wrap

    // run
    SBox ok_b;
    MenuInfo menu = {menu_b, ok_b, UF_CLOCKSOK, M_NOCANCEL, 1, NARRAY(mitems), mitems};
    if (runMenu (menu)) {

        // set new mask
        int n_bits = 0;
        mask = 0;
        for (int i = 0; i < WXS_N; i++) {
            if (mitems[i].set) {
                if (++n_bits > NCDXF_B_NFIELDS) {
                    Serial.printf ("WX: using only first %d %s selections\n", NCDXF_B_NFIELDS, whoami);
                    break;
                }
                mask |= (1<<i);
            }
        }

        // save
        NVWriteUInt16 (m == BRB_SHOW_DEWX ? NV_DEWXCHOICE : NV_DXWXCHOICE, mask);

        // redraw box
        drawNCDXFWx(m);
    }
}

/* display current DE weather in the given box and in NCDXF_b if up.
 */
bool updateDEWX (const SBox &box)
{
    Message ynot;
    WXInfo wxi;

    bool ok = getCurrentWX (de_ll, true, &wxi, ynot);
    if (ok)
        plotWX (box, DE_COLOR, wxi);
    else
        plotMessage (box, DE_COLOR, ynot.get());

    if (brb_mode == BRB_SHOW_DEWX)
        drawNCDXFBoxWx (BRB_SHOW_DEWX, wxi, ok);

    return (ok);
}

/* display current DX weather in the given box and in NCDXF_b if up.
 */
bool updateDXWX (const SBox &box)
{
    Message ynot;
    WXInfo wxi;

    bool ok = getCurrentWX (dx_ll, false, &wxi, ynot);
    if (ok)
        plotWX (box, DX_COLOR, wxi);
    else
        plotMessage (box, DX_COLOR, ynot.get());

    if (brb_mode == BRB_SHOW_DXWX)
        drawNCDXFBoxWx (BRB_SHOW_DXWX, wxi, ok);

    return (ok);
}

/* display weather for the given mode in NCDXF_b.
 * return whether all ok.
 */
bool drawNCDXFWx (BRB_MODE m)
{
    // get weather
    Message ynot;
    WXInfo wxi;
    bool ok = false;
    if (m == BRB_SHOW_DEWX)
        ok = getCurrentWX (de_ll, true, &wxi, ynot);
    else if (m == BRB_SHOW_DXWX)
        ok = getCurrentWX (dx_ll, false, &wxi, ynot);
    else
        fatalError ("Bogus drawNCDXFWx mode: %d", m);
    if (!ok)
        Serial.printf ("WX: %s\n", ynot.get());

    // show it
    drawNCDXFBoxWx (m, wxi, ok);

    // done
    return (ok);
}



/* return current WXInfo for the given de or dx, else NULL
 */
static const WXInfo *findWXTXCache (const LatLong &ll, bool is_de, Message &ynot)
{
    // who are we?
    WXCache &wxc = is_de ? de_cache : dx_cache;

    // prep err msg if needed
    char retry_msg[50];
    snprintf (retry_msg, sizeof(retry_msg), "%s WX/TZ", is_de ? "DE" : "DX");

    // new location?
    bool new_loc = ll.lat_d != wxc.lat_d || ll.lng_d != wxc.lng_d;

    // update depending same location and how well things are working
    if (myNow() > next_err_update && (new_loc || myNow() > wxc.next_update)) {

        // get fresh, schedule retry if fail
        if (!retrieveCurrentWX (ll, is_de, wxc)) {
            next_err_update = nextWiFiRetry (retry_msg);
            wxc.ok = false;
            ynot.set(wxc.ynot);
            return (NULL);
        }

        // ok! update location and next routine expiration

        wxc.ok = true;
        wxc.lat_d = ll.lat_d;
        wxc.lng_d = ll.lng_d;
        wxc.next_update = myNow() + MAX_WXTZ_AGE;

        // log
        int at = millis()/1000 + MAX_WXTZ_AGE;
        Serial.printf ("WXTZ: expires in %d sec at %d\n", MAX_WXTZ_AGE, at);
    }

    // return current info
    return (&wxc.info);
}

/* return current WXInfo with weather at de or dx.
 */
static const WXInfo *findWXCache (const LatLong &ll, bool is_de, Message &ynot)
{
    return (findWXTXCache (ll, is_de, ynot));
}

/* return current WXInfo with timezone at de or dx.
 */
const WXInfo *findTZCache (const LatLong &ll, bool is_de, Message &ynot)
{
    return (findWXTXCache (ll, is_de, ynot));
}



/* return closest WXInfo to ll within grid, else NULL.
 */
const WXInfo *findWXFast (const LatLong &ll)
{
    // update wwt cache if stale
    if (myNow() > wwt.next_update) {
        static const char wwx_label[] = "FastWXTable";
        if (retrieveWorldWx()) {
            wwt.next_update = myNow() + WWXTBL_INTERVAL;
            int at = millis()/1000 + WWXTBL_INTERVAL;
            Serial.printf ("WWX: Next %s update in %d sec at %d\n", wwx_label, WWXTBL_INTERVAL, at);
        } else {
            wwt.next_update = nextWiFiRetry (wwx_label);
            return (NULL);
        }
    }

    // find closest indices
    int row = floorf (wwt.n_rows*(ll.lng_d+180)/360);
    int col = floorf (wwt.n_cols*(ll.lat_d+90)/180);

    // ok
    return (&wwt.table[row*wwt.n_cols + col]);
}


/* look up wx conditions from local cache for the approx location, if possible.
 * return whether wxi has been filled
 */
bool getFastWx (const LatLong &ll, WXInfo &wxi)
{
    const WXInfo *wip = findWXFast (ll);
    if (wip) {
        wxi = *wip;
        return (true);
    }
    return (false);
}

/* fill wip with weather data for ll.
 * return whether ok, with short reason if not.
 */
bool getCurrentWX (const LatLong &ll, bool is_de, WXInfo *wip, Message &ynot)
{
    const WXInfo *new_wip = findWXCache (ll, is_de, ynot);
    if (new_wip) {
        *wip = *new_wip;
        return (true);
    }
    return (false);
}
