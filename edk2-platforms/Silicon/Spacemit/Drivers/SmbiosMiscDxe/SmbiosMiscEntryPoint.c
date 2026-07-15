/** @file
  This driver parses the mSmbiosMiscDataTable structure and reports
  any generated data using SMBIOS protocol.

  Based on files under Nt32Pkg/MiscSubClassPlatformDxe/

  Copyright (c) 2024, SpacemiT Co., Ltd. All rights reserved.
  Copyright (c) 2021, NUVIA Inc. All rights reserved.<BR>
  Copyright (c) 2006 - 2011, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2015, Hisilicon Limited. All rights reserved.<BR>
  Copyright (c) 2015, Linaro Limited. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HiiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/PlatformInfo.h>
#include <Guid/EventGroup.h>

#include "SmbiosMisc.h"

STATIC EFI_HANDLE           mSmbiosMiscImageHandle;
STATIC EFI_SMBIOS_PROTOCOL  *mSmbiosMiscSmbios = NULL;

EFI_HII_HANDLE  mSmbiosMiscHiiHandle = NULL;

//
// Cached PlatformInfo protocol (backed by the TLV EEPROM). The protocol is
// located either eagerly in the entry point (when PlatformInfoDxe has already
// dispatched) or from the protocol-notify callback below; SmbiosMiscGetPlatform-
// InfoString() consumes it for the Type 1/2/3 board-identity strings.
//
STATIC PLATFORM_INFO_PROTOCOL  *mPlatformInfo = NULL;

//
// SMBIOS tables are built exactly once, either as soon as
// gSpacemitPlatformInfoProtocolGuid becomes available (so the Type 1/2/3
// strings carry real per-unit data from the TLV EEPROM) or, as a fallback for
// boards where that protocol is never installed, at ReadyToBoot (built from
// the PCD/HII defaults). This avoids a hard [Depex] on the platform-info
// protocol, which would suppress all SMBIOS tables (including Type 0) on
// boards without the TLV/EEPROM chain, while still defeating the dispatch-
// ordering race that would otherwise see SmbiosMiscDxe run before
// PlatformInfoDxe and read unpopulated TLV data.
//
STATIC EFI_EVENT  mPlatformInfoEvent = NULL;
STATIC EFI_EVENT  mReadyToBootEvent  = NULL;
STATIC VOID       *mPlatformInfoReg  = NULL;
STATIC BOOLEAN    mSmbiosTablesBuilt = FALSE;

/**
  Try to read a board-identifying string from the SpacemiT PlatformInfo
  protocol (backed by the TLV EEPROM on i2c2). The value is returned as UCS-2
  so it can be fed directly to HiiSetString().

  The protocol is located lazily and cached on success. If it is absent (e.g.
  no EEPROM/TLV driver), the function returns EFI_NOT_FOUND so the caller can
  fall back to its PCD/HII default.

  @param[in]  FieldName  PlatformInfo field name, e.g. "manufacturer".
  @param[out] Out        Caller-allocated UCS-2 buffer.
  @param[in]  OutChars   Capacity of Out in CHAR16 units (incl. NUL terminator).

  @retval EFI_SUCCESS    A non-empty value was retrieved and copied to Out.
  @retval EFI_NOT_FOUND  Protocol absent, field absent, or value empty.
  @retval EFI_INVALID_PARAMETER  FieldName/Out is NULL or OutChars is 0.
**/
EFI_STATUS
SmbiosMiscGetPlatformInfoString (
  IN  CONST CHAR8  *FieldName,
  OUT CHAR16       *Out,
  IN  UINTN        OutChars
  )
{
  EFI_STATUS  Status;
  CHAR8       AsciiBuf[SMBIOS_STRING_MAX_LENGTH];

  if ((FieldName == NULL) || (Out == NULL) || (OutChars == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Out[0] = L'\0';

  if (mPlatformInfo == NULL) {
    Status = gBS->LocateProtocol (
                    &gSpacemitPlatformInfoProtocolGuid,
                    NULL,
                    (VOID **)&mPlatformInfo
                    );
    if (EFI_ERROR (Status)) {
      // Not available yet (or not built at all); retry on the next call.
      mPlatformInfo = NULL;
      return EFI_NOT_FOUND;
    }
  }

  Status = mPlatformInfo->GetPlatformInfo (
                            mPlatformInfo,
                            (CHAR8 *)FieldName,
                            AsciiBuf,
                            sizeof (AsciiBuf)
                            );
  if (EFI_ERROR (Status) || (AsciiBuf[0] == '\0')) {
    return EFI_NOT_FOUND;
  }

  AsciiStrToUnicodeStrS (AsciiBuf, Out, OutChars);
  return EFI_SUCCESS;
}

/**
  Walk mSmbiosMiscDataTable and add every record through the SMBIOS protocol.

  Runs exactly once: either eagerly from the entry point (when the platform-
  info protocol is already available) or, deferred, from a notify callback
  (see SmbiosMiscOnPlatformInfoReady / SmbiosMiscOnReadyToBoot).
**/
STATIC
VOID
SmbiosMiscBuildAllTables (
  VOID
  )
{
  UINTN       Index;
  EFI_STATUS  EfiStatus;

  if (mSmbiosTablesBuilt) {
    return;
  }

  mSmbiosTablesBuilt = TRUE;

  for (Index = 0; Index < mSmbiosMiscDataTableEntries; ++Index) {
    //
    // If the entry have a function pointer, just log the data.
    //
    if (mSmbiosMiscDataTable[Index].Function != NULL) {
      EfiStatus = (*mSmbiosMiscDataTable[Index].Function) (
                            mSmbiosMiscDataTable[Index].RecordData,
                            mSmbiosMiscSmbios
                            );

      if (EFI_ERROR (EfiStatus)) {
        DEBUG ((
          DEBUG_ERROR,
          "Misc smbios store error.  Index=%d,"
          "ReturnStatus=%r\n",
          Index,
          EfiStatus
          ));
      }
    }
  }
}

/**
  Notify callback for gSpacemitPlatformInfoProtocolGuid: the TLV-backed
  platform-info protocol has just been installed, so cache it and build the
  SMBIOS tables now (Type 1/2/3 strings will pick up real per-unit data).
**/
STATIC
VOID
EFIAPI
SmbiosMiscOnPlatformInfoReady (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  Status = gBS->LocateProtocol (
                  &gSpacemitPlatformInfoProtocolGuid,
                  NULL,
                  (VOID **)&mPlatformInfo
                  );
  if (!EFI_ERROR (Status)) {
    SmbiosMiscBuildAllTables ();
  }
}

/**
  Notify callback for the ReadyToBoot event group: last-resort fallback so that
  boards where gSpacemitPlatformInfoProtocolGuid is never installed (no
  TLV/EEPROM chain) still get SMBIOS tables, built from PCD/HII defaults.
**/
STATIC
VOID
EFIAPI
SmbiosMiscOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  SmbiosMiscBuildAllTables ();
}

/**
  Standard EFI driver point.  This driver parses the mSmbiosMiscDataTable
  structure and reports any generated data using SMBIOS protocol.

  @param  ImageHandle     Handle for the image of this driver
  @param  SystemTable     Pointer to the EFI System Table

  @retval  EFI_SUCCESS    The data was successfully stored.

**/
EFI_STATUS
EFIAPI
SmbiosMiscEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  mSmbiosMiscImageHandle = ImageHandle;

  Status = gBS->LocateProtocol (
                  &gEfiSmbiosProtocolGuid,
                  NULL,
                  (VOID **)&mSmbiosMiscSmbios
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Could not locate SMBIOS protocol.  %r\n", Status));
    return Status;
  }

  mSmbiosMiscHiiHandle = HiiAddPackages (
                            &gEfiCallerIdGuid,
                            mSmbiosMiscImageHandle,
                            SmbiosMiscDxeStrings,
                            NULL
                            );
  if (mSmbiosMiscHiiHandle == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  //
  // The Type 1/2/3 board-identity strings are sourced from the TLV EEPROM via
  // gSpacemitPlatformInfoProtocolGuid, produced by PlatformInfoDxe.  The DXE
  // dispatcher may run PlatformInfoDxe either before or after this driver
  // (SmbiosMiscDxe has no [Depex] on it - see the note above on
  // mSmbiosTablesBuilt), so:
  //   - if the protocol is already available, build the tables now;
  //   - otherwise defer to a protocol notify, building as soon as it appears;
  //   - and register a ReadyToBoot fallback so boards without the TLV/EEPROM
  //     chain still get SMBIOS tables (built from PCD/HII defaults).
  //
  Status = gBS->LocateProtocol (
                  &gSpacemitPlatformInfoProtocolGuid,
                  NULL,
                  (VOID **)&mPlatformInfo
                  );
  if (!EFI_ERROR (Status)) {
    SmbiosMiscBuildAllTables ();
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  SmbiosMiscOnPlatformInfoReady,
                  NULL,
                  &mPlatformInfoEvent
                  );
  if (EFI_ERROR (Status)) {
    SmbiosMiscBuildAllTables ();
    return EFI_SUCCESS;
  }

  Status = gBS->RegisterProtocolNotify (
                  &gSpacemitPlatformInfoProtocolGuid,
                  mPlatformInfoEvent,
                  &mPlatformInfoReg
                  );
  if (EFI_ERROR (Status)) {
    SmbiosMiscBuildAllTables ();
    return EFI_SUCCESS;
  }

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  SmbiosMiscOnReadyToBoot,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &mReadyToBootEvent
                  );
  if (EFI_ERROR (Status)) {
    //
    // No fallback event: build now (best-effort) so the tables at least exist.
    //
    SmbiosMiscBuildAllTables ();
  }

  return EFI_SUCCESS;
}

