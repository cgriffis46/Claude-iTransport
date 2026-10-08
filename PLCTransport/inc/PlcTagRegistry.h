#pragma once
#include <string>
#include <vector>
#include <cstring>
#include <mutex> // for std::lock_guard<T> only -- confirmed to work standalone on this
                 // project's real ARM toolchain even though std::mutex itself does not;
                 // the actual locked type below is PlcMutex, not std::mutex (see PlcMutex.h)
#include "PlcTagDescriptor.h"
#include "PlcDataType.h"
#include "PlcMutex.h"

// A copy of one tag, made under the registry's lock, for code that must
// not hold the lock while it works with the value: a web server writing
// to a socket, say, which could take seconds. Fixed size, no allocation.
struct PlcTagSnapshot {
    static constexpr size_t kMaxName = 63;    // longer names are cut (CIP's own limit is 40)
    static constexpr size_t kMaxValue = 64;   // bytes of the value kept; elementary types all fit

    char        name[kMaxName + 1];
    PlcDataType dataType;
    size_t      sizeBytes;                    // the tag's own size, even when value holds less
    bool        writable;
    uint8_t     value[kMaxValue];             // its first min(sizeBytes, kMaxValue) bytes
};

// Central, thread-safe registry of PLC tags — a name-addressable
// view onto live variables and structs elsewhere in the program.
// Multiple threads (the per-connection worker threads a TCP/CIP
// server spawns, for instance) can safely registerTag(), readTag(),
// and writeTag() concurrently; every operation is guarded by one
// mutex for the whole registry.
//
// Deliberately a single mutex, not per-tag locking: the tag count in
// a realistic PLC tag database is small enough, and each operation
// short enough (a memcpy of a few bytes), that per-tag locks would
// add complexity without a measurable benefit — and a single mutex
// means registerTag() can never race with a concurrent readTag()/
// writeTag() on a half-constructed descriptor, which per-tag locking
// would still need to account for separately.
//
// Does NOT copy the registered variable: data points directly at the
// caller's own storage, which must outlive the registry entry. This
// is deliberate — the entire point of a tag database is live access
// to the actual running program's state, not a stale snapshot.
//
// PORTABILITY, resolved rather than left as an open caveat: this
// class uses PlcMutex (see PlcMutex.h), not std::mutex directly.
// std::mutex was tried first and CONFIRMED broken on this project's
// real STM32 target — cross-compiling against the actual
// STM32F207ZG_SafeRelay project with arm-none-eabi-g++ reports
// "Thread model: single", and <mutex> fails outright. PlcMutex
// resolves this by wrapping CMSIS-RTOS2's osMutexId_t under FreeRTOS
// builds (PLC_MUTEX_USE_CMSIS_RTOS2 defined) and std::mutex elsewhere
// (Linux/KR260, where it's genuinely available) — see PlcMutex.h's
// own header for the full reasoning.
class PlcTagRegistry {
public:
    // Registers variable under name, inferring its CIP elementary
    // data type at compile time via PlcTypeTraits<T> — there's no
    // path to accidentally register an int32_t as a BOOL, say,
    // because the type code is derived, never passed by hand.
    //
    // Returns false (and registers nothing) if name is already
    // registered — silently allowing two tags to share a name would
    // make later lookups ambiguous about which one actually gets read
    // or written.
    template <typename T>
    bool registerTag(const std::string& name, T& variable, bool writable = true) {
        std::lock_guard<PlcMutex> lock(mutex_);
        if (findLocked(name) != nullptr) {
            return false;
        }
        tags_.push_back(PlcTagDescriptor{name, &variable, sizeof(T), PlcTypeTraits<T>::kType, writable});
        return true;
    }

    // Registers a struct/opaque block of memory explicitly — no
    // PlcTypeTraits match is possible for an arbitrary struct, so the
    // caller states its size (via sizeof(T)) directly; dataType is
    // always PlcDataType::Struct. Contents are opaque bytes to this
    // registry — see PlcDataType.h for the honest scope of what
    // "Struct" means here.
    template <typename T>
    bool registerStructTag(const std::string& name, T& variable, bool writable = true) {
        std::lock_guard<PlcMutex> lock(mutex_);
        if (findLocked(name) != nullptr) {
            return false;
        }
        tags_.push_back(PlcTagDescriptor{name, &variable, sizeof(T), PlcDataType::Struct, writable});
        return true;
    }

