# Getting early BIOS output over SOL (2026-08-02)

Analysis of the UEFI variables and the BMC-side UART, to work out how to see
what the BIOS is doing without the KVM. Companion to
[readytoboot-stall-investigation.md](readytoboot-stall-investigation.md).

## What the BMC actually presents to the host

| Fact | Value | Source |
|---|---|---|
| VUART enabled | yes, `GCRA`(0x1e787020) = 0x21, bit0 set | devmem |
| VUART LPC address | **0x3F8** (`ADDRL`/`ADDRH` = F8 / 03) | devmem + sysfs |
| VUART SIRQ | **4** (`GCRB` = 0x4F, bits[7:4]) | devmem + sysfs |
| Console server | `obmc-console@ttyVUART0`, active since 2026-07-30 | systemd |
| Bytes ever received | **0** (`/var/log/obmc-console.log` is 0 bytes) | ls |

So there is a live, enabled 16550 at LPC **0x3F8 / IRQ 4** that the BMC is
listening on, and the host has never written a byte to it.

The LPC IO path itself is known good: our injected DXE driver talks IPMI over
KCS at 0xCA2 during DXE. Host→BMC LPC IO cycles work at exactly the phase we
care about.

## What the BIOS's variables say

Serial devices. **The "Value" column here is NOT live** — varstores 3 and 4 never
resolve, so Redfish falls back to the XML's `CurrentVal`, and all 431 knobs in
that XML have `CurrentVal == default`. These readings carry no information about
the running system:

| Knob | Meaning | varstore | Schema echo (not live) |
|---|---|---|---|
| `GSIO201` | Serial Port (enable) | 3 | `0x00` |
| `SUPERIO001` | Serial Port Address | 3 | `0x02` = **3F8h/IRQ4** |
| `SUPERIO002` | SOL Port (enable) | 4 | `0x00` |
| `SUPERIO004` | SOL Port Address | 4 | `0x03` = **2F8h/IRQ3** |

The trustworthy source for these is the ROM's own `StdDefaults` NVRAM default
store, which has **both devices enabled**: `PNP0501_0_NV` = `01 02 00` (on,
3F8h/IRQ4) and `PNP0501_1_NV` = `01 03 00` (on, 2F8h/IRQ3). The addresses
corroborate the varstore mapping below exactly.

