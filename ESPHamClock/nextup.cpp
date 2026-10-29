/* Next Up pane: a ranked list of the contacts the operator would most like to make right now.
 *
 * spots come from whichever of the DX Cluster (which also carries WSJT-X decodes), HamAlert and On The Air
 * are active. each is scored by nextup_score.h: do I need it (from the ADIF log), can I reach it (from
 * evidence of DE being heard, and of the path being open) and is it fresh. the best is shown large; tapping
 * any row points the DX at the station and tunes the radio, same as tapping a row in the source panes.
 *
 * N.B. this pane only reads what the source modules already hold. those modules only collect data while their
 * own pane is in some pane's rotation set, so the footer shows which sources are actually feeding us.
 */

#include "HamClock.h"
#include "nextup_score.h"


// colors
#define NU_COLOR        RGB565(77,208,225)              // title cyan
#define NU_GREEN        RGB565(61,220,132)              // score >= NU_HI
#define NU_AMBER        RGB565(255,179,0)               // score >= NU_MID
#define NU_GRAY         RGB565(143,150,160)             // everything else
#define NU_CARD_BG      RGB565(8,36,42)                 // best-now card background

// thresholds
#define NU_HI           80
#define NU_MID          60

// limits
#define NU_MAXCAND      160                             // candidates kept per pass
#define NU_MAXRAW       700                             // raw spots remembered for corroboration
#define NU_MAXAGE_S     (30*60)                         // ignore spots older than this
#define NU_PSK_AGE_S    (15*60)                         // reports of DE considered this recent
#define NU_NEARBY_KM    3000.0F                         // "near" for corroborating spots
#define NU_MAXROWS      12                              // list rows, tall pane
#define NU_EARTH_KM     6371.0F

// layout, y down from box top. all text is FAST_FONT.
#define NU_CARD_Y       31
#define NU_CARD_H       31
#define NU_LIST_Y       (NU_CARD_Y + NU_CARD_H + 5)
#define NU_FOOT_H       14

// source bits
#define NUS_CLU         1
#define NUS_HA          2
#define NUS_OTA         4


typedef struct {
    DXSpot s;
    uint8_t srcs;                                       // NUS_* that reported it
    HamBandSetting band;
    float km, bear_deg;                                 // from DE
    char ref_org[8], ref_id[12];                        // On The Air program and reference if an OTA source named one
    NUResult r;
} NUCand;

static NUCand cands[NU_MAXCAND];
static int n_cands;
static const DXSpot *raws[NU_MAXRAW];                   // every spot seen this pass, for corroboration
static int n_raws;

// per-band reports of DE this pass
static int psk_count[HAMBAND_N];
static float psk_maxkm[HAMBAND_N];
static int psk_total;

// source health this pass
static bool src_clu, src_ha, src_ota, src_adif, src_psk;

// what was last drawn and where to tap
static DXSpot shown[NU_MAXROWS + 1];                    // [0] is the best-now card
static SBox hit[NU_MAXROWS + 1];
static int n_shown;
static uint32_t last_sig;


/* small cache of ADIF need lookups. onADIFList() scans the whole log and the prefix test even re-derives a
 * prefix for every logged QSO, so repeating it for every candidate every pass would be far too slow.
 * entries are valid only for the ADIF generation they were made under.
 */
typedef struct {
    char call[MAX_SPOTCALL_LEN];
    HamBandSetting band;
    uint32_t gen;
    NUInputs in;                                        // only the need fields are used
} NeedCache;
static NeedCache need_cache[NU_MAXCAND];
static int need_cache_next;


/* handy SBox maker, avoids narrowing in braced initializers
 */
static SBox mkBox (int x, int y, int w, int h)
{
    SBox b;
    b.x = x; b.y = y; b.w = w; b.h = h;
    return (b);
}

/* distance in km between two points
 */
static float nuKm (LatLong a, LatLong b)
{
    return (a.GSD(b) * NU_EARTH_KM);
}

/* fill the need fields of in for spot s, using the cache when possible
 */
