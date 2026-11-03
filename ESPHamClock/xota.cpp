/* xota.cpp -- see xota.h.
 */

#include "HamClock.h"

#include <cctype>
#include <cstring>

#include "xota.h"

// one entry per recognized "extra" xOTA program, in match priority order.
// order matters only in the sense that the first label found in the comment
// wins if a comment somehow mentions more than one -- not expected to matter
// in practice.
typedef struct {
    const char *label;         // token to search for in the comment, eg "WCA"
    const char *org;           // canonical org name to report, usually == label
} XOTALabel;

static const XOTALabel xota_labels[] = {
    { "WWBOTA", "WWBOTA" },
    { "ARLHS",  "ARLHS"  },
    { "ILLW",   "ILLW"   },
    { "SIOTA",  "SIOTA"  },
    { "SILO",   "SIOTA"  },     // SiOTA's own materials say "CQ SiOTA" or "CQ SILO" are
                                // both valid on-air calls -- catch either
    { "WCA",    "WCA"    },
    { "WAB",    "WAB"    },
};

/* true if c can be part of a reference token: alnum, or the separators that
 * legitimately appear inside one (eg "B/US-0001", "DL-01063", "VK-ARN2").
 */
static bool isRefChar (char c)
{
    return (isalnum ((unsigned char)c) || c == '/' || c == '-');
}

/* starting at comment[from], skip up to a few separator characters (space,
 * colon, dash, equals) that typically sit between a label and its reference,
 * eg "WCA: F-07849" or "WCA F-07849" or "WCA=F-07849". return the index of
 * the first non-separator character, which may be == from if there was
 * nothing to skip, or may run off the end of the string.
 */
static size_t skipLabelSeparator (const char *comment, size_t from)
{
    size_t i = from;
    int n_skipped = 0;
    while (comment[i] && n_skipped < 3
                    && (comment[i] == ' ' || comment[i] == ':' || comment[i] == '-' || comment[i] == '=')) {
        i++;
        n_skipped++;
    }
    return (i);
}

/* WWBOTA references are self-identifying -- always "B/" followed by a short country/prefix
 * code, a dash, and digits, eg "B/US-0001", "B/G-0042" -- distinctive enough (the literal "B/"
 * is not a shape anything else here produces) to trust without requiring the "WWBOTA" label
 * nearby, unlike the rest of xota_labels[]. This matters in practice: real WWBOTA spot comments
 * often lead with the reference itself rather than following a "WWBOTA: ..." convention, which
 * a label-anchored-only scan would miss entirely. Same character-walk style as findIOTARef().
 */
static bool findWWBOTAShape (const char *comment, char *ref, size_t ref_len)
{
    size_t len = strlen (comment);
    for (size_t i = 0; i + 4 <= len; i++) {
        const char *p = comment + i;
        if (toupper((unsigned char)p[0]) != 'B' || p[1] != '/')
            continue;
        // must not be glued to more alnum chars before the 'B'
        if (i > 0 && isalnum ((unsigned char)comment[i-1]))
            continue;

        size_t j = i + 2;                                  // just past "B/"
        size_t code_start = j;
        while (comment[j] && isalpha ((unsigned char)comment[j]) && j - code_start < 4)
            j++;
        if (j == code_start || comment[j] != '-')
            continue;                                       // no letters, or no dash where expected
        j++;                                                 // past the dash
        size_t digit_start = j;
        while (comment[j] && isdigit ((unsigned char)comment[j]) && j - digit_start < 5)
            j++;
        if (j == digit_start)
            continue;                                        // no digits
        if (isalnum ((unsigned char)comment[j]))
            continue;                                        // glued to more alnum after -- not a clean end

        size_t n = j - i;
        if (n >= ref_len)
            continue;
        for (size_t k = 0; k < n; k++)
            ref[k] = toupper ((unsigned char)p[k]);
        ref[n] = '\0';
        return (true);
    }
    return (false);
}

/* POTA references are "<prefix>-<digits>", eg "US-4778", "K-0001", "DL-01063", but cluster
 * comments rarely write them cleanly. Real examples seen in the wild:
 *      "US POTA 4778 MICHIGAN"         prefix BEFORE the label, digits after, no dash
 *      "POTA - US--2996"               separator then a doubled dash
 *      "POTA: K-1234", "pota US-0042"  the tidy cases
 * so this is a dedicated scan rather than the generic label walk below, which would accept
 * things like "POTA 59" (a signal report) as a reference. Rules:
 *   - the word POTA must stand alone (not glued to other alnum chars)
 *   - after it, skip separators, then accept either
 *        A) 1-3 letters, 1-2 dashes, 3-5 digits   -> normalized to a single dash
 *        B) 4-5 bare digits -- park numbers are always at least 4 digits, which is also what
 *           keeps "POTA 599" out -- optionally prefixed by the word just before the label
 *           when that word looks like a 1-3 letter program prefix ("US POTA 4778" -> US-4778)
 * a bare "POTA" with no recognizable park number is deliberately NOT a match, same
 * label-plus-reference strictness as the rest of this file.
 */
