/* POTA Scheduled Activations pane.
 *
 * Reads /ONTA/pota_scheduled.txt, produced server-side by OHB's
 * gen_pota_scheduled.pl: a rolling ~7-day window of upcoming POTA
 * activations, one line per activation:
 *      #call,ref,startEpoch,endEpoch,freqs
 *      N8XQM,US-3517,1789128000,1789340400,40m-6m
 * already sorted nearest-to-furthest by startEpoch. Deliberately does
 * NOT carry park name or state -- that's looked up locally here from
 * onta_parks.txt (ref -> 2-letter state/province), the same side file
 * ontheair.cpp's ONTA pane already maintains. This is an independent,
 * lightweight copy of that lookup (not shared code with ontheair.cpp)
 * so the two panes stay fully decoupled.
 *
 * Visually modeled on the ONTA pane (ontheair.cpp) for coloring: the park
 * reference field gets the same green background ontheair.cpp uses for POTA's
 * org marker color, and the 2-letter state is shown right after it, same as
 * ONTA. Row STRUCTURE is modeled on launches.cpp instead: two lines per
 * entry (call/ref/state on top, from-to range + freq below) at a fixed row
 * height, with a divider between entries -- makes each entry easier to read
 * than cramming everything onto one line, same rationale as launches.cpp's
 * name+info split.
 */

#include "HamClock.h"

#include <unordered_map>
#include <string>


// data source
static const char potasched_page[] = "/ONTA/pota_scheduled.txt";
static const char potasched_file[] = "pota_scheduled.txt";
#define POTASCHED_MAXAGE   900          // matches server cron cadence (15 min); secs
#define POTASCHED_MINSIZ   10           // min acceptable size is just the header line

// park/summit reference -> 2-letter state/province, same side file ontheair.cpp's ONTA
// pane already maintains (see gen_onta.pl / gen_xonta.pl server-side). independent
// lightweight copy here rather than shared code with ontheair.cpp -- see file header.
static const char potasched_parks_page[] = "/ONTA/onta_parks.txt";
static const char potasched_parks_file[] = "onta_parks.txt";
#define POTASCHED_PARKS_MAXAGE POTASCHED_MAXAGE
static std::unordered_map<std::string, std::string> potasched_park_states;

// colors -- POTASCHED_COLOR matches ONTA_COLOR (ontheair.cpp) for title/chrome;
// POTASCHED_PARKCOLOR matches ontheair.cpp's own POTA org marker color exactly, since
// this pane is entirely POTA data and the user asked for the reference field to read
// the same way ONTA's POTA entries already do.
#define POTASCHED_COLOR      RGB565(150,250,255)        // cyan -- title/subtitle/chrome
#define POTASCHED_PARKCOLOR  RGB565(60,200,90)           // green -- matches ONTA's POTA color
#define POTASCHED_NOWCOLOR   RA8875_YELLOW               // left-edge marker: in progress now
#define POTASCHED_INFOCOLOR  BRGRAY                       // row 2 (from/to/freq) text color

// row layout: two lines per entry, modeled on launches.cpp's LaunchEntry rows --
// fixed PS_ROW_H per entry, a divider between entries, row 1 / row 2 offsets from
// the row top. Row 1 is call + ref (colored, matching ONTA) + state; row 2 is
// "from - to   freq" as one composed, width-truncated string (same technique
// launches.cpp uses for its info line: quietStrncpy into a scratch buffer, then
// maxStringW to fit, one setCursor+print). Full untruncated detail is always
// available via tap-for-tooltip (checkPotaSchedTouch).
#define PS_ROW_H        24              // total height of one 2-line entry (px)
#define PS_NAME_DY      0               // row 1 offset from row top (px)
#define PS_INFO_DY      12              // row 2 offset from row top (px)
#define PS_DIV_COLOR    POTASCHED_COLOR // divider line between entries
#define PS_TITLE_RSV    28              // reserve at right so title clears the scroll-arrow
                                         // control, matches launches.cpp's LAUNCH_TITLE_RSV

// row 1 only needs char-grid math for the colored ref badge; row 2 is a single
// free-width string like launches.cpp's info line, no per-char budget needed
#define PS_CHAR_W       6               // FAST_FONT fixed width, px/char
#define PS_CALL_LEN     8
#define PS_REF_LEN      9

