# meta-x570d4i2t — ASRock Rack X570D4I-2T OpenBMC port

OpenBMC machine layer for the **ASRock Rack X570D4I-2T**, a Mini-ITX AMD AM4
(Ryzen) server board with an **ASPEED AST2500** BMC, dual Intel X550-AT2 10GbE
host NICs and a dedicated Realtek RTL8211E 1GbE management PHY.

`MACHINE = "x570d4i2t"` · BMC SoC AST2500 · 64 MiB SPI-NOR.

## Building

```sh
source ./setup x570d4i2t     # NOT `. ./openbmc-env`
bitbake obmc-phosphor-image
```

## Flash / MTD layout (64 MiB BMC SPI-NOR)

| mtd  | name     | size  | notes                                    |
|------|----------|-------|------------------------------------------|
| mtd0 | bmc      | 64M   | whole device                             |
| mtd1 | u-boot   | 896K  | offset 0 = boot                          |
| mtd2 | u-boot-env |     |                                          |
| mtd3 | kernel   | 9M    |                                          |
| mtd4 | rofs     | 32M   |                                          |
| mtd5 | rwfs     | 22M   | overlay (see "overlay shadow" caveat)    |

The **host BIOS** SPI (separate 32 MiB chip) can be muxed to the BMC for
offline edits — see *BIOS access* below.

---

## Redfish Host Interface (usb0) — the SuperIO / iLPC2AHB gate

This board's headline OEM feature is the **DMTF Redfish Host Interface (RHI)**:
the BMC presents a USB network gadget (`usb0`, link-local **169.254.0.17/16**)
and the host BIOS talks Redfish to bmcweb over it to publish its BIOS attribute
registry and settings. Getting the host BIOS to actually bring this up requires
satisfying a chain of gates that the stock u-boot deliberately closes.

### How the AMI BIOS gates RHI bring-up

The host BIOS (AMI) Redfish Host Interface driver **`RedfishHi`** probes an LPC
Super-I/O at host I/O ports **0x4E/0x4F**, selects logical device **0x0D** (the
**iLPC2AHB** bridge), and reads **AST2500 GPIO E4** via AHB **0x1E780020 bit4**.
If that read does not return `1`, it aborts the host Redfish-over-USB interface
(returns `0xA000000000000001`) **before bringing up any networking**.

By default u-boot's `isolate_bmc()` (`arch/arm/mach-aspeed/ast2500/board_common.c`)
slams both paths shut:

- **SCU70 bit20** (`SCU_HWSTRAP_LPC_SIO_DEC_DIS`, reg `0x1E6E2070`) — disables
  the Super-I/O LPC decode. (SCU70 is write-1-to-set; cleared via the SCU7C
  strap-clear register `0x1E6E207C`.)
- **LPC HICRB bit6** (`LPC_HICRB_SIO_ILPC2AHB_DIS`, reg `0x1E789100`) — disables
  the iLPC2AHB bridge itself.

### What this layer does to open the gate

1. **u-boot** (`recipes-bsp/u-boot/`):
   - `files/enable-superio.cfg` sets `CONFIG_ASPEED_ENABLE_SUPERIO=y` **and**
     `CONFIG_ASPEED_ALLOW_DANGEROUS_BACKDOORS=y`. `ENABLE_SUPERIO` sits inside
     `if ASPEED_ALLOW_DANGEROUS_BACKDOORS` in `arch/arm/mach-aspeed/Kconfig`, so
     the parent symbol **must** be enabled too or Kconfig silently drops it.
     This is intentionally a "dangerous backdoor": the iLPC2AHB grants the host
     read access to BMC address space — required here for the RHI.
   - `files/0001-ast2500-keep-ilpc2ahb-enabled-with-superio.patch` **actively
     clears** the two disable bits (SCU7C strap-clear for bit20; HICRB RMW for
     bit6). A passive "skip the disable" approach is **not** sufficient — the
     strap/reset defaults already leave both DISABLED, so the bits must be
     explicitly cleared.
   - Verified live: `SCU70 = 0x7101D246`, `HICRB = 0xC000`.

2. **Kernel device tree** (`recipes-kernel/linux/`): a GPIO hog drives
   **GPIOE4 high** so the BIOS's iLPC2AHB read of `0x1E780020` bit4 returns `1`:

   ```dts
   redfish-hi-ready-hog {
       gpio-hog;
       gpios = <ASPEED_GPIO(E, 4) GPIO_ACTIVE_HIGH>;
       output-high;
       line-name = "output-redfish-hi-ready";
   };
   ```

