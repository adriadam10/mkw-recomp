// Harness for test_power.py: looks up a fake power_supply tree and prints "found state percent coarse".
#include <stdio.h>
#include <stdlib.h>

#include "linux_power.h"

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    InputPower out = {INPUT_POWER_UNKNOWN, -1, 0, ""};
    int found = linux_power_lookup_at(argv[1], atoi(argv[2]), argv[3], &out);
    printf("%d %d %d %d\n", found, out.state, out.percent, out.coarse);
    return 0;
}