typedef struct {
    char call[13];              // HamClock rejects callsigns over 12 chars
    char ref[17];                // generous; longest real POTA refs are ~10 chars
    time_t start_t;
    time_t end_t;
    char freqs[40];
} PotaSchedEntry;

static PotaSchedEntry *potasched_entries;       // malloced array, count in ps_ss.n_data
static ScrollState ps_ss;


/* (re)load the park reference -> 2-letter state/province lookup, if due.
 * cheap and safe to call every time retrievePotaSched() runs: openCachedFile()
 * enforces POTASCHED_PARKS_MAXAGE itself, so this is a no-op download-wise most
 * of the time. a missing/failed/empty file is not an error -- rows just fall
 * back to showing the 2-letter country code parsed out of the reference's own
 * prefix (eg "US" from "US-3517"), same fallback philosophy as ontheair.cpp.
 */
static void retrievePotaSchedParkStates (void)
{
    FILE *fp = openCachedFile (potasched_parks_file, potasched_parks_page,
                                POTASCHED_PARKS_MAXAGE, 0);
    if (!fp)
        return;

    potasched_park_states.clear();

    char line[50];
    while (fgets (line, sizeof(line), fp)) {
        chompString (line);
        if (line[0] == '#' || line[0] == '\0')
            continue;
        char ref[20], state[8];                  // N.B. match sscanf fields
        if (sscanf (line, "%19[^,],%7s", ref, state) == 2)
            potasched_park_states[ref] = state;
    }

    fclose (fp);

    Serial.printf ("POTASCHED: read %d park states\n", (int)potasched_park_states.size());
}

/* fill out[3] with the 2-letter state/province for ref if cached, else fall back to
 * the 2-letter country code parsed directly out of the reference's own prefix.
 * Same fallback logic as ontheair.cpp's ontaStateOrCountry() -- kept as an
 * independent copy here rather than shared code, see file header.
 */
static void potaschedStateOrCountry (const char *ref, char out[3])
{
    auto it = potasched_park_states.find (ref);
    if (it != potasched_park_states.end() && it->second.length() >= 2) {
        out[0] = it->second[0];
        out[1] = it->second[1];
    } else {
        const char *dash = strchr (ref, '-');
        size_t n = dash ? (size_t)(dash - ref) : strlen(ref);
        out[0] = n >= 1 ? ref[0] : '?';
        out[1] = n >= 2 ? ref[1] : '?';
    }
    out[2] = '\0';
}

/* format the start/end pair as one range string, UTC -- same "collapse to one line
 * if same UTC day" logic as contests.cpp's formatTimeLine(), now feasible because
 * row 2 has the pane's full width to work with (row 1 no longer carries times).
 * Uses an actual date (month + day), not just a weekday name -- "Fri"/"Sat"/"Sun"
 * alone is ambiguous across a rolling multi-week window (which Friday?).
 */
static void formatPotaSchedRange (time_t t1, time_t t2, char *str, size_t str_l)
{
    struct tm tm1 = *gmtime (&t1);
    struct tm tm2 = *gmtime (&t2);

    if (tm1.tm_yday == tm2.tm_yday && tm1.tm_year == tm2.tm_year) {
        // starts and ends same UTC day -- show once with a time range
        snprintf (str, str_l, "%s%d %02d:%02d-%02d:%02dZ", monthShortStr (tm1.tm_mon+1), tm1.tm_mday,
                  tm1.tm_hour, tm1.tm_min, tm2.tm_hour, tm2.tm_min);
    } else {
        // different days -- show each
        char d1[10], d2[10];
        strcpy (d1, monthShortStr (tm1.tm_mon+1));
        strcpy (d2, monthShortStr (tm2.tm_mon+1));       // N.B. same static buffer as d1
        snprintf (str, str_l, "%s%d %02d:%02d - %s%d %02d:%02dZ", d1, tm1.tm_mday, tm1.tm_hour, tm1.tm_min,
                  d2, tm2.tm_mday, tm2.tm_hour, tm2.tm_min);
    }
}

/* remove entries whose end_t is already in the past -- keeps the pane's "next 7
 * days" promise honest between the server's own ~15-minute regeneration cycles.
 * return whether anything was removed (caller should redraw if so).
 */
