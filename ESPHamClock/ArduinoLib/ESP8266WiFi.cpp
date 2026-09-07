/* simple network interface functions for unix.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netdb.h>

#include "ESP8266WiFi.h"

#if defined(_LINUX_WIRELESS_OK)
#include <linux/wireless.h>
#include <sys/ioctl.h>
#endif

class WiFi WiFi;

/* run shell cmd and return the first line of response.
 * return whether ok
 */
static bool getCommand (const char cmd[], char line[], size_t line_len)
{
        // Serial.printf ("getCommand: %s\n", cmd);

	line[0] = '\0';
        if (debugLevel (DEBUG_NET, 2) || debugLevel (DEBUG_WIFI, 2))
            printf ("** run=%s\n", cmd);
	FILE *pp = popen (cmd, "r");
	if (!pp)
	    return (false);
	bool fgets_ok = fgets (line, line_len, pp) != NULL;
        int err = ferror(pp);
        int eof = feof(pp);
	int wstatus = pclose (pp);
        int exited = WIFEXITED(wstatus);
        int exstatus = WEXITSTATUS(wstatus);
        if (fgets_ok && exited && exstatus == 0 && strlen(line) > 1) {
            line[strlen(line)-1] = '\0';        // rm \n
        if (debugLevel (DEBUG_NET, 2) || debugLevel (DEBUG_WIFI, 2))
                printf ("** back=%s\n", line);
            return (true);
        }
        if (debugLevel (DEBUG_NET, 2) || debugLevel (DEBUG_WIFI, 2)) {
            printf ("** cmd=%s\n", cmd);
            int signaled = WIFSIGNALED(wstatus);
            int signal = WTERMSIG(wstatus);
            if (exited)
                printf ("** err=%d eof=%d exstatus=%d\n", err, eof, exstatus);
            else if (signaled)
                printf ("** err=%d eof=%d signal=%d\n", err, eof, signal);
            else if (wstatus == -1)
                printf ("** err=%d eof=%d wait(2) err: %s\n", err, eof, strerror(errno));
            else
                printf ("** err=%d eof=%d nknown wait status: %d\n", err, eof, wstatus);
        }
	return (false);
}

/* convert line containing a.b.c.d into IPaddress a.
 * return whether ok
 */
static bool crackIP (const char line[], IPAddress &a)
{
        int i[4] = {0, 0, 0, 0};

        if (sscanf (line, "%d.%d.%d.%d", &i[0], &i[1], &i[2], &i[3]) != 4)
            return (false);

        a[0] = i[0];
        a[1] = i[1];
        a[2] = i[2];
        a[3] = i[3];
        return (true);
}

void WiFi::begin (char *ssid, char *pw)
{
#if defined(_IS_LINUX)

	// set wpa_supplicant if given ssid and pw.

	static const char wpafn[] = "/etc/wpa_supplicant/wpa_supplicant.conf";

        if (!ssid || ssid[0]=='\0' || !pw || pw[0]=='\0')
            return;

	// create/overwrite supp file
        printf ("Creating %s with %s/%s\n", wpafn, ssid, pw);
	FILE *wfp = fopen (wpafn, "w");
	if (!wfp) {
	    printf ("Can not create %s: %s\n", wpafn, strerror(errno));
	    return;
	}

	// country=US makes a good default because it has the most restricted subset of channels,
	//  see https://en.wikipedia.org/wiki/List_of_WLAN_channels
        fprintf (wfp, "ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev\n");
        fprintf (wfp, "update_config=1\n");             // allow wpa_cli commands
        fprintf (wfp, "country=US\n");                  // most conservative set of channels
        fprintf (wfp, "network={\n");
            fprintf (wfp, "\tssid=\"%s\"\n", ssid);
            fprintf (wfp, "\tpsk=\"%s\"\n", pw);
            fprintf (wfp, "\tscan_ssid=1\n");           // allow invisible networks
        fprintf (wfp, "}\n");

	fclose (wfp);

	// restart, but don't wait here
        printf ("restarting wlan0\n");
        (void) !system ("wpa_cli -i wlan0 reconfigure");

#else

        (void) ssid;
        (void) pw;

#endif // !_IS_LINUX
}


