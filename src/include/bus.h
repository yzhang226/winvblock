/**
 * Copyright (C) 2009-2010, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
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
#ifndef WVL_M_BUS_H_
#  define WVL_M_BUS_H_

/**
 * @file
 *
 * Bus specifics.
 */

/**
 * A bus thread routine.
 *
 * @v bus       The bus to be used in the thread routine.
 *
 * If you implement your own bus thread routine, you should call
 * WvBusProcessWorkItems() within its loop.
 */
typedef void STDCALL WVL_F_BUS_THREAD(IN WVL_SP_BUS_T);
typedef WVL_F_BUS_THREAD * WVL_FP_BUS_THREAD;

/**
 * A bus PnP routine.
 *
 * @v bus               The bus to receive the PnP IRP.
 * @v irp               The IRP to process.
 * @ret NTSTATUS        The status of the operation.
 */
typedef NTSTATUS STDCALL WVL_F_BUS_PNP(IN WVL_SP_BUS_T, IN PIRP);
typedef WVL_F_BUS_PNP * WVL_FP_BUS_PNP;

/* Device state. */
typedef enum WVL_BUS_STATE {
    WvlBusStateNotStarted,
    WvlBusStateStarted,
    WvlBusStateStopPending,
    WvlBusStateStopped,
    WvlBusStateRemovePending,
    WvlBusStateSurpriseRemovePending,
    WvlBusStateDeleted,
    WvlBusStates
  } WVL_E_BUS_STATE, * WVL_EP_BUS_STATE;

/* The bus type. */
typedef struct WVL_BUS_T {
    PDEVICE_OBJECT LowerDeviceObject;
    PDEVICE_OBJECT Pdo;
    PDEVICE_OBJECT Fdo;
    winvblock__uint32 Children;
    WVL_FP_BUS_THREAD Thread;
    KEVENT ThreadSignal;
    winvblock__bool Stop;
    WVL_E_BUS_STATE OldState;
    WVL_E_BUS_STATE State;
    WVL_FP_BUS_PNP QueryDevText;
    struct {
        LIST_ENTRY Nodes;
        USHORT NodeCount;
        LIST_ENTRY WorkItems;
        KSPIN_LOCK WorkItemsLock;
      } BusPrivate_;
  } WVL_S_BUS_T, * WVL_SP_BUS_T;

/* A child PDO node on a bus.  Treat this as an opaque type. */
typedef struct WV_BUS_NODE {
    struct {
        LIST_ENTRY Link;
        PDEVICE_OBJECT Pdo;
        WVL_SP_BUS_T Bus;
        /* The child's unit number relative to the parent bus. */
        winvblock__uint32 Num;
      } BusPrivate_;
  } WVL_S_BUS_NODE, * WVL_SP_BUS_NODE;

/**
 * A custom work-item function.
 *
 * @v context           Function-specific data.
 *
 * If a driver needs to enqueue a work item which should execute in the
 * context of the bus' controlling thread (this is the thread which calls
 * WvBusProcessWorkItems()), then this is the function prototype to be
 * used.
 */
typedef void STDCALL WV_F_BUS_WORK_ITEM(void *);
typedef WV_F_BUS_WORK_ITEM * WV_FP_BUS_WORK_ITEM;
typedef struct WV_BUS_CUSTOM_WORK_ITEM {
    WV_FP_BUS_WORK_ITEM Func;
    void * Context;
  } WV_S_BUS_CUSTOM_WORK_ITEM, * WV_SP_BUS_CUSTOM_WORK_ITEM;

/* Exports. */
extern winvblock__lib_func void WvBusInit(WVL_SP_BUS_T);
extern winvblock__lib_func WVL_SP_BUS_T WvBusCreate(void);
extern winvblock__lib_func void WvBusProcessWorkItems(WVL_SP_BUS_T);
extern winvblock__lib_func void WvBusCancelWorkItems(WVL_SP_BUS_T);
extern winvblock__lib_func NTSTATUS WvBusStartThread(
    IN WVL_SP_BUS_T,
    OUT PETHREAD *
  );
extern winvblock__lib_func winvblock__bool STDCALL WvBusInitNode(
    OUT WVL_SP_BUS_NODE,
    IN PDEVICE_OBJECT
  );
extern winvblock__lib_func NTSTATUS STDCALL WvBusAddNode(
    WVL_SP_BUS_T,
    WVL_SP_BUS_NODE
  );
extern winvblock__lib_func NTSTATUS STDCALL WvBusRemoveNode(WVL_SP_BUS_NODE);
extern winvblock__lib_func NTSTATUS STDCALL WvBusEnqueueIrp(WVL_SP_BUS_T, PIRP);
extern winvblock__lib_func NTSTATUS STDCALL WvBusEnqueueCustomWorkItem(
    WVL_SP_BUS_T,
    WV_SP_BUS_CUSTOM_WORK_ITEM
  );
extern winvblock__lib_func NTSTATUS STDCALL WvBusSysCtl(
    IN WVL_SP_BUS_T,
    IN PIRP
  );
extern winvblock__lib_func NTSTATUS STDCALL WvBusPower(
    IN WVL_SP_BUS_T,
    IN PIRP
  );
/* IRP_MJ_PNP dispatcher in bus/pnp.c */
extern winvblock__lib_func NTSTATUS STDCALL WvBusPnp(
    IN WVL_SP_BUS_T,
    IN PIRP,
    IN UCHAR
  );
extern winvblock__lib_func winvblock__uint32 STDCALL WvBusGetNodeNum(
    IN WVL_SP_BUS_NODE
  );
extern winvblock__lib_func WVL_SP_BUS_NODE STDCALL WvBusGetNextNode(
    IN WVL_SP_BUS_T,
    IN WVL_SP_BUS_NODE
  );
extern winvblock__lib_func PDEVICE_OBJECT STDCALL WvBusGetNodePdo(
    IN WVL_SP_BUS_NODE
  );
extern winvblock__lib_func winvblock__uint32 STDCALL WvBusGetNodeCount(
    WVL_SP_BUS_T
  );

#endif  /* WVL_M_BUS_H_ */
