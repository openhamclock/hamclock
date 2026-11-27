/* North American time zones map overlay -- see natzones.h.
 *
 * Selected from the map View menu as Grid: "NA Time Zones".  Three parts:
 *
 *   drawNATZFill()  tints each zone and hatches the ones that never change clocks, in every projection.
 *                   N.B. blending is not idempotent so this must be called exactly once per repaint of
 *                   the underlying pixels, see earthmap.cpp.
 *   drawNATZones()  draws the zone-to-zone borders and a live clock label in each zone (all projections).
 *                   This is a normal idempotent grid overlay, called from drawMapGrid().
 *   natz_rules[]    the time rules: standard offset, and whether the zone follows the North American
 *                   daylight rule or keeps one time all year.  Edit the table if a rule changes.
 */

#include "HamClock.h"
#include "natzones.h"


/*********************************************************************************************
 *
 * time rules
 *
 */

typedef struct {
    const char *std_name;               // label abbreviation in standard time
    const char *dst_name;               // ... in daylight time, NULL if the clocks never change
    int16_t std_min;                    // standard time, minutes east of UTC
    uint8_t rgb[3];                     // tint color
} NATZRule;

// N.B. same order as enum NATZoneId. Colors are those of the NorthAmericanTimeZones web page.
//
// Canada's permanent-time zones (BC, NT, AB, MB) are fixed offsets here because that is set by provincial
// legislation. The same list on the web page is in tz-map.template.html (table CA). If a province
// changes its mind, change std_min and dst_name here.
static const NATZRule natz_rules[NZ_N] = {
    /* NZ_BCT  */ { "BCT",  NULL,   -420, {0x4f,0x8a,0x8b} },
    /* NZ_YKT  */ { "YKT",  NULL,   -420, {0x9a,0x6f,0x86} },
    /* NZ_NWT  */ { "NWT",  NULL,   -360, {0x6a,0x9a,0x7a} },
    /* NZ_ABT  */ { "ABT",  NULL,   -360, {0xa5,0x85,0x52} },
    /* NZ_SKT  */ { "SKT",  NULL,   -360, {0x8a,0x9a,0x55} },
    /* NZ_MBT  */ { "MBT",  NULL,   -300, {0x5e,0x8a,0xa8} },
    /* NZ_EAST */ { "EST",  "EDT",  -300, {0xa1,0x62,0x5f} },
    /* NZ_ATL  */ { "AST",  "ADT",  -240, {0x8a,0x6a,0xa0} },
    /* NZ_NLT  */ { "NST",  "NDT",  -210, {0x6f,0xa0,0x8a} },
    /* NZ_PAC  */ { "PST",  "PDT",  -480, {0x4f,0x6f,0x9a} },
    /* NZ_MTN  */ { "MST",  "MDT",  -420, {0x9a,0x7a,0x5f} },
    /* NZ_ARIZ */ { "MST",  NULL,   -420, {0xb0,0xa0,0x5f} },
    /* NZ_CEN  */ { "CST",  "CDT",  -360, {0x5f,0x8f,0x7a} },
    /* NZ_AK   */ { "AKST", "AKDT", -540, {0x6c,0x5f,0x8f} },
    /* NZ_HI   */ { "HST",  NULL,   -600, {0x8f,0x7f,0xb0} },
    /* NZ_PR   */ { "AST",  NULL,   -240, {0x7a,0x8f,0x9a} },
    /* NZ_MXN  */ { "PST",  "PDT",  -480, {0x3f,0x7f,0x9f} },       // Baja California follows US rules
    /* NZ_MXP  */ { "MST",  NULL,   -420, {0xa8,0x8a,0x4a} },
    /* NZ_MXC  */ { "CST",  NULL,   -360, {0x7a,0x9a,0x5a} },
    /* NZ_MXS  */ { "EST",  NULL,   -300, {0x9a,0x5f,0x7a} },
};

// show 12 hour times like 9:15p (1) or 24 hour times like 21:15 (0)
#define NATZ_12HOUR     1

// tint strength, 0..256 of zone color mixed into the map underneath
#define NATZ_TINT       96

// how many label heights up or down a tier 1 label may move when it would land on another label
// (tier 2 may move one, tier 3 not at all)
#define NATZ_MAXNUDGE   3


