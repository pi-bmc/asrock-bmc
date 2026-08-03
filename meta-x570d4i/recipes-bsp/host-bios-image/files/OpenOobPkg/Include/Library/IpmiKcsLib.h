/** @file
  Minimal IPMI transport over the host KCS (Keyboard Controller Style)
  interface, per IPMI v2.0 section 9.

  This exists instead of consuming the AMI gEfiIpmiTransportProtocol so that
  drivers injected into the stock ROM carry no dispatch dependency on AMI
  modules we intend to delete.

  Copyright (c) 2026, ASRock Rack X570D4I-2T OpenBMC port.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef OPEN_OOB_IPMI_KCS_LIB_H_
#define OPEN_OOB_IPMI_KCS_LIB_H_

#include <Uefi/UefiBaseType.h>

///
/// Largest IPMI request/response payload this library will carry. The KCS
/// interface itself imposes no limit, but phosphor-ipmi-blobs chunks writes at
/// 54 bytes and no command we issue needs more than a single 255-byte frame.
///
#define IPMI_KCS_MAX_PAYLOAD  255

///
/// Standard IPMI network function codes we use.
///
#define IPMI_NETFN_CHASSIS  0x00
#define IPMI_NETFN_SENSOR   0x04
#define IPMI_NETFN_APP      0x06
#define IPMI_NETFN_STORAGE  0x0A
#define IPMI_NETFN_OEM      0x2E

/**
  Execute one IPMI request/response transaction over the host KCS interface.

  The transaction is bracketed by a wait for the KCS state machine to return to
  IDLE, so this is safe to call while the platform firmware is issuing its own
  IPMI traffic — but it is NOT reentrant and must not be called from an
  interrupt or SMI context.

  @param[in]     NetFn        IPMI network function (see IPMI_NETFN_*).
  @param[in]     Command      IPMI command byte.
  @param[in]     Request      Request data, or NULL when RequestSize is 0.
  @param[in]     RequestSize  Length of Request in bytes.
  @param[out]    CompletionCode  Receives the IPMI completion code. Note that a
                                 non-zero completion code is reported here with
                                 EFI_SUCCESS as the return value: the transport
                                 worked, the BMC declined.
  @param[out]    Response     Buffer for response data after the completion
                              code, or NULL when ResponseSize is 0.
  @param[in,out] ResponseSize On input, capacity of Response. On output, bytes
                              actually written. May be NULL if Response is NULL.

  @retval EFI_SUCCESS            Transaction completed; check CompletionCode.
  @retval EFI_NOT_READY          KCS never reached IDLE; another agent owns it.
  @retval EFI_TIMEOUT            BMC did not advance the KCS handshake.
  @retval EFI_DEVICE_ERROR       KCS reported an unexpected state.
  @retval EFI_BUFFER_TOO_SMALL   Response did not fit; ResponseSize updated.
  @retval EFI_INVALID_PARAMETER  RequestSize exceeds IPMI_KCS_MAX_PAYLOAD.
**/
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
  );

/**
  Execute a pre-framed IPMI transaction over the host KCS interface.

  Lower level than IpmiKcsCommand(): the caller supplies and parses the whole
  frame. Exists because the SMBIOS-blob and BIOS-config drivers build their
  request bytes directly and want the raw reply including NetFn/Cmd/completion
  code, rather than having the library strip them.

  @param[in]     Request      Request bytes, starting with (NetFn << 2) then the
                              command byte. Must be at least 2 bytes.
  @param[in]     RequestSize  Length of Request.
  @param[out]    Response     Receives [NetFn|LUN][Cmd][CompletionCode][data...].
  @param[in,out] ResponseSize On entry the capacity of Response; on exit the
                              number of bytes actually received.

  @retval EFI_SUCCESS            A reply was read; inspect Response[2] for the
                                 IPMI completion code.
  @retval EFI_NOT_READY          KCS never went idle; the firmware holds it.
  @retval EFI_TIMEOUT            The BMC stopped advancing the handshake.
  @retval EFI_DEVICE_ERROR       KCS left the expected phase, or the reply was
                                 shorter than the mandatory 3-byte header.
  @retval EFI_BUFFER_TOO_SMALL   Response was too small; ResponseSize updated.
  @retval EFI_INVALID_PARAMETER  Request/Response NULL, or RequestSize < 2.
**/
EFI_STATUS
EFIAPI
IpmiKcsRawTransaction (
  IN     CONST UINT8  *Request,
  IN     UINT32       RequestSize,
  OUT    UINT8        *Response,
  IN OUT UINT32       *ResponseSize
  );

/**
  Write a byte to the platform POST code port.

  On this board the BMC snoops port 0x80, so this is the one progress channel
  that works with no display and no serial redirect. Lives here so the injected
  drivers share one definition instead of each owning a copy.

  @param[in] Value  The POST code to emit.
**/
VOID
EFIAPI
OobPostCode (
  IN UINT8  Value
  );

/**
  Probe for a responsive BMC by issuing Get Device ID (App / 0x01).

  Call this before doing real work: on this board the BMC can still be booting
  when the DXE phase starts, and a driver that assumes a live BMC will stall
  every boot.

  @param[out] DeviceId      Optional 15-byte Get Device ID response body.
  @param[in,out] DeviceIdSize  Capacity/length of DeviceId. May be NULL.

  @retval EFI_SUCCESS    The BMC answered Get Device ID.
  @retval other          The BMC is absent or not yet responsive.
**/
EFI_STATUS
EFIAPI
IpmiKcsProbe (
  OUT    VOID   *DeviceId     OPTIONAL,
  IN OUT UINTN  *DeviceIdSize OPTIONAL
  );

#endif // OPEN_OOB_IPMI_KCS_LIB_H_
