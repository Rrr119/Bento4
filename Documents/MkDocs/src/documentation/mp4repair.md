# mp4repair
```
MP4 File Repair - Version 1.0
(Bento4 Version 1.6.0.0)
(c) 2002-2026 Axiomatic Systems, LLC

usage: mp4repair [options] <input> <output>
options are:
  --verbose       print details about recovered atoms and samples
```

`mp4repair` rebuilds the movie index of an MP4 file when the normal Bento4
parser rejects a damaged `moov` atom. The input is opened read-only and is
never modified. The output is a new file with a valid, fast-start layout
(`ftyp`, `moov`, then `mdat`).

## Recovery Strategy

The tool does not re-encode media. It performs these steps:

1. Scans top-level boxes and resynchronizes when atom sizes are invalid.
2. Recovers `trak`, `stsd`, `stsz`/`stz2`, `stsc`, `stco`/`co64`, `stts`,
   `ctts`, and `stss` data even when a parent atom has a bad size.
3. Rebuilds a synthetic sample table and writes a new `moov` atom with
   corrected chunk offsets.
4. Copies the original encoded samples into a new `mdat` atom.

The scanner can recover files where the `moov` size is too small, too large,
zero, or spans the media data. It can also recover a damaged `moov` type when
the `mvhd` or `trak` atoms are still present.

If the sample tables are incomplete, two elementary-stream fallbacks are
available:

- Annex B H.264 with recoverable SPS/PPS data
- AAC in ADTS framing

AVCC-length-prefixed H.264 without a recoverable `avcC`, and raw AAC without a
sample table, cannot be decoded reliably. In those cases `mp4repair` exits
with an error instead of producing a misleading output file.

## Examples

Repair a damaged file:
```
mp4repair broken.mp4 fixed.mp4
```

Show what was recovered:
```
mp4repair --verbose broken.mp4 fixed.mp4
```

Validate the result with the other Bento4 tools:
```
mp4info fixed.mp4
mp4dump --verbosity 1 fixed.mp4
```

## Limitations

- Media is copied, not transcoded. Missing or undecodable payload bytes are
  not reconstructed.
- Encrypted tracks need their protection metadata to remain recoverable.
- Fragmented MP4 files should generally be repaired from their initialization
  segment and media fragments rather than treated as a single indexed file.
