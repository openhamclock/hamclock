/* Get lat/long from gpsd daemon running on any host port 2947.
 *
 *   general info: https://gpsd.gitlab.io/gpsd/
 *   raw interface: https://gpsd.gitlab.io/gpsd/client-howto.html
 *   more info: https://gpsd.gitlab.io/gpsd/gpsd_json.html
 *
 * Simple server test, run this command:
 *   while true; do echo '"class":"TPV","mode":2,"lat":34.567,"lon":-123.456,"time":"2020-01-02T03:04:05.000Z"'; done | nc -k -l 192.168.7.11 2947
 */

#include "HamClock.h"


#define GPSD_PORT       2947                // tcp port
#define GPSD_TO         5000                // timeout, msec



/* look for time and sufficient mode in the given string from gpsd.
 * if found, save in arg (ptr to a time_t) and return true, else return false.
 */
static bool lookforTime (const char *buf, void *arg)
{
        // get time now so we can correct after we process and display
        uint32_t t0 = millis();

        // check for all required fields, might be more than one class
        // Serial.printf ("\nGPSD: look for time in: %s\n", buf);

        const char *classstr;
        bool found_tpv = false;
        for (const char *bp = buf; !found_tpv && (classstr = strstr (bp, "\"class\":\"")) != NULL;
                                                                        bp = classstr+9) {
            if (strncmp (classstr+9, "TPV", 3) == 0)
                found_tpv = true;
        }
        if (!found_tpv) {
            Serial.print ("GPSD: no TPV\n");
            return (false);
        }

        const char *modestr = strstr (classstr, "\"mode\":");
        if (!modestr) {
            Serial.print ("GPSD: no mode\n");
            return (false);
        }
        int mode = atoi(modestr+7);
        if (mode < 2) {
            Serial.printf ("GPSD: bad mode '%s' %d\n", modestr+7, mode);
            return (false);
        }

        const char *timestr = strstr (classstr, "\"time\":\"");
        if (!timestr || strlen(timestr) < 8+19) {
            Serial.print ("GPSD: no time\n");
            return(false);
        }

        // crack time form: "time":"2012-04-05T15:00:01.501Z"
        const char *iso = timestr+8;
        time_t gpsd_time = crackISO8601 (iso);
        if (gpsd_time == 0) {
            Serial.printf ("GPSD: unexpected ISO8601: %.24s\n", iso);
            return (false);
        }

        // correct for time spent here
        gpsd_time += (millis() - t0 + 500)/1000;

        // good
        *(time_t*)arg = gpsd_time;

        // success
        return (true);
}

/* look for lat and lon and sufficient mode in the given string from gpsd.
 * if found, save in arg (ptr to LatLong) and return true, else return false.
 */
static bool lookforLatLong (const char *buf, void *arg)
{
        // check for all required fields, might be more than one class
        // Serial.printf ("\nGPSD: look for ll in: %s\n", buf);

        const char *classstr;
        bool found_tpv = false;
        for (const char *bp = buf; !found_tpv && (classstr = strstr (bp, "\"class\":\"")) != NULL;
                                                                        bp = classstr+9) {
            if (strncmp (classstr+9, "TPV", 3) == 0)
                found_tpv = true;
        }
        if (!found_tpv)
            return (false);

        const char *modestr = strstr (classstr, "\"mode\":");
        if (!modestr || atoi(modestr+7) < 2)
            return(false);

        const char *latstr = strstr (classstr, "\"lat\":");
        if (!latstr || strlen(latstr) < 6+10)
            return (false);

        const char *lonstr = strstr (classstr, "\"lon\":");
        if (!lonstr || strlen(latstr) < 6+10)
            return (false);

        // crack fields
        LatLong *llp = (LatLong *) arg;
        llp->lat_d = atof(latstr+6);
        llp->lng_d = atof(lonstr+6);

        // success
        Serial.printf ("GPSD: lat %.2f long %.2f\n", llp->lat_d, llp->lng_d);
        return (true);
}

/* connect to gpsd and return whether lookf() found what it wants.
 */
