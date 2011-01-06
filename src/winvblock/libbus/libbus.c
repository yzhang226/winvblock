/**
 * Copyright (C) 2009-2011, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 * Copyright 2006-2008, V.
 * For WinAoE contact information, see http://winaoe.org/
 *
 * This file is part of WinVBlock, derived from WinAoE.
 *
 * WinVBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinVBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinVBlock.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Bus specifics.
 */

#include <ntddk.h>

#include "portable.h"
#include "winvblock.h"
#include "wv_stdlib.h"
#include "irp.h"
#include "bus.h"
#include "debug.h"

/* Types. */
typedef enum WVL_BUS_WORK_ITEM_CMD {
    WvlBusWorkItemCmdAddPdo_,
    WvlBusWorkItemCmdRemovePdo_,
    WvlBusWorkItemCmdProcessIrp_,
    WvlBusWorkItemCmdCustom_,
    WvlBusWorkItemCmds_
  } WVL_E_BUS_WORK_ITEM_CMD, * WVL_EP_BUS_WORK_ITEM_CMD;

typedef struct WVL_BUS_WORK_ITEM {
    LIST_ENTRY Link;
    WVL_E_BUS_WORK_ITEM_CMD Cmd;
    union {
        WVL_SP_BUS_NODE Node;
        PIRP Irp;
        WVL_SP_BUS_CUSTOM_WORK_ITEM Custom;
      } Context;
  } WVL_S_BUS_WORK_ITEM, * WVL_SP_BUS_WORK_ITEM;

/* Forward declarations. */
static WVL_F_BUS_THREAD WvlBusDefaultThread;
static BOOLEAN WvlBusAddWorkItem(
    WVL_SP_BUS_T,
    WVL_SP_BUS_WORK_ITEM
  );
static WVL_SP_BUS_WORK_ITEM WvlBusGetWorkItem(WVL_SP_BUS_T);

/* Handle an IRP_MJ_SYSTEM_CONTROL IRP. */
WVL_M_LIB NTSTATUS STDCALL WvlBusSysCtl(
    IN WVL_SP_BUS_T Bus,
    IN PIRP Irp
  ) {
    PDEVICE_OBJECT lower = Bus->LowerDeviceObject;

    if (lower) {
        DBG("Passing IRP_MJ_SYSTEM_CONTROL down\n");
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(lower, Irp);
      }
    return WvlIrpComplete(Irp, 0, STATUS_SUCCESS);
  }

/* Handle a power IRP. */
WVL_M_LIB NTSTATUS STDCALL WvlBusPower(
    IN WVL_SP_BUS_T Bus,
    IN PIRP Irp
  ) {
    PDEVICE_OBJECT lower = Bus->LowerDeviceObject;

    PoStartNextPowerIrp(Irp);
    if (lower) {
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(lower, Irp);
      }
    return WvlIrpComplete(Irp, 0, STATUS_SUCCESS);
  }

/**
 * Initialize bus defaults.
 *
 * @v Bus               Points to the bus to initialize with defaults.
 */
WVL_M_LIB VOID WvlBusInit(WVL_SP_BUS_T Bus) {
    RtlZeroMemory(Bus, sizeof *Bus);
    /* Populate non-zero bus device defaults. */
    Bus->Thread = WvlBusDefaultThread;
    InitializeListHead(&Bus->BusPrivate_.Nodes);
    KeInitializeSpinLock(&Bus->BusPrivate_.WorkItemsLock);
    InitializeListHead(&Bus->BusPrivate_.WorkItems);
    KeInitializeEvent(&Bus->ThreadSignal, SynchronizationEvent, FALSE);
  }

/**
 * Add a work item for a bus to process.
 *
 * @v bus                       The bus to process the work item.
 * @v work_item                 The work item to add.
 * @ret BOOLEAN         TRUE if added, else FALSE
 *
 * Note that this function will initialize the work item's completion signal.
 */
