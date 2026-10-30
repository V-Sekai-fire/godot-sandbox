#include <catch2/catch_test_macros.hpp>
#include <libriscv/common.hpp>
#include <libriscv/native_heap.hpp>
#include <vector>
static const uintptr_t BEGIN = 0x1000000;
static const uintptr_t END   = 0x2000000;
#define IS_WITHIN(addr) (addr >= BEGIN && addr < END)
#define HPRINT(fmt, ...) /* */

int randInt(int min, int max) {
	return min + (rand() % static_cast<int>(max - min + 1));
}
unsigned randUpto(unsigned max) {
	return rand() % max;
}

struct Allocation {
	uint64_t  addr;
	size_t    size;
};

static Allocation alloc_random(riscv::Arena& arena)
{
	const size_t size = randInt(0, 8000);
	const uintptr_t addr = arena.malloc(size);
	REQUIRE(IS_WITHIN(addr));
	const Allocation a {
		.addr = addr, .size = arena.size(addr)
	};
	REQUIRE(a.size >= size);
	return a;
}

static Allocation alloc_sequential(riscv::Arena& arena)
{
	const size_t size	 = randInt(0, 4096);
	const uintptr_t addr = arena.seq_alloc_aligned(size, 8, false);
	REQUIRE(IS_WITHIN(addr));
	// In order for the memory to be sequential in both the
	// host and the guest, it must be on the same page. We explicitly
	// disable the flat read-write arena optimization for this test.
	if (size > 0 && size < RISCV_PAGE_SIZE)
	{
		const auto page1 = addr & ~(RISCV_PAGE_SIZE - 1);
		const auto page2 = (addr + size-1) & ~(RISCV_PAGE_SIZE - 1);
		REQUIRE(page1 == page2);
	}
	const Allocation a {.addr = addr, .size = arena.size(addr)};
	REQUIRE(a.size >= size);
	return a;
}

static std::tuple<Allocation, size_t>
realloc_random(riscv::Arena& arena, uint64_t addr)
{
	REQUIRE(IS_WITHIN(addr));
	const size_t size = randInt(0, 8000);
	auto [newaddr, len] = arena.realloc(addr, size);
	REQUIRE(IS_WITHIN(newaddr));
	const Allocation a {
		.addr = newaddr, .size = arena.size(newaddr)
	};
	REQUIRE(a.size >= size);
	return {a, size};
}

TEST_CASE("Basic heap usage", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};
	std::vector<Allocation> allocs;

	// General allocation test
	for (int i = 0; i < 1000; i++) {
		allocs.push_back(alloc_random(arena));
		allocs.push_back(alloc_sequential(arena));
	}

	for (auto entry : allocs) {
		REQUIRE(arena.size(entry.addr) == entry.size);
	  	REQUIRE(arena.free(entry.addr) == 0);
	}
	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
	allocs.clear();

	// Randomized allocations
	for (int i = 0; i < 10000; i++)
	{
		const int A = randInt(2, 50);
		for (int a = 0; a < A; a++) {
			allocs.push_back(alloc_random(arena));
			[[maybe_unused]] const auto alloc = allocs.back();
			HPRINT("Alloc %lX size: %4zu,  arena size: %4zu\n",
				alloc.addr, alloc.size, arena.size(alloc.addr));
		}
		const int B = std::min(randInt(2, allocs.size()), (int)allocs.size());
		for (int b = 0; b < B; b++) {
			auto& origin = allocs.at(b);
			const auto [alloc, size] = realloc_random(arena, origin.addr);
			HPRINT("Realloc %lX size: %4zu, arena size: %4zu  (origin %lX oldsize %zu)\n",
				alloc.addr, size, alloc.size, origin.addr, origin.size);
			if (alloc.addr == origin.addr) {
				origin.size = alloc.size;
				REQUIRE(arena.size(origin.addr) == origin.size);
			} else {
				// The old allocation has just been freed
				REQUIRE(arena.size(origin.addr) == 0);
				REQUIRE(arena.free(origin.addr) == -1);
				allocs.erase(allocs.begin() + b, allocs.begin() + b + 1);
				// Add the new reallocated address
				allocs.push_back(alloc);
				REQUIRE(arena.size(alloc.addr) == alloc.size);
			}
		}
		const int F = randInt(2, allocs.size());
		for (int f = 0; f < F && !allocs.empty(); f++) {
			const auto idx = randUpto(allocs.size());
			const auto alloc = allocs.at(idx);
			allocs.erase(allocs.begin() + idx, allocs.begin() + idx + 1);
			HPRINT("Free %lX size: %4zu, arena size: %4zu\n",
				alloc.addr, alloc.size, arena.size(alloc.addr));
			REQUIRE(arena.size(alloc.addr) == alloc.size);
	  		REQUIRE(arena.free(alloc.addr) == 0);
		}
	}
	// Verify all allocations still remaining
	for (auto entry : allocs) {
		REQUIRE(arena.size(entry.addr) == entry.size);
	}
	// Verify allocations still remaining, then free them
	for (auto entry : allocs) {
		REQUIRE(arena.size(entry.addr) == entry.size);
		REQUIRE(arena.free(entry.addr) == 0);
		REQUIRE(arena.size(entry.addr) == 0);
	}
	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
	allocs.clear();
}