IPAddress WiFi::localIP(void)
{
	static IPAddress a;                     // cache once found

        // try cache first
        if (a[0] != 0)
            return (a);

        // lookup host address, retry for several seconds in case network still coming up after host power-on
        // N.B. must call freeaddrinfo(aip) after successful call before returning
        struct addrinfo hints, *aip = NULL;
        char port_str[16];
        memset (&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        snprintf (port_str, sizeof(port_str), "%d", backend_port);
        int error = 1;
        for (time_t start_t = time(NULL); error && time(NULL) < start_t + 10; ) {
            error = ::getaddrinfo (backend_host, port_str, &hints, &aip);
            if (error) {
                printf ("getaddrinfo(%s:%d): %s\n", backend_host, backend_port, gai_strerror(error));
                usleep (1000000);
                aip = NULL;
            }
        }
        if (!aip || error)
            return (a);

        // create socket
        int sockfd;
        sockfd = ::socket (aip->ai_family, aip->ai_socktype, aip->ai_protocol);
        if (sockfd < 0) {
            freeaddrinfo (aip);
            printf ("socket(%s:%d): %s\n", backend_host, backend_port, strerror(errno));
            return (a);
        }

        // connect
        if (::connect (sockfd, aip->ai_addr, aip->ai_addrlen) < 0) {
            printf ("connect(%s,%d): %s\n", backend_host, backend_port, strerror(errno));
            freeaddrinfo (aip);
            close (sockfd);
            return (a);
        }

        // finished with aip
        freeaddrinfo (aip);

        // get local side ip
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        if (::getsockname (sockfd, (struct sockaddr *)&sa, &sl) < 0) {
            printf ("getsockname(%s,%d): %s\n", backend_host, backend_port, strerror(errno));
            close (sockfd);
            return (a);
        }

        // finished with socket
        close (sockfd);

        // crack addr
        char addr[32];
        strcpy (addr, inet_ntoa(sa.sin_addr));
        if (!crackIP (addr, a)) {
            printf ("bogus local IP: %s\n", addr);
            return (a);
        }

        // ok
        return (a);

}

IPAddress WiFi::subnetMask(void)
{
	static IPAddress a;                     // retain as cache

        // try cache first
        if (a[0] != 0)
            return (a);

	// get list of all interfaces
	struct ifaddrs *ifp0;
	if (getifaddrs(&ifp0) < 0) {
	    printf ("getifaddrs(): %s\n", strerror(errno));
	    return (a);
	}

	// scan for any UP AF_INET with adder other than 127.0.0.1
	for (struct ifaddrs *ifp = ifp0; ifp != NULL; ifp = ifp->ifa_next) {
	    if (ifp->ifa_addr && ifp->ifa_addr->sa_family == AF_INET && (ifp->ifa_flags & IFF_UP)) {
		void *addr_in = &((struct sockaddr_in *)ifp->ifa_addr)->sin_addr;
		char *addr_a = inet_ntoa(*(struct in_addr*)addr_in);
		if (strcmp ("127.0.0.1", addr_a) && ifp->ifa_netmask) {
                    void *netm_in = &((struct sockaddr_in *)ifp->ifa_netmask)->sin_addr;
                    char *netm_a = inet_ntoa(*(struct in_addr*)netm_in);
		    // printf ("interface %s: netmask %s\n", ifp->ifa_name, netm_a);
                    crackIP (netm_a, a);
		    break;
		}
	    }
	}

	// free list
	freeifaddrs (ifp0);

	return (a);
}

IPAddress WiFi::gatewayIP(void)
{
	static IPAddress a;                     // retain as cache
	char cmd[128], back[128];

        // try cache first
        if (a[0] != 0)
            return (a);

        strcpy (cmd,  "[ -x /sbin/ip ] && /sbin/ip route show default | awk '/default via/{print $3}'");
	if (getCommand (cmd, back, sizeof(back)) && crackIP (back, a))
            return (a);

        strcpy (cmd,  "netstat -rn | awk '(/^0.0.0.0/ || /^default/) && !/::/{print $2}'");
	if (getCommand (cmd, back, sizeof(back)) && crackIP (back, a))
            return (a);

        // default 0
	return (a);
}

IPAddress WiFi::dnsIP(void)
{
	static IPAddress a;                     // retain as cache
	char cmd[128], back[128];

        // try cache first
        if (a[0] != 0)
            return (a);

	strcpy (cmd, "awk '/nameserver/{print $2}' /etc/resolv.conf | head -1");
	if (getCommand (cmd, back, sizeof(back)) && crackIP (back, a))
            return (a);

        // default 0
	return (a);
}

/* pass back RSSI and whether it was dBm or percentage.
 * return whether values are valid.
 */
bool WiFi::RSSI(int &value, bool &is_dbm)
{
    bool ok = false;

#if defined (_IS_LINUX)

#if defined (_LINUX_WIRELESS_OK)

    struct ifaddrs *ifaddr;
    struct iw_statistics iwstats;

    // get list of interfaces. N.B. must freeifaddrs(ifaddr)
    if (getifaddrs(&ifaddr) < 0) {
        printf ("getifaddrs(): %s\n", strerror(errno));
        return (false);
    }

    // any socket will do
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf ("can not create socket for RSSI: %s\n", strerror(errno));
        freeifaddrs (ifaddr);
        return (false);
    }

    // look for interface for which SIOCGIWNAME works which can only be wireless
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {

        struct iwreq wrq;
        memcpy (wrq.ifr_name, ifa->ifa_name, IFNAMSIZ);
        if (ioctl(sock, SIOCGIWNAME, &wrq) == 0) {
            if (debugLevel (DEBUG_WIFI, 1))
                printf ("found wireless connection %s\n", wrq.ifr_name);

            // show ssid if interested
            if (debugLevel (DEBUG_WIFI, 1)) {
                char essid[IW_ESSID_MAX_SIZE+1];
                wrq.u.essid.pointer = essid;
                if (ioctl(sock, SIOCGIWESSID, &wrq) < 0)
                    printf ("can not retrieve SSID: %s\n", strerror(errno));
                else
                    printf("ESSID %s\n", essid);
            }

            // finally get signal info
            memset(&iwstats, 0, sizeof(iwstats));
            wrq.u.data.pointer = &iwstats;
            wrq.u.data.length = sizeof(struct iw_statistics);
            wrq.u.data.flags = 1;
            if (ioctl(sock, SIOCGIWSTATS, &wrq) == 0) {
                // found one!
                ok =  true;
                break;
            }

        } else if (debugLevel (DEBUG_WIFI, 1))
            printf ("SIOCGIWNAME: %s\n", strerror(errno));
    }

    // clean up
    close (sock);
    freeifaddrs (ifaddr);

    // go it?
    if (ok) {
        if (debugLevel(DEBUG_WIFI, 1)) {
            printf("Signal level%s is %d%s.\n",
               (iwstats.qual.updated & IW_QUAL_DBM ? " (in dBm)" :" (as percentage)"),
               iwstats.qual.level,
               (iwstats.qual.updated & IW_QUAL_LEVEL_UPDATED ? " (updated)" :""));
        }
        is_dbm = (iwstats.qual.updated & IW_QUAL_DBM) != 0;
        value = is_dbm ? -iwstats.qual.level/10 : iwstats.qual.level;    // level is -dbm*10 else percentage
    } else if (debugLevel(DEBUG_WIFI, 1))
        printf ("No wifi found\n");


#else // !_LINUX_WIRELESS_OK

    // simpler way for systems without linux headers

    FILE *fp = fopen ("/proc/net/wireless", "r");
    if (fp) {
        char buf[200];
        while (fgets (buf, sizeof(buf), fp)) {
            int status;
            float rssif;
            if (sscanf (buf, " %*[^:]: %d %*f %f %*f", &status, &rssif) == 2 && status == 0) {
                // reject if appears to be a percentage
                if (rssif > 0)
                    ok = false;
                else
                    value = (int)rssif;
                break;
            }
        }
        fclose (fp);
    }

#endif // _LINUX_WIRELESS_OK


#endif // _IS_LINUX



#ifdef _IS_APPLE

        static const char cmd[] =
            "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport -I";
        char ret[2048];

        if (getCommand (cmd, ret, sizeof(ret))) {
            int apple_rssi;
            char *rssi_kw = strstr (ret, "agrCtlRSSI: ");
            if (rssi_kw && sscanf (rssi_kw+11, "%d", &apple_rssi) == 1 && apple_rssi != 0) {
                value = apple_rssi;
                is_dbm = true;
                ok = true;
            }
        }

        if (debugLevel(DEBUG_WIFI, 1) && !ok)
            printf ("No RSSI from apple\n");

#endif // _IS_APPLE


    return (ok);
}