static BOOLEAN WvlBusAddWorkItem(
    WVL_SP_BUS_T bus,
    WVL_SP_BUS_WORK_ITEM work_item
  ) {
    ExInterlockedInsertTailList(
        &bus->BusPrivate_.WorkItems,
        &work_item->Link,
        &bus->BusPrivate_.WorkItemsLock
      );

    return TRUE;
  }

/**
 * Get (and dequeue) a work item from a bus' queue.
 *
 * @v bus                       The bus processing the work item.
 * @ret WVL_SP_BUS_WORK_ITEM    The work item, or NULL for an empty queue.
 */
static WVL_SP_BUS_WORK_ITEM WvlBusGetWorkItem(
    WVL_SP_BUS_T bus
  ) {
    PLIST_ENTRY list_entry;

    list_entry = ExInterlockedRemoveHeadList(
        &bus->BusPrivate_.WorkItems,
        &bus->BusPrivate_.WorkItemsLock
      );
    if (!list_entry)
      return NULL;

    return CONTAINING_RECORD(list_entry, WVL_S_BUS_WORK_ITEM, Link);
  }

/**
 * Add a PDO node to a bus' list of children.  Internal.
 *
 * @v bus               The bus to add the node to.
 * @v new_node          The PDO node to add to the bus.
 *
 * Don't call this function yourself.  It expects to have exclusive
 * access to the bus' list of children.
 */
static VOID STDCALL WvlBusAddNode_(WVL_SP_BUS_T bus, WVL_SP_BUS_NODE new_node) {
    PLIST_ENTRY walker;

    DBG(
        "Adding PDO 0x%08X to bus 0x%08X.\n",
        (PVOID) new_node->BusPrivate_.Pdo,
        (PVOID) bus
      );
    ObReferenceObject(new_node->BusPrivate_.Pdo);
    bus->BusPrivate_.NodeCount++;
    /* It's too bad about having both linked list and bus ref. */
    new_node->BusPrivate_.Bus = bus;

    /* Find a slot for the new child. */
    walker = &bus->BusPrivate_.Nodes;
    new_node->BusPrivate_.Num = 0;
    while ((walker = walker->Flink) != &bus->BusPrivate_.Nodes) {
        WVL_SP_BUS_NODE node = CONTAINING_RECORD(
            walker,
            WVL_S_BUS_NODE,
            BusPrivate_.Link
          );

        if (
            node->BusPrivate_.Num &&
            (node->BusPrivate_.Link.Blink == &bus->BusPrivate_.Nodes)
          ) {
            /* The first node's unit number is != 0.  Insert here. */
            break;
          }
        if (node->BusPrivate_.Num > new_node->BusPrivate_.Num) {
            /* There is a gap so insert here. */
            break;
          }
        /* Continue trying to find a slot. */
        new_node->BusPrivate_.Num++;
      } /* while */
    /* Insert before walker. */
    InsertTailList(walker, &new_node->BusPrivate_.Link);
    new_node->Linked = TRUE;
    return;
  }

/**
 * Remove a PDO node from a bus.  Internal.
 *
 * @v bus             The bus to remove the node from.
 * @v node            The PDO node to remove from its parent bus.
 *
 * Don't call this function yourself.  It expects to have exclusive
 * access to the bus' list of children.
 */
static VOID STDCALL WvlBusRemoveNode_(
    WVL_SP_BUS_T bus,
    WVL_SP_BUS_NODE node
  ) {
    DBG(
        "Removing PDO 0x%08X from bus 0x%08X.\n",
        (PVOID) node->BusPrivate_.Pdo,
        (PVOID) bus
      );
    RemoveEntryList(&node->BusPrivate_.Link);
    node->Linked = FALSE;
    ObDereferenceObject(node->BusPrivate_.Pdo);
    bus->BusPrivate_.NodeCount--;
    return;    
  }

/**
 * Process work items for a bus.
 *
 * @v Bus               The bus to process its work items.
 */
