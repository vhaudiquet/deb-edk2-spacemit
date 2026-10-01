/** @file

  Read the DTB root "model" property and call LibPcdSetSku() to select
  the correct PCD SKU before any SKU-sensitive driver runs.

  SKU mapping:
    model contains "com260"   ->  SKU 1 (COM260)
    model contains "fml13v05" ->  SKU 2 (FML13V05)
    anything else             ->  SKU 0 (DEFAULT)

  Copyright (c) 2025 - 2026, Spacemit Corporation

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Guid/Fdt.h>
#include <Guid/FdtHob.h>

#define SKU_ID_DEFAULT    0
#define SKU_ID_COM260     1
#define SKU_ID_FML13V05   2
#define SKU_ID_EVB        3

STATIC
EFI_STATUS
SetSkuAndSignalDone (
  IN UINTN  SkuId
  )
{
  EFI_HANDLE  Handle;
  EFI_STATUS  Status;
  UINTN       SelectedSkuId;

  LibPcdSetSku (SkuId);
  SelectedSkuId = LibPcdGetSku ();
  if (SelectedSkuId != SkuId) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to select SKU %u; active SKU is %u\n",
      __func__,
      SkuId,
      SelectedSkuId
      ));
    return EFI_UNSUPPORTED;
  }

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gSpacemitK3SkuSelectDoneProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: Failed to install SKU-select-done protocol: %r\n",
      __func__,
      Status
      ));
  }

  return Status;
}

STATIC
CONST VOID *
GetFdtBase (
  VOID
  )
{
  UINTN   Index;
  VOID    *FdtBase;
  VOID    *Hob;
  UINT64  FdtAddress;

  //
  // Prefer the configuration table entry installed by FdtDxe.
  //
  if ((gST != NULL) && (gST->ConfigurationTable != NULL)) {
    for (Index = 0; Index < gST->NumberOfTableEntries; Index++) {
      if (CompareGuid (&gST->ConfigurationTable[Index].VendorGuid, &gFdtTableGuid)) {
        FdtBase = gST->ConfigurationTable[Index].VendorTable;
        if ((FdtBase != NULL) && (FdtCheckHeader (FdtBase) == 0)) {
          return FdtBase;
        }

        DEBUG ((DEBUG_WARN, "%a: Configuration-table DTB is invalid; trying FDT HOB\n", __func__));
        break;
      }
    }
  }

  //
  // Fall back to the HOB left by SEC/PEI.
  //
  Hob = GetFirstGuidHob (&gFdtHobGuid);
  if ((Hob == NULL) || (GET_GUID_HOB_DATA_SIZE (Hob) != sizeof (UINT64))) {
    return NULL;
  }

  FdtAddress = *((UINT64 *)GET_GUID_HOB_DATA (Hob));
  FdtBase    = (VOID *)(UINTN)FdtAddress;
  if ((FdtBase == NULL) || (FdtCheckHeader (FdtBase) != 0)) {
    return NULL;
  }

  return FdtBase;
}

EFI_STATUS
EFIAPI
SkuSelectDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CHAR8        *UpperModel;
  CONST VOID   *Fdt;
  INTN         RootOffset;
  INT32        Len;
  CONST CHAR8  *Model;
  UINTN        Index;
  UINTN        ModelLength;
  UINTN        SkuId;

  Fdt = GetFdtBase ();
  if (Fdt == NULL) {
    DEBUG ((DEBUG_WARN, "%a: DTB not found, using DEFAULT SKU\n", __func__));
    return SetSkuAndSignalDone (SKU_ID_DEFAULT);
  }

  RootOffset = FdtPathOffset (Fdt, "/");
  if (RootOffset < 0) {
    DEBUG ((DEBUG_WARN, "%a: DTB root node not found, using DEFAULT SKU\n", __func__));
    return SetSkuAndSignalDone (SKU_ID_DEFAULT);
  }

  Model = FdtGetProp (Fdt, RootOffset, "model", &Len);
  if ((Model == NULL) || (Len <= 0)) {
    DEBUG ((DEBUG_WARN, "%a: DTB model property not found, using DEFAULT SKU\n", __func__));
    return SetSkuAndSignalDone (SKU_ID_DEFAULT);
  }

  ModelLength = AsciiStrnLenS (Model, (UINTN)Len);
  if ((ModelLength == 0) || (ModelLength == (UINTN)Len)) {
    DEBUG ((DEBUG_WARN, "%a: DTB model property is invalid, using DEFAULT SKU\n", __func__));
    return SetSkuAndSignalDone (SKU_ID_DEFAULT);
  }

  DEBUG ((DEBUG_INFO, "%a: DTB model = \"%a\"\n", __func__, Model));

  UpperModel = AllocateCopyPool (ModelLength + 1, Model);
  if (UpperModel == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to normalize DTB model, using DEFAULT SKU\n", __func__));
    SkuId = SKU_ID_DEFAULT;
    goto SetSelectedSku;
  }

  for (Index = 0; Index < ModelLength; Index++) {
    UpperModel[Index] = AsciiCharToUpper (UpperModel[Index]);
  }

  if (AsciiStrStr (UpperModel, "COM260") != NULL) {
    SkuId = SKU_ID_COM260;
    DEBUG ((DEBUG_INFO, "%a: SKU set to COM260 (%u)\n", __func__, SkuId));
  } else if (AsciiStrStr (UpperModel, "FML13V05") != NULL) {
    SkuId = SKU_ID_FML13V05;
    DEBUG ((DEBUG_INFO, "%a: SKU set to FML13V05 (%u)\n", __func__, SkuId));
  } else if (AsciiStrStr (UpperModel, "K3_EVB") != NULL) {
    SkuId = SKU_ID_EVB;
    DEBUG ((DEBUG_INFO, "%a: SKU set to EVB (%u)\n", __func__, SkuId));
  } else {
    SkuId = SKU_ID_DEFAULT;
    DEBUG ((DEBUG_INFO, "%a: SKU set to DEFAULT (%u)\n", __func__, SkuId));
  }

  FreePool (UpperModel);

SetSelectedSku:
  return SetSkuAndSignalDone (SkuId);
}
