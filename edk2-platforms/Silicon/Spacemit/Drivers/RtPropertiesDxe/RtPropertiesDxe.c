/** @file
  Publish EFI_RT_PROPERTIES_TABLE describing the runtime services this
  platform actually supports after ExitBootServices().

  The UEFI specification requires a platform to publish this table when
  not every runtime service remains callable once the OS owns the
  platform.  Without it the OS must assume all services work, which on
  this platform turns every runtime call into a gamble: the callers
  that lose wedge the CPU inside firmware.

  What this platform really supports at runtime:

  - Variable reads and enumeration, and QueryVariableInfo: the
    variable driver serves them from its RAM cache of the store, no
    flash is touched.
  - ResetSystem: the reset library reprograms the watchdog.

  What it does not, and is masked off here:

  - The time services: the platform RealTimeClockLib refuses them at
    runtime by design (see SpacemitRealTimeClockLib) -- the platform
    clock is virtual, so nothing is lost.

  Everything else the firmware implements is advertised, including
  SetVariable and the other non-volatile writers (GetNextHighMono-
  Count, the capsule services): on a board whose OS leaves the flash
    controller accessible those calls genuinely work, and on one whose
  OS gates the QSPI clocks they now fail fast with a bounded timeout
  rather than hanging, because STQspiDxe bounds every controller
  register poll.  The table describes the firmware implementation; it
  is not a guarantee that any particular OS configuration can deliver
  the hardware underneath it.

  Masked services keep their callable EFI_UNSUPPORTED implementations,
  as the specification requires.


  Copyright (c) 2026, SpacemiT Co., Ltd. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Guid/RtPropertiesTable.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

//
// Runtime services supported after ExitBootServices(): everything the
// firmware implements except the time services, which it refuses at
// runtime by design.  The variable writers are advertised as working:
// on a platform whose OS leaves the flash controller accessible they
// do, and on one that gates the QSPI clocks they fail fast with the
// bounded timeout from STQspiDxe instead of hanging.
//
#define RT_SERVICES_SUPPORTED  ( \
    EFI_RT_SUPPORTED_GET_VARIABLE                    | \
    EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME          | \
    EFI_RT_SUPPORTED_SET_VARIABLE                    | \
    EFI_RT_SUPPORTED_SET_VIRTUAL_ADDRESS_MAP         | \
    EFI_RT_SUPPORTED_CONVERT_POINTER                 | \
    EFI_RT_SUPPORTED_GET_NEXT_HIGH_MONOTONIC_COUNT   | \
    EFI_RT_SUPPORTED_RESET_SYSTEM                    | \
    EFI_RT_SUPPORTED_UPDATE_CAPSULE                  | \
    EFI_RT_SUPPORTED_QUERY_CAPSULE_CAPABILITIES      | \
    EFI_RT_SUPPORTED_QUERY_VARIABLE_INFO             \
    )

/**
  Entry point: install the runtime services properties table.

  @param[in]  ImageHandle   The firmware allocated handle for this image.
  @param[in]  SystemTable   The EFI system table.

  @retval EFI_SUCCESS             Table installed.
  @retval EFI_OUT_OF_RESOURCES    The table copy could not be allocated.
  @retval Other                   Installation failed.

**/
EFI_STATUS
EFIAPI
RtPropertiesInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_RT_PROPERTIES_TABLE   *Table;
  STATIC CONST EFI_RT_PROPERTIES_TABLE  Template = {
    EFI_RT_PROPERTIES_TABLE_VERSION,
    sizeof (EFI_RT_PROPERTIES_TABLE),
    RT_SERVICES_SUPPORTED
  };

  //
  // The OS reads the table through the system table configuration
  // entry at boot; keep it in runtime memory so its lifetime is not
  // tied to this driver's image.
  //
  Table = AllocateRuntimeCopyPool (sizeof (Template), &Template);
  if (Table == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->InstallConfigurationTable (&gEfiRtPropertiesTableGuid, Table);
  if (EFI_ERROR (Status)) {
    FreePool (Table);
    DEBUG ((DEBUG_ERROR, "%a: InstallConfigurationTable failed: %r\n", __func__, Status));
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "%a: runtime services supported mask 0x%08x\n",
    __func__,
    Table->RuntimeServicesSupported
    ));

  return EFI_SUCCESS;
}
