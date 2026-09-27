/* handy class to manage the scroll controls used in several panes.
 *
 * assumes data array is sorted oldest first. displayed list can either show newest entry
 * on top or bottom row, depending on scrollT2B():
 *   if true (newest is shown on top) then scrolling Up means show Newer entries and Down means Older.
 *   if false (newest is shown at the bottom) then scrolling Up means show Older entries and Down means Newer.
 * The data array and its state variables are exactly the same either way, only the displayed rows are
 * affected. Display row indices always start with 0 on top either way.
 *
 * state variables:
 *
 *   max_vis: maximum rows in the displayed list
 *   top_vis: index into the data array containing the newest entry to be displayed
 *   n_data:  the number of entries in the data array
 *
 * Examples show data in a box and visible list view into the data. Data indices are shown down the left
 * side, 0 always the oldest. Values within the box start with A, the first datum to be added and thus
 * the oldest. Display list is shown to the right with view port pointing into the viewable portion of data.
 *
 * example 1: n_data 7, max_vis 3, scrolled to show newest data
 *
 *  scrollT2B() is true:
 *  
 *    top_vis  6 |  G  |  <------|        display row 0, ie, top row shows newest data
 *             5 |  F  |       max_vis    display row 1
 *             4 |  E  |  <------|        display row 2, ie, bottom row shows oldest viewable data
 *             3 |  D  |   
 *             2 |  C  |   
 *             1 |  B  | 
 *             0 |  A  |                  oldest data array entry at index 0
 *               -------
 *  
 *  scrollT2B() is false, ie, srolling bottom-to-top:
 *  
 *               -------
 *             0 |  A  |                  oldest data array entry at index 0
 *             1 |  B  |
 *             2 |  C  |
 *             3 |  D  |
 *             4 |  E  |  <------|        display row 0, ie, top row shows oldest viewable data
 *             5 |  F  |      max_vis     display row 1
 *    top_vis  6 |  G  |  <------|        display row 2, ie, bottom row shows newest data
 *
 *
 * example 2: n_data 3, max_vis 6, scrolled to show newest data
 *
 *  scrollT2B() is true:
 *  
 *    top_vis  2 |  C  |  <------|        display row 0, ie, top row shows newest data
 *             1 |  B  |         |        display row 1
 *             0 |  A  |         |        display row 2, ie, middle row shows oldest data
 *               -------      max_vis     display row 3 empty
 *                               |        display row 4 empty
 *                        <------|        display row 5 empty
 *  
 *   scrollT2B() is false, ie, srolling bottom-to-top:
 *  
 *                        <------|        display row 0 empty
 *                               |        display row 1 empty
 *               -------      max_vis     display row 2 empty
 *             0 |  A  |         |        display row 3, ie, middle row shows oldest data
 *             1 |  B  |         |        display row 4
 *    top_vis  2 |  C  |  <------|        display row 5, ie, bottom row shows newest data
 *
 */

#include "HamClock.h"


// control layout geometry
#define SCR_DX          24                      // x offset back from box right to center of scroll arrows
#define SCRUP_DY        9                       // y offset from box top to center of up arrow
#define SCRDW_DY        23                      // y offset from box top to center of down arrow
#define SCR_W           6                       // scroll arrow width
#define SCR_H           10                      // scroll arrow height
#define NEWSYM_DX       6                       // "new spots" symbol dx from box left
#define NEWSYM_H        9                       // "new spots" symbol height
#define NEWSYM_DY       (PANETITLE_H-NEWSYM_H)  // "new spots" symbol top dy from box top
#define NEWSYM_W        20                      // "new spots" symbol width




/* draw or erase the up scroll control as needed.
 */
void ScrollState::drawScrollUpControl (const SBox &box, uint16_t arrow_color, uint16_t number_color) const
{
        bool draw = okToScrollUp();

        // up arrow

        const uint16_t scr_dx = box.w - SCR_DX;                 // center
        const uint16_t x0 = box.x + scr_dx;                     // top point
        const uint16_t y0 = box.y + SCRUP_DY - SCR_H/2;
        const uint16_t x1 = box.x + scr_dx - SCR_W/2;           // LL
        const uint16_t y1 = box.y + SCRUP_DY + SCR_H/2;
        const uint16_t x2 = box.x + scr_dx + SCR_W/2;           // LR
        const uint16_t y2 = box.y + SCRUP_DY + SCR_H/2;

        tft.fillRect (x2+1, y0+1, box.x + box.w - x2 - 2, SCR_H, RA8875_BLACK);
        // tft.drawRect (x2+1, y0+1, box.x + box.w - x2 - 2, SCR_H, RA8875_RED);

        if (draw) {
            tft.setCursor (x2+1, y0+2);
            selectFontStyle (LIGHT_FONT, FAST_FONT);
            tft.setTextColor (number_color);
            int nma = nMoreAbove();
            if (nma < 1000)
                tft.print (nma);
            else
                tft.print (">99");
        }

        tft.fillTriangle (x0, y0, x1, y1, x2, y2, draw ? arrow_color : RA8875_BLACK);
}

