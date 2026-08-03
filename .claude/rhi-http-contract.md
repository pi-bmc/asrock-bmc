# The HTTP contract the stock BIOS expects (2026-08-03)

Extracted statically from the stock BIOS drivers — no hardware needed. This is
what a BMC-side service must implement to replace our injected DXE drivers.

Source binaries (`edk2-x570d4i2t/Platform/X574I2T/`):
`FirmwareConfigDrv [1c5c6e7e]`, `AmiRedfishDynExt [3b1de95d]`,
`AmiRedFishApi [d4395796]`, `RedfishHi [cef05965]`.

**Direction: the BIOS is the HTTP _client_; the BMC is the _server_.**

## Endpoints

| Method | URI | Purpose |
|---|---|---|
| GET | `/redfish/v1/` | service root probe (`RedfishVersion`) |
| GET | `/redfish/v1/Registries` | what registries the BMC already holds |
| POST | `/redfish/v1/Registries/BiosAttributeRegistry%s.%d.%d.%d.json` | **BIOS uploads its own attribute registry** |
| GET | `/redfish/v1/Systems/Self/Bios` | read back current settings |
| POST | `/redfish/v1/Systems/Self/Bios` | **BIOS publishes current attributes** |
| PATCH | `/redfish/v1/Systems/Self/Bios` | update current attributes |
| **GET** | **`/redfish/v1/Systems/Self/Bios/SD`** | **BIOS reads PENDING settings — this is the write path** |
| DELETE | `/redfish/v1/Systems/Self/Bios/SD` | clear pending once applied |
| GET/POST/DELETE | `/redfish/v1/bios/defaultSD` | default settings object |
| GET/DELETE | `/redfish/v1/Systems/Self/Bios/Actions/Bios.ResetBios` | reset-to-defaults request queue |
| GET/DELETE | `/redfish/v1/Systems/Self/Bios/Actions/Bios.ChangePassword` | password-change request queue |
| POST | `/redfish/v1/BiosStaticFiles` + `/redfish/v1/BiosStaticFiles/PostStatus` | static file upload |
| POST | `/ami/static-file` | AMI static file upload |
| GET | `/redfish/v1/DynamicExtension/RedfishExtensions` (+`/PostStatus`) | AMI dynamic extension |
| GET | `/redfish/v1/Oem/Ami/InventoryData` | AMI inventory |

Note the non-standard verbs: `GET`/`DELETE` on `Actions/*` (Redfish normally uses
POST), and `POST` to `/Bios`. This is AMI's private protocol wearing Redfish
clothes — a stock Redfish implementation will not satisfy it by accident.

## Payloads

- `Content-Type: application/json;charset=utf-8` — the Bios/registry bodies
- `Content-Type: application/xml;charset=utf-16` — **the setup XML**, i.e. the
  same AMISCE-format knob dump we carry statically as
  `x570d4i2t-bios-knobs.xml`, but emitted by the running BIOS
- `Content-Type: application/gzip` and `multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxkTrZu0gW`
  — the static-file uploads
- JSON keys seen verbatim: `"@Redfish.Settings"`, `"@odata.type": "#Settings.v1_2_2.Settings"`,
  `"AttributeRegistry": "BiosAttributeRegistry%a.%d.%d.%d"`, `"Attributes":{`,
  `"Name"/"Description": "BIOS Configuration Current Settings"`,
  `"MessageId": "Base.1.0.SettingsFailed"`
- `@odata.id` template is `/redfish/v1/Systems/1/Bios` while the request URIs use
  `Systems/Self` — both spellings must resolve.

## Why this is worth doing

1. **It is bidirectional.** `GET /Bios/SD` then `DELETE /Bios/SD` is exactly the
   pending-apply cycle our injected `BiosCfgOobDxe` reimplements over IPMI
   netfn 0x30 (type 3). Nothing is lost by dropping the injected driver.
2. **The BIOS hands us the authoritative schema at runtime.** It uploads its own
   attribute registry and a UTF-16 setup XML. That removes every problem we have
   with the static XML: `CurrentVal` being a useless copy of `default`, varstores
   3/4/7 never resolving, 38 knobs stuck ReadOnly.
3. **No FFS injection.** No 4-alignment trap, no apriori pruning, no
   re-encoding risk, and it survives a vendor BIOS update.

## What the BMC must serve — bmcweb does NOT cover this

bmcweb serves `/redfish/v1/Systems/**system**/Bios` with `@Redfish.Settings`
backed by phosphor-bios-config-manager. Missing for AMI:

