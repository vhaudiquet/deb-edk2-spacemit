/** @file
*
*  Copyright (c) 2025, Spacemit Limited. All rights reserved.
*  Copyright (c) 2024, Rivos, Inc.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <libfdt.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/HobLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Uefi/UefiBaseType.h>

#include <Protocol/PlatformInfo.h>
#include <Protocol/GraphicsOutput.h>
#include "FdtFixupDxe.h"

#define SHIFT1  29
#define MASK1   0x5555555555555555ULL
#define SHIFT2  17
#define MASK2   0x71d67fffeda60000ULL
#define SHIFT3  37
#define MASK3   0xfff7eee000000000ULL
#define SHIFT4  43

//
// Handle for the fdt fixup Protocol
//
STATIC EFI_HANDLE              mHandle                = NULL;
STATIC PLATFORM_INFO_PROTOCOL  *mPlatformInfoProtocol = NULL;

STATIC
UINT64
Rand (
  VOID
  )
{
  UINT64  Y = GetPerformanceCounter ();

  Y ^= (Y >> SHIFT1) & MASK1;
  Y ^= (Y << SHIFT2) & MASK2;
  Y ^= (Y << SHIFT3) & MASK3;
  Y ^= Y >> SHIFT4;

  return Y;
}

STATIC
VOID
GenerateRandomEthaddr (
  UINT8  *MacAddr
  )
{
  INT32  I;

  MacAddr[0] = 0xfe;
  MacAddr[1] = 0xfe;
  MacAddr[2] = 0xfe;

  DEBUG ((DEBUG_INFO, "Build random mac address\n"));
  for (I = 3; I < 6; I++) {
    MacAddr[I] = (UINT8)Rand ();
  }
}

STATIC
VOID
IncreaseEthaddr (
  UINT8  *MacAddr,
  UINT8  *NewMacAddr,
  UINT8  Offset
  )
{
  CopyMem (NewMacAddr, MacAddr, 6);

  NewMacAddr[5] += Offset;
  if (NewMacAddr[5] < MacAddr[5]) {
    NewMacAddr[4]++;
    if (0 == NewMacAddr[4]) {
      NewMacAddr[3]++;
    }
  }
}

STATIC
INT32
FdtPackReg (
  CONST VOID  *Fdt,
  VOID        *Buf,
  UINT64      Address,
  UINT64      Size
  )
{
  INT32  AddressCells = fdt_address_cells (Fdt, 0);
  INT32  SizeCells    = fdt_size_cells (Fdt, 0);
  CHAR8  *P           = Buf;

  if (AddressCells == 2) {
    *(fdt64_t *)P = cpu_to_fdt64 (Address);
  } else {
    *(fdt32_t *)P = cpu_to_fdt32 (Address);
  }

  P += 4 * AddressCells;

  if (SizeCells == 2) {
    *(fdt64_t *)P = cpu_to_fdt64 (Size);
  } else {
    *(fdt32_t *)P = cpu_to_fdt32 (Size);
  }

  P += 4 * SizeCells;

  return P - (CHAR8 *)Buf;
}

STATIC
VOID
UpdateSerialNumber (
  IN VOID  *Fdt
  )
{
  CHAR8  Serial[64], *P = Serial;
  INT32  Ret;

  SetMem (Serial, sizeof (Serial), 0);
  if (EFI_ERROR (
        mPlatformInfoProtocol->GetPlatformInfo (
                                 mPlatformInfoProtocol,
                                 "serial#",
                                 Serial,
                                 sizeof (Serial)
                                 )
        ))
  {
    P = (CHAR8 *)PcdGetPtr (PcdDefaultSerialNumber);
  }

  Ret = fdt_setprop (Fdt, 0, "serial-number", P, AsciiStrLen (P) + 1);
  if (Ret < 0) {
    DEBUG ((DEBUG_WARN, "Set serial-number fail(%a).\n", fdt_strerror (Ret)));
  }
}

STATIC
VOID
UpdatePartNumber (
  IN VOID  *Fdt
  )
{
  CHAR8  Part[64], *P = Part;
  INT32  Ret;

  SetMem (Part, sizeof (Part), 0);
  if (EFI_ERROR (
        mPlatformInfoProtocol->GetPlatformInfo (
                                 mPlatformInfoProtocol,
                                 "part#",
                                 Part,
                                 sizeof (Part)
                                 )
        ))
  {
    P = (CHAR8 *)PcdGetPtr (PcdDefaultPartNumber);
  }

  Ret = fdt_setprop (Fdt, 0, "part-number", P, AsciiStrLen (P) + 1);
  if (Ret < 0) {
    DEBUG ((DEBUG_WARN, "Set part-number fail(%a).\n", fdt_strerror (Ret)));
  }
}

STATIC
VOID
UpdateMacAddr (
  IN VOID  *Fdt
  )
{
  INT32        I, N, Offset, NodeOff, AliasesOff;
  UINT8        Mac[6], MacTemp[6];
  CONST CHAR8  *Name, *Path, *Status;
  BOOLEAN      MacValid = FALSE;
  CHAR8        EtherPaths[8][128];
  INT32        PathCount = 0;

  if (!EFI_ERROR (
         mPlatformInfoProtocol->GetPlatformInfo (
                                  mPlatformInfoProtocol,
                                  "ethaddr",
                                  Mac,
                                  sizeof (Mac)
                                  )
         ))
  {
    MacValid = TRUE;
  }

  //
  // First pass: collect ethernet alias paths without modifying the DTB so
  // that property offsets remain valid throughout the walk (O(n) instead of
  // the previous O(n²) restart-from-head approach).
  //
  AliasesOff = fdt_path_offset (Fdt, "/aliases");
  if (AliasesOff >= 0) {
    for (Offset = fdt_first_property_offset (Fdt, AliasesOff);
         Offset >= 0 && PathCount < (INT32)ARRAY_SIZE (EtherPaths);
         Offset = fdt_next_property_offset (Fdt, Offset))
    {
      Path = fdt_getprop_by_offset (Fdt, Offset, &Name, NULL);
      if (AsciiStrnCmp (Name, "ethernet", 8) == 0) {
        AsciiStrCpyS (EtherPaths[PathCount], sizeof (EtherPaths[PathCount]), Path);
        PathCount++;
      }
    }
  }

  //
  // Second pass: assign MACs.  fdt_path_offset() re-resolves each path after
  // any DTB modifications made by fdt_setprop(), so saved string paths remain
  // valid even as the blob shifts.
  //
  for (I = 0, N = 0; I < PathCount; I++) {
    NodeOff = fdt_path_offset (Fdt, EtherPaths[I]);
    if (NodeOff < 0) {
      continue;
    }

    Status = fdt_getprop (Fdt, NodeOff, "status", NULL);
    if (Status && !AsciiStrCmp (Status, "disabled")) {
      continue;
    }

    if (MacValid) {
      IncreaseEthaddr (Mac, MacTemp, N);
      N++;
    } else {
      GenerateRandomEthaddr (MacTemp);
    }

    fdt_setprop (Fdt, NodeOff, "mac-address", MacTemp, 6);
    fdt_setprop (Fdt, NodeOff, "local-mac-address", MacTemp, 6);
  }
}

//
// compatible is a NUL-separated list of strings.
//
STATIC
BOOLEAN
FdtCompatibleListContains (
  IN CONST CHAR8  *Compatible,
  IN INT32        Length,
  IN CONST CHAR8  *Name
  )
{
  INT32  Offset = 0;
  INT32  StringLength;

  while (Offset < Length) {
    StringLength = AsciiStrLen (Compatible + Offset);
    if ((StringLength > 0) && (AsciiStrCmp (Compatible + Offset, Name) == 0)) {
      return TRUE;
    }

    Offset += StringLength + 1;
  }

  return FALSE;
}

STATIC
UINT32
FdtNextPhandle (
  IN VOID  *Fdt
  )
{
  CONST UINT8  *Prop;
  INT32        NodeOff, Len, Depth;
  UINT32       Max, Value;

  Max = 0;
  for (NodeOff = fdt_next_node (Fdt, -1, &Depth);
       NodeOff >= 0;
       NodeOff = fdt_next_node (Fdt, NodeOff, &Depth))
  {
    Prop = fdt_getprop (Fdt, NodeOff, "phandle", &Len);
    if ((Prop != NULL) && (Len == (INT32)sizeof (UINT32))) {
      Value = ((UINT32)Prop[0] << 24) | ((UINT32)Prop[1] << 16) |
              ((UINT32)Prop[2] << 8) | (UINT32)Prop[3];
      if (Value > Max) {
        Max = Value;
      }
    }
  }

  return Max + 1;
}

//
// Pack an address/size pair with the cells the given parent node declares
// for its children. Returns the packed length, or -1 on unsupported cells.
//
STATIC
INT32
FdtPackRegAt (
  IN CONST VOID  *Fdt,
  IN INT32       ParentOff,
  OUT VOID       *Buf,
  IN UINT64      Address,
  IN UINT64      Size
  )
{
  INT32  AddressCells = fdt_address_cells (Fdt, ParentOff);
  INT32  SizeCells    = fdt_size_cells (Fdt, ParentOff);
  CHAR8  *P           = Buf;

  if ((AddressCells < 1) || (AddressCells > 2) ||
      (SizeCells < 1) || (SizeCells > 2))
  {
    return -1;
  }

  if (AddressCells == 2) {
    *(fdt64_t *)P = cpu_to_fdt64 (Address);
  } else {
    *(fdt32_t *)P = cpu_to_fdt32 (Address);
  }

  P += 4 * AddressCells;

  if (SizeCells == 2) {
    *(fdt64_t *)P = cpu_to_fdt64 (Size);
  } else {
    *(fdt32_t *)P = cpu_to_fdt32 (Size);
  }

  P += 4 * SizeCells;

  return P - (CHAR8 *)Buf;
}

//
// Describe the GOP framebuffer to the kernel as a simple-framebuffer node
// under /chosen, backed by a /reserved-memory region, so the kernel's early
// display driver can keep the display pipeline (whose clocks the node
// references) alive from the earliest boot point until the real display
// driver claims it. Without the node, the framebuffer is only described by
// the EFI screen_info, which carries no clock information: the generic
// kernel then gates the firmware's pipeline clocks as unused and the early
// scanout dies until the display driver re-enables it.
//
// The memory is described through a /reserved-memory region referenced by
// a memory-region phandle, like the kernel's own simple-framebuffer users:
// a plain reg property under /chosen does not survive the kernel's address
// translation (children of non-bus nodes), and the node is rebuilt from
// live GOP state on every fixup. Only created when the firmware actually
// lit a display: a headless boot keeps the devicetree unchanged.
//
STATIC
VOID
UpdateFramebufferNode (
  IN VOID  *Fdt
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL         *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  CONST VOID                           *DpuClocks;
  CONST CHAR8                          *Compatible, *Format, *NodeStatus;
  EFI_STATUS                           Status;
  UINT8                                Clocks[128];
  CHAR8                                Reg[16];
  INT32                                ClocksLength;
  INT32                                ClkLen, CompatLen;
  INT32                                ChosenOff, RsvOff, FbRsvOff, FbOff, SocOff, NodeOff;
  INT32                                RegLength, Ret;
  UINT32                               Phandle;

  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR (Status) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    return;
  }

  Info = Gop->Mode->Info;

  switch (Info->PixelFormat) {
    case PixelRedGreenBlueReserved8BitPerColor:
      Format = "x8b8g8r8";
      break;
    case PixelBlueGreenRedReserved8BitPerColor:
      Format = "x8r8g8b8";
      break;
    default:
      // Bit-mask and BLT-only formats have no direct simplefb representation.
      return;
  }

  //
  // The framebuffer memory: a /reserved-memory region, referenced from the
  // /chosen node by a memory-region phandle.
  //
  RsvOff = fdt_path_offset (Fdt, "/reserved-memory");
  if (RsvOff < 0) {
    RsvOff = fdt_add_subnode (Fdt, 0, "reserved-memory");
    if (RsvOff < 0) {
      DEBUG ((DEBUG_WARN, "add reserved-memory node fail(%a).\n", fdt_strerror (RsvOff)));
      return;
    }

    fdt_setprop_u32 (Fdt, RsvOff, "#address-cells", 2);
    fdt_setprop_u32 (Fdt, RsvOff, "#size-cells", 2);
    fdt_setprop (Fdt, RsvOff, "ranges", "", 0);
  }

  FbRsvOff = fdt_subnode_offset (Fdt, RsvOff, "framebuffer");
  if (FbRsvOff >= 0) {
    fdt_del_node (Fdt, FbRsvOff);
  }

  FbRsvOff = fdt_add_subnode (Fdt, RsvOff, "framebuffer");
  if (FbRsvOff < 0) {
    DEBUG ((DEBUG_WARN, "add framebuffer reserved-memory node fail(%a).\n", fdt_strerror (FbRsvOff)));
    return;
  }

  RegLength = FdtPackRegAt (Fdt, RsvOff, Reg, Gop->Mode->FrameBufferBase, Gop->Mode->FrameBufferSize);
  if (RegLength < 0) {
    return;
  }

  fdt_setprop (Fdt, FbRsvOff, "reg", Reg, RegLength);

  Phandle = FdtNextPhandle (Fdt);
  fdt_setprop_u32 (Fdt, FbRsvOff, "phandle", Phandle);

  //
  // The /chosen node the kernel's framebuffer arbitration looks for.
  // Resolved only now: libfdt offsets do not survive tree mutations, and
  // the reserved-memory insertion above is a root-level mutation that
  // shifts every later node. An offset resolved before it goes stale.
  //
  ChosenOff = fdt_path_offset (Fdt, "/chosen");
  if (ChosenOff < 0) {
    return;
  }

  FbOff = fdt_subnode_offset (Fdt, ChosenOff, "framebuffer");
  if (FbOff >= 0) {
    fdt_del_node (Fdt, FbOff);
  }

  FbOff = fdt_add_subnode (Fdt, ChosenOff, "framebuffer");
  if (FbOff < 0) {
    DEBUG ((DEBUG_WARN, "add framebuffer node fail(%a).\n", fdt_strerror (FbOff)));
    return;
  }

  fdt_setprop (Fdt, FbOff, "compatible", "simple-framebuffer", sizeof ("simple-framebuffer"));
  fdt_setprop_u32 (Fdt, FbOff, "width", Info->HorizontalResolution);
  fdt_setprop_u32 (Fdt, FbOff, "height", Info->VerticalResolution);
  fdt_setprop_u32 (Fdt, FbOff, "stride", Info->PixelsPerScanLine * 4);
  fdt_setprop (Fdt, FbOff, "format", Format, AsciiStrLen (Format) + 1);
  fdt_setprop_u32 (Fdt, FbOff, "memory-region", Phandle);

  //
  // Reference the display pipelines' own clocks, so the kernel holds the
  // very clock objects its display driver will later claim. The clocks of
  // every available (non-disabled) Saturn DPU are included.
  //
  ClocksLength = 0;
  SocOff = fdt_path_offset (Fdt, "/soc");
  if (SocOff >= 0) {
    fdt_for_each_subnode (NodeOff, Fdt, SocOff) {
      Compatible = fdt_getprop (Fdt, NodeOff, "compatible", &CompatLen);
      if ((Compatible == NULL) ||
          !FdtCompatibleListContains (Compatible, CompatLen, "spacemit,k3-saturn-dpu"))
      {
        continue;
      }

      NodeStatus = fdt_getprop (Fdt, NodeOff, "status", NULL);
      if ((NodeStatus != NULL) && (AsciiStrCmp (NodeStatus, "disabled") == 0)) {
        continue;
      }

      DpuClocks = fdt_getprop (Fdt, NodeOff, "clocks", &ClkLen);
      if ((DpuClocks == NULL) || (ClkLen <= 0) ||
          ((ClocksLength + ClkLen) > (INT32)sizeof (Clocks)))
      {
        continue;
      }

      CopyMem (Clocks + ClocksLength, DpuClocks, ClkLen);
      ClocksLength += ClkLen;
    }

    if (ClocksLength > 0) {
      Ret = fdt_setprop (Fdt, FbOff, "clocks", Clocks, ClocksLength);
      if (Ret < 0) {
        DEBUG ((DEBUG_WARN, "set framebuffer clocks fail(%a).\n", fdt_strerror (Ret)));
      }
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "Framebuffer node: %dx%d, stride %d, 0x%lx+0x%lx\n",
    Info->HorizontalResolution,
    Info->VerticalResolution,
    Info->PixelsPerScanLine * 4,
    Gop->Mode->FrameBufferBase,
    Gop->Mode->FrameBufferSize
    ));
}

STATIC
VOID
UpdateMemoryNode (
  IN VOID  *Fdt
  )
{
  EFI_PEI_HOB_POINTERS  Hob;
  INT32                 Offset, Ret, Length;
  UINT64                MemBase, MemSize;
  CHAR8                 MemoryInfo[64];

  /* delete memory node before add new memory node. */
  do {
    Offset = fdt_subnode_offset (Fdt, 0, "memory");
    if (Offset >= 0) {
      fdt_del_node (Fdt, Offset);
    }
  } while (Offset >= 0);

  //
  // Get the system memory bank info from memory hobs
  //
  Hob.Raw = GetFirstHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR);
  ASSERT (Hob.Raw != NULL);
  while ((Hob.Raw != NULL) && (!END_OF_HOB_LIST (Hob))) {
    if (Hob.ResourceDescriptor->ResourceType == EFI_RESOURCE_SYSTEM_MEMORY) {
      // add memory node
      MemBase = Hob.ResourceDescriptor->PhysicalStart;
      MemSize = Hob.ResourceDescriptor->ResourceLength;
      DEBUG ((DEBUG_INFO, "Memory node: base=0x%lx, size=0x%lx\n", MemBase, MemSize));

      SetMem (MemoryInfo, sizeof (MemoryInfo), 0);
      AsciiSPrint (MemoryInfo, sizeof (MemoryInfo), "memory@0x%lx", MemBase);
      Offset = fdt_add_subnode (Fdt, 0, MemoryInfo);
      Ret    = fdt_setprop (Fdt, Offset, "device_type", "memory", sizeof ("memory"));
      if (Ret < 0) {
        DEBUG ((DEBUG_WARN, "Set device_type fail(%a).\n", fdt_strerror (Ret)));
        break;
      }

      SetMem (MemoryInfo, sizeof (MemoryInfo), 0);
      Length = FdtPackReg (Fdt, MemoryInfo, MemBase, MemSize);

      Ret = fdt_setprop (Fdt, Offset, "reg", MemoryInfo, Length);
      if (Ret < 0) {
        DEBUG ((DEBUG_WARN, "Set reg fail(%a).\n", fdt_strerror (Ret)));
        break;
      }
    }

    Hob.Raw = GET_NEXT_HOB (Hob);
    Hob.Raw = GetNextHob (EFI_HOB_TYPE_RESOURCE_DESCRIPTOR, Hob.Raw);
  }
}

