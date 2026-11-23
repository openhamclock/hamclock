/* Manage the gimbal GUI from hamlib rotators.
 *
 * We use the tcp socket interface to rotctld. Thus it must be running somewhere on the network and its
 * host and port set correctly in Setup.
 *
 * hamlib offers the opportunity for much useful rotator status information but unfortunately most drivers
 * only support the bare minimum of get_pos and set_pos. We use what we can find. We also assume the
 * drivers implement their own safety protocol so we do not try to stop or otherwise act if we receive
 * any errors.
 *
 * If only 1 axis is found, or no sat is defined, Auto points to DX. If 2 axes are found and a sat is
 * defined, then Auto tracks it.
 *
 * Some gimbals can move 0-180 in elevation. If so, satellites will be tracked "upside down" if necessary
 * to avoid unwrapping az at a limit. Gimbals without this capability will suffer a lengthy unwrap if
 * the sat moves through a limit.
 *
 * To be on the safe side, all motion is stopped unless the Gimbal plot pane is visible. If decide later to
 * leave it run note earthsat.cpp turns off tracking any time a new sat might be selected.
 *
 *
 */

#include "HamClock.h"



// GUI configuration
#define CHAR_H          25                              // large character height
#define VALU_INDENT     55                              // az or el value indent
#define STATE_INDENT    120                             // az or el state indent
#define LDIRBOX_SZ      10                              // large direction control box size
#define SDIRBOX_SZ      8                               // small direction control box size
#define DIRBOX_GAP      4                               // gap between control box pairs
#define ARROW_COLOR     RGB565(255,125,0)               // color for directional arrow controls
#define UPOVER_COLOR    RA8875_RED                      // upover symbol color
#define UPDATE_MS       1005                            // command update interval, ms
#define AZSTEP          5                               // small az manual step size
#define AZSTEP2         20                              // large az manual step size
#define ELSTEP          5                               // small el manual step size
#define ELSTEP2         10                              // large el manual step size
#define ERR_DWELL       5000                            // error message display period, ms
#define BEAM_W          10                              // angular width of map beam, degrees

// possible axis states
typedef enum {
    AZS_UNKNOWN,                                        // unknown
    AZS_STOPPED,                                        // stopped
    AZS_CWROT,                                          // rotating cw as seen from above
    AZS_CCWROT,                                         // rotating ccw "
    AZS_CCWLIMIT,                                       // at minimum rotation limit
    AZS_CWLIMIT,                                        // at maximum rotation limit
    AZS_INPOS,                                          // at commanded az
    AZS_NONE,                                           // no az axis
} AzState;
typedef enum {
    ELS_UNKNOWN,                                        // unknown
    ELS_STOPPED,                                        // stopped
    ELS_UPROT,                                          // el angle increasing
    ELS_DOWNROT,                                        // el angle decreasing
    ELS_UPLIMIT,                                        // at minimum rotation angle
    ELS_DOWNLIMIT,                                      // at maximum rotation angle
    ELS_INPOS,                                          // at commanded el
    ELS_NONE,                                           // no el axis
} ElState;

// arrows
typedef enum {
    AR_LEFT,
    AR_DOWN,
    AR_UP,
    AR_RIGHT
} ArrowDir;

// position changes considered insignificant, degrees.
// would be nice if all drivers reported these.
#define AZ_DEADBAND     5
#define EL_DEADBAND     5

// controls and state
static uint16_t AZ_Y, EL_Y;                             // top of current status lines
static SBox azccw_b, azcw_b, azccw2_b, azcw2_b;         // manual az ccw and cw buttons
static SBox elup_b, eldown_b, elup2_b, eldown2_b;       // manual el up and down buttons
static SBox auto_b;                                     // tracking button
static SBox stop_b;                                     // stop button
static bool auto_track;                                 // whether we track a target, else only manual input
static bool sat_upover;                                 // whether using el > 90 to avoid wrapping thru N
static bool upover_pending;                             // avoid sat el near SAT_MIN_EL
static bool user_stop;                                  // user has commanded stop
static float az_target, el_target;                      // target now, degrees
static float az_now, el_now;                            // gimbal now, degrees
static float az_min, az_max;                            // az command limits
static float el_min, el_max;                            // el command limits
static AzState az_state;                                // az run state now
static ElState el_state;                                // el run state now
static int16_t pgaz_target, pgel_target;                // previous GUI az and el target, degrees
static int16_t pgaz_now, pgel_now;                      // previous GUI az and el now, degrees
static AzState pgaz_state;                              // previous GUI az run state
static ElState pgel_state;                              // previous GUI el run state
static char title[20];                                  // title from model
static WiFiClient hamlib_client;                        // connection to hamlib

static void initGimbalGUI(const SBox &box);

/* return whether the clock is providing correct time
 */
static bool goodTime()
{
    return (utcOffset() == 0 && clockTimeOk());
}

/* return whether we are currently connected to hamlib
 */
static bool connectionOk()
{
    return (hamlib_client);
}

/* given a hamlib response and keyword, find pointer within rsp to value that follows.
 * return whether so found
 */
