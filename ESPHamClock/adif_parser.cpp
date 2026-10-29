/* ADIF parser and GenReader reader
 *
 * DEBUG_ADIF guidelines:
 *   level 1: log each successful spot, no errors
 *   level 2: 1+ errors in final check
 *   level 3: 2+ all else
 */

#include "HamClock.h"



typedef enum {
    ADIFPS_STARTFILE,                                   // initialize all
    ADIFPS_STARTSPOT,                                   // initialize parser and spot candidate
    ADIFPS_STARTFIELD,                                  // initialize parser for next field,retain spot so far
    ADIFPS_SEARCHING,                                   // looking for opening <
    ADIFPS_INNAME,                                      // after < collecting field name until :
    ADIFPS_INLENGTH,                                    // after first : building value_len until : or >
    ADIFPS_INTYPE,                                      // after second : skipping type until >
    ADIFPS_INVALUE,                                     // after > now collecting value
    ADIFPS_FINISHED,                                    // spot is complete
    ADIFPS_SKIPTOEOR,                                   // skip to EOR after finding an error
} ADIFParseState;

typedef enum {
    AFB_BAND,
    AFB_CALL,
    AFB_DXCC,
    AFB_CONTACTED_OP,
    AFB_FREQ,
    AFB_GRIDSQUARE,
    AFB_LAT,
    AFB_LON,
    AFB_MODE,
    AFB_MY_GRIDSQUARE,
    AFB_MY_LAT,
    AFB_MY_LON,
    AFB_MY_DXCC,
    AFB_OPERATOR,
    AFB_QSO_DATE,
    AFB_STATION_CALLSIGN,
    AFB_TIME_ON,
} ADIFFieldBit;

typedef struct {
    // running state
    ADIFParseState ps;                                  // what is happening now
    int line_n;                                         // line number for diagnostics

    // per-field state
    char name[20];                                      // field name so far, always includes EOS
    char value[48];                                     // field value so far, always includes EOS. N.B. room for a
                                                        // POTA two-fer list in POTA_REF
    unsigned name_seen;                                 // n name chars seen so far (avoids strlen(name))
    unsigned value_len;                                 // claimed value length so far from field defn
    unsigned value_seen;                                // n value chars seen so far (avoids strlen(value))

    // per-spot state
    uint32_t fields;                                    // bit mask of 1 << ADIFFieldBit seen
    char qso_date[10];                                  // temp QSO_DATE .. need both to get UNIX time
    char time_on[10];                                   // temp TIME_ON .. need both to get UNIX time

    // On The Air references of the station worked, kept until the record is known good then given to refsworked.
    // "MY_" variants name the operator's own location and are different fields so are never confused with these.
    char pota_ref[48], sota_ref[48], wwff_ref[48];      // POTA_REF SOTA_REF WWFF_REF, may be comma lists
    char sig[12], sig_info[24];                         // generic SIG and SIG_INFO, eg "LLOTA" "xx-nnn"
} ADIFParser;

#define CHECK_AFB(a,b)   ((a).fields & (1 << (b)))      // handy test for ADIFFieldBit
#define ADD_AFB(a,b)     ((a).fields |= (1 << (b)))     // handy way to add one ADIFFieldBit


// YYYYMMDD HHMM[SS]
static bool parseDT2UNIX (const char *date, const char *tim, time_t &unix)
{
    int yr, mo, dd, hh, mm, ss = 0;
    if (sscanf (date, "%4d%2d%2d", &yr, &mo, &dd) != 3 || sscanf (tim, "%2d%2d%2d", &hh, &mm, &ss) < 2) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: parseDT2UNIX(%s, %s) failed\n", date, tim);
        return (false);
    }

    tmElements_t tm;
    tm.Year = yr - 1970;                                // 1970-based
    tm.Month = mo;                                      // 1-based
    tm.Day = dd;                                        // 1-based
    tm.Hour = hh;
    tm.Minute = mm;
    tm.Second = ss;
    unix = makeTime(tm);

    if (debugLevel (DEBUG_ADIF, 3))
        Serial.printf ("ADIF: spotted %s %s -> %ld\n", date, tim, (long)unix);

    return (true);
}



