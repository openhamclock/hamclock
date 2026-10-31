/* Northen California DX Foundation Beacon Network.
 * http://www.ncdxf.org/beacon/index.html#Schedule
 */

/* manage the Northern California DX Foundation beacons.
 *
 * Each beacon is drawn as a colored triangle symbol with call sign text below. The triangle is drawn
 * to high res so is redrawn after being scanned. But the text is just jumped over and never redrawn.
 */

#include "HamClock.h"


#define NBEACONS        18                      // number of beacons
#define BEACONR         9                       // beacon symbol radius, pixels
#define BLEG            (BEACONR-4)             // beacon symbol leg length
#define BEACONCW        6                       // beacon char width

typedef struct {
    int16_t lat, lng;                           // location, degs north and east
    char call[7];                               // call sign
    SCoord s;                                   // screen coord of triangle symbol center
    uint16_t c;                                 // color
    SBox call_b;                                // enclosing background box
    bool is_drawn;                              // whether currently drawn on map
} NCDXFBeacon;

/* listed in order of 14, 18, 21, 24 and 28 MHz starting at 3N minutes after the hour.
 * 4 of the 18 stations each transmit for 10 seconds then rotate down.
 *
 * given s seconds after the hour, find index for each frequency:
 *   14 MHz i = (s/10+0+NBEACONS)%NBEACONS
 *   18 MHz i = (s/10-1+NBEACONS)%NBEACONS
 *   21 MHz i = (s/10-2+NBEACONS)%NBEACONS
 *   24 MHz i = (s/10-3+NBEACONS)%NBEACONS
 *   28 MHz i = (s/10-4+NBEACONS)%NBEACONS
 */
static NCDXFBeacon blist[NBEACONS] = {
    {  41,  -74, "4U1UN",  {0,0}, 0, {0,0,0,0}, false},
    {  68, -133, "VE8AT",  {0,0}, 0, {0,0,0,0}, false},
    {  37, -122, "W6WX",   {0,0}, 0, {0,0,0,0}, false},
    {  21, -156, "KH6RS",  {0,0}, 0, {0,0,0,0}, false},
    { -41,  176, "ZL6B",   {0,0}, 0, {0,0,0,0}, false},
    { -32,  116, "VK6RBP", {0,0}, 0, {0,0,0,0}, false},
    {  34,  137, "JA2IGY", {0,0}, 0, {0,0,0,0}, false},
    {  55,   83, "RR9O",   {0,0}, 0, {0,0,0,0}, false},
    {  22,  114, "VR2B",   {0,0}, 0, {0,0,0,0}, false},
    {   7,   80, "4S7B",   {0,0}, 0, {0,0,0,0}, false},
    { -26,   28, "ZS6DN",  {0,0}, 0, {0,0,0,0}, false},
    {  -1,   37, "5Z4B",   {0,0}, 0, {0,0,0,0}, false},
    {  32,   35, "4X6TU",  {0,0}, 0, {0,0,0,0}, false},
    {  60,   25, "OH2B",   {0,0}, 0, {0,0,0,0}, false},
    {  33,  -17, "CS3B",   {0,0}, 0, {0,0,0,0}, false},
    { -35,  -58, "LU4AA",  {0,0}, 0, {0,0,0,0}, false},
    { -12,  -77, "OA4B",   {0,0}, 0, {0,0,0,0}, false},
    {   9,  -68, "YV5B",   {0,0}, 0, {0,0,0,0}, false},
};


/* symbol color for each frequency
 */
#define BCOL_14 getMapColor(BAND20_CSPR)
#define BCOL_18 getMapColor(BAND17_CSPR)
#define BCOL_21 getMapColor(BAND15_CSPR)
#define BCOL_24 getMapColor(BAND12_CSPR)
#define BCOL_28 getMapColor(BAND10_CSPR)
#define BCOL_S  RA8875_BLACK            // silent, not actually drawn
#define BCOL_N  6                       // number of color states



/* using the current user time set the color state for each beacon.
 */
static void setBeaconStates ()
{
    time_t t = nowWO();
    int mn = minute(t);
    int sc = second(t);
    uint16_t s_10 = (60*mn + sc)/10;

    for (int id = 0; id < NBEACONS; id++)
        blist[id].c = BCOL_S;

    blist[(s_10-0+NBEACONS)%NBEACONS].c = BCOL_14;
    blist[(s_10-1+NBEACONS)%NBEACONS].c = BCOL_18;
    blist[(s_10-2+NBEACONS)%NBEACONS].c = BCOL_21;
    blist[(s_10-3+NBEACONS)%NBEACONS].c = BCOL_24;
    blist[(s_10-4+NBEACONS)%NBEACONS].c = BCOL_28;
}


/* draw beacon symbol centered at given screen location with the given color
 */