/* days since 1970-01-01 of the given civil date, proleptic Gregorian, m 1..12.
 * from Howard Hinnant's date algorithms.
 */
static int32_t daysFromCivil (int y, int m, int d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y-399) / 400;
    const int yoe = y - era*400;
    const int doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1;
    const int doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return (era*146097 + doe - 719468);
}

/* weekday of the given day number: 0 = Sunday
 */
static int weekdayOfDays (int32_t days)
{
    int w = (days + 4) % 7;             // 1970-01-01 was a Thursday
    return (w < 0 ? w + 7 : w);
}

/* day number of the nth (1 based) Sunday of the given month
 */
static int32_t nthSunday (int y, int m, int n)
{
    int32_t d1 = daysFromCivil (y, m, 1);
    int32_t first_sun = d1 + (7 - weekdayOfDays(d1)) % 7;
    return (first_sun + 7*(n-1));
}

/* return whether North American daylight time is in effect at utc for a zone whose standard offset
 * is std_min: from 02:00 standard on the second Sunday of March until 02:00 daylight on the first
 * Sunday of November.
 */
static bool naDSTNow (time_t utc, int std_min)
{
    // local standard calendar year decides which year's rule applies
    time_t loc = utc + (time_t)std_min*60;
    struct tm tm;
    gmtime_r (&loc, &tm);
    int y = tm.tm_year + 1900;

    int64_t start_utc = (int64_t)nthSunday (y, 3, 2)*86400 + 2*3600 - (int64_t)std_min*60;
    int64_t end_utc   = (int64_t)nthSunday (y, 11, 1)*86400 + 2*3600 - (int64_t)(std_min+60)*60;
    return (utc >= start_utc && utc < end_utc);
}

/* get label name and local time in minutes since local midnight for the given zone at utc.
 * return whether daylight (summer) time is in effect.
 */
static bool natzLocalTime (NATZoneId z, time_t utc, const char **name, int *hr, int *mn)
{
    const NATZRule &r = natz_rules[z];
    bool dst = r.dst_name != NULL && naDSTNow (utc, r.std_min);
    int off_min = r.std_min + (dst ? 60 : 0);

    time_t loc = utc + (time_t)off_min*60;
    struct tm tm;
    gmtime_r (&loc, &tm);

    *name = dst ? r.dst_name : r.std_name;
    *hr = tm.tm_hour;
    *mn = tm.tm_min;
    return (dst);
}

/* format the local time of the given zone as a label like "EDT 9:15p"
 */
static void natzLabelText (NATZoneId z, time_t utc, char *buf, size_t buf_len)
{
    const char *name;
    int hr, mn;
    natzLocalTime (z, utc, &name, &hr, &mn);

#if NATZ_12HOUR
    int h12 = hr % 12;
    if (h12 == 0)
        h12 = 12;
    snprintf (buf, buf_len, "%s %d:%02d%c", name, h12, mn, hr < 12 ? 'a' : 'p');
#else
    snprintf (buf, buf_len, "%s %02d:%02d", name, hr, mn);
#endif
}


/*********************************************************************************************
 *
 * projection helpers
 *
 */

/* project a degrees*100 location to raw or app screen coords for any map projection.
 * return whether the point is usable, ie, on the map and not near the far horizon of the single azimuthal.
 */
static bool natzProj (int16_t lat100, int16_t lng100, bool raw, uint8_t edge, SCoord &s)
{
    float lat = deg2rad (lat100*0.01F);
    float lng = deg2rad (lng100*0.01F);

    if (raw)
        ll2sRaw (lat, lng, s, edge);
    else
        ll2s (lat, lng, s, edge);

    if (s.x == 0)
        return (false);

    if (map_proj == MAPP_AZIM1) {
        // drop points near the antipodal horizon, as zones.cpp does
        int scale = raw ? tft.SCALESZ : 1;
        float dx = (float)s.x - scale*(map_b.x + map_b.w/2);
        float dy = (float)s.y - scale*(map_b.y + map_b.h/2);
        float hh = scale*map_b.h/2;
        if (dx*dx + dy*dy > 0.95F*hh*hh)
            return (false);
    }

    return (true);
}


/*********************************************************************************************
 *
 * zone lookup grid, used for the tint in every projection and for the info box
 *
 */

