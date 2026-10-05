/** @file
  Overrides the SMBIOS Type 1 (System), Type 2 (Base Board) and Type 3
  (Chassis) board-identity strings installed by SmbiosMiscDxe with per-unit
  data read from the TLV EEPROM through the Spacemit PlatformInfo protocol.

  SmbiosMiscDxe builds these records from the PcdSmbios* dynamic PCDs at DXE
  dispatch time.  This driver supplements it for boards that carry a TLV
  EEPROM: it runs at ReadyToBoot, by which point every DXE driver has
  installed its SMBIOS records, and rewrites the strings that have a TLV
  counterpart through EFI_SMBIOS_PROTOCOL.UpdateString().

  UpdateString() can only rewrite a string that already exists in a record.
  A field whose PcdSmbios* value is empty has no string slot, so the
  corresponding TLV value is skipped for it and the field reports
  "Not Specified".  Boards that want a field TLV-overridable must give it a
  non-empty PCD default; that default then doubles as the fallback for
  units whose EEPROM lacks the TLV field.

  Copyright (c) 2026, SpacemiT Co., Ltd. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Guid/EventGroup.h>
#include <IndustryStandard/SmBios.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Protocol/PlatformInfo.h>
#include <Protocol/Smbios.h>

//
// TLV fields are ASCII strings.  SMBIOS_STRING_MAX_LENGTH is the per-string
// limit enforced by EFI_SMBIOS_PROTOCOL.UpdateString(); keep room for the NUL.
//
#define TLV_STRING_MAX_SIZE  (SMBIOS_STRING_MAX_LENGTH + 1)

STATIC EFI_SMBIOS_PROTOCOL     *mSmbios       = NULL;
STATIC PLATFORM_INFO_PROTOCOL  *mPlatformInfo = NULL;
STATIC BOOLEAN                 mOverrideDone  = FALSE;

/**
  Rewrite one string of an existing SMBIOS record with a TLV EEPROM field.

  The string slot must already exist in the record (non-zero StringNumber),
  and the TLV field must be present on this unit; otherwise the string
  installed by SmbiosMiscDxe from the PCD default is kept as-is.

  @param[in] SmbiosHandle  Handle of the record to update.
  @param[in] StringNumber  1-based index of the string within the record,
                           0 if the field has no string slot.
  @param[in] TlvName       PlatformInfo field name, e.g. "serial#".

**/
STATIC
VOID
OverrideRecordString (
  IN EFI_SMBIOS_HANDLE    SmbiosHandle,
  IN SMBIOS_TABLE_STRING  StringNumber,
  IN CONST CHAR8          *TlvName
  )
{
  EFI_STATUS        Status;
  EFI_SMBIOS_HANDLE Handle;
  UINTN             StrNumber;
  CHAR8             TlvString[TLV_STRING_MAX_SIZE];

  if (StringNumber == 0) {
    //
    // No string slot in the record: UpdateString() cannot create one.
    //
    DEBUG ((
      DEBUG_VERBOSE,
      "%a: no string slot, skipping TLV \"%a\"\n",
      __FUNCTION__,
      TlvName
      ));
    return;
  }

  Status = mPlatformInfo->GetPlatformInfo (
                            mPlatformInfo,
                            (CHAR8 *)TlvName,
                            TlvString,
                            sizeof (TlvString)
                            );
  if (EFI_ERROR (Status) || (TlvString[0] == '\0')) {
    //
    // This unit's EEPROM lacks the TLV field: keep the PCD value.
    //
    return;
  }

  Handle    = SmbiosHandle;
  StrNumber = StringNumber;
  Status    = mSmbios->UpdateString (mSmbios, &Handle, &StrNumber, TlvString);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_WARN,
      "%a: UpdateString() of TLV \"%a\" failed: %r\n",
      __FUNCTION__,
      TlvName,
      Status
      ));
  }
}

/**
  Override the System Information (Type 1) record(s).

  Manufacturer, Product Name and Serial Number are sourced from the TLV
  EEPROM's "manufacturer", "product_name" and "serial#" fields; the SKU
  Number from its "part#" field.  Version and Family stay PCD-only.
**/
STATIC
VOID
OverrideType1 (
  VOID
  )
{
  EFI_SMBIOS_HANDLE        SmbiosHandle;
  EFI_SMBIOS_TYPE          SmbiosType;
  EFI_SMBIOS_TABLE_HEADER  *Header;
  SMBIOS_TABLE_TYPE1       *Record;
  SMBIOS_TABLE_STRING      Manufacturer;
  SMBIOS_TABLE_STRING      ProductName;
  SMBIOS_TABLE_STRING      SerialNumber;
  SMBIOS_TABLE_STRING      SKUNumber;

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  SmbiosType   = EFI_SMBIOS_TYPE_SYSTEM_INFORMATION;

  while (!EFI_ERROR (
           mSmbios->GetNext (mSmbios, &SmbiosHandle, &SmbiosType, &Header, NULL)
           ))
  {
    Record = (SMBIOS_TABLE_TYPE1 *)Header;

    //
    // Snapshot the string numbers first: UpdateString() may reallocate the
    // record, invalidating Header/Record.
    //
    Manufacturer = Record->Manufacturer;
    ProductName  = Record->ProductName;
    SerialNumber = Record->SerialNumber;
    SKUNumber    = Record->SKUNumber;

    OverrideRecordString (SmbiosHandle, Manufacturer, "manufacturer");
    OverrideRecordString (SmbiosHandle, ProductName, "product_name");
    OverrideRecordString (SmbiosHandle, SerialNumber, "serial#");
    OverrideRecordString (SmbiosHandle, SKUNumber, "part#");
  }
}