WVL_M_LIB VOID WvlBusProcessWorkItems(WVL_SP_BUS_T Bus) {
    WVL_SP_BUS_WORK_ITEM work_item;
    WVL_SP_BUS_NODE node;
    PIRP irp;
    PIO_STACK_LOCATION io_stack_loc;
    PDEVICE_OBJECT dev_obj;
    PDRIVER_OBJECT driver_obj;
    BOOLEAN nodes_changed = FALSE;

    while (work_item = WvlBusGetWorkItem(Bus)) {
        switch (work_item->Cmd) {
            case WvlBusWorkItemCmdAddPdo_:
              node = work_item->Context.Node;
              WvlBusAddNode_(Bus, node);
              nodes_changed = TRUE;
              break;

            case WvlBusWorkItemCmdRemovePdo_:
              node = work_item->Context.Node;
              WvlBusRemoveNode_(Bus, node);
              nodes_changed = TRUE;
              break;

            case WvlBusWorkItemCmdProcessIrp_:
              irp = work_item->Context.Irp;
              io_stack_loc = IoGetCurrentIrpStackLocation(irp);
              dev_obj = Bus->Fdo;
              if (!dev_obj)
                WvlIrpComplete(irp, 0, STATUS_NO_SUCH_DEVICE);
              driver_obj = dev_obj->DriverObject;
              driver_obj->MajorFunction[io_stack_loc->MajorFunction](
                  dev_obj,
                  irp
                );
              break;

            case WvlBusWorkItemCmdCustom_:
              DBG("Custom work item.\n");
              work_item->Context.Custom->Func(
                  work_item->Context.Custom->Context
                );
              break;

            default:
              DBG("Unknown work item type!\n");
          }
        wv_free(work_item);
      }
    if (nodes_changed && Bus->Pdo) {
        nodes_changed = FALSE;
        IoInvalidateDeviceRelations(
            Bus->Pdo,
            BusRelations
          );
      }
    return;
  }

/**
 * Cancel pending work items for a bus.
 *
 * @v Bus       The bus to cancel pending work items for.
 */
WVL_M_LIB VOID WvlBusCancelWorkItems(WVL_SP_BUS_T Bus) {
    WVL_SP_BUS_WORK_ITEM work_item;

    DBG("Canceling work items.\n");
    while (work_item = WvlBusGetWorkItem(Bus))
      wv_free(work_item);
    return;
  }

/**
 * The bus thread wrapper.
 *
 * @v context           The thread context.  In our case, it points to
 *                      the bus that the thread should use in processing.
 *
 * Note that we do not attempt to free the bus data; this is a bus
 * implementor's responsibility.  We do, however, set the ThreadStopped
 * signal which should mean that resources can be freed, from a completed
 * thread's perspective.
 */
static VOID STDCALL WvlBusThread(IN PVOID context) {
    WVL_SP_BUS_T bus = context;

    if (!bus || !bus->Thread) {
        DBG("No bus or no thread!\n");
        return;
      }

    bus->Thread(bus);
    DBG("Exiting.\n");
    PsTerminateSystemThread(STATUS_SUCCESS);
    return;
  }

/**
 * The default bus thread routine.
 *
 * @v bus       Points to the bus device for the thread to work with.
 *
 * Note that if you implement your own bus type using this library,
 * you can override the thread routine with your own.  If you do so,
 * your thread routine should call WvlBusProcessWorkItems() within
 * its loop.  To start a bus thread, use WvlBusStartThread()
 * If you implement your own thread routine, you are also responsible
 * for calling WvlBusCancelWorkItems() and freeing the bus.
 */
