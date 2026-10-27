#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Arduino.h"
#include "ESP8266WiFi.h"
#include "ESP.h"

class ESP ESP;

ESP::ESP()
{
        sn = 0;
}

void ESP::addArgv (char **&argv, int &argc, const char *arg)
{
        argv = (char **) realloc (argv, (argc+1)*sizeof(char*));
        argv[argc++] = arg ? strdup (arg) : NULL;
}

/* restart possibly with modified our_argv
 */
void ESP::restart (bool minus_K, bool minus_0)
{
        printf ("Restart -K? %d -0? %d\n", minus_K, minus_0);

        // copy our_argv, removing any -[K0] unless wanted
        char **tmp_argv = NULL;
        int tmp_argc = 0;
        for (char **argv = our_argv; *argv != NULL; argv++) {
            char *s = *argv;
            if (s[0] == '-') {
                while (*++s) {
                    switch (*s) {
                    case 'K':
                        if (!minus_K) {
                            memmove (s, s+1, strlen(s));        // remove K, include EOS
                            s -= 1;                             // recheck the new *s
                        }
                        minus_K = false;                        // prevent dup below
                        break;
                    case '0':
                        if (!minus_0) {
                            memmove (s, s+1, strlen(s));        // remove 0, include EOS
                            s -= 1;                             // recheck the new *s
                        }
                        minus_0 = false;                        // prevent dup below
                        break;
                    }
                }
                // add if anything meaningful is left
                if ((*argv)[1] != '\0')
                    addArgv (tmp_argv, tmp_argc, *argv);
            } else {
                // always add non-dash args
                addArgv (tmp_argv, tmp_argc, *argv);
            }
        }

        // add minus_X if not already
        if (minus_K)
            addArgv (tmp_argv, tmp_argc, "-K");
        if (minus_0)
            addArgv (tmp_argv, tmp_argc, "-0");

        // add final sentinel
        addArgv (tmp_argv, tmp_argc, NULL);

        // log
        printf ("Restart: args will be:\n");
        for (int i = 0; tmp_argv[i] != NULL; i++)
            printf ("  argv[%d]: %s\n", i, tmp_argv[i]);
        printf ("see you there!\n\n");

        // embedded host: hand over the new args; the hook does not return.
        // N.B. must come before closing fds, which belong to the host process too.
        if (hamclock_restart_hook)
            hamclock_restart_hook (tmp_argv);

        // close all but basic fd
        for (int i = 3; i < 100; i++)
            (void) ::close(i);

        // go
        execvp (tmp_argv[0], tmp_argv);

        printf ("execvp(%s): %s\n", tmp_argv[0], strerror(errno));
        exit(1);
}

/* try to get some sort of system serial number.
 * return 0xFFFFFFFF if unknown.
 */
uint32_t ESP::getChipId()
{
        // reuse once found
        if (sn)
            return (sn);

#if defined(_IS_LINUX)
        // try cpu serial number
        FILE *fp = fopen ("/proc/cpuinfo", "r");
        if (fp) {
            static const char serial[] = "Serial";
            char buf[1024];
            while (fgets (buf, sizeof(buf), fp)) {
                if (strncmp (buf, serial, sizeof(serial)-1) == 0) {
                    int l = strlen(buf);                        // includes nl
                    sn = strtoul (&buf[l-9], NULL, 16);         // 8 LSB
                    if (sn) {
                        // printf ("Found ChipId '%.*s' -> 0x%X = %u\n", l-1, buf, sn, sn);
                        break;
                    }
                }
            }
            fclose (fp);
            if (sn)
                return (sn);
        }
#endif

        // try MAC address
        std::string mac = WiFi.macAddress();
        unsigned int m1, m2, m3, m4, m5, m6;
        if (sscanf (mac.c_str(), "%x:%x:%x:%x:%x:%x", &m1, &m2, &m3, &m4, &m5, &m6) == 6) {
            sn = (m3<<24) + (m4<<16) + (m5<<8) + m6;
            // printf ("Found ChipId from MAC '%s' -> 0x%x = %u\n", mac.c_str(), sn, sn);
        } else {
            printf ("No ChipId\n");
            sn = 0xFFFFFFFF;
        }

        return (sn);
}

void yield(void)
{
}