/* draw, else erase, the down scroll control and associated count n.
 */
void ScrollState::drawScrollDownControl (const SBox &box, uint16_t arrow_color, uint16_t number_color) const
{
        bool draw = okToScrollDown();

        // down arrow 

        const uint16_t scr_dx = box.w - SCR_DX;                 // center
        const uint16_t x0 = box.x + scr_dx - SCR_W/2;           // UL
        const uint16_t y0 = box.y + SCRDW_DY - SCR_H/2;
        const uint16_t x1 = box.x + scr_dx + SCR_W/2;           // UR
        const uint16_t y1 = box.y + SCRDW_DY - SCR_H/2;
        const uint16_t x2 = box.x + scr_dx;                     // bottom point
        const uint16_t y2 = box.y + SCRDW_DY + SCR_H/2;

        tft.fillRect (x1+1, y0+1, box.x + box.w - x1 - 2, SCR_H, RA8875_BLACK);
        // tft.drawRect (x1+1, y0+1, box.x + box.w - x1 - 2, SCR_H, RA8875_RED);

        if (draw) {
            tft.setCursor (x1+1, y0+2);
            selectFontStyle (LIGHT_FONT, FAST_FONT);
            tft.setTextColor (number_color);
            int nmb = nMoreBeneath();
            if (nmb < 1000)
                tft.print (nmb);
            else
                tft.print (">99");
        }

        tft.fillTriangle (x0, y0, x1, y1, x2, y2, draw ? arrow_color : RA8875_BLACK);
}


/* draw, else erase, the New Spots symbol in newsym_b.
 * when drawing: active indicates whether it should be shown as having just been tapped.
 * N.B. must have already called initNewSpotsSymbol()
 */
void ScrollState::drawNewSpotsSymbol (bool draw, bool active) const
{
    if (draw) {
        selectFontStyle (LIGHT_FONT, FAST_FONT);
        drawStringInBox ("New", newsym_b, active, newsym_color);
    } else {
        fillSBox (newsym_b, RA8875_BLACK);
    }
}

/* return whether tap at s is over the New Spot symbol within box b.
 * N.B. must have already called initNewSpotsSymbol()
 */
bool ScrollState::checkNewSpotsTouch (const SCoord &s, const SBox &b) const
{
    return (inBox (s, b) && inBox (s, newsym_b));
}

/* prep the New Sym box to be withing the given box
 */
void ScrollState::initNewSpotsSymbol (const SBox &box, uint16_t color)
{
    newsym_b.x = box.x + NEWSYM_DX;
    newsym_b.y = box.y + NEWSYM_DY;
    newsym_b.w = NEWSYM_W;
    newsym_b.h = NEWSYM_H;

    newsym_color = color;
}



/* return whether tap at s within box b means to scroll up
 */
bool ScrollState::checkScrollUpTouch (const SCoord &s, const SBox &b) const
{
    return (s.x > b.x + b.w - SCR_DX - SCR_W && s.y <= b.y + SCRUP_DY + SCR_H/2);
}

/* return whether tap at s within box b means to scroll down
 */
bool ScrollState::checkScrollDownTouch (const SCoord &s, const SBox &b) const
{
    return (s.x > b.x + b.w - SCR_DX - SCR_W
                && s.y > b.y + SCRUP_DY + SCR_H/2 && s.y < b.y + SCRDW_DY + SCR_H/2);
}


/* move top_vis towards older data, ie, towards the beginning of the data array
 */
void ScrollState::moveTowardsOlder()
{
    top_vis -= max_vis - 1;         // leave one for context
    int llimit = max_vis - 1;
    if (top_vis < llimit)
        top_vis = llimit;
}

/* move top_vis towards newer data, ie, towards the end of the data array
 */
void ScrollState::moveTowardsNewer()
{
    top_vis += max_vis - 1;         // leave one for context
    int ulimit = n_data - 1;
    if (top_vis > ulimit)
        top_vis = ulimit;
}



