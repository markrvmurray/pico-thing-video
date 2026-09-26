/* mem_ops.c – FreeBSD style
 *
 * memcpy, memmove, memset and bzero that move WORDS, replacing the C
 * library's.
 *
 * WHY THIS EXISTS. The image that gets flashed is built with ATfE, and every
 * C-library variant ATfE 22 ships is a SIZE build ("..._unaligned_size" is
 * what this target links). In that build memset and memcpy are four-
 * instruction loops that move ONE BYTE per iteration -- read the disassembly:
 * memset is a single strb, memcpy a single ldrb/strb pair. There is no speed
 * variant to select and no flag that changes it, and the SDK offers no
 * alternative on the RP2350 (its fast mem-ops wrap RP2040 bootrom routines).
 *
 * It had been costing the renderer on every path, not just the obvious ones.
 * Measured on hardware: a 480 KB framebuffer memset took 7.8 ms, 16 ns a byte,
 * which is PSRAM speed from SRAM; memcpy ran at the speed of a byte loop to and
 * from PSRAM. The text blitter, scrolling (memmove), window parking and every
 * fill went through these. The GCC build was never affected -- newlib ships
 * optimised assembly -- which is why nothing measured on a gcc tree could
 * have shown it, and why "build with both toolchains" is a standing rule here.
 *
 * Defined for BOTH toolchains, deliberately. Keeping newlib's under gcc would
 * mean the two builds run different code on every hot path, which is exactly
 * the divergence that has already hidden a clang-only fault for a day. One
 * implementation, tested once, everywhere.
 *
 * WORD ACCESSES ONLY WHERE BOTH SIDES ARE ALIGNED ALIKE. When source and
 * destination differ in alignment the copy falls back to bytes. That is slower
 * than a shifting copy would be, but it never issues an unaligned word access
 * -- which matters because the SDK notes that an unaligned copy is not safe in
 * peripheral space -- and it is correct by inspection. Optimise the mismatched
 * case only if a measurement ever shows it matters.
 *
 * THE RECURSION TRAP. A compiler that recognises a byte loop as "a memcpy" will
 * helpfully replace it with a call to memcpy -- which, inside memcpy, never
 * returns. This file MUST be built -ffreestanding -fno-builtin (and, for gcc,
 * -fno-tree-loop-distribute-patterns); CMake does so, and the disassembly has
 * been checked to call nothing. The host test builds it the same way for the
 * same reason, and for one more: were the host compiler to turn these loops
 * into calls to the host's own memcpy, the test would be exercising the host
 * library and passing whatever this file did.
 */

#include <stddef.h>
#include <stdint.h>

/* The host test renames these so they can sit beside the host's own. A plain
 * flag rather than a function-like macro passed on the command line, because
 * CMake silently DROPS function-like -D definitions: the first attempt left
 * the test object defining the bare names, failing to link, while its
 * companion check passed on nothing. */
#ifdef MEM_OPS_TEST_NAMES
#define MEM_OPS_NAME(x)	t_##x
#else
#define MEM_OPS_NAME(x)	x
#endif

void *
MEM_OPS_NAME(memcpy)(void *restrict dst, const void *restrict src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
		while (n != 0u && ((uintptr_t)d & 3u) != 0u) {
			*d++ = *s++;
			n--;
		}
		uint32_t *dw = (uint32_t *)(void *)d;
		const uint32_t *sw = (const uint32_t *)(const void *)s;

		/* Four words a turn: the loads grouped ahead of the stores so
		 * the core can pipeline them, and a quarter of the loop
		 * overhead. */
		while (n >= 16u) {
			const uint32_t a = sw[0], b = sw[1], c = sw[2],
			    e = sw[3];

			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
			dw += 4;
			sw += 4;
			n -= 16u;
		}
		while (n >= 4u) {
			*dw++ = *sw++;
			n -= 4u;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n-- != 0u)
		*d++ = *s++;
	return dst;
}

void *
MEM_OPS_NAME(memmove)(void *dst, const void *src, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;

	/* Forward is safe unless the destination starts INSIDE the source:
	 * then a forward copy would overwrite source bytes before reading
	 * them. Compared as integers, because pointer ordering between
	 * unrelated objects is not something C will promise. */
	if ((uintptr_t)d <= (uintptr_t)s ||
	    (uintptr_t)d >= (uintptr_t)s + n)
		return MEM_OPS_NAME(memcpy)(dst, src, n);

	/* Backward. Scrolling the framebuffer down is exactly this case, and
	 * it is half a megabyte, so it gets words too. */
	d += n;
	s += n;
	if ((((uintptr_t)d ^ (uintptr_t)s) & 3u) == 0u) {
		while (n != 0u && ((uintptr_t)d & 3u) != 0u) {
			*--d = *--s;
			n--;
		}
		uint32_t *dw = (uint32_t *)(void *)d;
		const uint32_t *sw = (const uint32_t *)(const void *)s;

		while (n >= 4u) {
			*--dw = *--sw;
			n -= 4u;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n-- != 0u)
		*--d = *--s;
	return dst;
}

void *
MEM_OPS_NAME(memset)(void *dst, int c, size_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t b = (uint8_t)c;

	while (n != 0u && ((uintptr_t)d & 3u) != 0u) {
		*d++ = b;
		n--;
	}
	const uint32_t w = (uint32_t)b * 0x01010101u;
	uint32_t *dw = (uint32_t *)(void *)d;

	while (n >= 16u) {
		dw[0] = w;
		dw[1] = w;
		dw[2] = w;
		dw[3] = w;
		dw += 4;
		n -= 16u;
	}
	while (n >= 4u) {
		*dw++ = w;
		n -= 4u;
	}
	d = (uint8_t *)dw;
	while (n-- != 0u)
		*d++ = b;
	return dst;
}

void
MEM_OPS_NAME(bzero)(void *dst, size_t n)
{
	(void)MEM_OPS_NAME(memset)(dst, 0, n);
}

/* The ARM run-time ABI entry points. The compiler calls these directly -- in
 * the ATfE image __aeabi_memset has more call sites than memset and memcpy
 * together -- so replacing only the ISO names would have left most of the
 * slow paths exactly where they were. Note __aeabi_memset takes (dst, n, c):
 * the length and value are the other way round from memset. The 4- and
 * 8-suffixed forms promise alignment; the general code does not need the
 * promise, so they share it. */
#if defined(__ARM_EABI__)
void __aeabi_memcpy(void *d, const void *s, size_t n) { (void)memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { (void)memcpy(d, s, n); }
void __aeabi_memcpy8(void *d, const void *s, size_t n) { (void)memcpy(d, s, n); }
void __aeabi_memmove(void *d, const void *s, size_t n) { (void)memmove(d, s, n); }
void __aeabi_memmove4(void *d, const void *s, size_t n) { (void)memmove(d, s, n); }
void __aeabi_memmove8(void *d, const void *s, size_t n) { (void)memmove(d, s, n); }
void __aeabi_memset(void *d, size_t n, int c) { (void)memset(d, c, n); }
void __aeabi_memset4(void *d, size_t n, int c) { (void)memset(d, c, n); }
void __aeabi_memset8(void *d, size_t n, int c) { (void)memset(d, c, n); }
void __aeabi_memclr(void *d, size_t n) { (void)memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { (void)memset(d, 0, n); }
void __aeabi_memclr8(void *d, size_t n) { (void)memset(d, 0, n); }
#endif