static bool findHamlibRspKey (const char rsp[], const char key[], char **key_ptr)
{
    const char *key_str = strstr (rsp, key);
    if (key_str) {
        const char *colon = strchr (key_str, ':');
        if (colon) {
            *key_ptr = (char*)colon+1;
            while (isspace(**key_ptr))
                (*key_ptr)++;
            return (true);
        }
    }
    return (false);
}

/* given hamlib response and keyword of a numeric value, return its value.
 * return whether successful.
 */
static bool findHamlibRspValue (const char rsp[], const char key[], float *value_ptr)
{
    char *kp;
    if (findHamlibRspKey (rsp, key, &kp)) {
        *value_ptr = atof (kp);
        return (true);
    }
    return (false);
}

/* send cmd to hamlib, pass back complete reply in rsp.
 * return true if all, else fill rsp with error message and return false.
 * N.B. cmd must begin with +\ and end with \n.
 */
static bool askHamlib (const char *cmd, char rsp[], size_t rsp_len)
{
    // insure cmd starts with +\ and ends with \n
    int cmd_l = strlen (cmd);
    if (strncmp (cmd, "+\\", 2) != 0 || cmd[cmd_l-1] != '\n')
        fatalError ("malformed askHamlib cmd: '%s'", cmd);

    if (debugLevel (DEBUG_GIMBAL, 2))
        Serial.printf ("GBL: ask %s", cmd);       // includes \n

    // insure connected
    if (!connectionOk()) {
        snprintf (rsp, rsp_len, "No connection");
        return (false);
    }

    // send
    hamlib_client.print(cmd);

    // collect reply into rsp until find RPRT, fill rsp or time out
    size_t rsp_n = 0;
    bool found_RPRT = false;
    int RPRT = -1;
    uint16_t ll;
    while (!found_RPRT && rsp_n < rsp_len && getTCPLine (hamlib_client, rsp+rsp_n, rsp_len-rsp_n, &ll)) {
        if (debugLevel (DEBUG_GIMBAL, 2))
            Serial.printf ("GBL: reply %s\n", rsp+rsp_n);
        if (sscanf (rsp+rsp_n, "RPRT %d", &RPRT) == 1)
            found_RPRT = true;
        rsp_n += ll;

        // keep reply lines separated so callers can tell where one value ends, room permitting
        if (!found_RPRT && rsp_n + 1 < rsp_len)
            rsp[rsp_n++] = '\n';
    }

    // ok?
    if (found_RPRT && RPRT == 0)
        return (true);
    if (found_RPRT)
        snprintf (rsp, rsp_len, "Hamlib err: %.*s: %d", cmd_l-1, cmd, RPRT);       // discard \n
    else
        snprintf (rsp, rsp_len, "Hamlib err: no RPRT from %.*s", cmd_l-1, cmd);
    return (false);
}


/* get az and el position and attempt to ascertain status if possible.
 * log and report critical errors in box
 */
static bool getAzEl(const SBox &box)
{
    char rsp[100];

    // query position
    if (!askHamlib ("+\\get_pos\n", rsp, sizeof(rsp))) {
        plotMessage (box, RA8875_RED, rsp);
        wdDelay (ERR_DWELL);
        initGimbalGUI (box);
        return (false);
    }

    // crack
    float new_az, new_el;
    if (!findHamlibRspValue (rsp, "Azimuth", &new_az) || !findHamlibRspValue (rsp, "Elevation", &new_el)) {
        Serial.printf ("GBL: no az or el from get_pos: %s\n", rsp);
        plotMessage (box, RA8875_RED, "unexpected get_pos response");
        wdDelay (ERR_DWELL);
        initGimbalGUI (box);
        return (false);
    }

    // divine az state from change before changing az_now
    if (new_az < az_min + AZ_DEADBAND)
        az_state = AZS_CCWLIMIT;
    else if (new_az > az_max - AZ_DEADBAND)
        az_state = AZS_CWLIMIT;
    else if (new_az < az_now)
        az_state = AZS_CCWROT;
    else if (new_az > az_now)
        az_state = AZS_CWROT;
    else
        az_state = AZS_INPOS;

    // now save
    az_now = new_az;

    // el too if configured
    if (el_state != ELS_NONE) {

        // divine el state from change before changing el_now
        if (new_el < el_min + EL_DEADBAND)
            el_state = ELS_DOWNLIMIT;
        else if (new_el > el_max - EL_DEADBAND)
            el_state = ELS_UPLIMIT;
        else if (new_el < el_now)
            el_state = ELS_DOWNROT;
        else if (new_el > el_now)
            el_state = ELS_UPROT;
        else
            el_state = ELS_INPOS;

        // now save
        el_now = new_el;
    }

    // ok enough
    return (true);
}

/* get extra Az and EL info if possible, not fatal if can't.
 * TODO: we use Max Elevation == 0 to mean not supported, any better way?
 */