TEST_CASE("Allocate too many chunks", "[Heap]")
{
	REQUIRE_THROWS([] {
		riscv::Arena arena {BEGIN, END};
		while (true)
			arena.malloc(4);
	}());

	REQUIRE_THROWS([] {
		riscv::Arena arena {BEGIN, END};
		arena.set_max_chunks(0);
		arena.malloc(4);
	}());
}

TEST_CASE("Alignment and minimum allocation size", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};
	static constexpr size_t ALIGN = riscv::Arena::ALIGNMENT;

	// malloc(0) must return a valid in-range address with the minimum chunk size
	auto addr = arena.malloc(0);
	REQUIRE(IS_WITHIN(addr));
	REQUIRE(arena.size(addr) == ALIGN);
	REQUIRE((addr % ALIGN) == 0);
	REQUIRE(arena.free(addr) == 0);

	// malloc(1) must also give minimum chunk size
	addr = arena.malloc(1);
	REQUIRE(IS_WITHIN(addr));
	REQUIRE(arena.size(addr) == ALIGN);
	REQUIRE(arena.free(addr) == 0);

	// Every allocation must be ALIGN-byte aligned and large enough
	for (int i = 1; i <= 300; i++) {
		const size_t req = (size_t)i * 7;
		addr = arena.malloc(req);
		REQUIRE(IS_WITHIN(addr));
		REQUIRE((addr % ALIGN) == 0);
		REQUIRE(arena.size(addr) >= req);
		REQUIRE(arena.free(addr) == 0);
	}

	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
}

TEST_CASE("size() semantics", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};

	// Unallocated and out-of-range pointers return 0
	REQUIRE(arena.size(0) == 0);
	REQUIRE(arena.size(BEGIN - 1) == 0);
	REQUIRE(arena.size(BEGIN) == 0);   // not yet allocated
	REQUIRE(arena.size(END) == 0);

	auto addr = arena.malloc(100);
	REQUIRE(IS_WITHIN(addr));
	const size_t sz = arena.size(addr);
	REQUIRE(sz >= 100);

	// Mid-chunk pointer (not the start) is not a valid allocation key
	REQUIRE(arena.size(addr + 1) == 0);

	// After free, size() returns 0
	REQUIRE(arena.free(addr) == 0);
	REQUIRE(arena.size(addr) == 0);
}

TEST_CASE("free() semantics", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};

	// free(0) returns -1 (null is never a valid allocation)
	REQUIRE(arena.free(0) == -1);

	// free of a never-allocated address returns -1
	REQUIRE(arena.free(BEGIN + 64) == -1);

	// Normal allocation and free succeeds
	auto addr = arena.malloc(128);
	REQUIRE(arena.free(addr) == 0);

	// Double-free returns -1
	REQUIRE(arena.free(addr) == -1);

	// Mid-chunk pointer also fails
	addr = arena.malloc(256);
	REQUIRE(arena.free(addr + 1) == -1);
	REQUIRE(arena.free(addr) == 0);      // proper free still works
}

