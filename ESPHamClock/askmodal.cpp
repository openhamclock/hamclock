/* modal virtual keyboard text editor for titles, config names, watch lists, etc.
 */

#include "HamClock.h"

// virtual qwerty keyboard definition for modal text editing
#define MODAL_NQR 4
#define MODAL_NQC 13
#define MODAL_KB_CHAR_H 44
#define MODAL_KB_CHAR_W 59
#define MODAL_KB_INDENT 16
#define MODAL_KB_Y0 230
#define MODAL_TF_INDENT 8
#define MODAL_BF_INDENT 34
#define MODAL_F_DESCENT 5
#define MODAL_KB_SPC_Y (MODAL_KB_Y0 + MODAL_NQR * MODAL_KB_CHAR_H)
#define MODAL_KB_SPC_H 34
#define MODAL_SBAR_X (MODAL_KB_INDENT + 5 * MODAL_KB_CHAR_W / 2)
#define MODAL_SBAR_W (MODAL_KB_CHAR_W * 8)

#define TX_CLR  RA8875_WHITE
#define TL_CLR  RGB565(255,125,0)
#define NAME_CW 9
#define NAME_CCLR RA8875_RED

typedef struct {
    char normal, shifted;
} ModalKBKey;

static const ModalKBKey modal_qwerty[MODAL_NQR][MODAL_NQC] = {
    { {'`', '~'}, {'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'},
      {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}
    },
    { {'Q', 'q'}, {'W', 'w'}, {'E', 'e'}, {'R', 'r'}, {'T', 't'}, {'Y', 'y'}, {'U', 'u'},
      {'I', 'i'}, {'O', 'o'}, {'P', 'p'}, {'[', '{'}, {']', '}'}, {'\\', '|'}
    },
    { {'A', 'a'}, {'S', 's'}, {'D', 'd'}, {'F', 'f'}, {'G', 'g'}, {'H', 'h'}, {'J', 'j'},
      {'K', 'k'}, {'L', 'l'}, {';', ':'}, {'\'', '"'}, {0, 0}, {0, 0}
    },
    { {'Z', 'z'}, {'X', 'x'}, {'C', 'c'}, {'V', 'v'}, {'B', 'b'}, {'N', 'n'}, {'M', 'm'},
      {',', '<'}, {'.', '>'}, {'/', '?'}, {0, 0}, {0, 0}, {0, 0}
    }
};

static const uint8_t modal_qroff[MODAL_NQR] = {
    MODAL_KB_INDENT,
    MODAL_KB_INDENT,
    (uint8_t)(MODAL_KB_INDENT + MODAL_KB_CHAR_W),
    (uint8_t)(MODAL_KB_INDENT + 3 * MODAL_KB_CHAR_W / 2)
};

static const SBox modal_paste_b = {MODAL_KB_INDENT, MODAL_KB_SPC_Y,
                                    (uint16_t)(MODAL_SBAR_X - MODAL_KB_INDENT - 12), MODAL_KB_SPC_H};
static const SBox modal_space_b  = {MODAL_SBAR_X, MODAL_KB_SPC_Y, MODAL_SBAR_W, MODAL_KB_SPC_H};
static const SBox modal_delete_b = {(uint16_t)(MODAL_KB_INDENT + 12 * MODAL_KB_CHAR_W),
                                    (uint16_t)(MODAL_KB_Y0 + 2 * MODAL_KB_CHAR_H),
                                    MODAL_KB_CHAR_W, MODAL_KB_CHAR_H};
static const SBox modal_done_b   = {(uint16_t)(MODAL_KB_INDENT + 23 * MODAL_KB_CHAR_W / 2),
                                    (uint16_t)(MODAL_KB_Y0 + 3 * MODAL_KB_CHAR_H),
                                    (uint16_t)(3 * MODAL_KB_CHAR_W / 2), MODAL_KB_CHAR_H};
static const SBox modal_left_b   = {(uint16_t)(MODAL_SBAR_X + MODAL_SBAR_W),
                                    MODAL_KB_SPC_Y,
                                    (uint16_t)(5 * MODAL_KB_CHAR_W / 4), MODAL_KB_SPC_H};