static void drawBeaconSymbol (const SCoord &s, uint16_t c)
{
    tft.fillTriangle (s.x, s.y-BEACONR, s.x-9*BEACONR/10, s.y+BEACONR/2,
                s.x+9*BEACONR/10, s.y+BEACONR/2, RA8875_BLACK);
    tft.fillTriangle (s.x, s.y-BLEG, s.x-9*BLEG/10, s.y+BLEG/2, s.x+9*BLEG/10, s.y+BLEG/2, c);
}

/* draw the given beacon, including callsign beneath.
 */
static void drawBeacon (NCDXFBeacon &nb)
{
    // triangle symbol
    drawBeaconSymbol (nb.s, nb.c);

    // draw call sign
    drawMapTag (nb.call, nb.call_b);

    nb.is_drawn = true;
}

/* return the screen box enclosing nb's symbol and call sign tag, clipped to be non-negative.
 */
static SBox beaconBox (const NCDXFBeacon &nb)
{
    int x0 = nb.s.x - BEACONR - 1;
    int y0 = nb.s.y - BEACONR - 1;
    int x1 = nb.s.x + BEACONR + 1;
    int y1 = nb.s.y + BEACONR + 1;
    if ((int)nb.call_b.x < x0) x0 = nb.call_b.x;
    if ((int)nb.call_b.y < y0) y0 = nb.call_b.y;
    if ((int)(nb.call_b.x + nb.call_b.w) > x1) x1 = nb.call_b.x + nb.call_b.w;
    if ((int)(nb.call_b.y + nb.call_b.h) > y1) y1 = nb.call_b.y + nb.call_b.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    SBox b;
    b.x = x0;
    b.y = y0;
    b.w = x1 - x0 + 1;
    b.h = y1 - y0 + 1;
    return (b);
}

/* grow *u to also include b; *have says whether *u is valid yet.
 */
static void growBox (SBox *u, bool *have, const SBox &b)
{
    if (!*have) {
        *u = b;
        *have = true;
        return;
    }
    uint16_t x1 = (u->x + u->w > b.x + b.w) ? u->x + u->w : b.x + b.w;
    uint16_t y1 = (u->y + u->h > b.y + b.h) ? u->y + u->h : b.y + b.h;
    if (b.x < u->x) u->x = b.x;
    if (b.y < u->y) u->y = b.y;
    u->w = x1 - u->x;
    u->h = y1 - u->y;
}

/* erase the given beacon symbol and call sign tag from the map by restoring the underlying map pixels.
 * N.B. this restores ONLY the base map. Anything that was drawn on top of the beacon area (grid,
 *   spot/DX/PSK paths, satellite path, ...) is wiped too and the caller must repaint it -- see
 *   updateBeacons(), which does so with redrawMapBox().
 */
static void eraseBeacon (NCDXFBeacon &nb)
{
    if (overMap (nb.s)) {
        // triangle symbol
        SCircle sym_c;
        sym_c.s = nb.s;
        sym_c.r = BEACONR;
        eraseSCircle (sym_c);

        // call sign tag beneath it
        for (uint16_t dy = 0; dy < nb.call_b.h; dy++)
            for (uint16_t dx = 0; dx < nb.call_b.w; dx++)
                drawMapCoord (nb.call_b.x + dx, nb.call_b.y + dy);
    }

    nb.is_drawn = false;
}

/* tell us that the map pixels in box (or the whole map if box is NULL) have just been repainted
 * from the base map, so any beacon symbols/tags wholly within it are already gone from the screen.
 * This must be called before updateBeacons() runs for that repaint so that a beacon which has since
 * gone silent is NOT "erased" a second time -- that erase restores base map pixels on top of the
 * paths and grid lines which were just (re)drawn and leaves a symbol+tag shaped hole in them.
 */
void beaconsMapRepainted (const SBox *box)
{
    for (NCDXFBeacon *bp = blist; bp < &blist[NBEACONS]; bp++) {
        if (!bp->is_drawn)
            continue;
        if (!box) {
            bp->is_drawn = false;
        } else {
            SBox bb = beaconBox (*bp);
            if (bb.x >= box->x && bb.y >= box->y
                        && bb.x + bb.w <= box->x + box->w && bb.y + bb.h <= box->y + box->h)
                bp->is_drawn = false;
        }
    }
}

/* erase every beacon symbol and call sign tag from the map by restoring the underlying map
 * pixels. called once, right when beacons transition from on to off, so they don't linger
 * on screen until the next unrelated full map redraw happens to paint over them.
 */
static void eraseBeacons (void)
{
    for (NCDXFBeacon *bp = blist; bp < &blist[NBEACONS]; bp++) {
        if (bp->is_drawn)
            eraseBeacon (*bp);
    }

    if (!mapSweepActive())
        tft.drawPR();
}

