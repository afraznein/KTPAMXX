// Exercises KTPKillHold -- the Damage-hook kill that waits for DeathMsg to name the
// weapon the engine logged. Built and run by ci.yml twice: once with -DNEGATIVE_CONTROL,
// which must FAIL, and once without, which must pass.
//
// The negative control is the pre-fix flow, not a planted assert: the Damage hook
// reports at once with the held weapon. The Lane B pairs below (mp44 against kar,
// scopedkar against kar, bar against garandbutt) are the ones that failed the nightly
// frag-context join, so this harness fails on exactly the bug it exists for.
//
// The driver mirrors the call order in modules/dod/dodx/usermsg.cpp: Client_Health_End
// holds, Client_DeathMsg resolves, CPlayer::PreThink expires, Disconnect forces.
#include <stdio.h>
#include <string.h>
#include "KTPKillHold.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

// weaponData indices (modules/dod/dodx/Utils.cpp).
enum { W_GARAND = 5, W_SCOPEDKAR = 6, W_MP44 = 8, W_KAR = 10, W_BAR = 11, W_GARANDBUTT = 38 };

static const float kWindow = 0.033f;

struct Report { int attacker, victim, weapon, aim, TA; };
static Report reports[16];
static int nReports = 0;

static void Report_(int attacker, int victim, int weapon, int aim, int TA)
{
    if (nReports < 16)
        reports[nReports] = Report{ attacker, victim, weapon, aim, TA };
    nReports++;
}

static KTPKillHold hold;

static void Fresh()
{
    // Garbage first, so a slot that reads active without a hold() is caught.
    memset((void *)&hold, 0x5a, sizeof(hold));
    for (int v = 0; v < KTPKillHold::kSlots; v++)
    {
        KTPHeldKill drop;
        hold.expire(v, 1e9f, true, kWindow, drop);
    }
    nReports = 0;
}

static void DamageDeath(int victim, int attacker, int heldWeapon, int aim, int TA, float now)
{
#ifdef NEGATIVE_CONTROL
    (void)now;
    Report_(attacker, victim, heldWeapon, aim, TA);
#else
    KTPHeldKill k;
    if (hold.expire(victim, now, true, kWindow, k))
        Report_(k.attacker, victim, k.weapon, k.aim, k.TA);
    hold.hold(victim, attacker, heldWeapon, aim, TA, now);
#endif
}

static void DeathMsg(int killer, int victim, int engineWeapon)
{
    KTPHeldKill k;
    if (hold.resolve(victim, killer, engineWeapon, k))
        Report_(k.attacker, victim, k.weapon, k.aim, k.TA);
}

static void Think(int victim, float now, bool force = false)
{
    KTPHeldKill k;
    if (hold.expire(victim, now, force, kWindow, k))
        Report_(k.attacker, victim, k.weapon, k.aim, k.TA);
}

static void EngineNamesTheKill(const char *what, int heldWeapon, int engineWeapon, int aim)
{
    Fresh();
    DamageDeath(5, 11, heldWeapon, aim, 0, 246.90f);
    DeathMsg(11, 5, engineWeapon);
    Think(5, 247.00f);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: exactly one report", what);
    CHECK(nReports == 1, msg);
    snprintf(msg, sizeof(msg), "%s: reported under the engine's weapon", what);
    CHECK(nReports >= 1 && reports[0].weapon == engineWeapon, msg);
    snprintf(msg, sizeof(msg), "%s: hitplace survives the hold", what);
    CHECK(nReports >= 1 && reports[0].aim == aim, msg);
    snprintf(msg, sizeof(msg), "%s: killer and victim unchanged", what);
    CHECK(nReports >= 1 && reports[0].attacker == 11 && reports[0].victim == 5, msg);
}

