// Assertions over the controller layer's core. Used by the gameinput_tests executable on macOS and
// by `input_lab --selftest` on the iPad, so the same checks run on the device.
#pragma once

namespace gameinput
{
// Runs every check, reporting each line through `log`. Returns the number of failed checks.
int RunSelfTests(void (*log)(const char* line, void* user), void* user);
} // namespace gameinput