// grid cells per degree: a finer grid on the bigger screens
#define NATZ_CPD        (BUILD_W > 800 ? 20 : 10)

static uint8_t *natz_grid;                      // zone+1 per cell, 0 if none; row 0 is the northern edge
static int natz_grid_w, natz_grid_h;
static float natz_lat_n, natz_lng_w;            // north and west edges of the grid, degrees

/* sort floats, small n
 */
static void sortFloats (float *a, int n)
{
    for (int i = 1; i < n; i++) {
        float v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) {
            a[j+1] = a[j];
            j--;
        }
        a[j+1] = v;
    }
}

/* rasterize the zone rings into natz_grid, once, the first time it is needed.
 * each zone is filled by the even-odd rule so lakes and islands come out right.
 */
static void buildNATZGrid (void)
{
    if (natz_grid)
        return;

    // bounds
    int lat_min = 32767, lat_max = -32768, lng_min = 32767, lng_max = -32768, n_vert = 0;
    for (int r = 0; r < natz_n_rings; r++) {
        n_vert += natz_rings[r].n;
        for (int i = 0; i < natz_rings[r].n; i++) {
            const int16_t *p = &natz_fill_pts[2*(natz_rings[r].first + i)];
            if (p[0] < lat_min) lat_min = p[0];
            if (p[0] > lat_max) lat_max = p[0];
            if (p[1] < lng_min) lng_min = p[1];
            if (p[1] > lng_max) lng_max = p[1];
        }
    }
    const int cpd = NATZ_CPD;
    natz_lat_n = lat_max*0.01F + 0.1F;
    natz_lng_w = lng_min*0.01F - 0.1F;
    natz_grid_w = (int)ceilf ((lng_max*0.01F + 0.1F - natz_lng_w)*cpd);
    natz_grid_h = (int)ceilf ((natz_lat_n - (lat_min*0.01F - 0.1F))*cpd);

    uint8_t *grid = (uint8_t *) calloc ((size_t)natz_grid_w*natz_grid_h, 1);
    float *xs = (float *) malloc ((n_vert+1)*sizeof(float));
    if (!grid || !xs) {
        free (grid);
        free (xs);
        return;                                 // no memory: no tint and no zone lookup
    }

    for (int z = 0; z < NZ_N; z++) {
        for (int row = 0; row < natz_grid_h; row++) {

            // where this row's center line crosses any edge of any ring of this zone
            const float lat = natz_lat_n - (row + 0.5F)/cpd;
            int nx = 0;
            for (int r = 0; r < natz_n_rings; r++) {
                if (natz_rings[r].zone != z)
                    continue;
                const int n = natz_rings[r].n;
                const int16_t *pts = &natz_fill_pts[2*natz_rings[r].first];
                for (int i = 0; i < n; i++) {
                    const int16_t *p0 = &pts[2*i];
                    const int16_t *p1 = &pts[2*((i+1) % n)];
                    float la0 = p0[0]*0.01F, la1 = p1[0]*0.01F;
                    if ((la0 <= lat) != (la1 <= lat))
                        xs[nx++] = p0[1]*0.01F + (lat - la0)*(p1[1] - p0[1])*0.01F/(la1 - la0);
                }
            }
            sortFloats (xs, nx);

            // fill between pairs
            for (int i = 0; i + 1 < nx; i += 2) {
                int ca = (int) ceilf ((xs[i] - natz_lng_w)*cpd - 0.5F);
                int cb = (int) ceilf ((xs[i+1] - natz_lng_w)*cpd - 0.5F) - 1;
                if (ca < 0) ca = 0;
                if (cb >= natz_grid_w) cb = natz_grid_w - 1;
                for (int col = ca; col <= cb; col++)
                    grid[(size_t)row*natz_grid_w + col] = z + 1;
            }
        }
    }

    free (xs);
    natz_grid = grid;
}

/* return zone containing the given location, or -1 if none
 */
