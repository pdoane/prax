# Damaged inputs

`suites/prax-decoders` holds inputs whose decoders must survive them damaged: compressed streams and encoded images,
each a file of a format's own extension, and fonts. A decoder given one of its inputs with any byte changed, or cut
short, fails or decodes to something, and never reads or writes past what it was given. The runner
(`tools/suite/prax/decoders.py`) and its harness (`tools/suite/prax/decoder_check.cpp`) say how each case is checked.

The files are small on purpose: a case changes every byte of its file in turn, and decodes each change. A file earns
its place by reaching a part of its decoder that the others do not, such as a block type, a window size, a checksum or
a dictionary.

| directory | files | made by |
| --- | --- | --- |
| `zstd` | Zstandard frames, with checksums and content sizes, at levels 1 to 19 and small windows | the reference encoder, zstd 1.5 |
| `brotli` | Brotli streams at qualities 0 to 11, with small windows, of text that the encoder codes with dictionary words | the reference encoder |
| `lzfse` | LZFSE streams of LZVN, of both header versions, and with the farthest match LZFSE codes | Apple's encoder |
| `webp` | lossless, lossy, alpha, animated and extended files, one per feature | libwebp 1.6.0's tools (`lib/webp/tests/gen_webp_vectors.py`) |

`fonts.txt` lists fonts of the build's test fonts (`cmake/test_data.cmake`) by name, each with the tables to cut. A
font's case cuts the table to each of its lengths, and with its first offsets past its end, and reads it through every
call that reads the table.

A plain run checks the files in the default build, where a decoder that breaks its contract fails its case. The
sanitizer build sees what a case cannot, a read or a write out of bounds: `./dev.sh suite asan prax-decoders`.
