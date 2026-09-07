#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <memory.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include "i2cdriver.h"

/* debug levels:
 * RAW_VERBOSE shows i2cdriver traffic
 * I2C_VERBOSE just shows the I2C traffic
 */
// #define RAW_VERBOSE
// #define I2C_VERBOSE

// ******************************  Serial port  *********************************

static int openSerialPort(const char *portname)
{
    struct termios Settings;
    int fd;

    fd = open(portname, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        printf ("I2C: openSerialPort: open(%s): %s\n", portname, strerror(errno));
        return -1;
    }
    if (tcgetattr(fd, &Settings) < 0) {
        printf ("I2C: openSerialPort: tcgetattr(%s): %s\n", portname, strerror(errno));
        close (fd);
        return -1;
    }

#if defined(_IS_APPLE) && !defined(B1000000)
#include <IOKit/serial/ioss.h>
#else
    cfsetispeed(&Settings, B1000000);
    cfsetospeed(&Settings, B1000000);
#endif

    cfmakeraw(&Settings);
    Settings.c_cc[VMIN] = 1;
    if (tcsetattr(fd, TCSAFLUSH, &Settings) < 0) {
        printf ("I2C: openSerialPort: tcsetattr(%s): %s\n", portname, strerror(errno));
        close (fd);
        return -1;
    }

#if defined(_IS_APPLE) && !defined(B1000000)
    speed_t speed = (speed_t)1000000;
    if (ioctl(fd, IOSSIOSPEED, &speed) < 0) {
        printf ("I2C: openSerialPort: IOSSIOSPEED(%s): %s\n", portname, strerror(errno));
        close (fd);
        return -1;
    }
#endif

    if (tcflush (fd, TCIOFLUSH) < 0) {
        printf ("I2C: openSerialPort: TCIOFLUSH(%s): %s\n", portname, strerror(errno));
        close (fd);
        return -1;
    }

    return fd;
}


static size_t readFromSerialPort(I2CDriver *sd, uint8_t *b, size_t s)
{

#ifdef RAW_VERBOSE
    printf (" READING %d from 0x%02X\n", (int)s, sd->dev_addr);
#endif

    size_t t = 0;
    while (t < s) {
        fd_set rs;
        FD_ZERO (&rs);
        FD_SET (sd->port, &rs);
        struct timeval to;
        to.tv_sec = 5;
        to.tv_usec = 0;
        int sel = select (sd->port+1, &rs, NULL, NULL, &to);
        if (sel < 0) {
            printf ("I2C readFromSerialPort(0x%02X): select %zd/%zd failed: %s\n", sd->dev_addr, t, s,
                strerror(errno));
            break;
        }
        if (sel == 0) {
            printf ("I2C readFromSerialPort(0x%02X): select %zd/%zd timed out\n", sd->dev_addr, t, s);
            break;
        }
        ssize_t n = read(sd->port, b + t, s - t);
        if (n < 0) {
            printf ("I2C readFromSerialPort(0x%02X): read(%zd/%zd): %s\n", sd->dev_addr, t, s,                                                                                                  strerror(errno));
            break;
        }
        else if (n == 0) {
            printf ("I2C readFromSerialPort(0x%02X): read(%zd/%zd): EOF\n", sd->dev_addr, t, s);
            break;
        } else
            t += n;
    }

#ifdef RAW_VERBOSE
    printf(" READ dev 0x%02X %d of %d: ", sd->dev_addr, (int)t, (int)s);
    for (size_t i = 0; i < s; i++)
        printf("%02x ", 0xff & b[i]);
    printf("\n");
#endif

    return t;
}


static size_t writeToSerialPort(I2CDriver *sd, const uint8_t *b, size_t s)
{
    size_t t = 0;
    while (t < s) {
        ssize_t n = write(sd->port, b + t, s - t);
        if (n < 0) {
            printf ("I2C writeToSerialPort(0x%02X) write(%zd/%zd): %s\n", sd->dev_addr, t, s,
                                        strerror(errno));
            break;
        } else if (n == 0) {
            printf ("I2C writeToSerialPort(0x%02X) write(%zd/%zd): EOF\n", sd->dev_addr, t, s);
            break;
        } else
            t += n;
        tcdrain (sd->port);
    }

#ifdef RAW_VERBOSE
    printf("WRITE dev 0x%02x %u: ", sd->dev_addr, (int)s);
    for (size_t i = 0; i < s; i++)
        printf("%02x ", 0xff & b[i]);
    printf("\n");
#endif

    return (t);
}


// ******************************  CCITT CRC  *********************************

