/* the POTA "Hunted Parks" CSV: which parks the operator has hunted, for flagging new parks in the spot panes.
 *
 * pota.app has no hunter ADIF export but its stats page offers a CSV of hunted parks. the user downloads that and
 * uploads it here, from a browser, via parks.html; it is kept in our_dir and read back each time HamClock starts.
 * the parks go into refsworked.cpp as source RW_POTAHUNTED.
 *
 * the file is parsed forgivingly: the park reference is taken from the column headed "Reference" if the file has
 * a header row, else any field shaped like a park reference (US-0231, VE-0123, K-1234) on any row. we do not rely on
 * any other column. the CSV for activated parks has the same layout but means something else, so that one is
 * recognized by its "Attempts" and "Activations" columns and refused rather than counted as hunted.
 */

#include "HamClock.h"

#define HP_FILE         "hunted-parks.csv"
#define HP_MAX_BYTES    (4L*1024*1024)                  // a lifetime of hunting is a few 100 kB at most


/* full path of the saved file
 */
static void hpPath (char *buf, size_t buf_len)
{
    snprintf (buf, buf_len, "%s/%s", our_dir.c_str(), HP_FILE);
}

/* return whether s looks like a POTA reference: 1-3 letters, dash, 3-6 digits. eg US-0231
 */
static bool looksLikePark (const char *s)
{
    int nl = 0;
    while (isalpha ((unsigned char)s[nl]))
        nl++;
    if (nl < 1 || nl > 3 || s[nl] != '-')
        return (false);
    int nd = 0;
    const char *d = s + nl + 1;
    while (isdigit ((unsigned char)d[nd]))
        nd++;
    return (nd >= 3 && nd <= 6 && d[nd] == '\0');
}

/* split one CSV record starting at p (up to but excluding the end of buffer or line) into fields, handling
 * quotes, doubled quotes inside quotes, and CRLF. fields are copied into f[][] up to max_f of f_len each.
 * return pointer just past the end of the record, and the number of fields in *nf.
 */
#define HP_MAXFIELDS    16
#define HP_FIELDLEN     64
static const char *csvRecord (const char *p, const char *end, char f[][HP_FIELDLEN], int *nf)
{
    int n = 0, l = 0;
    bool inq = false;
    f[0][0] = '\0';
    while (p < end) {
        char c = *p++;
        if (inq) {
            if (c == '"') {
                if (p < end && *p == '"') {             // "" is a literal quote
                    if (l < HP_FIELDLEN-1) { f[n][l++] = '"'; f[n][l] = '\0'; }
                    p++;
                } else
                    inq = false;
            } else if (l < HP_FIELDLEN-1) {
                f[n][l++] = c;
                f[n][l] = '\0';
            }
        } else if (c == '"') {
            inq = true;
        } else if (c == ',') {
            if (n < HP_MAXFIELDS-1) {
                n++;
                l = 0;
                f[n][0] = '\0';
            }
        } else if (c == '\n') {
            break;
        } else if (c != '\r' && l < HP_FIELDLEN-1) {
            f[n][l++] = c;
            f[n][l] = '\0';
        }
    }
    *nf = n + 1;
    return (p);
}

/* parse a whole CSV held in buf, calling add for each park found if add is true.
 * pass back whether the header looks like the Activated Parks export.
 * return number of park references found.
 */
static int parseHuntedCSV (const char *buf, size_t len, bool add, bool &looks_activated)
{
    looks_activated = false;
    const char *p = buf;
    const char *end = buf + len;

    // skip a UTF-8 byte order mark which some spreadsheet exports start with
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;

    char f[HP_MAXFIELDS][HP_FIELDLEN];
    int refcol = -1;                                    // column headed Reference, once header seen
    bool header_done = false;
    int n_parks = 0;

    while (p < end) {
        int nf;
        p = csvRecord (p, end, f, &nf);

        // header is the first record that names a Reference column
        if (!header_done) {
            bool has_att = false, has_act = false;
            for (int i = 0; i < nf; i++) {
                if (strcasecmp (f[i], "Reference") == 0)
                    refcol = i;
                else if (strcasecmp (f[i], "Attempts") == 0)
                    has_att = true;
                else if (strcasecmp (f[i], "Activations") == 0)
                    has_act = true;
            }
            if (refcol >= 0) {
                header_done = true;
                looks_activated = has_att && has_act;
                continue;                               // the header row itself holds no park
            }
        }

        if (refcol >= 0) {
            if (refcol < nf && looksLikePark (f[refcol])) {
                n_parks++;
                if (add)
                    addRefWorked (RW_POTAHUNTED, "POTA", f[refcol]);
            }
        } else {
            // no header seen (yet): take every park-shaped field
            for (int i = 0; i < nf; i++) {
                if (looksLikePark (f[i])) {
                    n_parks++;
                    if (add)
                        addRefWorked (RW_POTAHUNTED, "POTA", f[i]);
                }
            }
        }
    }
    return (n_parks);
}

