/* windbadge.cpp -- on-map "Wind" badge for HamClock
 *
 * Same idea as adsbbadge.cpp: no on/off state of its own, draws nothing onto the map, just a
 * link button. Tapping it opens Windy.com centered on DE via openURLPopupEmbed() -- an in-app
 * overlay window on Live Web / Android, or a Chromium app-mode popup on the X11 desktop build -- see
 * qrz.cpp.
 *
 * On-map presence, not buried in the map menu: shown when core_map == CM_WX (the "Weather"
 * core map style) or CM_CLOUDS ("Clouds"), floating to the right of whichever badge is to its
 * left (WEFAX on CM_WX, Fires or Borders on CM_CLOUDS, else View).
 */

#include "HamClock.h"

SBox windmap_btn_b;                    // extern; badge box, geometry set each draw (floats with WEFAX/Fires/Borders)

/* return whether the on-map "Wind" badge should currently be shown.
 * Weather and Clouds maps -- no projection restriction, matching WEFAX's own simplicity, since this is
 * just a link and draws nothing that would look wrong in an azimuthal projection.
 */
bool windBadgeVisible(void)
{
    return (core_map == CM_WX || core_map == CM_CLOUDS);
}

/* draw (or blank) the on-map "Wind" badge.
 * Floats to the right of whichever badge is currently rightmost in its row to its left:
 * on Weather that's WEFAX (if showing) else View; on Clouds that's WEFAX (if showing)
 * else Fires (if showing) else Borders (if showing) else View. Recomputed here every draw,
 * same convention as drawFiresButton()/drawADSBBadge() tracking their neighbors.
 */
void drawWindButton(void)
{
    windmap_btn_b.y = view_btn_b.y;

    if (!windBadgeVisible()) {
        // wrong core map -- leave the map pixels already painted here alone.
        return;
    }

    const int gap = 4;
    const int pad = 8;
    selectFontStyle (LIGHT_FONT, FAST_FONT);
    uint16_t left_edge = view_btn_b.x + view_btn_b.w;
    if (bordersBadgeVisible())
        left_edge = borders_btn_b.x + borders_btn_b.w;
    if (firesBadgeVisible())
        left_edge = fires_btn_b.x + fires_btn_b.w;
    if (wefaxBadgeVisible())
        left_edge = wefax_btn_b.x + wefax_btn_b.w;
    windmap_btn_b.x = left_edge + gap;
    windmap_btn_b.w = getTextWidth ("Wind") + pad;
    windmap_btn_b.h = view_btn_b.h;

    // always the same "unpressed" look -- black fill, white outline/text -- since it has no
    // on/off state, just Borders/Fires' normal (off) styling permanently.
    tft.fillRect (windmap_btn_b.x, windmap_btn_b.y, windmap_btn_b.w-1, windmap_btn_b.h-1, RA8875_BLACK);
    tft.drawRect (windmap_btn_b.x, windmap_btn_b.y, windmap_btn_b.w-1, windmap_btn_b.h-1, RA8875_WHITE);

    const char *label = "Wind";
    uint16_t lbl_w = getTextWidth(label);
    tft.setCursor (windmap_btn_b.x+(windmap_btn_b.w-lbl_w)/2, windmap_btn_b.y+2);
    tft.setTextColor (RA8875_WHITE);
    tft.print (label);
}

/* open Windy.com centered on DE.
 * uses openURLPopupEmbed(): under Live Web / Android this is the in-page <iframe> overlay
 * using Windy's official embed widget (embed.windy.com/embed2.html); on the X11 desktop build it is
 * a chromeless Chromium app-mode popup positioned near the HamClock window; everywhere else
 * (fb0, ESP32, or no Chromium found) it falls back to openURL() in a browser tab -- see qrz.cpp.
 * call this from checkTouch() when windBadgeVisible() && inBox(s, windmap_btn_b).
 */
void windBadgeClicked(void)
{
    char url[256];
    snprintf (url, sizeof(url),
        "https://embed.windy.com/embed2.html?lat=%.3f&lon=%.3f&zoom=8&level=surface&overlay=wind",
        de_ll.lat_d, de_ll.lng_d);
    openURLPopupEmbed (url);
}
