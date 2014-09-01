
#include <vmm/vmx.h>
#include <inc/error.h>
#include <vmm/vmexits.h>
#include <vmm/ept.h>
#include <inc/x86.h>
#include <inc/assert.h>
#include <kern/pmap.h>
#include <kern/console.h>
#include <kern/kclock.h>
#include <kern/multiboot.h>
#include <inc/string.h>
#include <kern/syscall.h>
#include <kern/env.h>


bool
find_msr_in_region(uint32_t msr_idx, uintptr_t *area, int area_sz, struct vmx_msr_entry **msr_entry) {
    struct vmx_msr_entry *entry = (struct vmx_msr_entry *)area;
    int i;
    for(i=0; i<area_sz; ++i) {
        if(entry->msr_index == msr_idx) {
            *msr_entry = entry;
            return true;
        }
    }
    return false;
}

bool
handle_rdmsr(struct Trapframe *tf, struct VmxGuestInfo *ginfo) {
    uint64_t msr = tf->tf_regs.reg_rcx;
    if(msr == EFER_MSR) {
        // TODO: setup msr_bitmap to ignore EFER_MSR
        uint64_t val;
        struct vmx_msr_entry *entry;
        bool r = find_msr_in_region(msr, ginfo->msr_guest_area, ginfo->msr_count, &entry);
        assert(r);
        val = entry->msr_value;

        tf->tf_regs.reg_rdx = val << 32;
        tf->tf_regs.reg_rax = val & 0xFFFFFFFF;

        tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
        return true;
    }

    return false;
}

bool 
handle_wrmsr(struct Trapframe *tf, struct VmxGuestInfo *ginfo) {
    uint64_t msr = tf->tf_regs.reg_rcx;
    if(msr == EFER_MSR) {

        uint64_t cur_val, new_val;
        struct vmx_msr_entry *entry;
        bool r = 
            find_msr_in_region(msr, ginfo->msr_guest_area, ginfo->msr_count, &entry);
        assert(r);
        cur_val = entry->msr_value;

        new_val = (tf->tf_regs.reg_rdx << 32)|tf->tf_regs.reg_rax;
        if(BIT(cur_val, EFER_LME) == 0 && BIT(new_val, EFER_LME) == 1) {
            // Long mode enable.
            uint32_t entry_ctls = vmcs_read32( VMCS_32BIT_CONTROL_VMENTRY_CONTROLS );
            entry_ctls |= VMCS_VMENTRY_x64_GUEST;
            vmcs_write32( VMCS_32BIT_CONTROL_VMENTRY_CONTROLS, 
                    entry_ctls );

        }

        entry->msr_value = new_val;
        tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
        return true;
    }

    return false;
}

bool
handle_eptviolation(uint64_t *eptrt, struct VmxGuestInfo *ginfo) {
    uint64_t gpa = vmcs_read64(VMCS_64BIT_GUEST_PHYSICAL_ADDR);
    int r;
    if(gpa < 0xA0000 || (gpa >= 0x100000 && gpa < ginfo->phys_sz)) {
        // Allocate a new page to the guest.
        struct Page *p = page_alloc(0);
      
       
  
      if(!p)
            return false;
        p->pp_ref += 1;
        r = ept_map_hva2gpa(eptrt, 
                page2kva(p), (void *)ROUNDDOWN(gpa, PGSIZE), __EPTE_FULL, 0);

        
        assert(r >= 0);
   
    //  cprintf("EPT violation for gpa:%x mapped KVA:%x\n", gpa, page2kva(p));
        

        return true;
    } else if (gpa >= CGA_BUF && gpa < CGA_BUF + PGSIZE) {
        // FIXME: This give direct access to VGA MMIO region.
        r = ept_map_hva2gpa(eptrt, 
                (void *)(KERNBASE + CGA_BUF), (void *)CGA_BUF, __EPTE_FULL, 0);
        assert(r >= 0);
        return true;
    } 
    return false;
}