/* read whole file into a malloced buffer, return NULL if none or too big
 */
static char *slurp (const char *path, size_t &len)
{
    FILE *fp = fopen (path, "r");
    if (!fp)
        return (NULL);
    char *buf = NULL;
    size_t n = 0, cap = 0;
    char chunk[4096];
    size_t nr;
    while ((nr = fread (chunk, 1, sizeof(chunk), fp)) > 0) {
        if (n + nr > (size_t) HP_MAX_BYTES) {
            free (buf);
            fclose (fp);
            return (NULL);
        }
        if (n + nr > cap) {
            cap = (n + nr) * 2;
            buf = (char *) realloc (buf, cap);
            if (!buf)
                fatalError ("No memory for hunted parks");
        }
        memcpy (buf + n, chunk, nr);
        n += nr;
    }
    fclose (fp);
    len = n;
    return (buf);
}

/* replace the hunted parks in memory with those in the saved file, if any. return count.
 */
static int loadHuntedParksFile (void)
{
    resetRefsWorked (RW_POTAHUNTED);
    char path[1000];
    hpPath (path, sizeof(path));
    size_t len = 0;
    char *buf = slurp (path, len);
    if (!buf)
        return (0);
    bool activated;
    int n = parseHuntedCSV (buf, len, true, activated);
    free (buf);
    Serial.printf ("PARKS: loaded %d hunted parks from %s\n", n, path);
    return (n);
}

/* read the saved file the first time anyone asks about references. cheap to call repeatedly.
 */
void loadHuntedParksOnce (void)
{
    static bool tried;
    if (!tried) {
        tried = true;
        (void) loadHuntedParksFile();
    }
}

/* accept an uploaded Hunted Parks CSV of n_bytes from gr, keep it if it holds parks and make it current.
 * pass back the number of parks found. if not accepted, ynot says why and the previous file is left as it was.
 */
bool uploadHuntedParks (GenReader &gr, long n_bytes, int &n_parks, Message &ynot)
{
    n_parks = 0;

    if (n_bytes <= 0) {
        ynot.set ("Content-Length required");
        return (false);
    }
    if (n_bytes > HP_MAX_BYTES) {
        ynot.printf ("file too large, max %ld MB", HP_MAX_BYTES/(1024*1024));
        return (false);
    }

    // take it all into memory: it is small and we need it twice
    char *buf = (char *) malloc (n_bytes);
    if (!buf) {
        ynot.set ("no memory");
        return (false);
    }
    long n_in = 0;
    char c;
    while (n_in < n_bytes && gr.getChar (&c))
        buf[n_in++] = c;
    if (n_in < n_bytes) {
        free (buf);
        ynot.set ("upload was cut short");
        return (false);
    }

    // must hold parks, and must not be the activated parks file
    bool activated;
    n_parks = parseHuntedCSV (buf, n_in, false, activated);
    if (activated) {
        free (buf);
        n_parks = 0;
        ynot.set ("that is the Activated Parks file: on pota.app choose the Hunted Parks tab, then Export CSV");
        return (false);
    }
    if (n_parks == 0) {
        free (buf);
        ynot.set ("no park references found: use the Hunted Parks Export CSV from pota.app");
        return (false);
    }

    // good: save then load
    char path[1000], tmp[1010];
    hpPath (path, sizeof(path));
    snprintf (tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen (tmp, "w");
    bool ok = fp && fwrite (buf, 1, n_in, fp) == (size_t) n_in;
    if (fp)
        ok = (fclose (fp) == 0) && ok;
    free (buf);
    if (!ok) {
        remove (tmp);
        ynot.printf ("can not save file: %s", strerror (errno));
        n_parks = 0;
        return (false);
    }
    if (rename (tmp, path) < 0) {
        remove (tmp);
        ynot.printf ("can not install file: %s", strerror (errno));
        n_parks = 0;
        return (false);
    }

    loadHuntedParksOnce();                              // so it will not read the file again behind our back
    n_parks = loadHuntedParksFile();
    return (true);
}

/* write a few lines about the hunted parks into buf, in the style of the get_*.txt commands
 */
void getHuntedParksStatus (char *buf, size_t buf_len)
{
    loadHuntedParksOnce();
    snprintf (buf, buf_len, "POTA_hunted_parks  %d\n", countRefsWorked (RW_POTAHUNTED));
}
