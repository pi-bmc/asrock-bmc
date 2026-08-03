# Why ReadyToBoot never fires (2026-08-02)

Investigation notes. The OOB side is covered in
[host-bios-driver-injection-results.md](host-bios-driver-injection-results.md);
this file is only about the boot stall.

## The terminal POST code is 0xB6, not 0x99

Earlier notes (including mine) recorded the board as "stalling at 0x99". **That
was a sampling error** — 0x99 is a transient the boot passes through. Sampled
every ~40 s with the host up, the code sits at **0xB6** and the stored code count
stops growing, so this is a genuine stop, not a slow loop.

## Where BDS gets to

Decoded against the AMI Aptio V checkpoint table, the tail of a boot is:

```
90  BDS started
91  Connect drivers
92  PCI bus init
94  PCI enumeration        x17
95  PCI request resources
96  PCI assign resources
BF  40  43  1B             (OEM/AGESA, not AMI checkpoints)
9B  USB reset
40  42  77
B6  <- stops here
```

So PCI enumeration and resource assignment complete normally, a USB reset
happens, and then it stops.

**`0x97` "Console Output connect" never appears — not once in the whole boot.**
That is the checkpoint immediately after `0x96` in the normal AMI order, and its
absence lines up exactly with the long-standing blank-KVM symptom: the consoles
are never connected, so nothing is ever drawn.

`0xAD` ReadyToBoot never appears in the BDS region either. That is corroborated
independently and much more reliably by `OobProbeDxe`, whose ReadyToBoot and
EndOfDxe group callbacks never run.

**Caveat on 0xB6.** In the AMI table 0xB6 is "Clean-up of NVRAM", which is a
tempting story. Do not lean on it: this BIOS emits a lot of OEM codes in the same
range (0xBF, 0x40, 0x43, 0x1B, 0x77 right before it are not AMI checkpoints), so
the label is not trustworthy. The trustworthy parts are the unambiguous
0x90-0x96 BDS sequence and the *absence* of 0x97.

The NVRAM-full version of that story is disproved anyway: the live NVAR store has
68 entries, all valid, none deleted, **13.6% used with 113 KB free** and an
erased tail.

## The host also loses PWROK about 12 minutes in

```
22:26:21  Host system DC power is on
22:38:51  powerStateOn: power OK de-assert event received
22:38:51  Beep with priority: 8          <- beepPowerFail
```

Not a BMC-initiated transition — the PSU dropped. Seen once so far in this BMC
boot, so treat it as an observation rather than a rule, but it bounds every test
window to roughly 12 minutes and it is worth watching for.

Separately: after a **BMC** reboot the host stays down because the power restore
policy is `AlwaysOff`. A host found powered off is not automatically evidence of
a host-side fault.

## Why we are still blind, and what blocks fixing it

Serial console redirection would let the BIOS say where it is instead of us
inferring it from OEM POST codes. It is switched off in NVRAM:

| Key | Meaning | Value |
|---|---|---|
| `TER001`/`TER002` | Console Redirection | `0x00` disabled |
| `GSIO201` | Serial Port (COM) | `0x00` disabled |
| `PTT010` | Redirection Support | `0x00` disabled |
| `TER0A8` | Redirection COM Port | `0x00` |

`/var/log/obmc-console.log` is 10 bytes and stale, consistent with all of that.

We can now *read* all 373 knobs over Redfish, but **not write them**: every
attribute comes back `ReadOnly=true`. That is not arbitrary. In
`asrock-ipmi-oem/src/biosvarstore.cpp` a knob is only marked writable when its
`varstoreIndex` is found in the pushed snapshot:

```cpp
bool readOnly = true;
const Record* rec = snap.find(k.varstoreIndex);
if (rec != nullptr && readField(*rec, k.offset, k.size, value)) {
    current  = formatValue(k, value);
    readOnly = k.readOnly;      // false for every setupType in our XML
}
```

All 373 landing on `readOnly = true` means **that lookup failed for every single
knob**. Two consequences follow from the same fact:

1. Writes are impossible, so we cannot turn console redirection on this way.
2. The values on display are the schema's own `currentValStr` from
   `x570d4i2t-bios-knobs.xml`, **not** the live values from the snapshot our
   driver pushed.

The driver's side looks healthy — it reports `SNAPSHOT_SENT` with
`datalen 1936` — so the defect is in the match between that snapshot's records
and the schema's `varstoreIndex`/`offset`/`size`, on one side or the other.

## TESTED AND REVERTED: enabling SOL console redirection stops POST entirely