- the `Systems/Self` (and `Systems/1`) spelling
- `POST` to `/Bios` (bmcweb only takes PATCH on `/Bios/Settings`)
- `/Bios/SD` and `/bios/defaultSD` — AMI's pending/default resources
- `/BiosStaticFiles`, `/ami/static-file`, `/DynamicExtension/RedfishExtensions`
- `GET`/`DELETE` on the `Actions/*` queues
- `POST` of a registry to `/redfish/v1/Registries/...`

Cleanest route is a bmcweb bbappend adding these routes and bridging them to the
existing `xyz.openbmc_project.BIOSConfigManager` D-Bus interface
(`BaseBIOSTable` ← BIOS POST, `PendingAttributes` → `/Bios/SD`). bmcweb already
has the TLS, auth and D-Bus plumbing; a standalone service on usb0:443 would
collide with bmcweb's own :443 bind.

## How the STOCK MegaRAC firmware implements the BMC side

Reference implementation, from the extracted 01.91.00 image
(`bmcfw/extracted/rootfs`, `info/X570D4I-2T_K5.PRJ`).

**USB stack — AMI proprietary, not Linux configfs.** But the same silicon:

| Module | Role |
|---|---|
| `usb1_hw.ko` | "AST2100/2050/2200/2150/2300/2400 **virtual USB hub** controller driver" — the same vhub we drive with `aspeed-vhub` |
| `usbe.ko` | AMI USB device core |
| `eth.ko` | "USB Ethernet Device module"; `CreateEthernetDescriptor()`, strings `RNDIS Notification Interface` / `RNDIS Data Interface` |
| `iUSB.ko`, `hid.ko`, `cdrom.ko`, `hdisk.ko` | the other vhub functions (KVM, virtual media) |

`CONFIG_SPX_FEATURE_ETHERNET_OVER_USB=YES`. MACs come from EEPROM —
`USB_HOST_MAC_EEPROM_OFFSET=0x3fd0`, `USB_DEV_MAC_EEPROM_OFFSET=0x3fd8`
(ASRock-specific, `FEATURE_STORE_USB_MAC_TO_EEPROM=YES`).

**Significant:** stock uses the *same vhub hardware* we do. So our `f_rndis`
gadget is not on the wrong controller, and the BIOS's `UsbRndis.Supported()`
matches on interface-class triples that `f_rndis` already satisfies. That makes
the June 2026 "never binds" wall more likely to be gate 1 below
(`PTT005`/ConnectController) than a descriptor mismatch.

**Addressing** (`/usr/local/redfish/config.lua`) — matches `RedfishHi.efi`'s
hardcoded pair exactly:

```
DEFAULT_HI_USBADDRESS       = 169.254.0.17    (BMC side)
DEFAULT_SYSTEMS_HI_ETH_ADDR = 169.254.0.18    (host side)
DEFAULT_HI_ROLE_ID          = HostInterfaceAdministrator
sessions: HostInterfaceSessionForFirmware (HI_Session_FW) / ...ForKernel (HI_Session_OS)
```

**Web tier:** lighttpd proxies `^/redfish` to a Lua Redfish core on
127.0.0.1:9080, backed by redis. `redfish_lighttpd_bios.conf` adds, for
`^/bios/`: `Content-Encoding: gzip` and `server.error-handler-404 = "/"`.

**How a request is recognised as coming from the host** —
`extensions/host-interface/host-interface-support-module.lua`:
`is_host_interface_connection` compares lighttpd's **`X-Server-Addr`** header
against `DEFAULT_HI_USBADDRESS`. It keys on *which local address the connection
landed on*, not on the client IP. `HI_is_host_enable` then checks redis
`Redfish:Managers:%s:HostInterfaces:%s:InterfaceEnabled`.

**Auth model — the most useful finding.** `db_init/host_interface.rcmd` seeds:

```
Redfish:Managers:$M:HostInterfaces:$HI:AuthNoneRoleId  = "Administrator"
Redfish:AccountService:Accounts:HI-FW:FirmwareAuthRoleId = "Administrator"
```

**Connections arriving on the host-interface address get Administrator with no
credentials.** So the IPMI `0x2C` credential bootstrap is the standards-track
convenience path, not a hard gate — which fits the BIOS having stalled after
cmd-01 while the design still works, and fits `HostAutoFW` being present in
`RedfishHi.efi`.

**BIOS config storage** (`hi-handlers/bios-hi.lua`): two blobs,
`BIOS_CURRENT_PATH` and `BIOS_FUTURE_PATH` — current vs pending. Tunable
`CLEAR_FUTURE_BIOS_SETTINGS_ON_CURRENT_SETTINGS_POST` wipes pending when the BIOS
POSTs current settings, i.e. that POST *is* the apply acknowledgement. Payload
keys `AttributeRegistry`, `Attributes`, `@Redfish.Settings`; emits
`ResourceCreated`/`ResourceModified` plus an audit-log entry.

