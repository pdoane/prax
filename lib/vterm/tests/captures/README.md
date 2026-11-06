# Captures

Recorded program output that `test_vterm_captures.cpp` plays into a terminal.

Each `.tty` file holds every byte that a program wrote to a pseudoterminal until it exited. The pseudoterminal had the
extent in the table below, which is also the extent that the test gives the terminal. The program ran with
`TERM=xterm-256color` and `COLORTERM=truecolor`. Each `.pipe` file holds what the same program writes to a pipe. A test
compares that text with the screen that the capture leaves.

The programs are pieces of [ascii.rest](https://ascii.rest/), run with npx. Their art is under the MIT License
(THIRD_PARTY_NOTICES.md).

| capture | extent | command |
| --- | --- | --- |
| `banner` | 60x12 | `npx ascii.rest banner prax --seconds 1` |
| `donut` | 80x24 | `npx ascii.rest donut --seconds 0.01`, which draws one frame |
| `ocean-sunset` | 100x30 | `npx ascii.rest ocean-sunset --seconds 0.01` |
| `doom-fire` | 80x24 | `npx ascii.rest doom-fire --seconds 1` |

Each `.pipe` file is the output of the same command without `--seconds`, redirected to the file.
