/** @file
  AMI's DXE IPMI transport protocol, 4A1D0E66-5271-4E22-83FE-90921B748213.

  Binary-compatible reimplementation of the protocol published by the stock
  DxeIpmiBmcInitialize (6372357A), so that module can be stripped while the
  consumers we keep continue to work.

  ---------------------------------------------------------------------------
  Provenance: this layout is RECOVERED FROM THE SHIPPING BINARY, not guessed
  ---------------------------------------------------------------------------
  Recovered 2026-08-03 from bank 2 (the live Vermeer bank) of X574I2T 2.59F,
  DxeIpmiBmcInitialize PE32, image base 0, .text at RVA 0x1000.

  The publisher allocates ONE private context and installs a pointer 0x120
  bytes into it as the protocol interface:

      0x22aa  mov  edx, 0x178              ; sizeof(context)
      0x22af  mov  ecx, 6                  ; EfiRuntimeServicesData
      0x22b4  call 0x3ea8                  ; AllocateZeroPool
      0x22b9  mov  [0x5778], rax           ; global context pointer
      ...
      0x2069  mov  r9,  [0x5778]
      0x2070  lea  rdx, [0x50a0]           ; &gEfiIpmiTransportProtocolGuid
      0x2082  add  r9,  0x120              ; Interface = context + 0x120
      0x208d  xor  r8d, r8d                ; EFI_NATIVE_INTERFACE
      0x2090  call [rax + 0x80]            ; gBS->InstallProtocolInterface

  (+0x80 is InstallProtocolInterface and +0x48 -- used on the failure path --
  is FreePool, which confirms the register really is gBS.)

  Because the context comes from AllocateZeroPool and nothing ever writes the
  first two QWORDs of the protocol, Revision and the field after it are both
  ZERO in the shipping firmware. We reproduce that exactly rather than invent a
  revision number: a consumer that gates on Revision would see a value the
  stock ROM never produced.

  Only three slots are ever populated, and the publisher writes them from two
  different code paths (one per transport type), which is what identifies them
  as the vtable:

      context+0x130 (protocol+0x10)  <- 0x2eb0  /  path 2: another
      context+0x138 (protocol+0x18)  <- path 2: 0x2a78
      context+0x140 (protocol+0x20)  <- 0x2f88  /  path 2: 0x2a2c

  Everything after that is private state, not protocol:
      +0x148 = 0x0CA2   KCS data port
      +0x14a = 0x0CA3   KCS command/status port
      +0x14c = 0xC350   (50000) timeout
      +0x108 = BMC state enum, +0x118 = retry counter, +0x170 = flag

  SendIpmiCommand sits at protocol+0x10. That is not inferred from the
  publisher -- it is proven by a consumer. SerialMuxControl (129F6AA7):

      0x363  lea  rcx, [0x6d0]            ; &gEfiIpmiTransportProtocolGuid
      0x36a  call [rax + 0x140]           ; gBS->LocateProtocol
      ...
      0x3cc  mov  rax, [0x730]            ; the located interface
      0x3d3  mov  rcx, rax                ; This
      0x3d6  call [rax + 0x10]            ; <-- SendIpmiCommand

  with rdx=6 (NetFn App), r8=0 (Lun), r9=0x42 (Get Channel Info),
  [rsp+0x20]=CommandData, [rsp+0x28]=1, [rsp+0x30]=ResponseData,
  [rsp+0x38]=&ResponseDataSize (pre-set to 10). That fixes the signature below.

  It also fixes the response convention. SerialMuxControl reads ResponseData[1],
  masks it with 0x7F and compares against 5. The 0x7F mask is exactly the IPMI
  v2.0 Channel Medium Type field (bits [6:0]) and 5 is "asynch serial/modem" --
  which is precisely what a serial-mux driver looks for. So ResponseData[1] is
  the medium type, ResponseData[0] is the channel number, and the COMPLETION
  CODE IS NOT IN THE BUFFER. That matches IpmiKcsCommand()'s Response, so no
  re-framing is needed.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef AMI_IPMI_TRANSPORT_H_
#define AMI_IPMI_TRANSPORT_H_

#include <Uefi/UefiBaseType.h>

#define AMI_DXE_IPMI_TRANSPORT_PROTOCOL_GUID \
  { 0x4a1d0e66, 0x5271, 0x4e22, { 0x83, 0xfe, 0x90, 0x92, 0x1b, 0x74, 0x82, 0x13 }}

typedef struct _AMI_IPMI_TRANSPORT AMI_IPMI_TRANSPORT;

///
/// Values the stock publisher stores at context+0x108. Reproduced so
/// GetBmcStatus can answer with something a consumer will recognise.
///
typedef enum {
  AmiBmcStatusOk         = 0,
  AmiBmcStatusUpdateMode = 1,
  AmiBmcStatusNoReply    = 2,
  AmiBmcStatusHardFail   = 3
} AMI_BMC_STATUS;

///
/// Second out-parameter of GetBmcStatus in AMI's ServerMgmt headers. The stock
/// context keeps the KCS port pair at +0x148/+0x14a, which is what this
/// describes; consumers on this board never inspect it.
///
typedef struct {
  UINT8     Type;
  UINT8     Address;
  UINT16    Port;
} AMI_SM_COM_ADDRESS;

/**
  Execute one IPMI transaction.

  @param[in]     This              Protocol instance.
  @param[in]     NetFunction       IPMI network function (NOT pre-shifted).
  @param[in]     Lun               Logical unit, 0 on this board.
  @param[in]     Command           IPMI command byte.
  @param[in]     CommandData       Request payload, or NULL.
  @param[in]     CommandDataSize   Request payload length.
  @param[out]    ResponseData      Response payload EXCLUDING the completion
                                   code, or NULL.
  @param[in,out] ResponseDataSize  On entry the capacity of ResponseData, on
                                   exit the number of bytes written.

  @retval EFI_SUCCESS       The BMC answered with completion code 0x00.
  @retval EFI_DEVICE_ERROR  Transport failure, or a non-zero completion code.
**/
typedef
EFI_STATUS
(EFIAPI *AMI_IPMI_SEND_COMMAND)(
  IN     AMI_IPMI_TRANSPORT  *This,
  IN     UINT8               NetFunction,
  IN     UINT8               Lun,
  IN     UINT8               Command,
  IN     UINT8               *CommandData,
  IN     UINT8               CommandDataSize,
  OUT    UINT8               *ResponseData,
  IN OUT UINT8               *ResponseDataSize
  );