static int natzZoneAt (float lat_d, float lng_d)
{
    buildNATZGrid();
    if (!natz_grid)
        return (-1);

    while (lng_d >= 180)
        lng_d -= 360;
    while (lng_d < -180)
        lng_d += 360;

    int row = (int) floorf ((natz_lat_n - lat_d)*NATZ_CPD);
    int col = (int) floorf ((lng_d - natz_lng_w)*NATZ_CPD);
    if (row < 0 || row >= natz_grid_h || col < 0 || col >= natz_grid_w)
        return (-1);
    return ((int)natz_grid[(size_t)row*natz_grid_w + col] - 1);
}

/* public: find the time zone at ll, return whether there is one
 */
bool findNATZone (const LatLong &ll, int *zone)
{
    int z = natzZoneAt (ll.lat_d, ll.lng_d);
    if (z < 0)
        return (false);
    *zone = z;
    return (true);
}

/* public: get the abbreviation (EDT) and local time (3:20p) now in the given zone for the info box
 */
void getNATZTime (int zone, char *abbr, size_t abbr_len, char *tm, size_t tm_len)
{
    const char *name;
    int hr, mn;
    natzLocalTime ((NATZoneId)zone, nowWO(), &name, &hr, &mn);
    snprintf (abbr, abbr_len, "%s", name);
#if NATZ_12HOUR
    snprintf (tm, tm_len, "%d:%02d%c", hr % 12 == 0 ? 12 : hr % 12, mn, hr < 12 ? 'a' : 'p');
#else
    snprintf (tm, tm_len, "%02d:%02d", hr, mn);
#endif
}


/*********************************************************************************************
 *
 * tinted areas, all projections
 *
 */

/* mix the tint into the map pixel at app coords x,y, plus hatch stripes if the zone never changes clocks
 */
static void tintPixel (int x, int y, const uint8_t *rgb, bool hatch)
{
    uint8_t r, g, b;
    if (!tft.getPixelRaw (x, y, &r, &g, &b))
        return;

    const int a = NATZ_TINT;
    int rr = (r*(256-a) + rgb[0]*a) >> 8;
    int gg = (g*(256-a) + rgb[1]*a) >> 8;
    int bb = (b*(256-a) + rgb[2]*a) >> 8;

    if (hatch && ((x + y) % 6) < 2) {
        rr = rr*3/4;
        gg = gg*3/4;
        bb = bb*3/4;
    }

    tft.drawPixel (x, y, RGB565(rr, gg, bb));
}

/* tint the time zones into the map: each map pixel is turned back into a location and looked up.
 * only within clip if not NULL.
 * N.B. must be called after the base map pixels have been (re)painted and before anything else is drawn
 *   on top, and only once for the same pixels because tinting accumulates.
 */
void drawNATZFill (const SBox *clip)
{
    if (getRawPathWidth (GRID_CSPR) == 0)
        return;

    #if defined(_NATZ_TIME)                     // compile with -D_NATZ_TIME to log how long this takes
        struct timeval tv0;
        gettimeofday (&tv0, NULL);
    #endif

    // pixel rows and columns we may touch
    int px0 = map_b.x, px1 = map_b.x + map_b.w - 1;
    int py0 = map_b.y, py1 = map_b.y + map_b.h - 1;
    if (clip) {
        if ((int)clip->x > px0) px0 = clip->x;
        if ((int)(clip->x + clip->w - 1) < px1) px1 = clip->x + clip->w - 1;
        if ((int)clip->y > py0) py0 = clip->y;
        if ((int)(clip->y + clip->h - 1) < py1) py1 = clip->y + clip->h - 1;
    }

    for (int y = py0; y <= py1; y++) {
        for (int x = px0; x <= px1; x++) {
            LatLong ll;
            if (!s2ll ((uint16_t)x, (uint16_t)y, ll))
                continue;                       // off the map, eg, outside the azimuthal circle
            int z = natzZoneAt (ll.lat_d, ll.lng_d);
            if (z >= 0)
                tintPixel (x, y, natz_rules[z].rgb, natz_rules[z].dst_name == NULL);
        }
    }

    #if defined(_NATZ_TIME)
        struct timeval tv1;
        gettimeofday (&tv1, NULL);
        Serial.printf ("NATZ: fill took %ld us\n", (long)TVDELUS (tv0, tv1));
    #endif
}


/*********************************************************************************************
 *
 * borders and clock labels, all projections
 *
 */

/* draw lines between zones with different clocks. coasts and borders inside one zone are not drawn
 */