static VOID STDCALL WvlBusDefaultThread(IN WVL_SP_BUS_T bus) {
    LARGE_INTEGER timeout;

    /* Wake up at least every 30 seconds. */
    timeout.QuadPart = -300000000LL;

    /* When WVL_S_BUS_T::Stop is set, we shut down. */
    while (!bus->Stop) {
        DBG("Alive.\n");

        /* Wait for the work signal or the timeout. */
        KeWaitForSingleObject(
            &bus->ThreadSignal,
            Executive,
            KernelMode,
            FALSE,
            &timeout
          );
        /* Reset the work signal. */
        KeResetEvent(&bus->ThreadSignal);

        WvlBusProcessWorkItems(bus);
      } /* while !bus->Stop */

    WvlBusCancelWorkItems(bus);
    return;
  }

/**
 * Start a bus thread.
 *
 * @v Bus               The bus to start a thread for.
 * @v Thread            A PETHREAD to be filled to reference the thread.
 * @ret NTSTATUS        The status of the thread creation operation.
 *
 * Also see WVL_F_BUS_THREAD in the header for details about the prototype
 * for implementing your own bus thread routine.  You set WVL_S_BUS_T::Thread
 * to specify your own thread routine, then call this function to start it.
 * When stopping the thread, you can wait on the thread handle.
 */
WVL_M_LIB NTSTATUS WvlBusStartThread(
    IN WVL_SP_BUS_T Bus,
    OUT PETHREAD * Thread
  ) {
    OBJECT_ATTRIBUTES obj_attrs;
    HANDLE thread_handle;
    NTSTATUS status;

    if (!Bus) {
        DBG("No bus specified!\n");
        return STATUS_INVALID_PARAMETER;
      }

    InitializeObjectAttributes(
        &obj_attrs,
        NULL,
        OBJ_KERNEL_HANDLE,
        NULL,
        NULL
      );
    status = PsCreateSystemThread(
        &thread_handle,
        THREAD_ALL_ACCESS,
        &obj_attrs,
        NULL,
        NULL,
        WvlBusThread,
        Bus
      );
    if (!NT_SUCCESS(status))
      return status;
    return ObReferenceObjectByHandle(
        thread_handle,
        THREAD_ALL_ACCESS,
        *PsThreadType,
        KernelMode,
        Thread,
        NULL
      );
  }

/**
 * Initialize a bus node with an associated PDO.
 *
 * @v Node              The node to initialize.
 * @v Pdo               The PDO to associate the node with.
 * @ret BOOLEAN FALSE for a NULL argument, otherwise TRUE
 */
WVL_M_LIB BOOLEAN STDCALL WvlBusInitNode(
    OUT WVL_SP_BUS_NODE Node,
    IN PDEVICE_OBJECT Pdo
  ) {
    if (!Node || !Pdo)
      return FALSE;

    RtlZeroMemory(Node, sizeof *Node);
    Node->BusPrivate_.Pdo = Pdo;
    return TRUE;
  }

/**
 * Add a PDO node to a bus' list of children.
 *
 * @v Bus               The bus to add the node to.
 * @v Node              The PDO node to add to the bus.
 * @ret NTSTATUS        The status of the operation.
 *
 * Do not attempt to add the same node to more than one bus.
 * When WvlBusProcessWorkItems() is called for the bus, the
 * node will be added.  This is usually from the bus' thread.
 */
WVL_M_LIB NTSTATUS STDCALL WvlBusAddNode(
    WVL_SP_BUS_T Bus,
    WVL_SP_BUS_NODE Node
  ) {
    WVL_SP_BUS_WORK_ITEM work_item;

    if (
        !Bus ||
        !Node ||
        Bus->Fdo->DriverObject != Node->BusPrivate_.Pdo->DriverObject
      )
      return STATUS_INVALID_PARAMETER;

    if (Bus->Stop)
      return STATUS_NO_SUCH_DEVICE;

    if (!(work_item = wv_malloc(sizeof *work_item)))
      return STATUS_INSUFFICIENT_RESOURCES;

    work_item->Cmd = WvlBusWorkItemCmdAddPdo_;
    work_item->Context.Node = Node;
    if (!WvlBusAddWorkItem(Bus, work_item)) {
        wv_free(work_item);
        return STATUS_UNSUCCESSFUL;
      }
    /* Fire and forget. */
    KeSetEvent(&Bus->ThreadSignal, 0, FALSE);
    return STATUS_SUCCESS;
  }

