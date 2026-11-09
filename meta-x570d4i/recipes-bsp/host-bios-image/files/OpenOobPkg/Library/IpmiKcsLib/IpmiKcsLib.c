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
#define KCS_TIMEOUT_US  FixedPcdGet32 (PcdIpmiKcsTimeoutUs)

//
// How long a busy interface gets to go idle on its own before it is forced
// there with an abort, and how many abort attempts to make.
//
#define KCS_IDLE_GRACE_US  100000
#define KCS_ABORT_RETRIES  3

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

//
// Time base.
//
// Every wait below is bounded in real time, as Linux's KCS driver bounds its own
// (5 s). A spin count is not a time: each poll is one LPC status read, whose
// cost varies with the bus and the BMC, and the old 1M-poll bound came out at
// roughly 1-2 s here -- under a slow-but-valid ipmid reply.
//
// The clock is the HPET main counter. It is memory-mapped at a fixed address,
// states its own tick period, and needs no boot service -- this library runs
// inside AMI's DXE core, at raised TPL, before arch protocols such as the
// metronome are necessarily up. Only the low 32 bits are read; unsigned
// subtraction absorbs one wrap, and at the FCH's 14.318 MHz one wrap is ~300 s.
// Without a usable HPET (absent, stopped), polls are counted instead at a
// conservative 1 us each -- slower than any LPC read.
//
#define HPET_BASE           FixedPcdGet32 (PcdOobHpetBaseAddress)
#define HPET_CAP_ID         0x000   // revision, vendor id, counter size
#define HPET_CAP_PERIOD     0x004   // main counter tick period, femtoseconds
#define HPET_CONFIG         0x010   // bit 0 = ENABLE_CNF
#define HPET_MAIN_COUNTER   0x0F0
#define HPET_MAX_PERIOD_FS  100000000   // the spec's ceiling: 100 ns per tick

typedef struct {
  BOOLEAN  Hpet;
  UINT32   Start;
  UINT32   Limit;      // HPET ticks, or polls without an HPET
  UINT32   Polls;
} KCS_DEADLINE;

//
// Tick period of a verified-running HPET, 0 until one has been seen. Only
// success is cached, so a counter enabled later in DXE is still picked up.
//
STATIC UINT32  mHpetPeriodFs = 0;