static bool checkActivePotaSched (void)
{
    bool any_past = false;
    time_t now = myNow();

    for (int i = 0; i < ps_ss.n_data; i++) {
        PotaSchedEntry *ep = &potasched_entries[i];
        if (ep->end_t <= now) {
            memmove (ep, ep+1, (--ps_ss.n_data - i) * sizeof(PotaSchedEntry));
            i -= 1;                             // examine new [i] again next loop
            any_past = true;
        }
    }

    if (any_past)
        ps_ss.scrollToNewest();

    return (any_past);
}

/* download and parse pota_scheduled.txt into potasched_entries[].
 * return whether io ok (even if the resulting list is empty).
 */
static bool retrievePotaSched (const SBox &box)
{
    // refresh the state-lookup side file first, same cadence, same cheap no-op
    // behavior as ontheair.cpp -- see retrievePotaSchedParkStates() header comment
    retrievePotaSchedParkStates();

    FILE *fp = openCachedFile (potasched_file, potasched_page, POTASCHED_MAXAGE, POTASCHED_MINSIZ);
    if (!fp)
        return (false);

    // look alive
    updateClocks(false);

    // reset
    free (potasched_entries);
    potasched_entries = NULL;
    ps_ss.init ((box.h - LISTING_Y0)/PS_ROW_H, 0, 0, ScrollState::DIR_TOPDOWN);

    char line[100];

    // header line: "#call,ref,startEpoch,endEpoch,freqs" -- just skip it, no credit
    // text to capture the way contests.cpp's contests311.txt has one
    if (!fgets (line, sizeof(line), fp)) {
        Serial.printf ("POTASCHED: %s empty\n", potasched_file);
        fclose (fp);
        return (false);
    }

    time_t now = myNow();

    while (fgets (line, sizeof(line), fp)) {
        chompString (line);
        if (line[0] == '\0')
            continue;

        char call[30], ref[30], freqs[60];
        long start_l, end_l;
        int n = sscanf (line, "%29[^,],%29[^,],%ld,%ld,%59[^\n]",
                         call, ref, &start_l, &end_l, freqs);
        if (n < 4) {
            // freqs is allowed to be empty (trailing comma with nothing after) --
            // sscanf's %s-style conversion won't match zero chars, so n==4 is a
            // valid "no freqs" row, not an error. n<4 means a genuinely malformed
            // line -- skip it rather than risk garbage in the pane.
            Serial.printf ("POTASCHED: bad line: %s\n", line);
            continue;
        }
        if (n == 4)
            freqs[0] = '\0';

        // already-past by the time we're parsing it (server regenerates every
        // ~15 min, this file could be nearly that stale) -- skip rather than
        // show something that's already over
        if ((time_t)end_l <= now)
            continue;

        potasched_entries = (PotaSchedEntry *) realloc (potasched_entries,
                                                          (ps_ss.n_data+1) * sizeof(PotaSchedEntry));
        if (!potasched_entries)
            fatalError ("No memory for %d POTA scheduled activations", ps_ss.n_data+1);

        PotaSchedEntry &pe = potasched_entries[ps_ss.n_data++];
        quietStrncpy (pe.call, call, sizeof(pe.call));
        quietStrncpy (pe.ref, ref, sizeof(pe.ref));
        pe.start_t = (time_t) start_l;
        pe.end_t   = (time_t) end_l;
        quietStrncpy (pe.freqs, freqs, sizeof(pe.freqs));
    }

    fclose (fp);

    // Reverse to descending (furthest-future at index 0, soonest at the end) --
    // ScrollState with DIR_TOPDOWN expects "newest" (== highest index) at the
    // TOP of the screen (see scrollstate.cpp's own header diagram), and its
    // scrollToNewest() always points top_vis at n_data-1. The server sends this
    // file ascending (soonest first), which is the opposite of what ScrollState
    // wants here -- without this reversal, the pane showed furthest-future
    // activations at the top and in-progress/soonest ones at the bottom, which
    // is backwards. Same convention contests.cpp uses (see its qsContestStart()
    // comment: "puts the first [smallest time] contest ... at the end of the
    // array, as expected by ScrollState") -- we reverse in place here instead of
    // qsort'ing since the server already hands us a sorted list, just in the
    // wrong direction for this widget.
    for (int a = 0, b = ps_ss.n_data-1; a < b; a++, b--) {
        PotaSchedEntry tmp = potasched_entries[a];
        potasched_entries[a] = potasched_entries[b];
        potasched_entries[b] = tmp;
    }

    Serial.printf ("POTASCHED: found %d activations in %s\n", ps_ss.n_data, potasched_file);
    return (true);
}

