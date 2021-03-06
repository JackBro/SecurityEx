/** @file
  UEFI Memory page management functions.

Copyright (c) 2007 - 2016, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "DxeMain.h"
#include "Imem.h"
#include <Library/PageTableLib.h>

#define EFI_DEFAULT_PAGE_ALLOCATION_ALIGNMENT  (EFI_PAGE_SIZE)

//
// Entry for tracking the memory regions for each memory type to coalesce similar memory types
//
typedef struct {
  EFI_PHYSICAL_ADDRESS  BaseAddress;
  EFI_PHYSICAL_ADDRESS  MaximumAddress;
  UINT64                CurrentNumberOfPages;
  UINT64                NumberOfPages;
  UINTN                 InformationIndex;
  BOOLEAN               Special;
  BOOLEAN               Runtime;
} EFI_MEMORY_TYPE_STATISTICS;

EFI_STATUS
CoreConvertPages (
  IN UINT64           Start,
  IN UINT64           NumberOfPages,
  IN EFI_MEMORY_TYPE  NewType
  );

//
// MemoryMap - The current memory map
//
UINTN     mMemoryMapKey = 0;

#define MAX_MAP_DEPTH 6

///
/// mMapDepth - depth of new descriptor stack
///
UINTN         mMapDepth = 0;
///
/// mMapStack - space to use as temp storage to build new map descriptors
///
MEMORY_MAP    mMapStack[MAX_MAP_DEPTH];
UINTN         mFreeMapStack = 0;
///
/// This list maintain the free memory map list
///
LIST_ENTRY   mFreeMemoryMapEntryList = INITIALIZE_LIST_HEAD_VARIABLE (mFreeMemoryMapEntryList);
BOOLEAN      mMemoryTypeInformationInitialized = FALSE;

EFI_MEMORY_TYPE_STATISTICS mMemoryTypeStatistics[EfiMaxMemoryType + 1] = {
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, TRUE,  FALSE },  // EfiReservedMemoryType
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiLoaderCode
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiLoaderData
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiBootServicesCode
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiBootServicesData
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, TRUE,  TRUE  },  // EfiRuntimeServicesCode
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, TRUE,  TRUE  },  // EfiRuntimeServicesData
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiConventionalMemory
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiUnusableMemory
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, TRUE,  FALSE },  // EfiACPIReclaimMemory
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, TRUE,  FALSE },  // EfiACPIMemoryNVS
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiMemoryMappedIO
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiMemoryMappedIOPortSpace
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, TRUE,  TRUE  },  // EfiPalCode
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE },  // EfiPersistentMemory
  { 0, MAX_ADDRESS, 0, 0, EfiMaxMemoryType, FALSE, FALSE }   // EfiMaxMemoryType
};

EFI_PHYSICAL_ADDRESS mDefaultMaximumAddress = MAX_ADDRESS;
EFI_PHYSICAL_ADDRESS mDefaultBaseAddress = MAX_ADDRESS;

EFI_MEMORY_TYPE_INFORMATION gMemoryTypeInformation[EfiMaxMemoryType + 1] = {
  { EfiReservedMemoryType,      0 },
  { EfiLoaderCode,              0 },
  { EfiLoaderData,              0 },
  { EfiBootServicesCode,        0 },
  { EfiBootServicesData,        0 },
  { EfiRuntimeServicesCode,     0 },
  { EfiRuntimeServicesData,     0 },
  { EfiConventionalMemory,      0 },
  { EfiUnusableMemory,          0 },
  { EfiACPIReclaimMemory,       0 },
  { EfiACPIMemoryNVS,           0 },
  { EfiMemoryMappedIO,          0 },
  { EfiMemoryMappedIOPortSpace, 0 },
  { EfiPalCode,                 0 },
  { EfiPersistentMemory,        0 },
  { EfiMaxMemoryType,           0 }
};
//
// Only used when load module at fixed address feature is enabled. True means the memory is alreay successfully allocated
// and ready to load the module in to specified address.or else, the memory is not ready and module will be loaded at a 
//  address assigned by DXE core.
//
GLOBAL_REMOVE_IF_UNREFERENCED   BOOLEAN       gLoadFixedAddressCodeMemoryReady = FALSE;

EFI_MEMORY_TYPE        mMemoryTypeForHeapGuard[] = {
  // EfiReservedMemoryType,
  EfiLoaderCode,
  EfiLoaderData,
  EfiBootServicesCode,
  EfiBootServicesData,
  // EfiRuntimeServicesCode,
  // EfiRuntimeServicesData,
  // EfiConventionalMemory,
  // EfiUnusableMemory,
  // EfiACPIReclaimMemory,
  // EfiACPIMemoryNVS,
  // EfiMemoryMappedIO,
  // EfiMemoryMappedIOPortSpace,
  //  EfiPalCode,
};

BOOLEAN
IsMemoryTypeForHeapGuard (
  IN EFI_MEMORY_TYPE        MemoryType
  )
{
  UINTN  Index;

  for (Index = 0; Index < sizeof(mMemoryTypeForHeapGuard) / sizeof(mMemoryTypeForHeapGuard[0]); Index++) {
    if (MemoryType == mMemoryTypeForHeapGuard[Index]) {
      return TRUE;
    }
  }
  return FALSE;
}

BOOLEAN
IsAllocateTypeForHeapGuard (
  IN EFI_ALLOCATE_TYPE      Type
  )
{
  if ((Type == AllocateMaxAddress || Type == AllocateAnyPages)) {
    return TRUE;
  }
  return FALSE;
}

typedef enum {
  GuardPageTypeUnallocated,
  GuardPageTypeAllocatedUnguarded,
  GuardPageTypeGuarded,
} GUARD_PAGE_TYPE;

typedef enum {
  GuardOperationNoAction,
  GuardOperationSetGuard,
  GuardOperationClearGuard,
} GUARD_OPERATION;

#define GUARD_PAGE_HEAD_SIGNATURE   SIGNATURE_32('g','h','d','0')

typedef struct {
  UINT32            Signature;
  UINT32            Reserved;
  PHYSICAL_ADDRESS  Address;
  LIST_ENTRY        Link;
} GUARD_PAGE_HEAD;

#define GUARD_PAGE_TAIL_SIGNATURE   SIGNATURE_32('g','t','a','l')

typedef GUARD_PAGE_HEAD GUARD_PAGE_TAIL;

#define GUARD_HEAD_TO_TAIL(a)   \
  ((GUARD_PAGE_TAIL *) (((CHAR8 *) (a)) + EFI_PAGES_TO_SIZE(1) - sizeof(GUARD_PAGE_TAIL)));

#define GUARD_TAIL_TO_HEAD(a)   \
  ((GUARD_PAGE_HEAD *) (((CHAR8 *) (a)) + sizeof(GUARD_PAGE_TAIL) - EFI_PAGES_TO_SIZE(1)));

GLOBAL_REMOVE_IF_UNREFERENCED LIST_ENTRY  mGuardPageList = INITIALIZE_LIST_HEAD_VARIABLE (mGuardPageList);

BOOLEAN
IsTheGuardPageGuarded(
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  EFI_STATUS  Status;
  UINT64      Attributes;

  ASSERT((Address & (SIZE_4KB - 1)) == 0);
  if (Address == ((UINTN)&mGuardPageList & ~(SIZE_4KB - 1))) {
    // Skip the list head node.
    return FALSE;
  }

  Status = GetMemoryPageAttributes(
             NULL,
             Address,
             &Attributes,
             NULL
             );
  ASSERT_EFI_ERROR(Status);
  if ((Attributes & EFI_MEMORY_RP) == 0) {
    return FALSE;
  } else {
    return TRUE;
  }
}

VOID
UnguardTheGuardPage (
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  ASSERT((Address & (SIZE_4KB - 1)) == 0);
  if (Address == ((UINTN)&mGuardPageList & ~(SIZE_4KB - 1))) {
    // Skip the list head node.
    return;
  }

  ClearMemoryPageAttributes (
    NULL,
    Address,
    EFI_PAGES_TO_SIZE(1),
    EFI_MEMORY_RP,
    NULL
    );
}

VOID
GuardTheGuardPage (
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  ASSERT((Address & (SIZE_4KB - 1)) == 0);
  if (Address == ((UINTN)&mGuardPageList & ~(SIZE_4KB - 1))) {
    // Skip the list head node.
    return;
  }

  SetMemoryPageAttributes (
    NULL,
    Address,
    EFI_PAGES_TO_SIZE(1),
    EFI_MEMORY_RP,
    NULL
    );
}

VOID
DumpGuardPages (
  VOID
  )
{
  LIST_ENTRY      *Link;
  GUARD_PAGE_HEAD *GuardPageHead;
  GUARD_PAGE_TAIL *GuardPageTail;

  DEBUG((EFI_D_INFO, "DumpGuardPages - head: 0x%08x (0x%08x, 0x%08x)\n", &mGuardPageList, mGuardPageList.BackLink, mGuardPageList.ForwardLink));

  for (Link = mGuardPageList.ForwardLink; Link != &mGuardPageList; Link = Link->ForwardLink) {
    GuardPageHead = BASE_CR(Link, GUARD_PAGE_HEAD, Link);

    if (IsTheGuardPageGuarded((UINTN)GuardPageHead & ~(SIZE_4KB - 1))) {
      // Do not go through link list, the are not present.
      return;
    }

    ASSERT ((GuardPageHead->Signature == GUARD_PAGE_HEAD_SIGNATURE) || (GuardPageHead->Signature == GUARD_PAGE_TAIL_SIGNATURE));
    if (GuardPageHead->Signature == GUARD_PAGE_HEAD_SIGNATURE) {
      GuardPageTail = GUARD_HEAD_TO_TAIL(GuardPageHead);
      ASSERT(GuardPageHead->Address == (UINTN)GuardPageHead);
      ASSERT(GuardPageHead->Address == GuardPageTail->Address);
      DEBUG((EFI_D_INFO, "GUARD_HEAD: 0x%08x, 0x%08x (0x%08x, 0x%08x)\n", GuardPageHead, &GuardPageHead->Link, GuardPageHead->Link.BackLink, GuardPageHead->Link.ForwardLink));
    }
    if (GuardPageHead->Signature == GUARD_PAGE_TAIL_SIGNATURE) {
      GuardPageTail = GuardPageHead;
      GuardPageHead = GUARD_TAIL_TO_HEAD(GuardPageTail);
      ASSERT(GuardPageHead->Address == (UINTN)GuardPageHead);
      ASSERT(GuardPageHead->Address == GuardPageTail->Address);
      DEBUG((EFI_D_INFO, "GUARD_TAIL: 0x%08x, 0x%08x (0x%08x, 0x%08x)\n", GuardPageTail, &GuardPageTail->Link, GuardPageTail->Link.BackLink, GuardPageTail->Link.ForwardLink));
    }
  }
  DEBUG((EFI_D_INFO, "DumpGuardPages Done\n"));
}

VOID
CheckGuardPages(
  VOID
  )
{
  LIST_ENTRY      *Link;
  GUARD_PAGE_HEAD *GuardPageHead;
  GUARD_PAGE_TAIL *GuardPageTail;

  for (Link = mGuardPageList.ForwardLink; Link != &mGuardPageList; Link = Link->ForwardLink) {
    GuardPageHead = BASE_CR(Link, GUARD_PAGE_HEAD, Link);
      
    if (IsTheGuardPageGuarded((UINTN)GuardPageHead & ~(SIZE_4KB - 1))) {
      // No need to check, they are already page protected.
      return;
    }

    ASSERT((GuardPageHead->Signature == GUARD_PAGE_HEAD_SIGNATURE) || (GuardPageHead->Signature == GUARD_PAGE_TAIL_SIGNATURE));
    if (GuardPageHead->Signature == GUARD_PAGE_HEAD_SIGNATURE) {
      GuardPageTail = GUARD_HEAD_TO_TAIL(GuardPageHead);
      ASSERT(GuardPageHead->Address == (UINTN)GuardPageHead);
      ASSERT(GuardPageHead->Address == GuardPageTail->Address);
    }
    if (GuardPageHead->Signature == GUARD_PAGE_TAIL_SIGNATURE) {
      GuardPageTail = GuardPageHead;
      GuardPageHead = GUARD_TAIL_TO_HEAD(GuardPageTail);
      ASSERT(GuardPageHead->Address == (UINTN)GuardPageHead);
      ASSERT(GuardPageHead->Address == GuardPageTail->Address);
    }
  }
}

GUARD_PAGE_TYPE
GetGuardPageType (
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  LIST_ENTRY      *Link;
  MEMORY_MAP      *Entry;
  GUARD_PAGE_HEAD *GuardPageHead;

  //
  // Find the entry that the covers the range
  //
  Entry = NULL;
  for (Link = gMemoryMap.ForwardLink; Link != &gMemoryMap; Link = Link->ForwardLink) {
    Entry = CR(Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);
    if (Entry->Start <= Address && Entry->End > Address) {
        break;
    }
  }
  if (Link == &gMemoryMap) {
    return GuardPageTypeUnallocated;
  }

  ASSERT (Entry != NULL);

  if (Entry->Type == EfiConventionalMemory) {
    return GuardPageTypeUnallocated;
  }

  //
  // Now we know it is allocated
  //

  for (Link = mGuardPageList.ForwardLink; Link != &mGuardPageList; Link = Link->ForwardLink) {
    GuardPageHead = BASE_CR(Link, GUARD_PAGE_HEAD, Link);

    if (IsTheGuardPageGuarded((UINTN)GuardPageHead & ~(SIZE_4KB - 1))) {
      // Do not go through link list, check presence directly.
      if (IsTheGuardPageGuarded(Address)) {
        return GuardPageTypeGuarded;
      } else {
        return GuardPageTypeAllocatedUnguarded;
      }
    }

    ASSERT((GuardPageHead->Signature == GUARD_PAGE_HEAD_SIGNATURE) || (GuardPageHead->Signature == GUARD_PAGE_TAIL_SIGNATURE));
    if (GuardPageHead->Address == Address) {
      return GuardPageTypeGuarded;
    }
  }
  return GuardPageTypeAllocatedUnguarded;
}

VOID
AllocateGuardPage(
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  EFI_STATUS  Status;
//  DEBUG ((EFI_D_INFO, "AllocateGuardPage - 0x%x\n", Address));
  Status = CoreConvertPages (Address, 1, EfiBootServicesData);
  ASSERT_EFI_ERROR(Status);
}

VOID
FreeGuardPage(
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  EFI_STATUS  Status;
//  DEBUG ((EFI_D_INFO, "FreeGuardPage - 0x%x\n", Address));
  Status = CoreConvertPages (Address, 1, EfiConventionalMemory);
  ASSERT_EFI_ERROR(Status);
}


/**
  Adds a node to the beginning of a doubly-linked list, and returns the pointer
  to the head node of the doubly-linked list.

  Adds the node Entry at the beginning of the doubly-linked list denoted by
  ListHead, and returns ListHead.

  If ListHead is NULL, then ASSERT().
  If Entry is NULL, then ASSERT().
  If ListHead was not initialized with INTIALIZE_LIST_HEAD_VARIABLE() or
  InitializeListHead(), then ASSERT().
  If PcdMaximumLinkedListLength is not zero, and prior to insertion the number
  of nodes in ListHead, including the ListHead node, is greater than or
  equal to PcdMaximumLinkedListLength, then ASSERT().

  @param  ListHead  A pointer to the head node of a doubly-linked list.
  @param  Entry     A pointer to a node that is to be inserted at the beginning
                    of a doubly-linked list.

  @return ListHead

**/
LIST_ENTRY *
EFIAPI
InsertHeadListGuarded (
  IN OUT  LIST_ENTRY                *ListHead,
  IN OUT  LIST_ENTRY                *Entry
  )
{
  BOOLEAN   IsEntryForwardLinkGuarded;

  IsEntryForwardLinkGuarded = IsTheGuardPageGuarded((UINTN)ListHead->ForwardLink & ~(SIZE_4KB - 1));
  if (IsEntryForwardLinkGuarded) {
    UnguardTheGuardPage((UINTN)ListHead->ForwardLink & ~(SIZE_4KB - 1));
  }

//  DEBUG((EFI_D_INFO, "InsertHeadListGuarded - Entry (0x%x)\n", Entry));
//  DEBUG((EFI_D_INFO, "InsertHeadListGuarded - ListHead (0x%x)\n", ListHead));
  Entry->ForwardLink = ListHead->ForwardLink;
  Entry->BackLink = ListHead;
//  DEBUG((EFI_D_INFO, "InsertHeadListGuarded - Entry->ForwardLink (0x%x)\n", Entry->ForwardLink));
  Entry->ForwardLink->BackLink = Entry;
  ListHead->ForwardLink = Entry;

  if (IsEntryForwardLinkGuarded) {
    UnguardTheGuardPage((UINTN)Entry->ForwardLink & ~(SIZE_4KB - 1));
  }
  return ListHead;
}

