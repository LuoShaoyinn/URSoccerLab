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
//   - One atomic exchange per publish/read; no wait for the other side
//   - Consumer never blocks producer, producer never blocks consumer
//   - Consumer always gets the most-recently-published COMPLETE snapshot
//   - If producer publishes faster than consumer reads, intermediate
//     snapshots are silently dropped (UDP-over-TCP semantics)

#include <atomic>
#include <mutex>
#include <type_traits>

template <typename T>
class URSTripleBuffer
{
	static_assert(std::is_trivially_destructible<T>::value ||
		std::is_default_constructible<T>::value,
		"T must be trivially destructible or default constructible");

	T Items[3]{};
	mutable std::atomic<int32_t> Published;  // -1 = no unread publication
	mutable std::atomic<int32_t> Released;   // slot returned by consumer
	int32_t BackIdx;                 // producer-private
	mutable int32_t FrontIdx;                // consumer-private

public:
	URSTripleBuffer()
		: Published(-1)
		, Released(2)
		, BackIdx(0)
		, FrontIdx(1)
	{
	}

	// ---- Producer (call from ONE thread) ----

	T& Back() { return Items[BackIdx]; }

	void Publish()
	{
		PublishLocked();
	}

	// ---- Consumer (call from ONE thread) ----

	const T& Front() const
	{
		// Take an unread publication, if one exists. Clearing Published avoids
		// mistaking the slot returned below for a second publication when the
		// producer has not written again yet.
		const int32_t P = Published.exchange(-1, std::memory_order_acq_rel);
		if (P >= 0 && P != FrontIdx)
		{
			const int32_t OldFront = FrontIdx;
			FrontIdx = P;
			Released.store(OldFront, std::memory_order_release);
		}
		return Items[FrontIdx];
	}

	bool HasNew() const
	{
		int32_t P = Published.load(std::memory_order_acquire);
		return P >= 0 && P != FrontIdx;
	}

	// Command and gain channels can have more than one producer (for example
	// network commands and an admin pose reset). Serialize only that write;
	// the normal physics/network snapshot path remains lock-free SPSC.
	void PublishValue(const T& Value)
	{
		std::lock_guard<std::mutex> Lock(PublishMutex);
		Items[BackIdx] = Value;
		PublishLocked();
	}

	// Disable copies
	URSTripleBuffer(const URSTripleBuffer&) = delete;
	URSTripleBuffer& operator=(const URSTripleBuffer&) = delete;
	URSTripleBuffer(URSTripleBuffer&&) = delete;
	URSTripleBuffer& operator=(URSTripleBuffer&&) = delete;

private:
	void PublishLocked()
	{
		const int32_t Old = Published.exchange(BackIdx, std::memory_order_acq_rel);
		if (Old >= 0)
			BackIdx = Old;
		else
			BackIdx = Released.exchange(BackIdx, std::memory_order_acq_rel);
	}
	std::mutex PublishMutex;
};