**The BIOS installs Redfish extensions into the BMC.** `db_init/bios_dre.rcmd`
seeds `Dynamic:RedfishExtensions:<uuid>:<biosver>:Md5Checksum` for `ami`,
`ami_bios`, `ami_inventory`, `ami_util`, `FwConfig_LoadDef_dynextn` — keyed by
BIOS version (`170`, `180`, `18a`) — plus
`SET Redfish:bios:defaultSD:ResourceExists "true"`. That is what
`AmiRedfishDynExt`'s `/redfish/v1/DynamicExtension/RedfishExtensions` and
`POST /ami/static-file` are for: the BIOS compares MD5s for its own version and
uploads any missing gzip'd extension bundle, which lighttpd then serves under
`/bios/`.

## IMPLEMENTED 2026-08-03 — `bmcweb/0002-redfish-ami-host-interface-routes.patch`

Built, deployed and validated on hardware. **23/23 conformance checks pass**
(`meta-x570d4i/tools/rhi-replay.sh`), and a clean `cleansstate` rebuild applies
both patches to pristine upstream source and compiles.

Containment verified from the management LAN — every AMI path returns 401
unauthenticated, including upstream's own `/Systems/system/Bios`:

```
/redfish/v1/Systems/Self/Bios        401     (authenticated: 200, 373 attributes)
/redfish/v1/Systems/Self/Bios/SD     401     (authenticated: 200)
/redfish/v1/bios/defaultSD           401
/ami/static-file                     401
```

**Two real bugs that only testing found:**

1. **bmcweb's trie tries the `<str>` parameter branch BEFORE static siblings**
   (`findHelper`, `http/routing/trie.hpp`). A static
   `/redfish/v1/Systems/Self/Bios/` route can therefore *never* win against
   upstream's `/redfish/v1/Systems/<str>/Bios/` — the first attempt returned
   upstream's 404 "ComputerSystem named 'Self' was not found". The AMI methods
   had to be merged onto upstream's rule in `bios.hpp` instead. Static routes do
   still work where upstream claims no rule at that exact path (which is why
   `/Bios/SD` worked immediately), so only `/Bios/` and
   `/Bios/Actions/Bios.ResetBios/` needed merging.
2. **A `multipart/form-data` body leaves `req.body()` EMPTY** — bmcweb parses it
   into `FormPart`s. `/ami/static-file` is exactly what FirmwareConfigDrv posts
   as multipart (that WebKitFormBoundary string is a literal in the binary), so
   the first implementation archived a **zero-byte file and silently discarded
   the real upload**. `handleUpload` now takes a mutable `crow::Request&` and
   archives `req.multipart()` parts when present. Note also that bmcweb drops a
   *malformed* multipart body with no HTTP response at all (curl reports `000`),
   which is what made this look like a routing failure at first.

**One deliberate loosening:** `requestRoutesBiosService` no longer carries
`.privileges(getBios)`, because privileges are per-rule and the rule now also
serves the unauthenticated host-interface path. For LAN clients that changes
reading `/Bios` from "needs the Login privilege" to "needs any authenticated
session"; unauthenticated LAN access is still refused by the auth layer.

**Still unvalidated:** the actual BIOS interaction. That remains blocked behind
gates 0-2 below (DXE never completes; `PTT005`/`NetworkStackVar`).

## DECIDED 2026-08-03: the routes must live INSIDE bmcweb

A standalone service was written, built and deployed first
(`meta-x570d4i/recipes-phosphor/rhi/asrock-rhi`). It is **not viable**, for one
measured reason: **it cannot bind port 443.**

```
bmcweb listens on [::]:443            (/proc/net/tcp6, dual-stack wildcard)
asrock-rhi bind 127.0.0.1:443  ->  bind: Address already in use [system:98]
```

A specific-address listener cannot coexist with bmcweb's dual-stack wildcard on
443, and 443 is not negotiable: `RedfishHi.efi` builds `https://%a%a` with no
port, and it is the port advertised in the SMBIOS Type 42 record. Making bmcweb
give up the wildcard would mean enumerating every other address in
`bmcweb.socket` — and those are DHCP-dynamic on eth0, so a mistake costs
management access. Not worth it.

Secondary reasons the same way:

- **Certificate drift.** bmcweb owns `/etc/ssl/certs/https/server.pem` and
  regenerates it via `phosphor-certificate-manager@bmcweb`. A second process
  reading that file once at start-up would serve a stale certificate after any
  renewal — and the IPMI `0x2C` cmd-01 fingerprint must match whatever the BIOS
  actually connects to.