/**
  Adds a node to the end of a doubly-linked list, and returns the pointer to
  the head node of the doubly-linked list.

  Adds the node Entry to the end of the doubly-linked list denoted by ListHead,
  and returns ListHead.

  If ListHead is NULL, then ASSERT().
  If Entry is NULL, then ASSERT().
  If ListHead was not initialized with INTIALIZE_LIST_HEAD_VARIABLE() or 
  InitializeListHead(), then ASSERT().
  If PcdMaximumLinkedListLength is not zero, and prior to insertion the number
  of nodes in ListHead, including the ListHead node, is greater than or
  equal to PcdMaximumLinkedListLength, then ASSERT().

  @param  ListHead  A pointer to the head node of a doubly-linked list.
  @param  Entry     A pointer to a node that is to be added at the end of the
                    doubly-linked list.

  @return ListHead

**/
LIST_ENTRY *
EFIAPI
InsertTailListGuarded (
  IN OUT  LIST_ENTRY                *ListHead,
  IN OUT  LIST_ENTRY                *Entry
  )
{
  BOOLEAN   IsEntryBackLinkGuarded;

  IsEntryBackLinkGuarded = IsTheGuardPageGuarded((UINTN)ListHead->BackLink & ~(SIZE_4KB - 1));
  if (IsEntryBackLinkGuarded) {
    UnguardTheGuardPage((UINTN)ListHead->BackLink & ~(SIZE_4KB - 1));
  }

//  DEBUG((EFI_D_INFO, "InsertTailListGuarded - Entry (0x%x)\n", Entry));
  Entry->ForwardLink = ListHead;
//  DEBUG((EFI_D_INFO, "InsertTailListGuarded - ListHead (0x%x)\n", ListHead));
  Entry->BackLink = ListHead->BackLink;
//  DEBUG((EFI_D_INFO, "InsertTailListGuarded - Entry->BackLink (0x%x)\n", Entry->BackLink));
  Entry->BackLink->ForwardLink = Entry;
  ListHead->BackLink = Entry;

  if (IsEntryBackLinkGuarded) {
    UnguardTheGuardPage((UINTN)Entry->BackLink & ~(SIZE_4KB - 1));
  }
  return ListHead;
}

/**
  Removes a node from a doubly-linked list, and returns the node that follows
  the removed node.

  Removes the node Entry from a doubly-linked list. It is up to the caller of
  this function to release the memory used by this node if that is required. On
  exit, the node following Entry in the doubly-linked list is returned. If
  Entry is the only node in the linked list, then the head node of the linked
  list is returned.

  If Entry is NULL, then ASSERT().
  If Entry is the head node of an empty list, then ASSERT().
  If PcdMaximumLinkedListLength is not zero, and the number of nodes in the
  linked list containing Entry, including the Entry node, is greater than
  or equal to PcdMaximumLinkedListLength, then ASSERT().

  @param  Entry A pointer to a node in a linked list.

  @return Entry.

**/
LIST_ENTRY *
EFIAPI
RemoveEntryListGuarded (
  IN      CONST LIST_ENTRY          *Entry
  )
{
  BOOLEAN   IsEntryForwardLinkGuarded;
  BOOLEAN   IsEntryBackLinkGuarded;

  IsEntryForwardLinkGuarded = IsTheGuardPageGuarded((UINTN)Entry->ForwardLink & ~(SIZE_4KB - 1));
  if (IsEntryForwardLinkGuarded) {
    UnguardTheGuardPage((UINTN)Entry->ForwardLink & ~(SIZE_4KB - 1));
  }
  IsEntryBackLinkGuarded = IsTheGuardPageGuarded((UINTN)Entry->BackLink & ~(SIZE_4KB - 1));
  if (IsEntryBackLinkGuarded) {
    UnguardTheGuardPage((UINTN)Entry->BackLink & ~(SIZE_4KB - 1));
  }

//  DEBUG((EFI_D_INFO, "RemoveEntryListGuarded - Entry (0x%x)\n", Entry));
//  DEBUG((EFI_D_INFO, "RemoveEntryListGuarded - Entry->ForwardLink (0x%x)\n", Entry->ForwardLink));
//  DEBUG((EFI_D_INFO, "RemoveEntryListGuarded - Entry->BackLink (0x%x)\n", Entry->BackLink));
  Entry->ForwardLink->BackLink = Entry->BackLink;
  Entry->BackLink->ForwardLink = Entry->ForwardLink;

  if (IsEntryForwardLinkGuarded) {
    UnguardTheGuardPage((UINTN)Entry->ForwardLink & ~(SIZE_4KB - 1));
  }
  if (IsEntryBackLinkGuarded) {
    UnguardTheGuardPage((UINTN)Entry->BackLink & ~(SIZE_4KB - 1));
  }
  return Entry->ForwardLink;
}


VOID
SetGuardPage (
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  GUARD_PAGE_HEAD  *GuardPageHead;
  GUARD_PAGE_TAIL  *GuardPageTail;

//  DEBUG ((EFI_D_INFO, "SetGuardPage - 0x%x\n", Address));

  GuardPageHead = (VOID *)(UINTN)Address;
  GuardPageHead->Signature = GUARD_PAGE_HEAD_SIGNATURE;
  GuardPageHead->Address = Address;
  InsertTailListGuarded (&mGuardPageList, &GuardPageHead->Link);

  GuardPageTail = GUARD_HEAD_TO_TAIL(GuardPageHead);
  GuardPageTail->Signature = GUARD_PAGE_TAIL_SIGNATURE;
  GuardPageTail->Address = Address;
  InsertTailListGuarded (&mGuardPageList, &GuardPageTail->Link);
}

VOID
ClearGuardPage (
  IN EFI_PHYSICAL_ADDRESS   Address
  )
{
  GUARD_PAGE_HEAD  *GuardPageHead;
  GUARD_PAGE_TAIL  *GuardPageTail;

//  DEBUG ((EFI_D_INFO, "ClearGuardPage - 0x%x\n", Address));

  GuardPageHead = (VOID *)(UINTN)Address;
  UnguardTheGuardPage(Address);

  ASSERT (GuardPageHead->Signature == GUARD_PAGE_HEAD_SIGNATURE);
  ASSERT (GuardPageHead->Address == Address);
  RemoveEntryListGuarded (&GuardPageHead->Link);

  GuardPageTail = GUARD_HEAD_TO_TAIL(GuardPageHead);
  ASSERT (GuardPageTail->Signature == GUARD_PAGE_TAIL_SIGNATURE);
  ASSERT (GuardPageTail->Address == Address);
  RemoveEntryListGuarded (&GuardPageTail->Link);
}

/**
  +---------+--------------+---------+
  |GuardPage|Allocated Page|GuardPage|
  +---------+--------------+---------+
**/
VOID
SetGuardPageOnAllocatePages (
  IN EFI_PHYSICAL_ADDRESS   Memory,
  IN UINTN                  NumberOfPages
  )
{
  DEBUG ((EFI_D_INFO, "SetGuardPageOnAllocatePages - 0x%lx (0x%x)\n", Memory, NumberOfPages));

  SetGuardPage (Memory - EFI_PAGES_TO_SIZE(1));
  SetGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages));

//  DumpGuardPages ();
}