typedef struct {
    const char *name;
    float MHz;
} ADIFBand;

/* convert BAND ADIF enumeration to typical frequency in kHz.
 * N.B. return whether recognized as one supported by HamClock
 */
static bool parseADIFBand (const char *band, float &kHz)
{
    // https://www.adif.org/315/ADIF_315.htm#Band_Enumeration
    static ADIFBand bands[] = {
        { "2190m",       0.1357	},
        { "630m",        0.472	},
        { "560m",        0.501	},
        { "160m",        1.8	},
        { "80m",         3.5	},
        { "60m",         5.36	},
        { "40m",         7.0	},
        { "30m",        10.1	},
        { "20m",        14.0	},
        { "17m",        18.068	},
        { "15m",        21.0	},
        { "12m",        24.890	},
        { "10m",        28.0	},
        { "8m",         40	},
        { "6m",         50	},
        { "5m",         54	},
        { "4m",         70	},
        { "2m",        144	},
        { "1.25m",     222	},
        { "70cm",      420	},
        { "33cm",      902	},
        { "23cm",     1240	},
        { "13cm",     2300	},
        { "9cm",      3300	},
        { "6cm",      5650	},
        { "3cm",     10000	},
        { "1.25cm",  24000	},
        { "6mm",     47000	},
        { "4mm",     75500	},
        { "2.5mm",  119980	},
        { "2mm",    134000	},
        { "1mm",    241000	},
        { "submm",  300000	},
    };

    for (int i = 0; i < NARRAY(bands); i++) {
        if (strcasecmp (band, bands[i].name) == 0) {
            kHz = 1e3 * bands[i].MHz;
            if (findHamBand (kHz) != HAMBAND_NONE)
                return (true);
        }
    }
    return (false);
}


/* crack a lat/long location of the form XDDD MM.MMM to degrees +N +E.
 */
static bool parseADIFLocation (const char *loc, float &degs)
{
    char dir;
    int deg;
    float min;
    if (sscanf (loc, "%c%d %f", &dir, &deg, &min) != 3)
        return (false);

    degs = deg + min/60;
    if (tolower(dir)=='w' || tolower(dir) == 's')
        degs = -degs;

    return (true);
}

/* add a completed ADIF name/value pair to spot and update fields mask if qualifies.
 * return false if outright syntax error.
 * N.B. within spot we assign "my" fields to be rx, the "other" guy to be tx.
 */