static const SBox modal_right_b  = {(uint16_t)(MODAL_SBAR_X + MODAL_SBAR_W + 5 * MODAL_KB_CHAR_W / 4),
                                    MODAL_KB_SPC_Y,
                                    (uint16_t)(5 * MODAL_KB_CHAR_W / 4), MODAL_KB_SPC_H};

static void drawModalKeyboard (void)
{
    tft.fillRect (0, MODAL_KB_Y0, tft.width(), tft.height()-MODAL_KB_Y0, RA8875_BLACK);
    tft.setTextColor (RA8875_WHITE);
    selectFontStyle (LIGHT_FONT, SMALL_FONT);

    for (int r = 0; r < MODAL_NQR; r++) {
        uint16_t y = r * MODAL_KB_CHAR_H + MODAL_KB_Y0 + MODAL_KB_CHAR_H;
        const ModalKBKey *row = modal_qwerty[r];
        for (int c = 0; c < MODAL_NQC; c++) {
            const ModalKBKey *kp = &row[c];
            char n = kp->normal;
            if (n) {
                uint16_t x = modal_qroff[r] + c * MODAL_KB_CHAR_W;

                // shifted char above left
                tft.setCursor (x + MODAL_TF_INDENT, y - MODAL_KB_CHAR_H/3 - MODAL_F_DESCENT);
                tft.print((char)kp->shifted);

                // non-shifted below right
                tft.setCursor (x + MODAL_BF_INDENT, y - MODAL_F_DESCENT);
                tft.print(n);

                // key border
                tft.drawRect (x, y - MODAL_KB_CHAR_H, MODAL_KB_CHAR_W, MODAL_KB_CHAR_H, RGB565(80,80,255));
            }
        }
    }

    drawStringInBox ("Paste", modal_paste_b, false, RA8875_CYAN);
    drawStringInBox ("", modal_space_b, false, RA8875_WHITE);
    drawStringInBox ("Del", modal_delete_b, false, RA8875_RED);
    drawStringInBox ("Done", modal_done_b, false, RA8875_GREEN);
    drawStringInBox ("<==", modal_left_b, false, RA8875_RED);
    drawStringInBox ("==>", modal_right_b, false, RA8875_RED);
}

static bool modalS2Char (const SCoord &s, char &kbchar)
{
    if (s.y >= MODAL_KB_Y0 && s.y < MODAL_KB_Y0 + MODAL_NQR * MODAL_KB_CHAR_H) {
        uint16_t kb_y = s.y - MODAL_KB_Y0;
        uint8_t row = kb_y / MODAL_KB_CHAR_H;
        if (row < MODAL_NQR && s.x > modal_qroff[row]) {
            uint8_t col = (s.x - modal_qroff[row]) / MODAL_KB_CHAR_W;
            if (col < MODAL_NQC) {
                const ModalKBKey *kp = &modal_qwerty[row][col];
                char norm_char = kp->normal;
                if (norm_char) {
                    if (s.y < MODAL_KB_Y0 + row * MODAL_KB_CHAR_H + MODAL_KB_CHAR_H / 2)
                        kbchar = kp->shifted;
                    else
                        kbchar = norm_char;
                    return true;
                }
            }
        }
    }

    if (inBox (s, modal_space_b)) {
        kbchar = CHAR_SPACE;
        return true;
    }
    if (inBox (s, modal_delete_b)) {
        kbchar = CHAR_DEL;
        return true;
    }
    if (inBox (s, modal_done_b)) {
        kbchar = CHAR_NL;
        return true;
    }
    if (inBox (s, modal_left_b)) {
        kbchar = CHAR_LEFT;
        return true;
    }
    if (inBox (s, modal_right_b)) {
        kbchar = CHAR_RIGHT;
        return true;
    }

    return false;
}