/**
We need consider below situations.
                 +-------+-----------+------------+---------+-------+
                 | GUARD | Mem_Start | Mem_Middle | Mem_End | GUARD |
                 +-------+-----------+------------+---------+-------+

==> (Free All)

                         +-----------+------------+---------+-------+
==> (Free Start)         |   GUARD   | Mem_Middle | Mem_End | GUARD |
                         +-----------+------------+---------+-------+
                 +-------+-----------+------------+---------+-------+
==> (Free Middle)| GUARD | Mem_Start |   GUARD    | Mem_End | GUARD |
                 +-------+-----------+------------+---------+-------+
                 +-------+-----------+------------+---------+
==> (Free End)   | GUARD | Mem_Start | Mem_Middle |  GUARD  |
                 +-------+-----------+------------+---------+
}

Operation is an array to carry 4 entries.
  0: Operation for Memory - EFI_PAGES_TO_SIZE(1)                   // GuardOperationNoAction or GuardOperationClearGuard
  1: Operation for Memory                                          // GuardOperationNoAction or GuardOperationSetGuard
  2: Operation for Memory + EFI_PAGES_TO_SIZE(NumberOfPages - 1)   // GuardOperationNoAction or GuardOperationSetGuard
  3: Operation for Memory + EFI_PAGES_TO_SIZE(NumberOfPages)       // GuardOperationNoAction or GuardOperationClearGuard

**/
VOID
ClearGuardPageOnFreePages (
  IN EFI_PHYSICAL_ADDRESS   Memory,
  IN UINTN                  NumberOfPages,
  OUT GUARD_OPERATION       *Operation
  )
{
  GUARD_PAGE_TYPE   PreviousPageType;
  GUARD_PAGE_TYPE   Previous2PageType;
  GUARD_PAGE_TYPE   NextPageType;
  GUARD_PAGE_TYPE   Next2PageType;

  DEBUG ((EFI_D_INFO, "ClearGuardPageOnFreePages - 0x%lx (0x%x)\n", Memory, NumberOfPages));

  ZeroMem (Operation, sizeof(GUARD_OPERATION) * 4);

  PreviousPageType = GetGuardPageType (Memory - EFI_PAGES_TO_SIZE(1));
  Previous2PageType = GetGuardPageType (Memory - EFI_PAGES_TO_SIZE(2));
  if (PreviousPageType == GuardPageTypeGuarded) {
    if (Previous2PageType != GuardPageTypeAllocatedUnguarded) {
      ClearGuardPage (Memory - EFI_PAGES_TO_SIZE(1));
      FreeGuardPage (Memory - EFI_PAGES_TO_SIZE(1));
      Operation[0] = GuardOperationNoAction;
    }
  } else if (PreviousPageType == GuardPageTypeAllocatedUnguarded) {
    AllocateGuardPage (Memory);
    SetGuardPage (Memory);
    Operation[1] = GuardOperationSetGuard;
  } else {
    ASSERT(FALSE);
  }

  NextPageType = GetGuardPageType (Memory + EFI_PAGES_TO_SIZE(NumberOfPages));
  Next2PageType = GetGuardPageType (Memory + EFI_PAGES_TO_SIZE(NumberOfPages + 1));
  if (NextPageType == GuardPageTypeGuarded) {
    if (Next2PageType != GuardPageTypeAllocatedUnguarded) {
      ClearGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages));
      FreeGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages));
      Operation[3] = GuardOperationNoAction;
    }
  } else if (NextPageType == GuardPageTypeAllocatedUnguarded) {
    AllocateGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages - 1));
    SetGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages - 1));
    Operation[2] = GuardOperationSetGuard;
  } else {
    ASSERT(FALSE);
  }

//  DumpGuardPages ();
}

VOID
SetGuardPageOnAllocatePoolPages (
  IN EFI_PHYSICAL_ADDRESS   Memory,
  IN UINTN                  NumberOfPages
  )
{
  DEBUG ((EFI_D_INFO, "SetGuardPageOnAllocatePoolPages - 0x%lx (0x%x)\n", Memory, NumberOfPages));

  SetGuardPage (Memory - EFI_PAGES_TO_SIZE(1));
  SetGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages));

//  DumpGuardPages ();
}

VOID
ClearGuardPageOnFreePoolPages (
  IN EFI_PHYSICAL_ADDRESS   Memory,
  IN UINTN                  NumberOfPages
  )
{
  DEBUG ((EFI_D_INFO, "ClearGuardPageOnFreePoolPages - 0x%lx (0x%x)\n", Memory, NumberOfPages));

  ClearGuardPage (Memory - EFI_PAGES_TO_SIZE(1));
  FreeGuardPage (Memory - EFI_PAGES_TO_SIZE(1));

  ClearGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages));
  FreeGuardPage (Memory + EFI_PAGES_TO_SIZE(NumberOfPages));

//  DumpGuardPages();
}

/**
  Enter critical section by gaining lock on gMemoryLock.

**/
VOID
CoreAcquireMemoryLock (
  VOID
  )
{
  CoreAcquireLock (&gMemoryLock);
}



/**
  Exit critical section by releasing lock on gMemoryLock.

**/
VOID
CoreReleaseMemoryLock (
  VOID
  )
{
  CoreReleaseLock (&gMemoryLock);
}




/**
  Internal function.  Removes a descriptor entry.

  @param  Entry                  The entry to remove

**/
VOID
RemoveMemoryMapEntry (
  IN OUT MEMORY_MAP      *Entry
  )
{
  RemoveEntryList (&Entry->Link);
  Entry->Link.ForwardLink = NULL;

  if (Entry->FromPages) {
    //
    // Insert the free memory map descriptor to the end of mFreeMemoryMapEntryList
    //
    InsertTailList (&mFreeMemoryMapEntryList, &Entry->Link);
  }
}

/**
  Internal function.  Adds a ranges to the memory map.
  The range must not already exist in the map.

  @param  Type                   The type of memory range to add
  @param  Start                  The starting address in the memory range Must be
                                 paged aligned
  @param  End                    The last address in the range Must be the last
                                 byte of a page
  @param  Attribute              The attributes of the memory range to add

**/
VOID
CoreAddRange (
  IN EFI_MEMORY_TYPE          Type,
  IN EFI_PHYSICAL_ADDRESS     Start,
  IN EFI_PHYSICAL_ADDRESS     End,
  IN UINT64                   Attribute
  )
{
  LIST_ENTRY        *Link;
  MEMORY_MAP        *Entry;

  ASSERT ((Start & EFI_PAGE_MASK) == 0);
  ASSERT (End > Start) ;

  ASSERT_LOCKED (&gMemoryLock);

  DEBUG ((DEBUG_PAGE, "AddRange: %lx-%lx to %d\n", Start, End, Type));
  
  //
  // If memory of type EfiConventionalMemory is being added that includes the page 
  // starting at address 0, then zero the page starting at address 0.  This has 
  // two benifits.  It helps find NULL pointer bugs and it also maximizes 
  // compatibility with operating systems that may evaluate memory in this page 
  // for legacy data structures.  If memory of any other type is added starting 
  // at address 0, then do not zero the page at address 0 because the page is being 
  // used for other purposes.
  //  
  if (Type == EfiConventionalMemory && Start == 0 && (End >= EFI_PAGE_SIZE - 1)) {
    SetMem ((VOID *)(UINTN)Start, EFI_PAGE_SIZE, 0);
  }
  
  //
  // Memory map being altered so updated key
  //
  mMemoryMapKey += 1;

  //
  // UEFI 2.0 added an event group for notificaiton on memory map changes.
  // So we need to signal this Event Group every time the memory map changes.
  // If we are in EFI 1.10 compatability mode no event groups will be
  // found and nothing will happen we we call this function. These events
  // will get signaled but since a lock is held around the call to this
  // function the notificaiton events will only be called after this funciton
  // returns and the lock is released.
  //
  CoreNotifySignalList (&gEfiEventMemoryMapChangeGuid);

  //
  // Look for adjoining memory descriptor
  //

  // Two memory descriptors can only be merged if they have the same Type
  // and the same Attribute
  //

  Link = gMemoryMap.ForwardLink;
  while (Link != &gMemoryMap) {
    Entry = CR (Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);
    Link  = Link->ForwardLink;

    if (Entry->Type != Type) {
      continue;
    }

    if (Entry->Attribute != Attribute) {
      continue;
    }

    if (Entry->End + 1 == Start) {

      Start = Entry->Start;
      RemoveMemoryMapEntry (Entry);

    } else if (Entry->Start == End + 1) {

      End = Entry->End;
      RemoveMemoryMapEntry (Entry);
    }
  }

  //
  // Add descriptor
  //

  mMapStack[mMapDepth].Signature     = MEMORY_MAP_SIGNATURE;
  mMapStack[mMapDepth].FromPages      = FALSE;
  mMapStack[mMapDepth].Type          = Type;
  mMapStack[mMapDepth].Start         = Start;
  mMapStack[mMapDepth].End           = End;
  mMapStack[mMapDepth].VirtualStart  = 0;
  mMapStack[mMapDepth].Attribute     = Attribute;
  InsertTailList (&gMemoryMap, &mMapStack[mMapDepth].Link);

  mMapDepth += 1;
  ASSERT (mMapDepth < MAX_MAP_DEPTH);

  return ;
}

/**
  Internal function.  Deque a descriptor entry from the mFreeMemoryMapEntryList.
  If the list is emtry, then allocate a new page to refuel the list.
  Please Note this algorithm to allocate the memory map descriptor has a property
  that the memory allocated for memory entries always grows, and will never really be freed
  For example, if the current boot uses 2000 memory map entries at the maximum point, but
  ends up with only 50 at the time the OS is booted, then the memory associated with the 1950
  memory map entries is still allocated from EfiBootServicesMemory.


  @return The Memory map descriptor dequed from the mFreeMemoryMapEntryList

**/
MEMORY_MAP *
AllocateMemoryMapEntry (
  VOID
  )
{
  MEMORY_MAP*            FreeDescriptorEntries;
  MEMORY_MAP*            Entry;
  UINTN                  Index;

  if (IsListEmpty (&mFreeMemoryMapEntryList)) {
    //
    // The list is empty, to allocate one page to refuel the list
    //
    FreeDescriptorEntries = CoreAllocatePoolPages (EfiBootServicesData, EFI_SIZE_TO_PAGES(DEFAULT_PAGE_ALLOCATION), DEFAULT_PAGE_ALLOCATION, FALSE);
    if(FreeDescriptorEntries != NULL) {
      //
      // Enque the free memmory map entries into the list
      //
      for (Index = 0; Index< DEFAULT_PAGE_ALLOCATION / sizeof(MEMORY_MAP); Index++) {
        FreeDescriptorEntries[Index].Signature = MEMORY_MAP_SIGNATURE;
        InsertTailList (&mFreeMemoryMapEntryList, &FreeDescriptorEntries[Index].Link);
      }
    } else {
      return NULL;
    }
  }
  //
  // dequeue the first descriptor from the list
  //
  Entry = CR (mFreeMemoryMapEntryList.ForwardLink, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);
  RemoveEntryList (&Entry->Link);

  return Entry;
}


/**
  Internal function.  Moves any memory descriptors that are on the
  temporary descriptor stack to heap.

**/
VOID
CoreFreeMemoryMapStack (
  VOID
  )
{
  MEMORY_MAP      *Entry;
  MEMORY_MAP      *Entry2;
  LIST_ENTRY      *Link2;

  ASSERT_LOCKED (&gMemoryLock);

  //
  // If already freeing the map stack, then return
  //
  if (mFreeMapStack != 0) {
    return ;
  }

  //
  // Move the temporary memory descriptor stack into pool
  //
  mFreeMapStack += 1;

  while (mMapDepth != 0) {
    //
    // Deque an memory map entry from mFreeMemoryMapEntryList
    //
    Entry = AllocateMemoryMapEntry ();

    ASSERT (Entry);

    //
    // Update to proper entry
    //
    mMapDepth -= 1;

    if (mMapStack[mMapDepth].Link.ForwardLink != NULL) {

      //
      // Move this entry to general memory
      //
      RemoveEntryList (&mMapStack[mMapDepth].Link);
      mMapStack[mMapDepth].Link.ForwardLink = NULL;

      CopyMem (Entry , &mMapStack[mMapDepth], sizeof (MEMORY_MAP));
      Entry->FromPages = TRUE;

      //
      // Find insertion location
      //
      for (Link2 = gMemoryMap.ForwardLink; Link2 != &gMemoryMap; Link2 = Link2->ForwardLink) {
        Entry2 = CR (Link2, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);
        if (Entry2->FromPages && Entry2->Start > Entry->Start) {
          break;
        }
      }

      InsertTailList (Link2, &Entry->Link);

    } else {
      //
      // This item of mMapStack[mMapDepth] has already been dequeued from gMemoryMap list,
      // so here no need to move it to memory.
      //
      InsertTailList (&mFreeMemoryMapEntryList, &Entry->Link);
    }
  }

  mFreeMapStack -= 1;
}

