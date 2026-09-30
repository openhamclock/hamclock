/* implement EEPROM class using a local file.
 * format is %08X %02X\n for each address/byte pair.
 * updates of existing address are performed in place.
 */

#include <string>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>

#include "Arduino.h"
#include "EEPROM.h"

class EEPROM EEPROM;

EEPROM::EEPROM()
{
        fp = NULL;
        filename = NULL;
}

const char *EEPROM::getFilename(void)
{
        // establish file name
	if (!filename) {

            // new file name
            std::string newfn = our_dir + "eeprom";

            // preserve old file if found
	    char oldfn[1024];
	    snprintf (oldfn, sizeof(oldfn), "%s/.rpihamclock_eeprom", getenv("HOME"));
            rename (oldfn, newfn.c_str());

	    filename = strdup (newfn.c_str());
	}

        return (filename);
}

void EEPROM::begin (int s)
{
        // establish filename
        filename = getFilename();

        // start over if called again or force
        if (fp) {
            fclose(fp);
            fp = NULL;
        }
        if (rm_eeprom) {
            if (unlink (filename) < 0)
                (void) truncate (filename, 0);
            rm_eeprom = false;  // only once!
        }
        if (data_array) {
            free (data_array);
            data_array = NULL;
        }

        // open RW, create if new owned by real user
	fp = fopen (filename, "r+");
        if (!fp) {
            fp = fopen (filename, "w+");
            if (!fp) {
                fprintf (stderr, "%s: %s\n", filename, strerror(errno));
                exit(1);
            }
        }
        (void) !fchown (fileno(fp), getuid(), getgid());

        // check lock
        if (flock (fileno(fp), LOCK_EX|LOCK_NB) < 0) {
            fprintf (stderr, "Another instance of HamClock has been detected.\n"
                        "Only one at a time is allowed or use -d, -e and -w to make each unique.\n");
            exit(1);
        }

        // malloc memory, init as zeros
        n_data_array = s;
        data_array = (uint8_t *) calloc (n_data_array, sizeof(uint8_t));

        // init data_array from file .. support old version of random memory locations ...
        // and another old version with bug that wrote valid locations a second time with zeros.
	char line[64];
	unsigned int a, v;
        unsigned int largest_a = 0;
	while (fp && fgets (line, sizeof(line), fp)) {
	    if (sscanf (line, "%x %x", &a, &v) == 2 && a < n_data_array && a >= largest_a) {
                data_array[a] = v;
                largest_a = a;
            }
        }
}

bool EEPROM::commit(void)
{
        // (over)write entire data_array array
        if (ftruncate (fileno(fp), 0) < 0 || fseek (fp, 0L, SEEK_SET) < 0) {
            printf ("eeprom: %s\n", strerror(errno));
            return (false);
        }
        for (unsigned a = 0; a < n_data_array; a++)
            fprintf (fp, "%08X %02X\n", a, data_array[a]);
        fflush (fp);

        // return whether io ok
        return (!feof(fp) && !ferror(fp));
}

void EEPROM::write (uint32_t address, uint8_t byte)
{
        // set array if available and address is in bounds
        if (!data_array)
            printf ("EEPROM.write: no data_array\n");
        else if (address >= n_data_array)
            printf ("EEPROM.write: %d >= %d\n", address, (int)n_data_array);
        else 
            data_array[address] = byte;
}

uint8_t EEPROM::read (uint32_t address)
{
        // use array if available and address is in bounds
        if (data_array && address < n_data_array)
            return (data_array[address]);

        // not found
        return (0);
}
