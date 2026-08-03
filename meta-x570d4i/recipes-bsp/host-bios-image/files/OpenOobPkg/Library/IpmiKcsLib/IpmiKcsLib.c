/** @file
  Minimal IPMI transport over the host KCS interface (IPMI v2.0 section 9.15).

  The state machine is deliberately written against the spec's flow chart with
  no BMC-specific shortcuts, because the AST2500's KCS emulation on this board
  is timing-sensitive during its own boot: it will sit in WRITE_STATE for tens
  of milliseconds before draining a byte.

  All PCDs referenced here are PcdsFixedAtBuild, so FixedPcdGetNN() resolves to
  a literal at compile time and this library needs no working PcdLib at runtime
  — which matters because it is linked into drivers injected into a foreign
  (AMI) DXE core whose PCD database we cannot rely on.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/IpmiKcsLib.h>

#define KCS_DATA_PORT  FixedPcdGet16 (PcdIpmiKcsDataPort)
#define KCS_CMD_PORT   FixedPcdGet16 (PcdIpmiKcsCmdPort)
#define KCS_SPIN       FixedPcdGet32 (PcdIpmiKcsSpinCount)

//
// KCS status register bits (read from the command port).
//
#define KCS_STATUS_OBF  BIT0
#define KCS_STATUS_IBF  BIT1

//
// KCS interface state, encoded in the top two bits of the status register.
//
#define KCS_STATE(Status)  ((UINT8)((Status) >> 6))
#define KCS_STATE_IDLE     0x00
#define KCS_STATE_READ     0x01
#define KCS_STATE_WRITE    0x02
#define KCS_STATE_ERROR    0x03

//
// Control codes written to the command port.
//
#define KCS_CTL_GET_STATUS   0x60
#define KCS_CTL_WRITE_START  0x61
#define KCS_CTL_WRITE_END    0x62
#define KCS_CTL_READ         0x68

/**
  Spin until the Input Buffer Full flag clears, i.e. the BMC has consumed the
  byte we wrote.
**/
STATIC
EFI_STATUS
KcsWaitInputEmpty (
  VOID
  )
{
  UINT32  Spin;

  for (Spin = 0; Spin < KCS_SPIN; Spin++) {
    if ((IoRead8 (KCS_CMD_PORT) & KCS_STATUS_IBF) == 0) {
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

/**
  Spin until the Output Buffer Full flag sets, i.e. the BMC has a byte for us.
**/
STATIC
EFI_STATUS
KcsWaitOutputFull (
  VOID
  )
{
  UINT32  Spin;

  for (Spin = 0; Spin < KCS_SPIN; Spin++) {
    if ((IoRead8 (KCS_CMD_PORT) & KCS_STATUS_OBF) != 0) {
      return EFI_SUCCESS;
    }
  }

  return EFI_TIMEOUT;
}

/**
  Discard a stale byte in the output buffer.

  The spec requires a dummy read after every phase transition; skipping it
  leaves OBF set and the next handshake desynchronises.
**/
STATIC
VOID
KcsFlushOutput (
  VOID
  )
{
  if ((IoRead8 (KCS_CMD_PORT) & KCS_STATUS_OBF) != 0) {
    IoRead8 (KCS_DATA_PORT);
  }
}

/**
  Wait for the interface to be idle and unowned before starting a transaction.

  The platform firmware issues its own IPMI traffic throughout DXE, so barging
  in mid-transaction would corrupt both requests.
**/
STATIC
EFI_STATUS
KcsAcquire (
  VOID
  )
{
  UINT32  Spin;
  UINT8   Status;

  for (Spin = 0; Spin < KCS_SPIN; Spin++) {
    Status = IoRead8 (KCS_CMD_PORT);
    if (((Status & KCS_STATUS_IBF) == 0) && (KCS_STATE (Status) == KCS_STATE_IDLE)) {
      KcsFlushOutput ();
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_WARN, "IpmiKcs: interface never went idle (status %02x)\n", Status));
  return EFI_NOT_READY;
}

/**
  Write one byte and confirm the interface is still in WRITE_STATE afterwards.
**/
STATIC
EFI_STATUS
KcsWriteByte (
  IN UINT16  Port,
  IN UINT8   Value
  )
{
  EFI_STATUS  Status;

  IoWrite8 (Port, Value);

  Status = KcsWaitInputEmpty ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (KCS_STATE (IoRead8 (KCS_CMD_PORT)) != KCS_STATE_WRITE) {
    return EFI_DEVICE_ERROR;
  }

  KcsFlushOutput ();
  return EFI_SUCCESS;
}

/**
  Drive the write phase: WRITE_START, all but the last byte, WRITE_END, then the
  final byte. The final byte must follow WRITE_END, not precede it — this is the
  most commonly mis-implemented part of the KCS flow.
**/
STATIC
EFI_STATUS
KcsWritePhase (
  IN CONST UINT8  *Frame,
  IN UINTN        FrameSize
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  Status = KcsWriteByte (KCS_CMD_PORT, KCS_CTL_WRITE_START);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index + 1 < FrameSize; Index++) {
    Status = KcsWriteByte (KCS_DATA_PORT, Frame[Index]);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Status = KcsWriteByte (KCS_CMD_PORT, KCS_CTL_WRITE_END);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The last data byte: do not require WRITE_STATE afterwards, because the BMC
  // legitimately transitions to READ_STATE as soon as it has the full request.
  //
  IoWrite8 (KCS_DATA_PORT, Frame[FrameSize - 1]);
  return KcsWaitInputEmpty ();
}

/**
  Drive the read phase, collecting bytes until the interface returns to IDLE.

  Bytes beyond Capacity are read and discarded rather than left in the
  interface: abandoning a transaction mid-read desynchronises KCS for every
  subsequent caller, including the platform firmware's own IPMI stack.
**/
STATIC
EFI_STATUS
KcsReadPhase (
  OUT UINT8  *Frame,
  IN  UINTN  Capacity,
  OUT UINTN  *Received,
  OUT BOOLEAN *Truncated
  )
{
  EFI_STATUS  Status;
  UINT32      Spin;
  UINT8       StatusReg;
  UINT8       Value;

  *Received  = 0;
  *Truncated = FALSE;

  while (TRUE) {
    for (Spin = 0; Spin < KCS_SPIN; Spin++) {
      StatusReg = IoRead8 (KCS_CMD_PORT);
      if ((KCS_STATE (StatusReg) == KCS_STATE_READ) ||
          (KCS_STATE (StatusReg) == KCS_STATE_IDLE))
      {
        break;
      }

      if (KCS_STATE (StatusReg) == KCS_STATE_ERROR) {
        DEBUG ((DEBUG_ERROR, "IpmiKcs: BMC signalled ERROR_STATE\n"));
        return EFI_DEVICE_ERROR;
      }
    }

    if (Spin >= KCS_SPIN) {
      return EFI_TIMEOUT;
    }

    if (KCS_STATE (StatusReg) == KCS_STATE_IDLE) {
      //
      // IDLE with OBF set carries one final dummy byte to consume.
      //
      KcsFlushOutput ();
      return EFI_SUCCESS;
    }

    Status = KcsWaitOutputFull ();
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Value = IoRead8 (KCS_DATA_PORT);
    if (*Received < Capacity) {
      Frame[*Received] = Value;
    } else {
      *Truncated = TRUE;
    }

    (*Received)++;

    //
    // Ask for the next byte. The BMC answers by either handing us another byte
    // (still READ_STATE) or dropping to IDLE.
    //
    IoWrite8 (KCS_DATA_PORT, KCS_CTL_READ);
    Status = KcsWaitInputEmpty ();
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
}

/**
  Run one complete request/response exchange: acquire the interface, drive the
  write phase, drive the read phase.

  The single place the KCS state machine is implemented. Both the framed
  (IpmiKcsCommand) and raw (IpmiKcsRawTransaction) entry points funnel here so
  there is exactly one copy of the handshake to get wrong.
**/
STATIC
EFI_STATUS
KcsExchange (
  IN     CONST UINT8  *Frame,
  IN     UINTN        FrameSize,
  OUT    UINT8        *Reply,
  IN     UINTN        ReplyCapacity,
  OUT    UINTN        *Received,
  OUT    BOOLEAN      *Truncated
  )
{
  EFI_STATUS  Status;

  Status = KcsAcquire ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = KcsWritePhase (Frame, FrameSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "IpmiKcs: write phase failed - %r\n", Status));
    return Status;
  }

  Status = KcsReadPhase (Reply, ReplyCapacity, Received, Truncated);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "IpmiKcs: read phase failed - %r\n", Status));
  }

  return Status;
}