/**
  Find untested but initialized memory regions in GCD map and convert them to be DXE allocatable.

**/
BOOLEAN
PromoteMemoryResource (
  VOID
  )
{
  LIST_ENTRY         *Link;
  EFI_GCD_MAP_ENTRY  *Entry;
  BOOLEAN            Promoted;

  DEBUG ((DEBUG_PAGE, "Promote the memory resource\n"));

  CoreAcquireGcdMemoryLock ();

  Promoted = FALSE;
  Link = mGcdMemorySpaceMap.ForwardLink;
  while (Link != &mGcdMemorySpaceMap) {

    Entry = CR (Link, EFI_GCD_MAP_ENTRY, Link, EFI_GCD_MAP_SIGNATURE);

    if (Entry->GcdMemoryType == EfiGcdMemoryTypeReserved &&
        Entry->EndAddress < MAX_ADDRESS &&
        (Entry->Capabilities & (EFI_MEMORY_PRESENT | EFI_MEMORY_INITIALIZED | EFI_MEMORY_TESTED)) ==
          (EFI_MEMORY_PRESENT | EFI_MEMORY_INITIALIZED)) {
      //
      // Update the GCD map
      //
      if ((Entry->Capabilities & EFI_MEMORY_MORE_RELIABLE) == EFI_MEMORY_MORE_RELIABLE) {
        Entry->GcdMemoryType = EfiGcdMemoryTypeMoreReliable;
      } else {
        Entry->GcdMemoryType = EfiGcdMemoryTypeSystemMemory;
      }
      Entry->Capabilities |= EFI_MEMORY_TESTED;
      Entry->ImageHandle  = gDxeCoreImageHandle;
      Entry->DeviceHandle = NULL;

      //
      // Add to allocable system memory resource
      //

      CoreAddRange (
        EfiConventionalMemory,
        Entry->BaseAddress,
        Entry->EndAddress,
        Entry->Capabilities & ~(EFI_MEMORY_PRESENT | EFI_MEMORY_INITIALIZED | EFI_MEMORY_TESTED | EFI_MEMORY_RUNTIME)
        );
      CoreFreeMemoryMapStack ();

      Promoted = TRUE;
    }

    Link = Link->ForwardLink;
  }

  CoreReleaseGcdMemoryLock ();

  return Promoted;
}
/**
  This function try to allocate Runtime code & Boot time code memory range. If LMFA enabled, 2 patchable PCD 
  PcdLoadFixAddressRuntimeCodePageNumber & PcdLoadFixAddressBootTimeCodePageNumber which are set by tools will record the 
  size of boot time and runtime code.

**/
VOID
CoreLoadingFixedAddressHook (
  VOID
  )
{
   UINT32                     RuntimeCodePageNumber;
   UINT32                     BootTimeCodePageNumber;
   EFI_PHYSICAL_ADDRESS       RuntimeCodeBase;
   EFI_PHYSICAL_ADDRESS       BootTimeCodeBase;
   EFI_STATUS                 Status;

   //
   // Make sure these 2 areas are not initialzied.
   //
   if (!gLoadFixedAddressCodeMemoryReady) {   
     RuntimeCodePageNumber = PcdGet32(PcdLoadFixAddressRuntimeCodePageNumber);
     BootTimeCodePageNumber= PcdGet32(PcdLoadFixAddressBootTimeCodePageNumber);
     RuntimeCodeBase       = (EFI_PHYSICAL_ADDRESS)(gLoadModuleAtFixAddressConfigurationTable.DxeCodeTopAddress - EFI_PAGES_TO_SIZE (RuntimeCodePageNumber));
     BootTimeCodeBase      = (EFI_PHYSICAL_ADDRESS)(RuntimeCodeBase - EFI_PAGES_TO_SIZE (BootTimeCodePageNumber));
     //
     // Try to allocate runtime memory.
     //
     Status = CoreAllocatePages (
                       AllocateAddress,
                       EfiRuntimeServicesCode,
                       RuntimeCodePageNumber,
                       &RuntimeCodeBase
                       );
     if (EFI_ERROR(Status)) {
       //
       // Runtime memory allocation failed 
       //
       return;
     }
     //
     // Try to allocate boot memory.
     //
     Status = CoreAllocatePages (
                       AllocateAddress,
                       EfiBootServicesCode,
                       BootTimeCodePageNumber,
                       &BootTimeCodeBase
                       );
     if (EFI_ERROR(Status)) {
       //
     	 // boot memory allocation failed. Free Runtime code range and will try the allocation again when 
     	 // new memory range is installed.
     	 //
     	 CoreFreePages (
              RuntimeCodeBase,
              RuntimeCodePageNumber
              );
       return;
     }
     gLoadFixedAddressCodeMemoryReady = TRUE;
   } 
   return;
}  

/**
  Called to initialize the memory map and add descriptors to
  the current descriptor list.
  The first descriptor that is added must be general usable
  memory as the addition allocates heap.

  @param  Type                   The type of memory to add
  @param  Start                  The starting address in the memory range Must be
                                 page aligned
  @param  NumberOfPages          The number of pages in the range
  @param  Attribute              Attributes of the memory to add

  @return None.  The range is added to the memory map

**/
VOID
CoreAddMemoryDescriptor (
  IN EFI_MEMORY_TYPE       Type,
  IN EFI_PHYSICAL_ADDRESS  Start,
  IN UINT64                NumberOfPages,
  IN UINT64                Attribute
  )
{
  EFI_PHYSICAL_ADDRESS        End;
  EFI_STATUS                  Status;
  UINTN                       Index;
  UINTN                       FreeIndex;
  
  if ((Start & EFI_PAGE_MASK) != 0) {
    return;
  }

  if (Type >= EfiMaxMemoryType && Type < MEMORY_TYPE_OEM_RESERVED_MIN) {
    return;
  }
  CoreAcquireMemoryLock ();
  End = Start + LShiftU64 (NumberOfPages, EFI_PAGE_SHIFT) - 1;
  CoreAddRange (Type, Start, End, Attribute);
  CoreFreeMemoryMapStack ();
  CoreReleaseMemoryLock ();

  //
  // If Loading Module At Fixed Address feature is enabled. try to allocate memory with Runtime code & Boot time code type
  //
  if (PcdGet64(PcdLoadModuleAtFixAddressEnable) != 0) {
    CoreLoadingFixedAddressHook();
  }
  
  //
  // Check to see if the statistics for the different memory types have already been established
  //
  if (mMemoryTypeInformationInitialized) {
    return;
  }

  
  //
  // Loop through each memory type in the order specified by the gMemoryTypeInformation[] array
  //
  for (Index = 0; gMemoryTypeInformation[Index].Type != EfiMaxMemoryType; Index++) {
    //
    // Make sure the memory type in the gMemoryTypeInformation[] array is valid
    //
    Type = (EFI_MEMORY_TYPE) (gMemoryTypeInformation[Index].Type);
    if ((UINT32)Type > EfiMaxMemoryType) {
      continue;
    }
    if (gMemoryTypeInformation[Index].NumberOfPages != 0) {
      //
      // Allocate pages for the current memory type from the top of available memory
      //
      Status = CoreAllocatePages (
                 AllocateAnyPages,
                 Type,
                 gMemoryTypeInformation[Index].NumberOfPages,
                 &mMemoryTypeStatistics[Type].BaseAddress
                 );
      if (EFI_ERROR (Status)) {
        //
        // If an error occurs allocating the pages for the current memory type, then
        // free all the pages allocates for the previous memory types and return.  This
        // operation with be retied when/if more memory is added to the system
        //
        for (FreeIndex = 0; FreeIndex < Index; FreeIndex++) {
          //
          // Make sure the memory type in the gMemoryTypeInformation[] array is valid
          //
          Type = (EFI_MEMORY_TYPE) (gMemoryTypeInformation[FreeIndex].Type);
          if ((UINT32)Type > EfiMaxMemoryType) {
            continue;
          }

          if (gMemoryTypeInformation[FreeIndex].NumberOfPages != 0) {
            CoreFreePages (
              mMemoryTypeStatistics[Type].BaseAddress,
              gMemoryTypeInformation[FreeIndex].NumberOfPages
              );
            mMemoryTypeStatistics[Type].BaseAddress    = 0;
            mMemoryTypeStatistics[Type].MaximumAddress = MAX_ADDRESS;
          }
        }
        return;
      }

      //
      // Compute the address at the top of the current statistics
      //
      mMemoryTypeStatistics[Type].MaximumAddress =
        mMemoryTypeStatistics[Type].BaseAddress +
        LShiftU64 (gMemoryTypeInformation[Index].NumberOfPages, EFI_PAGE_SHIFT) - 1;

      //
      // If the current base address is the lowest address so far, then update the default
      // maximum address
      //
      if (mMemoryTypeStatistics[Type].BaseAddress < mDefaultMaximumAddress) {
        mDefaultMaximumAddress = mMemoryTypeStatistics[Type].BaseAddress - 1;
      }
    }
  }

  //
  // There was enough system memory for all the the memory types were allocated.  So,
  // those memory areas can be freed for future allocations, and all future memory
  // allocations can occur within their respective bins
  //
  for (Index = 0; gMemoryTypeInformation[Index].Type != EfiMaxMemoryType; Index++) {
    //
    // Make sure the memory type in the gMemoryTypeInformation[] array is valid
    //
    Type = (EFI_MEMORY_TYPE) (gMemoryTypeInformation[Index].Type);
    if ((UINT32)Type > EfiMaxMemoryType) {
      continue;
    }
    if (gMemoryTypeInformation[Index].NumberOfPages != 0) {
      CoreFreePages (
        mMemoryTypeStatistics[Type].BaseAddress,
        gMemoryTypeInformation[Index].NumberOfPages
        );
      mMemoryTypeStatistics[Type].NumberOfPages   = gMemoryTypeInformation[Index].NumberOfPages;
      gMemoryTypeInformation[Index].NumberOfPages = 0;
    }
  }

  //
  // If the number of pages reserved for a memory type is 0, then all allocations for that type
  // should be in the default range.
  //
  for (Type = (EFI_MEMORY_TYPE) 0; Type < EfiMaxMemoryType; Type++) {
    for (Index = 0; gMemoryTypeInformation[Index].Type != EfiMaxMemoryType; Index++) {
      if (Type == (EFI_MEMORY_TYPE)gMemoryTypeInformation[Index].Type) {
        mMemoryTypeStatistics[Type].InformationIndex = Index;
      }
    }
    mMemoryTypeStatistics[Type].CurrentNumberOfPages = 0;
    if (mMemoryTypeStatistics[Type].MaximumAddress == MAX_ADDRESS) {
      mMemoryTypeStatistics[Type].MaximumAddress = mDefaultMaximumAddress;
    }
  }

  mMemoryTypeInformationInitialized = TRUE;
}