STATIC
VOID
UpdatePlatformInfo (
  IN VOID  *Fdt
  )
{
  EFI_STATUS  Status;
  UINT32      Value;
  INT32       NodeOff, Ret;

  Value  = 0;
  Status = mPlatformInfoProtocol->GetPlatformInfo (
                                    mPlatformInfoProtocol,
                                    "wafer_id",
                                    &Value,
                                    sizeof (Value)
                                    );
  if (!EFI_ERROR (Status)) {
    Ret = fdt_setprop_u32 (Fdt, 0, "wafer-id", Value);
    if (Ret < 0) {
      DEBUG ((DEBUG_WARN, "Set wafer-id fail(%a).\n", fdt_strerror (Ret)));
    }
  }

  Value  = 0;
  Status = mPlatformInfoProtocol->GetPlatformInfo (
                                    mPlatformInfoProtocol,
                                    "product_id",
                                    &Value,
                                    sizeof (Value)
                                    );
  if (!EFI_ERROR (Status)) {
    Ret = fdt_setprop_u32 (Fdt, 0, "product-id", Value);
    if (Ret < 0) {
      DEBUG ((DEBUG_WARN, "Set product-id fail(%a).\n", fdt_strerror (Ret)));
    }
  }

  Value  = 0;
  Status = mPlatformInfoProtocol->GetPlatformInfo (
                                    mPlatformInfoProtocol,
                                    "svt_dro",
                                    &Value,
                                    sizeof (Value)
                                    );
  if (!EFI_ERROR (Status)) {
    NodeOff = fdt_path_offset (Fdt, "/cpus");
    if (NodeOff >= 0) {
      Ret = fdt_setprop_u32 (Fdt, NodeOff, "svt-dro", Value);
      if (Ret < 0) {
        DEBUG ((DEBUG_WARN, "Set svt-dro fail(%a).\n", fdt_strerror (Ret)));
      }
    }
  }
}