bool
handle_ioinstr(struct Trapframe *tf, struct VmxGuestInfo *ginfo) {
    static int port_iortc;

    uint64_t qualification = vmcs_read64(VMCS_VMEXIT_QUALIFICATION);
    int port_number = (qualification >> 16) & 0xFFFF;
    bool is_in = BIT(qualification, 3);
    bool handled = false;

    // handle reading physical memory from the CMOS.
    if(port_number == IO_RTC) {
        if(!is_in) {
            port_iortc = tf->tf_regs.reg_rax;
            handled = true;
        }
    } else if (port_number == IO_RTC + 1) {
        if(is_in) {
            if(port_iortc == NVRAM_BASELO) {
                tf->tf_regs.reg_rax = 640 & 0xFF;
                handled = true;
            } else if (port_iortc == NVRAM_BASEHI) {
                tf->tf_regs.reg_rax = (640 >> 8) & 0xFF;
                handled = true;
            } else if (port_iortc == NVRAM_EXTLO) {
                tf->tf_regs.reg_rax = ((ginfo->phys_sz / 1024) - 1024) & 0xFF;
                handled = true;
            } else if (port_iortc == NVRAM_EXTHI) {
                tf->tf_regs.reg_rax = (((ginfo->phys_sz / 1024) - 1024) >> 8) & 0xFF;
                handled = true;
            }
        }

    }

    if(handled) {
        tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
        return true;
    } else {
        cprintf("%x %x\n", qualification, port_iortc);
        return false;    
    }
}

// Emulate a cpuid instruction.
// It is sufficient to issue the cpuid instruction here and collect the return value.
// You can store the output of the instruction in Trapframe tf,
//  but you should hide the presence of vmx from the guest if processor features are requested.
// 
// Return true if the exit is handled properly, false if the VM should be terminated.
//
// Finally, you need to increment the program counter in the trap frame.
// 
// Hint: The TA's solution does not hard-code the length of the cpuid instruction.
bool
handle_cpuid(struct Trapframe *tf, struct VmxGuestInfo *ginfo)
{
    int temp = tf->tf_regs.reg_rax;

    uint32_t eax, ebx, ecx, edx;
    cpuid( temp , &eax, &ebx, &ecx, &edx );

    if(temp == 1)
    {
	ecx = ecx & ~32;
    }

    tf->tf_regs.reg_rax = eax;
    tf->tf_regs.reg_rbx = ebx;
    tf->tf_regs.reg_rcx = ecx;
    tf->tf_regs.reg_rdx = edx;

    tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);

    return true;

}

// Handle vmcall traps from the guest.
// We currently support 3 traps: read the virtual e820 map, 
//   and use host-level IPC (send andrecv).
//
// Return true if the exit is handled properly, false if the VM should be terminated.
//
// Finally, you need to increment the program counter in the trap frame.
// 
// Hint: The TA's solution does not hard-code the length of the cpuid instruction.//

