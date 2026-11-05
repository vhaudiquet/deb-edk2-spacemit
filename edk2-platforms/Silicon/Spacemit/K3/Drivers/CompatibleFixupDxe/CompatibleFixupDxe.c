/** @file
  Fix up the root DTB "compatible" property for the COM260 board family.

  Every COM260 variant (k3_com260_ifx, k3_com260_tq, k3_com260_ifx_tq, ...)
  shares the k3-com260-ifx.dtb, whose first "compatible" entry is a static
  string.  Linux binds a DTB by matching that first entry, so this driver
  rewrites it to "spacemit,<product_name>" (product_name read from the TLV
  EEPROM) when the selected SKU is COM260.  The remaining entries are kept
  unchanged.

  Copyright (c) 2026, Spacemit Ltd.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Guid/Fdt.h>
#include <Guid/FdtHob.h>
#include <Protocol/PlatformInfo.h>

//
// MUSE-Pico.dsc [SkuIds]: 1|COM260
//
#define SKU_ID_COM260  1

#define PRODUCT_NAME_MAX  64
#define COMPATIBLE_MAX    256
#define DTB_EXPAND_EXTRA  256

STATIC
VOID *
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

        break;
      }
    }
  }

  //
  // Fall back to the HOB left by SEC/PEI.  It points at the same page-aligned
  // copy of the DTB.
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
CompatibleFixupDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS              Status;
  PLATFORM_INFO_PROTOCOL  *PlatformInfo;
  VOID                    *Fdt;
  CONST CHAR8             *Compatible;
  INT32                   RootOffset;
  INT32                   Len;
  INT32                   FirstNul;
  UINTN                   NewLen;
  UINTN                   RemainderLen;
  UINTN                   Index;
  CHAR8                   ProductName[PRODUCT_NAME_MAX];
  CHAR8                   NewCompatible[COMPATIBLE_MAX];

  if (LibPcdGetSku () != SKU_ID_COM260) {
    return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (
                  &gSpacemitPlatformInfoProtocolGuid,
                  NULL,
                  (VOID **)&PlatformInfo
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to locate platform info protocol: %r\n", __func__, Status));
    return Status;
  }

  SetMem (ProductName, sizeof (ProductName), 0);
  Status = PlatformInfo->GetPlatformInfo (
                           PlatformInfo,
                           "product_name",
                           ProductName,
                           sizeof (ProductName)
                           );
  if (EFI_ERROR (Status) || (ProductName[0] == '\0')) {
    DEBUG ((DEBUG_WARN, "%a: product_name unavailable: %r\n", __func__, Status));
    return Status;
  }

  //
  // Compatible strings use '-' rather than '_'; normalize the product_name
  // before embedding it (e.g. "k3_com260_ifx" -> "k3-com260-ifx").
  //
  for (Index = 0; ProductName[Index] != '\0'; Index++) {
    if (ProductName[Index] == '_') {
      ProductName[Index] = '-';
    }
  }

  Fdt = GetFdtBase ();
  if (Fdt == NULL) {
    DEBUG ((DEBUG_WARN, "%a: DTB not found\n", __func__));
    return EFI_NOT_FOUND;
  }

  RootOffset = FdtPathOffset (Fdt, "/");
  if (RootOffset < 0) {
    DEBUG ((DEBUG_WARN, "%a: DTB root node not found\n", __func__));
    return EFI_NOT_FOUND;
  }

  Compatible = FdtGetProp (Fdt, RootOffset, "compatible", &Len);
  if ((Compatible == NULL) || (Len <= 1)) {
    DEBUG ((DEBUG_WARN, "%a: DTB compatible property not found\n", __func__));
    return EFI_NOT_FOUND;
  }

  //
  // Locate the end of the first string in the compatible string list.
  //
  FirstNul = 0;
  while ((FirstNul < Len) && (Compatible[FirstNul] != '\0')) {
    FirstNul++;
  }

  if (FirstNul >= Len) {
    DEBUG ((DEBUG_WARN, "%a: DTB compatible property is not a string list\n", __func__));
    return EFI_INVALID_PARAMETER;
  }

  //
  // Rebuild the list: "spacemit,<product_name>" as the first entry, then the
  // remaining entries unchanged.
  //
  SetMem (NewCompatible, sizeof (NewCompatible), 0);
  AsciiSPrint (NewCompatible, sizeof (NewCompatible), "spacemit,%a", ProductName);
  NewLen = AsciiStrLen (NewCompatible) + 1;

  RemainderLen = (UINTN)(Len - FirstNul - 1);
  if (RemainderLen > 0) {
    if ((NewLen + RemainderLen) > sizeof (NewCompatible)) {
      DEBUG ((DEBUG_ERROR, "%a: compatible property too long\n", __func__));
      return EFI_BUFFER_TOO_SMALL;
    }

    CopyMem (NewCompatible + NewLen, Compatible + FirstNul + 1, RemainderLen);
    NewLen += RemainderLen;
  }

  //
  // Expand the DTB in place (it lives in a page-aligned buffer with slack) and
  // write the new compatible property.
  //
  if (FdtOpenInto (Fdt, Fdt, (INT32)(FdtTotalSize (Fdt) + DTB_EXPAND_EXTRA)) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to expand DTB\n", __func__));
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtSetProp (Fdt, FdtPathOffset (Fdt, "/"), "compatible", NewCompatible, (UINT32)NewLen) != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed to update compatible property\n", __func__));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "%a: compatible[0] = \"spacemit,%a\"\n", __func__, ProductName));
  return EFI_SUCCESS;
}