static void drawNATZBorders (int lw)
{
    for (int b = 0; b < natz_n_borders; b++) {
        const NATZLine &ln = natz_borders[b];
        SCoord sp = {0, 0};
        bool have_sp = false;
        for (int i = 0; i < ln.n; i++) {
            const int16_t *p = &natz_border_pts[2*(ln.first + i)];
            SCoord sc;
            bool ok = natzProj (p[0], p[1], true, lw, sc);
            if (ok && have_sp && segmentSpanOkRaw (sp, sc, lw))
                tft.drawLineRaw (sp.x, sp.y, sc.x, sc.y, lw, EARTH_GRIDC);
            sp = sc;
            have_sp = ok;
        }
    }
}

/* draw the clock labels, most important tier first. a tier 1 label that would land on one already drawn
 * is nudged up or down a little, tier 2 a little less; tier 3 that would collide is skipped. tier 2 labels appear at zoom 2
 * (or on a 1600x960 screen) and tier 3 at zoom 3 (or 2400x1440), so zoom in to see them all.
 */
static void drawNATZLabels (void)
{
    const time_t utc = nowWO();

    SBox placed[64];
    int n_placed = 0;

    // less crowded the more room there is: bigger screens and a zoomed Mercator map show more labels
    const int max_tier = (map_proj == MAPP_MERCATOR ? pan_zoom.zoom : 1) * (BUILD_W/800);

    for (int i = 0; i < natz_n_labels && n_placed < (int)NARRAY(placed); i++) {
        const NATZLabel &lb = natz_labels[i];
        if (lb.tier > max_tier)
            continue;

        SCoord s0;
        if (!natzProj (lb.lat, lb.lng, false, 1, s0) || !overMap (s0))
            continue;

        char tag[32];
        natzLabelText ((NATZoneId)lb.zone, utc, tag, sizeof(tag));

        // try the anchor first then, for tier 1 only, alternate below and above in one text-height steps
        SBox box0;
        setMapTagBox (tag, s0, 0, box0);
        const int step_px = box0.h;
        const int max_step = lb.tier == 1 ? NATZ_MAXNUDGE : (lb.tier == 2 ? 1 : 0);

        for (int step = 0; step <= 2*max_step; step++) {
            int dy = (step % 2 ? 1 : -1) * ((step + 1)/2) * step_px;
            SCoord s = s0;
            s.y += dy;
            if (!overMap (s))
                continue;

            SBox box;
            setMapTagBox (tag, s, 0, box);
            if (!overMap (box))
                continue;
            if (overViewBtn (SCoord{box.x, box.y}, 0)
                        || overViewBtn (SCoord{(uint16_t)(box.x+box.w), (uint16_t)(box.y+box.h)}, 0))
                continue;

            bool clash = false;
            for (int j = 0; j < n_placed && !clash; j++)
                clash = boxesOverlap (box, placed[j]);
            if (clash)
                continue;

            drawMapTag (tag, box);
            placed[n_placed++] = box;
            break;
        }
    }
}

/* draw the zone borders and live clock labels. call from the grid drawing step of every map repaint.
 */
void drawNATZones (void)
{
    int lw = getRawPathWidth (GRID_CSPR);
    if (lw == 0)
        return;

    drawNATZBorders (lw);
    drawNATZLabels();
}

/* outline the given zone, used to highlight the zone under the cursor like the CQ and ITU zones do
 */
void drawNATZHighlight (int zone)
{
    int lw = getRawPathWidth (GRID_CSPR);
    if (lw == 0)
        return;

    for (int r = 0; r < natz_n_rings; r++) {
        const NATZRing &ring = natz_rings[r];
        if (ring.zone != zone)
            continue;
        SCoord sp = {0, 0};
        bool have_sp = false;
        for (int i = 0; i <= ring.n; i++) {                     // <= to close the ring
            const int16_t *p = &natz_fill_pts[2*(ring.first + (i % ring.n))];
            SCoord sc;
            bool ok = natzProj (p[0], p[1], true, lw, sc);
            if (ok && have_sp && segmentSpanOkRaw (sp, sc, lw))
                tft.drawLineRaw (sp.x, sp.y, sc.x, sc.y, lw, EARTH_GRIDC00);
            sp = sc;
            have_sp = ok;
        }
    }
}