bool
handle_vmcall(struct Trapframe *tf, struct VmxGuestInfo *gInfo, uint64_t *eptrt)
{
    bool handled = false;
    multiboot_info_t mbinfo;
    int perm, r;
    void *gpa_pg, *hva_pg;
    envid_t to_env;
    uint32_t val;

    memory_map_t mmap_list[3];
    uintptr_t *hva;

     struct Page * page ;

    // phys address of the multiboot map in the guest.
    uint64_t multiboot_map_addr = 0x6000;

    switch(tf->tf_regs.reg_rax) {
        case VMX_VMCALL_MBMAP:
 


            // Craft a multiboot (e820) memory map for the guest.
	    //
            // Create three  memory mapping segments: 640k of low mem, the I/O hole (unusable), and 
	    //   high memory (phys_size - 1024k).
	    //
	    // Once the map is ready, find the kernel virtual address of the guest page (if present),
	    //   or allocate one and map it at the multiboot_map_addr (0x6000).
	    // Copy the mbinfo and memory_map_t (segment descriptions) into the guest page, and return
	    //   a pointer to this region in rbx (as a guest physical address).
   

            mmap_list[0].size = 20;

            mmap_list[0].base_addr_low = 0;
            mmap_list[0].base_addr_high = 0;
            mmap_list[0].length_low = 0x000A0000;
            mmap_list[0].length_high = 0;
            mmap_list[0].type = MB_TYPE_USABLE;

            mmap_list[1].size = 20;
            mmap_list[1].base_addr_low = 0x000A0000;
            mmap_list[1].base_addr_high = 0;
            mmap_list[1].length_low = 384*1024;
            mmap_list[1].length_high = 0;
            mmap_list[1].type = MB_TYPE_RESERVED;

            mmap_list[2].size = 20;
            mmap_list[2].base_addr_low = 0x00100000;
            mmap_list[2].base_addr_high = 0;
            mmap_list[2].length_low = gInfo->phys_sz - 1024*1024;
            mmap_list[2].length_high = 0;
            mmap_list[2].type = MB_TYPE_USABLE;


           mbinfo.flags = MB_FLAG_MMAP;
           mbinfo.mem_lower = 0;
           mbinfo.mem_upper = 0;
           mbinfo.boot_device = 0;
           mbinfo.cmdline = 0;
           mbinfo.mods_count = 0;
           mbinfo.mods_addr = 0;
          // mbinfo.u = 0;
           mbinfo.mmap_length =sizeof(mmap_list);

           mbinfo.mmap_addr = multiboot_map_addr + sizeof(mbinfo);

          struct Page *p = page_alloc(0);
          if(!p)
             return false;
          p->pp_ref += 1;  

          memcpy(&mbinfo,page2kva(p),sizeof(mbinfo));  	
          memcpy(&mmap_list,page2kva(p)+sizeof(mbinfo),sizeof(memory_map_t)*3);	

	   ept_map_hva2gpa(eptrt,page2kva(p),(void*)0x6000, __EPTE_FULL,1);

          tf->tf_regs.reg_rbx = 0x6000;


        //  cprintf("\nCASE :  VMX_VMCALL_MBMAP\n"); 	    

	    handled = true;

	    break;
        case VMX_VMCALL_IPCSEND:

       

 	    cprintf("");
           
           envid_t to_env = (envid_t)tf->tf_regs.reg_rdx;

/*
           void* pg = (void*) tf->tf_regs.reg_rbx;

           uint32_t val = (uint32_t) tf->tf_regs.reg_rcx;
           
	    int perm = (int) tf->tf_regs.reg_rdi;

	    int r = sys_ipc_try_send(to_env, val, pg, perm);

*/
           int i;
           if(to_env == VMX_HOST_FS_ENV)
		{
			 for (i = 0; i < NENV; i++) {
        			if (envs[i].env_type == ENV_TYPE_FS)
            			{
					to_env =  envs[i].env_id;
					break;
				}
			}
		}	

           int r = 	syscall(SYS_ipc_try_send,(uint64_t)to_env,tf->tf_regs.reg_rcx, tf->tf_regs.reg_rbx, tf->tf_regs.reg_rdi, 0);
         
           tf->tf_regs.reg_rax = r;   
	    
	    handled = true;
           break;

        case VMX_VMCALL_IPCRECV:
          // cprintf("\nCASE :  VMX_VMCALL_IPCRECV\n");
           tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);

	    int r1 = 	syscall(SYS_ipc_recv,tf->tf_regs.reg_rdx,0, 0, 0, 0);

           tf->tf_regs.reg_rax = r1;   
	    
	    handled = true;
           break;
    }
    if(handled) {
                   tf->tf_rip += vmcs_read32(VMCS_32BIT_VMEXIT_INSTRUCTION_LENGTH);
    }
    return handled;
}