TEST_CASE("realloc() semantics", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};
	using PT = riscv::Arena::PointerType;

	// realloc(0, size) acts like malloc
	{
		auto [addr, len] = arena.realloc(PT(0), 200);
		REQUIRE(IS_WITHIN(addr));
		REQUIRE(len == 0);  // no old data to copy
		REQUIRE(arena.size(addr) >= 200);
		REQUIRE(arena.free(addr) == 0);
	}

	// realloc shrink: same pointer returned, no copy needed
	{
		auto addr = arena.malloc(1024);
		auto [newaddr, len] = arena.realloc(addr, 64);
		REQUIRE(newaddr == addr);
		REQUIRE(len == 0);
		REQUIRE(arena.size(addr) >= 64);
		REQUIRE(arena.free(addr) == 0);
	}

	// realloc grow via subsume: next chunk is free, grow in-place
	{
		auto addr  = arena.malloc(512);
		auto next  = arena.malloc(512);
		arena.free(next);  // free the chunk immediately after addr

		auto [newaddr, len] = arena.realloc(addr, 900);
		REQUIRE(newaddr == addr);  // must not move
		REQUIRE(len == 0);
		REQUIRE(arena.size(addr) >= 900);
		REQUIRE(arena.free(addr) == 0);
	}

	// realloc grow that must move (next chunk is occupied)
	{
		auto addr    = arena.malloc(512);
		auto blocker = arena.malloc(64);  // occupy the space after addr

		auto [newaddr, len] = arena.realloc(addr, 1024);
		REQUIRE(IS_WITHIN(newaddr));
		REQUIRE(newaddr != addr);    // had to move
		REQUIRE(len > 0);            // old length returned so caller can copy
		REQUIRE(arena.size(addr) == 0);    // old chunk freed
		REQUIRE(arena.size(newaddr) >= 1024);
		REQUIRE(arena.free(newaddr) == 0);
		REQUIRE(arena.free(blocker) == 0);
	}

	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
}

TEST_CASE("Exact-fit allocation (split_next regression)", "[Heap]")
{
	// Regression test: allocating a free chunk whose size equals the request exactly
	// (no remainder split) must leave the linked list consistent.
	riscv::Arena arena {BEGIN, END};
	static constexpr size_t SZ = 256; // already ALIGNMENT-aligned

	// Build: P(used) → X(used) → N(used) → D(free)
	auto p = arena.malloc(SZ);
	auto x = arena.malloc(SZ);
	auto n = arena.malloc(SZ);

	// Free X to create a free hole of exactly SZ between two used chunks
	REQUIRE(arena.free(x) == 0);

	// malloc(SZ) must find X via exact fit and reuse its address
	auto x2 = arena.malloc(SZ);
	REQUIRE(x2 == x);
	REQUIRE(arena.size(x2) == SZ);

	// Neighbors must still be intact
	REQUIRE(arena.size(p) == SZ);
	REQUIRE(arena.size(n) == SZ);

	// Free everything and verify full arena recovery
	REQUIRE(arena.free(x2) == 0);
	REQUIRE(arena.free(p) == 0);
	REQUIRE(arena.free(n) == 0);
	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
}

TEST_CASE("Merge of adjacent free chunks", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};

	// Allocate three adjacent chunks A → B → C
	auto a = arena.malloc(512);
	auto b = arena.malloc(512);
	auto c = arena.malloc(512);

	const size_t sz_a = arena.size(a);
	const size_t sz_b = arena.size(b);
	const size_t sz_c = arena.size(c);

	// Free A then C, leaving B used between two free regions
	REQUIRE(arena.free(a) == 0);
	REQUIRE(arena.free(c) == 0);

	// Freeing B merges all three (and the trailing free space) into one chunk
	REQUIRE(arena.free(b) == 0);

	// Arena must be fully restored
	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);

	// A single malloc can now claim the combined space of A+B+C
	auto big = arena.malloc(sz_a + sz_b + sz_c);
	REQUIRE(IS_WITHIN(big));
	REQUIRE(arena.size(big) >= sz_a + sz_b + sz_c);
	REQUIRE(arena.free(big) == 0);
}

TEST_CASE("Allocation counters", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};
	REQUIRE(arena.allocation_counter()   == 0);
	REQUIRE(arena.deallocation_counter() == 0);

	auto a = arena.malloc(64);
	REQUIRE(arena.allocation_counter() == 1);
	REQUIRE(arena.deallocation_counter() == 0);

	auto b = arena.malloc(64);
	REQUIRE(arena.allocation_counter() == 2);

	arena.free(a);
	REQUIRE(arena.deallocation_counter() == 1);
	arena.free(b);
	REQUIRE(arena.deallocation_counter() == 2);

	// realloc that moves also counts as alloc + dealloc
	auto c   = arena.malloc(64);
	auto blk = arena.malloc(64);
	const unsigned alloc_before = arena.allocation_counter();
	const unsigned dealloc_before = arena.deallocation_counter();
	auto [newc, len] = arena.realloc(c, 512);
	if (newc != c) {
		// realloc moved: one new malloc + one free internally
		REQUIRE(arena.allocation_counter()   == alloc_before + 1);
		REQUIRE(arena.deallocation_counter() == dealloc_before + 1);
	}
	arena.free(newc);
	arena.free(blk);
}