static bool getGPSDSomething(bool (*lookf)(const char *buf, void *arg), void *arg)
{
        // skip if not configured at all
        if (!useGPSDTime() && !useGPSDLoc())
            return (false);

        // get host name
        const char *host = getGPSDHost();
        Serial.printf ("GPSD: trying %s:%d\n", host, GPSD_PORT);

        // prep state
        WiFiClient gpsd_client;
        bool look_ok = false;
        bool connect_ok = false;
        bool got_something = false;
        #define MAXGLL 1000                 // max line length
        StackMalloc line_mem(MAXGLL);
        char *line = (char *) line_mem.getMem();

        // connect to and read from gpsd server, 
        // N.B. do not use getTCPLine() which calls updateClocks() which calls now() which can call for
        //      a time refresh which calls us!
        if (gpsd_client.connect (host, GPSD_PORT)) {

            // initial progress
            connect_ok = true;
            uint32_t t0 = millis();

            // enable reporting
            gpsd_client.print ("?WATCH={\"enable\":true,\"json\":true};?POLL;\n");

            // read lines, give to lookf, done when it's happy or no more or time out
            #define GPSD_TAIL 100           // bytes retained when a too-long line fills the buffer
            size_t ll = 0;
            while (!timesUp(&t0,GPSD_TO) && ll < MAXGLL && !look_ok && getTCPChar(gpsd_client,&line[ll])) {

                // hopeful?
                got_something = true;

                // add to line, crack when full or see nl
                bool see_nl = (line[ll] == '\n');
                if (see_nl || ll == MAXGLL-1) {
                    char line_covered = line[ll];
                    line[ll] = '\0';
                    look_ok = (*lookf)(line, arg);
                    if (!look_ok) {
                        if (see_nl) {
                            // complete line consumed, start the next one from scratch
                            ll = 0;
                        } else {
                            // buffer filled without a nl: retain the last GPSD_TAIL bytes of what we
                            // really read in case a keyword got split, and keep filling after them.
                            line[ll] = line_covered;
                            memmove (line, line+(MAXGLL-GPSD_TAIL), GPSD_TAIL);
                            ll = GPSD_TAIL;
                        }
                    }
                } else
                    ll++;
            }

            // ensure line is a valid string for the problem report below, even after a partial read
            if (ll > 0)
                line[ll < MAXGLL ? ll : MAXGLL-1] = '\0';
        }

        // finished with connection
        gpsd_client.stop();

        // report problems
        if (!look_ok) {
            if (got_something)
                Serial.printf ("GPSD: unexpected response: %s\n", line);
            else if (connect_ok)
                Serial.println ("GPSD: connected but no response");
            else
                Serial.println ("GPSD: no connection");
        }

        // success?
        return (look_ok);
}

/* return time from GPSD if available, else return 0
 */
time_t getGPSDUTC(void)
{
        time_t gpsd_time;

        if (getGPSDSomething (lookforTime, &gpsd_time))
            return (gpsd_time);

        return (0);
}

/* get lat/long from GPSD and set de_ll, return whether successful.
 */
bool getGPSDLatLong(LatLong *llp)
{
        return (getGPSDSomething (lookforLatLong, llp));
}

/* occasionaly refresh DE from GPSD if enabled and we moved a little.
 */
void updateGPSDLoc()
{
        // out fast if not configured
        if (!useGPSDLoc())
            return;

        // not crazy often
        static uint32_t to_t;
        if (!timesUp (&to_t, FOLLOW_DT))
            return;

        // get loc
        LatLong ll;
        if (!getGPSDLatLong(&ll))
            return;

        // dist from DE
        float miles = ERAD_M*de_ll.GSD(ll);

        // engage if large enough, consider 6 char grid is 5'x2.5' or about 6x3 mi at equator
        if (miles > FOLLOW_MIND)
            newDE (ll, NULL);
}

/* convert YYYY-MM-DD?HH:MM:SS to unix time, or 0 if fails
 */
time_t crackISO8601 (const char *iso)
{
        time_t t = 0;
        int yr, mo, dy, hr, mn, sc;
        if (sscanf (iso, "%d-%d-%d%*c%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) == 6) {

            // reformat
            tmElements_t tm;
            tm.Year = yr - 1970;
            tm.Month = mo;
            tm.Day = dy;
            tm.Hour = hr;
            tm.Minute = mn;
            tm.Second = sc;
            t = makeTime(tm);
        }
        return (t);
}
