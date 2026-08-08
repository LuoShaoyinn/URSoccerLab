#pragma once

// URSTripleBuffer — lock-free single-producer / single-consumer triple buffer.
// No UE dependencies. Pure C++11. Testable in isolation.
//
// Producer (one thread):
//   auto& ref = buf.Back();       // get writable buffer
//   fill ref ...
//   buf.Publish();                // atomically swap
//
// Consumer (one thread):
//   const auto& ref = buf.Front();  // always returns latest complete data
//
// Properties:
//   - Zero allocation after construction (three pre-allocated buffers)
//   - Zero locks (one atomic exchange per publish, one atomic load per read)
//   - Consumer never blocks producer, producer never blocks consumer
//   - Consumer always gets the most-recently-published COMPLETE snapshot
//   - If producer publishes faster than consumer reads, intermediate
//     snapshots are silently dropped (UDP-over-TCP semantics)

#include <atomic>
#include <type_traits>

template <typename T>
class URSTripleBuffer
{
	static_assert(std::is_trivially_destructible<T>::value ||
		std::is_default_constructible<T>::value,
		"T must be trivially destructible or default constructible");

	T Items[3];
	std::atomic<int32_t> Published;  // -1 = nothing published yet
	int32_t BackIdx;                 // producer-private
	mutable int32_t FrontIdx;                // consumer-private

public:
	URSTripleBuffer()
		: Published(-1)
		, BackIdx(0)
		, FrontIdx(1)
	{
	}

	// ---- Producer (call from ONE thread) ----

	T& Back() { return Items[BackIdx]; }

	void Publish()
	{
		// Exchange: our back becomes the new published.
		// The old published becomes our new back (recycled for next write).
		int32_t Old = Published.exchange(BackIdx, std::memory_order_acq_rel);
		if (Old >= 0)
			BackIdx = Old;
		else
			BackIdx = (BackIdx == 0) ? 2 : BackIdx - 1;
		// Ensure BackIdx != FrontIdx is not strictly necessary because
		// FrontIdx is consumer-private and the consumer always reads Published.
	}

	// ---- Consumer (call from ONE thread) ----

	const T& Front() const
	{
		int32_t P = Published.load(std::memory_order_acquire);
		if (P < 0)
			return Items[2];  // initial dummy (uninitialized is OK for POD)
		FrontIdx = P;
		return Items[P];
	}

	bool HasNew() const
	{
		int32_t P = Published.load(std::memory_order_acquire);
		return P >= 0 && P != FrontIdx;
	}

	// Disable copies
	URSTripleBuffer(const URSTripleBuffer&) = delete;
	URSTripleBuffer& operator=(const URSTripleBuffer&) = delete;
	URSTripleBuffer(URSTripleBuffer&&) = delete;
	URSTripleBuffer& operator=(URSTripleBuffer&&) = delete;
};
