# URLs

`lib/url` depends on `lib/core`, on `lib/unicode` for the Unicode processing of domains, and on `lib/charset` for the
encodings of queries. Other libraries reach it through `pr/url.h`.

- `Url` (`src/url_record.h`) stores each component as its serialization. A non-opaque path has `/` before each
  segment. A null host, query or fragment is the null string.
- `urlParse` is the spec's state machine, with one `case` per state. `urlParseFrom` runs it from a state override, and
  every setter implements the spec's steps around that call. Input is UTF-8, read one byte at a time.
- A parse takes the charset that its query is percent-encoded in (url/#concept-basic-url-parser's encoding), which
  `lib/charset` encodes. The URL API parses in UTF-8. A document parses in its own charset.
- The domain parser lowercases an ASCII domain and leaves the rest to UTS #46 ToASCII (`pr/unicode_idna.h`), with the flags
  that the URL Standard gives a parser that is not strict.