/**
 * Remove a PDO node from a bus.
 *
 * @v Node              The PDO node to remove from its parent bus.
 * @ret NTSTATUS        The status of the operation.
 *
 * When WvlBusProcessWorkItems() is called for the bus, it will
 * then remove the node.  This is usually from the bus' thread.
 */
WVL_M_LIB NTSTATUS STDCALL WvlBusRemoveNode(
    WVL_SP_BUS_NODE Node
  ) {
    WVL_SP_BUS_T bus;
    WVL_SP_BUS_WORK_ITEM work_item;

    if (!Node || !(bus = Node->BusPrivate_.Bus))
      return STATUS_INVALID_PARAMETER;

    if (bus->Stop)
      return STATUS_NO_SUCH_DEVICE;

    if (!(work_item = wv_malloc(sizeof *work_item)))
      return STATUS_INSUFFICIENT_RESOURCES;

    work_item->Cmd = WvlBusWorkItemCmdRemovePdo_;
    work_item->Context.Node = Node;
    if (!WvlBusAddWorkItem(bus, work_item)) {
        wv_free(work_item);
        return STATUS_UNSUCCESSFUL;
      }
    /* Fire and forget. */
    KeSetEvent(&bus->ThreadSignal, 0, FALSE);
    return STATUS_SUCCESS;
  }

/**
 * Enqueue an IRP for a bus' thread to process.
 *
 * @v Bus               The bus for the IRP.
 * @v Irp               The IRP for the bus.
 * @ret NTSTATUS        The status of the operation.  Returns STATUS_PENDING
 *                      if the IRP is successfully added to the queue.
 */
WVL_M_LIB NTSTATUS STDCALL WvlBusEnqueueIrp(
    WVL_SP_BUS_T Bus,
    PIRP Irp
  ) {
    WVL_SP_BUS_WORK_ITEM work_item;

    if (!Bus || !Irp)
      return STATUS_INVALID_PARAMETER;

    if (Bus->Stop)
      return STATUS_NO_SUCH_DEVICE;

    if (!(work_item = wv_malloc(sizeof *work_item)))
      return STATUS_INSUFFICIENT_RESOURCES;

    work_item->Cmd = WvlBusWorkItemCmdProcessIrp_;
    work_item->Context.Irp = Irp;
    IoMarkIrpPending(Irp);
    if (!WvlBusAddWorkItem(Bus, work_item)) {
        wv_free(work_item);
        return STATUS_UNSUCCESSFUL;
      }
    /* Fire and forget. */
    KeSetEvent(&Bus->ThreadSignal, 0, FALSE);
    return STATUS_PENDING;
  }

/**
 * Enqueue a custom work item for a bus' thread to process.
 *
 * @v Bus               The bus for the IRP.
 * @v CustomWorkItem    The custom work item for the bus' thread to process.
 * @ret NTSTATUS        The status of the operation.
 */
WVL_M_LIB NTSTATUS STDCALL WvlBusEnqueueCustomWorkItem(
    WVL_SP_BUS_T Bus,
    WVL_SP_BUS_CUSTOM_WORK_ITEM CustomWorkItem
  ) {
    WVL_SP_BUS_WORK_ITEM work_item;

    if (!Bus || !CustomWorkItem)
      return STATUS_INVALID_PARAMETER;

    if (Bus->Stop)
      return STATUS_NO_SUCH_DEVICE;

    if (!(work_item = wv_malloc(sizeof *work_item)))
      return STATUS_INSUFFICIENT_RESOURCES;

    work_item->Cmd = WvlBusWorkItemCmdCustom_;
    work_item->Context.Custom = CustomWorkItem;
    if (!WvlBusAddWorkItem(Bus, work_item)) {
        wv_free(work_item);
        return STATUS_UNSUCCESSFUL;
      }
    /* Fire and forget. */
    KeSetEvent(&Bus->ThreadSignal, 0, FALSE);
    return STATUS_SUCCESS;
  }