/* draw potasched_entries[] in the given pane box, from scratch
 */
static void drawPotaSchedPane (const SBox &box)
{
    prepPlotBox (box);

    // title -- centered, but kept clear of the scroll-arrow control in the
    // top-right corner (same problem/fix as launches.cpp's drawLaunchesPane:
    // that control always erases a band at the right edge, even when the
    // arrows are inactive, so a full-width centered title can end up with its
    // last letter(s) covered -- this was literally happening to the "d" in
    // "Sked"). Shift left to clear it if a straight centered position would
    // reach under the reserved band.
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (POTASCHED_COLOR);
    static const char *title = "POTA Sked";
    uint16_t tw = getTextWidth (title);
    uint16_t avail_r = box.w > PS_TITLE_RSV ? box.w - PS_TITLE_RSV : box.w;    // right limit
    uint16_t tx = box.w > tw ? (box.w - tw)/2 : 2;                             // ideal centered x
    if (tx + tw > avail_r)                                                    // would hit arrows
        tx = avail_r > tw ? avail_r - tw : 2;                                 // shift left to clear
    tft.setCursor (box.x + tx, box.y + PANETITLE_H);
    tft.print (title);

    // subtitle: count
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    tft.setTextColor (POTASCHED_COLOR);
    char sub[30];
    snprintf (sub, sizeof(sub), "%d upcoming", ps_ss.n_data);
    uint16_t sw = getTextWidth (sub);
    tft.setCursor (box.x + (box.w-sw)/2, box.y + SUBTITLE_Y0);
    tft.print (sub);

    // listing area, cleared
    uint16_t x0 = box.x + 1;
    uint16_t y0 = box.y + LISTING_Y0;
    tft.fillRect (box.x+1, y0-LISTING_OS, box.w-2, box.h - (LISTING_Y0-LISTING_OS+1), RA8875_BLACK);
    selectFontStyle (LIGHT_FONT, FAST_FONT);

    time_t now = myNow();

    int min_i, max_i;
    if (ps_ss.getVisDataIndices (min_i, max_i) > 0) {
        for (int i = min_i; i <= max_i; i++) {
            const PotaSchedEntry &pe = potasched_entries[i];
            uint16_t row_y = y0 + ps_ss.getDisplayRow(i) * PS_ROW_H;

            // left-edge marker, full row height, if this activation is already
            // underway -- deliberately NOT a full-row fill (would visually
            // collide with the ref field's own green background); just a thin
            // bar, unambiguous either way. See conversation: this is what the
            // yellow bar means.
            if (pe.start_t <= now && now < pe.end_t)
                tft.fillRect (box.x, row_y-2, 2, PS_ROW_H-1, POTASCHED_NOWCOLOR);

            // ---- Row 1: call + ref (colored) + state ----
            uint16_t x = x0;
            uint16_t box_right = box.x + box.w - 2;    // don't draw past here -- matters a
                                                        // lot now that this pane is also
                                                        // eligible for PANE_0's narrow 139px
                                                        // "Data Pane" width (PANE_0_CH_MASK)

            tft.setTextColor (RA8875_WHITE);
            tft.setCursor (x, row_y + PS_NAME_DY);
            tft.printf ("%-*.*s", PS_CALL_LEN, PS_CALL_LEN, pe.call);
            x += (PS_CALL_LEN+1) * PS_CHAR_W;

            if (x < box_right) {
                tft.fillRect (x, row_y + PS_NAME_DY-2, PS_REF_LEN*PS_CHAR_W, LISTING_DY-2,
                              POTASCHED_PARKCOLOR);
                tft.setTextColor (getGoodTextColor (POTASCHED_PARKCOLOR));
                tft.setCursor (x, row_y + PS_NAME_DY);
                tft.printf ("%-*.*s", PS_REF_LEN, PS_REF_LEN, pe.ref);
            }
            x += (PS_REF_LEN+1) * PS_CHAR_W;

            if (x < box_right) {
                char st[3];
                potaschedStateOrCountry (pe.ref, st);
                tft.setTextColor (RA8875_WHITE);
                tft.setCursor (x, row_y + PS_NAME_DY);
                tft.printf ("%2.2s", st);
            }

            // ---- Row 2: from - to, one width-truncated string. Freq/bands is
            // NOT shown here -- doesn't fit alongside the date+time range on a
            // single row; see the tap-for-tooltip instead ----
            char range[40];
            formatPotaSchedRange (pe.start_t, pe.end_t, range, sizeof(range));
            maxStringW (range, box.w - 4);

            tft.setTextColor (POTASCHED_INFOCOLOR);
            tft.setCursor (x0, row_y + PS_INFO_DY);
            tft.print (range);

            // ---- Divider ----
            tft.drawLine (box.x + 1, row_y + PS_ROW_H - 2, box.x + box.w - 2, row_y + PS_ROW_H - 2,
                          1, PS_DIV_COLOR);
        }
    } else {
        selectFontStyle (LIGHT_FONT, FAST_FONT);
        tft.setTextColor (RA8875_WHITE);
        const char *msg = "No activations in next 7 days";
        uint16_t mw = getTextWidth (msg);
        tft.setCursor (box.x + (box.w-mw)/2, y0 + PS_ROW_H);
        tft.print (msg);
    }

    ps_ss.drawScrollUpControl (box, POTASCHED_COLOR, POTASCHED_COLOR);
    ps_ss.drawScrollDownControl (box, POTASCHED_COLOR, POTASCHED_COLOR);
}

