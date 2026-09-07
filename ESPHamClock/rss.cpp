/* manage displaying RSS feeds or local RESTful content.
 */

#include "HamClock.h"

// global state
uint8_t rss_interval = RSS_DEF_INT;             // polling period, secs
SBox rss_bnr_b;                                 // main rss banner over map
uint8_t rss_on;                                 // whether currently on
bool rss_local;                                 // if set: use local titles, not server

// local state
#define RSS_MAXN        15                      // max number RSS entries to cache
static const char rss_page[] = "/RSS/web15rss.pl";
static char *rss_titles[RSS_MAXN];              // malloced titles
static char *rss_tapurl;                        // malloced URL if tapped, if any
static uint8_t rss_ntitles, rss_title_i;        // n titles and rolling index
static time_t rss_next;                         // when to retrieve next set

/* download more RSS titles.
 * return whether io ok, even if no new titles.
 */
static bool retrieveRSS (void)
{
    // prep
    WiFiClient rss_client;
    char line[256];
    bool ok = false;

    // reset count and index
    rss_ntitles = rss_title_i = 0;

    Serial.println(rss_page);
    if (rss_client.connect(backend_host, backend_port)) {

        updateClocks(false);

        // fetch feed page
        httpHCGET (rss_client, backend_host, rss_page);

        // skip response header
        if (!httpSkipHeader (rss_client)) {
            Serial.println ("RSS header short");
            goto out;
        }

        // io ok
        ok = true;

        // get up to RSS_MAXN more rss_titles[]
        for (rss_ntitles = 0; rss_ntitles < RSS_MAXN; rss_ntitles++) {
            if (!getTCPLine (rss_client, line, sizeof(line), NULL))
                goto out;
            if (rss_titles[rss_ntitles])
                free (rss_titles[rss_ntitles]);
            rss_titles[rss_ntitles] = strdup (line);
            // Serial.printf ("RSS[%d] len= %d\n", rss_ntitles, strlen(rss_titles[rss_ntitles]));
        }
    }

  out:

    rss_client.stop();

    return (ok);
}

/* display next RSS feed item if on, retrieving more as needed.
 * if local always return true, else return whether retrieval io was ok.
 */