static bool addADIFFIeld (ADIFParser &adif, DXSpot &spot)
{
    // false if fall thru all the tests
    bool useful_field = true;

    if (!strcasecmp (adif.name, "OPERATOR")) {
        ADD_AFB (adif, AFB_OPERATOR);
        quietStrncpy (spot.rx_call, adif.value, sizeof(spot.rx_call));


    } else if (!strcasecmp (adif.name, "STATION_CALLSIGN")) {
        ADD_AFB (adif, AFB_STATION_CALLSIGN);
        quietStrncpy (spot.rx_call, adif.value, sizeof(spot.rx_call));


    } else if (!strcasecmp (adif.name, "MY_GRIDSQUARE")) {
        if (!maidenhead2ll (spot.rx_ll, adif.value)) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus MY_GRIDSQUARE %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_MY_GRIDSQUARE);
        quietStrncpy (spot.rx_grid, adif.value, sizeof(spot.rx_grid));


    } else if (!strcasecmp (adif.name, "MY_LAT")) {
        if (!parseADIFLocation (adif.value, spot.rx_ll.lat_d)
                                        || spot.rx_ll.lat_d < -90 || spot.rx_ll.lat_d > 90) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus MY_LAT %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_MY_LAT);

    } else if (!strcasecmp (adif.name, "MY_LON")) {
        if (!parseADIFLocation (adif.value, spot.rx_ll.lng_d)
                                        || spot.rx_ll.lng_d < -180 || spot.rx_ll.lng_d > 180) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus MY_LON %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_MY_LON);

    } else if (!strcasecmp (adif.name, "MY_DXCC")) {
        ADD_AFB (adif, AFB_MY_DXCC);
        spot.rx_dxcc = atoi (adif.value);

    } else if (!strcasecmp (adif.name, "DXCC")) {
        ADD_AFB (adif, AFB_DXCC);
        spot.tx_dxcc = atoi (adif.value);

    } else if (!strcasecmp (adif.name, "CALL")) {
        ADD_AFB (adif, AFB_CALL);
        quietStrncpy (spot.tx_call, adif.value, sizeof(spot.tx_call));


    } else if (!strcasecmp (adif.name, "CONTACTED_OP")) {
        ADD_AFB (adif, AFB_CONTACTED_OP);
        quietStrncpy (spot.tx_call, adif.value, sizeof(spot.tx_call));


    } else if (!strcasecmp (adif.name, "QSO_DATE")) {
        if (CHECK_AFB (adif, AFB_TIME_ON)) {
            if (!parseDT2UNIX (adif.value, adif.time_on, spot.spotted)) {
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d bogus QSO_DATE %s or TIME_ON %s\n", adif.line_n,
                                                adif.value, adif.time_on);
                return (false);
            }
        }
        ADD_AFB (adif, AFB_QSO_DATE);
        quietStrncpy (adif.qso_date, adif.value, sizeof(adif.qso_date));


    } else if (!strcasecmp (adif.name, "TIME_ON")) {
        if (CHECK_AFB (adif, AFB_QSO_DATE)) {
            if (!parseDT2UNIX (adif.qso_date, adif.value, spot.spotted)) {
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d bogus TIME_ON %s or QSO_DATE %s\n", adif.line_n,
                                                adif.value, adif.qso_date);
                return (false);
            }
        }
        ADD_AFB (adif, AFB_TIME_ON);
        quietStrncpy (adif.time_on, adif.value, sizeof(adif.time_on));


    } else if (!strcasecmp (adif.name, "BAND")) {
        // don't use BAND if FREQ already set
        if (!CHECK_AFB (adif, AFB_FREQ) && !parseADIFBand (adif.value, spot.kHz)) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d unknown or unsupported band %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_BAND);


    } else if (!strcasecmp (adif.name, "FREQ")) {
        spot.kHz = 1e3 * atof(adif.value); // ADIF stores MHz
        if (findHamBand (spot.kHz) == HAMBAND_NONE) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus FREQ %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_FREQ);


    } else if (!strcasecmp (adif.name, "MODE")) {
        ADD_AFB (adif, AFB_MODE);
        quietStrncpy (spot.mode, adif.value, sizeof(spot.mode));


    } else if (!strcasecmp (adif.name, "GRIDSQUARE")) {
        if (!maidenhead2ll (spot.tx_ll, adif.value)) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus GRIDSQUARE %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_GRIDSQUARE);

        quietStrncpy (spot.tx_grid, adif.value, sizeof(spot.tx_grid));


    } else if (!strcasecmp (adif.name, "LAT")) {
        if (!parseADIFLocation (adif.value, spot.tx_ll.lat_d)
                                        || spot.tx_ll.lat_d < -90 || spot.tx_ll.lat_d > 90) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus LAT %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_LAT);


    } else if (!strcasecmp (adif.name, "LON")) {
        if (!parseADIFLocation (adif.value, spot.tx_ll.lng_d)
                                        || spot.tx_ll.lng_d < -180 || spot.tx_ll.lng_d > 180) {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d bogus LON %s\n", adif.line_n, adif.value);
            return (false);
        }
        ADD_AFB (adif, AFB_LON);

    } else if (!strcasecmp (adif.name, "POTA_REF")) {
        quietStrncpy (adif.pota_ref, adif.value, sizeof(adif.pota_ref));

    } else if (!strcasecmp (adif.name, "SOTA_REF")) {
        quietStrncpy (adif.sota_ref, adif.value, sizeof(adif.sota_ref));

    } else if (!strcasecmp (adif.name, "WWFF_REF")) {
        quietStrncpy (adif.wwff_ref, adif.value, sizeof(adif.wwff_ref));

    } else if (!strcasecmp (adif.name, "SIG")) {
        quietStrncpy (adif.sig, adif.value, sizeof(adif.sig));

    } else if (!strcasecmp (adif.name, "SIG_INFO")) {
        quietStrncpy (adif.sig_info, adif.value, sizeof(adif.sig_info));

    } else 
        useful_field = false;

    if (debugLevel (DEBUG_ADIF, 3)) {
        if (useful_field)
            Serial.printf ("ADIF: added <%s:%d>%s\n", adif.name, adif.value_seen, adif.value);
        else
            Serial.printf ("ADIF: unused field <%s:%d>%s\n", adif.name, adif.value_seen, adif.value);
    }

    // keep going
    return (true);
}