/**
 * Get the unit number for a child node on a bus.
 *
 * @v Node              The node whose unit number we request.
 * @ret UINT32          The unit number for the node.
 */
WVL_M_LIB UINT32 STDCALL WvlBusGetNodeNum(
    IN WVL_SP_BUS_NODE Node
  ) {
    return Node->BusPrivate_.Num;
  }

/**
 * Get the next child node on a bus.
 *
 * @v Bus               The bus whose nodes are fetched.
 * @v PrevNode          The previous node.  Pass NULL to begin.
 * @ret WVL_SP_BUS_NODE  Returns NULL when there are no more nodes.
 *
 * This function should only be called within the thread context of
 * whichever thread calls WvlBusProcessWorkItems() because it expects
 * the list of child nodes to remain static between calls.
 */
WVL_M_LIB WVL_SP_BUS_NODE STDCALL WvlBusGetNextNode(
    IN WVL_SP_BUS_T Bus,
    IN WVL_SP_BUS_NODE PrevNode
  ) {
    PLIST_ENTRY link;

    if (!PrevNode)
      link = &Bus->BusPrivate_.Nodes;
      else
      link = &PrevNode->BusPrivate_.Link;
    link = link->Flink;
    if (link == &Bus->BusPrivate_.Nodes)
      return NULL;
    return CONTAINING_RECORD(link, WVL_S_BUS_NODE, BusPrivate_.Link);
  }

/**
 * Get a child node's PDO.
 *
 * @v Node              The node whose PDO will be returned.
 * @ret PDEVICE_OBJECT  The PDO for the node.
 */
WVL_M_LIB PDEVICE_OBJECT STDCALL WvlBusGetNodePdo(
    IN WVL_SP_BUS_NODE Node
  ) {
    return Node->BusPrivate_.Pdo;
  }

/**
 * Get the count of child nodes on a bus.
 *
 * @v Bus               The bus whose node-count will be returned.
 * @v UINT32            The count of nodes on the bus.
 *
 * In order for this function to yield a race-free, useful result, it
 * should be used by whatever thread calls WvlBusProcessWorkItems()
 */
WVL_M_LIB UINT32 STDCALL WvlBusGetNodeCount(
    WVL_SP_BUS_T Bus
  ) {
    return Bus->BusPrivate_.NodeCount;
  }

/**
 * Register an owning thread for a bus.
 *
 * @v Bus               The bus to take ownership of with the current thread.
 * @ret BOOLEAN         FALSE if the bus is already owned, else TRUE.
 *
 * Some operations, such as manipulations of the child node list,
 * are best carried out serialized on a single, owning thread.
 * This is to avoid race conditions but without holding an
 * expensive lock.
 */
WVL_M_LIB BOOLEAN STDCALL WvlBusRegisterOwnerThread(IN WVL_SP_BUS_T Bus) {
    if (Bus->BusPrivate_.Thread)
      return FALSE;
    Bus->BusPrivate_.Thread = PsGetCurrentThread();
    return TRUE;
  }

/**
 * Check if the current thread owns a bus.
 *
 * @v Bus               The bus to check ownership of.
 * @ret BOOLEAN         FALSE if the bus is owned or ownerless, else TRUE.
 *
 * If a bus doesn't have an owning thread, this returns FALSE.
 */
WVL_M_LIB BOOLEAN STDCALL WvlBusNotOwned(IN WVL_SP_BUS_T Bus) {
    if (
        !Bus->BusPrivate_.Thread ||
        (Bus->BusPrivate_.Thread == PsGetCurrentThread())
      )
      return FALSE;
    return TRUE;
  }