**Only the BMC side decides whether a port is reachable.** The board DTS enables
`&uart5` (the BMC's own console) and `&vuart` — nothing else. There is no
`&uart1`/`&uart2` and no LPC UART pinctrl, so **no tty backs 0x2F8** whatever the
BIOS does with it. The one reachable console is the VUART at 0x3F8 = COM1 =
device 0.

Redirection (all in varstore 1 = `Setup`):

| Knob | Offset | Meaning | Value |
|---|---|---|---|
| `PTT010` | 0x0007 | Redirection Support | `0x00` disabled |
| `TER0A8` | 0x0180 | Redirection COM Port | `0x00` = **COM1** |
| `TER001` | 0x016A | Console Redirection, port 0 | `0x00` |
| `TER002` | 0x016B | Console Redirection, port 1 | `0x00` |
| `TER0021` | 0x0160 | Bits per second, port 0 | `0x07` = **115200** |
| `TER012` | 0x016C | Terminal Type, port 0 | `0x01` = VT100+ |
| `TER0022` | 0x0161 | Bits per second, port 1 | `0x03` = 9600 (default) |
| `TER0A7` | 0x0177 | Redirect After POST | `0x00` Always Enable |

Note `TER0021` and `TER012` are **non-default** (XML defaults are 9600 / VT100)
while the port-1 equivalents are stock. Port 0 has been deliberately configured
for 115200 — which is also obmc-console's default. Port 0 is COM1 is 0x3F8 is
the VUART. Everything points at the same port.

## Why the earlier test killed POST

We set `PTT010` = Enabled **and** `TER0A8` COM1 → **SOL**. `TER0A8` was already
`0x00` = COM1, i.e. already pointing at the only port the BMC can read. Changing
it was the error.

Note what these two knobs actually are. `TER0A8`'s own description is *"Select a
COM port to display redirection of **Legacy OS and Legacy OPROM** Messages"* —
this is the **Legacy Console Redirection** submenu, not the UEFI one. `PTT010`
sits with it. The UEFI per-port switches are `TER001`/`TER002`, which were never
touched. So the fatal change was to the *legacy/CSM* redirection path, and
CSM/console bytes are independently known to be load-bearing for POST here (see
the "do not re-apply bios-now.bin's CSM bytes" warning).

**The mechanism is not established.** An earlier draft of this file argued the
2F8 port simply did not exist; `StdDefaults` then showed `PNP0501_1_NV` =
`01 03 00`, i.e. the BIOS enables it, so that story is at best incomplete. What
is certain is that no BMC tty backs 0x2F8, so redirecting there was pointless
regardless.

**This does not establish that console redirection is fatal on this board** —
and the 2026-08-03 test below confirms `TER001` is safe.

## Why we cannot just fix it over Redfish

`GSIO201`, `SUPERIO002` and `SUPERIO004` all come back `ReadOnly=true`. That is
not arbitrary — `biosvarstore.cpp` marks a knob writable only when
`snap.find(k.varstoreIndex)` succeeds. Cross-referencing the published
BaseBIOSTable against the XML's `varstoreIndex`:

```
 varstore  writable  readOnly   verdict
        1       261         2   RESOLVED -> Setup (511 B)
        6        44         0   RESOLVED -> AMITSESetup (65 B)
       14        30        10   RESOLVED -> ServerSetup (739 B)
    0,2,3,4,5,7..13   0    38   MISSING -> everything readOnly
```

Only three varstores resolve. `asrock-bios-nvram` pairs a varstore to an NVAR
variable by scoring how many knob bytes hold a declared option value; varstores
3 and 4 span **2 bytes each**, which is far too little evidence to pick a
candidate, so they are left UNRESOLVED and every knob in them is readOnly.

But scoring is not the real obstacle. Scanning the extracted ROM for
`EFI_IFR_VARSTORE` opcodes recovers the real varstore table, and the serial ones
are:

```
id=0x0015 size=9  PNP0501_0_VV   560bf58a-1e0d-4d7e-953f-2980a261e031
id=0x0016 size=3  PNP0501_0_NV   560bf58a-1e0d-4d7e-953f-2980a261e031
id=0x0017 size=9  PNP0501_1_VV   560bf58a-1e0d-4d7e-953f-2980a261e031
id=0x0018 size=3  PNP0501_1_NV   560bf58a-1e0d-4d7e-953f-2980a261e031
id=0x001E size=7  TerminalSerialVar  (same GUID; this one DOES exist in NVRAM)
```

`PNP0501` is the ACPI PnP ID for a 16550. `_NV` = the non-volatile per-device
varstore, 3 bytes, laid out `[0] = enable, [1] = address/mode index` — exactly
the shape of XML varstores 3 and 4. So:

- varstore 3 → `PNP0501_0_NV` (COM1)
- varstore 4 → `PNP0501_1_NV` (SOL)

and **neither variable exists in the NVAR store.** A dump of all 41 named NVAR
variables contains no `PNP0501_*`. AMI creates these lazily, on the first change
made in Setup — and we can never reach Setup, because there is no console. That
is the chicken-and-egg at the centre of this.

## HARDWARE RESULT 2026-08-03: stock + `TER001` = 1, flashed and booted

Built `build/sol-stock.rom` from `backup-live-20260729-1813.bin` (verified to be
genuine stock — `census2.rom` differs from it in exactly one region, our injected
FFS at 0x1E153F8). One byte changed, at file offset 0x1038320:
`Setup + 0x016A  TER001  0x00 -> 0x01`. Script: `scripts/preset_sol_console.py`,
which asserts nine preconditions before writing and refuses on any mismatch.
Flashed to /dev/mtd6 over the mux, `flashcp -v` verified 100%.

**Result: POST is unaffected, and the console stays completely silent.**

| | |
|---|---|
| POST codes this boot | 396, terminal **0x99**, stable |
| `0x97` console-connect | **never** (0 occurrences) |
| obmc-console bytes | **0** |

Three things follow.

**1. `TER001` alone is safe.** It did not reproduce the no-POST. That supports
the reading that the 2026-08-02 failure came from the *legacy* pair
(`PTT010` + `TER0A8`), which this image deliberately left untouched.

**2. The stall predates our drivers — caveat now closed by measurement.** A
stock image with no injected FFS stalls too, and never reaches `0x97`.

**3. I was wrong to call the old "stalls at 0x99" note a sampling error.** Both
readings are real, for different images:

| Image | Terminal code | Tail |
|---|---|---|
| stock | **0x99** | `… 9B 40 42 5A 5E 5A 06 07 99` |
| stock + our injected DXE | **0xB6** | `… 9B 40 42 77 B6` |

`0xB6` occurs mid-boot (index 117 of 396) in the stock run, so it is not a
special terminal value. Our drivers do perturb the tail; the earlier correction
overreached. What holds either way is that `0x97` never appears in either image.

**Why the console is silent is not a mystery.** AMI's Terminal driver installs
`SimpleTextOut` on the serial device during **BDS console connect**. BDS never
runs. So enabling `TER001` cannot produce output until the DXE stall is fixed —
this is the limit already stated at the bottom of this file, now confirmed on
hardware rather than argued. It also means SOL cannot be used to *diagnose* the
stall through AMI's own machinery, which leaves route A as the only way to get
bytes out before BDS.

## Routes

### A. Our own UART logger in DXE — recommended, and independent of all of the above

The VUART decodes 0x3F8 *right now*, regardless of what the BIOS thinks about
its SuperIO. `OobProbeDxe` already dispatches reliably during DXE and already
links `IoLib`. Writing a 16550 at 0x3F8 from it needs about 20 lines and gives
early BIOS output on `obmc-console` with **zero UEFI variable changes and zero
risk of another no-POST**:

- No BIOS knob is touched, so the fatal failure mode cannot recur.
- Writing to a UART cannot hang if the THRE poll is bounded by a counter rather
  than spinning. If the VUART somehow did not answer, reads return 0xFF, THRE
  reads as set, and the writes are simply discarded.
- It is the instrument we actually need: the stall is **inside DXE**, and this
  turns our driver from "leaves four bytes in an NVAR record per boot" into a
  live printf channel.

It also settles the address question empirically — write a marker to 0x3F8 and
to 0x2F8 and see which one arrives.

### B. Enable the port properly, then AMI redirection

Needs `PNP0501_0_NV` to exist. Two ways to create it:

1. `gRT->SetVariable(L"PNP0501_0_NV", {560bf58a-…}, NV|BS|RT, 3, {0x01, 0x02, 0x00})`
   from an injected DXE driver. Takes effect the *following* boot, since the SIO
   driver reads its config long before our appended driver runs.
2. Append the NVAR entry offline with the mux pulled, reusing the GUID index
   that `TerminalSerialVar` already occupies (same GUID). `scripts/patch_setup_nvar.py`
   is the existing precedent for editing this store; the store has ~113 KB free.

Then, in `Setup`: `PTT010`(0x0007) = 0x01, `TER001`(0x016A) = 0x01, and leave
`TER0A8`(0x0180) = 0x00 = COM1. `TER0021`/`TER012` are already 115200/VT100+.

Residual risk worth respecting: enabling the SIO serial device may make the
AST2500 SuperIO decode 0x3F8 in addition to the VUART. On ASRock Rack AST2500
boards these are usually the same decoder, but that is an assumption. Route A
does not depend on resolving it.

### D. `IPMI003 "Log EFI Status Codes"` — the one variable that emits before BDS

There is no UEFI variable that selects "the vuart" — the BIOS cannot see a
VUART, only a 16550 at an LPC address, and device 0's address knob is already
`0x02` = 3F8h/IRQ4, exactly where the VUART decodes. The standard UEFI console
variables (`ConOut`/`ConIn`/`ErrOut`, device paths carrying an `ACPI(PNP0501)` +
`UART(115200,8,N,1)` node) are absent from this flash *and* are consumed by BDS,
so creating them changes nothing. Every serial-console path is BDS-gated.

`IPMI003` is not. Live value, read from the image now on the board:

| Knob | Where | Live | Options |
|---|---|---|---|
| `IPMI003` Log EFI Status Codes | ServerSetup + 0x020D | **0x02 Error code** | 0x00 Disabled, 0x01 Both, 0x02 Error, **0x03 Progress** |
| `IPMI000` SEL Components | ServerSetup + 0x020A | 0x01 Enabled | |
| `IPMI600` BMC Support | ServerSetup + 0x0000 | 0x01 Enabled | |
| `IPMI601` Wait For BMC | ServerSetup + 0x0001 | 0x01 Enabled | |

EFI status codes go through the ReportStatusCode router from early PEI onward,
and this ROM contains the AMI drivers that forward them to the BMC SEL over KCS:

```
E9DD7F62-25EC-4F9D-A4AB-AAD20BF59A10  StatusCodePei
2700F72F-E0EA-4767-9A1E-D172F0704778  PeiSelStatusCode
61422D26-81EC-47FF-B6CF-939EAEE73FBA  StatusCodeDxe
AE587172-CC15-48E1-8BE0-29DDF05C6A1F  DxeSelStatusCode
```

No BDS, no console connect, no serial port. KCS is already proven working at the
exact phase that stalls. ServerSetup is varstore 14, which **resolves**, so this
is writable both through the flash-sync path and as a one-byte ROM edit.

Caveats: every SEL entry is a KCS transaction, so `0x01 Both` could slow POST
badly — start with `0x03 Progress code`. And the BMC SEL is currently empty,
consistent with "error codes only, none raised", but it does mean the channel has
never been observed working end to end.

Also noted: `IPMI400 "Serial Mux"` (ServerSetup + 0x0219) is live **0x00
Disabled** against a default of Enabled — a deliberate deviation. It muxes a
physical shared UART between host and BMC, and the DTS shows no such UART here,
so it is probably inert, but it is the only other serial-routing knob in the tree.

### C. Fix varstore resolution generally

The IFR scan above yields name + GUID + size for every varstore. Feeding that
table into `asrock-bios-nvram` replaces the heuristic scorer with an exact map,
which would make the remaining 38 knobs writable and their values live. Worth
doing regardless — it is the same fix the `/Bios` read-only problem needs.

## The honest limit: SOL will not show the UEFI menu yet

Setup runs in **BDS**. We have measured that DXE never completes — no
`DxeSmmReadyToLock`, no `PciEnumerationComplete`, no `EFI_PCI_IO_PROTOCOL`, no
GOP, no `SimpleTextOut` after our driver's entry. BDS never starts, so there is
no Setup to redirect and no console to connect, on serial or anywhere else.

Serial does not route around the stall. What it does is let us *see* the stall
from inside DXE, which NVAR records can only hint at. Route A first; the menu
becomes a question worth asking again only once DXE completes.