/* make sure spot is complete and ready to use.
 * return whether spot is good to go.
 */
static bool spotLooksGood (ADIFParser &adif, DXSpot &spot)
{
    if (! (CHECK_AFB(adif, AFB_CALL) || CHECK_AFB(adif, AFB_CONTACTED_OP)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No CALL or CONTACTED_OP\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_FREQ) || CHECK_AFB(adif, AFB_BAND)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No FREQ or BAND\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_QSO_DATE)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No QSO_DATE\n", adif.line_n);
        return (false);
    }

    if (! (CHECK_AFB(adif, AFB_TIME_ON)) ) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No TIME_ON\n", adif.line_n);
        return (false);
    }

    // must have tx location for plotting
    if (!CHECK_AFB(adif,AFB_LAT) || !CHECK_AFB(adif,AFB_LON)) {
        if (CHECK_AFB(adif,AFB_GRIDSQUARE)) {
            if (!maidenhead2ll (spot.tx_ll, spot.tx_grid)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d bogus grid %s for %s\n", adif.line_n,spot.tx_grid,spot.tx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from grid %s\n", adif.line_n, spot.tx_call,
                                            spot.tx_grid);
        } else {
            if (!call2LL (spot.tx_call, spot.tx_ll)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d No GRIDSQUARE LAT or LON and cty lookup for %s failed\n",
                                    adif.line_n, spot.tx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from cty\n", adif.line_n, spot.tx_call);
        }
    }

    // at this point we know we have tx_ll so might as well add tx_grid if not already
    if (!CHECK_AFB(adif,AFB_GRIDSQUARE)) {
        ll2maidenhead (spot.tx_grid, spot.tx_ll);
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: line %d: add grid %s for %s from ll\n", adif.line_n, spot.tx_grid,
                                            spot.tx_call);
    }

    if (!CHECK_AFB(adif, AFB_MODE)) {
        if (debugLevel (DEBUG_ADIF, 2))
            Serial.printf ("ADIF: line %d No MODE\n", adif.line_n);
        return (false);
    }


    if (! (CHECK_AFB(adif, AFB_OPERATOR) || CHECK_AFB(adif, AFB_STATION_CALLSIGN)) ) {
        // assume us?
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: assuming RX is %s\n", getCallsign());
        quietStrncpy (spot.rx_call, getCallsign(), sizeof(spot.rx_call));
        spot.rx_ll = de_ll;
        ll2maidenhead (spot.rx_grid, spot.rx_ll);
        ADD_AFB (adif, AFB_MY_LAT);
        ADD_AFB (adif, AFB_MY_LON);
        ADD_AFB (adif, AFB_MY_GRIDSQUARE);
    }


    // must have rx location for plotting
    if (!CHECK_AFB(adif,AFB_MY_LAT) || !CHECK_AFB(adif,AFB_MY_LON)) {
        if (CHECK_AFB(adif,AFB_MY_GRIDSQUARE)) {
            if (!maidenhead2ll (spot.rx_ll, spot.rx_grid)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d bogus grid %s for %s\n", adif.line_n,spot.rx_grid,spot.rx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from grid %s\n", adif.line_n, spot.rx_call,
                                            spot.rx_grid);
        } else {
            if (!call2LL (spot.rx_call, spot.rx_ll)) {
                if (debugLevel (DEBUG_ADIF, 2))
                    Serial.printf ("ADIF: line %d No GRIDSQUARE LAT or LON and cty lookup for %s failed\n",
                                    adif.line_n, spot.rx_call);
                return (false);
            }
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: add ll for %s from cty\n", adif.line_n, spot.rx_call);
        }
    }

    // at this point we know we have rx_ll so might as well add rx_grid if not already
    if (!CHECK_AFB(adif,AFB_MY_GRIDSQUARE)) {
        ll2maidenhead (spot.rx_grid, spot.rx_ll);
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: line %d: add grid %s for %s from ll\n", adif.line_n, spot.rx_grid,
                                            spot.rx_call);
    }

    // check rx and tx dxcc
    if (!CHECK_AFB(adif,AFB_MY_DXCC)) {
        if (!call2DXCC (spot.rx_call, spot.rx_dxcc)) {
            if (debugLevel (DEBUG_ADIF, 2))
                Serial.printf ("ADIF: line %d no DXCC for %s\n", adif.line_n, spot.rx_call);
            return (false);
        }
    }
    if (!CHECK_AFB(adif,AFB_DXCC)) {
        if (!call2DXCC (spot.tx_call, spot.tx_dxcc)) {
            if (debugLevel (DEBUG_ADIF, 2))
                Serial.printf ("ADIF: line %d no DXCC for %s\n", adif.line_n, spot.tx_call);
            return (false);
        }
    }

    // all good, just tidy up a bit
    strtoupper (spot.tx_call);
    spot.tx_ll.normalize();
    strtoupper (spot.rx_call);
    spot.rx_ll.normalize();

    return (true);
}

