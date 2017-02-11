#include "sysconfig.h"
#include "sysdeps.h"
#include "include/memory.h"
#include "uae/mman.h"
#include "uae/vm.h"
#include "options.h"
#include "autoconf.h"
#include "gfxboard.h"
#include "rommgr.h"
#include "newcpu.h"

#ifdef __x86_64__
static int os_64bit = 1;
#else
static int os_64bit = 0;
#endif

#include <sys/mman.h>

#define MEM_COMMIT       0x00001000
#define MEM_RESERVE      0x00002000
#define MEM_DECOMMIT         0x4000
#define MEM_RELEASE          0x8000
#define MEM_WRITE_WATCH  0x00200000
#define MEM_TOP_DOWN     0x00100000

#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04

typedef void * LPVOID;
typedef size_t SIZE_T;

typedef struct {
	int dwPageSize;
} SYSTEM_INFO;

static void GetSystemInfo(SYSTEM_INFO *si)
{
	si->dwPageSize = sysconf(_SC_PAGESIZE);
}

#define USE_MMAP

#ifdef USE_MMAP
#ifdef MACOSX
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

static void *VirtualAlloc(void *lpAddress, size_t dwSize, int flAllocationType,
	int flProtect)
{
	write_log("- VirtualAlloc addr=%p size=%zu type=%d prot=%d\n",
		lpAddress, dwSize, flAllocationType, flProtect);
	if (flAllocationType & MEM_RESERVE) {
		write_log("  MEM_RESERVE\n");
	}
	if (flAllocationType & MEM_COMMIT) {
		write_log("  MEM_COMMIT\n");
	}

	int prot = 0;
	if (flProtect == PAGE_READWRITE) {
		write_log("  PAGE_READWRITE\n");
		prot = UAE_VM_READ_WRITE;
	}
	else if (flProtect == PAGE_READONLY) {
		write_log("  PAGE_READONLY\n");
		prot = UAE_VM_READ;
	}
	else if (flProtect == PAGE_EXECUTE_READWRITE) {
		write_log("  PAGE_EXECUTE_READWRITE\n");
		prot = UAE_VM_READ_WRITE_EXECUTE;
	}
	else {
		write_log("  WARNING: unknown protection\n");
	}

	void *address = NULL;

	if (flAllocationType == MEM_COMMIT && lpAddress == NULL) {
		write_log("NATMEM: Allocated non-reserved memory size %zu\n", dwSize);
		void *memory = uae_vm_alloc(dwSize, 0, UAE_VM_READ_WRITE);
		if (memory == NULL) {
			write_log("memory allocated failed errno %d\n", errno);
		}
		return memory;
	}

	if (flAllocationType & MEM_RESERVE) {
		address = uae_vm_reserve(dwSize, 0);
	}
	else {
		address = lpAddress;
	}

	if (flAllocationType & MEM_COMMIT) {
		write_log("commit prot=%d\n", prot);
		uae_vm_commit(address, dwSize, prot);
	}

	return address;
}

static int VirtualProtect(void *lpAddress, int dwSize, int flNewProtect,
	unsigned int *lpflOldProtect)
{
	write_log("- VirtualProtect addr=%p size=%d prot=%d\n",
		lpAddress, dwSize, flNewProtect);
	int prot = 0;
	if (flNewProtect == PAGE_READWRITE) {
		write_log("  PAGE_READWRITE\n");
		prot = UAE_VM_READ_WRITE;
	}
	else if (flNewProtect == PAGE_READONLY) {
		write_log("  PAGE_READONLY\n");
		prot = UAE_VM_READ;
	}
	else {
		write_log("  -- unknown protection --\n");
	}
	if (uae_vm_protect(lpAddress, dwSize, prot) == 0) {
		write_log("mprotect failed errno %d\n", errno);
		return 0;
	}
	return 1;
}

static bool VirtualFree(void *lpAddress, size_t dwSize, int dwFreeType)
{
	int result = 0;
	if (dwFreeType == MEM_DECOMMIT) {
		return uae_vm_decommit(lpAddress, dwSize);
	}
	else if (dwFreeType == MEM_RELEASE) {
		return uae_vm_free(lpAddress, dwSize);
	}
	return 0;
}