static void getAzElAux()
{
    StackMalloc rsp_mem(2000);
    char *rsp = (char *) rsp_mem.getMem();

    if (!askHamlib ("+\\dump_caps\n", rsp, rsp_mem.getSize()))
        return;

    (void) findHamlibRspValue (rsp, "Min Azimuth", &az_min);
    (void) findHamlibRspValue (rsp, "Max Azimuth", &az_max);
    (void) findHamlibRspValue (rsp, "Min Elevation", &el_min);
    (void) findHamlibRspValue (rsp, "Max Elevation", &el_max);

    if (el_max == 0)
        el_state = ELS_NONE;
    else
        el_state = ELS_STOPPED;

    Serial.printf ("GBL: Az %g .. %g EL %g .. %g\n", az_min, az_max, el_min, el_max);
}

/* send target az and el
 */
static bool setAzEl()
{
    char cmd[50];
    char rsp[50];

    snprintf (cmd, sizeof(cmd), "+\\set_pos %g %g\n", az_target, el_target);
    if (!askHamlib (cmd, rsp, sizeof(rsp))) {
        Serial.printf ("GBL: %s\n", rsp);
        return (false);
    }

    return (true);
}

/* try to connect hamlib_client to rotctld if not already.
 * if successful try to collect title and whether el axis.
 * print any error in the given plot box.
 * return whether successful.
 */
static bool connectHamlib (const SBox &box)
{
    char buf[50];

    // success if already connected
    if (connectionOk())
        return (true);

    if (debugLevel (DEBUG_GIMBAL, 1))
        Serial.printf ("GBL: starting connection attempt\n");

    // get connection info
    char host[NV_ROTHOST_LEN];
    int port;
    if (!getRotctld (host, &port)) {
        plotMessage (box, RA8875_RED, "Setup info disappeared");
        return (false);
    }

    // connect
    Serial.printf ("GBL: %s:%d\n", host, port);
    if (!hamlib_client.connect (host, port)) {
        char msg[NV_ROTHOST_LEN+30];
        snprintf (msg, sizeof(msg), "%s:%d connection failed", host, port);
        plotMessage (box, RA8875_RED, msg);
        return (false);
    }

    // get model if possible
    // many rotators have no info and report "None": show nothing in that case.
    title[0] = '\0';
    if (askHamlib ("+\\get_info\n", buf, sizeof(buf))) {
        char *name;
        if (findHamlibRspKey (buf, "Info", &name)) {
            char *eol = strchr (name, '\n');           // value is only the rest of this line
            if (eol)
                *eol = '\0';
            if (strcasecmp (name, "None") != 0 && strncmp (name, "RPRT", 4) != 0)
                snprintf (title, sizeof(title), "%.*s", (int)(sizeof(title)-1), name);
        }
    }

    // init target to current position
    if (!getAzEl(box)) {
        hamlib_client.stop();
        return (false);
    }
    az_target = az_now;
    el_target = el_now;

    // get auxillary info if possible
    getAzElAux();

    // stop
    stopGimbalNow();

    // ok
    return (true);
}

/* return whether a sat with the given rise and set azimuths will pass through either end of travel.
 * N.B. we assume raz and saz are in the range 0..360
 * N.B. we assume no satellite orbit can ever subtend more than 180 in az on the horizon.
 */
static bool passesThruEOT (float raz, float saz, bool isMoon)
{
    if (isMoon) {

        // TODO: moon can be up for wider than 180 degrees and can go through either N or S meridian.
        return (false);

    } else {

        // normalize as if az_min was north
        raz = fmodf (raz - az_min + 720, 360);
        saz = fmodf (saz - az_min + 720, 360);

        return ((raz > 180 && saz < raz - 180) || (raz < 180 && saz > raz + 180));
    }
}

/* draw current Track button state with message msg, else default.
 */
static void drawTrackButton(bool force, const char *msg)
{
    // decide string to draw
    const char *str = msg ? msg : "Auto";

    // avoid flashing from redrawing the same string in the same state
    static char prev_str[15];
    static bool prev_track;
    if (!force && prev_str[0] != 0 && strcmp (str, prev_str) == 0 && prev_track == auto_track)
        return;
    strncpy (prev_str, str, sizeof(prev_str)-1);                // preserve EOS
    prev_track = auto_track;

    // prepare button
    if (auto_track) {
        fillSBox (auto_b, RA8875_WHITE);
        tft.setTextColor (RA8875_BLACK);
    } else {
        fillSBox (auto_b, RA8875_BLACK);
        drawSBox (auto_b, RA8875_WHITE);
        tft.setTextColor (msg ? RA8875_RED : RA8875_WHITE);
    }

    // draw string
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    uint16_t sw = getTextWidth ((char*)str);
    tft.setCursor (auto_b.x+(auto_b.w-sw)/2, auto_b.y+3);
    tft.print (str);

    // wait a moment if message is temporary
    if (msg)
        wdDelay(1500);
}

/* draw Stop button in the given state
 */
static void drawStopButton (bool stop)
{
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    if (stop) {
        fillSBox (stop_b, RA8875_WHITE);
        tft.setTextColor (RA8875_BLACK);
    } else {
        fillSBox (stop_b, RA8875_BLACK);
        drawSBox (stop_b, RA8875_WHITE);
        tft.setTextColor (RA8875_WHITE);
    }
    tft.setCursor (stop_b.x+7, stop_b.y+3);
    tft.print ("Stop");
}