/* parse the next character of an ADIF file, updating parser state and filling in spot as we go along.
 * set adis.ps to ADIFPS_STARTFILE on first call then just leave ps alone.
 * returns true when a candidate spot has been assembled.
 */
static bool parseADIF (char c, ADIFParser &adif, DXSpot &spot)
{
    // update running line count
    if (c == '\n')
        adif.line_n++;

    // next action depends on current state

    switch (adif.ps) {

    case ADIFPS_STARTFILE:
        // full init
        memset (&adif, 0, sizeof(adif));
        adif.line_n = 1;
        spot = {};

        // fallthru

    case ADIFPS_FINISHED:
        // putting FINISHED here allows caller to not have to change ps to look for the next spot

        // fallthru

    case ADIFPS_STARTSPOT:
        // init spot
        if (debugLevel (DEBUG_ADIF, 3))
            Serial.printf ("ADIF: starting new spot scan\n");
        spot = {};

        // init per-spot fields in parser
        adif.qso_date[0] = '\0';
        adif.time_on[0] = '\0';
        adif.fields = 0;
        adif.pota_ref[0] = adif.sota_ref[0] = adif.wwff_ref[0] = '\0';
        adif.sig[0] = adif.sig_info[0] = '\0';

        // fallthru

    case ADIFPS_STARTFIELD:
        // init per-field fields in parser
        adif.name[0] = '\0';
        adif.value[0] = '\0';
        adif.name_seen = 0;
        adif.value_len = 0;
        adif.value_seen = 0;

        // fallthru

    case ADIFPS_SEARCHING:
        if (c == '<')
            adif.ps = ADIFPS_INNAME;                    // found opening <, start looking for field name
        else
            adif.ps = ADIFPS_SEARCHING;                 // in case we got here via a fallthru
        break;

    case ADIFPS_INNAME:
        if (c == ':') {
            // finish field name, start building value length until find > or optionl type :
            adif.value_len = 0;
            adif.ps = ADIFPS_INLENGTH;
        } else if (c == '>') {
            // bogus unless EOH or EOF
            if (!strcasecmp (adif.name, "EOH")) {
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: found EOH\n");
                adif.ps = ADIFPS_STARTSPOT;
            } else if (!strcasecmp (adif.name, "EOR")) {
                // yah!
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: found EOR\n");
                adif.ps = ADIFPS_FINISHED;
            } else {
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d no length with field %s", adif.line_n, adif.name);
                adif.ps = ADIFPS_SKIPTOEOR;
            }
        } else if (adif.name_seen > sizeof(adif.name)-1) {
            // too long for name[] but none of the field names we care about will overflow so just skip it
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: ignoring long name %.*s %d > %d\n", adif.line_n,
                                adif.name_seen, adif.name, adif.name_seen, (int)(sizeof(adif.name)-1));
            adif.ps = ADIFPS_STARTFIELD;
        } else {
            // append next character to field name, maintaining EOS
            adif.name[adif.name_seen] = c;
            adif.name[++adif.name_seen] = '\0';
        }
        break;

    case ADIFPS_INLENGTH:
        if (c == ':') {
            // finish value length, start skipping optional data type. TODO?
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF: line %d: in type for %s\n", adif.line_n, adif.name);
            adif.ps = ADIFPS_INTYPE;
        } else if (c == '>') {
            // finish value length, start collecting value_len chars for field value unless 0
            adif.value[0] = '\0';
            adif.value_seen = 0;
            if (adif.value_len == 0) {
                adif.ps = ADIFPS_STARTFIELD;
                if (debugLevel (DEBUG_ADIF, 3))
                    Serial.printf ("ADIF: line %d: 0 length data field %s\n", adif.line_n, adif.name);
            } else
                adif.ps = ADIFPS_INVALUE;
        } else if (isdigit(c)) {
            // fold c as int into value_len
            adif.value_len = 10*adif.value_len + (c - '0');
            if (debugLevel (DEBUG_ADIF, 3) && adif.value_len == 0)
                Serial.printf ("ADIF: line %d: 0 in length field %s now %d\n", adif.line_n, adif.name,
                                        adif.value_len);
        } else {
            if (debugLevel (DEBUG_ADIF, 3))
                Serial.printf ("ADIF line %d: non-digit %c in field %s length\n", adif.line_n, c, adif.name);
            adif.ps = ADIFPS_SKIPTOEOR;
        }
        break;

    case ADIFPS_INTYPE:
        // just skip until see >
        if (c == '>') {
            // finish optional type length, start collecting value_len chars for field value
            adif.value[0] = '\0';
            adif.value_seen = 0;
            if (adif.value_len == 0)
                adif.ps = ADIFPS_STARTFIELD;
            else
                adif.ps = ADIFPS_INVALUE;
        }
        break;

    case ADIFPS_INVALUE:
        // append next character + EOS to field value if room, but always keep counting
        if (adif.value_seen < sizeof(adif.value)-1) {
            adif.value[adif.value_seen] = c;
            adif.value[adif.value_seen+1] = '\0';
        }
        adif.value_seen += 1;

        // finished when found entire field
        if (adif.value_seen == adif.value_len) {
            // install if we had room to store it al
            if (adif.value_seen < sizeof(adif.value)) {
                (void) addADIFFIeld (adif, spot);       // rely on spotLooksGood() for final qualification
            } else if (debugLevel (DEBUG_ADIF, 3)) {
                Serial.printf ("ADIF: ignoring long value <%s:%d>%.*s %d > %d\n",
                                adif.name, adif.value_len, adif.value_seen, adif.value,
                                adif.value_len, (int)(sizeof(adif.value)-1));
            }
            // start next field
            adif.ps = ADIFPS_STARTFIELD;
        }
        break;

    case ADIFPS_SKIPTOEOR:
        // just keep looking for <eor> in adif.name, start fresh spot when find it
        if (c == '>') {
            if (adif.name_seen < sizeof(adif.name)) {
                adif.name[adif.name_seen] = '\0';
                if (strcasecmp (adif.name, "EOR") == 0)
                    adif.ps = ADIFPS_STARTSPOT;
            }
            adif.name_seen = 0;
        } else if (c == '<') {
            adif.name_seen = 0;
        } else if (adif.name_seen < sizeof(adif.name)-1)
            adif.name[adif.name_seen++] = c;
        break;
    }

    // return whether finished
    bool finished = adif.ps == ADIFPS_FINISHED;
    if (finished && debugLevel (DEBUG_ADIF, 3))
        Serial.printf ("ADIF: finished parsing\n");
    return (finished);
}

