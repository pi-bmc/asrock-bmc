# Injected-driver hardware results (2026-07-29 / 2026-07-30)

Fifteen flash/boot cycles on the live board. This supersedes several earlier wrong
conclusions of mine — see "Corrections" at the end.

## The two rules that matter

1. **Never re-encode the `9E21FD93` LZMA container.** Any repack hangs the host at
   POST 0x4F, including a no-op one.
2. **Append to the outer `FVMAIN_COMPACT` free space instead — this WORKS.** The
   AMI DXE core dispatches a plain FFS written there, proven on hardware
   2026-07-30. 597,000 bytes are free at abs `0x1E153F8` and no byte of existing
   content changes.

## PROVEN: injected drivers are dispatched (2026-07-30)

`OobProbeDxe` (2,368 B) was appended as a plain `DXE_DRIVER` FFS at abs
`0x1E153F8` in the live image — no recompression, no existing byte touched. After
one boot, the flash readback contains a new AMI NVAR entry:

```
0x103B254  NVAR size=0x3C attr=0x83  name "OobProbe"
           payload: "BOOPROBE" phase=1 writes=1 tsc=0x58E419C19F
                    caller=7c4d1a92-6f38-4b05-9e2a-3d81c0b7f4e6
```

That span was `0xFF` in the image we flashed, the caller GUID is the probe's own
FILE_GUID, and the boot changed only two 64 KiB blocks (both NVRAM). The module
ran. The image booted to the ordinary 0x99 baseline, exactly like stock.

This overturns the earlier "accepted, **not dispatched**" verdict for this vector.
That verdict came from seeing no IPMI traffic, which we now know proves nothing.

Two further findings fall straight out of the record:

- **Variable write services are already up when our entry point runs.** The
  `SetVariable` issued at entry succeeded on the first try (`writes=1`), so the
  protocol-notify fallback never had to fire.
- **`EndOfDxe` and `ReadyToBoot` never fire on this board.** Both callbacks were
  registered and neither ran — `phase` stayed at 1 (DISPATCHED) instead of
  advancing to 3 or 4. This is the real reason `SmbiosBmcPushDxe` and
  `BiosCfgOobDxe` have never produced IPMI traffic: **they do their work at
  ReadyToBoot, and ReadyToBoot never happens.** Dispatch was never the blocker
  for them; the 0x99 stall is. Any OOB module that must run on this board has to
  act at entry or on an early protocol notify, not on a late boot event.

### How to re-run this test

`tools/outerfv.py add` injects, `tools/probecheck.py` decides. The test is
pre-registered so it cannot be rationalised afterwards: AMI stores NVAR names in
**ASCII** while the driver's own copy of the name sits in its PE32 as **UTF-16**,
so ASCII hits *outside the injected FFS span* can only come from a real variable
write. Confirm the candidate image scores zero before flashing — it is the
negative control.

## Measurement caveat — IPMI count is NOT reliable, POST code is

A boot sometimes logs ~141 IPMI requests (`journalctl | grep -c IPMI-REQ`) — AMI OEM
traffic like `netfn=0x0A cmd=0x23` ×35, `netfn=0x3A cmd=0xB5` pushing the ASCII
string "X570D4I-2T". **But the identical, byte-verified stock image produced 141 on
one boot and 0 on the two following boots.** The KCS bridge
(`phosphor-ipmi-kcs@ipmi-kcs3`, note the hyphen) was active and running throughout,
so this is not a transport outage — the host's IPMI activity is simply intermittent
across boots of the same image.