/* draw info for one axis in box.
 * N.B. we assume initGimbalGUI() has already been called.
 */
static void drawAxisInfo (const SBox &box, float target_value, float value_now, SBox &lbox, SBox &rbox,
    uint16_t y0, uint16_t state_color)
{
    // erase from indent to end of box
    tft.fillRect (box.x+VALU_INDENT, y0, box.w-VALU_INDENT-1, CHAR_H+1, RA8875_BLACK);

    // show value now
    char buf[10];
    snprintf (buf, sizeof(buf), "%4.0f", value_now);
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (RA8875_WHITE);
    tft.setCursor (box.x+VALU_INDENT, y0+CHAR_H);
    tft.print(buf);

    // show state diagram
    const uint16_t s_r = CHAR_H/2;
    uint16_t s_x = box.x + STATE_INDENT + s_r;
    if (y0 == AZ_Y) {
        uint16_t s_y = y0 + CHAR_H/2 + 2;
        tft.drawCircle (s_x, s_y, s_r, GRAY);
        int16_t l_dx = s_r*sinf(deg2rad(value_now));
        int16_t l_dy = s_r*cosf(deg2rad(value_now));
        tft.drawLine (s_x, s_y, s_x+l_dx, s_y-l_dy, 1, state_color);
    } else if (y0 == EL_Y) {
        uint16_t s_y = y0 + 3*CHAR_H/4;
        tft.drawCircle (s_x, s_y, s_r, GRAY);
        tft.drawLine (s_x-s_r, s_y, s_x+s_r, s_y, GRAY);
        tft.fillRect (s_x-s_r, s_y+1, 2*s_r+1, s_r, RA8875_BLACK);
        int16_t l_dx = s_r*cosf(deg2rad(value_now));
        int16_t l_dy = s_r*sinf(deg2rad(value_now));
        tft.drawLine (s_x, s_y, s_x+l_dx, s_y-l_dy, 1, state_color);
    }

    // show target value between control boxes
    uint16_t x_l = lbox.x + lbox.w + 1;
    uint16_t gap = rbox.x - x_l;
    tft.fillRect (x_l, lbox.y, gap, lbox.h, RA8875_BLACK);
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor (RA8875_WHITE);
    snprintf (buf, sizeof(buf), "%.0f", target_value);
    uint16_t b_w = getTextWidth(buf);
    tft.setCursor (x_l + (gap-b_w)/2, lbox.y+1);
    tft.print (buf);
}

/* draw or erase the up-and-over symbol
 */
static void drawUpOver()
{
    uint16_t r = elup2_b.h - 3;
    uint16_t x_c = elup2_b.x + 25;
    uint16_t y_c = elup2_b.y + elup2_b.h - 2;

    if (el_target > 90 || el_now > 90) {
        tft.drawCircle (x_c, y_c, r, UPOVER_COLOR);
        tft.drawLine (x_c-r, y_c+1, x_c-11*r/8, y_c-r/2, UPOVER_COLOR);
        tft.drawLine (x_c-r, y_c+1, x_c-r/2, y_c-r/2, UPOVER_COLOR);
        tft.fillRect (x_c-r-2, y_c+1, 2*r+4, r, RA8875_BLACK);
    } else {
        tft.fillRect (x_c-r-2, y_c-r-2, 9*r/4+4, r+4, RA8875_BLACK);
    }
}

/* return whether az target changed much since previous call
 */
static bool azTargetChanged()
{
    bool chg = roundf(az_target) != pgaz_target;
    pgaz_target = roundf(az_target);
    return (chg);
}

/* return whether az target changed much since previous call
 */
static bool elTargetChanged()
{
    bool chg = roundf(el_target) != pgel_target;
    pgel_target = roundf(el_target);
    return (chg);
}

/* return whether az now changed much since previous call
 */
static bool azNowChanged()
{
    bool chg = az_state != pgaz_state || roundf(az_now) != pgaz_now;
    pgaz_state = az_state;
    pgaz_now = roundf(az_now);
    return (chg);
}

/* return whether az now changed much since previous call
 */
static bool elNowChanged()
{
    bool chg = el_state != pgel_state || roundf(el_now)!= pgel_now;
    pgel_state = el_state;
    pgel_now = roundf(el_now);
    return (chg);
}

/* draw current state of gimbal in box.
 * N.B. we assume initGimbalGUI() has already been called.
 */