VOID
EFIAPI
OobPostCode (
  IN UINT8  Value
  )
{
  IoWrite8 (FixedPcdGet16 (PcdOobPostCodePort), Value);
}

EFI_STATUS
EFIAPI
IpmiKcsRawTransaction (
  IN     CONST UINT8  *Request,
  IN     UINT32       RequestSize,
  OUT    UINT8        *Response,
  IN OUT UINT32       *ResponseSize
  )
{
  EFI_STATUS  Status;
  UINTN       Received;
  BOOLEAN     Truncated;

  if ((Request == NULL) || (Response == NULL) || (ResponseSize == NULL) ||
      (RequestSize < 2))
  {
    return EFI_INVALID_PARAMETER;
  }

  Received  = 0;
  Truncated = FALSE;

  Status = KcsExchange (Request, RequestSize, Response, *ResponseSize,
                        &Received, &Truncated);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // The caller parses the frame itself, so hand back everything including the
  // NetFn/Cmd/completion-code header — but still insist the mandatory 3-byte
  // header arrived, or the caller would read uninitialised bytes.
  //
  if (Received < 3) {
    DEBUG ((DEBUG_ERROR, "IpmiKcs: short raw reply (%d bytes)\n", (UINT32)Received));
    return EFI_DEVICE_ERROR;
  }

  *ResponseSize = (UINT32)Received;
  return Truncated ? EFI_BUFFER_TOO_SMALL : EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
IpmiKcsCommand (
  IN     UINT8       NetFn,
  IN     UINT8       Command,
  IN     CONST VOID  *Request      OPTIONAL,
  IN     UINTN       RequestSize,
  OUT    UINT8       *CompletionCode,
  OUT    VOID        *Response     OPTIONAL,
  IN OUT UINTN       *ResponseSize OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT8       Frame[IPMI_KCS_MAX_PAYLOAD + 2];
  UINT8       Reply[IPMI_KCS_MAX_PAYLOAD + 3];
  UINTN       Received;
  UINTN       Capacity;
  BOOLEAN     Truncated;

  if ((CompletionCode == NULL) || (RequestSize > IPMI_KCS_MAX_PAYLOAD)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((RequestSize > 0) && (Request == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Capacity = ((Response != NULL) && (ResponseSize != NULL)) ? *ResponseSize : 0;

  //
  // Request frame is NetFn/LUN, Command, then data. The KCS interface carries
  // no checksums — those belong to IPMB, not to the system interface.
  //
  Frame[0] = (UINT8)(NetFn << 2);
  Frame[1] = Command;
  if (RequestSize > 0) {
    CopyMem (&Frame[2], Request, RequestSize);
  }

  Status = KcsExchange (Frame, RequestSize + 2, Reply, sizeof (Reply),
                        &Received, &Truncated);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "IpmiKcs: exchange failed for NetFn %02x Cmd %02x - %r\n",
      NetFn,
      Command,
      Status
      ));
    return Status;
  }

  //
  // Reply frame is NetFn/LUN, Command, completion code, then data.
  //
  if (Received < 3) {
    DEBUG ((DEBUG_ERROR, "IpmiKcs: short reply (%d bytes)\n", (UINT32)Received));
    return EFI_DEVICE_ERROR;
  }

  *CompletionCode = Reply[2];
  Received       -= 3;

  if (ResponseSize != NULL) {
    if (Received > Capacity) {
      *ResponseSize = Received;
      return EFI_BUFFER_TOO_SMALL;
    }

    if ((Received > 0) && (Response != NULL)) {
      CopyMem (Response, &Reply[3], Received);
    }

    *ResponseSize = Received;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
IpmiKcsProbe (
  OUT    VOID   *DeviceId     OPTIONAL,
  IN OUT UINTN  *DeviceIdSize OPTIONAL
  )
{
  EFI_STATUS  Status;
  UINT8       CompletionCode;

  Status = IpmiKcsCommand (
             IPMI_NETFN_APP,
             0x01,               // Get Device ID
             NULL,
             0,
             &CompletionCode,
             DeviceId,
             DeviceIdSize
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CompletionCode != 0x00) {
    DEBUG ((DEBUG_WARN, "IpmiKcs: Get Device ID returned cc=%02x\n", CompletionCode));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}
