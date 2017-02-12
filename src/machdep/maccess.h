/*
 * UAE - The Un*x Amiga Emulator
 *
 * Memory access functions
 *
 * Copyright 1996 Bernd Schmidt
 */

#ifndef MACCESS_UAE_H
#define MACCESS_UAE_H

#ifdef ARMV6_ASSEMBLY

STATIC_INLINE uae_u16 do_get_mem_word(uae_u16 *a)
{
    uae_u16 v;
    __asm__ (
        "ldrh %[v], [%[a]] \n\t"
        "rev16 %[v], %[v] \n\t"
        : [v] "=r" (v) : [a] "r" (a) );
    return v;
}
#else
static __inline__ uint16_t do_get_mem_word(uint16_t *a)
{
	uint8_t *b = (uint8_t *)a;

	return (*b << 8) | (*(b + 1));
}
#endif


#ifdef ARMV6_ASSEMBLY

STATIC_INLINE uae_u32 do_get_mem_long(uae_u32 *a)
{
    uae_u32 v;
    __asm__ (
        "ldr %[v], [%[a]] \n\t"
        "rev %[v], %[v] \n\t"
        : [v] "=r" (v) : [a] "r" (a) );
    return v;
}
#else
static __inline__ uint32_t do_get_mem_long(uint32_t *a)
{
	uint8_t *b = (uint8_t *)a;

	return (*b << 24) | (*(b + 1) << 16) | (*(b + 2) << 8) | (*(b + 3));
}
#endif


static __inline__ uint8_t do_get_mem_byte(uint8_t *a)
{
	return *a;
}

#ifdef ARMV6_ASSEMBLY
STATIC_INLINE void do_put_mem_word(uae_u16 *a, uae_u16 v)
{
    __asm__ (
        "rev16 r2, %[v] \n\t"
        "strh r2, [%[a]] \n\t"
        : : [v] "r" (v), [a] "r" (a) : "r2", "memory" );
}
#else
static __inline__ void do_put_mem_word(uint16_t *a, uint16_t v)
{
	uint8_t *b = (uint8_t *)a;

	*b = v >> 8;
	*(b + 1) = v;
}
#endif

#ifdef ARMV6_ASSEMBLY
STATIC_INLINE void do_put_mem_long(uae_u32 *a, uae_u32 v)
{
    __asm__ (
        "rev r2, %[v] \n\t"
        "str r2, [%[a]] \n\t"
        : : [v] "r" (v), [a] "r" (a) : "r2", "memory" );
}
#else
static __inline__ void do_put_mem_long(uint32_t *a, uint32_t v)
{
	uint8_t *b = (uint8_t *)a;

	*b = v >> 24;
	*(b + 1) = v >> 16;
	*(b + 2) = v >> 8;
	*(b + 3) = v;
}
#endif

static __inline__ void do_put_mem_byte(uint8_t *a, uint8_t v)
{
	*a = v;
}

#define ALIGN_POINTER_TO32(p) ((~(unsigned long)(p)) & 3)

#define call_mem_get_func(func, addr) ((*func)(addr))
#define call_mem_put_func(func, addr, v) ((*func)(addr, v))

#undef USE_MAPPED_MEMORY
#undef CAN_MAP_MEMORY
#undef NO_INLINE_MEMORY_ACCESS
#undef MD_HAVE_MEM_1_FUNCS

#endif