/**
  Adds an SMBIOS record.

  @param  Buffer        The data for the SMBIOS record.
                        The format of the record is determined by
                        EFI_SMBIOS_TABLE_HEADER.Type. The size of the
                        formatted area is defined by EFI_SMBIOS_TABLE_HEADER.Length
                        and either followed by a double-null (0x0000) or a set
                        of null terminated strings and a null.
  @param  SmbiosHandle  A unique handle will be assigned to the SMBIOS record
                        if not NULL.

  @retval EFI_SUCCESS           Record was added.
  @retval EFI_OUT_OF_RESOURCES  Record was not added due to lack of system resources.
  @retval EFI_ALREADY_STARTED   The SmbiosHandle passed in was already in use.

**/
EFI_STATUS
SmbiosMiscAddRecord (
  IN  UINT8                 *Buffer,
  IN OUT EFI_SMBIOS_HANDLE  *SmbiosHandle OPTIONAL
  )
{
  EFI_STATUS         Status;
  EFI_SMBIOS_HANDLE  Handle;

  Handle = SMBIOS_HANDLE_PI_RESERVED;

  if (SmbiosHandle != NULL) {
    Handle = *SmbiosHandle;
  }

  Status = mSmbiosMiscSmbios->Add (
                                mSmbiosMiscSmbios,
                                NULL,
                                &Handle,
                                (EFI_SMBIOS_TABLE_HEADER *)Buffer
                                );

  if (SmbiosHandle != NULL) {
    *SmbiosHandle = Handle;
  }

  return Status;
}