/**
  Override the Base Board (Type 2) record(s).

  Manufacturer and Product Name come from the TLV EEPROM's "manufacturer"
  and "product_name" fields, Version from its "part#" field, Serial Number
  from its "serial#" field.  Asset Tag and Location In Chassis stay
  PCD-only.
**/
STATIC
VOID
OverrideType2 (
  VOID
  )
{
  EFI_SMBIOS_HANDLE        SmbiosHandle;
  EFI_SMBIOS_TYPE          SmbiosType;
  EFI_SMBIOS_TABLE_HEADER  *Header;
  SMBIOS_TABLE_TYPE2       *Record;
  SMBIOS_TABLE_STRING      Manufacturer;
  SMBIOS_TABLE_STRING      ProductName;
  SMBIOS_TABLE_STRING      Version;
  SMBIOS_TABLE_STRING      SerialNumber;

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  SmbiosType   = EFI_SMBIOS_TYPE_BASEBOARD_INFORMATION;

  while (!EFI_ERROR (
           mSmbios->GetNext (mSmbios, &SmbiosHandle, &SmbiosType, &Header, NULL)
           ))
  {
    Record = (SMBIOS_TABLE_TYPE2 *)Header;

    //
    // Snapshot the string numbers first: UpdateString() may reallocate the
    // record, invalidating Header/Record.
    //
    Manufacturer = Record->Manufacturer;
    ProductName  = Record->ProductName;
    Version      = Record->Version;
    SerialNumber = Record->SerialNumber;

    OverrideRecordString (SmbiosHandle, Manufacturer, "manufacturer");
    OverrideRecordString (SmbiosHandle, ProductName, "product_name");
    OverrideRecordString (SmbiosHandle, Version, "part#");
    OverrideRecordString (SmbiosHandle, SerialNumber, "serial#");
  }
}

/**
  Override the System Enclosure (Type 3) record(s).

  Manufacturer and Serial Number come from the TLV EEPROM's "manufacturer"
  and "serial#" fields.  Version, Asset Tag and SKU Number stay PCD-only.
**/
STATIC
VOID
OverrideType3 (
  VOID
  )
{
  EFI_SMBIOS_HANDLE        SmbiosHandle;
  EFI_SMBIOS_TYPE          SmbiosType;
  EFI_SMBIOS_TABLE_HEADER  *Header;
  SMBIOS_TABLE_TYPE3       *Record;
  SMBIOS_TABLE_STRING      Manufacturer;
  SMBIOS_TABLE_STRING      SerialNumber;

  SmbiosHandle = SMBIOS_HANDLE_PI_RESERVED;
  SmbiosType   = EFI_SMBIOS_TYPE_SYSTEM_ENCLOSURE;

  while (!EFI_ERROR (
           mSmbios->GetNext (mSmbios, &SmbiosHandle, &SmbiosType, &Header, NULL)
           ))
  {
    Record = (SMBIOS_TABLE_TYPE3 *)Header;

    //
    // Snapshot the string numbers first: UpdateString() may reallocate the
    // record, invalidating Header/Record.
    //
    Manufacturer = Record->Manufacturer;
    SerialNumber = Record->SerialNumber;

    OverrideRecordString (SmbiosHandle, Manufacturer, "manufacturer");
    OverrideRecordString (SmbiosHandle, SerialNumber, "serial#");
  }
}

/**
  ReadyToBoot notification: rewrite the Type 1/2/3 board-identity strings
  from the TLV EEPROM.

  Runs once.  By this point every DXE driver, SmbiosMiscDxe included, has
  installed its SMBIOS records, so this works regardless of dispatch order.
**/
STATIC
VOID
EFIAPI
SmbiosTlvOverrideOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mOverrideDone) {
    return;
  }

  mOverrideDone = TRUE;

  OverrideType1 ();
  OverrideType2 ();
  OverrideType3 ();
}

/**
  Entry point of this driver.

  Both protocols are guaranteed present by the depex.  The override itself
  is deferred to ReadyToBoot (see SmbiosTlvOverrideOnReadyToBoot()).

  @param[in] ImageHandle    The firmware allocated handle for the image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       Event registered successfully.
  @retval other             Failed to locate a protocol or register the event.

**/
EFI_STATUS
EFIAPI
SmbiosTlvOverrideEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   ReadyToBootEvent;

  Status = gBS->LocateProtocol (
                  &gEfiSmbiosProtocolGuid,
                  NULL,
                  (VOID **)&mSmbios
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: no SMBIOS protocol: %r\n", __FUNCTION__, Status));
    return Status;
  }

  Status = gBS->LocateProtocol (
                  &gSpacemitPlatformInfoProtocolGuid,
                  NULL,
                  (VOID **)&mPlatformInfo
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: no PlatformInfo protocol: %r\n",
      __FUNCTION__,
      Status
      ));
    return Status;
  }

  return gBS->CreateEventEx (
                EVT_NOTIFY_SIGNAL,
                TPL_CALLBACK,
                SmbiosTlvOverrideOnReadyToBoot,
                NULL,
                &gEfiEventReadyToBootGuid,
                &ReadyToBootEvent
                );
}