**Consequence: any conclusion drawn from IPMI count alone is unsound**, including
two I previously recorded here ("nested FV_IMAGE breaks IPMI", "TPM-container
re-encode breaks IPMI"). Those two images may in fact be fine; they are UNPROVEN in
both directions and need re-testing with a reliable signal.

The POST code IS deterministic for *image health*: 0x4F on 7/7 re-encode boots,
0x99 on every other boot. Treat 0x4F vs 0x99 as the health discriminator.

**But POST codes are useless for driver liveness, now measured rather than
assumed.** OobProbeDxe writes the signature `5A A5 5A A5 <phase>` to port 0x80,
one byte per `gBS->Stall(1000)`, before doing anything else. On the boot where the
driver *provably ran* (it left the NVAR entry below), that signature does **not**
appear anywhere in the 266-code history — only one isolated 0x5A and one isolated
0xA5, non-adjacent. So port-80 writes from an injected DXE driver do not reliably
reach the BMC snoop here. Use the NVAR variable, not POST codes.

| What was changed | POST | Verdict |
|---|---|---|
| Nothing (stock) — several boots | 0x99 | baseline (0x99 is a *pre-existing* stall) |
| NVAR store bytes (the 2026-07-27 CSM/VideoOpROM patch) | 0x99 | accepted |
| 16 stray bytes in the outer FV's erased tail | 0x99 | accepted |
| Plain DXE_DRIVER FFS added to outer FV free space | 0x99 | accepted and **DISPATCHED** (proven 2026-07-30) |
| Same driver wrapped as nested FV_IMAGE (type 0x0B) | 0x99 | accepted; runtime effect unproven |
| Re-encode of the **1DF36FF9** (TPM) container, no content change | 0x99 | accepted; runtime effect unproven |
| *Any* re-encode of the **9E21FD93** blob — 7 boots | **0x4F** | **dead at DXE IPL** |

The 9E21FD93 result is the one solid finding: it is deterministic, POST-code based,
and reproduced across python-lzma and EDK2's reference encoder, with and without
exact-length padding, and for a no-op repack.

Note the contrast now visible: re-encoding **1DF36FF9 still reached 0x99**, whereas
re-encoding 9E21FD93 always gives 0x4F. That is consistent with the DxeIpl-vs-DXE-core
extraction-path theory and means **re-encoding a non-main container may well be
viable** — it just has not been proven to actually dispatch anything yet.

## THE INJECTION BUG: UI section size must be 4-aligned

This cost three hardware cycles and produced a string of wrong theories, so it is
worth stating plainly: **AMI's DXE core silently skips any FFS whose UI section
size is not a multiple of 4.** No error, no POST code, and the FFS chain walk
continues past it normally — so the file is simply never dispatched, and the
symptom looks like "every other file works".

Proven with a six-slot ladder of byte-identical drivers differing only in UI name
length:

| Slot | UI name | len | UI section size | Dispatched |
|---|---|---|---|---|
| 1 | `LadderA`  | 7 | 20 | **yes** |
| 2 | `LadderBB` | 8 | 22 | no |
| 3 | `LadderC`  | 7 | 20 | **yes** |
| 4 | `LadderDD` | 8 | 22 | no |
| 5 | `LadderE`  | 7 | 20 | **yes** |
| 6 | `LadderFF` | 8 | 22 | no |

The cause is in `scripts/patch_rom.py:make_section`, which pads its output to 4
bytes but declares `size = len(data) + 4`, excluding the pad. An odd/even name
therefore flips whether the declared size is aligned. `SmbiosBmcPushDxe` is 16
characters — even — which is why it never ran once across three cycles while
`OobProbeDxe` (11) and `BiosCfgOobDxe` (13) always did.

Fixed in `tools/outerfv.py:ui_section_body`, which pads the name with an extra
NUL so the section is aligned for any name. **`romsurgeon.py` still uses the raw
`P.make_section` for UI sections and has the same latent bug.**

Wrong theories this killed, all of which fit the data at the time: the failing
slot's address, the module's blocking KCS ping at entry, PE `.reloc` differences,
GUID-table exhaustion, and a dispatcher that walks every other file.

## WORKING: BIOS config over Redfish, no AMI OOB code (2026-07-30)

With the alignment fix plus protocol-notify retries, `BiosCfgOobDxe` completes
its full exchange:

```
flags 0x00191943  ENTRY KCS_OK DATA_FOUND XFER_OK PROTO_READY
                  TIMER_ARMED CAP_SENT SCHEMA_SENT SNAPSHOT_SENT
writes 7   ticks 0   datalen 1936
```

335 requests on `netfn=0x30` (1× 0xD3 capabilities, 331× 0xD5 payload chunks,
3× 0xD6), and `GET /redfish/v1/Systems/system/Bios` returns **HTTP 200 with 373
attributes** — real setup options ("Above 4G Decoding", "AMD CPU fTPM", "BME DMA
Mitigation"). Note `ticks 0`: the timer contributed nothing, the protocol
notifies did all of it.

`SmbiosBmcPushDxe` now dispatches and its notifies fire, but reports
`DATA_MISSING` — AMI has not built the SMBIOS table at any point we can hook,
because it finalises it at ReadyToBoot, which never happens on this board. **The
SMBIOS half is blocked on the 0x99 stall, not on injection.**

## The 141-request IPMI burst is AMI's, not ours

Worth settling because it looks like a promising signal and is not one. A boot
that runs AMI's OOB stack produces ~141 requests on ch=3, dominated by
`netfn=0x0A cmd=0x23` ×35, `netfn=0x32 cmd=0x72` ×21, `netfn=0x06 cmd=0x42` ×11
and `netfn=0x3A cmd=0xBD/0xBE/0xB5` ×10 each. Our drivers can only ever emit
three things: `netfn=0x06 cmd=0x01` (entry ping), `netfn=0x2E cmd=0x80`
(phosphor-ipmi-blobs) and `netfn=0x30 cmd=0xD3-0xD8` (BIOS config). Of the 26
distinct netfn/cmd pairs in that burst, exactly one is in our vocabulary, and it
is the one AMI also issues.

An older note here claimed "AMI alone emits exactly two requests". That was an
undercount from a boot where AMI's stack did not run, and it is what made the
count look bistable (2 vs 141). Treat the whole burst as AMI's.

## Timer events and late boot events do not fire

Measured, from the telemetry record: `BiosCfgOobDxe` armed a 2-second periodic
timer and reported `ticks 0` after five minutes of wall clock, with `EndOfDxe`
and `ReadyToBoot` likewise never firing. So an injected driver effectively gets
**one shot: its entry point**. Retry-on-timer designs — which both OOB drivers
were built around — never get a second chance. A `TIMER_ARMED` telemetry flag
was added to tell "never armed" apart from "armed but never fired".

## Why the OOB drivers stayed silent (answered)

Not because they were never dispatched — see the proof above — but because they
hang their work off `ReadyToBoot`, and **`ReadyToBoot` never fires on this board**.
`EndOfDxe` does not fire either. Rework any module that must run here to act at
its entry point or on an early protocol notify.

Historical note, now explained rather than mysterious:

No `netfn=0x2E` (phosphor-ipmi-blobs) or `netfn=0x30` (BIOS config) has ever been
logged, and `/var/lib/smbios/smbios2` has never been refreshed. Given the IPMI
intermittency above, absence of that traffic is weak evidence — a driver liveness
signal that does not depend on IPMI or POST codes is needed.

The 0x4F group covers: full strip (27 modules) + 3 added FFS + apriori; replace-only
(one PE32); strip-only (no custom binaries at all); and four no-op repacks that
changed nothing but the compression. Python liblzma and EDK2's own reference
`LzmaCompress` both fail, with and without padding back to the exact original length.

0x4F is the PI code for **DXE IPL started** — precisely where that volume is
decompressed. Baseline 0x99 is a *pre-existing* stall unrelated to any of this.

## What is and is not verified — measured, not assumed

The board does **not** validate the BIOS byte-identically. Proven three ways:

- The live flash differs from pristine vendor 2.59C in 22 blocks (NVAR stores,
  APOB NV, APCB) and boots fine.
- 16 arbitrary bytes written into the DXE region's erased tail boot fine.
- A well-formed FFS added to that same free space boots fine.

And the DXE volume is not PSP-verified: parsing every AMD BIOS Directory in the
image (`$BHD` at 0x1F8000/0x318000/0x12D9000/0x13E9000/0x14E9000, `$BL2` at
0x500000/0x66F000/0x1791000/0x1921000/0x1AA1000) shows entries only for APCB,
APOB, APOB_NV, PMU, UCODE, MP2, MSMU and **type 0x62 BIOS_RESET_IMAGE = the PEI FV
at bank-relative 0xEA9000**. Nothing references the DXE FV at 0xAC1000. The PSP
loads and verifies PEI only; DXE is outside its coverage.

## The mechanism behind 0x4F is still unidentified

Everything that would explain it has been ruled out:

- **No stored digest.** Searched the whole 32 MiB for sha256/sha1/md5/sha384/
  sha512/crc32 of: the LZMA blob, the whole `9E21FD93` FFS, its body, the
  decompressed payload, the inner FV, and the outer FV's used region. Zero hits.
- **No hidden trailing metadata.** The LZMA stream consumes exactly all 2,549,703
  bytes of the GUIDED section — `unused_data` is empty, so nothing is being
  overwritten past the stream.
- **Section is ordinary.** `EE4E5898` (plain LZMA, not the `D42AE6BD` BCJ/F86
  variant), DataOffset 0x18, Attributes 0x0001 (PROCESSING_REQUIRED only, no
  AUTH_STATUS_VALID), no extra header bytes.
- **Structure is preserved.** EDK2 `VolInfo` reports the same 7 files with
  identical types and sizes before and after; the outer FFS header rebuilds
  byte-identically; the inner FV round-trips byte-identically.
- **Not the encoder, header or length.** EDK2's reference encoder even picks the
  same 16 MiB dictionary and yields a byte-identical 13-byte header. Padding back
  to the exact original length so no size field anywhere changes still fails.

Best remaining guess: something in PEI validates the DXE volume before handing off
(the PEI FV contains `IsSecRecoveryPEI`, `RecoveryControl`, `Recovery`,
`AmiPspRecovery`, `AmiPspSetPcdForRecovery`). Unproven.

## The viable injection vector

The outer `FVMAIN_COMPACT` at `0x1AC1000` is **uncompressed** and has **583 KiB
free** after its last file (`FV+0x3543F8`, abs `0x1E153F8`). Writing there changes
no existing byte and needs no recompression — and it boots.

Open question: whether the DXE dispatcher actually *scans* that outer volume. The
test boot logged no IPMI traffic, but that is not evidence either way, because the
board's baseline stall at 0x99 likely means EndOfDxe/ReadyToBoot never fire, and
`KcsAcquire` returns `EFI_NOT_READY` without emitting any bytes. Needs a driver
whose liveness signal does not depend on either — but note POST codes are useless
here (see below), so it needs something like an early unconditional KCS write.

## Corrections to earlier claims

- I wrote that the outer-FV FFS was "accepted, not dispatched". **Wrong**, and
  wrong in the direction that cost the most time — it wrote off the one injection
  vector that actually works. The verdict rested on seeing no IPMI traffic, a
  metric later shown to be irreproducible. Hardware now says the module runs.
- I wrote that "the strip is exonerated". **Wrong.** The replace-only probe showed
  the replace is *sufficient* to hang; it never showed the strip is harmless. In
  fact every image in that bisection carried the LZMA defect below, so the whole
  bisection was invalid and told us nothing about strip vs replace.
- I wrote that the failure was the LZMA uncompressed-size field. **That was a real
  bug but not the cause.** Python's `lzma.compress(FORMAT_ALONE)` writes
  `0xFFFFFFFFFFFFFFFF` there and EDK2's `LzmaUefiDecompressGetInfo()` reads it to
  size its output buffer — genuinely broken, now fixed in `romsurgeon.py`,
  `uefifv.py` and `patch_rom.py` (which also hardcoded a 1 MiB dictionary against
  the real 16 MiB). Fixing it did not make the board boot.

## POST codes cannot identify our drivers

AMI uses the entire byte range. A stock boot contains 0xE0, 0xE1, all of 0x70–0x79
and all of 0x60–0x6A, including a long descending sweep `90 89 88 … 01`. The
comment in `SmbiosBmcPushDxe.c` claiming "0x6x/0x7x not used by AMI on this board"
is false. Use `journalctl | grep IPMI-REQ` instead — AMI alone emits exactly two on
`ch=3` (`netfn=0x06 cmd=0x04`, then `cmd=0x01`).