/** Fetches the number of handles of the specified SMBIOS type.
 *
 *  @param SmbiosType The type of SMBIOS record to look for.
 *
 *  @return The number of handles
 *
**/
STATIC
UINTN
GetHandleCount (
  IN  UINT8  SmbiosType
  )
{
  UINTN                    HandleCount;
  EFI_STATUS               Status;
  EFI_SMBIOS_HANDLE        SmbiosHandle;
  EFI_SMBIOS_TABLE_HEADER  *Record;

  HandleCount = 0;

  // Iterate through entries to get the number
  do {
    Status = mSmbiosMiscSmbios->GetNext (
                                  mSmbiosMiscSmbios,
                                  &SmbiosHandle,
                                  &SmbiosType,
                                  &Record,
                                  NULL
                                  );

    if (Status == EFI_SUCCESS) {
      HandleCount++;
    }
  } while (!EFI_ERROR (Status));

  return HandleCount;
}

/**
  Fetches a list of the specified SMBIOS table types.

  @param[in]  SmbiosType    The type of table to fetch
  @param[out] **HandleArray The array of handles
  @param[out] *HandleCount  Number of handles in the array
**/
VOID
SmbiosMiscGetLinkTypeHandle (
  IN  UINT8          SmbiosType,
  OUT SMBIOS_HANDLE  **HandleArray,
  OUT UINTN          *HandleCount
  )
{
  UINTN                    Index;
  EFI_STATUS               Status;
  EFI_SMBIOS_HANDLE        SmbiosHandle;
  EFI_SMBIOS_TABLE_HEADER  *Record;

  if (mSmbiosMiscSmbios == NULL) {
    return;
  }

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  *HandleCount = GetHandleCount (SmbiosType);

  *HandleArray = AllocateZeroPool (sizeof (SMBIOS_HANDLE) * (*HandleCount));
  if (*HandleArray == NULL) {
    DEBUG ((DEBUG_ERROR, "HandleArray allocate memory resource failed.\n"));
    *HandleCount = 0;
    return;
  }

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;

  for (Index = 0; Index < (*HandleCount); Index++) {
    Status = mSmbiosMiscSmbios->GetNext (
                                  mSmbiosMiscSmbios,
                                  &SmbiosHandle,
                                  &SmbiosType,
                                  &Record,
                                  NULL
                                  );

    if (!EFI_ERROR (Status)) {
      (*HandleArray)[Index] = Record->Handle;
    } else {
      break;
    }
  }
}