static void updateGimbalGUI(const SBox &box)
{
    // find az state description
    uint16_t color = 0;
    switch (az_state) {
    case AZS_STOPPED:
        color = BRGRAY;
        break;
    case AZS_CWROT:
        color = DYELLOW;
        break;
    case AZS_CCWROT:
        color = DYELLOW;
        break;
    case AZS_CCWLIMIT:
        color = RA8875_RED;
        break;
    case AZS_CWLIMIT:
        color = RA8875_RED;
        break;
    case AZS_INPOS:
        color = RA8875_GREEN;
        break;
    case AZS_UNKNOWN:
        color = RA8875_RED;
        break;
    case AZS_NONE:
        return;
    }

    // draw
    if (azNowChanged() || azTargetChanged())
        drawAxisInfo (box, az_target, az_now, azccw_b, azcw_b, AZ_Y, color);

    // show el state if gimbal
    if (el_state != ELS_NONE) {
        switch (el_state) {
        case ELS_STOPPED:
            color = BRGRAY;
            break;
        case ELS_UPROT:
            color = DYELLOW;
            break;
        case ELS_DOWNROT:
            color = DYELLOW;
            break;
        case ELS_DOWNLIMIT:
            color = RA8875_RED;
            break;
        case ELS_UPLIMIT:
            color = RA8875_RED;
            break;
        case ELS_INPOS:
            color = RA8875_GREEN;
            break;
        case ELS_UNKNOWN:
            color = RA8875_RED;
            break;
        case ELS_NONE:
            break;
        }

        // draw
        if (elNowChanged() || elTargetChanged())
            drawAxisInfo (box, el_target, el_now, eldown_b, elup_b, EL_Y, color);
    }

    // button
    drawTrackButton(false, NULL);
    drawStopButton(user_stop);

    // add up-over marker
    drawUpOver();
}

/* draw an expanding set of wavefronts in direction of az_now
 */
static void drawGimbalBeam()
{
    // thickness if any
    int lw = getRawPathWidth (ROTATOR_CSPR);
    if (lw == 0)
        return;

    // show full if long path unless showing sat
    float end_b = show_lp && !isSatDefined() ? 2*M_PIF : M_PIF;

    float ca, B;
    uint16_t col = getMapColor (ROTATOR_CSPR);
    float wave_step = deg2rad (4.0F/tft.SCALESZ);               // show wave front lines
    float az_0 = el_state != ELS_NONE && el_now > 90 ? az_now + 180 : az_now; // account for up over
    for (float b = 0; b < end_b; b += wave_step) {
        SCoord s1, s2 = {0, 0};
        for (int i = 0; i <= BEAM_W; i++) {                     // inclusive to get symmetric about center
            float beam_az = az_0 + (i-BEAM_W/2.0F);
            solveSphere (deg2rad(beam_az), b, sdelat, cdelat, &ca, &B);
            ll2sRaw (asinf(ca), fmodf(de_ll.lng+B+5*M_PIF,2*M_PIF)-M_PIF, s1, lw);
            if (b > M_PIF) {
                // show long path dotted
                if ((i%2) == 0)
                    tft.drawPixelRaw (s1.x, s1.y, col);
            } else if (s2.x != 0 && segmentSpanOkRaw (s1, s2, lw))
                tft.drawLineRaw (s1.x, s1.y, s2.x, s2.y, lw, col);
            s2 = s1;
        }
    }
}

static void drawArrow (const SBox &b, ArrowDir d)
{
    uint16_t x_c = b.x + b.w/2;         // x center
    uint16_t x_r = b.x + b.w - 1;       // x right
    uint16_t y_c = b.y + b.h/2;         // y center
    uint16_t y_b = b.y + b.h - 1;       // y bottom

    switch (d) {
    case AR_LEFT:
        tft.drawTriangle (b.x, y_c,   x_r, b.y,   x_r, y_b, ARROW_COLOR);
        break;

    case AR_DOWN:
        tft.drawTriangle (b.x, b.y,   x_r, b.y,   x_c, y_b, ARROW_COLOR);
        break;

    case AR_UP:
        tft.drawTriangle (x_c, b.y,   b.x, y_b,   x_r, y_b, ARROW_COLOR);
        break;

    case AR_RIGHT:
        tft.drawTriangle (b.x, b.y,   x_r, y_c,   b.x, y_b, ARROW_COLOR);
        break;
    }
}


/* determine sat_upover if we have a 2-axis gimbal tracking sats with el_max > 90.
 * "upover" means use gimbal el > 90 with opposite az to avoid tracking through wrap location.
 * N.B. beware sat rise/set times may not occur exactly when el is SAT_MIN_EL so avoid when near 
 *      by setting upover_pending then calling again often from updateGimbal().
 */
static void initUpOver()
{
    // approx el gap due to err in predicted rise/set times
    #define SAT_EL_RSERR  0.2F 

    // assume no
    sat_upover = false;

    // never for 1 axis system or one with no el travel
    if (el_state == ELS_NONE || el_max <= 90)
        return;

    SatNow satnow;
    if (getSatNow (satnow) && satnow.raz != SAT_NOAZ) {
        if (satnow.el < SAT_MIN_EL - SAT_EL_RSERR) {

            // sat not up so determine upover using next rise/set locations
            sat_upover = satnow.saz == SAT_NOAZ ? false : passesThruEOT (satnow.raz, satnow.saz, isSatMoon());
            upover_pending = false;
            if (debugLevel (DEBUG_GIMBAL, 1))
                Serial.printf ("GBL: UPOVER %d el %g raz %g saz %g\n", sat_upover, satnow.el, satnow.raz,
                                                                                        satnow.saz);

        } else if (satnow.el < SAT_MIN_EL + SAT_EL_RSERR) {

            // defer until out of abiguous range
            upover_pending = true;
            if (debugLevel (DEBUG_GIMBAL, 1))
                Serial.printf ("GBL: UPOVER pending el %g\n", satnow.el);

        } else {

            // sat is up now so determine upover using az now and set az at end of pass
            sat_upover = satnow.saz == SAT_NOAZ ? false : passesThruEOT (satnow.az, satnow.saz, isSatMoon());
            upover_pending = false;
            if (debugLevel (DEBUG_GIMBAL, 1))
                Serial.printf ("GBL: UPOVER %d el %g az %g saz %g\n", sat_upover, satnow.el, satnow.az,
                                                                                        satnow.saz);
        }
    }
}


