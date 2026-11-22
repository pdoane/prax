# exr scoreboard

The cases that a plain run passes, by part of the suite, and the rate. The run writes this table, and
the diff of a change to it is what the change did.

| Part           |   Cases | Passing |
|----------------|--------:|--------:|
| openexr-images | 283/283 |  100.0% |
| recompressed   | 100/100 |  100.0% |
| written        |   70/70 |  100.0% |
| **total**      | 453/453 |  100.0% |

Of the 453 files, 184 are refused by both decoders, and 14 hold deep data alone, which lib/exr does not
read. 636748087 samples are compared, and 18502928 of data windows outside their display windows are not. The parts
that decode are, by compression: none 17, rle 17, zips 110, zip 36, piz 53, pxr24 25, b44 20, b44a 18, dwaa 17, dwab 17.
