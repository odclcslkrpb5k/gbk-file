/* libFuzzer entry point. Each fuzz-<name>.c defines it. In fuzzing builds libFuzzer drives
 * it; in normal builds driver.c replays the seed corpus through it as a regression test. */
#pragma once

int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size);
