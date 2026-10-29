// standalone unit test for nextup_score.h
// build and run:  g++ -std=c++17 -Wall -I.. -o nextup_score_test nextup_score_test.cpp && ./nextup_score_test

#include <stdio.h>
#include <string.h>
#include "nextup_score.h"

static int n_fail;

#define CHECK(cond, ...) do { if (!(cond)) { n_fail++; printf ("FAIL line %d: ", __LINE__); printf (__VA_ARGS__); printf ("\n"); } } while (0)

// a well-reached, fresh candidate with a log loaded, nothing worked yet
static NUInputs base()
{
    NUInputs in;
    memset (&in, 0, sizeof(in));
    in.have_log = true;
    in.grid_known = true;
    in.have_tx_obs = true;
    in.tx_on_band = 5;
    in.tx_max_km = 10000;
    in.spot_km = 9000;
    in.have_rx_obs = true;
    in.rx_support = 3;
    in.age_s = 60;
    return in;
}

int main()
{
    // brand new DXCC, strongly reached, fresh: top score
    {
        NUInputs in = base();
        NUResult r = nuScore (in);
        CHECK (r.reason == NU_DXCC, "reason %d", r.reason);
        CHECK (r.score == 100, "score %d", r.score);
        CHECK (r.reach_known && !r.unconfirmed, "flags");
    }

    // need cascade: each worked step falls to the next reason and value
    {
        NUInputs in = base();
        in.dxcc_worked = true;
        NUResult r = nuScore (in);
        CHECK (r.reason == NU_BAND && fabsf (r.need - 0.7F) < 1e-6, "band: %d %g", r.reason, r.need);

        in.dxcc_band_worked = true;
        r = nuScore (in);
        CHECK (r.reason == NU_GRID && fabsf (r.need - 0.5F) < 1e-6, "grid: %d %g", r.reason, r.need);

        in.grid_worked = true;
        r = nuScore (in);
        CHECK (r.reason == NU_PREFIX && fabsf (r.need - 0.4F) < 1e-6, "prefix: %d %g", r.reason, r.need);

        in.prefix_worked = true;
        r = nuScore (in);
        CHECK (r.reason == NU_WORKED && r.score <= 5, "worked: %d score %d", r.reason, r.score);
    }

    // grid unknown is skipped, not treated as new
    {
        NUInputs in = base();
        in.dxcc_worked = in.dxcc_band_worked = true;
        in.grid_known = false;
        NUResult r = nuScore (in);
        CHECK (r.reason == NU_PREFIX, "grid unknown should fall to prefix, got %d", r.reason);
    }

    // watch list boosts need but never past 1 and never for a worked spot
    {
        NUInputs in = base();
        in.dxcc_worked = in.dxcc_band_worked = true;
        in.wanted_list = true;
        NUResult r = nuScore (in);
        CHECK (fabsf (r.need - 0.625F) < 1e-6, "boosted grid need %g", r.need);

        in = base();
        in.wanted_list = true;
        r = nuScore (in);
        CHECK (r.need == 1.0F, "boost capped, got %g", r.need);

        in = base();
        in.dxcc_worked = in.dxcc_band_worked = in.grid_worked = in.prefix_worked = true;
        in.wanted_list = true;
        r = nuScore (in);
        CHECK (fabsf (r.need - 0.05F) < 1e-6, "worked stays unboosted, got %g", r.need);
    }

    // no log: neutral need, flagged unknown
    {
        NUInputs in = base();
        in.have_log = false;
        NUResult r = nuScore (in);
        CHECK (r.reason == NU_UNKNOWN && r.need == 0.5F, "no log: %d %g", r.reason, r.need);
    }

    // unconfirmed: DE heard elsewhere but not on this band drags reach down
    {
        NUInputs in = base();
        in.tx_on_band = 0;
        in.tx_max_km = 0;
        in.rx_support = 3;
        NUResult r = nuScore (in);
        CHECK (r.unconfirmed, "should be unconfirmed");
        CHECK (fabsf (r.reach - 0.4F) < 1e-6, "reach %g (0.6*0 + 0.4*1.0)", r.reach);

        // same candidate once DE is heard on band
        in.tx_on_band = 5;
        in.tx_max_km = 9000;
        NUResult r2 = nuScore (in);
        CHECK (!r2.unconfirmed && r2.score > r.score, "heard on band must score higher: %d vs %d", r2.score, r.score);
    }

    // DE not heard anywhere at all is NOT punished as unconfirmed: probably just not transmitting
    {
        NUInputs in = base();
        in.have_tx_obs = false;
        in.tx_on_band = 0;
        in.rx_support = 3;
        NUResult r = nuScore (in);
        CHECK (!r.unconfirmed, "not unconfirmed when no tx data");
        CHECK (fabsf (r.reach - 1.0F) < 1e-6, "reach from rx alone, got %g", r.reach);
    }

    // rx floor: no corroboration is unproven, not closed
    {
        NUInputs in = base();
        in.have_tx_obs = false;
        in.rx_support = 0;
        NUResult r = nuScore (in);
        CHECK (fabsf (r.reach - 0.2F) < 1e-6, "rx floor, got %g", r.reach);
    }

    // coverage: reports that fell short of the needed distance count for less
    {
        NUInputs a = base(), b = base();
        a.tx_max_km = 9000;
        b.tx_max_km = 3000;
        CHECK (nuScore(a).reach > nuScore(b).reach, "farther reports must count for more");
    }

    // no evidence anywhere: neutral reach, not known
    {
        NUInputs in = base();
        in.have_tx_obs = false;
        in.have_rx_obs = false;
        NUResult r = nuScore (in);
        CHECK (!r.reach_known && r.reach == 0.5F, "neutral reach");
    }

    // freshness curve
    CHECK (nuFresh (0) == 1.0F, "fresh 0");
    CHECK (nuFresh (300) == 1.0F, "fresh 5m");
    CHECK (fabsf (nuFresh (17*60 + 30) - 0.6F) < 1e-5, "fresh midway, got %g", nuFresh (17*60+30));
    CHECK (nuFresh (30*60) == 0.2F, "fresh 30m");
    CHECK (nuFresh (3*3600) == 0.2F, "fresh old");
    for (int a = 1; a < 3600; a++)
        CHECK (nuFresh (a) <= nuFresh (a-1) + 1e-6F, "fresh not monotonic at %d", a);

    // multiplicative: a zero factor sinks the whole score
    {
        NUInputs in = base();
        in.dxcc_worked = in.dxcc_band_worked = in.grid_worked = in.prefix_worked = true;
        CHECK (nuScore(in).score <= 5, "worked sinks score");
    }

    // same spot always scores the same
    {
        NUInputs in = base();
        in.age_s = 777;
        CHECK (nuScore(in).score == nuScore(in).score, "deterministic");
    }

    if (n_fail == 0)
        printf ("nextup_score: all tests passed\n");
    return n_fail ? 1 : 0;
}