/**
  Internal function.  Converts a memory range to the specified type or attributes.
  The range must exist in the memory map.  Either ChangingType or
  ChangingAttributes must be set, but not both.

  @param  Start                  The first address of the range Must be page
                                 aligned
  @param  NumberOfPages          The number of pages to convert
  @param  ChangingType           Boolean indicating that type value should be changed
  @param  NewType                The new type for the memory range
  @param  ChangingAttributes     Boolean indicating that attributes value should be changed
  @param  NewAttributes          The new attributes for the memory range

  @retval EFI_INVALID_PARAMETER  Invalid parameter
  @retval EFI_NOT_FOUND          Could not find a descriptor cover the specified
                                 range  or convertion not allowed.
  @retval EFI_SUCCESS            Successfully converts the memory range to the
                                 specified type.

**/
EFI_STATUS
CoreConvertPagesEx (
  IN UINT64           Start,
  IN UINT64           NumberOfPages,
  IN BOOLEAN          ChangingType,
  IN EFI_MEMORY_TYPE  NewType,
  IN BOOLEAN          ChangingAttributes,
  IN UINT64           NewAttributes
  )
{

  UINT64          NumberOfBytes;
  UINT64          End;
  UINT64          RangeEnd;
  UINT64          Attribute;
  EFI_MEMORY_TYPE MemType;
  LIST_ENTRY      *Link;
  MEMORY_MAP      *Entry;

  Entry = NULL;
  NumberOfBytes = LShiftU64 (NumberOfPages, EFI_PAGE_SHIFT);
  End = Start + NumberOfBytes - 1;

  ASSERT (NumberOfPages);
  ASSERT ((Start & EFI_PAGE_MASK) == 0);
  ASSERT (End > Start) ;
  ASSERT_LOCKED (&gMemoryLock);
  ASSERT ( (ChangingType == FALSE) || (ChangingAttributes == FALSE) );

  if (NumberOfPages == 0 || ((Start & EFI_PAGE_MASK) != 0) || (Start >= End)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Convert the entire range
  //

  while (Start < End) {

    //
    // Find the entry that the covers the range
    //
    for (Link = gMemoryMap.ForwardLink; Link != &gMemoryMap; Link = Link->ForwardLink) {
      Entry = CR (Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);

      if (Entry->Start <= Start && Entry->End > Start) {
        break;
      }
    }

    if (Link == &gMemoryMap) {
      DEBUG ((DEBUG_ERROR | DEBUG_PAGE, "ConvertPages: failed to find range %lx - %lx\n", Start, End));
      return EFI_NOT_FOUND;
    }

    //
    // Convert range to the end, or to the end of the descriptor
    // if that's all we've got
    //
    RangeEnd = End;

    ASSERT (Entry != NULL);
    if (Entry->End < End) {
      RangeEnd = Entry->End;
    }

    if (ChangingType) {
      DEBUG ((DEBUG_PAGE, "ConvertRange: %lx-%lx to type %d\n", Start, RangeEnd, NewType));
    }
    if (ChangingAttributes) {
      DEBUG ((DEBUG_PAGE, "ConvertRange: %lx-%lx to attr %lx\n", Start, RangeEnd, NewAttributes));
    }

    if (ChangingType) {
      //
      // Debug code - verify conversion is allowed
      //
      if (!(NewType == EfiConventionalMemory ? 1 : 0) ^ (Entry->Type == EfiConventionalMemory ? 1 : 0)) {
        DEBUG ((DEBUG_ERROR | DEBUG_PAGE, "ConvertPages: Incompatible memory types\n"));
        return EFI_NOT_FOUND;
      }

      //
      // Update counters for the number of pages allocated to each memory type
      //
      if ((UINT32)Entry->Type < EfiMaxMemoryType) {
        if ((Start >= mMemoryTypeStatistics[Entry->Type].BaseAddress && Start <= mMemoryTypeStatistics[Entry->Type].MaximumAddress) ||
            (Start >= mDefaultBaseAddress && Start <= mDefaultMaximumAddress)                                                          ) {
          if (NumberOfPages > mMemoryTypeStatistics[Entry->Type].CurrentNumberOfPages) {
            mMemoryTypeStatistics[Entry->Type].CurrentNumberOfPages = 0;
          } else {
            mMemoryTypeStatistics[Entry->Type].CurrentNumberOfPages -= NumberOfPages;
          }
        }
      }

      if ((UINT32)NewType < EfiMaxMemoryType) {
        if ((Start >= mMemoryTypeStatistics[NewType].BaseAddress && Start <= mMemoryTypeStatistics[NewType].MaximumAddress) ||
            (Start >= mDefaultBaseAddress && Start <= mDefaultMaximumAddress)                                                  ) {
          mMemoryTypeStatistics[NewType].CurrentNumberOfPages += NumberOfPages;
          if (mMemoryTypeStatistics[NewType].CurrentNumberOfPages > gMemoryTypeInformation[mMemoryTypeStatistics[NewType].InformationIndex].NumberOfPages) {
            gMemoryTypeInformation[mMemoryTypeStatistics[NewType].InformationIndex].NumberOfPages = (UINT32)mMemoryTypeStatistics[NewType].CurrentNumberOfPages;
          }
        }
      }
    }

    //
    // Pull range out of descriptor
    //
    if (Entry->Start == Start) {

      //
      // Clip start
      //
      Entry->Start = RangeEnd + 1;

    } else if (Entry->End == RangeEnd) {

      //
      // Clip end
      //
      Entry->End = Start - 1;

    } else {

      //
      // Pull it out of the center, clip current
      //

      //
      // Add a new one
      //
      mMapStack[mMapDepth].Signature = MEMORY_MAP_SIGNATURE;
      mMapStack[mMapDepth].FromPages  = FALSE;
      mMapStack[mMapDepth].Type      = Entry->Type;
      mMapStack[mMapDepth].Start     = RangeEnd+1;
      mMapStack[mMapDepth].End       = Entry->End;

      //
      // Inherit Attribute from the Memory Descriptor that is being clipped
      //
      mMapStack[mMapDepth].Attribute = Entry->Attribute;

      Entry->End = Start - 1;
      ASSERT (Entry->Start < Entry->End);

      Entry = &mMapStack[mMapDepth];
      InsertTailList (&gMemoryMap, &Entry->Link);

      mMapDepth += 1;
      ASSERT (mMapDepth < MAX_MAP_DEPTH);
    }

    //
    // The new range inherits the same Attribute as the Entry
    // it is being cut out of unless attributes are being changed
    //
    if (ChangingType) {
      Attribute = Entry->Attribute;
      MemType = NewType;
    } else {
      Attribute = NewAttributes;
      MemType = Entry->Type;
    }

    //
    // If the descriptor is empty, then remove it from the map
    //
    if (Entry->Start == Entry->End + 1) {
      RemoveMemoryMapEntry (Entry);
      Entry = NULL;
    }

    //
    // Add our new range in
    //
    CoreAddRange (MemType, Start, RangeEnd, Attribute);
    if (ChangingType && (MemType == EfiConventionalMemory)) {
      //
      // Avoid calling DEBUG_CLEAR_MEMORY() for an address of 0 because this
      // macro will ASSERT() if address is 0.  Instead, CoreAddRange() guarantees
      // that the page starting at address 0 is always filled with zeros.
      //
      if (Start == 0) {
        if (RangeEnd > EFI_PAGE_SIZE) {
          DEBUG_CLEAR_MEMORY ((VOID *)(UINTN) EFI_PAGE_SIZE, (UINTN) (RangeEnd - EFI_PAGE_SIZE + 1));
        }
      } else {
        DEBUG_CLEAR_MEMORY ((VOID *)(UINTN) Start, (UINTN) (RangeEnd - Start + 1));
      }
    }

    //
    // Move any map descriptor stack to general pool
    //
    CoreFreeMemoryMapStack ();

    //
    // Bump the starting address, and convert the next range
    //
    Start = RangeEnd + 1;
  }

  //
  // Converted the whole range, done
  //

  return EFI_SUCCESS;
}


/**
  Internal function.  Converts a memory range to the specified type.
  The range must exist in the memory map.

  @param  Start                  The first address of the range Must be page
                                 aligned
  @param  NumberOfPages          The number of pages to convert
  @param  NewType                The new type for the memory range

  @retval EFI_INVALID_PARAMETER  Invalid parameter
  @retval EFI_NOT_FOUND          Could not find a descriptor cover the specified
                                 range  or convertion not allowed.
  @retval EFI_SUCCESS            Successfully converts the memory range to the
                                 specified type.

**/
EFI_STATUS
CoreConvertPages (
  IN UINT64           Start,
  IN UINT64           NumberOfPages,
  IN EFI_MEMORY_TYPE  NewType
  )
{
  return CoreConvertPagesEx(Start, NumberOfPages, TRUE, NewType, FALSE, 0);
}


/**
  Internal function.  Converts a memory range to use new attributes.

  @param  Start                  The first address of the range Must be page
                                 aligned
  @param  NumberOfPages          The number of pages to convert
  @param  NewAttributes          The new attributes value for the range.

**/
VOID
CoreUpdateMemoryAttributes (
  IN EFI_PHYSICAL_ADDRESS  Start,
  IN UINT64                NumberOfPages,
  IN UINT64                NewAttributes
  )
{
  CoreAcquireMemoryLock ();

  //
  // Update the attributes to the new value
  //
  CoreConvertPagesEx(Start, NumberOfPages, FALSE, (EFI_MEMORY_TYPE)0, TRUE, NewAttributes);

  CoreReleaseMemoryLock ();
}


/**
  Internal function. Finds a consecutive free page range below
  the requested address.

  @param  MaxAddress             The address that the range must be below
  @param  MinAddress             The address that the range must be above
  @param  NumberOfPages          Number of pages needed
  @param  NewType                The type of memory the range is going to be
                                 turned into
  @param  Alignment              Bits to align with

  @return The base address of the range, or 0 if the range was not found

**/
UINT64
CoreFindFreePagesI (
  IN UINT64           MaxAddress,
  IN UINT64           MinAddress,
  IN UINT64           NumberOfPages,
  IN EFI_MEMORY_TYPE  NewType,
  IN UINTN            Alignment
  )
{
  UINT64          NumberOfBytes;
  UINT64          Target;
  UINT64          DescStart;
  UINT64          DescEnd;
  UINT64          DescNumberOfBytes;
  LIST_ENTRY      *Link;
  MEMORY_MAP      *Entry;

  if ((MaxAddress < EFI_PAGE_MASK) ||(NumberOfPages == 0)) {
    return 0;
  }

  if ((MaxAddress & EFI_PAGE_MASK) != EFI_PAGE_MASK) {

    //
    // If MaxAddress is not aligned to the end of a page
    //

    //
    // Change MaxAddress to be 1 page lower
    //
    MaxAddress -= (EFI_PAGE_MASK + 1);

    //
    // Set MaxAddress to a page boundary
    //
    MaxAddress &= ~(UINT64)EFI_PAGE_MASK;

    //
    // Set MaxAddress to end of the page
    //
    MaxAddress |= EFI_PAGE_MASK;
  }

  NumberOfBytes = LShiftU64 (NumberOfPages, EFI_PAGE_SHIFT);
  Target = 0;

  for (Link = gMemoryMap.ForwardLink; Link != &gMemoryMap; Link = Link->ForwardLink) {
    Entry = CR (Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);

    //
    // If it's not a free entry, don't bother with it
    //
    if (Entry->Type != EfiConventionalMemory) {
      continue;
    }

    DescStart = Entry->Start;
    DescEnd = Entry->End;

    //
    // If desc is past max allowed address or below min allowed address, skip it
    //
    if ((DescStart >= MaxAddress) || (DescEnd < MinAddress)) {
      continue;
    }

    //
    // If desc ends past max allowed address, clip the end
    //
    if (DescEnd >= MaxAddress) {
      DescEnd = MaxAddress;
    }

    DescEnd = ((DescEnd + 1) & (~(Alignment - 1))) - 1;

    // Skip if DescEnd is less than DescStart after alignment clipping
    if (DescEnd < DescStart) {
      continue;
    }

    //
    // Compute the number of bytes we can used from this
    // descriptor, and see it's enough to satisfy the request
    //
    DescNumberOfBytes = DescEnd - DescStart + 1;

    if (DescNumberOfBytes >= NumberOfBytes) {
      //
      // If the start of the allocated range is below the min address allowed, skip it
      //
      if ((DescEnd - NumberOfBytes + 1) < MinAddress) {
        continue;
      }

      //
      // If this is the best match so far remember it
      //
      if (DescEnd > Target) {
        Target = DescEnd;
      }
    }
  }

  //
  // If this is a grow down, adjust target to be the allocation base
  //
  Target -= NumberOfBytes - 1;

  //
  // If we didn't find a match, return 0
  //
  if ((Target & EFI_PAGE_MASK) != 0) {
    return 0;
  }

  return Target;
}


/**
  Internal function.  Finds a consecutive free page range below
  the requested address

  @param  MaxAddress             The address that the range must be below
  @param  NoPages                Number of pages needed
  @param  NewType                The type of memory the range is going to be
                                 turned into
  @param  Alignment              Bits to align with

  @return The base address of the range, or 0 if the range was not found.

**/
UINT64
FindFreePages (
    IN UINT64           MaxAddress,
    IN UINT64           NoPages,
    IN EFI_MEMORY_TYPE  NewType,
    IN UINTN            Alignment
    )
{
  UINT64   Start;

  //
  // Attempt to find free pages in the preferred bin based on the requested memory type
  //
  if ((UINT32)NewType < EfiMaxMemoryType && MaxAddress >= mMemoryTypeStatistics[NewType].MaximumAddress) {
    Start = CoreFindFreePagesI (
              mMemoryTypeStatistics[NewType].MaximumAddress, 
              mMemoryTypeStatistics[NewType].BaseAddress, 
              NoPages, 
              NewType, 
              Alignment
              );
    if (Start != 0) {
      return Start;
    }
  }

  //
  // Attempt to find free pages in the default allocation bin
  //
  if (MaxAddress >= mDefaultMaximumAddress) {
    Start = CoreFindFreePagesI (mDefaultMaximumAddress, 0, NoPages, NewType, Alignment);
    if (Start != 0) {
      if (Start < mDefaultBaseAddress) {
        mDefaultBaseAddress = Start;
      }
      return Start;
    }
  }

  //
  // The allocation did not succeed in any of the prefered bins even after 
  // promoting resources. Attempt to find free pages anywhere is the requested 
  // address range.  If this allocation fails, then there are not enough 
  // resources anywhere to satisfy the request.
  //
  Start = CoreFindFreePagesI (MaxAddress, 0, NoPages, NewType, Alignment);
  if (Start != 0) {
    return Start;
  }

  //
  // If allocations from the preferred bins fail, then attempt to promote memory resources.
  //
  if (!PromoteMemoryResource ()) {
    return 0;
  }

  //
  // If any memory resources were promoted, then re-attempt the allocation
  //
  return FindFreePages (MaxAddress, NoPages, NewType, Alignment);
}


/**
  Allocates pages from the memory map.

  @param  Type                   The type of allocation to perform
  @param  MemoryType             The type of memory to turn the allocated pages
                                 into
  @param  NumberOfPages          The number of pages to allocate
  @param  Memory                 A pointer to receive the base allocated memory
                                 address

  @return Status. On success, Memory is filled in with the base address allocated
  @retval EFI_INVALID_PARAMETER  Parameters violate checking rules defined in
                                 spec.
  @retval EFI_NOT_FOUND          Could not allocate pages match the requirement.
  @retval EFI_OUT_OF_RESOURCES   No enough pages to allocate.
  @retval EFI_SUCCESS            Pages successfully allocated.

**/
EFI_STATUS
EFIAPI
CoreInternalAllocatePages (
  IN EFI_ALLOCATE_TYPE      Type,
  IN EFI_MEMORY_TYPE        MemoryType,
  IN UINTN                  NumberOfPages,
  IN OUT EFI_PHYSICAL_ADDRESS  *Memory,
  IN BOOLEAN                NeedGuard
  )
{
  EFI_STATUS      Status;
  UINT64          Start;
  UINT64          NumberOfBytes;
  UINT64          End;
  UINT64          MaxAddress;
  UINTN           Alignment;

  if ((UINT32)Type >= MaxAllocateType) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MemoryType >= EfiMaxMemoryType && MemoryType < MEMORY_TYPE_OEM_RESERVED_MIN) ||
       (MemoryType == EfiConventionalMemory) || (MemoryType == EfiPersistentMemory)) {
    return EFI_INVALID_PARAMETER;
  }

  if (Memory == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Alignment = EFI_DEFAULT_PAGE_ALLOCATION_ALIGNMENT;

  if  (MemoryType == EfiACPIReclaimMemory   ||
       MemoryType == EfiACPIMemoryNVS       ||
       MemoryType == EfiRuntimeServicesCode ||
       MemoryType == EfiRuntimeServicesData) {

    Alignment = EFI_ACPI_RUNTIME_PAGE_ALLOCATION_ALIGNMENT;
  }

  if (Type == AllocateAddress) {
    if ((*Memory & (Alignment - 1)) != 0) {
      return EFI_NOT_FOUND;
    }
  }

  NumberOfPages += EFI_SIZE_TO_PAGES (Alignment) - 1;
  NumberOfPages &= ~(EFI_SIZE_TO_PAGES (Alignment) - 1);

  //
  // If this is for below a particular address, then
  //
  Start = *Memory;

  //
  // The max address is the max natively addressable address for the processor
  //
  MaxAddress = MAX_ADDRESS;

  //
  // Check for Type AllocateAddress,
  // if NumberOfPages is 0 or
  // if (NumberOfPages << EFI_PAGE_SHIFT) is above MAX_ADDRESS or
  // if (Start + NumberOfBytes) rolls over 0 or
  // if Start is above MAX_ADDRESS or
  // if End is above MAX_ADDRESS,
  // return EFI_NOT_FOUND.
  //
  if (Type == AllocateAddress) {
    if ((NumberOfPages == 0) ||
        (NumberOfPages > RShiftU64 (MaxAddress, EFI_PAGE_SHIFT))) {
      return EFI_NOT_FOUND;
    }
    NumberOfBytes = LShiftU64 (NumberOfPages, EFI_PAGE_SHIFT);
    End = Start + NumberOfBytes - 1;

    if ((Start >= End) ||
        (Start > MaxAddress) || 
        (End > MaxAddress)) {
      return EFI_NOT_FOUND;
    }
  }

  if (Type == AllocateMaxAddress) {
    MaxAddress = Start;
  }

  if (FeaturePcdGet(PcdHeapPageGuard) && NeedGuard) {
    NumberOfPages += 2;
  }

  CoreAcquireMemoryLock ();

  //
  // If not a specific address, then find an address to allocate
  //
  if (Type != AllocateAddress) {
    Start = FindFreePages (MaxAddress, NumberOfPages, MemoryType, Alignment);
    if (Start == 0) {
      Status = EFI_OUT_OF_RESOURCES;
      goto Done;
    }
  }

  //
  // Convert pages from FreeMemory to the requested type
  //
  Status = CoreConvertPages (Start, NumberOfPages, MemoryType);

Done:
  if (FeaturePcdGet(PcdHeapPageGuard) && NeedGuard && (!EFI_ERROR (Status))) {
    Start += EFI_PAGES_TO_SIZE(1);
    SetGuardPageOnAllocatePages (Start, NumberOfPages - 2);
  }

  CoreReleaseMemoryLock ();

  if (!EFI_ERROR (Status)) {
    *Memory = Start;
  }

  return Status;
}

