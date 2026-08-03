# Host BIOS flashing procedure (X570D4I-2T, via the BMC)

The BMC at **10.0.80.1** (default OpenBMC creds `root` / `0penBmc`) can take over the
host's BIOS SPI flash through an onboard mux. This is the recovery path as well as
the deployment path, which is why bricking the host BIOS is cheap here.

**Never flash `/dev/mtd0`** — that is the BMC's own 64 MiB flash. The host BIOS is
`mtd6`, and `mtd6` only exists while the mux is pulled.

## Preconditions

The host must be **powered off**. Check both, not just one:

```sh
busctl get-property xyz.openbmc_project.State.Chassis /xyz/openbmc_project/state/chassis0 \
  xyz.openbmc_project.State.Chassis CurrentPowerState     # -> PowerState.Off
busctl get-property xyz.openbmc_project.State.Host /xyz/openbmc_project/state/host0 \
  xyz.openbmc_project.State.Host CurrentHostState         # -> HostState.Off
```

Cold power cycles need roughly **120 s off** before powering back on, or the PSU
protection latches (`beepPowerFail=8`). Graceful power-off is ignored by this host —
use the hard chassis transition.

## Acquire the flash

`gpiochip0` line **73** is `control-bios-spi-mux-n`, active-high, and reads **1**
when the flash belongs to the host. Pull it to 0 and hold it; `gpioset` must stay
resident, so use `--background --mode=signal`.

```sh
gpioset --background --mode=signal gpiochip0 73=0
echo spi1.0 > /sys/bus/spi/drivers/spi-nor/bind
cat /proc/mtd      # mtd6: 02000000 00010000 "bios"  <- 32 MiB, appears now
```

`spi0.0` is already bound and is the BMC's own flash. Only `spi1.0` is the host's.

## Back up before writing — this is the recovery image

Stream it off rather than staging on the BMC, and verify both ends:

```sh
ssh root@10.0.80.1 'dd if=/dev/mtd6 bs=64k' > backup-live.bin   # ~24 s
sha256sum backup-live.bin
ssh root@10.0.80.1 'sha256sum /dev/mtd6'                        # must match
```

## Write

`/tmp` is tmpfs with ~240 MiB free, so a 32 MiB upload fits. `flash_erase` and
`mtd_debug` are **absent**; `flashcp` is present and does erase + write + verify.

```sh
ssh root@10.0.80.1 'cat > /tmp/bios-new.bin' < candidate.bin
ssh root@10.0.80.1 'sha256sum /tmp/bios-new.bin'    # confirm upload first
ssh root@10.0.80.1 'flashcp -v /tmp/bios-new.bin /dev/mtd6'   # ~3 min for 32 MiB
ssh root@10.0.80.1 'sha256sum /dev/mtd6'            # independent readback check
```

Re-assert the guards immediately before the write: `mtd6` is the 32 MiB `"bios"`
device, chassis is Off, image is exactly 33554432 bytes. NOR can only clear bits
without an erase, so any 0→1 change needs a full `flashcp`, not a partial `dd`.

## Release the flash back to the host

BusyBox has **no `pkill`** — kill `gpioset` by PID.

```sh
echo spi1.0 > /sys/bus/spi/drivers/spi-nor/unbind
for p in $(pgrep gpioset); do kill $p; done
gpioget gpiochip0 73    # -> 1  (host side; errors "Device or resource busy" if still held)
cat /proc/mtd           # mtd6 gone
```

## Observing the boot

Clear the postcode history first so the boot is unambiguous:

```sh
rm -f /var/lib/phosphor-post-code-manager/host0/*
systemctl restart xyz.openbmc_project.State.Boot.PostCode@0.service
```

Then power on and read the sequence back. Note `busctl get-property ... Boot.Raw
Value` prints the current code in **decimal** — `79` is `0x4F`, not `0x79`. For the
full ordered history use the D-Bus method (BusyBox `head` has no `-c`, so redirect
to a file and decode off-box):

```sh
busctl call xyz.openbmc_project.State.Boot.PostCode0 \
  /xyz/openbmc_project/State/Boot/PostCode0 \
  xyz.openbmc_project.State.Boot.PostCode GetPostCodes q 1
```

The on-disk history files under `/var/lib/phosphor-post-code-manager/host0/` are
cereal-serialized — use the D-Bus method, not the raw file.

## Did our drivers run? Read it out of the flash

This is the only signal that has proven reliable — see
[[host-bios-driver-injection-results]] for why IPMI counts and POST codes are not.
Our drivers record progress to NV UEFI variables, which land in AMI's NVAR store
in the SPI flash. So: let the host boot, power it down, pull the mux again, read
`mtd6` back, and decode.

```sh
ssh root@10.0.80.1 'dd if=/dev/mtd6 bs=64k' > readback.bin
python3 tools/probecheck.py --rom readback.bin --ffs 0x1E153F8:0x1E1CE50
```

`--ffs` excludes the span holding the injected drivers: their PE32s contain the
variable names too (as UTF-16, and sometimes ASCII debug strings), so only hits
*outside* that span prove a real variable write. Always run the same command
against the candidate image **before** flashing — it must report nothing, which
is the negative control that makes the post-boot result mean something.

Variables in use: `OobProbe` (OobProbeDxe), `OobSmbios` (SmbiosBmcPushDxe),
`OobBiosCfg` (BiosCfgOobDxe).

## Weaker signals, kept for context

Functional evidence beats POST codes, because our POST ranges collide with AMI's
(see [[host-bios-driver-injection-results]]):

- **Did SmbiosBmcPushDxe run?** `/var/lib/smbios/smbios2` mtime should update, and
  `journalctl | grep IPMI-REQ` should show `netfn=0x2e cmd=0x80` (blob) traffic.
- **Did BiosCfgOobDxe run?** look for `netfn=0x30 cmd=0xd3/0xd5/0xd6`.
- AMI's own `DxeIpmiBmcInitialize` produces exactly two requests on `ch=3`:
  `netfn=0x06 cmd=0x04` then `cmd=0x01`. Seeing only those two means none of our
  drivers issued IPMI.
- SOL console: `/var/log/obmc-console.log`.
