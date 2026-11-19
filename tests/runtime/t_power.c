// Harness for test_power.py: "isxbox <name>" prints whether the name counts as an Xbox pad; otherwise it looks up
// a fake power_supply tree and prints "found state percent coarse".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_power.h"

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "isxbox")) {
        printf("%d\n", linux_power_is_xbox(argv[2]));
        return 0;
    }
    if (argc != 4) return 2;
    InputPower out = {INPUT_POWER_UNKNOWN, -1, 0, ""};
    int found = linux_power_lookup_at(argv[1], atoi(argv[2]), argv[3], &out);
    printf("%d %d %d %d\n", found, out.state, out.percent, out.coarse);
    return 0;
}
