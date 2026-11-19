#include "../graphics/include/boot_handoff.h"
#include <cassert>
#include <cstdio>

int main()
{
    AuroraBootHandoff normal;
    assert(!normal.inspect(1000, false, false, false));
    assert(normal.done());
    AuroraBootHandoff restarted;
    assert(!restarted.inspect(45000, true, true, true));
    assert(restarted.done());

    AuroraBootHandoff boot;
    assert(!boot.inspect(1000, true, false, false));
    assert(boot.inspect(5000, true, true, false));
    assert(!boot.hold(false)); // Initial black animation frames are real.
    assert(!boot.hold(true));
    assert(boot.inspect(41000, true, true, true));
    assert(!boot.hold(true));
    assert(boot.hold(false)); // Empty handoff keeps the last boot frame.
    assert(boot.inspect(42999, true, true, true));
    assert(boot.hold(false));
    assert(!boot.hold(true)); // First actual UI frame ends the policy.
    assert(boot.done());
    assert(!boot.inspect(43000, true, true, true));
    assert(!boot.hold(false)); // Later legitimate black is never suppressed.

    AuroraBootHandoff timeout;
    assert(timeout.inspect(5000, true, true, false));
    assert(!timeout.hold(true));
    assert(timeout.inspect(41000, true, true, true));
    assert(timeout.hold(false));
    assert(!timeout.inspect(43000, true, true, true));
    assert(!timeout.hold(false));
    AuroraBootHandoff no_completion;
    assert(no_completion.inspect(5000, true, true, false));
    assert(!no_completion.inspect(125000, true, true, false));
    AuroraBootHandoff no_content;
    assert(no_content.inspect(5000, true, true, false));
    assert(!no_content.hold(false));
    assert(no_content.inspect(41000, true, true, true));
    assert(!no_content.hold(false));
    AuroraBootHandoff power;
    assert(power.inspect(5000, true, true, false));
    power.stop();
    assert(!power.inspect(41000, true, true, true));
    assert(!power.hold(false));
    std::puts("BOOT_HANDOFF_SCOPE_AND_DEADLINES_PASS");
}