/* init the gimbal GUI and state info
 */
static void initGimbalGUI(const SBox &box)
{
    // erase all then draw border
    prepPlotBox(box);

    // position main rows, Y depends on 1 or 2 axes
    AZ_Y = el_state != ELS_NONE ? box.y + box.h/3-10 : box.y + box.h/2-20;
    EL_Y = box.y + 2*box.h/3-18;

    // position controls
    const uint16_t az_y_center = AZ_Y + CHAR_H + LDIRBOX_SZ/2 + 5;

    // az arrow centers on a line
    azccw_b.x = box.x + box.w/3;
    azccw_b.y = az_y_center - SDIRBOX_SZ/2;
    azccw_b.w = SDIRBOX_SZ;
    azccw_b.h = SDIRBOX_SZ;

    azccw2_b.x = azccw_b.x - DIRBOX_GAP - LDIRBOX_SZ;
    azccw2_b.y = az_y_center - LDIRBOX_SZ/2;
    azccw2_b.w = LDIRBOX_SZ;
    azccw2_b.h = LDIRBOX_SZ;

    azcw_b.x = box.x + 2*box.w/3 - SDIRBOX_SZ;
    azcw_b.y = azccw_b.y;
    azcw_b.w = SDIRBOX_SZ;
    azcw_b.h = SDIRBOX_SZ;

    azcw2_b.x = azcw_b.x + SDIRBOX_SZ + DIRBOX_GAP;
    azcw2_b.y = azccw2_b.y;
    azcw2_b.w = LDIRBOX_SZ;
    azcw2_b.h = LDIRBOX_SZ;

    const uint16_t el_y_center = EL_Y + CHAR_H + LDIRBOX_SZ/2 + 5;

    // base of small el arrows same as base of large arrows
    eldown_b.x = azccw_b.x;
    eldown_b.y = el_y_center - LDIRBOX_SZ/2;
    eldown_b.w = SDIRBOX_SZ;
    eldown_b.h = SDIRBOX_SZ;

    eldown2_b.x = azccw2_b.x;
    eldown2_b.y = eldown_b.y;
    eldown2_b.w = LDIRBOX_SZ;
    eldown2_b.h = LDIRBOX_SZ;

    elup_b.x = azcw_b.x;
    elup_b.y = eldown_b.y + (LDIRBOX_SZ-SDIRBOX_SZ)/2 + 1;
    elup_b.w = SDIRBOX_SZ;
    elup_b.h = SDIRBOX_SZ;

    elup2_b.x = elup_b.x + SDIRBOX_SZ + DIRBOX_GAP;
    elup2_b.y = eldown_b.y;
    elup2_b.w = LDIRBOX_SZ;
    elup2_b.h = LDIRBOX_SZ;

    stop_b.x = box.x + box.w/8;
    stop_b.y = box.y + box.h - 20;
    stop_b.w = 2*box.w/8;
    stop_b.h = 15;

    auto_b.x = box.x + 4*box.w/8;
    auto_b.y = box.y + box.h - 20;
    auto_b.w = 3*box.w/8;
    auto_b.h = 15;

    // draw title, try to break at space if necessary to fit
    tft.setTextColor(RA8875_WHITE);
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    uint16_t tw = getTextWidth (title);
    while (tw > box.w - 6) {
        char *space = strrchr(title, ' ');
        if (space)
            *space = '\0';
        else
            title[strlen(title)-1] = '\0';
        tw = getTextWidth (title);
    }
    tft.setCursor (box.x+(box.w-tw)/2, box.y + PANETITLE_H);
    tft.print(title);

    // label az for sure
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor(BRGRAY);
    tft.setCursor (box.x+15, AZ_Y+CHAR_H);
    tft.print("Az");

    // az controls
    drawArrow (azccw_b, AR_LEFT);
    drawArrow (azccw2_b, AR_LEFT);
    drawArrow (azcw_b, AR_RIGHT);
    drawArrow (azcw2_b, AR_RIGHT);

    // el labels and controls only if gimbal
    if (el_state != ELS_NONE) {
        tft.setTextColor(BRGRAY);
        tft.setCursor (box.x+15, EL_Y+CHAR_H);
        tft.print("El");

        drawArrow (elup_b, AR_UP);
        drawArrow (elup2_b, AR_UP);
        drawArrow (eldown_b, AR_DOWN);
        drawArrow (eldown2_b, AR_DOWN);
    }

    // draw buttons
    drawStopButton(user_stop);
    drawTrackButton(true, NULL);

    // insure all previous values appear invalid so updateGimbalGUI will draw them
    pgaz_target = 999;
    pgel_target = 999;
    pgaz_now = 999;
    pgel_now = 999;
    pgaz_state = AZS_UNKNOWN;
    pgel_state = ELS_UNKNOWN;
}

