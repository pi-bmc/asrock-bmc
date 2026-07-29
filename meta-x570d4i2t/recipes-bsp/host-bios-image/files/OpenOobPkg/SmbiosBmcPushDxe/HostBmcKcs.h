/** @file
  Shared host->BMC KCS transport for the modules injected into the AMI
  SendInfoBmcIpmiDxe FFS slot (SmbiosBmcPushDxe + BiosCfgOobDxe).

  Both halves of the injected driver talk to the BMC over the same LPC KCS
  system interface (I/O 0xCA2/0xCA3), so the transaction primitive lives here
  and is implemented once in SmbiosBmcPushDxe.c.

  Copyright (c) 2024, ASRockRack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HOST_BMC_KCS_H_
#define HOST_BMC_KCS_H_

#include <Uefi.h>

/**
  Run one IPMI request/response transaction over the host KCS interface.

  @param[in]      Req      Request bytes, starting with (NetFn << 2) then Cmd.
  @param[in]      ReqLen   Length of Req; must be >= 2.
  @param[out]     Rsp      Response buffer. On success holds
                           [NetFn|LUN][Cmd][CompletionCode][data...].
  @param[in,out]  RspLen   On entry the size of Rsp, on exit the byte count
                           actually received.

  @retval EFI_SUCCESS       A response was read (inspect Rsp[2] for the IPMI
                            completion code).
  @retval EFI_NOT_READY     KCS never went idle; the BIOS holds the interface.
  @retval EFI_TIMEOUT       The BMC stopped responding mid-transaction.
  @retval EFI_DEVICE_ERROR  KCS left the expected state machine phase.
**/
EFI_STATUS
KcsTxn (
  CONST UINT8  *Req,
  UINT32       ReqLen,
  UINT8        *Rsp,
  UINT32       *RspLen
  );

/** Write a POST code to port 0x80 (BMC-snooped progress marker). */
VOID
Post (
  UINT8  Value
  );

/**
  Initialise the BIOS-config OOB half. Called once from the driver entry point,
  before any events are armed.
**/
VOID
BiosCfgOobInit (
  VOID
  );

/**
  Run one BIOS-config OOB exchange with the BMC. Safe to call repeatedly: it
  is internally idempotent and returns immediately once the boot's work is
  finished. Called from the same EndOfDxe / timer / ReadyToBoot hooks that
  drive the SMBIOS push.
**/
VOID
BiosCfgOobRun (
  VOID
  );

/**
  @retval TRUE  The BIOS-config exchange finished for this boot, so the retry
                timer no longer has to run on its account.
**/
BOOLEAN
BiosCfgOobIsDone (
  VOID
  );

#endif /* HOST_BMC_KCS_H_ */