VOID *
EFIAPI
AllocatePagesForGuard (
  IN UINTN  Pages
  )
{
  EFI_PHYSICAL_ADDRESS Memory;
  EFI_STATUS           Status;

  Status = CoreInternalAllocatePages (AllocateAnyPages, EfiBootServicesData, Pages, &Memory, FALSE);
  if (!EFI_ERROR (Status)) {
    return (VOID *)(UINTN)Memory;
  }
  return NULL;
}

/**
  Allocates pages from the memory map.

  @param  Type                   The type of allocation to perform
  @param  MemoryType             The type of memory to turn the allocated pages
                                 into
  @param  NumberOfPages          The number of pages to allocate
  @param  Memory                 A pointer to receive the base allocated memory
                                 address

  @return Status. On success, Memory is filled in with the base address allocated
  @retval EFI_INVALID_PARAMETER  Parameters violate checking rules defined in
                                 spec.
  @retval EFI_NOT_FOUND          Could not allocate pages match the requirement.
  @retval EFI_OUT_OF_RESOURCES   No enough pages to allocate.
  @retval EFI_SUCCESS            Pages successfully allocated.

**/
EFI_STATUS
EFIAPI
CoreAllocatePages (
  IN  EFI_ALLOCATE_TYPE     Type,
  IN  EFI_MEMORY_TYPE       MemoryType,
  IN  UINTN                 NumberOfPages,
  OUT EFI_PHYSICAL_ADDRESS  *Memory
  )
{
  EFI_STATUS  Status;
  BOOLEAN               NeedGuard;
  EFI_PHYSICAL_ADDRESS  BasePage;

  NeedGuard = FALSE;
  if (FeaturePcdGet(PcdHeapPageGuard)) {
    CheckGuardPages();
    if (IsAllocateTypeForHeapGuard(Type) && IsMemoryTypeForHeapGuard(MemoryType)) {
      NeedGuard = TRUE;
    }
  }

  Status = CoreInternalAllocatePages (Type, MemoryType, NumberOfPages, Memory, NeedGuard);
  if (!EFI_ERROR (Status)) {
    CoreUpdateProfile (
      (EFI_PHYSICAL_ADDRESS) (UINTN) RETURN_ADDRESS (0),
      MemoryProfileActionAllocatePages,
      MemoryType,
      EFI_PAGES_TO_SIZE (NumberOfPages),
      (VOID *) (UINTN) *Memory,
      NULL
      );
    InstallMemoryAttributesTableOnMemoryAllocation (MemoryType);
    if (FeaturePcdGet(PcdHeapPageGuard) && NeedGuard) {
      // we must defer heap guard here to avoid allocation re-entry issue.
      BasePage = *Memory - EFI_PAGES_TO_SIZE(1);
      SetMemoryPageAttributes (
        NULL,
        BasePage,
        EFI_PAGES_TO_SIZE(1),
        EFI_MEMORY_RP,
        AllocatePagesForGuard
        );
      BasePage = *Memory + EFI_PAGES_TO_SIZE(NumberOfPages);
      SetMemoryPageAttributes (
        NULL,
        BasePage,
        EFI_PAGES_TO_SIZE(1),
        EFI_MEMORY_RP,
        AllocatePagesForGuard
        );
    }
  }
  return Status;
}

/**
  Frees previous allocated pages.

  @param  Memory                 Base address of memory being freed
  @param  NumberOfPages          The number of pages to free
  @param  MemoryType             Pointer to memory type

  @retval EFI_NOT_FOUND          Could not find the entry that covers the range
  @retval EFI_INVALID_PARAMETER  Address not aligned
  @return EFI_SUCCESS         -Pages successfully freed.

**/
EFI_STATUS
EFIAPI
CoreInternalFreePages (
  IN EFI_PHYSICAL_ADDRESS   Memory,
  IN UINTN                  NumberOfPages,
  OUT EFI_MEMORY_TYPE       *MemoryType OPTIONAL,
  OUT GUARD_OPERATION       *GuardOperation OPTIONAL
  )
{
  EFI_STATUS      Status;
  LIST_ENTRY      *Link;
  MEMORY_MAP      *Entry;
  UINTN           Alignment;
  BOOLEAN         IsGuarded;

  IsGuarded = FALSE;

  //
  // Free the range
  //
  CoreAcquireMemoryLock ();

  //
  // Find the entry that the covers the range
  //
  Entry = NULL;
  for (Link = gMemoryMap.ForwardLink; Link != &gMemoryMap; Link = Link->ForwardLink) {
    Entry = CR(Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);
    if (Entry->Start <= Memory && Entry->End > Memory) {
        break;
    }
  }
  if (Link == &gMemoryMap) {
    Status = EFI_NOT_FOUND;
    goto Done;
  }

  Alignment = EFI_DEFAULT_PAGE_ALLOCATION_ALIGNMENT;

  ASSERT (Entry != NULL);
  if  (Entry->Type == EfiACPIReclaimMemory   ||
       Entry->Type == EfiACPIMemoryNVS       ||
       Entry->Type == EfiRuntimeServicesCode ||
       Entry->Type == EfiRuntimeServicesData) {

    Alignment = EFI_ACPI_RUNTIME_PAGE_ALLOCATION_ALIGNMENT;

  }

  if (FeaturePcdGet(PcdHeapPageGuard)) {
    if (IsMemoryTypeForHeapGuard(Entry->Type)) {
      IsGuarded = TRUE;
    }
  }

  if ((Memory & (Alignment - 1)) != 0) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  NumberOfPages += EFI_SIZE_TO_PAGES (Alignment) - 1;
  NumberOfPages &= ~(EFI_SIZE_TO_PAGES (Alignment) - 1);

  if (MemoryType != NULL) {
    *MemoryType = Entry->Type;
  }

  Status = CoreConvertPages (Memory, NumberOfPages, EfiConventionalMemory);

  if (EFI_ERROR (Status)) {
    goto Done;
  }

Done:
  if (FeaturePcdGet(PcdHeapPageGuard)) {
    if (!EFI_ERROR (Status) && IsGuarded) {
      ClearGuardPageOnFreePages (Memory, NumberOfPages, GuardOperation);
    }
  }
  CoreReleaseMemoryLock ();
  return Status;
}

