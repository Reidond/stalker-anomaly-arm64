#include <cstdio>

#include "SelfTest.h"

int main()
{
    const int failures = gameinput::RunSelfTests([](const char* line, void*) { std::puts(line); }, nullptr);
    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
