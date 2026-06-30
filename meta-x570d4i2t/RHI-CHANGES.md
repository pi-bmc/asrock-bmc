# Redfish Host Interface — Implementation Changes

This document describes all changes made to the `meta-x570d4i2t` layer to
implement the DMTF Redfish Host Interface (RHI) credential-bootstrapping flow,
enabling the ASRock X570D4I-2T's AMI BIOS to authenticate to bmcweb over the
BMC's USB gadget network interface (`usb0`, 169.254.0.17).

## Problem Statement

During POST the AMI BIOS performs a credential-bootstrapping handshake with the
BMC over IPMI KCS (NetFn 0x2C, Group Extension). The sequence is:

1. **Cmd 0x01 — GetManagerCertificateFingerprint**: BIOS requests the BMC's TLS
   certificate fingerprint to verify the HTTPS endpoint.
2. **Cmd 0x02 — GetBootstrapAccountCredentials**: BIOS requests a one-time
   username/password to authenticate to the Redfish service.

If either command returns CC 0xC1 (invalid/unsupported), the BIOS **aborts the
entire RHI sequence** — it never brings up USB RNDIS networking, and `usb0` on
the BMC receives zero packets. Stock OpenBMC implements neither command.

## Root Cause Analysis (Why Previous Attempts Failed)

Initial implementation registered the handlers and returned CC=0x00, but the
BIOS still stopped after cmd 0x01 without proceeding to cmd 0x02. Reverse
engineering of the stock AMI MegaRAC firmware (`libipmiredfishhostiface.so`)
via ARM disassembly revealed **two format mismatches**:

### 1. Response Group Byte Mismatch

The BIOS sends NetFn 0x2C with group byte `0x00` in the request, but the stock
MegaRAC firmware hard-codes `0x52` ('R' = DMTF Redfish) as the **response**
group byte. The phosphor-ipmi-host framework unconditionally echoes the
**request** group byte (0x00) back in the response. The BIOS validates this
byte and rejects responses containing 0x00.

### 2. Fingerprint Format Mismatch

The initial implementation computed a SHA-256 hash of the TLS certificate
(32 bytes). ARM disassembly of the stock firmware at offset 0x2100 showed it
actually reads `/conf/certs/server.pem`, strips PEM header/footer lines
(lines containing "CERTIFICATE"), concatenates the remaining base64 body, and
copies the **first 50 bytes** verbatim — it is not a cryptographic hash at all,
just a base64 prefix used as a fingerprint-like identifier.

The stock response wire format for cmd 0x01 is:
```
[CC=0x00] [0x52] [0x01] [50 bytes base64 prefix]   — total 53 bytes (0x35)
```

## Changes Made

### 1. `redfishhostiface.cpp` — RHI IPMI Handler

**Path**: `recipes-phosphor/ipmi/asrock-ipmi-oem/src/redfishhostiface.cpp`

Implements both credential-bootstrapping commands as a phosphor-ipmi-host
group extension provider:

- **Cmd 0x01 — GetManagerCertificateFingerprint**:
  - Reads `/etc/ssl/certs/https/server.pem` (bmcweb's TLS cert)
  - Captures the base64 body **between the `BEGIN/END CERTIFICATE` markers
    only** (bmcweb's PEM is a combined file with the private key block first;
    the fingerprint must reflect the certificate, never the key)
  - Returns the first 50 bytes as the fingerprint (matching stock format)
  - Sets `ctx->group = 0x52` to override the response group byte
  - Returns CC 0xCB if certificate number ≠ 1 (matches stock error code)

- **Cmd 0x02 — GetBootstrapAccountCredentials**:
  - Generates random 16-character alphanumeric username and password
    using `/dev/urandom` (matches stock `Generate16ByteRandomAlpNumPasswd`)
  - Creates a phosphor-user-manager user via D-Bus (`CreateUser`) with
    `priv-admin` privilege and `redfish` group
  - Sets the password via PAM (`pam_chauthtok` on the "passwd" stack)
  - Sets `ctx->group = 0x52` for the response group byte
  - Returns `[username 16B][password 16B]` (matches stock wire format)

- **Registration**: Uses `registerGroupHandler()` at `prioOemBase` under
  group 0x00 (matching the BIOS request) for both commands.

- **Dependencies**: PAM (`libpam`) for password setting; no OpenSSL dependency
  (removed — the fingerprint is not a hash).

### 2. ipmid Framework Patch — Group Extension Response Override

**Path**: `recipes-phosphor/ipmi/phosphor-ipmi-host/0001-group-ext-use-ctx-group-for-response-echo.patch`

A minimal one-line patch to `ipmid-new.cpp` in phosphor-ipmi-host:

```diff
-    prefix.pack(bytes);
+    prefix.pack(static_cast<uint8_t>(request->ctx->group));
```

**Justification**: The framework's `executeIpmiGroupCommand()` at line 307
unconditionally echoes the raw request group byte in the response prefix. This
is correct for standard IPMI group extensions where request and response group
bytes match, but the AMI MegaRAC RHI protocol violates this convention — the
BIOS sends group 0x00 but expects 0x52 back. Alternative approaches were
considered and rejected:

- **`registerHandler()` for raw NetFn 0x2C**: Dead code — the group extension
  dispatcher intercepts all NetFn 0x2C traffic before regular handlers.
- **Raw KCS handler bypass**: Would require reimplementing the entire IPMI
  message framing outside the framework.
- **Registering under group 0x52**: The dispatcher matches on the request group
  byte, which is 0x00, so a handler registered under 0x52 would never match.

The chosen approach is backward-compatible: `executeIpmiGroupCommand()` sets
`ctx->group` from the request byte before dispatching, so handlers that do not
modify `ctx->group` produce identical behavior to before the patch.

The patch includes an `Upstream-Status: Pending` header as required by Yocto
QA checks.

### 3. `phosphor-ipmi-host_%.bbappend` — Yocto Integration

**Path**: `recipes-phosphor/ipmi/phosphor-ipmi-host_%.bbappend`

Added `SRC_URI:append` entry for the ipmid patch file and updated the
`FILESEXTRAPATHS` to include the patch directory.

### 4. `meson.build` — Build System Update

**Path**: `recipes-phosphor/ipmi/asrock-ipmi-oem/meson.build`

- Added `pam_dep` (libpam) as a dependency for the PAM-based password setting
  in the bootstrap credential handler.
- Removed `openssl_dep` — no longer needed since the fingerprint is a base64
  prefix, not a cryptographic hash.
- Added `src/redfishhostiface.cpp` to the source file list.

### 5. `asrock-ipmi-oem_0.1.bb` — Recipe Update

**Path**: `recipes-phosphor/ipmi/asrock-ipmi-oem_0.1.bb`

- Added `libpam` to `DEPENDS`.
- Removed `openssl` from `DEPENDS`.
- Added `src/redfishhostiface.cpp` to `SRC_URI`.

## Stock Firmware Reference

All wire formats and behaviors were reverse-engineered from the stock ASRock
X570D4I-2T BMC firmware v01.91.00:

- **Binary**: `/usr/local/lib/ipmi/libipmiredfishhostiface.so.6.6.1`
- **Architecture**: ARM (AST2500 / ARM1176JZS)
- **Key symbols**: `g_coreHIGroup`, `g_coreHIGroup_CmdHndlr`
- **Cert path**: `/conf/certs/server.pem`
- **Fingerprint**: 50 bytes, extracted via 3×16-byte LDM/STM + 1×LDRH/STRH
  (48 + 2 = 50 bytes) from the concatenated base64 PEM body
- **Credential generation**: Redis-backed (`GET ENV:ManagerSelf`,
  `GET ENV:HostInterfaceSelf`), random alphanumeric username/password

## Deployment

After building, strip and deploy the two binaries to the BMC:

```sh
# Build
source ./setup x570d4i2t
bitbake -c compile -f phosphor-ipmi-host   # patched ipmid
bitbake -c compile -f asrock-ipmi-oem      # RHI handler .so

# Strip with the cross toolchain
STRIP=$(find build/x570d4i2t/tmp/work/arm1176jzs-*/phosphor-ipmi-host/*/recipe-sysroot-native \
    -name 'arm-openbmc-linux-gnueabi-strip' | head -1)
cp .../build/ipmid /tmp/ipmid.stripped && $STRIP /tmp/ipmid.stripped
cp .../build/libzasrockoemcmds.so.0.1 /tmp/lib.stripped && $STRIP /tmp/lib.stripped

# Deploy (BMC at 10.0.80.1, password 0penBmc)
scp /tmp/ipmid.stripped root@10.0.80.1:/dev/shm/ipmid.new
scp /tmp/lib.stripped   root@10.0.80.1:/dev/shm/lib.new
ssh root@10.0.80.1 'systemctl stop phosphor-ipmi-host; \
    cp -f /dev/shm/ipmid.new /usr/bin/ipmid; \
    cp -f /dev/shm/lib.new /usr/lib/ipmid-providers/libzasrockoemcmds.so.0.1; \
    sync; systemctl start phosphor-ipmi-host; rm -f /dev/shm/*.new'
```

**Critical**: The debug-build ipmid is ~10 MB; the BMC's AST2500 (256 MB RAM)
can load it but `systemctl start` will hang. Always strip before deploying
(target size: ~314 KB for ipmid, ~544 KB for the .so).

## Verification

1. **Handler registration** — check journal:
   ```
   journalctl -u phosphor-ipmi-host | grep "Redfish Host Interface"
   # Expect: "ASRock Redfish Host Interface IPMI group registered (NetFn 0x2C / Group 0x00, cmds 0x01 0x02)"
   ```

2. **Cold POST** — power cycle via chassis off→on (not ForceRestart):
   ```sh
   busctl call xyz.openbmc_project.State.Chassis \
       /xyz/openbmc_project/state/chassis0 \
       org.freedesktop.DBus.Properties Set ssv \
       xyz.openbmc_project.State.Chassis RequestedPowerTransition \
       s xyz.openbmc_project.State.Chassis.Transition.Off
   # Wait for PowerState.Off, then:
   busctl call ... s xyz.openbmc_project.State.Chassis.Transition.On
   ```

3. **Monitor IPMI traffic** — BIOS sends cmd 0x01 approximately 105 seconds
   into POST:
   ```
   journalctl -u phosphor-ipmi-host -f | grep "netfn=0x2C\|RHI"
   ```

4. **USB traffic** — success criteria is `rx_packets > 0`:
   ```
   cat /sys/class/net/usb0/statistics/rx_packets
   ```

5. **RNDIS kernel debug** (optional):
   ```sh
   for f in rndis.c f_rndis.c u_ether.c; do
       echo "file $f +p" > /sys/kernel/debug/dynamic_debug/control
   done
   dmesg -w | grep -i rndis
   ```

## Files Summary

| File | Action | Purpose |
|------|--------|---------|
| `recipes-phosphor/ipmi/asrock-ipmi-oem/src/redfishhostiface.cpp` | New | RHI IPMI handler (cmds 0x01, 0x02) |
| `recipes-phosphor/ipmi/phosphor-ipmi-host/0001-group-ext-use-ctx-group-for-response-echo.patch` | New | Allow response group byte override |
| `recipes-phosphor/ipmi/phosphor-ipmi-host_%.bbappend` | Modified | Add patch to ipmid build |
| `recipes-phosphor/ipmi/asrock-ipmi-oem/meson.build` | Modified | Add PAM dep, remove OpenSSL, add source |
| `recipes-phosphor/ipmi/asrock-ipmi-oem_0.1.bb` | Modified | Add libpam dep, remove openssl, add source |