/**
  Frees previous allocated pages.

  @param  Memory                 Base address of memory being freed
  @param  NumberOfPages          The number of pages to free

  @retval EFI_NOT_FOUND          Could not find the entry that covers the range
  @retval EFI_INVALID_PARAMETER  Address not aligned
  @return EFI_SUCCESS         -Pages successfully freed.

**/
EFI_STATUS
EFIAPI
CoreFreePages (
  IN EFI_PHYSICAL_ADDRESS  Memory,
  IN UINTN                 NumberOfPages
  )
{
  EFI_STATUS        Status;
  EFI_MEMORY_TYPE   MemoryType;
  GUARD_OPERATION       GuardOperation[4];
  UINTN                 Index;
  EFI_PHYSICAL_ADDRESS  BasePage;

  if (FeaturePcdGet(PcdHeapPageGuard)) {
    CheckGuardPages();
  }

  Status = CoreInternalFreePages (Memory, NumberOfPages, &MemoryType, GuardOperation);
  if (!EFI_ERROR (Status)) {
    CoreUpdateProfile (
      (EFI_PHYSICAL_ADDRESS) (UINTN) RETURN_ADDRESS (0),
      MemoryProfileActionFreePages,
      MemoryType,
      EFI_PAGES_TO_SIZE (NumberOfPages),
      (VOID *) (UINTN) Memory,
      NULL
      );
    InstallMemoryAttributesTableOnMemoryAllocation (MemoryType);
    if (FeaturePcdGet(PcdHeapPageGuard)) {
      // we must defer heap guard here to avoid allocation re-entry issue.
      if (IsMemoryTypeForHeapGuard(MemoryType)) {
        for (Index = 0; Index < 4; Index++) {
          switch(Index) {
          case 0:
            BasePage = Memory - EFI_PAGES_TO_SIZE(1);
            break;
          case 1:
            BasePage = Memory;
            break;
          case 2:
            BasePage = Memory + EFI_PAGES_TO_SIZE(NumberOfPages - 1);
            break;
          case 3:
            BasePage = Memory + EFI_PAGES_TO_SIZE(NumberOfPages);
            break;
          }
          switch(GuardOperation[Index]) {
          case GuardOperationSetGuard:
            SetMemoryPageAttributes (
              NULL,
              BasePage,
              EFI_PAGES_TO_SIZE(1),
              EFI_MEMORY_RP,
              AllocatePagesForGuard
              );
            break;
          case GuardOperationClearGuard:
            ClearMemoryPageAttributes (
              NULL,
              BasePage,
              EFI_PAGES_TO_SIZE(1),
              EFI_MEMORY_RP,
              AllocatePagesForGuard
              );
            break;
          case GuardOperationNoAction:
            break;
          default:
            ASSERT(FALSE);
            break;
          }
        }
      }
    }
  }
  return Status;
}

/**
  This function checks to see if the last memory map descriptor in a memory map
  can be merged with any of the other memory map descriptors in a memorymap.
  Memory descriptors may be merged if they are adjacent and have the same type
  and attributes.

  @param  MemoryMap              A pointer to the start of the memory map.
  @param  MemoryMapDescriptor    A pointer to the last descriptor in MemoryMap.
  @param  DescriptorSize         The size, in bytes, of an individual
                                 EFI_MEMORY_DESCRIPTOR.

  @return  A pointer to the next available descriptor in MemoryMap

**/
EFI_MEMORY_DESCRIPTOR *
MergeMemoryMapDescriptor (
  IN EFI_MEMORY_DESCRIPTOR  *MemoryMap,
  IN EFI_MEMORY_DESCRIPTOR  *MemoryMapDescriptor,
  IN UINTN                  DescriptorSize
  )
{
  //
  // Traverse the array of descriptors in MemoryMap
  //
  for (; MemoryMap != MemoryMapDescriptor; MemoryMap = NEXT_MEMORY_DESCRIPTOR (MemoryMap, DescriptorSize)) {
    //
    // Check to see if the Type fields are identical.
    //
    if (MemoryMap->Type != MemoryMapDescriptor->Type) {
      continue;
    }

    //
    // Check to see if the Attribute fields are identical.
    //
    if (MemoryMap->Attribute != MemoryMapDescriptor->Attribute) {
      continue;
    }

    //
    // Check to see if MemoryMapDescriptor is immediately above MemoryMap
    //
    if (MemoryMap->PhysicalStart + EFI_PAGES_TO_SIZE ((UINTN)MemoryMap->NumberOfPages) == MemoryMapDescriptor->PhysicalStart) { 
      //
      // Merge MemoryMapDescriptor into MemoryMap
      //
      MemoryMap->NumberOfPages += MemoryMapDescriptor->NumberOfPages;

      //
      // Return MemoryMapDescriptor as the next available slot int he MemoryMap array
      //
      return MemoryMapDescriptor;
    }

    //
    // Check to see if MemoryMapDescriptor is immediately below MemoryMap
    //
    if (MemoryMap->PhysicalStart - EFI_PAGES_TO_SIZE ((UINTN)MemoryMapDescriptor->NumberOfPages) == MemoryMapDescriptor->PhysicalStart) {
      //
      // Merge MemoryMapDescriptor into MemoryMap
      //
      MemoryMap->PhysicalStart  = MemoryMapDescriptor->PhysicalStart;
      MemoryMap->VirtualStart   = MemoryMapDescriptor->VirtualStart;
      MemoryMap->NumberOfPages += MemoryMapDescriptor->NumberOfPages;

      //
      // Return MemoryMapDescriptor as the next available slot int he MemoryMap array
      //
      return MemoryMapDescriptor;
    }
  }

  //
  // MemoryMapDescrtiptor could not be merged with any descriptors in MemoryMap.
  //
  // Return the slot immediately after MemoryMapDescriptor as the next available 
  // slot in the MemoryMap array
  //
  return NEXT_MEMORY_DESCRIPTOR (MemoryMapDescriptor, DescriptorSize);
}

