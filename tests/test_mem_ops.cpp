// test_mem_ops.cpp -- the firmware's own memcpy/memmove/memset/bzero, tested
// exhaustively on the host.
//
// These replace the C library's on EVERY path in the firmware (ATfE ships only
// size-optimised libraries whose versions move a byte per iteration), so a
// subtle fault here would corrupt the framebuffer, the text grid, the parked
// windows and the command FIFO all at once -- and would look like any of them
// being broken rather than like this. So nothing is sampled: every source and
// destination alignment from 0 to 7, every length from 0 through well past the
// 16-byte unrolled block, guard bytes either side, and overlapping moves in
// both directions at every delta a word loop could get wrong.
//
// mem_ops.c is compiled here under renamed symbols (t_memcpy and so on) and
// with -ffreestanding -fno-builtin. Without that the host compiler may
// recognise the loops and replace them with calls to the host's own memcpy,
// and this file would then be testing the host library and passing whatever
// mem_ops.c did. A separate CTest check (mem_ops_no_libc_calls) asserts the
// compiled object references none of them.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <cstddef>

extern "C" {
void *t_memcpy(void *, const void *, size_t);
void *t_memmove(void *, const void *, size_t);
void *t_memset(void *, int, size_t);
void t_bzero(void *, size_t);
}

namespace {

constexpr size_t MAX_N = 100;		// past 6 x 16-byte blocks + tails
constexpr size_t PAD = 16;
constexpr size_t BUF = PAD + 8 + MAX_N + 8 + PAD;
constexpr uint8_t GUARD = 0xC7;

// Content no word-sized shift can reproduce: a byte-at-offset ramp would be
// invariant under some shifts, which is the same symmetry trap that has
// already let a block test here pass while the picture was wrong.
uint8_t pattern(size_t i) { return (uint8_t)(i * 37u + (i >> 3) * 11u + 5u); }

}

TEST_CASE("memcpy: every alignment pair and length", "[mem_ops]")
{
	for (size_t so = 0; so < 8; so++)
		for (size_t dofs = 0; dofs < 8; dofs++)
			for (size_t n = 0; n <= MAX_N; n++) {
				alignas(8) uint8_t src[BUF], dst[BUF];

				for (size_t i = 0; i < BUF; i++) {
					src[i] = pattern(i);
					dst[i] = GUARD;
				}
				uint8_t *d = dst + PAD + dofs;
				const uint8_t *s = src + PAD + so;
				void *r = t_memcpy(d, s, n);

				REQUIRE(r == d);
				REQUIRE(std::memcmp(d, s, n) == 0);
				for (size_t i = 0; i < PAD + dofs; i++)
					REQUIRE(dst[i] == GUARD);
				for (size_t i = PAD + dofs + n; i < BUF; i++)
					REQUIRE(dst[i] == GUARD);
			}
}

TEST_CASE("memset: every alignment and length, low byte only", "[mem_ops]")
{
	// 0x1A5 checks that only the low byte of the int is used -- a word fill
	// built from the whole int would write 0xA5A5 wrongly shifted bytes.
	const int values[] = { 0x00, 0xFF, 0xA5, 0x5A, 0x1A5, -1 };

	for (int v : values)
		for (size_t dofs = 0; dofs < 8; dofs++)
			for (size_t n = 0; n <= MAX_N; n++) {
				alignas(8) uint8_t dst[BUF];

				std::memset(dst, GUARD, BUF);
				uint8_t *d = dst + PAD + dofs;
				void *r = t_memset(d, v, n);

				REQUIRE(r == d);
				for (size_t i = 0; i < n; i++)
					REQUIRE(d[i] == (uint8_t)v);
				for (size_t i = 0; i < PAD + dofs; i++)
					REQUIRE(dst[i] == GUARD);
				for (size_t i = PAD + dofs + n; i < BUF; i++)
					REQUIRE(dst[i] == GUARD);
			}
}

TEST_CASE("bzero: every alignment and length", "[mem_ops]")
{
	for (size_t dofs = 0; dofs < 8; dofs++)
		for (size_t n = 0; n <= MAX_N; n++) {
			alignas(8) uint8_t dst[BUF];

			std::memset(dst, GUARD, BUF);
			t_bzero(dst + PAD + dofs, n);
			for (size_t i = 0; i < BUF; i++) {
				const bool in = i >= PAD + dofs &&
				    i < PAD + dofs + n;
				REQUIRE(dst[i] == (in ? 0 : GUARD));
			}
		}
}

TEST_CASE("memmove: overlapping in both directions", "[mem_ops]")
{
	// Every delta from -9 to +9 covers: identical pointers, overlap by less
	// than a word, exactly a word, and more than a word, both ways. Scrolling
	// the framebuffer is the real customer, in both directions.
	for (int delta = -9; delta <= 9; delta++)
		for (size_t base = 0; base < 8; base++)
			for (size_t n = 0; n <= MAX_N; n++) {
				alignas(8) uint8_t buf[BUF], want[BUF];

				for (size_t i = 0; i < BUF; i++)
					buf[i] = pattern(i);
				const size_t s = PAD + 8 + base;
				const size_t d = (size_t)((long)s + delta);

				// Reference: via a separate temporary, which is
				// correct by construction for any overlap.
				std::memcpy(want, buf, BUF);
				uint8_t tmp[MAX_N];
				std::memcpy(tmp, buf + s, n);
				std::memcpy(want + d, tmp, n);

				void *r = t_memmove(buf + d, buf + s, n);

				REQUIRE(r == buf + d);
				REQUIRE(std::memcmp(buf, want, BUF) == 0);
			}
}