static void scrollPotaSchedUp (const SBox &box)
{
    if (ps_ss.okToScrollUp()) {
        ps_ss.scrollUp();
        drawPotaSchedPane (box);
    }
}

static void scrollPotaSchedDown (const SBox &box)
{
    if (ps_ss.okToScrollDown()) {
        ps_ss.scrollDown();
        drawPotaSchedPane (box);
    }
}

/* collect POTA scheduled-activation info and show in the given pane box.
 */
bool updatePotaSched (const SBox &box, bool fresh)
{
    // retrieve at most every POTASCHED_MAXAGE, staggered by a random offset so
    // many HamClocks don't all hit the OHB server on the same tick -- same
    // load-spreading motivation as contests.cpp's random-minute-past-the-hour
    // trick, just expressed as a plain elapsed-time gate instead of an hour/
    // minute-of-day coupling (simpler, and this pane's refresh cadence is
    // itself already a variable -- POTASCHED_MAXAGE -- not a fixed "once an
    // hour", so there's no natural "minute" to anchor to the way contests.cpp
    // has).
    static time_t next_retrieve_t;
    static bool jittered;
    if (!jittered) {
        next_retrieve_t = myNow() + random (60);
        jittered = true;
    }

    bool ok = true;

    if (fresh || myNow() >= next_retrieve_t) {

        next_retrieve_t = myNow() + POTASCHED_MAXAGE;

        ok = retrievePotaSched (box);
        if (ok) {
            ps_ss.scrollToNewest();
            fresh = true;
        }
    }

    if (ok) {

        if (checkActivePotaSched() || fresh)
            drawPotaSchedPane (box);

    } else {

        plotMessage (box, POTASCHED_COLOR, "POTA Sked error");
    }

    return (ok);
}

/* return true if user is interacting with this pane, false if wants to change pane.
 * N.B. we assume s is within box
 */
bool checkPotaSchedTouch (const SCoord &s, const SBox &box)
{
    if (s.y < box.y + PANETITLE_H) {

        if (ps_ss.checkScrollUpTouch (s, box)) {
            scrollPotaSchedUp (box);
            return (true);
        }
        if (ps_ss.checkScrollDownTouch (s, box)) {
            scrollPotaSchedDown (box);
            return (true);
        }

        // else tapping title leaves this pane, same as every other simple pane
        return (false);
    }

    // tapped a row: show freq/bands as a tooltip -- that's the one thing row 2
    // dropped entirely (see prior change), so it's the only thing missing from
    // the on-screen row worth popping up; call/ref/state/range are already
    // fully visible on the row itself (just possibly truncated on a very
    // narrow pane), so repeating them here would just make the tooltip too
    // big to fit -- see conversation.
    int vis_row = (s.y - (box.y + LISTING_Y0)) / PS_ROW_H;
    int i;
    if (ps_ss.findDataIndex (vis_row, i)) {
        const PotaSchedEntry &pe = potasched_entries[i];
        if (pe.freqs[0]) {
            char tip[60];
            snprintf (tip, sizeof(tip), "Freq/Bands: %s", pe.freqs);
            tooltip (s, tip);
        }
    }

    // ours even if row is empty
    return (true);
}
