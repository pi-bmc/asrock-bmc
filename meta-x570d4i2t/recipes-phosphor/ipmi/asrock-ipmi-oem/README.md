# asrock-ipmi-oem — ASRock/AMI OEM IPMI provider (X570D4I-2T)

A `phosphor-host-ipmid` provider library (`libzasrockoemcmds.so`). The library
as built today ships two things (see `meson.build`):

* **`kcsmonitor.cpp`** — a pass-through request filter that logs every inbound
  IPMI message (see below).
* **`biosconfigcommands.cpp`** — the OpenBMC **BIOS OOB config** command set,
  adapted from `intel-ipmi-oem` for AMI firmware (see next section).

> The historical command tables (`oemcommands.cpp`, `amicommands.cpp`,
> `redfishhostiface.cpp`, `smbiosbuilder.cpp`, …) documented further below were
> **removed** from the meson build; the sections are kept as a reverse-engineering
> reference. SMBIOS now flows over the standard smbios-ipmi-blob receiver.

## BIOS OOB config (`biosconfigcommands.cpp`) — the host-push path

Adapted from [`intel-ipmi-oem/src/biosconfigcommands.cpp`](https://github.com/openbmc/intel-ipmi-oem/blob/master/src/biosconfigcommands.cpp).
Lets the AMI host BIOS push its attribute registry to the BMC in-band over KCS so
the stock bmcweb `/redfish/v1/Systems/system/Bios[/Settings]` endpoints populate.
This replaces the removed USB Redfish Host Interface config push.

| NetFn | Cmd | Name | Priv | Purpose |
|---|---|---|---|---|
| 0x30 | 0xD3 | SetBIOSCapabilities | Admin | store OOB capability byte |
| 0x30 | 0xD4 | GetBIOSCapabilities | User | read it back |
| 0x30 | 0xD5 | SetPayload | Admin | chunked host→BMC transfer (Start/InProgress/End/Abort), CRC32 |
| 0x30 | 0xD6 | GetPayload | User | Info / Data / Status readback (incl. PendingAttributes) |
| 0x30 | 0xD7 | SetBIOSPwdHashInfo | Admin | store admin password hash + seed → `bios-settings-manager/seedData` |
| 0x30 | 0xD8 | GetBIOSPwdHash | User | return seed + stored hash |

**Key adaptation vs. Intel:** Intel ships proprietary BIOS Setup *XML* (parsed by
`biosxml.hpp` + a spawned converter). That is removed. The `SetPayload` transport
here carries a **JSON** document (PayloadType 0) deserialised straight into the
`xyz.openbmc_project.BIOSConfig.Manager` `BaseBIOSTable` property — no vendor
translation, no `oemcommands.hpp`, no `/var/oob`. The commands stay on Intel's
**NetFn 0x30** path (cmds 0xD3–0xD8): a live KCS capture confirms the stock AMI
BIOS drives exactly this protocol — it sends `SetBIOSCap` (0xD3) on NetFn 0x30
during POST — so the handlers must match Intel's numbers. The chunked
transfer/CRC protocol is unchanged. All tunables (NetFn/cmd, D-Bus names, paths)
are at the top of `biosconfigcommands.hpp`.

### Payload format — Aptio XML (confirmed by capture)

A live `SetPayload` StartTransfer from the BIOS decodes as:

```
cmd=0xD5 data=[00 01 | 01 00 | 97 35 00 00 | CC CF BC 66 | 00]
             state=Start type=1  ver=1  size=0x3597(13719) crc32=0x66BCCFCC flag=0
```

So the body is **payloadType 1, ~13.7 KB** — Intel-Aptio **BIOS-config XML**, not
JSON (Intel server BIOS is AMI Aptio; this board's AMI BIOS emits the same XML).
The StartTransfer struct is Intel's exact packed layout
`{u16 version, u32 totalSize, u32 totalChecksum, u8 flag}`.

The handler runs the full chunked transfer + CRC32 and persists the assembled
payload to `/var/lib/asrock-bios-config/PayloadN`. **Turning that XML into the
`BaseBIOSTable` D-Bus property is the remaining work** and needs the concrete
Aptio schema:

1. Boot the host, then `cat /var/lib/asrock-bios-config/Payload1` to grab the XML.
2. Add an `XML → BiosBaseTable` parser in `biosconfigcommands.cpp` — it can reuse
   the D-Bus property-`Set` block in the (currently `[[maybe_unused]]`)
   `setBaseBIOSTable()` helper, which already builds the exact
   `a{s(sbsssvva(svs))}` structure the manager expects.

Transfers are ACKed as successful in the meantime so the BIOS handshake completes
and the bytes always land on disk. The `setBaseBIOSTable()` JSON helper documents
the target D-Bus shape: `attributeType` ∈ {Enumeration, String, Password, Integer,
Boolean}; `boundType` ∈ {OneOf, LowerBound, UpperBound, ScalarIncrement,
MinStringLength, MaxStringLength}.

## Historical / reference command groups (removed from the build)

| File | NetFn | What |
|---|---|---|
| `oemcommands.cpp` | 0x3A (`netFnOemSix`) | ASRock OEM: inventory, sensor info, FW version, GPIOJ1 KVM/SPI mux, PSU info, BMC config, SEL policy, YAFU stubs |
| `amicommands.cpp` | 0x32 | AMI OEM / AMI-MDR SMBIOS chunks |
| `appcommands.cpp` | 0x06 | App overrides (GetDeviceId from D-Bus, GetSystemGuid) |
| `chassiscommands.cpp` | 0x00 | GetChassisStatus (SIO intrusion + identify LED), ChassisIdentify, restart cause |
| `sensorcommands.cpp` | 0x04 | PlatformEvent → phosphor-logging/Redfish |
| `storagecommands.cpp` | 0x0A | SEL get/add/time (Redfish bridge) |
| `smbiosbuilder.cpp` | — | Synthesize SMBIOS table from FRU/SPD into smbios-mdrv2 |
| `kcsmonitor.cpp` | — | Logs every host IPMI request: `IPMI-REQ ch=.. netfn=.. cmd=.. data=[..]` |
| **`redfishhostiface.cpp`** | **0x2C grp 0x00** | **RHI credential bootstrap (the RHI fix) — see below** |
| `redfish_to_ipmi_hooks.cpp` | — | standalone daemon: inbound Redfish HTTP on usb0 → IPMI |

SMBIOS arrives over IPMI AMI-MDR (NetFn 0x3A `0xB5`/`0xB2` + NetFn 0x32 `0x5D`),
handled here and synthesized from FRU EEPROM + DIMM SPD.

## Redfish Host Interface bootstrap (`redfishhostiface.cpp`) — the RHI gate

**Why it exists:** the host BIOS will not bring up the usb0 Redfish Host Interface
until it completes a DMTF DSP0270 **credential-bootstrap handshake over KCS IPMI**.
Stock OpenBMC didn't implement it → returned `0xC1` → BIOS aborted RHI every boot.

Commands (`NetFn 0x2C` Group Extension):

| Cmd | Name | Response |
|---|---|---|
| 0x01 | GetManagerCertificateFingerprint | `[CC][group][0x01][cert fingerprint]` |
| 0x02 | GetBootstrapAccountCredentials | `[CC][group][username 16B][password 16B]` |

**Empirically (live KCS capture) the BIOS sends `NetFn 0x2C, Cmd 0x01, data=[0x00]`**
— group/defining-body **0x00**, no trailing byte. So the handler is registered under
`ipmi::registerGroupHandler(prio, 0x00, cmd, priv, h)` and its params are
`std::optional<uint8_t>` (the byte is absent). cmd 0x02 generates a random 16-char
user+password, creates a real BMC user (`User.Manager CreateUser` + PAM
`pam_chauthtok("passwd")`), and returns the creds.

**STATUS:** deployed; the BIOS now invokes the handler and gets `cc=0` (past the old
`0xC1` wall) but then **stops** — it is rejecting our cmd-01 *response format*. Stock
MegaRAC returns `[0x52][0x01][50-byte fingerprint]` (hard-codes group `0x52`, 50-byte
non-SHA256 fingerprint); we currently return `[0x00][0x01][32-byte SHA-256]`.

➡ **Full root-cause, reverse-engineering, next steps, and operational runbook are in
[`../../../RHI-HANDOFF.md`](../../../RHI-HANDOFF.md).** Read that before continuing.

## Deps / build

`DEPENDS` adds `openssl` (cert fingerprint) + `libpam` (set bootstrap password).
Build/deploy and in-band test commands are in RHI-HANDOFF.md §4. Hot-patch deploy:
the `.so` is ~20 MB — stage in `/dev/shm` on the BMC (the `/home/root` cow overlay is
only 22 MB), `cp` over `/usr/lib/ipmid-providers/libzasrockoemcmds.so.0.1`, then
`systemctl restart phosphor-ipmi-host`.

## Reference: stock MegaRAC command tables

`IPMIMain` + per-NetFn cores in `libipmimsghndlr.so` (g_coreApp/Chassis/Storage/
Transport/Sensor/Bridge/NetFn30/AMI…), `libipmipdkcmds.so` (g_NetFn30), `libasrrcmds.so`
(ASRock OEM, full symbols), `libipmidcmi.so` (DCMI group 0xDC), **`libipmiredfishhostiface.so`**
(RHI group). Located at `/home/appkins/Downloads/X570D4I-2T(01.91.00)BMC/rootfs_squash/rootfs/usr/local/lib/`.
Disassemble ARM with `arm-none-eabi-objdump -D -j .text` (system objdump has no ARM).