static bool updateRSS (void)
{
    // skip if not on and clear list if not local
    if (!rss_on) {
        if (!rss_local) {
            while (rss_ntitles > 0) {
                free (rss_titles[--rss_ntitles]);
                rss_titles[rss_ntitles] = NULL;
            }
        }
        return (true);
    }

    // prepare background to show life before possibly lengthy net update
    fillSBox (rss_bnr_b, RSS_BG_COLOR);
    tft.drawLine (rss_bnr_b.x, rss_bnr_b.y, rss_bnr_b.x+rss_bnr_b.w, rss_bnr_b.y, GRAY);

    // fill rss_titles[] from network if empty and wanted
    if (!rss_local && rss_title_i >= rss_ntitles) {
        bool ok = retrieveRSS();

        // display err msg if still no rss_titles
        if (!ok || rss_ntitles == 0) {
            selectFontStyle (LIGHT_FONT, SMALL_FONT);
            const char *msg = ok ? "No RSS titles" : "RSS network error";
            uint16_t msg_w = getTextWidth (msg);
            tft.setTextColor (RSS_FG_COLOR);
            tft.setCursor (rss_bnr_b.x + (rss_bnr_b.w-msg_w)/2, rss_bnr_b.y + 2*rss_bnr_b.h/3-1);
            tft.print (msg);
            Serial.printf ("RSS: %s\n", msg);
            return (ok);
        }
    }

    // done if no titles
    if (rss_ntitles == 0)
        return (true);

    // draw next rss_title
    char *title = rss_titles[rss_title_i];

    // usable banner drawing x and width
    uint16_t ubx = rss_bnr_b.x + 5;
    uint16_t ubw = rss_bnr_b.w - 10;

    // get title width in pixels
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    int tw = getTextWidth (title);

    // draw as 1 or 2 lines to fit within ubw
    tft.setTextColor (RSS_FG_COLOR);
    if (tw < ubw) {
        // title fits on one row, draw centered horizontally and vertically
        tft.setCursor (ubx + (ubw-tw)/2, rss_bnr_b.y + 2*rss_bnr_b.h/3-1);
        tft.print (title);
    } else {
        // title too long, keep shrinking until it fits
        for (bool fits = false; !fits; ) {

            // split at center blank
            int tl = strlen(title);
            char *row2 = strchr (title+tl/2, ' ');
            if (!row2)
                row2 = title+tl/2;          // no blanks! just split in half?
            char sep_char = *row2;          // save to restore
            *row2++ = '\0';                 // replace blank with EOS and move to start of row 2 -- restore!
            uint16_t r1w = getTextWidth (title);
            uint16_t r2w = getTextWidth (row2);

            // draw if fits
            if (r1w <= ubw && r2w <= ubw) {
                tft.setCursor (ubx + (ubw-r1w)/2, rss_bnr_b.y + rss_bnr_b.h/2 - 8);
                tft.print (title);
                tft.setCursor (ubx + (ubw-r2w)/2, rss_bnr_b.y + rss_bnr_b.h - 9);
                tft.print (row2);

                // got it
                fits = true;
            }

            // restore zerod char
            row2[-1] = sep_char;

            if (!fits) {
                Serial.printf ("RSS shrink from %d %d ", tw, tl);
                tw = maxStringW (title, 9*tw/10);       // modifies title
                tl = strlen(title);
                Serial.printf ("to %d %d\n", tw, tl);
            }
        }
    }

    // reset url for tap, restore if available
    free (rss_tapurl);
    rss_tapurl = NULL;

    // if local just cycle to next title, else remove from list and advance
    if (rss_local) {
        rss_title_i = (rss_title_i + 1) % rss_ntitles;
    } else {
        // before removing current title, save up to : in rss_tapurl in case of tap
        char *title = rss_titles[rss_title_i];
        char *colon = strchr (title, ':');
        if (colon) {
            char url[200];
            snprintf (url, sizeof(url), "https://%.*s", (int)(colon-title), title);
            rss_tapurl = strdup (url);
        }

        // finished with this title
        free (rss_titles[rss_title_i]);
        rss_titles[rss_title_i++] = NULL;
    }
 
    return (true);
}

/* called frequently to check for RSS updates
 */
void checkRSS(void)
{
    if (myNow() >= rss_next) {
        if (updateRSS())
            rss_next = myNow() + rss_interval;
        else
            rss_next = nextWiFiRetry("RSS");
    }
}

/* used by web server to control local RSS title list.
 * if title == NULL
 *   restore normal network operation
 * else if title[0] == '\0'
 *   turn off network and empty the local title list
 * else
 *   turn off network and add the given title to the local list
 * always report the current number of titles in the list and max number possible.
 * return whether ok
 */
bool setRSSTitle (const char *title, int &n_titles, int &max_titles)
{
    if (!title) {

        // restore network operation
        rss_local = false;
        rss_ntitles = rss_title_i = 0;

    } else {

        // erase list if network on or asked to do so
        if (!rss_local || title[0] == '\0') {
            rss_ntitles = rss_title_i = 0;
            for (int i = 0; i < RSS_MAXN; i++) {
                if (rss_titles[i]) {
                    free (rss_titles[i]);
                    rss_titles[i] = NULL;
                }
            }
        }

        // turn off network
        rss_local = true;

        // add title if room unless blank
        if (title[0] != '\0') {
            if (rss_ntitles < RSS_MAXN) {
                if (rss_titles[rss_ntitles])       // just paranoid
                    free (rss_titles[rss_ntitles]);
                rss_titles[rss_ntitles] = strdup (title);
                rss_title_i = rss_ntitles++;       // show new title
            } else {
                n_titles = RSS_MAXN;
                max_titles = RSS_MAXN;
                return (false);
            }
        }
    }

    // update info and refresh
    n_titles = rss_ntitles;
    max_titles = RSS_MAXN;
    scheduleRSSNow();

    // ok
    return (true);
}

/* called when RSS has just been turned on: update now and restart refresh cycle
 */
void scheduleRSSNow()
{
    rss_next = 0;
}

/* open the web page associated with the current title, if any
 */
void checkRSSTouch(void)
{
    if (rss_tapurl)
        showQRCodeModal (rss_tapurl, "RSS News Story", "Scan with phone or open below", "Open Article");
}