/* update map beacons, typically on each 10 second period unless immediate.
 */
void updateBeacons (bool immediate)
{
    // counts as on as long as in rotation set, need not be in front now
    bool beacons_on = brb_rotset & (1 << BRB_SHOW_BEACONS);

    // if just turned off, erase immediately instead of leaving stale symbols on the map
    // until whatever next unrelated event happens to trigger a full map redraw
    static bool was_on;
    if (!beacons_on) {
        if (was_on) {
            eraseBeacons();
            was_on = false;
        }
        return;
    }
    was_on = true;

    // process if immediate or it's a new time period and no map sweep is active
    static uint8_t prev_sec10;
    uint8_t sec10 = second(nowWO())/10;
    if (!immediate && (sec10 == prev_sec10 || mapSweepActive()))
        return;
    prev_sec10 = sec10;

    // now update each beacon as required
    setBeaconStates();
    bool any_changed = false;
    bool any_erased = false;
    SBox erased_b = {0,0,0,0};
    for (NCDXFBeacon *bp = blist; bp < &blist[NBEACONS]; bp++) {
        if (bp->c != BCOL_S) {
            if (overMap(bp->s) && !overRSS (bp->call_b)) {
                drawBeacon (*bp);
                any_changed = true;
            }
        } else if (bp->is_drawn) {
            SBox bb = beaconBox (*bp);
            eraseBeacon (*bp);
            growBox (&erased_b, &any_erased, bb);
            any_changed = true;
        }
    }

    // erasing a beacon restored just the base map, punching a symbol+tag shaped hole in whatever
    // else lives there (grid, spot/DX/PSK paths, sat path, ...). repaint that area properly, but not
    // while a full sweep is in progress (it will repaint everything anyway) nor recursively, since
    // redrawMapBox() reaches us again through drawAllSymbols().
    static bool in_repaint;
    if (any_erased && !mapSweepActive() && !in_repaint) {
        in_repaint = true;
        if (erased_b.x + erased_b.w > map_b.x + map_b.w)
            erased_b.w = map_b.x + map_b.w - erased_b.x;
        if (erased_b.y + erased_b.h > map_b.y + map_b.h)
            erased_b.h = map_b.y + map_b.h - erased_b.y;
        redrawMapBox (erased_b);
        in_repaint = false;
    } else if (any_changed && !immediate && !mapSweepActive()) {
        tft.drawPR();
    }

    updateClocks(false);
}

/* update screen location for all beacons.
 */
void updateBeaconMapLocations()
{
    for (NCDXFBeacon *bp = blist; bp < &blist[NBEACONS]; bp++) {
        ll2s (deg2rad(bp->lat), deg2rad(bp->lng), bp->s, 3*BEACONCW);   // about max
        setMapTagBox (bp->call, bp->s, BEACONR/2+1, bp->call_b);
        bp->is_drawn = false;
    }
}

/* draw the beacon key in NCDXF_b.
 */
static void drawBeaconKey()
{
    // tiny font
    selectFontStyle (BOLD_FONT, FAST_FONT);

    // draw title
    tft.setCursor (NCDXF_b.x+13, NCDXF_b.y+2);
    tft.setTextColor (RA8875_WHITE);
    tft.print ("NCDXF");

    // draw each key

    SCoord s;
    s.x = NCDXF_b.x + BEACONR+1;
    s.y = NCDXF_b.y + 16 + BEACONR;
    uint8_t dy = (NCDXF_b.h-16)/(BCOL_N-1);         // silent color not drawn
    uint16_t c;
    
    c = BCOL_14;
    drawBeaconSymbol (s, c);
    tft.setTextColor (c);
    tft.setCursor (s.x+BEACONR, s.y-BEACONR/2);
    tft.print ("14.10");

    s.y += dy;
    c = BCOL_18;
    drawBeaconSymbol (s, c);
    tft.setTextColor (c);
    tft.setCursor (s.x+BEACONR, s.y-BEACONR/2);
    tft.print ("18.11");

    s.y += dy;
    c = BCOL_21;
    drawBeaconSymbol (s, c);
    tft.setTextColor (c);
    tft.setCursor (s.x+BEACONR, s.y-BEACONR/2);
    tft.print ("21.15");

    s.y += dy;
    c = BCOL_24;
    drawBeaconSymbol (s, c);
    tft.setTextColor (c);
    tft.setCursor (s.x+BEACONR, s.y-BEACONR/2);
    tft.print ("24.93");

    s.y += dy;
    c = BCOL_28;
    drawBeaconSymbol (s, c);
    tft.setTextColor (c);
    tft.setCursor (s.x+BEACONR, s.y-BEACONR/2);
    tft.print ("28.20");
}