static bool findPOTAShape (const char *comment, char *ref, size_t ref_len)
{
    static const char *not_prefix[] = { "CQ", "DE", "TU", "UP", "ON", "IN", "TO", "AT", "OF",
                                        "QSY", "QRP", "QRO", "QRZ", "SSB", "LSB", "USB", "FM",
                                        "AM", "CW", "FT8", "FT4", "THE", "NEW", "FOR" };
    const char *label = "POTA";
    const size_t label_len = 4;
    const char *search_from = comment;
    const char *hit;

    while ((hit = strcistr (search_from, label)) != NULL) {
        search_from = hit + label_len;
        size_t i = hit - comment;

        if ((i > 0 && isalnum ((unsigned char)comment[i-1])) || isalnum ((unsigned char)hit[label_len]))
            continue;

        size_t j = skipLabelSeparator (comment, i + label_len);
        size_t n_pfx = 0;
        while (isalpha ((unsigned char)comment[j+n_pfx]) && n_pfx < 4)
            n_pfx++;

        char dash_form[16];
        bool got = false;

        if (n_pfx >= 1 && n_pfx <= 3) {
            // form A: letters, 1-2 dashes, digits
            size_t k = j + n_pfx;
            size_t n_dash = 0;
            while (comment[k] == '-' && n_dash < 2) { k++; n_dash++; }
            size_t d0 = k;
            while (isdigit ((unsigned char)comment[k]) && k - d0 < 6)
                k++;
            size_t n_dig = k - d0;
            // the dash is optional when the label is right here (eg "POTA es1885"), but then
            // insist on 4+ digits so things like "POTA FT8" or "POTA k59" can't match
            if (((n_dash >= 1 && n_dig >= 3) || (n_dash == 0 && n_dig >= 4)) && n_dig <= 5
                                && !isalnum ((unsigned char)comment[k])) {
                snprintf (dash_form, sizeof(dash_form), "%.*s-%.*s", (int)n_pfx, comment+j, (int)n_dig, comment+d0);
                got = true;
            }
        } else if (n_pfx == 0) {
            // form B: bare digits, borrow prefix from preceding word if it looks like one
            size_t k = j;
            while (isdigit ((unsigned char)comment[k]) && k - j < 6)
                k++;
            size_t n_dig = k - j;
            if (n_dig >= 4 && n_dig <= 5 && !isalnum ((unsigned char)comment[k])) {
                // preceding word: skip spaces back from the label, then collect letters
                size_t e = i;
                while (e > 0 && comment[e-1] == ' ')
                    e--;
                size_t b = e;
                while (b > 0 && isalpha ((unsigned char)comment[b-1]))
                    b--;
                size_t n_p = e - b;
                bool pfx_ok = (n_p >= 1 && n_p <= 3 && (b == 0 || !isalnum ((unsigned char)comment[b-1]))
                                        && (b == 0 || comment[b-1] != '/'));
                if (pfx_ok) {
                    char w[4];
                    for (size_t q = 0; q < n_p; q++)
                        w[q] = toupper ((unsigned char)comment[b+q]);
                    w[n_p] = '\0';
                    for (const char *np : not_prefix)
                        if (!strcmp (np, w))
                            pfx_ok = false;
                    if (pfx_ok)
                        snprintf (dash_form, sizeof(dash_form), "%s-%.*s", w, (int)n_dig, comment+j);
                }
                if (!pfx_ok)
                    snprintf (dash_form, sizeof(dash_form), "%.*s", (int)n_dig, comment+j);
                got = true;
            }
        }

        if (got && strlen (dash_form) < ref_len) {
            for (size_t q = 0; (ref[q] = toupper ((unsigned char)dash_form[q])) != '\0'; q++)
                continue;
            return (true);
        }
    }
    return (false);
}

/* parse a SOTA summit reference starting exactly at p: association (1-3 alnum, eg "LA", "W7A", "9A"),
 * then '/' or a single space, then 2-letter region, optional dash, 3 digits, eg "LA/HM-226",
 * "LA HM226", "W7A/CE-001". must not be glued to more alnum after. on success write the normalized
 * "ASSOC/RG-NNN" form to ref and return true.
 */
