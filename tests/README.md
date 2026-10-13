# Armor Enumeration Tests

Run the production enumeration state machine with a stub CAN transmitter.
These are host tests, not board or CAN electrical validation.

From the L431PM directory on Linux/WSL:

```sh
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -IApplication/Referee -IBsp tests/test_armor_enum.c \
    Application/Referee/app_armor_enum.c -o /tmp/l431_test_armor_enum
/tmp/l431_test_armor_enum
```

Covers zero through four boards, incremental assignment, stable UID slots,
periodic discovery, hot insertion, a missing middle slot, reinsertion, missing
ACKs, incomplete fragments and failed start transmissions.

The production CAN scheduler has an additional modeled-board integration test:

```sh
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -IApplication/Referee -IBsp tests/test_armor_hotplug.c \
    -o /tmp/l431_test_armor_hotplug
/tmp/l431_test_armor_hotplug
```

The model clears each board's NodeID on the start broadcast and uses separate
announce delays. It verifies three-board startup, automatic fourth-board join,
middle-board removal and reinsertion, stable IDs and polling gaps under 300 ms
for healthy peers in this modeled traffic. It does not simulate all arbitration
collisions, electrical faults or real LEDs; physical timing remains unverified.