static void fillNeed (NUInputs &in, const DXSpot &s, HamBandSetting band)
{
    in.have_log = src_adif;
    in.grid_known = s.tx_grid[0] != '\0';
    if (!in.have_log)
        return;

    // cached?
    uint32_t gen = getADIFGeneration();
    for (int i = 0; i < NU_MAXCAND; i++) {
        NeedCache &nc = need_cache[i];
        if (nc.gen == gen && nc.band == band && strcasecmp (nc.call, s.tx_call) == 0) {
            in.dxcc_worked = nc.in.dxcc_worked;
            in.dxcc_band_worked = nc.in.dxcc_band_worked;
            in.grid_worked = nc.in.grid_worked;
            in.prefix_worked = nc.in.prefix_worked;
            return;
        }
    }

    // cascade, stopping at the first "new" because later fields are not read then
    in.dxcc_worked = onADIFList (s, true, false, false, false);
    if (in.dxcc_worked) {
        in.dxcc_band_worked = onADIFList (s, true, false, false, true);
        if (in.dxcc_band_worked) {
            if (in.grid_known)
                in.grid_worked = onADIFList (s, false, true, false, false);
            else
                in.grid_worked = true;
            if (in.grid_worked)
                in.prefix_worked = onADIFList (s, false, false, true, false);
        }
    }

    // remember
    NeedCache &nc = need_cache[need_cache_next];
    need_cache_next = (need_cache_next + 1) % NU_MAXCAND;
    quietStrncpy (nc.call, s.tx_call, sizeof(nc.call));
    nc.band = band;
    nc.gen = gen;
    nc.in = in;
}

/* count other spots on the same band that show the path to c is open: spotter near DE, station near c.
 * WSJT-X decodes arrive with DE itself as the spotter so they count strongly.
 */
static int rxSupport (const NUCand &c)
{
    int n = 0;
    for (int i = 0; i < n_raws; i++) {
        const DXSpot &r = *raws[i];
        if (findHamBand (r.kHz) != c.band || strcasecmp (r.tx_call, c.s.tx_call) == 0)
            continue;
        if (nuKm (de_ll, r.rx_ll) > NU_NEARBY_KM)
            continue;
        if (nuKm (c.s.tx_ll, r.tx_ll) > NU_NEARBY_KM)
            continue;
        n++;
    }
    return (n);
}

/* tally reports of DE by band, if we have any
 */
static void gatherPSK (time_t now)
{
    memset (psk_count, 0, sizeof(psk_count));
    memset (psk_maxkm, 0, sizeof(psk_maxkm));
    psk_total = 0;

    const DXSpot *rp;
    int n_rep;
    src_psk = getPSKReportsOfDE (rp, n_rep);
    if (!src_psk)
        return;

    for (int i = 0; i < n_rep; i++) {
        if (now - rp[i].spotted > NU_PSK_AGE_S)
            continue;
        HamBandSetting b = findHamBand (rp[i].kHz);
        if (b == HAMBAND_NONE)
            continue;
        psk_count[b]++;
        psk_total++;
        float km = nuKm (de_ll, rp[i].rx_ll);
        if (km > psk_maxkm[b])
            psk_maxkm[b] = km;
    }
}

/* record the On The Air program and reference carried by an On The Air spot. these are kept in the candidate
 * itself because c.s can be replaced by a newer DX Cluster spot of the same station, which has no reference.
 * N.B. On The Air spots repurpose rx_call for the reference id and rx_grid for the program name.
 */
static void setCandRef (NUCand &c, const DXSpot &s)
{
    if (s.rx_call[0] && s.rx_grid[0]) {
        quietStrncpy (c.ref_org, s.rx_grid, sizeof(c.ref_org));
        quietStrncpy (c.ref_id, s.rx_call, sizeof(c.ref_id));
    }
}

/* add one spot to the candidate list, merging with an existing one for the same call and band
 */
static void addCand (const DXSpot &s, uint8_t src, time_t now)
{
    if (s.tx_call[0] == '\0' || s.kHz <= 0)
        return;
    if (strcasecmp (s.tx_call, getCallsign()) == 0)
        return;
    HamBandSetting band = findHamBand (s.kHz);
    if (band == HAMBAND_NONE)
        return;
    if (now - s.spotted > NU_MAXAGE_S)
        return;

    for (int i = 0; i < n_cands; i++) {
        NUCand &c = cands[i];
        if (c.band == band && strcasecmp (c.s.tx_call, s.tx_call) == 0) {
            c.srcs |= src;
            if (s.spotted > c.s.spotted)
                c.s = s;
            if (src == NUS_OTA)
                setCandRef (c, s);
            return;
        }
    }

    if (n_cands >= NU_MAXCAND)
        return;
    NUCand &c = cands[n_cands++];
    c.s = s;
    c.srcs = src;
    c.band = band;
    c.ref_org[0] = c.ref_id[0] = '\0';
    if (src == NUS_OTA)
        setCandRef (c, s);
}

