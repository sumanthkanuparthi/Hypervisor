#include <inc/lib.h>
#include <inc/vmx.h>
#include <inc/elf.h>
#include <inc/ept.h>

#define GUEST_KERN "/vmm/kernel"
#define GUEST_BOOT "/vmm/boot"

#define JOS_ENTRY 0x7000

// Map a region of file fd into the guest at guest physical address gpa.
// The file region to map should start at fileoffset and be length filesz.
// The region to map in the guest should be memsz.  The region can span multiple pages.
//
// Return 0 on success, <0 on failure.

//
static int
map_in_guest( envid_t guest, uintptr_t gpa, size_t memsz, 
        int fd, size_t filesz, off_t fileoffset ) {

      int i, r;
    void *blk;


    if ((i = PGOFF(gpa))) {
        gpa-= i;
        memsz += i;
        filesz += i;
        fileoffset -= i;
    }
   
    for (i = 0; i < memsz; i += PGSIZE) { {


            // from file
            if ((r = sys_page_alloc(0, UTEMP, PTE_P|PTE_U|PTE_W)) < 0)
                return r;
            if ((r = seek(fd, fileoffset + i)) < 0)
                return r;
            if ((r = readn(fd, UTEMP, MIN(PGSIZE, filesz-i))) < 0)
                return r;
            if ((r = sys_ept_map(0, UTEMP, guest, (void*) (gpa + i), PTE_P|PTE_U|PTE_W)) < 0)
		         return r;

            sys_page_unmap(0, UTEMP);
        }
    }

    return 0;

} 

// Read the ELF headers of kernel file specified by fname,
// mapping all valid segments into guest physical memory as appropriate.
//
// Return 0 on success, <0 on error
//
// Hint: compare with ELF parsing in env.c, and use map_in_guest for each segment.
static int
copy_guest_kern_gpa( envid_t guest, char* fname ) {

	int fd;
    	if ((fd = open( GUEST_KERN, O_RDONLY)) < 0 ) {
        	cprintf("open %s for read: %e\n", GUEST_KERN, fd );
        	exit();
    	}
    struct Elf *elf;


    unsigned char elf_buf[512];
    elf = (struct Elf*) elf_buf;
    if (readn(fd, elf_buf, sizeof(elf_buf)) != sizeof(elf_buf)
            || elf->e_magic != ELF_MAGIC) {
        close(fd);
        cprintf("elf magic %08x want %08x\n", elf->e_magic, ELF_MAGIC);
        return -E_NOT_EXEC;
    }


    struct Proghdr *ph, *eph;
    struct Page *p = NULL;
    ph = (struct Proghdr *) ((uint8_t *) elf + elf->e_phoff);
    eph = ph + elf->e_phnum;

    for (; ph < eph; ph++) {
	if (ph->p_type == ELF_PROG_LOAD) 
   {
	    uint64_t va = ph->p_va;
	    uint64_t size = ph->p_memsz;
	    uint64_t offset = ph->p_offset;
	    uint64_t i = 0;
	    if (ph->p_filesz > ph->p_memsz)
		{
		cprintf("Wrong size in elf binary\n");
		return -1;
		}
  		cprintf("%x\n\n",ph->p_pa+ph->p_offset);

          int ret = map_in_guest(guest, ph->p_pa , ph->p_memsz, fd, ph->p_filesz, ph->p_offset);

             if(ret < 0)
			return ret;
     
	}
    }
  return 0;
}

void
umain(int argc, char **argv) {
    int ret;
    envid_t guest;


    if ((ret = sys_env_mkguest( GUEST_MEM_SZ, JOS_ENTRY )) < 0) {
        cprintf("Error creating a guest OS env: %e\n", ret );
        exit();
    }
    guest = ret;


    // Copy the guest kernel code into guest phys mem.
    if((ret = copy_guest_kern_gpa(guest, GUEST_KERN)) < 0) {
	cprintf("Error copying page into the guest - %d.\n", ret);
        exit();
    }


    // Now copy the bootloader.
    int fd;
    if ((fd = open( GUEST_BOOT, O_RDONLY)) < 0 ) {
        cprintf("open %s for read: %e\n", GUEST_BOOT, fd );
        exit();
    }

    // sizeof(bootloader) < 512.
    if ((ret = map_in_guest(guest, JOS_ENTRY, 512, fd, 512, 0)) < 0) {
	cprintf("Error mapping bootloader into the guest - %d\n.", ret);
	exit();
    }


    // Mark the guest as runnable.
    sys_env_set_status(guest, ENV_RUNNABLE);
    wait(guest);

}