Do not repeat this without a reason to expect a different outcome.

Staged through the supported path — `PendingAttributes` on D-Bus, then
`asrock-bios-flash-sync.sh apply`, which folds them into the flash while the host
is off and clears pending:

```
PTT010  Redirection Support   0x00 Disabled -> 0x01 Enabled
TER0A8  Redirection COM Port  0x00 COM1     -> 0x01 SOL
```

Both verified written (`flash updated; clearing PendingAttributes`, and Redfish
read back `0x01`/`0x01`). **The host then did not POST at all**: DC power on, but
after 3.5 minutes zero port-80 codes, zero IPMI, zero console bytes, with
`lpcsnoop` and the postcode manager both healthy — so not a monitoring artifact.

Reflashing the previous image (`oobstack3.rom`, sha256 `33bc4318…`) over the mux
restored POST within seconds — 608 codes on the next boot and the OOB driver
running again. That makes it causal, not coincidental.

This is the same shape as the older warning about not re-applying
`bios-now.bin`'s CSM/console bytes: **something about the console/redirection
configuration on this board is load-bearing for POST.** Note `TER001`/`TER002`
("Console Redirection" proper) were never even set — they are Enumeration
attributes with *no bound options*, so `PendingAttributes` rejects any value for
them with `Invalid argument`. Only the two supporting knobs were changed, and
that alone was enough to prevent POST.

Consequence: the serial-console route to observing BDS is **closed** for now.
Reopening it means understanding why redirection is fatal here, not just setting
more knobs.

## MEASURED: DXE never finishes, so BDS never runs

`OobProbeDxe` now takes a census of the console/graphics stack and watches for
the first install of each protocol BDS would need. Result, from one boot:

| Watch | Fired? |
|---|---|
| entry census | yes — GOP 0, TextOut 0, UGA 0, PciIo 0, `gST->ConOut` NULL |
| `PciEnumerationComplete` install | **never** |
| `DxeSmmReadyToLock` install | **never** |
| first `EFI_PCI_IO_PROTOCOL` install | **never** |
| first `EFI_GRAPHICS_OUTPUT_PROTOCOL` install | **never** |
| first `EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL` install | **never** |

After our driver's entry point, **nothing is ever installed into the UEFI handle
database again**. `DxeSmmReadyToLock` is the decisive one: it is signalled at the
*end of DXE*, before BDS. If it never appears, DXE never completes, so BDS never
starts.

That reframes the earlier "missing 0x97" reading. Absent 0x97 is a *symptom of
never reaching BDS at all*, not evidence of a console-connect failure
specifically — and the `90 91 92 94… 95 96` run in the POST tail is very
probably OEM codes rather than AMI BDS checkpoints, exactly like the `0xBF`,
`0x40`, `0x43`, `0x1B`, `0x77` around it. Everything else lines up: EndOfDxe and
ReadyToBoot never fire, and ConOut/ConIn/ErrOut/BootOrder have never been created
in flash because nothing ever got far enough to create them.

**Methodology warning that produced a false result first.**
`EfiCreateProtocolNotifyEvent` signals its callback **once immediately at
registration**, whether or not the protocol exists. The first version of this
census therefore ran all its "late" passes back-to-back inside the entry point
and reported identical zeros at passes 1/2/3 — which looked like a finding and
was an artifact. Every notify callback must `LocateProtocol` first and bail out
until it genuinely succeeds. The same unguarded pattern is still in
`SmbiosBmcPushDxe` and `BiosCfgOobDxe`, which means **their `PROTO_READY` flags
are not trustworthy** — BiosCfgOobDxe's success almost certainly came from the
direct `BiosCfgOobRun()` call at entry, not from a notify.

**Caveat not yet closed:** we have not measured a *stock* image's terminal POST
code with this rigour. The stall is believed to predate our drivers, but "our
injected driver is not contributing" is assumed, not demonstrated. Worth one
cycle with a clean stock image before building on this.

## Next step

Fix the snapshot/varstore match. It is the single change that unlocks both live
values and writability, and writability is what lets us enable Console
Redirection and finally read the BIOS's own account of where BDS stops — instead
of guessing from OEM POST codes.

Compare what `BuildSnapshot` in `BiosCfgOobDxe.c` emits (varstore ids and record
layout) against what `biosvarstore.cpp:snap.find(k.varstoreIndex)` expects, and
against the `varstoreIndex` values in the XML. Worth checking first whether the
XML's varstore indices survived schema generation at all — `varstoreIndex < 0`
means "the XML did not carry one", which alone would make every lookup fail.