static int GetLastError()
{
	return errno;
}

static int my_getpagesize(void)
{
	return uae_vm_page_size();
}

#define getpagesize my_getpagesize

//uae_u8* natmem_offset = nullptr;
//uae_u32 natmem_size;
//static uae_u64 totalAmigaMemSize;
//#define MAXAMIGAMEM 0x6000000 // 64 MB (16 MB for standard Amiga stuff, 16 MG RTG, 64 MB Z3 fast)
//uae_u32 max_z3fastmem;
//
///* JIT can access few bytes outside of memory block of it executes code at the very end of memory block */
//#define BARRIER 32
//
//static uae_u8* additional_mem = static_cast<uae_u8*>(MAP_FAILED);
//#define ADDITIONAL_MEMSIZE (128 + 16) * 1024 * 1024
//
//int z3_start_adr = 0;
//int rtg_start_adr = 0;
//
//
//void free_AmigaMem()
//{
//	if (natmem_offset != nullptr)
//	{
//		free(natmem_offset);
//		natmem_offset = nullptr;
//	}
//	if (additional_mem != MAP_FAILED)
//	{
//		munmap(additional_mem, ADDITIONAL_MEMSIZE);
//		additional_mem = static_cast<uae_u8*>(MAP_FAILED);
//	}
//}
//
//
//void alloc_AmigaMem()
//{
//	int i;
//	uae_u64 total;
//	int max_allowed_mman;
//
//	free_AmigaMem();
//
//	// First attempt: allocate 16 MB for all memory in 24-bit area 
//	// and additional mem for Z3 and RTG at correct offset
//	natmem_size = 16 * 1024 * 1024;
//	natmem_offset = static_cast<uae_u8*>(valloc(natmem_size));
//	max_z3fastmem = ADDITIONAL_MEMSIZE - 16 * 1024 * 1024;
//	if (!natmem_offset)
//	{
//		write_log("Can't allocate 16M of virtual address space!?\n");
//		abort();
//	}
//	additional_mem = static_cast<uae_u8*>(mmap(natmem_offset + 0x10000000, ADDITIONAL_MEMSIZE + BARRIER,
//	                                           PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0));
//	if (additional_mem != MAP_FAILED)
//	{
//		// Allocation successful -> we can use natmem_offset for entire memory access
//		z3_start_adr = 0x10000000;
//		rtg_start_adr = 0x18000000;
//		write_log("Allocated 16 MB for 24-bit area and %d MB for Z3 and RTG\n", ADDITIONAL_MEMSIZE / (1024 * 1024));
//		return;
//	}
//	free(natmem_offset);
//
//	// Second attempt: allocate huge memory block for entire area
//	natmem_size = ADDITIONAL_MEMSIZE + 256 * 1024 * 1024;
//	natmem_offset = static_cast<uae_u8*>(valloc(natmem_size + BARRIER));
//	if (natmem_offset)
//	{
//		// Allocation successful
//		z3_start_adr = 0x10000000;
//		rtg_start_adr = 0x18000000;
//		write_log("Allocated %d MB for entire memory\n", natmem_size / (1024 * 1024));
//		return;
//	}
//
//	// Third attempt: old style: 64 MB allocated and Z3 and RTG at wrong address
//
//	// Get max. available size
//	total = static_cast<uae_u64>(sysconf(_SC_PHYS_PAGES)) * static_cast<uae_u64>(getpagesize());
//
//	// Limit to max. 64 MB
//	natmem_size = total;
//	if (natmem_size > MAXAMIGAMEM)
//		natmem_size = MAXAMIGAMEM;
//
//	// We need at least 16 MB
//	if (natmem_size < 16 * 1024 * 1024)
//		natmem_size = 16 * 1024 * 1024;
//
//	write_log("Total physical RAM %lluM. Attempting to reserve: %uM.\n", total >> 20, natmem_size >> 20);
//	natmem_offset = static_cast<uae_u8*>(valloc(natmem_size + BARRIER));
//
//	if (!natmem_offset)
//	{
//		for (;;)
//		{
//			natmem_offset = static_cast<uae_u8*>(valloc(natmem_size + BARRIER));
//			if (natmem_offset)
//				break;
//			natmem_size -= 16 * 1024 * 1024;
//			if (!natmem_size)
//			{
//				write_log("Can't allocate 16M of virtual address space!?\n");
//				abort();
//			}
//		}
//	}
//
//	z3_start_adr = 0x01000000;
//	rtg_start_adr = 0x03000000;
//	max_z3fastmem = natmem_size - 32 * 1024 * 1024;
//	if (max_z3fastmem <= 0)
//	{
//		z3_start_adr = 0x00000000; // No mem for Z3
//		if (max_z3fastmem == 0)
//			rtg_start_adr = 0x01000000; // We have mem for RTG
//		else
//			rtg_start_adr = 0x00000000; // No mem for expansion at all
//		max_z3fastmem = 0;
//	}
//	write_log("Reserved: %p-%p (0x%08x %dM)\n", natmem_offset, (uae_u8*)natmem_offset + natmem_size,
//		natmem_size, natmem_size >> 20);
//}
//
//
//static unsigned long getz2rtgaddr(void)
//{
//	unsigned long start;
//	start = changed_prefs.fastmem_size;
//	while (start & (changed_prefs.rtgmem_size - 1) && start < 4 * 1024 * 1024)
//		start += 1024 * 1024;
//	return start + 2 * 1024 * 1024;
//}
//
//
//uae_u8* mapped_malloc(size_t s, const char* file)
//{
//	if (!strcmp(file, "chip"))
//		return natmem_offset + chipmem_start_addr;
//
//	if (!strcmp(file, "fast"))
//		return natmem_offset + 0x200000;
//
//	if (!strcmp(file, "bogo"))
//		return natmem_offset + bogomem_start_addr;
//
//	if (!strcmp(file, "rom_f0"))
//		return natmem_offset + 0xf00000;
//
//	if (!strcmp(file, "rom_e0"))
//		return natmem_offset + 0xe00000;
//
//	if (!strcmp(file, "rom_a8"))
//		return natmem_offset + 0xa80000;
//
//	if (!strcmp(file, "kick"))
//		return natmem_offset + kickmem_start_addr;
//
//	if (!strcmp(file, "z3"))
//		return natmem_offset + z3_start_adr; //z3fastmem_start;
//
//#ifdef PICASSO96
//	if (!strcmp(file, "z3_gfx"))
//	{
//		gfxmem_bank.start = rtg_start_adr;
//		return natmem_offset + rtg_start_adr;
//	}
//
//	if (!strcmp(file, "z2_gfx"))
//	{
//		gfxmem_bank.start = getz2rtgaddr();
//		return natmem_offset + gfxmem_bank.start;
//	}
//#endif
//	if (!strcmp(file, "rtarea"))
//		return natmem_offset + rtarea_base;
//
//	return nullptr;
//}
//
//
//void mapped_free(uae_u8* p)
//{
//}
//
//
//void protect_roms(bool protect)
//{
//	/*
//	  If this code is enabled, we can't switch back from JIT to nonJIT emulation...
//	  
//		if (protect) {
//			// protect only if JIT enabled, always allow unprotect
//			if (!currprefs.cachesize)
//				return;
//		}
//	
//	  // Protect all regions, which contains ROM
//	  if(extendedkickmem_bank.baseaddr != NULL)
//	    mprotect(extendedkickmem_bank.baseaddr, 0x80000, protect ? PROT_READ : PROT_READ | PROT_WRITE);
//	  if(extendedkickmem2_bank.baseaddr != NULL)
//	    mprotect(extendedkickmem2_bank.baseaddr, 0x80000, protect ? PROT_READ : PROT_READ | PROT_WRITE);
//	  if(kickmem_bank.baseaddr != NULL)
//	    mprotect(kickmem_bank.baseaddr, 0x80000, protect ? PROT_READ : PROT_READ | PROT_WRITE);
//	  if(rtarea != NULL)
//	    mprotect(rtarea, RTAREA_SIZE, protect ? PROT_READ : PROT_READ | PROT_WRITE);
//	  if(filesysory != NULL)
//	    mprotect(filesysory, 0x10000, protect ? PROT_READ : PROT_READ | PROT_WRITE);
//	*/
//}