/**
  This function returns a copy of the current memory map. The map is an array of
  memory descriptors, each of which describes a contiguous block of memory.

  @param  MemoryMapSize          A pointer to the size, in bytes, of the
                                 MemoryMap buffer. On input, this is the size of
                                 the buffer allocated by the caller.  On output,
                                 it is the size of the buffer returned by the
                                 firmware  if the buffer was large enough, or the
                                 size of the buffer needed  to contain the map if
                                 the buffer was too small.
  @param  MemoryMap              A pointer to the buffer in which firmware places
                                 the current memory map.
  @param  MapKey                 A pointer to the location in which firmware
                                 returns the key for the current memory map.
  @param  DescriptorSize         A pointer to the location in which firmware
                                 returns the size, in bytes, of an individual
                                 EFI_MEMORY_DESCRIPTOR.
  @param  DescriptorVersion      A pointer to the location in which firmware
                                 returns the version number associated with the
                                 EFI_MEMORY_DESCRIPTOR.

  @retval EFI_SUCCESS            The memory map was returned in the MemoryMap
                                 buffer.
  @retval EFI_BUFFER_TOO_SMALL   The MemoryMap buffer was too small. The current
                                 buffer size needed to hold the memory map is
                                 returned in MemoryMapSize.
  @retval EFI_INVALID_PARAMETER  One of the parameters has an invalid value.

**/
EFI_STATUS
EFIAPI
CoreGetMemoryMap (
  IN OUT UINTN                  *MemoryMapSize,
  IN OUT EFI_MEMORY_DESCRIPTOR  *MemoryMap,
  OUT UINTN                     *MapKey,
  OUT UINTN                     *DescriptorSize,
  OUT UINT32                    *DescriptorVersion
  )
{
  EFI_STATUS                        Status;
  UINTN                             Size;
  UINTN                             BufferSize;
  UINTN                             NumberOfEntries;
  LIST_ENTRY                        *Link;
  MEMORY_MAP                        *Entry;
  EFI_GCD_MAP_ENTRY                 *GcdMapEntry;
  EFI_GCD_MAP_ENTRY                 MergeGcdMapEntry;
  EFI_MEMORY_TYPE                   Type;
  EFI_MEMORY_DESCRIPTOR             *MemoryMapStart;

  //
  // Make sure the parameters are valid
  //
  if (MemoryMapSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CoreAcquireGcdMemoryLock ();

  //
  // Count the number of Reserved and runtime MMIO entries
  // And, count the number of Persistent entries.
  //
  NumberOfEntries = 0;
  for (Link = mGcdMemorySpaceMap.ForwardLink; Link != &mGcdMemorySpaceMap; Link = Link->ForwardLink) {
    GcdMapEntry = CR (Link, EFI_GCD_MAP_ENTRY, Link, EFI_GCD_MAP_SIGNATURE);
    if ((GcdMapEntry->GcdMemoryType == EfiGcdMemoryTypePersistentMemory) || 
        (GcdMapEntry->GcdMemoryType == EfiGcdMemoryTypeReserved) ||
        ((GcdMapEntry->GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) &&
        ((GcdMapEntry->Attributes & EFI_MEMORY_RUNTIME) == EFI_MEMORY_RUNTIME))) {
      NumberOfEntries ++;
    }
  }

  Size = sizeof (EFI_MEMORY_DESCRIPTOR);

  //
  // Make sure Size != sizeof(EFI_MEMORY_DESCRIPTOR). This will
  // prevent people from having pointer math bugs in their code.
  // now you have to use *DescriptorSize to make things work.
  //
  Size += sizeof(UINT64) - (Size % sizeof (UINT64));

  if (DescriptorSize != NULL) {
    *DescriptorSize = Size;
  }

  if (DescriptorVersion != NULL) {
    *DescriptorVersion = EFI_MEMORY_DESCRIPTOR_VERSION;
  }

  CoreAcquireMemoryLock ();

  //
  // Compute the buffer size needed to fit the entire map
  //
  BufferSize = Size * NumberOfEntries;
  for (Link = gMemoryMap.ForwardLink; Link != &gMemoryMap; Link = Link->ForwardLink) {
    BufferSize += Size;
  }

  if (*MemoryMapSize < BufferSize) {
    Status = EFI_BUFFER_TOO_SMALL;
    goto Done;
  }

  if (MemoryMap == NULL) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  //
  // Build the map
  //
  ZeroMem (MemoryMap, BufferSize);
  MemoryMapStart = MemoryMap;
  for (Link = gMemoryMap.ForwardLink; Link != &gMemoryMap; Link = Link->ForwardLink) {
    Entry = CR (Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);
    ASSERT (Entry->VirtualStart == 0);

    //
    // Convert internal map into an EFI_MEMORY_DESCRIPTOR
    //
    MemoryMap->Type           = Entry->Type;
    MemoryMap->PhysicalStart  = Entry->Start;
    MemoryMap->VirtualStart   = Entry->VirtualStart;
    MemoryMap->NumberOfPages  = RShiftU64 (Entry->End - Entry->Start + 1, EFI_PAGE_SHIFT);
    //
    // If the memory type is EfiConventionalMemory, then determine if the range is part of a
    // memory type bin and needs to be converted to the same memory type as the rest of the
    // memory type bin in order to minimize EFI Memory Map changes across reboots.  This
    // improves the chances for a successful S4 resume in the presence of minor page allocation
    // differences across reboots.
    //
    if (MemoryMap->Type == EfiConventionalMemory) {
      for (Type = (EFI_MEMORY_TYPE) 0; Type < EfiMaxMemoryType; Type++) {
        if (mMemoryTypeStatistics[Type].Special                        &&
            mMemoryTypeStatistics[Type].NumberOfPages > 0              &&
            Entry->Start >= mMemoryTypeStatistics[Type].BaseAddress    &&
            Entry->End   <= mMemoryTypeStatistics[Type].MaximumAddress) {
          MemoryMap->Type = Type;
        }
      }
    }
    MemoryMap->Attribute = Entry->Attribute;
    if (MemoryMap->Type < EfiMaxMemoryType) {
      if (mMemoryTypeStatistics[MemoryMap->Type].Runtime) {
        MemoryMap->Attribute |= EFI_MEMORY_RUNTIME;
      }
    }

    //
    // Check to see if the new Memory Map Descriptor can be merged with an 
    // existing descriptor if they are adjacent and have the same attributes
    //
    MemoryMap = MergeMemoryMapDescriptor (MemoryMapStart, MemoryMap, Size);
  }

 
  ZeroMem (&MergeGcdMapEntry, sizeof (MergeGcdMapEntry));
  GcdMapEntry = NULL;
  for (Link = mGcdMemorySpaceMap.ForwardLink; ; Link = Link->ForwardLink) {
    if (Link != &mGcdMemorySpaceMap) {
      //
      // Merge adjacent same type and attribute GCD memory range
      //
      GcdMapEntry = CR (Link, EFI_GCD_MAP_ENTRY, Link, EFI_GCD_MAP_SIGNATURE);
  
      if ((MergeGcdMapEntry.Capabilities == GcdMapEntry->Capabilities) && 
          (MergeGcdMapEntry.Attributes == GcdMapEntry->Attributes) &&
          (MergeGcdMapEntry.GcdMemoryType == GcdMapEntry->GcdMemoryType) &&
          (MergeGcdMapEntry.GcdIoType == GcdMapEntry->GcdIoType)) {
        MergeGcdMapEntry.EndAddress  = GcdMapEntry->EndAddress;
        continue;
      }
    }

    if ((MergeGcdMapEntry.GcdMemoryType == EfiGcdMemoryTypeReserved) ||
        ((MergeGcdMapEntry.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) &&
        ((MergeGcdMapEntry.Attributes & EFI_MEMORY_RUNTIME) == EFI_MEMORY_RUNTIME))) {
      //
      // Page Align GCD range is required. When it is converted to EFI_MEMORY_DESCRIPTOR, 
      // it will be recorded as page PhysicalStart and NumberOfPages. 
      //
      ASSERT ((MergeGcdMapEntry.BaseAddress & EFI_PAGE_MASK) == 0);
      ASSERT (((MergeGcdMapEntry.EndAddress - MergeGcdMapEntry.BaseAddress + 1) & EFI_PAGE_MASK) == 0);
      
      // 
      // Create EFI_MEMORY_DESCRIPTOR for every Reserved and runtime MMIO GCD entries
      //
      MemoryMap->PhysicalStart = MergeGcdMapEntry.BaseAddress;
      MemoryMap->VirtualStart  = 0;
      MemoryMap->NumberOfPages = RShiftU64 ((MergeGcdMapEntry.EndAddress - MergeGcdMapEntry.BaseAddress + 1), EFI_PAGE_SHIFT);
      MemoryMap->Attribute     = (MergeGcdMapEntry.Attributes & ~EFI_MEMORY_PORT_IO) | 
                                (MergeGcdMapEntry.Capabilities & (EFI_MEMORY_RP | EFI_MEMORY_WP | EFI_MEMORY_XP | EFI_MEMORY_RO |
                                EFI_MEMORY_UC | EFI_MEMORY_UCE | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB));

      if (MergeGcdMapEntry.GcdMemoryType == EfiGcdMemoryTypeReserved) {
        MemoryMap->Type = EfiReservedMemoryType;
      } else if (MergeGcdMapEntry.GcdMemoryType == EfiGcdMemoryTypeMemoryMappedIo) {
        if ((MergeGcdMapEntry.Attributes & EFI_MEMORY_PORT_IO) == EFI_MEMORY_PORT_IO) {
          MemoryMap->Type = EfiMemoryMappedIOPortSpace;
        } else {
          MemoryMap->Type = EfiMemoryMappedIO;
        }
      }

      //
      // Check to see if the new Memory Map Descriptor can be merged with an 
      // existing descriptor if they are adjacent and have the same attributes
      //
      MemoryMap = MergeMemoryMapDescriptor (MemoryMapStart, MemoryMap, Size);
    }
    
    if (MergeGcdMapEntry.GcdMemoryType == EfiGcdMemoryTypePersistentMemory) {
      //
      // Page Align GCD range is required. When it is converted to EFI_MEMORY_DESCRIPTOR, 
      // it will be recorded as page PhysicalStart and NumberOfPages. 
      //
      ASSERT ((MergeGcdMapEntry.BaseAddress & EFI_PAGE_MASK) == 0);
      ASSERT (((MergeGcdMapEntry.EndAddress - MergeGcdMapEntry.BaseAddress + 1) & EFI_PAGE_MASK) == 0);

      // 
      // Create EFI_MEMORY_DESCRIPTOR for every Persistent GCD entries
      //
      MemoryMap->PhysicalStart = MergeGcdMapEntry.BaseAddress;
      MemoryMap->VirtualStart  = 0;
      MemoryMap->NumberOfPages = RShiftU64 ((MergeGcdMapEntry.EndAddress - MergeGcdMapEntry.BaseAddress + 1), EFI_PAGE_SHIFT);
      MemoryMap->Attribute     = MergeGcdMapEntry.Attributes | EFI_MEMORY_NV | 
                                (MergeGcdMapEntry.Capabilities & (EFI_MEMORY_RP | EFI_MEMORY_WP | EFI_MEMORY_XP | EFI_MEMORY_RO |
                                EFI_MEMORY_UC | EFI_MEMORY_UCE | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB));
      MemoryMap->Type          = EfiPersistentMemory;
      
      //
      // Check to see if the new Memory Map Descriptor can be merged with an 
      // existing descriptor if they are adjacent and have the same attributes
      //
      MemoryMap = MergeMemoryMapDescriptor (MemoryMapStart, MemoryMap, Size);
    }
    if (Link == &mGcdMemorySpaceMap) {
      //
      // break loop when arrive at head.
      //
      break;
    }
    if (GcdMapEntry != NULL) {
      //
      // Copy new GCD map entry for the following GCD range merge
      //
      CopyMem (&MergeGcdMapEntry, GcdMapEntry, sizeof (MergeGcdMapEntry));
    }
  }

  //
  // Compute the size of the buffer actually used after all memory map descriptor merge operations
  //
  BufferSize = ((UINT8 *)MemoryMap - (UINT8 *)MemoryMapStart);

  Status = EFI_SUCCESS;

Done:
  //
  // Update the map key finally
  //
  if (MapKey != NULL) {
    *MapKey = mMemoryMapKey;
  }

  CoreReleaseMemoryLock ();

  CoreReleaseGcdMemoryLock ();

  *MemoryMapSize = BufferSize;

  return Status;
}


/**
  Internal function.  Used by the pool functions to allocate pages
  to back pool allocation requests.

  @param  PoolType               The type of memory for the new pool pages
  @param  NumberOfPages          No of pages to allocate
  @param  Alignment              Bits to align.

  @return The allocated memory, or NULL

**/
VOID *
CoreAllocatePoolPages (
  IN EFI_MEMORY_TYPE    PoolType,
  IN UINTN              NumberOfPages,
  IN UINTN              Alignment,
  IN BOOLEAN            NeedGuard
  )
{
  UINT64            Start;

  if (FeaturePcdGet(PcdHeapPageGuard) && NeedGuard) {
    NumberOfPages += 2;
  }

  //
  // Find the pages to convert
  //
  Start = FindFreePages (MAX_ADDRESS, NumberOfPages, PoolType, Alignment);

  //
  // Convert it to boot services data
  //
  if (Start == 0) {
    DEBUG ((DEBUG_ERROR | DEBUG_PAGE, "AllocatePoolPages: failed to allocate %d pages\n", (UINT32)NumberOfPages));
  } else {
    CoreConvertPages (Start, NumberOfPages, PoolType);
  }

  if (FeaturePcdGet(PcdHeapPageGuard) && NeedGuard && (Start != 0)) {
    Start += EFI_PAGES_TO_SIZE(1);
    SetGuardPageOnAllocatePoolPages (Start, NumberOfPages - 2);
  }

  return (VOID *)(UINTN) Start;
}


/**
  Internal function.  Frees pool pages allocated via AllocatePoolPages ()

  @param  Memory                 The base address to free
  @param  NumberOfPages          The number of pages to free

**/
VOID
CoreFreePoolPages (
  IN EFI_PHYSICAL_ADDRESS   Memory,
  IN UINTN                  NumberOfPages,
  IN EFI_MEMORY_TYPE        PoolType
  )
{
  BOOLEAN         IsGuarded;
  EFI_STATUS      Status;

  IsGuarded = FALSE;
  if (FeaturePcdGet(PcdHeapPageGuard)) {
    if (IsMemoryTypeForHeapGuard(PoolType)) {
      IsGuarded = TRUE;
    }
  }

  Status = CoreConvertPages (Memory, NumberOfPages, EfiConventionalMemory);

  if (FeaturePcdGet(PcdHeapPageGuard)) {
    if (!EFI_ERROR (Status) && IsGuarded) {
      ClearGuardPageOnFreePoolPages (Memory, NumberOfPages);
    }
  }
}



/**
  Make sure the memory map is following all the construction rules,
  it is the last time to check memory map error before exit boot services.

  @param  MapKey                 Memory map key

  @retval EFI_INVALID_PARAMETER  Memory map not consistent with construction
                                 rules.
  @retval EFI_SUCCESS            Valid memory map.

**/
EFI_STATUS
CoreTerminateMemoryMap (
  IN UINTN          MapKey
  )
{
  EFI_STATUS        Status;
  LIST_ENTRY        *Link;
  MEMORY_MAP        *Entry;

  Status = EFI_SUCCESS;

  CoreAcquireMemoryLock ();

  if (MapKey == mMemoryMapKey) {

    //
    // Make sure the memory map is following all the construction rules
    // This is the last chance we will be able to display any messages on
    // the  console devices.
    //

    for (Link = gMemoryMap.ForwardLink; Link != &gMemoryMap; Link = Link->ForwardLink) {
      Entry = CR(Link, MEMORY_MAP, Link, MEMORY_MAP_SIGNATURE);
      if (Entry->Type < EfiMaxMemoryType) {
        if (mMemoryTypeStatistics[Entry->Type].Runtime) {
          ASSERT (Entry->Type != EfiACPIReclaimMemory);
          ASSERT (Entry->Type != EfiACPIMemoryNVS);
          if ((Entry->Start & (EFI_ACPI_RUNTIME_PAGE_ALLOCATION_ALIGNMENT - 1)) != 0) {
            DEBUG((DEBUG_ERROR | DEBUG_PAGE, "ExitBootServices: A RUNTIME memory entry is not on a proper alignment.\n"));
            Status =  EFI_INVALID_PARAMETER;
            goto Done;
          }
          if (((Entry->End + 1) & (EFI_ACPI_RUNTIME_PAGE_ALLOCATION_ALIGNMENT - 1)) != 0) {
            DEBUG((DEBUG_ERROR | DEBUG_PAGE, "ExitBootServices: A RUNTIME memory entry is not on a proper alignment.\n"));
            Status =  EFI_INVALID_PARAMETER;
            goto Done;
          }
        }
      }
    }

    //
    // The map key they gave us matches what we expect. Fall through and
    // return success. In an ideal world we would clear out all of
    // EfiBootServicesCode and EfiBootServicesData. However this function
    // is not the last one called by ExitBootServices(), so we have to
    // preserve the memory contents.
    //
  } else {
    Status = EFI_INVALID_PARAMETER;
  }

Done:
  CoreReleaseMemoryLock ();

  return Status;
}









