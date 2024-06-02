#pragma once

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
#include <tbb/scalable_allocator.h>

class FixedAllocator
{
public:
	FixedAllocator(uint32_t allocation_size, uint16_t num_blocks) noexcept
		: allocation_size(allocation_size)
		, num_blocks(num_blocks)
	{
	}

	void* allocate()
	{
#ifdef USE_SCALABLE_ALLOCATOR
			return allocator.allocate(allocation_size);
#else
			if (free_list.empty())
			{
				auto size = allocation_size * num_blocks;
				allocator.emplace_back(std::make_unique<unsigned char[]>(size));
				free_list.reserve(num_blocks * allocator.size());
				uint16_t i = 0;
				for (unsigned char* p = allocator.back().get(); i != num_blocks;
					 p += allocation_size, ++i)
				{
					free_list.push_back(p);
				}
			}

			unsigned char* result = free_list.back();
			free_list.pop_back();
			return result;
#endif
	}

	void deallocate(void* p) noexcept {
#ifdef USE_SCALABLE_ALLOCATOR
			allocator.deallocate(static_cast<char*>(p), allocation_size);
#else
			free_list.push_back(static_cast<unsigned char*>(p));
#endif
	}

	uint32_t getAllocationSize() const noexcept { return allocation_size; }

private:

#ifdef USE_SCALABLE_ALLOCATOR
					   tbb::scalable_allocator<char> allocator;
#else
					   std::vector<std::unique_ptr<unsigned char[]>> allocator;
	std::vector<unsigned char*> free_list;
#endif
	uint32_t allocation_size;
	uint16_t num_blocks;
};

class Allocator
{
public:
	explicit Allocator(uint16_t num_blocks)
		: num_blocks(num_blocks)
	{
	}

	void* allocate(uint32_t allocation_size)
	{
		for (auto& alloc : fixed_allocators)
		{
			if (alloc.getAllocationSize() == allocation_size)
			{
				return alloc.allocate();
			}
		}

		return fixed_allocators.emplace_back(allocation_size, num_blocks).allocate();
	}

	void deallocate(void* p, uint32_t allocation_size) noexcept
	{
		for (auto& alloc : fixed_allocators)
		{
			if (alloc.getAllocationSize() == allocation_size)
			{
				return alloc.deallocate(p);
			}
		}
	}

private:
	std::vector<FixedAllocator> fixed_allocators;
	uint16_t num_blocks;
};