TEST_CASE("high_watermark", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};

	// Empty arena: watermark equals the base address
	REQUIRE(arena.high_watermark() == BEGIN);

	auto a = arena.malloc(1024);
	const size_t sz_a = arena.size(a);
	REQUIRE(arena.high_watermark() == BEGIN + sz_a);

	auto b = arena.malloc(512);
	const size_t sz_b = arena.size(b);
	REQUIRE(arena.high_watermark() == BEGIN + sz_a + sz_b);

	// Freeing A (first alloc) does not lower the watermark while B is live
	arena.free(a);
	REQUIRE(arena.high_watermark() == BEGIN + sz_a + sz_b);

	// Freeing B lowers watermark back to base
	arena.free(b);
	REQUIRE(arena.high_watermark() == BEGIN);
}

TEST_CASE("bytes_used + bytes_free invariant", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};
	const size_t total = END - BEGIN;

	REQUIRE(arena.bytes_used() + arena.bytes_free() == total);

	std::vector<riscv::Arena::PointerType> ptrs;
	for (int i = 0; i < 100; i++) {
		ptrs.push_back(arena.malloc((size_t)(i + 1) * 16));
		REQUIRE(arena.bytes_used() + arena.bytes_free() == total);
	}

	for (auto p : ptrs) {
		arena.free(p);
		REQUIRE(arena.bytes_used() + arena.bytes_free() == total);
	}

	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == total);
}

TEST_CASE("Arena transfer (copy constructor)", "[Heap]")
{
	riscv::Arena src {BEGIN, END};

	auto a = src.malloc(256);
	auto b = src.malloc(512);
	auto c = src.malloc(128);
	src.free(b);  // leave a hole to copy too

	const size_t src_used = src.bytes_used();
	const size_t src_free = src.bytes_free();
	const size_t sz_a = src.size(a);
	const size_t sz_c = src.size(c);

	// Copy-construct a new arena from src
	riscv::Arena dst {src};

	// dst reflects src's state
	REQUIRE(dst.bytes_used() == src_used);
	REQUIRE(dst.bytes_free() == src_free);
	REQUIRE(dst.size(a) == sz_a);
	REQUIRE(dst.size(c) == sz_c);
	REQUIRE(dst.size(b) == 0);  // b was freed before copy

	// Operations on dst don't affect src
	dst.free(a);
	REQUIRE(src.size(a) == sz_a);  // src unchanged

	dst.free(c);
	REQUIRE(dst.bytes_used() == 0);
	REQUIRE(src.bytes_used() == src_used);

	src.free(a);
	src.free(c);
}

TEST_CASE("Unknown free/realloc callbacks", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};

	int free_calls = 0;
	int realloc_calls = 0;

	arena.on_unknown_free([&](riscv::Arena::PointerType, riscv::ArenaChunk*) -> int {
		free_calls++;
		return -1;
	});
	arena.on_unknown_realloc([&](riscv::Arena::PointerType, size_t) -> riscv::Arena::ReallocResult {
		realloc_calls++;
		return {0, 0};
	});

	// free of unknown pointer invokes callback
	arena.free(BEGIN + 0x100);
	REQUIRE(free_calls == 1);

	// double-free invokes callback
	auto addr = arena.malloc(64);
	arena.free(addr);
	arena.free(addr);
	REQUIRE(free_calls == 2);

	// realloc of unknown pointer invokes callback
	arena.realloc(BEGIN + 0x200, 128);
	REQUIRE(realloc_calls == 1);
}

TEST_CASE("seq_alloc_aligned page-boundary guarantee", "[Heap]")
{
	riscv::Arena arena {BEGIN, END};

	// With arena_is_flat=true it's just malloc
	auto addr = arena.seq_alloc_aligned(256, 8, true);
	REQUIRE(IS_WITHIN(addr));
	REQUIRE(arena.size(addr) >= 256);
	REQUIRE(arena.free(addr) == 0);

	// With arena_is_flat=false each allocation must fit on one page
	for (int i = 1; i <= 50; i++) {
		const size_t sz = (size_t)i * 64;
		if (sz >= RISCV_PAGE_SIZE) break;
		addr = arena.seq_alloc_aligned(sz, 8, false);
		REQUIRE(IS_WITHIN(addr));
		const auto page_start = addr & ~(uintptr_t)(RISCV_PAGE_SIZE - 1);
		const auto page_end   = (addr + sz - 1) & ~(uintptr_t)(RISCV_PAGE_SIZE - 1);
		REQUIRE(page_start == page_end);
		REQUIRE(arena.size(addr) >= sz);
		REQUIRE(arena.free(addr) == 0);
	}

	REQUIRE(arena.bytes_used() == 0);
	REQUIRE(arena.bytes_free() == END - BEGIN);
}
