/** @file
  OpenOob IPMI transport protocol.

  A thin, serialising wrapper around IpmiKcsLib so several injected drivers can
  share one KCS channel. Intentionally NOT source- or binary-compatible with
  AMI's gEfiIpmiTransportProtocol: nothing that consumes this should be able to
  fall back to the proprietary stack by accident.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPEN_OOB_IPMI_TRANSPORT_H_
#define OPEN_OOB_IPMI_TRANSPORT_H_

#define OPEN_OOB_IPMI_TRANSPORT_PROTOCOL_GUID \
  { 0x2c9e7a41, 0x6b3d, 0x4f52, { 0x8e, 0x17, 0xd4, 0x0b, 0x93, 0x62, 0xa5, 0x18 }}

typedef struct _OPEN_OOB_IPMI_TRANSPORT_PROTOCOL OPEN_OOB_IPMI_TRANSPORT_PROTOCOL;

/**
  Execute an IPMI command. Semantics match IpmiKcsCommand(), plus mutual
  exclusion against other consumers of this protocol.
**/
typedef
EFI_STATUS
(EFIAPI *OPEN_OOB_IPMI_EXECUTE)(
  IN     OPEN_OOB_IPMI_TRANSPORT_PROTOCOL  *This,
  IN     UINT8                             NetFn,
  IN     UINT8                             Command,
  IN     CONST VOID                        *Request      OPTIONAL,
  IN     UINTN                             RequestSize,
  OUT    UINT8                             *CompletionCode,
  OUT    VOID                              *Response     OPTIONAL,
  IN OUT UINTN                             *ResponseSize OPTIONAL
  );

struct _OPEN_OOB_IPMI_TRANSPORT_PROTOCOL {
  UINT32                   Revision;
  ///
  /// TRUE once the BMC has answered Get Device ID at least once. Consumers
  /// should skip optional work rather than retry when this is FALSE.
  ///
  BOOLEAN                  BmcResponsive;
  ///
  /// Get Device ID response body, valid when BmcResponsive is TRUE.
  ///
  UINT8                    DeviceId[16];
  UINT8                    DeviceIdSize;
  OPEN_OOB_IPMI_EXECUTE    Execute;
};

#define OPEN_OOB_IPMI_TRANSPORT_REVISION  0x00010000

extern EFI_GUID  gOpenOobIpmiTransportProtocolGuid;

#endif // OPEN_OOB_IPMI_TRANSPORT_H_