/* add all spots from one source list
 */
static void addList (const DXSpot *sp, int n, uint8_t src, time_t now)
{
    for (int i = 0; i < n; i++) {
        if (n_raws < NU_MAXRAW && now - sp[i].spotted <= NU_MAXAGE_S)
            raws[n_raws++] = &sp[i];
        addCand (sp[i], src, now);
    }
}

/* qsort: best score first, ties to the newest
 */
static int qsNU (const void *p1, const void *p2)
{
    const NUCand *a = (const NUCand *)p1;
    const NUCand *b = (const NUCand *)p2;
    if (a->r.score != b->r.score)
        return (b->r.score - a->r.score);
    return ((int)(b->s.spotted - a->s.spotted));
}

/* collect, score and rank everything. result is cands[0..n_cands-1], best first.
 */
static void rankAll (time_t now)
{
    n_cands = 0;
    n_raws = 0;

    DXSpot *sp;
    uint8_t n8;
    int n;

    src_clu = getDXClusterSpots (&sp, &n8);
    if (src_clu)
        addList (sp, n8, NUS_CLU, now);
    src_ha = getHamAlertSpots (&sp, &n);
    if (src_ha)
        addList (sp, n, NUS_HA, now);
    src_ota = getOnTheAirSpots (&sp, &n8);
    if (src_ota)
        addList (sp, n8, NUS_OTA, now);
    src_adif = getADIFCount() > 0;
    gatherPSK (now);

    for (int i = 0; i < n_cands; i++) {
        NUCand &c = cands[i];

        // geometry
        float dist_rad, bear_rad;
        propDEPath (false, c.s.tx_ll, &dist_rad, &bear_rad);
        c.km = dist_rad * NU_EARTH_KM;
        c.bear_deg = rad2deg (bear_rad);

        // inputs
        NUInputs in;
        memset (&in, 0, sizeof(in));
        fillNeed (in, c.s, c.band);
        in.have_tx_obs = psk_total > 0;
        in.tx_on_band = psk_count[c.band];
        in.tx_max_km = psk_maxkm[c.band];
        in.spot_km = c.km;
        in.have_rx_obs = n_raws > 0;
        in.age_s = now > c.s.spotted ? (int)(now - c.s.spotted) : 0;

        // reference need, but only if the log holds references for this program else we could not tell
        in.has_ref = c.ref_id[0] && orgHasRefsWorked (c.ref_org);
        in.ref_worked = in.has_ref && isRefWorked (c.ref_org, c.ref_id);

        // corroboration is only worth computing for spots we might actually want
        NUReason why;
        if (nuNeed (in, why) > 0.1F)
            in.rx_support = rxSupport (c);

        c.r = nuScore (in);
    }

    qsort (cands, n_cands, sizeof(NUCand), qsNU);

    // keep only the best candidate for each DXCC entity: three stations from the same new country are
    // one opportunity, not three, and would otherwise crowd out everything else in a small pane.
    // list is sorted so the first seen for an entity is its best.
    int n_keep = 0;
    for (int i = 0; i < n_cands; i++) {
        bool dup = false;
        for (int j = 0; j < n_keep && !dup; j++)
            dup = cands[j].s.tx_dxcc == cands[i].s.tx_dxcc;
        if (!dup) {
            if (n_keep != i)
                cands[n_keep] = cands[i];
            n_keep++;
        }
    }
    n_cands = n_keep;
}

/* color for a score
 */
static uint16_t scoreColor (int score)
{
    return (score >= NU_HI ? NU_GREEN : score >= NU_MID ? NU_AMBER : NU_GRAY);
}

/* short phrase for why the spot is wanted
 */
static const char *reasonStr (NUReason r, const char *ref_org)
{
    switch (r) {
    case NU_REF:
        return (!strcasecmp (ref_org, "POTA") ? "NEW PARK" : !strcasecmp (ref_org, "SOTA") ? "new summit"
                                                                                        : "new ref");
    case NU_DXCC:    return ("NEW DXCC");
    case NU_BAND:    return ("new band");
    case NU_GRID:    return ("new grid");
    case NU_PREFIX:  return ("new prefix");
    case NU_WORKED:  return ("worked");
    case NU_UNKNOWN: return ("need ?");
    }
    return ("");
}

/* band label such as "17m"; the band table stores "23cm" whole and bare numbers otherwise
 */
