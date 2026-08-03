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

## Gate ladder — what must be true, in order

| # | Gate | State |
|---|---|---|
| 0 | **DXE completes so BDS/driver-connect runs** | **FAILING** — measured, no `DxeSmmReadyToLock`, no PciIo, no SNP possible |
| 1 | `PTT005` Network Stack Driver Support (`Setup+0x0008`) = 1 | currently `0x00` |
| 1b | `NetworkStackVar` created with both PXE off | variable absent; StdDefaults turn both PXE stacks ON → POST hang if 1 is set alone |
| 2 | USB-RNDIS chain binds → SNP exists | unproven; the June 2026 wall (UsbLan never bound) is plausibly explained by gate 1 |
| 3 | IPMI `0x2C` cmd-01/02 credential bootstrap accepted | cmd-01 returns cc=0 but BIOS stopped; `HostAutoFW` user may bypass |
| 4 | BMC serves the endpoints above over TLS at 169.254.0.17 | not written |

Gate 4 is the only one that can be built and tested **today**, because it needs
no host: replay the exact requests above with curl against the service. Doing
that now means the BMC side is ready the moment gates 0-2 clear.