int main()
{
    // ---------------- the Lane B disagreements ----------------
    // Run 42970288647: held mp44 against engine kar on a headshot (which is also the
    // headshot 11-vs-10 shortfall), and held scopedkar against engine kar.
    EngineNamesTheKill("mp44 held, kar logged, headshot", W_MP44, W_KAR, 1);
    EngineNamesTheKill("scopedkar held, kar logged", W_SCOPEDKAR, W_KAR, 0);
    // Run 40907170635: a butt kill.
    EngineNamesTheKill("bar held, garandbutt logged", W_BAR, W_GARANDBUTT, 0);
    EngineNamesTheKill("garand held, garandbutt logged", W_GARAND, W_GARANDBUTT, 0);

    // ---------------- agreement is unchanged ----------------
    EngineNamesTheKill("kar held, kar logged", W_KAR, W_KAR, 2);

    // ---------------- the engine's name is not always usable ----------------
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 0, 10.0f);
        DeathMsg(7, 5, W_KAR);
        CHECK(nReports == 1, "a different killer still releases the held kill");
        CHECK(nReports == 1 && reports[0].weapon == W_MP44,
            "a name for a different killer is not this killer's weapon");
        CHECK(nReports == 1 && reports[0].attacker == 11,
            "the hook's attacker stands; DeathMsg only supplies a weapon");
    }
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 0, 10.0f);
        DeathMsg(11, 5, 0);
        CHECK(nReports == 1 && reports[0].weapon == W_MP44,
            "an unresolved logname (index 0) keeps the held read");
    }
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 1, 10.0f);
        DeathMsg(11, 5, W_KAR);
        CHECK(nReports == 1 && reports[0].TA == 1,
            "the hook's teamkill flag survives; DeathMsg carries none");
    }

    // ---------------- DeathMsg never comes ----------------
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 0, 10.0f);
        Think(5, 10.0f);
        Think(5, 10.0f + kWindow * 0.5f);
        CHECK(nReports == 0, "held through the window, not reported early");
        Think(5, 10.0f + kWindow * 1.5f);
        CHECK(nReports == 1 && reports[0].weapon == W_MP44,
            "reported with the held read once the window closes");
        DeathMsg(11, 5, W_KAR);
        CHECK(nReports == 1, "a DeathMsg after expiry finds nothing to release");
    }
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 0, 10.0f);
        Think(5, 10.001f, true);
        CHECK(nReports == 1, "a forced flush (disconnect, stats reset) reports at once");
    }

    // ---------------- a clock restart drops, never credits ----------------
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 0, 900.0f);
        Think(5, 1.5f);
        CHECK(nReports == 0, "a kill stamped after now predates a map change and is dropped");
        CHECK(!hold.holding(5), "and the slot is clear afterwards");
        Think(5, 1.5f, true);
        CHECK(nReports == 0, "a forced flush does not resurrect it");
    }

    // ---------------- one slot per victim ----------------
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 0, 10.0f);
        DamageDeath(6, 7, W_SCOPEDKAR, 0, 0, 10.0f);
        DeathMsg(7, 6, W_KAR);
        CHECK(nReports == 1 && reports[0].victim == 6 && reports[0].weapon == W_KAR,
            "one victim's DeathMsg releases only that victim");
        CHECK(hold.holding(5), "the other victim is still held");
        DeathMsg(11, 5, W_KAR);
        CHECK(nReports == 2 && reports[1].victim == 5 && reports[1].weapon == W_KAR,
            "and is released by its own DeathMsg");
    }
    {
        Fresh();
        DamageDeath(5, 11, W_MP44, 0, 0, 10.0f);
        DamageDeath(5, 12, W_KAR, 0, 0, 20.0f);
        CHECK(nReports == 1 && reports[0].attacker == 11,
            "a second hold on a slot reports the first rather than overwriting it");
    }
    {
        Fresh();
        hold.hold(0, 11, W_MP44, 0, 0, 10.0f);
        hold.hold(KTPKillHold::kSlots, 11, W_MP44, 0, 0, 10.0f);
        CHECK(!hold.holding(0) && !hold.holding(KTPKillHold::kSlots),
            "out-of-range victims hold nothing");
        KTPHeldKill k;
        CHECK(!hold.resolve(3, 11, W_KAR, k), "nothing held means nothing resolved");
    }

    if (fails)
    {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("all kill-hold checks passed\n");
    return 0;
}