/* draw any of the various contents in NCDXF_b depending on brb_mode.
 * most just draw but return success for options that require fresh lookups.
 */
bool drawNCDXFBox()
{
    bool ok = true;

    // erase
    fillSBox (NCDXF_b, RA8875_BLACK);

    // draw appropriate content
    switch ((BRB_MODE)brb_mode) {

    case BRB_SHOW_BEACONS:

        drawBeaconKey();
        break;

    case BRB_SHOW_SWSTATS:

        (void) checkForNewSpaceWx();
        drawNCDXFSpcWxStats(RA8875_BLACK);
        break;

    case BRB_SHOW_BME76:        // fallthru
    case BRB_SHOW_BME77:

        drawBMEStats();
        break;

    case BRB_SHOW_ONOFF:        // fallthru
    case BRB_SHOW_PHOT:         // fallthru
    case BRB_SHOW_BR:

        drawBrightness();
        break;

    case BRB_SHOW_DEWX: // fallthru
    case BRB_SHOW_DXWX:
        ok = drawNCDXFWx ((BRB_MODE)brb_mode);
        break;

    case BRB_SHOW_LIGHTNING:
        drawNCDXFLightningStats();
        break;

    case BRB_N:
        
        // lint
        break;
    }

    // border -- avoid base line
    tft.drawLine (NCDXF_b.x, NCDXF_b.y, NCDXF_b.x+NCDXF_b.w-1, NCDXF_b.y, GRAY);
    tft.drawLine (NCDXF_b.x, NCDXF_b.y, NCDXF_b.x, NCDXF_b.y+NCDXF_b.h-1, GRAY);
    tft.drawLine (NCDXF_b.x+NCDXF_b.w-1, NCDXF_b.y, NCDXF_b.x+NCDXF_b.w-1, NCDXF_b.y+NCDXF_b.h-1, GRAY);

    // ack
    return (ok);
}

/* common template to draw table of stats in NCDXF_b.
 * divide NCDXF_b into NCDXF_B_NFIELDS equal regions vertically with titles near each bottom.
 * use white text and colors for each unless color is black in which case us it for everything.
 */
void drawNCDXFStats (uint16_t color,
                     const char titles[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN],
                     const char values[NCDXF_B_NFIELDS][NCDXF_B_MAXLEN],
                     const uint16_t colors[NCDXF_B_NFIELDS])
{
    // each box height and nice offsets down to value and title
    const uint16_t h = NCDXF_b.h / NCDXF_B_NFIELDS;
    const uint16_t vo = 24*h/37;                         // font baseline
    const uint16_t to = 28*h/37;                         // font top

    // show each item
    for (int i = 0; i < NCDXF_B_NFIELDS; i++) {

        // top height
        uint16_t y = i * NCDXF_b.h / NCDXF_B_NFIELDS;

        selectFontStyle (LIGHT_FONT, SMALL_FONT);
        tft.setTextColor (color == RA8875_BLACK ? colors[i] : color);
        tft.fillRect (NCDXF_b.x+1, y, NCDXF_b.w-2, h, RA8875_BLACK);
        // tft.drawRect (NCDXF_b.x+1, y, NCDXF_b.w-2, h, RA8875_RED);              // RBF
        tft.setCursor (NCDXF_b.x + (NCDXF_b.w-getTextWidth(values[i]))/2, y+vo);
        tft.print (values[i]);

        selectFontStyle (LIGHT_FONT, FAST_FONT);
        tft.setTextColor (color == RA8875_BLACK ? RA8875_WHITE : color);
        tft.setCursor (NCDXF_b.x + (NCDXF_b.w-getTextWidth(titles[i]))/2, y+to);
        tft.print (titles[i]);
    }
}

/* init brb_rotset and brb_mode
 */
void initBRBRotset()
{
    if (!NVReadUInt16 (NV_BRB_ROTSET, &brb_rotset)) {
        // check for old style
        uint8_t old_rotset;
        if (NVReadUInt8 (NV_BRB_ROTSET_OLD, &old_rotset))
            brb_rotset = old_rotset;
        else
            brb_rotset = 0;
    }
    checkBRBRotset();
}

/* insure brb_rotset and brb_mode are set sensibly
 */
void checkBRBRotset()
{
    if (!brb_rotset) {
        brb_mode = BRB_SHOW_BEACONS;            // just pick one that is always possible
        brb_rotset = 1 << BRB_SHOW_BEACONS;    
    } else {
        // arbitrarily set brb_mode to first set bit
        for (int i = 0; i < BRB_N; i++) {
            if (brb_rotset & (1 << i)) {
                brb_mode = i;
                break;
            }
        }
    }
    NVWriteUInt16 (NV_BRB_ROTSET, brb_rotset);
    logBRBRotSet();
}