/* general purpose ADIF parser from a GenReader.
 * add malloced DXSpots to spots, add to prefix table and return count.
 * also:
 *   we pass back count of any broken spots or did not qualify WLID_ADIF if used.
 *   use_wl determines whether spots are checked against WLID_ADIF.
 * N.B. must call with spots = NULL and caller is responsible to free (spots).
 * N.B. caller must close gr
 */
int readADIFFile (GenReader &gr, DXSpot *&spots, bool use_wl, int &n_bad)
{
    // init counts, timer
    int n_read = 0;
    int n_good = 0;
    int n_malloc = 0;
    const int malloc_more = 1000;
    n_bad = 0;
    struct timeval tv0;
    gettimeofday (&tv0, NULL);

    // reset dxpeds worked list, and the worked references too if this is the main log load
    resetDXPedsWorked();
    if (use_wl)
        resetRefsWorked (RW_ADIF);

    // crack file
    DXSpot spot;
    ADIFParser adif;
    adif.ps = ADIFPS_STARTFILE;
    char c;
    if (debugLevel (DEBUG_ADIF, 1))
        Serial.printf ("ADIF: WL DE_Call   Grid   DXCC  DX_Call   Grid   DXCC    Lat   Long Mode      kHz\n");
    while (gr.getChar(&c)) {
        if (parseADIF (c, adif, spot)) {
            // spot parsing complete
            if (spotLooksGood (adif, spot)) {
                // at this point all spot fields are complete
                n_read++;

                // add to the DXPeds indices regardless of watch list
                addDXPedsWorked (spot);

                // likewise note any park or summit worked. N.B. only the main log load does this (use_wl) so a
                // trial read of an upload or a one-record UDP packet can not disturb the real list.
                if (use_wl) {
                    if (adif.pota_ref[0])
                        addRefsWorkedList (RW_ADIF, "POTA", adif.pota_ref);
                    if (adif.sota_ref[0])
                        addRefsWorkedList (RW_ADIF, "SOTA", adif.sota_ref);
                    if (adif.wwff_ref[0])
                        addRefsWorkedList (RW_ADIF, "WWFF", adif.wwff_ref);
                    if (adif.sig[0] && adif.sig_info[0])
                        addRefsWorkedList (RW_ADIF, adif.sig, adif.sig_info);
                }

                // add to list if qualifies watch list
                bool wl_ok = !use_wl || checkWatchListSpot(WLID_ADIF, spot) != WLS_NO;
                if (wl_ok) {
                    // add to *spots_p
                    if (n_good+1 > n_malloc) {
                        spots = (DXSpot *) realloc (spots, (n_malloc += malloc_more) * sizeof(DXSpot));
                        if (!spots)
                            fatalError ("No memory for %d ADIF Spots", n_malloc);
                    }
                    spots[n_good++] = spot;
                }
                // nice logging if enabled
                if ((wl_ok && debugLevel (DEBUG_ADIF, 1)) || (!wl_ok && debugLevel (DEBUG_ADIF, 2))) {
                    Serial.printf("ADIF: %s %-9.9s %-6.6s %4d  %-9.9s %-6.6s %4d  %5.1f %6.1f %4.4s %8.1f\n", 
                        wl_ok ? "OK" : "NO",
                        spot.rx_call, spot.rx_grid, spot.rx_dxcc,
                        spot.tx_call, spot.tx_grid, spot.tx_dxcc,
                        spot.tx_ll.lat_d, spot.tx_ll.lng_d, spot.mode, spot.kHz);
                }
            } else
                n_bad++;        // count actual broken spots, not ones that just aren't selected by WL

            // look alive
            if (((n_good + n_bad)%100) == 0)
                updateClocks(false);
        }
    }

    // rm excess spots
    spots = (DXSpot *) realloc (spots, n_good * sizeof(DXSpot));

    if (debugLevel (DEBUG_ADIF, 1)) {
        struct timeval tv1;
        gettimeofday (&tv1, NULL);
        long usec = TVDELUS (tv0, tv1);
        Serial.printf ("ADIF: file read %d required %ld ms = %ld spots/s\n", n_read, usec/1000,
                                                                                1000000L*n_read/usec);
    }

    return (n_good);
}
