/* Next Up scoring -- pure functions, deliberately free of any HamClock dependency so they can be unit
 * tested on their own (see test/nextup_score_test.cpp).
 *
 * a candidate spot's score is  Need x Reach x Freshness, each in 0..1, times 100.
 * multiplying means any one factor near zero sinks the spot: a new DXCC that nobody can reach is not worth
 * chasing and neither is a sure thing you have already worked on that band.
 *
 *   Need      do I want this?       from the user's ADIF log, cascading from the most valuable "new" to least.
 *   Reach     can I get through?    from what has actually been observed, never a guess:
 *                                    - TX evidence: PSKReporter-style reports OF DE on the spot's band
 *                                    - RX evidence: other spots on the same band whose spotter is near DE and
 *                                      whose station is near the candidate, ie. the path is demonstrably open
 *   Fresh     will it still be there?  decays with spot age.
 *
 * N.B. there is deliberately no VOACAP/predicted term yet; when neither kind of evidence exists Reach is a
 * neutral 0.5 and reach_known is false so the UI can say "no evidence" instead of implying confidence.
 */

#ifndef NEXTUP_SCORE_H
#define NEXTUP_SCORE_H

#include <math.h>

// why a spot is wanted, strongest first. N.B. NU_UNKNOWN means no ADIF log is loaded so need can not be judged.
typedef enum {
    NU_WORKED,                  // already worked on this band: not wanted
    NU_PREFIX,                  // new prefix
    NU_GRID,                    // new grid
    NU_BAND,                    // DXCC worked, but never on this band
    NU_DXCC,                    // brand new DXCC
    NU_REF,                     // new park/summit/etc: a reference never worked, whatever else is known of the call
    NU_UNKNOWN                  // no log loaded
} NUReason;

// everything the scorer needs to know about one candidate spot.
// need fields are a cascade: the scorer reads them in order and stops at the first "new", so a caller may
// leave later fields false once an earlier one has decided the outcome (this lets the caller skip expensive
// ADIF searches).
typedef struct {

    // need
    bool have_log;              // an ADIF log is loaded
    bool grid_known;            // the spot has a usable grid
    bool dxcc_worked;           // DXCC entity worked on any band
    bool dxcc_band_worked;      // DXCC entity worked on this band
    bool grid_worked;           // grid worked (any band); only read when grid_known
    bool prefix_worked;         // prefix worked (any band)
    bool wanted_list;           // matches the user's watch list: boosts need

    // reference need, only for spots that name an On The Air reference. caller sets has_ref only when some source
    // (ADIF log, POTA hunted parks) is known to hold references for that program at all: a list without any (eg a
    // contest log) can not say a park is new.
    bool has_ref;               // spot has a reference AND we hold references for its program to compare with
    bool ref_worked;            // that reference is in the log; only read when has_ref

    // reach, transmit evidence
    bool have_tx_obs;           // we have any reports of DE at all (ie. DE has been heard somewhere recently)
    int tx_on_band;             // reports of DE on the spot's band
    float tx_max_km;            // farthest report of DE on the spot's band
    float spot_km;              // distance DE to the candidate

    // reach, receive evidence
    bool have_rx_obs;           // at least one spot source is active
    int rx_support;             // other spots on this band with spotter near DE and station near candidate

    // freshness
    int age_s;                  // seconds since spotted
} NUInputs;

typedef struct {
    float need, reach, fresh;   // each 0..1
    int score;                  // 0..100
    NUReason reason;
    bool reach_known;           // false if Reach is the neutral default because no evidence existed
    bool unconfirmed;           // DE has been heard elsewhere but NOT on this band
} NUResult;


/* need in 0..1 and the reason for it
 */
static inline float nuNeed (const NUInputs &in, NUReason &why)
{
    if (!in.have_log) {
        // neutral: neither boost nor sink what we can not judge. but a reference list does not depend on the log,
        // so a reference known to be new still lifts above neutral.
        if (in.has_ref && !in.ref_worked) {
            why = NU_REF;
            return (0.6F);
        }
        why = NU_UNKNOWN;
        return (0.5F);
    }

    float n;
    if (!in.dxcc_worked) {
        why = NU_DXCC;
        n = 1.0F;
    } else if (!in.dxcc_band_worked) {
        why = NU_BAND;
        n = 0.7F;
    } else if (in.grid_known && !in.grid_worked) {
        why = NU_GRID;
        n = 0.5F;
    } else if (!in.prefix_worked) {
        why = NU_PREFIX;
        n = 0.4F;
    } else {
        why = NU_WORKED;
        n = 0.05F;
    }

    // a reference never worked is wanted even when the call itself is not: hunters chase the park, not the call.
    // only lifts, never lowers, so a new DXCC or band still reads as such.
    const float ref_need = 0.6F;
    if (in.has_ref && !in.ref_worked && n < ref_need) {
        why = NU_REF;
        n = ref_need;
    }

    if (why == NU_WORKED)
        return (n);                     // worked: no watch list boost for something we do not need

    if (in.wanted_list)
        n = fminf (1.0F, n * 1.25F);
    return (n);
}

/* reach in 0..1. set known false if there was no evidence either way.
 */
static inline float nuReach (const NUInputs &in, bool &known, bool &unconfirmed)
{
    unconfirmed = in.have_tx_obs && in.tx_on_band == 0;

    // transmit evidence: how many heard us and how far, relative to the distance we need to cover
    bool have_tx = in.have_tx_obs;
    float tx = 0;
    if (have_tx && in.tx_on_band > 0) {
        float count_f = fminf (1.0F, in.tx_on_band / 5.0F);
        float cover_f = in.spot_km > 0 ? fminf (1.0F, in.tx_max_km / in.spot_km) : 1.0F;
        tx = 0.5F * count_f + 0.5F * cover_f;
    }

    // receive evidence: floor of 0.2 because having no corroborating spot means "unproven", not "closed"
    bool have_rx = in.have_rx_obs;
    float rx = 0.2F + 0.8F * fminf (1.0F, in.rx_support / 3.0F);

    if (have_tx && have_rx) {
        known = true;
        return (0.6F * tx + 0.4F * rx);
    }
    if (have_tx) {
        known = true;
        return (tx);
    }
    if (have_rx) {
        known = true;
        return (rx);
    }

    known = false;
    return (0.5F);
}

/* freshness in 0..1: full for 5 minutes then linearly down to 0.2 at 30 minutes, flat after that
 */
static inline float nuFresh (int age_s)
{
    if (age_s <= 5*60)
        return (1.0F);
    if (age_s >= 30*60)
        return (0.2F);
    return (1.0F - 0.8F * (age_s - 5*60) / (25.0F*60));
}

/* score one candidate
 */
static inline NUResult nuScore (const NUInputs &in)
{
    NUResult r;
    r.need = nuNeed (in, r.reason);
    r.reach = nuReach (in, r.reach_known, r.unconfirmed);
    r.fresh = nuFresh (in.age_s);
    r.score = (int) lroundf (100.0F * r.need * r.reach * r.fresh);
    return (r);
}

#endif // NEXTUP_SCORE_H