3. **BIOS Setup** (`Setup`/REDF000, VarID 1, VarOffset 55): "Redfish" enable
   must be nonzero. Confirmed **= 0x01 (ENABLED)** on this board in both BIOS
   copies; default is enabled, so no host-side change is normally required.

### BIOS-side driver chain (from the firmware decompile)

| BIOS module                  | role                                                       |
|------------------------------|------------------------------------------------------------|
| `RedfishHi [cef05965]`       | Host-interface bring-up: SMBIOS Type 42 + EFI NIC IP + creds; gated by the iLPC2AHB/GPIOE4 read above |
| `AmiRedFishApi [d4395796]`   | Redfish transport library                                  |
| `AmiRedfishDynExt [3b1de95d]`| Dynamic extension                                          |
| `FirmwareConfigDrv [1c5c6e7e]` | **The actual RHI client** — the only BIOS binary that references `169.254.0.17`. GET/POST/PATCHes `/redfish/v1/Systems/Self/Bios`, `/Bios/SD` (AMI pending-settings), `BiosAttributeRegistry…json`, `/redfish/v1/BiosStaticFiles`, `/ami/static-file`; uploads `SetupData.xml`. |

Chain: `RedfishHi` must succeed → `AmiRedFishApi` transport → `FirmwareConfigDrv`
sends. If `RedfishHi` aborts at the GPIOE4 gate, there is no EFI NIC and no
transport, so nothing is ever transmitted on usb0.

### USB gadget must be RNDIS

The BIOS ships **exactly one** USB-network class driver — `UsbRndisDriverSrc
[11e32c34]` (**RNDIS**). There is **no CDC-ECM/NCM driver** in the firmware. So
for the EFI pre-OS phase to enumerate the gadget, the BMC **must present an
RNDIS interface**. `recipes-phosphor/network/usb-network/usb_network.sh` builds
the gadget with `functions/rndis.usb0` plus the Microsoft OS descriptors
(`os_desc/use=1`, `b_vendor_code=0xcd`, `qw_sign=MSFT100`,
`interface.rndis/compatible_id=RNDIS`, `sub_compatible_id=5162001`).

Addressing is owned by systemd-networkd (`00-bmc-usb0.network`,
169.254.0.17/16, **scope global** so phosphor-network reports `Static` for IPMI
"Get LAN ch8"); the gadget script does not assign the address.

---

## BIOS access (host SPI mux)

To read/edit host BIOS Setup vars offline, with the **host powered Off**:

```sh
gpioset $(gpiofind control-bios-spi-mux-n)=0      # GPIOJ1 (gpiochip0 line 73); hold in background
echo 1e630000.spi > /sys/bus/platform/drivers/spi-aspeed-smc/bind
cat /proc/mtd                                     # host BIOS appears as a new 32 MiB mtd
# ... dd / flashcp ...
# release: kill the gpioset, then unbind 1e630000.spi
```

The host BIOS uses the **AMI NVAR** format (not EDK2 VSS); mtd edits stick (no
checksum). A ~30 s AC power-drain is needed for some Setup changes to take.

---

## Layer contents

- `recipes-bsp/u-boot/` — SuperIO/iLPC2AHB enablement (above).
- `recipes-kernel/linux/` — machine device tree (managed `.dts` + Makefile
  patch), kernel config fragment, GPIOE4 hog.
- `recipes-phosphor/network/usb-network/` — RNDIS gadget + usb0 addressing.
- `recipes-phosphor/ipmi/asrock-ipmi-oem/` — ASRock/AMI OEM IPMI command set,
  SMBIOS-over-IPMI (AMI MDR) handling, Redfish↔IPMI bridge.
- `recipes-phosphor/sensors/`, `recipes-asrock/nct6779-bridge/` — NCT6779 hwmon
  bridge daemon, fan/temp mapping.
- `recipes-phosphor/flash/` — host BIOS flashing via the GPIOJ1 SPI mux.
- `recipes-asrock/vga-enable/`, `recipes-graphics/aspeed-video-watchdog/` —
  KVM/VGA.

## Caveats

- **Overlay shadow hazard:** dev hot-patches leave copy-ups/whiteouts in the
  rwfs overlay (mtd5) that survive firmware updates and can silently mask a new
  image. Clear them after deploying a clean build.
- The `enable-superio` / iLPC2AHB backdoor is a **deliberate security
  trade-off** required by the host RHI. Do not enable it on builds where host
  read access to BMC address space is unacceptable.