static void toggleAutoTrack(void)
{
    auto_track = !auto_track;
    if (auto_track) {
        Serial.println ("GBL: track on");
        // this is the only command that automatically turns off Stop
        if (user_stop)
            user_stop = false;
        initUpOver();
        setAzEl();  // "unstop"
    } else {
        // always Stop when turning off Auto
        Serial.println ("GBL: track off");
        user_stop = true;
        stopGimbalNow();
    }
}

static void toggleStop(void)
{
    user_stop = !user_stop;
    if (user_stop) {
        Serial.println ("GBL: stop on");
        stopGimbalNow();
    } else {
        Serial.println ("GBL: stop off");
        setAzEl();  // "unstop"
    }
}


/* call any time to stop all motion immediately.
 * safe to call under any circumstances.
 */
void stopGimbalNow()
{
    if (connectionOk()) {
        char buf[100];
        if (!askHamlib ("+\\stop\n", buf, sizeof(buf)))
            Serial.printf ("GBL: %s\n", buf);
    }

    az_target = az_now;
    el_target = el_now;

    auto_track = false;
    sat_upover = false;
    user_stop = true;
}

/* insure disconnected from hamlib
 */
void closeGimbal()
{
    if (connectionOk()) {
        Serial.print ("GBL: disconnected\n");
        hamlib_client.stop();
    }
}

/* return whether we are built to handle a rotator; not whether one is actually connected now.
 */
bool haveGimbal()
{
    return (getRotctld (NULL, NULL));
}

/* called often to update gimbal (or rotator).
 */
void updateGimbal (const SBox &box)
{
    // draw beam every time
    drawGimbalBeam();

    // other stuff not crazy often
    static uint32_t prev_ms;
    if (!timesUp(&prev_ms, UPDATE_MS))
        return;

    // init GUI if just opened
    if (!connectionOk()) {
        if (!connectHamlib (box))
            return;             // already informed op
        initGimbalGUI(box);
    }

    // get current positions
    if (!getAzEl(box)) {
        closeGimbal();
        return;
    }

    // if auto: set target to satellite if one is defined and we have a gimbal, else DX az
    if (auto_track) {

        // can reject sat for several reasons
        bool sat_ok = false;

        if (el_state != ELS_NONE) {

            // try getting sat location
            SatNow satnow;
            if (getSatNow (satnow)) {

                // sat is defined but we also require current time
                if (!goodTime()) {

                    auto_track = false;
                    stopGimbalNow();
                    drawTrackButton(false, "Not UTC");

                } else {

                    // compute new upover if new pass or pending an accurate prediction
                    if (isNewPass() || upover_pending)
                        initUpOver();

                    // just hold position if still pending
                    if (!upover_pending) {

                        // decide how to track
                        if (satnow.el < SAT_MIN_EL && satnow.raz == SAT_NOAZ) {
                            // down now and doesn't rise
                            stopGimbalNow();
                            drawTrackButton(false, "No Rise");
                            return;
                        }

                        if (satnow.el < SAT_MIN_EL) {
                            // sat not up yet so sit on horizon at its rise az
                            if (sat_upover) {
                                az_target = fmodf (satnow.raz + 180 + 360, 360);
                                el_target = 180;
                            } else {
                                az_target = satnow.raz;
                                el_target = 0;
                            }

                        } else if (sat_upover) {
                            // avoid wrap by running upside down
                            az_target = fmodf (satnow.az + 180 + 360, 360);
                            el_target = 180 - satnow.el;

                        } else {
                            // no mods required
                            az_target = satnow.az;
                            el_target = satnow.el;
                        }

                        // az came in 0..360, fix into gimbal coords if necessary
                        if (az_target > az_max)
                            az_target -= 360;
                        else if (az_target < az_min)
                            az_target += 360;

                        // move into mount range
                        if (az_target < az_min)
                            az_target += 360;
                        if (az_target > az_max)
                            az_target -= 360;
                    }

                    // sat is go
                    sat_ok = true;
                }
            }
        }

        if (!sat_ok) {

            // no sat or no el so point at DX, time does not matter
            float dist, bear;
            propDEPath (show_lp, dx_ll, &dist, &bear);          // honor desired short/long path
            az_target = rad2deg(bear);
            if (el_state != ELS_NONE && el_now > 90)
                az_target = fmodf (az_target + 180 + 720, 360);
        }

    } // else move to location commanded from GUI

    if (!user_stop && (azTargetChanged() || elTargetChanged()))
        setAzEl();
    updateGimbalGUI(box);
}

/* handle a touch in our pane.
 * return whether really ours or just tapped on title to leave
 */