STATIC
EFI_STATUS
EFIFdtUpdate (
  IN UINTN   FdtFileAddr,
  OUT UINTN  *DtbSize
  )
{
  INTN  Error;
  VOID  *Fdt;

  Fdt   = (VOID *)(UINTN)FdtFileAddr;
  Error = fdt_check_header (Fdt);
  if (0 != Error) {
    DEBUG ((DEBUG_ERROR, "ERROR: Device Tree header not valid (%a)\n", fdt_strerror (Error)));
    return EFI_INVALID_PARAMETER;
  }

  UpdateSerialNumber (Fdt);
  UpdatePartNumber (Fdt);
  UpdateMacAddr (Fdt);
  UpdateMemoryNode (Fdt);
  UpdateFramebufferNode (Fdt);
  UpdatePlatformInfo (Fdt);

  *DtbSize = (UINTN)fdt_totalsize (Fdt);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
EfiDeviceTreeFixup (
  IN EFI_DT_FIXUP_PROTOCOL  *This,
  IN OUT VOID               *Dtb,
  OUT UINTN                 *BufferSize,
  IN UINT32                 Flags
  )
{
  UINTN  RequiredSize;
  UINTN  TotalSize;

  DEBUG ((DEBUG_VERBOSE, "Dtb address: %p, %d", Dtb, Flags));

  if (!Dtb || !BufferSize || (Flags & ~EFI_DT_ALL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (fdt_check_header (Dtb)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Flags & EFI_DT_APPLY_FIXUPS) {
    // reserve more dtb space
    RequiredSize = fdt_off_dt_strings (Dtb) +
                   fdt_size_dt_strings (Dtb) + 0x4000;
    TotalSize = fdt_totalsize (Dtb);
    if (RequiredSize < TotalSize) {
      RequiredSize = TotalSize;
    }

    if (RequiredSize > *BufferSize) {
      *BufferSize = RequiredSize;
      return EFI_BUFFER_TOO_SMALL;
    }

    // expand dtb size for further modification
    fdt_set_totalsize (Dtb, *BufferSize);
    if (EFIFdtUpdate ((UINTN)Dtb, BufferSize)) {
      DEBUG ((DEBUG_ERROR, "failed to process device tree\n"));
      return EFI_INVALID_PARAMETER;
    }

    // set the correct size
    fdt_set_totalsize (Dtb, *BufferSize);
  }

  return EFI_SUCCESS;
}

//
// fdt fixup Protocol instance
//
STATIC EFI_DT_FIXUP_PROTOCOL  mFdtFixup = {
  EFI_DT_FIXUP_PROTOCOL_REVISION,
  EfiDeviceTreeFixup
};

EFI_STATUS
EFIAPI
FdtFixupInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  ASSERT_PROTOCOL_ALREADY_INSTALLED (NULL, &gEfiFdtFixupProtocolGuid);
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mHandle,
                  &gEfiFdtFixupProtocolGuid,
                  &mFdtFixup,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  // Locate the PlatformInfo protocol
  Status = gBS->LocateProtocol (
                  &gSpacemitPlatformInfoProtocolGuid,
                  NULL,
                  (void **)&mPlatformInfoProtocol
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}