static const uint16_t crc_table[256] =
{
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

static void crc_update(I2CDriver *sd, const uint8_t *data, size_t data_len)
{
    unsigned int tbl_idx;
    uint16_t crc = sd->e_ccitt_crc;

    while (data_len--) {
        tbl_idx = ((crc >> 8) ^ *data) & 0xff;
        crc = (crc_table[tbl_idx] ^ (crc << 8)) & 0xffff;
        data++;
    }
    sd->e_ccitt_crc = crc;
}


// ******************************  I2CDriver  *********************************

bool i2c_connect(I2CDriver *sd, const char* portname)
{
    int i;

    sd->connected = 0;
    sd->port = openSerialPort(portname);
    if (sd->port == -1) {
        printf ("I2C: i2c_connect(0x%02X,%s) open failed\n", sd->dev_addr, portname);
        return (false);
    }
    if (writeToSerialPort(sd,
        (uint8_t*)"@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@", 64) != 64) {
        printf ("I2C: i2c_connect(0x%02X,%s) init string failed\n", sd->dev_addr, portname);
        close (sd->port);
        sd->port = -1;
        return (false);
    }

    const uint8_t tests[] = "A\r\n\0xff";
    for (i = 0; i < 4; i++) {
        uint8_t tx[2] = {'e', tests[i]};
        if (writeToSerialPort(sd, tx, 2) != 2) {
            printf ("I2C: i2c_connect(0x%02X,%s) test string write failed\n", sd->dev_addr, portname);
            close (sd->port);
            sd->port = -1;
            return (false);
        }
        uint8_t rx[1];
        size_t n = readFromSerialPort(sd, rx, 1);
        if ((n != 1) || (rx[0] != tests[i])) {
            printf ("I2C: i2c_connect(0x%02X,%s) test string readback failed\n", sd->dev_addr, portname);
            close (sd->port);
            sd->port = -1;
            return (false);
        }
    }

    if (!i2c_getstatus(sd)) {
        printf ("I2C: i2c_connect(0x%02X,%s) getstatus failed\n", sd->dev_addr, portname);
        close (sd->port);
        sd->port = -1;
        return (false);
    }

    sd->connected = 1;
    sd->e_ccitt_crc = sd->ccitt_crc;
    return (true);
}


static bool charCommand(I2CDriver *sd, char c)
{
    return (writeToSerialPort(sd, (uint8_t*)&c, 1) == 1);
}


static bool i2c_ack(I2CDriver *sd)
{
    uint8_t a[1];
    if (readFromSerialPort(sd, a, 1) != 1) {
        printf ("I2C: i2c_ack(0x%02X) failed\n", sd->dev_addr);
        return (false);
    }
    if ((a[0] & 1) != 0)
        return (true);
    else {
        printf ("I2C: i2c_ack(0x%02X) bad value\n", sd->dev_addr);
        return (false);
    }
}


bool i2c_getstatus(I2CDriver *sd)
{
    #define _I2C_GETSTATSIZE 80
    uint8_t readbuffer[_I2C_GETSTATSIZE+1];             // +1 for EOS
    size_t bytesRead;
    char mode;

    if (!charCommand(sd, '?')) {
        printf ("I2C: i2c_getstatus(0x%02X) failed\n", sd->dev_addr);
        return (false);
    }

    bytesRead = readFromSerialPort(sd, readbuffer, _I2C_GETSTATSIZE);
    if (bytesRead != _I2C_GETSTATSIZE) {
        printf ("I2C: i2c_getstatus(0x%02X) failed\n", sd->dev_addr);
        return (false);
    }

    readbuffer[_I2C_GETSTATSIZE] = 0;
    int n_scan = sscanf((char*)readbuffer, "[%15s %8s %" SCNu64 " %f %f %f %c %u %u %u %u %x ]",
        sd->model,
        sd->serial,
        &sd->uptime,
        &sd->voltage_v,
        &sd->current_ma,
        &sd->temp_celsius,
        &mode,
        &sd->sda,
        &sd->scl,
        &sd->speed,
        &sd->pullups,
        &sd->ccitt_crc
        );
    if (n_scan != 12) {
        printf ("I2C: i2c_getstatus(0x%02X) bogus response\n", sd->dev_addr);
        return (false);
    }

    sd->mode = mode;

    #ifdef I2C_VERBOSE
        printf ("I2C mode %c sda %u scl %u\n", mode, sd->sda, sd->scl);
    #endif

    return (true);
}


void i2c_scan(I2CDriver *sd, uint8_t devices[128])
{
    charCommand(sd, 'd');
    (void)readFromSerialPort(sd, devices + 8, 112);
}


bool i2c_reset(I2CDriver *sd)
{
    if (!charCommand(sd, 'x')) {
        printf ("I2C: i2c_reset(0x%02X) write failed\n", sd->dev_addr);
        return (0);
    }

    uint8_t a[1];
    if (readFromSerialPort(sd, a, 1) != 1) {
        printf ("I2C: i2c_reset(0x%02X) read failed\n", sd->dev_addr);
        return (0);
    }

    if ((a[0] & 0x3) == 0x3)            // bits SDA and SCL both 1 indicates bus is free
        return (true);

    printf ("I2C: i2c_reset(0x%02X) bus is froze\n", sd->dev_addr);
    return (false);
}


void i2c_reboot(I2CDriver *sd)
{
    if (!charCommand(sd, '_'))
        printf ("I2C: i2c_reboot(0x%02X) failed\n", sd->dev_addr);
    else
        usleep (500000);

    // no response
}


bool i2c_start(I2CDriver *sd, uint8_t dev, uint8_t op)
{
    sd->dev_addr = dev;
    uint8_t start[2] = {'s', (uint8_t)((dev << 1) | op)};

    if (writeToSerialPort(sd, start, sizeof(start)) != sizeof(start)) {
        printf ("I2C: i2c_start(0x%02X,%d) failed\n", sd->dev_addr, dev);
        return (false);
    }

    bool ack = i2c_ack(sd);

    #ifdef I2C_VERBOSE
        if (ack)
            printf ("START 0x%02X %s\n", dev, op ? "read" : "write");
        else
            printf ("START failed\n");
    #endif

    return (ack);
}


bool i2c_stop(I2CDriver *sd)
{
    if (!charCommand(sd, 'p')) {
        printf ("I2C: i2c_stop(0x%02X) failed\n", sd->dev_addr);
        return (false);
    }

    #ifdef I2C_VERBOSE
        printf("STOP\n");
    #endif

    return (true);
}


bool i2c_write(I2CDriver *sd, const uint8_t bytes[], size_t nn)
{
    bool ack = true;
    size_t i;

    for (i = 0; i < nn; i += 64) {
        size_t len = ((nn - i) < 64) ? (nn - i) : 64;
        uint8_t cmd[65] = {(uint8_t)(0xc0 + len - 1)};
        memcpy(cmd + 1, bytes + i, len);
        if (writeToSerialPort(sd, cmd, 1 + len) != 1+len) {
            printf ("I2C: i2c_write(0x%02X,%zd) failed\n", sd->dev_addr, nn);
            return (false);
        }
        if (!i2c_ack(sd)) {
            printf ("I2C: i2c_write(0x%02X,%zd) ack failed\n", sd->dev_addr, nn);
            return (false);
        }
    }
    crc_update(sd, bytes, nn);

    #ifdef I2C_VERBOSE
        if (ack) {
            printf ("WRITE %ld:", nn);
                for (size_t i = 0; i < nn; i++)
                printf (" 0x%02X", bytes[i]);
                printf ("\n");
        } else {
            printf ("WRITE failed\n");
        }
    #endif

    return ack;
}


bool i2c_read(I2CDriver *sd, uint8_t bytes[], size_t nn)
{
    size_t i;

    for (i = 0; i < nn; i += 64) {
        size_t len = ((nn - i) < 64) ? (nn - i) : 64;
        uint8_t cmd[1] = {(uint8_t)(0x80 + len - 1)};
        if (writeToSerialPort(sd, cmd, 1) != 1) {
            printf ("I2C: i2c_read(0x%02X,%zd) write failed\n", sd->dev_addr, nn);
            return (false);
        }
        if (readFromSerialPort(sd, bytes + i, len) != len) {
            printf ("I2C: i2c_read(0x%02X,%zd) read failed\n", sd->dev_addr, nn);
            return (false);
        }
        crc_update(sd, bytes + i, len);
    }

    #ifdef I2C_VERBOSE
        printf ("READ  %ld:", nn);
        for (size_t i = 0; i < nn; i++)
            printf (" 0x%02X", bytes[i]);
        printf ("\n");
    #endif

    return (true);
}


void i2c_monitor(I2CDriver *sd, int enable)
{
    charCommand(sd, enable ? 'm' : '@');
}


void i2c_capture(I2CDriver *sd)
{
    printf("Capture started\n");
    charCommand(sd, 'c');
    uint8_t bytes[1];

    int starting = 0;
    int nbits = 0, bits = 0;
    while (1) {
        int i;
        readFromSerialPort(sd, bytes, 1);
        for (i = 0; i < 2; i++) {
            int symbol = (i == 0) ? (bytes[0] >> 4) : (bytes[0] & 0xf);
            switch (symbol) {
            case 0:
                break;
            case 1:
                starting = 1;
                break;
            case 2:
                printf("STOP\n");
                starting = 1;
                break;
            case 8:
            case 9:
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
            case 15:
                bits = (bits << 3) | (symbol & 7);
                nbits += 3;
                if (nbits == 9) {
                    int b8 = (bits >> 1), ack = !(bits & 1);
                    if (starting) {
                        starting = 0;
                        printf("START %02x %s", b8 >> 1, (b8 & 1) ? "READ" : "WRITE");
                    } else {
                        printf("BYTE %02x", b8);
                    }
                    printf(" %s\n", ack ? "ACK" : "NAK");
                    nbits = 0;
                    bits = 0;
                }
                break;
            }
        }
    }
}


int i2c_commands(I2CDriver *sd, int argc, char *argv[])
{
    int i;

    for (i = 0; i < argc; i++) {
        char *token = argv[i];
        // printf("token [%s]\n", token);
        if (strlen(token) != 1)
            goto badcommand;
        switch (token[0]) {

        case 'i':
            i2c_getstatus(sd);
            printf("uptime %" SCNu64"  %.3f V  %.0f mA  %.1f C SDA=%d SCL=%d speed=%d kHz\n",
                        sd->uptime, sd->voltage_v, sd->current_ma, sd->temp_celsius, sd->sda,
                        sd->scl, sd->speed);
            break;

        case 'x':
            {
                i2c_reset(sd);
            }
        break;

        case 'd':
            {
                uint8_t devices[128];
                int i;

                i2c_scan(sd, devices);
                printf("\n");
                for (i = 8; i < 0x78; i++) {
                    if (devices[i] == '1')
                        printf("%02x  ", i);
                    else
                        printf("--  ");
                    if ((i % 8) == 7)
                        printf("\n");
                }
                printf("\n");
            }
        break;

        case 'w':
            {
                token = argv[++i];
                unsigned int dev = strtol(token, NULL, 0);

                token = argv[++i];
                uint8_t bytes[8192];
                char *endptr = token;
                size_t nn = 0;
                while (nn < sizeof(bytes)) {
                    bytes[nn++] = strtol(endptr, &endptr, 0);
                    if (*endptr == '\0')
                        break;
                    if (*endptr != ',') {
                        fprintf(stderr, "Invalid bytes '%s'\n", token);
                        return 1;
                    }
                    endptr++;
                }

                i2c_start(sd, dev, 0);
                i2c_write(sd, bytes, nn);
            }
        break;

        case 'r':
            {
                token = argv[++i];
                unsigned int dev = strtol(token, NULL, 0);

                token = argv[++i];
                size_t nn = strtol(token, NULL, 0);
                uint8_t bytes[8192];

                i2c_start(sd, dev, 1);
                i2c_read(sd, bytes, nn);
                i2c_stop(sd);

                size_t i;
                for (i = 0; i < nn; i++)
                    printf("%s0x%02x", i ? "," : "", 0xff & bytes[i]);
                printf("\n");
        }
        break;

        case 'p':
            i2c_stop(sd);
            break;

        case 'm':
            {
                char line[100];

                i2c_monitor(sd, 1);
                printf("[Hit return to exit monitor mode]\n");
                if (!fgets(line, sizeof(line) - 1, stdin))
                    return 0;
                i2c_monitor(sd, 0);
            }
        break;

        case 'c':
            i2c_capture(sd);
            break;

        default:
            badcommand:
            fprintf(stderr, "Bad command '%s'\n", token);
                fprintf(stderr, "\n");
                fprintf(stderr, "Commands are:");
                fprintf(stderr, "\n");
                fprintf(stderr, "  i              display status information (uptime, voltage, current, temperature)\n");
                fprintf(stderr, "  x              I2C bus reset\n");
                fprintf(stderr, "  d              device scan\n");
                fprintf(stderr, "  w dev <bytes>  write bytes to I2C device dev\n");
                fprintf(stderr, "  p              send a STOP\n");
                fprintf(stderr, "  r dev N        read N bytes from I2C device dev, then STOP\n");
                fprintf(stderr, "  m              enter I2C bus monitor mode\n");
                fprintf(stderr, "  c              enter I2C bus capture mode\n");
                fprintf(stderr, "\n");

                return 1;
        }
    }

    return 0;
}