static void drawModalInputBox (const SBox &box, const char *str, int cursor)
{
    fillSBox (box, RA8875_BLACK);
    drawSBox (box, RA8875_WHITE);
    selectFontStyle (LIGHT_FONT, SMALL_FONT);
    tft.setTextColor (TX_CLR);
    tft.setCursor (box.x + 12, box.y + 28);
    tft.print (str);

    // draw cursor
    StackMalloc pfx_mem(cursor + 1);
    char *prefix = (char *) pfx_mem.getMem();
    strncpy (prefix, str, cursor);
    prefix[cursor] = '\0';
    uint16_t cur_x = box.x + 12 + getTextWidth (prefix);
    uint16_t cur_y = box.y + 33;
    tft.drawLine (cur_x, cur_y, cur_x + NAME_CW, cur_y, NAME_CCLR);
    tft.drawLine (cur_x, cur_y + 1, cur_x + NAME_CW, cur_y + 1, NAME_CCLR);
}

/* general modal virtual keyboard dialog.
 * user edits text up to max_len (including EOS).
 * returns true if user accepted with Ok or Done, false if cancelled.
 */
bool askModalText (const char *title, const char *prompt, char text[], size_t max_len, bool to_upper, const char *disallow)
{
    // capture full screen backing store
    uint8_t *backing_store = NULL;
    if (!tft.getBackingStore (backing_store, 0, 0, tft.width(), tft.height())) {
        Serial.printf ("askModalText: failed to get backing store\n");
        return false;
    }

    FontWeight saved_fw;
    FontSize saved_fs;
    getFontStyle (&saved_fw, &saved_fs);

    bool saved_mainpage_up = mainpage_up;

    // clear screen / draw dialog background container
    tft.fillScreen (RA8875_BLACK);
    SBox screen_b = {0, 0, (uint16_t)tft.width(), (uint16_t)tft.height()};
    drawSBox (screen_b, GRAY);

    // draw title
    selectFontStyle (BOLD_FONT, SMALL_FONT);
    tft.setTextColor (TL_CLR);
    const char *use_title = title ? title : "Edit Text";
    uint16_t tw = getTextWidth ((char*)use_title);
    tft.setCursor ((tft.width() - tw) / 2, 35);
    tft.print (use_title);

    // draw prompt
    if (prompt && strlen(prompt) > 0) {
        selectFontStyle (LIGHT_FONT, SMALL_FONT);
        tft.setTextColor (TL_CLR);
        tft.setCursor (100, 70);
        tft.print (prompt);
    }

    // buffer to edit
    StackMalloc edit_mem (max_len + 2);
    char *edit_buf = (char *) edit_mem.getMem();
    quietStrncpy (edit_buf, text ? text : "", max_len);
    int cursor = strlen (edit_buf);

    // input box
    SBox input_b = {100, 85, 600, 42};
    drawModalInputBox (input_b, edit_buf, cursor);

    // Ok and Cancel buttons
    SBox ok_b = {240, 145, 140, 40};
    SBox cancel_b = {420, 145, 140, 40};
    selectFontStyle (BOLD_FONT, SMALL_FONT);
    drawStringInBox ("Ok", ok_b, false, RA8875_WHITE);
    drawStringInBox ("Cancel", cancel_b, false, RA8875_WHITE);

    // draw virtual keyboard
    drawModalKeyboard();
    tft.drawPR();

    // event loop
    UserInput ui = {
        screen_b,
        UI_UFuncNone,
        UF_UNUSED,
        UI_NOTIMEOUT,
        UF_NOCLOCKS,
        {0, 0}, TT_NONE, '\0', false, false
    };

    auto isCharAllowed = [&] (char c) -> bool {
        if (!isprint(c)) return false;
        if (disallow && strchr(disallow, c)) return false;
        return true;
    };

    bool ok = false;
    while (waitForUser (ui)) {
        char kbc = ui.kb_char;

        // check cancel
        if (kbc == CHAR_ESC || inBox (ui.tap, cancel_b)) {
            selectFontStyle (BOLD_FONT, SMALL_FONT);
            drawStringInBox ("Cancel", cancel_b, true, RA8875_WHITE);
            wdDelay (200);
            ok = false;
            break;
        }

        // check ok
        if (kbc == CHAR_NL || kbc == CHAR_CR || inBox (ui.tap, ok_b)) {
            selectFontStyle (BOLD_FONT, SMALL_FONT);
            drawStringInBox ("Ok", ok_b, true, RA8875_WHITE);
            wdDelay (200);
            ok = true;
            break;
        }

        // check tapping in input box to position cursor
        if (inBox (ui.tap, input_b)) {
            uint16_t sdx = (ui.tap.x > input_b.x + 12) ? (ui.tap.x - (input_b.x + 12)) : 0;
            StackMalloc copy_mem (strlen(edit_buf) + 1);
            char *copy = (char *) copy_mem.getMem();
            strcpy (copy, edit_buf);
            (void) maxStringW (copy, sdx);
            cursor = strlen (copy);
            drawModalInputBox (input_b, edit_buf, cursor);
            continue;
        }

        // check tapping Paste button
        if (inBox (ui.tap, modal_paste_b)) {
            drawStringInBox ("Paste", modal_paste_b, true, RA8875_CYAN);
            StackMalloc paste_mem (max_len + 100);
            char *paste_buf = (char *) paste_mem.getMem();
            bool got_clip = false;
#if defined(_IS_ANDROID)
            got_clip = android_get_clipboard (paste_buf, max_len + 100);
#endif
            if (got_clip) {
                strTrimAll (paste_buf);
                for (int i = 0; paste_buf[i] != '\0'; i++) {
                    char c = paste_buf[i];
                    if (to_upper)
                        c = toupper(c);
                    if (isCharAllowed(c)) {
                        size_t sl = strlen (edit_buf);
                        selectFontStyle (LIGHT_FONT, SMALL_FONT);
                        if (sl < max_len - 1 && getTextWidth (edit_buf) < input_b.w - 30) {
                            memmove (edit_buf + cursor + 1, edit_buf + cursor,
                                     strlen (edit_buf) - cursor + 1);
                            edit_buf[cursor] = c;
                            cursor++;
                        }
                    }
                }
                drawModalInputBox (input_b, edit_buf, cursor);
            } else {
                tft.pasteClipboard();
                requestLiveWebPaste();
            }
            wdDelay (150);
            drawStringInBox ("Paste", modal_paste_b, false, RA8875_CYAN);
            continue;
        }

        // check virtual keyboard tap if no physical key
        if (kbc == CHAR_NONE && modalS2Char (ui.tap, kbc)) {
            if (kbc == CHAR_NL) {
                drawStringInBox ("Done", modal_done_b, true, RA8875_GREEN);
                wdDelay (200);
                ok = true;
                break;
            }
        }

        if (to_upper && isprint(kbc))
            kbc = toupper(kbc);

        // process editing char
        if (kbc == CHAR_RIGHT) {
            if (cursor < (int)strlen (edit_buf)) {
                cursor++;
                drawModalInputBox (input_b, edit_buf, cursor);
            }
        } else if (kbc == CHAR_LEFT) {
            if (cursor > 0) {
                cursor--;
                drawModalInputBox (input_b, edit_buf, cursor);
            }
        } else if (kbc == CHAR_BS || kbc == CHAR_DEL) {
            if (cursor > 0) {
                memmove (edit_buf + cursor - 1, edit_buf + cursor,
                         strlen (edit_buf) - cursor + 1);
                cursor--;
                drawModalInputBox (input_b, edit_buf, cursor);
            }
        } else if (isCharAllowed(kbc)) {
            size_t sl = strlen (edit_buf);
            selectFontStyle (LIGHT_FONT, SMALL_FONT);
            if (sl < max_len - 1 && getTextWidth (edit_buf) < input_b.w - 30) {
                memmove (edit_buf + cursor + 1, edit_buf + cursor,
                         strlen (edit_buf) - cursor + 1);
                edit_buf[cursor] = kbc;
                cursor++;
                drawModalInputBox (input_b, edit_buf, cursor);
            }
        }
    }

    if (ok) {
        strTrimAll (edit_buf);
        quietStrncpy (text, edit_buf, max_len);
    }

    // restore screen backing store
    if (!tft.setBackingStore (backing_store, 0, 0, tft.width(), tft.height()))
        fatalError ("mem pixel restore failed in askModalText");
    selectFontStyle (saved_fw, saved_fs);
    mainpage_up = saved_mainpage_up;
    if (saved_mainpage_up) {
        showClocks();
        tft.setPR (map_b.x, map_b.y, map_b.w, map_b.h);
    }
    tft.drawPR();

    return ok;
}