/**
  Report BMC health.

  @param[in]  This        Protocol instance.
  @param[out] BmcStatus   Receives an AMI_BMC_STATUS value.
  @param[out] ComAddress  Receives the transport address descriptor.
**/
typedef
EFI_STATUS
(EFIAPI *AMI_IPMI_GET_BMC_STATUS)(
  IN  AMI_IPMI_TRANSPORT  *This,
  OUT AMI_BMC_STATUS      *BmcStatus,
  OUT AMI_SM_COM_ADDRESS  *ComAddress
  );

/**
  Extended send. The stock implementation at 0x2f88 takes one more argument
  than SendIpmiCommand -- it reads a ninth slot at [rsp+0xa0] after its
  `sub rsp, 0x58` prologue, where SendIpmiCommand stops at eight. AMI uses the
  extra argument to select the transport interface explicitly instead of using
  the context's default.

  No module we retain calls this; it is implemented so the vtable is not short,
  and it simply ignores the selector and forwards to the KCS path.
**/
typedef
EFI_STATUS
(EFIAPI *AMI_IPMI_SEND_COMMAND_EX)(
  IN     AMI_IPMI_TRANSPORT  *This,
  IN     UINT8               NetFunction,
  IN     UINT8               Lun,
  IN     UINT8               Command,
  IN     UINT8               *CommandData,
  IN     UINT8               CommandDataSize,
  OUT    UINT8               *ResponseData,
  IN OUT UINT8               *ResponseDataSize,
  IN     UINT8               InterfaceType
  );

///
/// Offsets are load-bearing -- see the provenance note above. Revision must be
/// the first QWORD and SendIpmiCommand must land at +0x10.
///
struct _AMI_IPMI_TRANSPORT {
  UINT64                      Revision;             ///< +0x00, zero in stock
  UINT64                      Reserved;             ///< +0x08, zero in stock
  AMI_IPMI_SEND_COMMAND       SendIpmiCommand;      ///< +0x10  VERIFIED
  AMI_IPMI_GET_BMC_STATUS     GetBmcStatus;         ///< +0x18
  AMI_IPMI_SEND_COMMAND_EX    SendIpmiCommandEx;    ///< +0x20
};

extern EFI_GUID  gAmiDxeIpmiTransportProtocolGuid;

#endif // AMI_IPMI_TRANSPORT_H_