    // Copies the tag's current live bytes into outBuffer (which must
    // be at least as large as the tag's own size — outBufferSize is
    // checked, not trusted). Returns false if the tag doesn't exist,
    // or outBuffer is too small to hold it.
    bool readTag(const std::string& name, void* outBuffer, size_t outBufferSize,
                 PlcDataType* outType = nullptr) {
        std::lock_guard<PlcMutex> lock(mutex_);
        const PlcTagDescriptor* tag = findLocked(name);
        if (!tag || outBufferSize < tag->sizeBytes) {
            return false;
        }
        std::memcpy(outBuffer, tag->data, tag->sizeBytes);
        if (outType) *outType = tag->dataType;
        return true;
    }

    // Copies inBufferSize bytes from inBuffer into the tag's live
    // storage. Returns false if the tag doesn't exist, isn't
    // writable, or inBufferSize doesn't exactly match the tag's own
    // size — a partial or oversized write is rejected outright rather
    // than silently truncated or zero-padded, since either would
    // quietly corrupt whatever's actually running off this variable.
    bool writeTag(const std::string& name, const void* inBuffer, size_t inBufferSize) {
        std::lock_guard<PlcMutex> lock(mutex_);
        PlcTagDescriptor* tag = findLocked(name);
        if (!tag || !tag->writable || inBufferSize != tag->sizeBytes) {
            return false;
        }
        std::memcpy(tag->data, inBuffer, inBufferSize);
        return true;
    }

    // Copies up to max tags, from the first'th in the order they were
    // registered, into out: name, type, size, writable and value, all
    // under one hold of the lock. Returns how many. Tags are never
    // removed, so paging through with first = 0, max, 2*max, ... sees
    // each once (and any registered meanwhile, at the end).
    size_t snapshot(size_t first, PlcTagSnapshot* out, size_t max) const {
        std::lock_guard<PlcMutex> lock(mutex_);
        size_t n = 0;
        for (size_t i = first; i < tags_.size() && n < max; ++i, ++n) copyLocked(tags_[i], out[n]);
        return n;
    }

    // The same for one tag, by name. No std::string is made for the
    // lookup. false: no such tag.
    bool snapshot(const char* name, PlcTagSnapshot& out) const {
        std::lock_guard<PlcMutex> lock(mutex_);
        for (const auto& tag : tags_) {
            if (tag.name == name) {
                copyLocked(tag, out);
                return true;
            }
        }
        return false;
    }

    size_t tagCount() const {
        std::lock_guard<PlcMutex> lock(mutex_);
        return tags_.size();
    }

    // Returns a BY-VALUE copy of the descriptor's metadata (size,
    // type, writable) — never the live data pointer exposed outside
    // the lock, since holding onto it after this call returns would
    // let a caller touch the tag's bytes without going through
    // readTag()/writeTag()'s own synchronization.
    bool describeTag(const std::string& name, size_t& outSize, PlcDataType& outType, bool& outWritable) const {
        std::lock_guard<PlcMutex> lock(mutex_);
        const PlcTagDescriptor* tag = findLocked(name);
        if (!tag) return false;
        outSize = tag->sizeBytes;
        outType = tag->dataType;
        outWritable = tag->writable;
        return true;
    }

private:
    static void copyLocked(const PlcTagDescriptor& tag, PlcTagSnapshot& out) {
        const size_t n = tag.name.size() < PlcTagSnapshot::kMaxName ? tag.name.size() : PlcTagSnapshot::kMaxName;
        std::memcpy(out.name, tag.name.data(), n);
        out.name[n] = 0;
        out.dataType = tag.dataType;
        out.sizeBytes = tag.sizeBytes;
        out.writable = tag.writable;
        std::memcpy(out.value, tag.data, tag.sizeBytes < PlcTagSnapshot::kMaxValue ? tag.sizeBytes : PlcTagSnapshot::kMaxValue);
    }

    // Caller must already hold mutex_.
    const PlcTagDescriptor* findLocked(const std::string& name) const {
        for (const auto& tag : tags_) {
            if (tag.name == name) return &tag;
        }
        return nullptr;
    }
    PlcTagDescriptor* findLocked(const std::string& name) {
        for (auto& tag : tags_) {
            if (tag.name == name) return &tag;
        }
        return nullptr;
    }

    mutable PlcMutex mutex_;
    std::vector<PlcTagDescriptor> tags_;
};
