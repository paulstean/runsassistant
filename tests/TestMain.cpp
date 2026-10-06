// Test runner entry point. All TEST_CASEs self-register.

#include <cstdio>

#include "TestFramework.h"

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);   // unbuffered: survives crashes
    std::printf ("%d registered\n", (int) ::coretest::registry().size());
    int ran = 0;
    for (const auto& c : ::coretest::registry())
    {
        ++ran;
        const int before = ::coretest::failures();
        c.fn();
        if (::coretest::failures() == before)
            std::printf ("ok %s\n", c.name);
        else
            std::printf ("NOT-OK %s\n", c.name);
    }
    std::printf ("%d cases, %d checks, %d failures\n",
                 ran, ::coretest::checks(), ::coretest::failures());
    return ::coretest::failures() == 0 ? 0 : 1;
}