static bool parseSOTARef (const char *p, bool need_slash, char *ref, size_t ref_len)
{
    size_t a = 0;
    while (isalnum ((unsigned char)p[a]) && a < 4)
        a++;
    if (a < 1 || a > 3)
        return (false);
    size_t j = a;
    if (p[j] == '/')
        j++;
    else if (!need_slash && p[j] == ' ')
        j++;
    else
        return (false);
    if (!isalpha ((unsigned char)p[j]) || !isalpha ((unsigned char)p[j+1]))
        return (false);
    const char *rg = p + j;
    j += 2;
    if (p[j] == '-')
        j++;
    if (!isdigit ((unsigned char)p[j]) || !isdigit ((unsigned char)p[j+1]) || !isdigit ((unsigned char)p[j+2])
                    || isalnum ((unsigned char)p[j+3]))
        return (false);
    char tmp[16];
    snprintf (tmp, sizeof(tmp), "%.*s/%.2s-%.3s", (int)a, p, rg, p+j);
    if (strlen (tmp) >= ref_len)
        return (false);
    for (size_t k = 0; (ref[k] = toupper ((unsigned char)tmp[k])) != '\0'; k++)
        continue;
    return (true);
}

/* SOTA: "SOTA <assoc> <region><nnn>" etc after the word SOTA, or a self-identifying
 * "ASSOC/RG-NNN" anywhere (eg "LA/HM-226"; the slash form is distinctive enough to trust unlabeled).
 */
static bool findSOTAShape (const char *comment, char *ref, size_t ref_len)
{
    const char *search_from = comment;
    const char *hit;
    while ((hit = strcistr (search_from, "SOTA")) != NULL) {
        search_from = hit + 4;
        size_t i = hit - comment;
        if ((i > 0 && isalnum ((unsigned char)comment[i-1])) || isalnum ((unsigned char)hit[4]))
            continue;
        size_t j = skipLabelSeparator (comment, i + 4);
        if (parseSOTARef (comment + j, false, ref, ref_len))
            return (true);
    }
    for (size_t i = 0; comment[i]; i++) {
        if (!isalnum ((unsigned char)comment[i]) || (i > 0 && (isalnum ((unsigned char)comment[i-1])
                                || comment[i-1] == '/' || comment[i-1] == '-')))
            continue;
        if (parseSOTARef (comment + i, true, ref, ref_len))
            return (true);
    }
    return (false);
}

/* WWFF references are self-identifying: a 2-5 letter program prefix ending in "FF", a dash, and 4
 * digits, eg "KFF-1234", "ONFF-0707", "EAFF-0071", "FF-0001" -- so like WWBOTA they are trusted
 * without needing a "WWFF" label nearby. Must stand alone (not glued to other alnum chars).
 */
static bool findWWFFShape (const char *comment, char *ref, size_t ref_len)
{
    size_t len = strlen (comment);
    for (size_t i = 0; i < len; i++) {
        if (!isalpha ((unsigned char)comment[i]))
            continue;
        if (i > 0 && (isalnum ((unsigned char)comment[i-1]) || comment[i-1] == '/' || comment[i-1] == '-'))
            continue;

        size_t j = i;
        while (isalpha ((unsigned char)comment[j]) && j - i < 6)
            j++;
        size_t n_pfx = j - i;
        if (n_pfx < 2 || n_pfx > 5 || comment[j] != '-')
            continue;
        if (toupper((unsigned char)comment[j-1]) != 'F' || toupper((unsigned char)comment[j-2]) != 'F')
            continue;
        j++;                                                // past the dash
        size_t d0 = j;
        while (isdigit ((unsigned char)comment[j]) && j - d0 < 6)
            j++;
        size_t n_dig = j - d0;
        if (n_dig != 4 || isalnum ((unsigned char)comment[j]) || comment[j] == '-' || comment[j] == '/')
            continue;

        size_t n = j - i;
        if (n >= ref_len)
            continue;
        for (size_t k = 0; k < n; k++)
            ref[k] = toupper ((unsigned char)comment[i+k]);
        ref[n] = '\0';
        return (true);
    }
    return (false);
}

/* last-resort POTA detection for comments that give a bare park reference with no "POTA" label,
 * eg "US-6954 8313", "K-1234", "DL-01063". Shape: 1-3 letters, ONE dash, 4-5 digits, standing alone
 * (not glued to other alnum chars or a '/'). Only called after every label-anchored scan has failed,
 * so a labeled WCA/ARLHS/etc reference in the same comment always wins over this guess. WWFF
 * references ("KFF-1234", "FF-0001") have the same shape, so any prefix ending in "FF" is rejected.
 * IOTA ("EU-005") is only 3 digits so is not confused, and findIOTARef() runs first anyway.
 */