int WiFi::status(void)
{
	// get list of all interfaces
	struct ifaddrs *ifp0;
	if (getifaddrs(&ifp0) < 0) {
	    printf ("getifaddrs(): %s\n", strerror(errno));
	    return (WL_OTHER);
	}

	// scan for any UP AF_INET with adder other than 127.0.0.1
	bool ok = false;
	for (struct ifaddrs *ifp = ifp0; ifp != NULL && !ok; ifp = ifp->ifa_next) {
	    if (ifp->ifa_addr && ifp->ifa_addr->sa_family == AF_INET && (ifp->ifa_flags & IFF_UP)) {
		void *addr_in = &((struct sockaddr_in *)ifp->ifa_addr)->sin_addr;
		char *addr_a = inet_ntoa(*(struct in_addr*)addr_in);
		if (strcmp ("127.0.0.1", addr_a)) {
		    // printf ("interface %s: addr %s\n", ifp->ifa_name, addr_a);
		    ok = true;
		}
	    }
	}

	// always free list
	freeifaddrs (ifp0);

	// return result code
        if (!ok)
            printf ("no net connections\n");
	return (ok ? WL_CONNECTED : WL_OTHER);
}

int WiFi::mode (int m)
{
        (void) m;

	return (WIFI_OTHER);
}

