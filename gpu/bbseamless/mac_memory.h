// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdint>
namespace BbSeamless {
inline bool HostRegion(uintptr_t at,uintptr_t& first,uintptr_t& last,uint32_t& protection) {
    mach_vm_address_t address=at;
    mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object=MACH_PORT_NULL;
    const auto result=mach_vm_region(mach_task_self(),&address,&size,VM_REGION_BASIC_INFO_64,
                                     reinterpret_cast<vm_region_info_t>(&info),&count,&object);
    if (object!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
    if (result!=KERN_SUCCESS || address>at || at-address>=size) return false;
    first=address;last=address+size;protection=info.protection;return true;
}
}
