/* North American time zones map overlay.
 *
 * Shows the time zones of Canada, the USA and Mexico on the HamClock map, tinted by zone, with the
 * zone borders and a live clock label in each zone (EDT 9:15p, CST 8:15p, ...).
 *
 * Based on the NorthAmericanTimeZones map by Vince d'Eon VE6LK and Tom Salzer KJ7T (CC0).
 * Geometry is Natural Earth admin-1 (public domain), simplified by tools/gen_natzones.py.
 */

#ifndef _NATZONES_H
#define _NATZONES_H

#include <stdint.h>

/* one entry per time zone shown. N.B. keep in the same order as ZONES in tools/gen_natzones.py
 * and as natz_rules[] in natzones.cpp
 */
typedef enum {
    NZ_BCT,             // British Columbia, permanent time
    NZ_YKT,             // Yukon
    NZ_NWT,             // Northwest Territories, permanent time
    NZ_ABT,             // Alberta, permanent time
    NZ_SKT,             // Saskatchewan
    NZ_MBT,             // Manitoba, permanent time
    NZ_EAST,            // Eastern (Ontario, Quebec, Nunavut east, US Eastern)
    NZ_ATL,             // Atlantic (NB, NS, PEI, most of Labrador)
    NZ_NLT,             // Newfoundland
    NZ_PAC,             // Pacific
    NZ_MTN,             // Mountain
    NZ_ARIZ,            // Arizona
    NZ_CEN,             // Central
    NZ_AK,              // Alaska
    NZ_HI,              // Hawaii
    NZ_PR,              // Puerto Rico and US Virgin Islands
    NZ_MXN,             // Mexico noroeste (Baja California)
    NZ_MXP,             // Mexico pacifico
    NZ_MXC,             // Mexico centro
    NZ_MXS,             // Mexico sureste (Quintana Roo)
    NZ_N
} NATZoneId;

/* generated geometry, see natzones_data.cpp. coordinates are degrees*100 as (lat, lng) pairs.
 */
typedef struct {
    uint8_t zone;               // NATZoneId
    int32_t first;              // index of first vertex in natz_fill_pts (vertex = lat,lng pair)
    int32_t n;                  // number of vertices (ring is implicitly closed)
} NATZRing;

typedef struct {
    int32_t first;              // index of first vertex in natz_border_pts
    int32_t n;                  // number of vertices (open polyline)
} NATZLine;

typedef struct {
    uint8_t zone;               // NATZoneId
    int16_t lat, lng;           // degrees*100
    uint8_t tier;               // 1 = most important, drawn first; larger tiers are drawn only if there is room
} NATZLabel;

extern const int16_t natz_fill_pts[];
extern const NATZRing natz_rings[];
extern const int natz_n_rings;
extern const int16_t natz_border_pts[];
extern const NATZLine natz_borders[];
extern const int natz_n_borders;
extern const NATZLabel natz_labels[];
extern const int natz_n_labels;

#endif // _NATZONES_H
