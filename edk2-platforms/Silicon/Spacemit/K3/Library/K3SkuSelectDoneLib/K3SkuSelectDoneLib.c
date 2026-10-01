/** @file
  Dependency hook for modules that require K3 SKU selection to be complete.

  Copyright (c) 2026, SpacemiT Co., Ltd. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

EFI_STATUS
EFIAPI
K3SkuSelectDoneLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EFI_SUCCESS;
}