static void bandStr (HamBandSetting b, char *buf, size_t n)
{
    const char *name = findBandName (b);
    if (strchr (name, 'c'))
        snprintf (buf, n, "%s", name);
    else
        snprintf (buf, n, "%sm", name);
}

/* coarse age class of a spot, so the pane can show how stale it is and redraw when that changes:
 * 0 just spotted (< 90 s), 1 under 5 min, 2 under 15 min, 3 older.
 */
static int ageBucket (time_t now, time_t spotted)
{
    long age = now > spotted ? (long)(now - spotted) : 0;
    return (age < 90 ? 0 : age < 5*60 ? 1 : age < 15*60 ? 2 : 3);
}

/* call sign color for an age bucket: brightest when brand new, fading as the spot goes stale
 */
static uint16_t ageColor (int bucket)
{
    switch (bucket) {
    case 0:  return (NU_GREEN);
    case 1:  return (RA8875_WHITE);
    case 2:  return (RGB565(190,196,204));
    default: return (NU_GRAY);
    }
}

/* mix a string into a running FNV-1a hash
 */
static uint32_t hashStr (uint32_t h, const char *s)
{
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return (h);
}

/* draw the pane
 */
static void drawNextUp (const SBox &box, time_t now)
{
    prepPlotBox (box);

    // title
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (NU_COLOR);
    const char *title = "Next Up";
    uint16_t tw = getTextWidth (title);
    tft.setCursor (box.x + (box.w-tw)/2, box.y + PANETITLE_H);
    tft.print (title);

    selectFontStyle (LIGHT_FONT, FAST_FONT);
    n_shown = 0;
    char line[60], bs[8];

    // anything to show?
    if (n_cands == 0 || cands[0].r.score <= 0) {
        tft.setTextColor (RA8875_WHITE);
        const char *msg = (src_clu || src_ha || src_ota) ? "Nothing needed right now"
                                                         : "No spot source active";
        tw = getTextWidth (msg);
        tft.setCursor (box.x + (box.w-tw)/2, box.y + NU_CARD_Y + 8);
        tft.print (msg);
        if (!(src_clu || src_ha || src_ota)) {
            const char *m2 = "Add DX Cluster, HamAlert";
            tw = getTextWidth (m2);
            tft.setCursor (box.x + (box.w-tw)/2, box.y + NU_CARD_Y + 24);
            tft.print (m2);
            const char *m3 = "or On The Air to a pane";
            tw = getTextWidth (m3);
            tft.setCursor (box.x + (box.w-tw)/2, box.y + NU_CARD_Y + 38);
            tft.print (m3);
        }
    } else {

        // best-now card
        const NUCand &b = cands[0];
        tft.fillRect (box.x+2, box.y+NU_CARD_Y, box.w-4, NU_CARD_H, NU_CARD_BG);
        tft.drawRect (box.x+2, box.y+NU_CARD_Y, box.w-4, NU_CARD_H, NU_COLOR);
        bandStr (b.band, bs, sizeof(bs));
        snprintf (line, sizeof(line), "%s %s %s", b.s.tx_call, bs, b.s.mode);
        tft.setTextColor (ageColor (ageBucket (now, b.s.spotted)));
        tft.setCursor (box.x+5, box.y+NU_CARD_Y+3);
        tft.print (line);
        snprintf (line, sizeof(line), "%d", b.r.score);
        tw = getTextWidth (line);
        tft.setTextColor (scoreColor (b.r.score));
        tft.setCursor (box.x+box.w-5-tw, box.y+NU_CARD_Y+3);
        tft.print (line);

        bool km = showDistKm();
        if (!src_adif && b.r.reason != NU_REF) {
            // scores mean little without a log, so say how to fix that rather than imply need was judged
            snprintf (line, sizeof(line), "no log: /adif.html");
            tft.setTextColor (NU_AMBER);
        } else {
            snprintf (line, sizeof(line), "%s az%.0f %.0f%s", b.r.unconfirmed ? "unconfirmed" : reasonStr (b.r.reason, b.ref_org),
                        b.bear_deg, km ? b.km : b.km*0.621371F, km ? "km" : "mi");
            tft.setTextColor ((b.r.reason == NU_DXCC || b.r.reason == NU_REF) && !b.r.unconfirmed ? NU_GREEN
                                : b.r.unconfirmed ? NU_AMBER : NU_COLOR);
        }
        tft.setCursor (box.x+5, box.y+NU_CARD_Y+17);
        tft.print (line);

        shown[0] = b.s;
        hit[0] = mkBox (box.x, box.y+NU_CARD_Y, box.w, NU_CARD_H);
        n_shown = 1;

        // ranked rows below the card
        int n_rows = (box.h - NU_LIST_Y - NU_FOOT_H) / LISTING_DY;
        if (n_rows > NU_MAXROWS)
            n_rows = NU_MAXROWS;
        for (int i = 1, row = 0; i < n_cands && row < n_rows; i++, row++) {
            const NUCand &c = cands[i];
            if (c.r.score <= 0)
                break;
            uint16_t y = box.y + NU_LIST_Y + row*LISTING_DY;
            tft.drawLine (box.x+2, y-3, box.x+box.w-3, y-3, RGB565(30,36,43));
            bandStr (c.band, bs, sizeof(bs));
            snprintf (line, sizeof(line), "%-7.7s %-4s", c.s.tx_call, bs);
            tft.setTextColor (ageColor (ageBucket (now, c.s.spotted)));
            tft.setCursor (box.x+3, y);
            tft.print (line);

            // source tags
            snprintf (line, sizeof(line), "%s%s%s%s", (c.srcs & NUS_CLU) ? "C" : "", (c.srcs & NUS_HA) ? "H" : "",
                                        (c.srcs & NUS_OTA) ? "O" : "", c.r.reason == NU_REF ? "*" : "");
            tft.setTextColor (c.r.unconfirmed ? NU_AMBER : c.r.reason == NU_REF ? NU_GREEN : NU_GRAY);
            tft.setCursor (box.x+88, y);
            tft.print (c.r.unconfirmed ? "?" : line);

            snprintf (line, sizeof(line), "%d", c.r.score);
            tw = getTextWidth (line);
            tft.setTextColor (scoreColor (c.r.score));
            tft.setCursor (box.x+box.w-4-tw, y);
            tft.print (line);

            shown[n_shown] = c.s;
            hit[n_shown] = mkBox (box.x, y-LISTING_OS, box.w, LISTING_DY);
            n_shown++;
        }
    }

    // footer: which sources are feeding us
    struct { const char *tag; bool on; } srcs[] = {
        {"CLU", src_clu}, {"HA", src_ha}, {"OTA", src_ota}, {"ADIF", src_adif}, {"PSK", src_psk},
    };
    uint16_t fx = box.x + 3;
    const uint16_t fy = box.y + box.h - NU_FOOT_H + 2;
    for (size_t i = 0; i < NARRAY(srcs); i++) {
        tft.setTextColor (srcs[i].on ? NU_GREEN : RGB565(90,96,104));
        tft.setCursor (fx, fy);
        tft.print (srcs[i].tag);
        fx += getTextWidth (srcs[i].tag) + 6;
    }

}