/* modify top_vis to expose data below the visible list
 */
void ScrollState::scrollDown (void)
{
    if (scrollT2B())
        moveTowardsOlder();
    else
        moveTowardsNewer();
}

/* modify top_vis to expose data above the visible list
 */
void ScrollState::scrollUp (void)
{
    if (scrollT2B())
        moveTowardsNewer();
    else
        moveTowardsOlder();
}




/* return whether there is more data beneath the displayed list
 */
bool ScrollState::okToScrollDown (void) const
{
    return (nMoreBeneath() > 0);

}

/* return whether there is more data above the displayed list
 */
bool ScrollState::okToScrollUp (void) const
{
    return (nMoreAbove() > 0);
}

/* return whether the displayed rows are at the top most (newest) data entry
 */
bool ScrollState::atNewest (void) const
{
    return (n_data == 0 || top_vis == n_data-1);
}





/* return the additional number of spots not shown above the current list
 */
int ScrollState::nMoreAbove (void) const
{
    int n;
    if (scrollT2B())
        n = n_data - top_vis - 1;
    else
        n = top_vis - max_vis + 1;
    return (n);
}

/* return the additional number of spots not shown below the current list
 */
int ScrollState::nMoreBeneath (void) const
{
    int n;
    if (scrollT2B())
        n = top_vis - max_vis + 1;
    else
        n = n_data - top_vis - 1;
    return (n);
}

/* scroll to position the newest entry at the head of the list.
 */
void ScrollState::scrollToNewest (void)
{
    top_vis = n_data - 1;
    if (top_vis < 0)
        top_vis = 0;
}

/* scroll to position the very first (array index 0) entry into the visible list, ie, the
 * opposite of scrollToNewest(). handy for lists whose sort order isn't chronological (eg
 * ONTA's Band or Org sort, ascending low to high) where "newest" doesn't mean anything and
 * the desired initial view is simply the beginning of the array, not whichever end
 * scrollToNewest() happens to land on.
 */
void ScrollState::scrollToOldest (void)
{
    top_vis = max_vis - 1;
    if (top_vis > n_data - 1)
        top_vis = n_data - 1;
    if (top_vis < 0)
        top_vis = 0;
}

/* given a display row index, which always start with 0 on top, find the corresponding data array index.
 * return whether actually within range.
 */
bool ScrollState::findDataIndex (int display_row, int &array_index) const
{
    int i;

    if (scrollT2B())
        i = top_vis - display_row;
    else
        i = top_vis - max_vis + 1 + display_row;

    bool ok = i >= 0 && i < n_data;
    if (ok)
        array_index = i;

    return (ok);
}

/* pass back the min and max data _array_ indices currently visible and return total row count.
 * use getDisplayRow() to convert to display row.
 */
int ScrollState::getVisDataIndices (int &min_i, int &max_i) const
{
    // N.B. inclusivity used for computing n below gives 1 when there is no data
    if (n_data == 0)
        return (0);

    max_i = top_vis;                            // max index is always newest being displayed
    min_i = top_vis - max_vis + 1;              // min index is max_vis lower, inclusive
    if (min_i < 0)                              // but might not be enough to fill the list
        min_i = 0;
    int n = max_i - min_i + 1;                  // count, inclusive

    if (debugLevel (DEBUG_SCROLL, 1))
        Serial.printf ("SCROLL: top_vis= %d max_vis= %d n_data= %d  min_i= %d max_i= %d n= %d\n",
                            top_vis, max_vis, n_data, min_i, max_i, n);
    return (n);
}

/* given a data array index ala getVisDataIndices, return the display row number starting with 0 on top.
 */
int ScrollState::getDisplayRow (int array_index) const
{
    int i;

    if (scrollT2B())
        i = top_vis - array_index;
    else
        i = array_index - (top_vis - (max_vis-1));

    return (i);
}

/* return whether to scroll top-to-bottom else bottom-to-top
 */
bool ScrollState::scrollT2B(void) const
{
    if (dir == DIR_FROMSETUP)
        return (scrollTopToBottom());
    if (dir == DIR_TOPDOWN)
        return (true);
    if (dir == DIR_BOTUP)
        return (false);
     // defensive: called before init set dir -- don't take down the whole app over a
    // scroll direction. Log it so it's still visible/traceable, then use a sane default.
    Serial.printf ("SCROLL: scrollT2B() called with dir unset -- defaulting to top-to-bottom\n");
    return (scrollTopToBottom());
}