STATIC
BOOLEAN
KcsHpetUsable (
  VOID
  )
{
  UINT32  Period;
  UINT32  First;
  UINTN   Index;

  if (mHpetPeriodFs != 0) {
    return TRUE;
  }

  Period = MmioRead32 (HPET_BASE + HPET_CAP_PERIOD);
  if ((MmioRead32 (HPET_BASE + HPET_CAP_ID) == 0xFFFFFFFF) ||
      (Period == 0) || (Period > HPET_MAX_PERIOD_FS) ||
      ((MmioRead32 (HPET_BASE + HPET_CONFIG) & BIT0) == 0))
  {
    return FALSE;
  }

  //
  // Enabled is not the same as counting: insist on seeing it move.
  //
  First = MmioRead32 (HPET_BASE + HPET_MAIN_COUNTER);
  for (Index = 0; Index < 1000; Index++) {
    if (MmioRead32 (HPET_BASE + HPET_MAIN_COUNTER) != First) {
      mHpetPeriodFs = Period;
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
KcsDeadlineStart (
  OUT KCS_DEADLINE  *Deadline,
  IN  UINT32        Microseconds
  )
{
  Deadline->Polls = 0;
  if (KcsHpetUsable ()) {
    Deadline->Hpet  = TRUE;
    Deadline->Limit = (UINT32)DivU64x32 (
                                MultU64x32 (Microseconds, 1000000000),
                                mHpetPeriodFs
                                );
    Deadline->Start = MmioRead32 (HPET_BASE + HPET_MAIN_COUNTER);
  } else {
    Deadline->Hpet  = FALSE;
    Deadline->Start = 0;
    Deadline->Limit = Microseconds;
  }
}

STATIC
BOOLEAN
KcsDeadlineExpired (
  IN OUT KCS_DEADLINE  *Deadline
  )
{
  if (Deadline->Hpet) {
    return (BOOLEAN)(
      (UINT32)(MmioRead32 (HPET_BASE + HPET_MAIN_COUNTER) - Deadline->Start)
      >= Deadline->Limit);
  }

  return (BOOLEAN)(++Deadline->Polls > Deadline->Limit);
}

/**
  Wait until the Input Buffer Full flag clears, i.e. the BMC has consumed the
  byte we wrote.
**/
STATIC
EFI_STATUS
KcsWaitInputEmpty (
  VOID
  )
{
  KCS_DEADLINE  Deadline;

  KcsDeadlineStart (&Deadline, KCS_TIMEOUT_US);
  do {
    if ((IoRead8 (KCS_CMD_PORT) & KCS_STATUS_IBF) == 0) {
      return EFI_SUCCESS;
    }
  } while (!KcsDeadlineExpired (&Deadline));

  return EFI_TIMEOUT;
}

/**
  Wait until the Output Buffer Full flag sets, i.e. the BMC has a byte for us.
**/
STATIC
EFI_STATUS
KcsWaitOutputFull (
  VOID
  )
{
  KCS_DEADLINE  Deadline;

  KcsDeadlineStart (&Deadline, KCS_TIMEOUT_US);
  do {
    if ((IoRead8 (KCS_CMD_PORT) & KCS_STATUS_OBF) != 0) {
      return EFI_SUCCESS;
    }
  } while (!KcsDeadlineExpired (&Deadline));

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
  Return the interface to IDLE_STATE with the IPMI v2.0 section 9.15 error-exit
  ("GET_STATUS/ABORT") sequence.

  Nothing else recovers KCS once a transaction is abandoned part-way. The case
  that matters: a request is written, the BMC is slow to answer, the host gives
  up. The BMC then posts its late reply, which leaves the interface in
  READ_STATE with a byte waiting -- and every later transaction, ours and the
  platform firmware's (OobIpmiDxe carries AMI's IPMI too), finds it never idle.
  On this board that silently lost the BIOS's boot-option read, so the boot
  override was ignored, and could hang POST outright, until Linux's ipmi_si
  reset the interface at OS boot. The AST2500 kcs_bmc driver implements the BMC
  half of this sequence and discards the stale reply.

  @retval EFI_SUCCESS       The interface is idle again.
  @retval EFI_NOT_READY     Nothing is driving the interface (it reads 0xFF).
  @retval EFI_DEVICE_ERROR  The BMC did not follow the sequence.
**/
STATIC
EFI_STATUS
KcsAbort (
  VOID
  )
{
  UINTN  Attempt;
  UINT8  ErrorCode;

  //
  // A floating bus reads 0xFF, and a BMC that never drains the input buffer is
  // not running its KCS driver at all. Neither can follow the sequence, so fail
  // fast instead of stacking timeouts onto every attempt while it boots.
  //
  if (IoRead8 (KCS_CMD_PORT) == 0xFF) {
    return EFI_NOT_READY;
  }

  for (Attempt = 0; Attempt < KCS_ABORT_RETRIES; Attempt++) {
    if (EFI_ERROR (KcsWaitInputEmpty ())) {
      break;
    }

    IoWrite8 (KCS_CMD_PORT, KCS_CTL_GET_STATUS);
    if (EFI_ERROR (KcsWaitInputEmpty ()) ||
        (KCS_STATE (IoRead8 (KCS_CMD_PORT)) != KCS_STATE_WRITE))
    {
      continue;
    }

    KcsFlushOutput ();
    IoWrite8 (KCS_DATA_PORT, 0x00);
    if (EFI_ERROR (KcsWaitInputEmpty ()) ||
        (KCS_STATE (IoRead8 (KCS_CMD_PORT)) != KCS_STATE_READ) ||
        EFI_ERROR (KcsWaitOutputFull ()))
    {
      continue;
    }

    ErrorCode = IoRead8 (KCS_DATA_PORT);
    IoWrite8 (KCS_DATA_PORT, KCS_CTL_READ);
    if (EFI_ERROR (KcsWaitInputEmpty ()) ||
        (KCS_STATE (IoRead8 (KCS_CMD_PORT)) != KCS_STATE_IDLE))
    {
      continue;
    }

    //
    // IDLE carries one final dummy byte; consume it so OBF is clear.
    //
    if (!EFI_ERROR (KcsWaitOutputFull ())) {
      IoRead8 (KCS_DATA_PORT);
    }

    DEBUG ((DEBUG_WARN, "IpmiKcs: interface reset (BMC status %02x)\n", ErrorCode));
    (VOID)ErrorCode;  // only DEBUG reads it; keep release builds warning-free
    return EFI_SUCCESS;
  }

  DEBUG ((DEBUG_ERROR, "IpmiKcs: abort sequence failed\n"));
  return EFI_DEVICE_ERROR;
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
  KCS_DEADLINE  Deadline;
  UINT8         Status;

  KcsDeadlineStart (&Deadline, KCS_IDLE_GRACE_US);
  do {
    Status = IoRead8 (KCS_CMD_PORT);
    if (((Status & KCS_STATUS_IBF) == 0) && (KCS_STATE (Status) == KCS_STATE_IDLE)) {
      KcsFlushOutput ();
      return EFI_SUCCESS;
    }
  } while (!KcsDeadlineExpired (&Deadline));

  //
  // Still not idle. Waiting longer cannot help: either an earlier exchange was
  // abandoned and the BMC's late reply is parked in READ_STATE, or another
  // client we pre-empted holds it mid-transfer and cannot run until we return.
  // Force it idle; a client we interrupted sees an error and retries.
  //
  if (!EFI_ERROR (KcsAbort ())) {
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
  EFI_STATUS    Status;
  KCS_DEADLINE  Deadline;
  BOOLEAN       Ready;
  UINT8         StatusReg;
  UINT8         Value;

  *Received  = 0;
  *Truncated = FALSE;

  while (TRUE) {
    //
    // The first pass of this wait is the BMC executing the command; later
    // passes are one byte each. Both get the full timeout, as in Linux.
    //
    Ready = FALSE;
    KcsDeadlineStart (&Deadline, KCS_TIMEOUT_US);
    do {
      StatusReg = IoRead8 (KCS_CMD_PORT);
      if ((KCS_STATE (StatusReg) == KCS_STATE_READ) ||
          (KCS_STATE (StatusReg) == KCS_STATE_IDLE))
      {
        Ready = TRUE;
        break;
      }

      if (KCS_STATE (StatusReg) == KCS_STATE_ERROR) {
        DEBUG ((DEBUG_ERROR, "IpmiKcs: BMC signalled ERROR_STATE\n"));
        return EFI_DEVICE_ERROR;
      }
    } while (!KcsDeadlineExpired (&Deadline));

    if (!Ready) {
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
    //
    // Never leave a half-finished exchange behind: the next caller -- possibly
    // the platform firmware's own IPMI stack -- would find the interface stuck.
    //
    KcsAbort ();
    return Status;
  }

  Status = KcsReadPhase (Reply, ReplyCapacity, Received, Truncated);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "IpmiKcs: read phase failed - %r\n", Status));
    KcsAbort ();
    return Status;
  }

  //
  // Only accept a reply that answers this request: NetFn + 1 with the same LUN,
  // and the same command. KCS carries no sequence number, so a reply the BMC
  // posted late for an earlier, abandoned exchange would otherwise be read as
  // the answer to this one -- and every reply after it would be off by one.
  // The exchange itself completed cleanly, so the interface is idle; the
  // caller just has to ask again.
  //
  if ((MIN (*Received, ReplyCapacity) < 2) ||
      (Reply[0] != (UINT8)((((Frame[0] >> 2) + 1) << 2) | (Frame[0] & 0x03))) ||
      (Reply[1] != Frame[1]))
  {
    DEBUG ((
      DEBUG_ERROR,
      "IpmiKcs: reply %02x/%02x does not answer request %02x/%02x\n",
      (MIN (*Received, ReplyCapacity) > 0) ? Reply[0] : 0,
      (MIN (*Received, ReplyCapacity) > 1) ? Reply[1] : 0,
      Frame[0],
      Frame[1]
      ));
    return EFI_PROTOCOL_ERROR;
  }

  return EFI_SUCCESS;
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
