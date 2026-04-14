/*
 * QEMU emulation of an RISC-V IOMMU
 *
 * Copyright (C) 2022-2023 Rivos Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_RISCV_IOMMU_H
#define HW_RISCV_IOMMU_H

#include "qemu/osdep.h"
#include "qom/object.h"

#define TYPE_RISCV_IOMMU "riscv-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(RISCVIOMMUState, RISCV_IOMMU)
typedef struct RISCVIOMMUState RISCVIOMMUState;

#define TYPE_RISCV_IOMMU_MEMORY_REGION "riscv-iommu-mr"
typedef struct RISCVIOMMUSpace RISCVIOMMUSpace;

#define TYPE_RISCV_IOMMU_PCI "riscv-iommu-pci"
OBJECT_DECLARE_TYPE(RISCVIOMMUStatePci, RISCVIOMMUPciClass, RISCV_IOMMU_PCI)
typedef struct RISCVIOMMUStatePci RISCVIOMMUStatePci;
typedef struct RISCVIOMMUPciClass RISCVIOMMUPciClass;

#define TYPE_RISCV_IOMMU_SYS "riscv-iommu-device"
OBJECT_DECLARE_TYPE(RISCVIOMMUStateSys, RISCVIOMMUSysClass, RISCV_IOMMU_SYS)
typedef struct RISCVIOMMUStateSys RISCVIOMMUStateSys;
typedef struct RISCVIOMMUSysClass RISCVIOMMUSysClass;

#define FDT_IRQ_TYPE_EDGE_LOW 1

#include "hw/pci/pci_bus.h"

/**
 * riscv_iommu_sys_setup_pci_bus - Wire a PCIe root bus to a RISC-V IOMMU
 *                                  platform device.
 *
 * Call this after both the TYPE_RISCV_IOMMU_SYS device and the PCIe root bus
 * have been realized.  It installs the RISC-V IOMMU as the address-space
 * provider for @bus so that DMA initiated by PCIe devices passes through IOMMU
 * translation instead of bypassing it.
 *
 * @iommu_sys_dev: A realized TYPE_RISCV_IOMMU_SYS DeviceState.
 * @bus:           The PCIe root bus to attach this IOMMU to.
 * @errp:          Standard QEMU error pointer.
 */
void riscv_iommu_sys_setup_pci_bus(DeviceState *iommu_sys_dev,
                                   PCIBus *bus, Error **errp);

#endif