std::string WiFi::macAddress(void)
{
#if defined(_IS_IOS)
    return ios_get_mac_address();
#else
	char line[128];

	// try a few different variations, first two try to find the default interface
        static const char *cmds[] = {
            "[ -x /sbin/ip ] && /sbin/ip addr show dev "
                "`/sbin/ip route show default 0.0.0.0/0 | perl -n -e '/default.* dev (\\S+) / and print $1'`"
                "| perl -n -e '/ether ([a-fA-F0-9:]+)/ and print \"$1\\n\"'",
            "[ -x /sbin/ifconfig ] && /sbin/ifconfig | awk '/ether/{print $2}' | head -1",
            "[ -x /sbin/ifconfig ] && /sbin/ifconfig | awk '/HWaddr/{print $5}' | head -1",
            "[ -x /sbin/ifconfig ] && /sbin/ifconfig | awk '/address/{print $2}' | head -1",
            "[ -x /sbin/ifconfig -a -x /sbin/route ] && /sbin/ifconfig "
                "`/sbin/route -n | awk '/UG/{print $8}'` | awk '/ether/{print $2}'",
        };
        const int n_cmds = sizeof(cmds)/sizeof(cmds[0]);

        for (int i = 0; i < n_cmds; i++) {
            if (getCommand (cmds[i], line, sizeof(line))) {
                // insure 5 :
                unsigned int m1, m2, m3, m4, m5, m6;
                if (sscanf (line, "%x:%x:%x:%x:%x:%x", &m1, &m2, &m3, &m4, &m5, &m6) == 6)
                    break;
            }
            strcpy (line, "FF:FF:FF:FF:FF:FF");
        }

	return (std::string(line));
#endif
}

std::string WiFi::hostname(void)
{
        char hn[512];
        if (gethostname (hn, sizeof(hn))) {
            strcpy (hn, "hostname??");
        } else {
            char *dot = strchr (hn, '.');
            if (dot)
                *dot = '\0';
        }
	return (std::string(hn));
}

int WiFi::channel(void)
{
	int channel = 0;

#ifdef _IS_LINUX
	FILE *pf = popen ("/usr/sbin/iw wlan0 info", "r");
	if (pf) {
	    char buf[1024];
	    while (fgets (buf, sizeof(buf), pf))
		if (sscanf (buf, " channel %d", &channel) == 1)
		    break;
	    pclose (pf);
	}
#endif

	return (channel);
}

std::string WiFi::SSID(void)
{
	return (std::string(""));
}

std::string WiFi::psk(void)
{
	return (std::string(""));
}
