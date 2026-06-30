# asrock-ipmi-oem — ASRock/AMI OEM IPMI provider (X570D4I-2T)

A `phosphor-host-ipmid` provider library (`libzasrockoemcmds.so`) that reproduces
the stock AMI MegaRAC OEM IPMI command set the host BIOS uses during POST, plus the
**Redfish Host Interface (RHI) credential-bootstrap commands** that gate usb0 RHI.

## Command groups implemented

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
