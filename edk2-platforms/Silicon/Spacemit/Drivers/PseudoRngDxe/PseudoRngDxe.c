/** @file

  This driver produces an EFI_RNG_PROTOCOL instance using a pseudorandom
  number generator based on performance counter.

  Copyright (C) 2025, Spacemit Ltd. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/TimerLib.h>
#include <Protocol/Rng.h>

#define SHIFT1  29
#define MASK1   0x5555555555555555ULL
#define SHIFT2  17
#define MASK2   0x71d67fffeda60000ULL
#define SHIFT3  37
#define MASK3   0xfff7eee000000000ULL
#define SHIFT4  43

/**
  Generate a pseudorandom number.

  @retval  A pseudorandom number based on performance counter.

**/
STATIC
UINTN
Rand (
  VOID
  )
{
  UINTN  Y;

  Y = GetPerformanceCounter ();

  Y ^= (Y >> SHIFT1) & MASK1;
  Y ^= (Y << SHIFT2) & MASK2;
  Y ^= (Y << SHIFT3) & MASK3;
  Y ^= Y >> SHIFT4;

  return Y;
}

/**
  Fill a buffer with pseudorandom data.

  @param[in]  Length  The length in bytes of the buffer to fill.
  @param[out] Bits    Pointer to the buffer to fill with random data.

  @retval EFI_SUCCESS  The buffer was filled successfully.

**/
STATIC
EFI_STATUS
GetTrngData (
  IN    UINTN  Length,
  OUT   UINT8  *Bits
  )
{
  UINTN  Index;
  UINTN  Value;
  UINTN  Bytes;

  for (Index = 0; Index < Length; Index += sizeof (Value)) {
    Value = Rand ();
    Bytes = MIN (sizeof (Value), Length - Index);
    CopyMem (Bits, &Value, Bytes);
  }

  return EFI_SUCCESS;
}

/**
  Returns information about the random number generation implementation.

  @param[in]     This                 A pointer to the EFI_RNG_PROTOCOL
                                      instance.
  @param[in,out] RNGAlgorithmListSize On input, the size in bytes of
                                      RNGAlgorithmList.
                                      On output with a return code of
                                      EFI_SUCCESS, the size in bytes of the
                                      data returned in RNGAlgorithmList. On
                                      output with a return code of
                                      EFI_BUFFER_TOO_SMALL, the size of
                                      RNGAlgorithmList required to obtain the
                                      list.
  @param[out] RNGAlgorithmList        A caller-allocated memory buffer filled
                                      by the driver with one EFI_RNG_ALGORITHM
                                      element for each supported RNG algorithm.
                                      The list must not change across multiple
                                      calls to the same driver. The first
                                      algorithm in the list is the default
                                      algorithm for the driver.

  @retval EFI_SUCCESS                 The RNG algorithm list was returned
                                      successfully.
  @retval EFI_UNSUPPORTED             The services is not supported by this
                                      driver.
  @retval EFI_DEVICE_ERROR            The list of algorithms could not be
                                      retrieved due to a hardware or firmware
                                      error.
  @retval EFI_INVALID_PARAMETER       One or more of the parameters are
                                      incorrect.
  @retval EFI_BUFFER_TOO_SMALL        The buffer RNGAlgorithmList is too small
                                      to hold the result.

**/
STATIC
EFI_STATUS
EFIAPI
PseudoRngGetInfo (
  IN      EFI_RNG_PROTOCOL   *This,
  IN OUT  UINTN              *RNGAlgorithmListSize,
  OUT     EFI_RNG_ALGORITHM  *RNGAlgorithmList
  )
{
  if ((This == NULL) || (RNGAlgorithmListSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (*RNGAlgorithmListSize < sizeof (EFI_RNG_ALGORITHM)) {
    *RNGAlgorithmListSize = sizeof (EFI_RNG_ALGORITHM);
    return EFI_BUFFER_TOO_SMALL;
  }

  if (RNGAlgorithmList == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *RNGAlgorithmListSize = sizeof (EFI_RNG_ALGORITHM);
  CopyGuid (RNGAlgorithmList, &gEfiRngAlgorithmRaw);

  return EFI_SUCCESS;
}

/**
  Produces and returns an RNG value using either the default or specified RNG
  algorithm.

  @param[in]  This                    A pointer to the EFI_RNG_PROTOCOL
                                      instance.
  @param[in]  RNGAlgorithm            A pointer to the EFI_RNG_ALGORITHM that
                                      identifies the RNG algorithm to use. May
                                      be NULL in which case the function will
                                      use its default RNG algorithm.
  @param[in]  RNGValueLength          The length in bytes of the memory buffer
                                      pointed to by RNGValue. The driver shall
                                      return exactly this numbers of bytes.
  @param[out] RNGValue                A caller-allocated memory buffer filled
                                      by the driver with the resulting RNG
                                      value.

  @retval EFI_SUCCESS                 The RNG value was returned successfully.
  @retval EFI_UNSUPPORTED             The algorithm specified by RNGAlgorithm
                                      is not supported by this driver.
  @retval EFI_DEVICE_ERROR            An RNG value could not be retrieved due
                                      to a hardware or firmware error.
  @retval EFI_NOT_READY               There is not enough random data available
                                      to satisfy the length requested by
                                      RNGValueLength.
  @retval EFI_INVALID_PARAMETER       RNGValue is NULL or RNGValueLength is
                                      zero.

**/
STATIC
EFI_STATUS
EFIAPI
PseudoRngGetRNG (
  IN  EFI_RNG_PROTOCOL  *This,
  IN  EFI_RNG_ALGORITHM *RNGAlgorithm OPTIONAL,
  IN  UINTN             RNGValueLength,
  OUT UINT8             *RNGValue
  )
{
  EFI_STATUS  Status;

  if ((This == NULL) || (RNGValueLength == 0) || (RNGValue == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // We only support the raw algorithm, so reject requests for anything else
  //
  if ((RNGAlgorithm != NULL) &&
      !CompareGuid (RNGAlgorithm, &gEfiRngAlgorithmRaw)) {
    return EFI_UNSUPPORTED;
  }

  Status = GetTrngData (RNGValueLength, RNGValue);

  return Status;
}

///
/// The EFI_RNG_PROTOCOL instance produced by this driver.
///
STATIC EFI_RNG_PROTOCOL  mRngProtocol = {
  PseudoRngGetInfo,
  PseudoRngGetRNG
};

/**
  The entry point of the PseudoRng driver.

  @param[in] ImageHandle  The image handle of the driver.
  @param[in] SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS     The driver was initialized successfully.
  @retval Others          Failed to install the RNG protocol.

**/
EFI_STATUS
EFIAPI
RngDxeEntryPoint (
  IN  EFI_HANDLE        ImageHandle,
  IN  EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return gBS->InstallMultipleProtocolInterfaces (
                &ImageHandle,
                &gEfiRngProtocolGuid,
                &mRngProtocol,
                NULL
                );
}