static bool findBarePOTAShape (const char *comment, char *ref, size_t ref_len)
{
    size_t len = strlen (comment);
    for (size_t i = 0; i < len; i++) {
        if (!isalpha ((unsigned char)comment[i]))
            continue;
        if (i > 0 && (isalnum ((unsigned char)comment[i-1]) || comment[i-1] == '/' || comment[i-1] == '-'))
            continue;

        size_t j = i;
        while (isalpha ((unsigned char)comment[j]) && j - i < 4)
            j++;
        size_t n_pfx = j - i;
        if (n_pfx > 3 || comment[j] != '-')
            continue;
        if (n_pfx >= 2 && toupper((unsigned char)comment[j-1]) == 'F' && toupper((unsigned char)comment[j-2]) == 'F')
            continue;                                       // WWFF, not POTA
        j++;                                                // past the dash
        size_t d0 = j;
        while (isdigit ((unsigned char)comment[j]) && j - d0 < 6)
            j++;
        size_t n_dig = j - d0;
        if (n_dig < 4 || n_dig > 5 || isalnum ((unsigned char)comment[j]) || comment[j] == '-' || comment[j] == '/')
            continue;

        size_t n = j - i;
        if (n >= ref_len)
            continue;
        for (size_t k = 0; k < n; k++)
            ref[k] = toupper ((unsigned char)comment[i+k]);
        ref[n] = '\0';
        return (true);
    }
    return (false);
}

/* see xota.h
 */
bool findXOTARef (const char *comment, char *org, size_t org_len, char *ref, size_t ref_len)
{
    if (!comment || org_len < 8 || ref_len < 12)
        return (false);

    if (findWWBOTAShape (comment, ref, ref_len)) {
        quietStrncpy (org, "WWBOTA", org_len);
        return (true);
    }

    // POTA has its own live feed in the ONTA pane, but DX Cluster spots only ever carry it as
    // free text in the comment, so it gets a dedicated (stricter-than-generic) scan here
    if (findPOTAShape (comment, ref, ref_len)) {
        quietStrncpy (org, "POTA", org_len);
        return (true);
    }

    if (findSOTAShape (comment, ref, ref_len)) {
        quietStrncpy (org, "SOTA", org_len);
        return (true);
    }

    // WWFF: self-identifying shape, checked after an explicitly labeled POTA so a comment naming
    // both programs still reports POTA
    if (findWWFFShape (comment, ref, ref_len)) {
        quietStrncpy (org, "WWFF", org_len);
        return (true);
    }

    for (const XOTALabel &xl : xota_labels) {

        size_t label_len = strlen (xl.label);
        const char *search_from = comment;

        // a given label might appear more than once in weird comments (rare); only the
        // first occurrence with a plausible reference following it is used
        const char *hit;
        while ((hit = strcistr (search_from, xl.label)) != NULL) {

            size_t i = hit - comment;

            // must not be glued to more alnum chars on either side, else eg "SIOTATION"
            // (not a real word, but the spirit) could false-match
            bool left_ok  = (i == 0) || !isalnum ((unsigned char)comment[i-1]);
            bool right_ok = !isalnum ((unsigned char)hit[label_len]);
            if (left_ok && right_ok) {

                size_t ref_start = skipLabelSeparator (comment, i + label_len);

                // capture a contiguous run of reference-shaped characters
                char candidate[16];
                size_t n = 0;
                while (comment[ref_start + n] && isRefChar (comment[ref_start + n])
                                && n + 1 < sizeof(candidate)) {
                    candidate[n] = comment[ref_start + n];
                    n++;
                }
                // trim a dangling separator left at the end, eg captured "DL-" with
                // nothing after because the comment ended or hit whitespace mid-token
                while (n > 0 && (candidate[n-1] == '-' || candidate[n-1] == '/'))
                    n--;
                candidate[n] = '\0';

                // require something plausible: at least one digit (every one of these
                // programs' reference schemes ends in a number) and reasonable length
                bool has_digit = false;
                for (size_t k = 0; k < n; k++)
                    if (isdigit ((unsigned char)candidate[k]))
                        has_digit = true;

                if (n >= 2 && n < ref_len && has_digit) {
                    quietStrncpy (org, xl.org, org_len);
                    for (size_t k = 0; k < n; k++)
                        ref[k] = toupper ((unsigned char)candidate[k]);
                    ref[n] = '\0';
                    return (true);
                }
            }

            // try again in case this occurrence was a false boundary match or had no
            // usable reference following it -- move just past this hit
            search_from = hit + label_len;
        }
    }

    // no labeled match anywhere: a bare "US-6954"-shaped token is most likely a POTA park
    if (findBarePOTAShape (comment, ref, ref_len)) {
        quietStrncpy (org, "POTA", org_len);
        return (true);
    }

    return (false);
}
