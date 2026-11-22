# webp scoreboard

The cases that a plain run passes, by part of the suite, and the rate. The run writes this table, and
the diff of a change to it is what the change did.

| Part              |   Cases | Passing |
|-------------------|--------:|--------:|
| libwebp-test-data | 131/131 |  100.0% |
| written           | 340/340 |  100.0% |
| **total**         | 471/471 |  100.0% |

Of the 471 files, 471 decode to 501 frames, whose 118984526 bytes of Rgba8 are compared, and 0 are refused by both
decoders. 189 still lossy images' planes are compared. Of the images that say they have no alpha, 4096
pixels decode to alpha other than opaque in libwebp, which lib/webp takes as opaque.