bool checkGimbalTouch (const SCoord &s, const SBox &box)
{
    // out fast if not ours 
    if (!inBox (s, box))
        return (false);

    // our box but disavow and stop if leaving by tapping title
    if (s.y < box.y + PANETITLE_H) {
        stopGimbalNow();
        closeGimbal();
        return (false);
    }

    // if click while no connection, try to restablish, if still can't then move on
    if (!connectionOk()) {
        if (connectHamlib(box)) {
            initGimbalGUI(box);
            return (true);
        } else
            return (false);
    }

    // check manual controls
    if (inBox (s, stop_b)) {
        toggleStop();
    } else if (inBox (s, auto_b)) {
        toggleAutoTrack();
    } else if (inBox (s, azccw_b)) {
        az_target = (auto_track ? az_now : az_target) - AZSTEP;
        az_target -= fmodf (az_target, AZSTEP);
        az_target = fmaxf (az_min, az_target);
        if (!user_stop) {
            auto_track = false;
            sat_upover = false;
        }
    } else if (inBox (s, azccw2_b)) {
        az_target = (auto_track ? az_now : az_target) - AZSTEP2;
        az_target -= fmodf (az_target, AZSTEP);
        az_target = fmaxf (az_min, az_target);
        if (!user_stop) {
            auto_track = false;
            sat_upover = false;
        }
    } else if (inBox (s, azcw_b)) {
        az_target = (auto_track ? az_now : az_target) + AZSTEP;
        az_target -= fmodf (az_target, AZSTEP);
        az_target = fminf (az_max, az_target);
        if (!user_stop) {
            auto_track = false;
            sat_upover = false;
        }
    } else if (inBox (s, azcw2_b)) {
        az_target = (auto_track ? az_now : az_target) + AZSTEP2;
        az_target -= fmodf (az_target, AZSTEP);
        az_target = fminf (az_max, az_target);
        if (!user_stop) {
            auto_track = false;
            sat_upover = false;
        }
    } else if (el_state != ELS_NONE) {
        if (inBox (s, eldown_b)) {
            el_target = (auto_track ? el_now : el_target) - ELSTEP;
            el_target -= fmodf (el_target, ELSTEP);
            el_target = fmaxf (el_min, el_target);
            if (!user_stop) {
                auto_track = false;
                sat_upover = false;
            }
        } else if (inBox (s, eldown2_b)) {
            el_target = (auto_track ? el_now : el_target) - ELSTEP2;
            el_target -= fmodf (el_target, ELSTEP);
            el_target = fmaxf (el_min, el_target);
            if (!user_stop) {
                auto_track = false;
                sat_upover = false;
            }
        } else if (inBox (s, elup_b)) {
            el_target = (auto_track ? el_now : el_target) + ELSTEP;
            el_target -= fmodf (el_target, ELSTEP);
            el_target = fminf (el_max, el_target);
            if (!user_stop) {
                auto_track = false;
                sat_upover = false;
            }
        } else if (inBox (s, elup2_b)) {
            el_target = (auto_track ? el_now : el_target) + ELSTEP2;
            el_target -= fmodf (el_target, ELSTEP);
            el_target = fminf (el_max, el_target);
            if (!user_stop) {
                auto_track = false;
                sat_upover = false;
            }
        }
    }

    if (debugLevel (DEBUG_GIMBAL, 1))
        Serial.printf ("GBL: target after touch %g %g\n", az_target, el_target);

    // ours
    return (true);
}

/* get gimbal state: whether pane is showing, has el axis, stopped and auto.
 * return whether configured at all
 */
bool getGimbalState (bool &conn, bool &vis_now, bool &has_el, bool &is_stop, bool &is_auto,
float &az, float &el)
{
    if (!haveGimbal())
        return (false);

    vis_now = findPaneChoiceNow (PLOT_CH_GIMBAL) != PANE_NONE;
    has_el = el_state != ELS_NONE;
    conn = connectionOk();
    is_stop = user_stop;
    is_auto = auto_track;
    az = az_now;
    el = el_now;

    return (true);
}

/* external rotator command using the given strings.
 * state=[un]stop|[un]auto
 * return true else false with brief reason ynot.
 */
bool commandRotator (const char *new_state, const char *new_az, const char *new_el, Message &ynot)
{
    if (!haveGimbal()) {
        ynot.set("Rotator not enabled");
        return (false);
    }
    if (!connectionOk()) {
        ynot.set("Rotator not connected");
        return (false);
    }

    if (new_state) {
        if (strcmp (new_state, "auto") == 0) {
            if (!auto_track)
                toggleAutoTrack();
        } else if (strcmp (new_state, "unauto") == 0) {
            if (auto_track)
                toggleAutoTrack();
        } else if (strcmp (new_state, "stop") == 0) {
            if (!user_stop)
                toggleStop();
        } else if (strcmp (new_state, "unstop") == 0) {
            if (user_stop)
                toggleStop();
        } else {
            ynot.set("state must be stop, unstop, auto or unauto");
            return (false);
        }
    }

    if (new_az) {
        float cmd_az = atof (new_az);
        if (cmd_az < az_min || cmd_az > az_max) {
            ynot.printf ("az must be %g .. %g\n", az_min, az_max);
            return (false);
        }
        az_target = cmd_az;
    }

    if (new_el) {
        float cmd_el = atof (new_el);
        if (cmd_el < el_min || cmd_el > el_max) {
            ynot.printf ("el must be %g .. %g\n", el_min, el_max);
            return (false);
        }
        el_target = cmd_el;
    }

    // ok!
    return (true);
}