- bmcweb already carries our `BaseBIOSTable` → `/Bios` Attributes patch, so a
  separate service duplicates that logic.

**What the standalone attempt cost us: nothing structural.** `biosstore.cpp/.hpp`
(BaseBIOSTable ↔ Attributes conversion, blob storage, `safeName` traversal
guard), every JSON body shape, and `rhi-replay.sh` are all transport-agnostic
and port straight into a bmcweb route module. Keep the recipe directory as the
source of that code; it is **not** in `packagegroup-asrock-apps` and must not be
shipped, since its unit would fail on every boot with EADDRINUSE.

**The one property that must now be enforced explicitly.** A separate process
bound only to 169.254.0.17 is *structurally* unreachable from the management
LAN. Inside bmcweb that is no longer free: these routes are unauthenticated by
design, so each one MUST verify the connection's **local** address is the
host-interface address before doing anything, mirroring stock's `X-Server-Addr`
check in `host-interface-support-module.lua`. Get that wrong and an
unauthenticated BIOS-config surface appears on the management network.

**Not yet validated:** the route logic has never been exercised end to end. The
BMC image has no `curl`, and its dropbear refuses TCP forwarding
(`administratively prohibited`), so `rhi-replay.sh` could not reach a
loopback-bound instance. Validate by running the harness from a workstation
against the BMC once the routes are in bmcweb and reachable on 443.

### What this changes about our plan

- **Auth is nearly free.** Bind on 169.254.0.17 and treat those connections as
  Administrator. No bootstrap needed to get started.
- **The BIOS-config half is small** — two blobs plus the clear-on-POST rule,
  which maps cleanly onto `BaseBIOSTable` / `PendingAttributes`.
- **The dynamic-extension half is optional-ish.** For pure config sync we can
  answer `/DynamicExtension/RedfishExtensions` with matching MD5s so the BIOS
  skips uploading, or accept-and-discard. Unverified which the BIOS tolerates —
  this is the main unknown in the BMC-side work.
- No need to reproduce AMI's Lua/redis stack at all.

## Gate ladder — what must be true, in order

| # | Gate | State |
|---|---|---|
| 0 | **DXE completes so BDS/driver-connect runs** | **partially cleared** — on pristine 2.59F DXE _does_ run to completion and reaches `0x9A` USB-init twice, but the host loops ~111 s/cycle and then hangs at `0x99` Super I/O init. See [[pristine-259f-post-loop-and-0x99-hang]] |
| 1 | `PTT005` Network Stack Driver Support (`Setup+0x0008`) = 1 | currently `0x00` — **the next real gate** |
| 1b | `NetworkStackVar` created with both PXE off | variable absent; StdDefaults turn both PXE stacks ON → POST hang if 1 is set alone |
| 2 | USB-RNDIS chain binds → SNP exists | unproven; blocked behind 0/1. vhub shows all 5 ports `not attached`, usb0 rx 0 |
| 3 | IPMI `0x2C` cmd-01/02 credential bootstrap accepted | **NOT served** — measured 2026-08-03, both return `0xC1` Invalid Command. Not fatal: bmcweb grants unauthenticated Administrator on the HI address, mirroring stock's `AuthNoneRoleId` |
| 4 | BMC serves the endpoints above over TLS at 169.254.0.17 | **DONE** — bmcweb patch 0002, 23/23 conformance, containment verified |

**BMC-side addressing is verified end-to-end (2026-08-03).** The host does not
need any out-of-band knowledge of the BMC's address: `channel_config.json`
declares channel 8 = usb0, and RedfishHi reads it over KCS with "Get LAN
Configuration Parameters, channel 8". Measured on hardware:

| param | value | meaning |
|---|---|---|
| 3 | `a9 fe 00 11` | 169.254.0.17 |
| 4 | `01` | **Static** — this is why `00-bmc-usb0.network` must use `Scope=global`; a scope-link address is classified `LinkLocal` and `getIfAddr4` returns 0.0.0.0 |
| 5 | `02 00 16 92 54 17` | BMC MAC, matches `usb_network.sh` `BMC_MAC` |
| 6 | `ff ff 00 00` | 255.255.0.0 (/16) |

`channel_access.json` needs a matching `"8"` entry or the channel defaults to
`access_mode: disabled` even though the parameters above read back correctly
(fixed 2026-08-03; `Get Channel Access 8` now reports "always available").

Gates 0-2 are host-side. The BMC side is complete: gadget identity, addressing,
channel-8 advertisement, and the Redfish endpoints are all in place and tested.