/* called often while the pane is visible, fresh when newly so. always ok: we do no I/O of our own.
 * redraws only when what would be shown has changed so the pane does not flicker.
 */
bool updateNextUp (const SBox &box, bool fresh)
{
    time_t now = myNow();
    freshenADIFFile();                                  // load the log if set, whether or not ADIF is in a pane
    rankAll (now);

    // signature of everything that would be drawn
    uint32_t sig = 2166136261u;
    char buf[80];
    int n_sig = box.h > 200 ? NU_MAXROWS + 1 : 5;
    for (int i = 0; i < n_cands && i < n_sig; i++) {
        snprintf (buf, sizeof(buf), "%.14s%d%d%d%d%d", cands[i].s.tx_call, cands[i].band, cands[i].r.score,
                        cands[i].srcs, (int)cands[i].r.reason, ageBucket (now, cands[i].s.spotted));
        sig = hashStr (sig, buf);
    }
    snprintf (buf, sizeof(buf), "%d%d%d%d%d%d", src_clu, src_ha, src_ota, src_adif, src_psk, showDistKm());
    sig = hashStr (sig, buf);

    if (fresh || sig != last_sig) {
        drawNextUp (box, now);
        last_sig = sig;
    }

    return (true);
}

/* act on a tap in box. return whether it was ours.
 */
bool checkNextUpTouch (const SCoord &s, const SBox &box)
{
    (void) box;
    for (int i = 0; i < n_shown; i++) {
        if (inBox (s, hit[i])) {
            newDX (shown[i].tx_ll, NULL, shown[i].tx_call);
            setRadioSpot (shown[i].kHz, shown[i].mode);
            return (true);
        }
    }
    return (false);
}
