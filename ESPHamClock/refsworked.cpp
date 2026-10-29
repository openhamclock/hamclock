/* the set of On The Air references (parks, summits, ...) the operator has already worked, so the spot panes can
 * tell a new reference from one already worked.
 *
 * a reference is identified by program and id together, eg "POTA" "US-1234" or "SOTA" "W7A/AN-011". this answers
 * "have I ever worked this reference?" not "have I worked it on this band?" because hunter awards count a
 * reference once.
 *
 * references come from more than one place and each is loaded and reloaded independently, so they are kept in
 * separate sets keyed by RWSource and a lookup consults all of them:
 *   RW_ADIF       from POTA_REF, SOTA_REF, WWFF_REF or SIG/SIG_INFO in the ADIF log, see adif_parser.cpp
 *   RW_POTAHUNTED from the Hunted Parks CSV downloaded from pota.app, see huntedparks.cpp
 *
 * N.B. this module knows nothing about file syntax, its callers feed it normalized-or-not strings.
 */

#include "HamClock.h"

#define RW_KEY_LEN      32                              // "ORG:REF" including EOS

typedef struct {
    char key[RW_KEY_LEN];
} RWEntry;

typedef struct {
    RWEntry *list;                                      // malloced
    int n, n_malloc;
    bool sorted;                                        // false after any add until next lookup
} RWSet;

static RWSet rw_sets[RW_N] = { {NULL,0,0,true}, {NULL,0,0,true} };


/* build the canonical key for a program and reference id: upper case, no blanks, no "@region" suffix.
 * POTA's original "K-1234" US prefix and its current "US-1234" name the same park so we fold the former into the
 * latter; without this a log made before the change would show every US park as new.
 * return false if either is empty or too long.
 */
static bool makeKey (const char *org, const char *ref, char key[RW_KEY_LEN])
{
    if (!org || !ref)
        return (false);

    char o[12], r[RW_KEY_LEN];
    size_t no = 0, nr = 0;
    for (const char *p = org; *p && no < sizeof(o)-1; p++)
        if (!isspace ((unsigned char)*p))
            o[no++] = toupper ((unsigned char)*p);
    o[no] = '\0';
    for (const char *p = ref; *p && *p != '@' && nr < sizeof(r)-4; p++)     // room for K- -> US- growth
        if (!isspace ((unsigned char)*p))
            r[nr++] = toupper ((unsigned char)*p);
    r[nr] = '\0';
    if (no == 0 || nr == 0)
        return (false);

    if (strcmp (o, "POTA") == 0 && strncmp (r, "K-", 2) == 0) {
        memmove (r + 3, r + 2, nr - 1);                 // include EOS
        memcpy (r, "US-", 3);
        nr++;
    }

    if (no + 1 + nr + 1 > RW_KEY_LEN)
        return (false);
    snprintf (key, RW_KEY_LEN, "%s:%s", o, r);
    return (true);
}

static int qsRWCmp (const void *a, const void *b)
{
    return (strcmp (((const RWEntry*)a)->key, ((const RWEntry*)b)->key));
}

/* sort and remove duplicates if anything was added to s since last time
 */
static void settle (RWSet &s)
{
    if (s.sorted)
        return;
    qsort (s.list, s.n, sizeof(RWEntry), qsRWCmp);
    int j = 0;
    for (int i = 0; i < s.n; i++)
        if (j == 0 || strcmp (s.list[j-1].key, s.list[i].key) != 0)
            s.list[j++] = s.list[i];
    s.n = j;
    s.sorted = true;
}

/* return whether key is in s
 */
static bool inSet (RWSet &s, const char *key)
{
    settle (s);
    RWEntry k;
    strcpy (k.key, key);
    return (bsearch (&k, s.list, s.n, sizeof(RWEntry), qsRWCmp) != NULL);
}

/* return whether any key in s starts with prefix, which includes the colon
 */
static bool anyWithPrefix (RWSet &s, const char *prefix, int pl)
{
    settle (s);
    int lo = 0, hi = s.n;
    while (lo < hi) {
        int mid = (lo + hi)/2;
        if (strcmp (s.list[mid].key, prefix) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < s.n && strncmp (s.list[lo].key, prefix, pl) == 0);
}

/* forget everything that came from the given source, leaving other sources alone
 */
void resetRefsWorked (RWSource src)
{
    rw_sets[src].n = 0;
    rw_sets[src].sorted = true;
}

/* record that the reference has been worked. harmless to repeat.
 */
void addRefWorked (RWSource src, const char *org, const char *ref)
{
    char key[RW_KEY_LEN];
    if (!makeKey (org, ref, key))
        return;

    RWSet &s = rw_sets[src];
    if (s.n + 1 > s.n_malloc) {
        s.list = (RWEntry *) realloc (s.list, (s.n_malloc += 256) * sizeof(RWEntry));
        if (!s.list)
            fatalError ("No memory for %d worked references", s.n_malloc);
    }
    strcpy (s.list[s.n++].key, key);
    s.sorted = false;
}

/* add every reference in a comma or semicolon separated list as found in ADIF POTA_REF, eg a two-fer
 * "US-1234@US-NC,US-5678@US-NC". any one of them worked counts for that one.
 */
void addRefsWorkedList (RWSource src, const char *org, const char *list)
{
    char buf[80];
    quietStrncpy (buf, list, sizeof(buf));
    char *save = NULL;
    for (char *tok = strtok_r (buf, ",;", &save); tok; tok = strtok_r (NULL, ",;", &save))
        addRefWorked (src, org, tok);
}

/* return whether the reference is known from any source. an empty or malformed id is never "worked".
 */
bool isRefWorked (const char *org, const char *ref)
{
    loadHuntedParksOnce();

    char key[RW_KEY_LEN];
    if (!makeKey (org, ref, key))
        return (false);
    for (int i = 0; i < RW_N; i++)
        if (inSet (rw_sets[i], key))
            return (true);
    return (false);
}

/* how many distinct references are known from all sources, for status displays
 */
int countRefsWorked (void)
{
    loadHuntedParksOnce();

    int n = 0;
    for (int i = 0; i < RW_N; i++) {
        settle (rw_sets[i]);
        n += rw_sets[i].n;
    }
    // a reference in more than one source was counted twice: discount those
    for (int i = 0; i < rw_sets[RW_ADIF].n; i++)
        if (inSet (rw_sets[RW_POTAHUNTED], rw_sets[RW_ADIF].list[i].key))
            n--;
    return (n);
}

/* how many references one source holds
 */
int countRefsWorked (RWSource src)
{
    loadHuntedParksOnce();
    settle (rw_sets[src]);
    return (rw_sets[src].n);
}

/* return whether any source holds at least one worked reference for the given program. needed because a log may
 * well carry POTA references but nothing for, say, SOTA, and then "not worked" would be meaningless for SOTA.
 */
bool orgHasRefsWorked (const char *org)
{
    loadHuntedParksOnce();

    char prefix[16];
    int pl = snprintf (prefix, sizeof(prefix), "%.10s:", org);
    for (int i = 0; i < pl; i++)
        prefix[i] = toupper ((unsigned char)prefix[i]);
    for (int i = 0; i < RW_N; i++)
        if (anyWithPrefix (rw_sets[i], prefix, pl))
            return (true);
    return (false);
}
