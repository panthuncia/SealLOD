// The builder's allocations go to the TBB scalable allocator instead of the CRT heap, whose single lock had most
// workers blocked in it with every core building (see CMakeLists.txt).
//
// This replaces the program's global operator new and delete, which C++ allows, rather than patching the CRT the way
// tbbmalloc_proxy does: the proxy declines current UCRTs ("unknown prologue for function _msize") and silently leaves
// the CRT heap in place. Everything statically linked into this executable allocates through these: the builder,
// meshoptimizer (its allocator defaults to operator new) and the standard containers. Other modules keep their own
// heaps. A pointer another module allocated and this one deletes is handed back to the CRT by tbbmalloc's "safer"
// entry points, which recognise memory they did not allocate.

#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>

extern "C" {
	void* scalable_malloc(std::size_t size);
	void* scalable_aligned_malloc(std::size_t size, std::size_t alignment);
	void __TBB_malloc_safer_free(void* ptr, void (*original_free)(void*));
	void* __TBB_malloc_safer_aligned_realloc(void* ptr, std::size_t size, std::size_t alignment, void* original_function);
}

namespace
{
	void* Allocate(std::size_t size)
	{
		for (;;) {
			if (void* p = scalable_malloc(size != 0 ? size : 1)) return p;
			if (const std::new_handler handler = std::get_new_handler()) handler();
			else throw std::bad_alloc();
		}
	}

	void* AllocateAligned(std::size_t size, std::align_val_t alignment)
	{
		for (;;) {
			if (void* p = scalable_aligned_malloc(size != 0 ? size : 1, static_cast<std::size_t>(alignment))) return p;
			if (const std::new_handler handler = std::get_new_handler()) handler();
			else throw std::bad_alloc();
		}
	}

	void Free(void* p) noexcept
	{
		if (p) __TBB_malloc_safer_free(p, &std::free);
	}

	void FreeAligned(void* p, std::align_val_t alignment) noexcept
	{
		// Reallocating to nothing frees: tbbmalloc's own block, or through _aligned_realloc one it does not own.
		if (p) (void)__TBB_malloc_safer_aligned_realloc(p, 0, static_cast<std::size_t>(alignment), reinterpret_cast<void*>(&_aligned_realloc));
	}
}

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch (...) { return nullptr; } }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch (...) { return nullptr; } }
void* operator new(std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return AllocateAligned(size, alignment); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return AllocateAligned(size, alignment); } catch (...) { return nullptr; } }
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return AllocateAligned(size, alignment); } catch (...) { return nullptr; } }

void operator delete(void* p) noexcept { Free(p); }
void operator delete[](void* p) noexcept { Free(p); }
void operator delete(void* p, std::size_t) noexcept { Free(p); }
void operator delete[](void* p, std::size_t) noexcept { Free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete(void* p, std::align_val_t alignment) noexcept { FreeAligned(p, alignment); }
void operator delete[](void* p, std::align_val_t alignment) noexcept { FreeAligned(p, alignment); }
void operator delete(void* p, std::size_t, std::align_val_t alignment) noexcept { FreeAligned(p, alignment); }
void operator delete[](void* p, std::size_t, std::align_val_t alignment) noexcept { FreeAligned(p, alignment); }
void operator delete(void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept { FreeAligned(p, alignment); }
void operator delete[](void* p, std::align_val_t alignment, const std::nothrow_t&) noexcept { FreeAligned(p, alignment); }
