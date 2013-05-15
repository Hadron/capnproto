// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#define CAPNPROTO_PRIVATE
#include "layout.h"
#include "logging.h"
#include "arena.h"
#include <string.h>
#include <limits>
#include <stdlib.h>

namespace capnproto {
namespace internal {

// =======================================================================================

struct WirePointer {
  // A pointer, in exactly the format in which it appears on the wire.

  // Copying and moving is not allowed because the offset would become wrong.
  WirePointer(const WirePointer& other) = delete;
  WirePointer(WirePointer&& other) = delete;
  WirePointer& operator=(const WirePointer& other) = delete;
  WirePointer& operator=(WirePointer&& other) = delete;

  // -----------------------------------------------------------------
  // Common part of all pointers:  kind + offset
  //
  // Actually this is not terribly common.  The "offset" could actually be different things
  // depending on the context:
  // - For a regular (e.g. struct/list) pointer, a signed word offset from the word immediately
  //   following the pointer pointer.  (The off-by-one means the offset is more often zero, saving
  //   bytes on the wire when packed.)
  // - For an inline composite list tag (not really a pointer, but structured similarly), an
  //   element count.
  // - For a FAR pointer, an unsigned offset into the target segment.
  // - For a FAR landing pad, zero indicates that the target value immediately follows the pad while
  //   1 indicates that the pad is followed by another FAR pointer that actually points at the
  //   value.

  enum Kind {
    STRUCT = 0,
    // Reference points at / describes a struct.

    LIST = 1,
    // Reference points at / describes a list.

    FAR = 2,
    // Reference is a "far pointer", which points at data located in a different segment.  The
    // eventual target is one of the other kinds.

    RESERVED_3 = 3
    // Reserved for future use.
  };

  WireValue<uint32_t> offsetAndKind;

  CAPNPROTO_ALWAYS_INLINE(Kind kind() const) {
    return static_cast<Kind>(offsetAndKind.get() & 3);
  }

  CAPNPROTO_ALWAYS_INLINE(word* target()) {
    return reinterpret_cast<word*>(this) + 1 + (static_cast<int32_t>(offsetAndKind.get()) >> 2);
  }
  CAPNPROTO_ALWAYS_INLINE(const word* target() const) {
    return reinterpret_cast<const word*>(this) + 1 +
        (static_cast<int32_t>(offsetAndKind.get()) >> 2);
  }
  CAPNPROTO_ALWAYS_INLINE(void setKindAndTarget(Kind kind, word* target)) {
    offsetAndKind.set(((target - reinterpret_cast<word*>(this) - 1) << 2) | kind);
  }
  CAPNPROTO_ALWAYS_INLINE(void setKindWithZeroOffset(Kind kind)) {
    offsetAndKind.set(kind);
  }

  CAPNPROTO_ALWAYS_INLINE(ElementCount inlineCompositeListElementCount() const) {
    return (offsetAndKind.get() >> 2) * ELEMENTS;
  }
  CAPNPROTO_ALWAYS_INLINE(void setKindAndInlineCompositeListElementCount(
      Kind kind, ElementCount elementCount)) {
    offsetAndKind.set(((elementCount / ELEMENTS) << 2) | kind);
  }

  CAPNPROTO_ALWAYS_INLINE(WordCount farPositionInSegment() const) {
    DPRECOND(kind() == FAR,
        "positionInSegment() should only be called on FAR pointers.");
    return (offsetAndKind.get() >> 3) * WORDS;
  }
  CAPNPROTO_ALWAYS_INLINE(bool isDoubleFar() const) {
    DPRECOND(kind() == FAR,
        "isDoubleFar() should only be called on FAR pointers.");
    return (offsetAndKind.get() >> 2) & 1;
  }
  CAPNPROTO_ALWAYS_INLINE(void setFar(bool isDoubleFar, WordCount pos)) {
    offsetAndKind.set(((pos / WORDS) << 3) | (static_cast<uint32_t>(isDoubleFar) << 2) |
                      static_cast<uint32_t>(Kind::FAR));
  }

  // -----------------------------------------------------------------
  // Part of pointer that depends on the kind.

  union {
    uint32_t upper32Bits;

    struct {
      WireValue<WordCount16> dataSize;
      WireValue<WirePointerCount16> ptrCount;

      inline WordCount wordSize() const {
        return dataSize.get() + ptrCount.get() * WORDS_PER_POINTER;
      }

      CAPNPROTO_ALWAYS_INLINE(void set(WordCount ds, WirePointerCount rc)) {
        dataSize.set(ds);
        ptrCount.set(rc);
      }
      CAPNPROTO_ALWAYS_INLINE(void set(StructSize size)) {
        dataSize.set(size.data);
        ptrCount.set(size.pointers);
      }
    } structRef;
    // Also covers capabilities.

    struct {
      WireValue<uint32_t> elementSizeAndCount;

      CAPNPROTO_ALWAYS_INLINE(FieldSize elementSize() const) {
        return static_cast<FieldSize>(elementSizeAndCount.get() & 7);
      }
      CAPNPROTO_ALWAYS_INLINE(ElementCount elementCount() const) {
        return (elementSizeAndCount.get() >> 3) * ELEMENTS;
      }
      CAPNPROTO_ALWAYS_INLINE(WordCount inlineCompositeWordCount() const) {
        return elementCount() * (1 * WORDS / ELEMENTS);
      }

      CAPNPROTO_ALWAYS_INLINE(void set(FieldSize es, ElementCount ec)) {
        DPRECOND(ec < (1 << 29) * ELEMENTS, "Lists are limited to 2**29 elements.");
        elementSizeAndCount.set(((ec / ELEMENTS) << 3) | static_cast<int>(es));
      }

      CAPNPROTO_ALWAYS_INLINE(void setInlineComposite(WordCount wc)) {
        DPRECOND(wc < (1 << 29) * WORDS, "Inline composite lists are limited to 2**29 words.");
        elementSizeAndCount.set(((wc / WORDS) << 3) |
                                static_cast<int>(FieldSize::INLINE_COMPOSITE));
      }
    } listRef;

    struct {
      WireValue<SegmentId> segmentId;

      CAPNPROTO_ALWAYS_INLINE(void set(SegmentId si)) {
        segmentId.set(si);
      }
    } farRef;
  };

  CAPNPROTO_ALWAYS_INLINE(bool isNull() const) {
    // If the upper 32 bits are zero, this is a pointer to an empty struct.  We consider that to be
    // our "null" value.
    // TODO:  Maybe this would be faster, but violates aliasing rules; does it matter?:
    //    return *reinterpret_cast<const uint64_t*>(this) == 0;
    return (offsetAndKind.get() == 0) & (upper32Bits == 0);
  }
};
static_assert(sizeof(WirePointer) == sizeof(word),
    "capnproto::WirePointer is not exactly one word.  This will probably break everything.");
static_assert(POINTERS * WORDS_PER_POINTER * BYTES_PER_WORD / BYTES == sizeof(WirePointer),
    "WORDS_PER_POINTER is wrong.");
static_assert(POINTERS * BYTES_PER_POINTER / BYTES == sizeof(WirePointer),
    "BYTES_PER_POINTER is wrong.");
static_assert(POINTERS * BITS_PER_POINTER / BITS_PER_BYTE / BYTES == sizeof(WirePointer),
    "BITS_PER_POINTER is wrong.");

// =======================================================================================

struct WireHelpers {
  static CAPNPROTO_ALWAYS_INLINE(WordCount roundUpToWords(BitCount64 bits)) {
    static_assert(sizeof(word) == 8, "This code assumes 64-bit words.");
    return (bits + 63 * BITS) / BITS_PER_WORD;
  }

  static CAPNPROTO_ALWAYS_INLINE(WordCount roundUpToWords(ByteCount bytes)) {
    static_assert(sizeof(word) == 8, "This code assumes 64-bit words.");
    return (bytes + 7 * BYTES) / BYTES_PER_WORD;
  }

  static CAPNPROTO_ALWAYS_INLINE(ByteCount roundUpToBytes(BitCount bits)) {
    return (bits + 7 * BITS) / BITS_PER_BYTE;
  }

  static CAPNPROTO_ALWAYS_INLINE(word* allocate(
      WirePointer*& ref, SegmentBuilder*& segment, WordCount amount,
      WirePointer::Kind kind)) {
    word* ptr = segment->allocate(amount);

    if (ptr == nullptr) {
      // Need to allocate in a new segment.  We'll need to allocate an extra pointer worth of
      // space to act as the landing pad for a far pointer.

      WordCount amountPlusRef = amount + POINTER_SIZE_IN_WORDS;
      segment = segment->getArena()->getSegmentWithAvailable(amountPlusRef);
      ptr = segment->allocate(amountPlusRef);

      // Set up the original pointer to be a far pointer to the new segment.
      ref->setFar(false, segment->getOffsetTo(ptr));
      ref->farRef.set(segment->getSegmentId());

      // Initialize the landing pad to indicate that the data immediately follows the pad.
      ref = reinterpret_cast<WirePointer*>(ptr);
      ref->setKindAndTarget(kind, ptr + POINTER_SIZE_IN_WORDS);

      // Allocated space follows new pointer.
      return ptr + POINTER_SIZE_IN_WORDS;
    } else {
      ref->setKindAndTarget(kind, ptr);
      return ptr;
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(word* followFars(WirePointer*& ref, SegmentBuilder*& segment)) {
    if (ref->kind() == WirePointer::FAR) {
      segment = segment->getArena()->getSegment(ref->farRef.segmentId.get());
      WirePointer* pad =
          reinterpret_cast<WirePointer*>(segment->getPtrUnchecked(ref->farPositionInSegment()));
      if (!ref->isDoubleFar()) {
        ref = pad;
        return pad->target();
      }

      // Landing pad is another far pointer.  It is followed by a tag describing the pointed-to
      // object.
      ref = pad + 1;

      segment = segment->getArena()->getSegment(pad->farRef.segmentId.get());
      return segment->getPtrUnchecked(pad->farPositionInSegment());
    } else {
      return ref->target();
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(
      const word* followFars(const WirePointer*& ref, SegmentReader*& segment)) {
    if (ref->kind() == WirePointer::FAR) {
      // Look up the segment containing the landing pad.
      segment = segment->getArena()->tryGetSegment(ref->farRef.segmentId.get());
      VALIDATE_INPUT(segment != nullptr, "Message contains far pointer to unknown segment.") {
        return nullptr;
      }

      // Find the landing pad and check that it is within bounds.
      const word* ptr = segment->getStartPtr() + ref->farPositionInSegment();
      WordCount padWords = (1 + ref->isDoubleFar()) * POINTER_SIZE_IN_WORDS;
      VALIDATE_INPUT(segment->containsInterval(ptr, ptr + padWords),
                     "Message contains out-of-bounds far pointer.") {
        return nullptr;
      }

      const WirePointer* pad = reinterpret_cast<const WirePointer*>(ptr);

      // If this is not a double-far then the landing pad is our final pointer.
      if (!ref->isDoubleFar()) {
        ref = pad;
        return pad->target();
      }

      // Landing pad is another far pointer.  It is followed by a tag describing the pointed-to
      // object.
      ref = pad + 1;

      segment = segment->getArena()->tryGetSegment(pad->farRef.segmentId.get());
      VALIDATE_INPUT(segment != nullptr,
                     "Message contains double-far pointer to unknown segment.") {
        return nullptr;
      }

      return segment->getStartPtr() + pad->farPositionInSegment();
    } else {
      return ref->target();
    }
  }

  // -----------------------------------------------------------------

  static CAPNPROTO_ALWAYS_INLINE(
      void copyStruct(SegmentBuilder* segment, word* dst, const word* src,
                      WordCount dataSize, WirePointerCount pointerCount)) {
    memcpy(dst, src, dataSize * BYTES_PER_WORD / BYTES);

    const WirePointer* srcRefs = reinterpret_cast<const WirePointer*>(src + dataSize);
    WirePointer* dstRefs = reinterpret_cast<WirePointer*>(dst + dataSize);

    for (uint i = 0; i < pointerCount / POINTERS; i++) {
      SegmentBuilder* subSegment = segment;
      WirePointer* dstRef = dstRefs + i;
      copyMessage(subSegment, dstRef, srcRefs + i);
    }
  }

  static word* copyMessage(
      SegmentBuilder*& segment, WirePointer*& dst, const WirePointer* src) {
    // Not always-inline because it's recursive.

    switch (src->kind()) {
      case WirePointer::STRUCT: {
        if (src->isNull()) {
          memset(dst, 0, sizeof(WirePointer));
          return nullptr;
        } else {
          const word* srcPtr = src->target();
          word* dstPtr = allocate(dst, segment, src->structRef.wordSize(), WirePointer::STRUCT);

          copyStruct(segment, dstPtr, srcPtr, src->structRef.dataSize.get(),
                     src->structRef.ptrCount.get());

          dst->structRef.set(src->structRef.dataSize.get(), src->structRef.ptrCount.get());
          return dstPtr;
        }
      }
      case WirePointer::LIST: {
        switch (src->listRef.elementSize()) {
          case FieldSize::VOID:
          case FieldSize::BIT:
          case FieldSize::BYTE:
          case FieldSize::TWO_BYTES:
          case FieldSize::FOUR_BYTES:
          case FieldSize::EIGHT_BYTES: {
            WordCount wordCount = roundUpToWords(
                ElementCount64(src->listRef.elementCount()) *
                dataBitsPerElement(src->listRef.elementSize()));
            const word* srcPtr = src->target();
            word* dstPtr = allocate(dst, segment, wordCount, WirePointer::LIST);
            memcpy(dstPtr, srcPtr, wordCount * BYTES_PER_WORD / BYTES);

            dst->listRef.set(src->listRef.elementSize(), src->listRef.elementCount());
            return dstPtr;
          }

          case FieldSize::POINTER: {
            const WirePointer* srcRefs = reinterpret_cast<const WirePointer*>(src->target());
            WirePointer* dstRefs = reinterpret_cast<WirePointer*>(
                allocate(dst, segment, src->listRef.elementCount() *
                    (1 * POINTERS / ELEMENTS) * WORDS_PER_POINTER,
                    WirePointer::LIST));

            uint n = src->listRef.elementCount() / ELEMENTS;
            for (uint i = 0; i < n; i++) {
              SegmentBuilder* subSegment = segment;
              WirePointer* dstRef = dstRefs + i;
              copyMessage(subSegment, dstRef, srcRefs + i);
            }

            dst->listRef.set(FieldSize::POINTER, src->listRef.elementCount());
            return reinterpret_cast<word*>(dstRefs);
          }

          case FieldSize::INLINE_COMPOSITE: {
            const word* srcPtr = src->target();
            word* dstPtr = allocate(dst, segment,
                src->listRef.inlineCompositeWordCount() + POINTER_SIZE_IN_WORDS,
                WirePointer::LIST);

            dst->listRef.setInlineComposite(src->listRef.inlineCompositeWordCount());

            const WirePointer* srcTag = reinterpret_cast<const WirePointer*>(srcPtr);
            memcpy(dstPtr, srcTag, sizeof(WirePointer));

            const word* srcElement = srcPtr + POINTER_SIZE_IN_WORDS;
            word* dstElement = dstPtr + POINTER_SIZE_IN_WORDS;

            CHECK(srcTag->kind() == WirePointer::STRUCT,
                "INLINE_COMPOSITE of lists is not yet supported.");

            uint n = srcTag->inlineCompositeListElementCount() / ELEMENTS;
            for (uint i = 0; i < n; i++) {
              copyStruct(segment, dstElement, srcElement,
                  srcTag->structRef.dataSize.get(), srcTag->structRef.ptrCount.get());
              srcElement += srcTag->structRef.wordSize();
              dstElement += srcTag->structRef.wordSize();
            }
            return dstPtr;
          }
        }
        break;
      }
      default:
        FAIL_PRECOND("Copy source message contained unexpected kind.");
        break;
    }

    return nullptr;
  }

  static void transferPointer(SegmentBuilder* dstSegment, WirePointer* dst,
                              SegmentBuilder* srcSegment, WirePointer* src) {
    // Make *dst point to the same object as *src.  Both must reside in the same message, but can
    // be in different segments.  Not always-inline because this is rarely used.

    if (src->isNull()) {
      memset(dst, 0, sizeof(WirePointer));
    } else if (src->kind() == WirePointer::FAR) {
      // Far pointers are position-independent, so we can just copy.
      memcpy(dst, src, sizeof(WirePointer));
    } else if (dstSegment == srcSegment) {
      // Same segment, so create a direct pointer.
      dst->setKindAndTarget(src->kind(), src->target());

      // We can just copy the upper 32 bits.  (Use memcpy() to comply with aliasing rules.)
      memcpy(&dst->upper32Bits, &src->upper32Bits, sizeof(src->upper32Bits));
    } else {
      // Need to create a far pointer.  Try to allocate it in the same segment as the source, so
      // that it doesn't need to be a double-far.

      WirePointer* landingPad =
          reinterpret_cast<WirePointer*>(srcSegment->allocate(1 * WORDS));
      if (landingPad == nullptr) {
        // Darn, need a double-far.
        SegmentBuilder* farSegment = srcSegment->getArena()->getSegmentWithAvailable(2 * WORDS);
        landingPad = reinterpret_cast<WirePointer*>(farSegment->allocate(2 * WORDS));
        DCHECK(landingPad != nullptr,
            "getSegmentWithAvailable() returned segment without space available.");

        landingPad[0].setFar(false, srcSegment->getOffsetTo(src->target()));
        landingPad[0].farRef.segmentId.set(srcSegment->getSegmentId());

        landingPad[1].setKindWithZeroOffset(src->kind());
        memcpy(&landingPad[1].upper32Bits, &src->upper32Bits, sizeof(src->upper32Bits));

        dst->setFar(true, farSegment->getOffsetTo(reinterpret_cast<word*>(landingPad)));
        dst->farRef.set(farSegment->getSegmentId());
      } else {
        // Simple landing pad is just a pointer.
        landingPad->setKindAndTarget(src->kind(), src->target());
        memcpy(&landingPad->upper32Bits, &src->upper32Bits, sizeof(src->upper32Bits));

        dst->setFar(false, srcSegment->getOffsetTo(reinterpret_cast<word*>(landingPad)));
        dst->farRef.set(srcSegment->getSegmentId());
      }
    }
  }

  // -----------------------------------------------------------------

  static CAPNPROTO_ALWAYS_INLINE(StructBuilder initStructPointer(
      WirePointer* ref, SegmentBuilder* segment, StructSize size)) {
    // Allocate space for the new struct.  Newly-allocated space is automatically zeroed.
    word* ptr = allocate(ref, segment, size.total(), WirePointer::STRUCT);

    // Initialize the pointer.
    ref->structRef.set(size);

    // Build the StructBuilder.
    return StructBuilder(segment, ptr, reinterpret_cast<WirePointer*>(ptr + size.data),
                         size.data * BITS_PER_WORD, size.pointers, 0 * BITS);
  }

  static CAPNPROTO_ALWAYS_INLINE(StructBuilder getWritableStructPointer(
      WirePointer* ref, SegmentBuilder* segment, StructSize size, const word* defaultValue)) {
    word* ptr;

    if (ref->isNull()) {
    useDefault:
      if (defaultValue == nullptr ||
          reinterpret_cast<const WirePointer*>(defaultValue)->isNull()) {
        ptr = allocate(ref, segment, size.total(), WirePointer::STRUCT);
        ref->structRef.set(size);
      } else {
        ptr = copyMessage(segment, ref, reinterpret_cast<const WirePointer*>(defaultValue));
      }
      return StructBuilder(segment, ptr, reinterpret_cast<WirePointer*>(ptr + size.data),
                           size.data * BITS_PER_WORD, size.pointers, 0 * BITS);
    } else {
      WirePointer* oldRef = ref;
      SegmentBuilder* oldSegment = segment;
      word* oldPtr = followFars(oldRef, oldSegment);

      VALIDATE_INPUT(oldRef->kind() == WirePointer::STRUCT,
          "Message contains non-struct pointer where struct pointer was expected.") {
        goto useDefault;
      }

      WordCount oldDataSize = oldRef->structRef.dataSize.get();
      WirePointerCount oldPointerCount = oldRef->structRef.ptrCount.get();
      WirePointer* oldPointerSection =
          reinterpret_cast<WirePointer*>(oldPtr + oldDataSize);

      if (oldDataSize < size.data || oldPointerCount < size.pointers) {
        // The space allocated for this struct is too small.  Unlike with readers, we can't just
        // run with it and do bounds checks at access time, because how would we handle writes?
        // Instead, we have to copy the struct to a new space now.

        WordCount newDataSize = std::max<WordCount>(oldDataSize, size.data);
        WirePointerCount newPointerCount =
            std::max<WirePointerCount>(oldPointerCount, size.pointers);
        WordCount totalSize = newDataSize + newPointerCount * WORDS_PER_POINTER;

        ptr = allocate(ref, segment, totalSize, WirePointer::STRUCT);
        ref->structRef.set(newDataSize, newPointerCount);

        // Copy data section.
        memcpy(ptr, oldPtr, oldDataSize * BYTES_PER_WORD / BYTES);

        // Copy pointer section.
        WirePointer* newPointerSection = reinterpret_cast<WirePointer*>(ptr + newDataSize);
        for (uint i = 0; i < oldPointerCount / POINTERS; i++) {
          transferPointer(segment, newPointerSection + i, oldSegment, oldPointerSection + i);
        }

        return StructBuilder(segment, ptr, newPointerSection, newDataSize * BITS_PER_WORD,
                             newPointerCount, 0 * BITS);
      } else {
        return StructBuilder(oldSegment, oldPtr, oldPointerSection, oldDataSize * BITS_PER_WORD,
                             oldPointerCount, 0 * BITS);
      }
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(ListBuilder initListPointer(
      WirePointer* ref, SegmentBuilder* segment, ElementCount elementCount,
      FieldSize elementSize)) {
    DPRECOND(elementSize != FieldSize::INLINE_COMPOSITE,
        "Should have called initStructListPointer() instead.");

    BitCount dataSize = dataBitsPerElement(elementSize) * ELEMENTS;
    WirePointerCount pointerCount = pointersPerElement(elementSize) * ELEMENTS;
    auto step = (dataSize + pointerCount * BITS_PER_POINTER) / ELEMENTS;

    // Calculate size of the list.
    WordCount wordCount = roundUpToWords(ElementCount64(elementCount) * step);

    // Allocate the list.
    word* ptr = allocate(ref, segment, wordCount, WirePointer::LIST);

    // Initialize the pointer.
    ref->listRef.set(elementSize, elementCount);

    // Build the ListBuilder.
    return ListBuilder(segment, ptr, step, elementCount, dataSize, pointerCount);
  }

  static CAPNPROTO_ALWAYS_INLINE(ListBuilder initStructListPointer(
      WirePointer* ref, SegmentBuilder* segment, ElementCount elementCount,
      StructSize elementSize)) {
    if (elementSize.preferredListEncoding != FieldSize::INLINE_COMPOSITE) {
      // Small data-only struct.  Allocate a list of primitives instead.
      return initListPointer(ref, segment, elementCount, elementSize.preferredListEncoding);
    }

    auto wordsPerElement = elementSize.total() / ELEMENTS;

    // Allocate the list, prefixed by a single WirePointer.
    WordCount wordCount = elementCount * wordsPerElement;
    word* ptr = allocate(ref, segment, POINTER_SIZE_IN_WORDS + wordCount, WirePointer::LIST);

    // Initialize the pointer.
    // INLINE_COMPOSITE lists replace the element count with the word count.
    ref->listRef.setInlineComposite(wordCount);

    // Initialize the list tag.
    reinterpret_cast<WirePointer*>(ptr)->setKindAndInlineCompositeListElementCount(
        WirePointer::STRUCT, elementCount);
    reinterpret_cast<WirePointer*>(ptr)->structRef.set(elementSize);
    ptr += POINTER_SIZE_IN_WORDS;

    // Build the ListBuilder.
    return ListBuilder(segment, ptr, wordsPerElement * BITS_PER_WORD, elementCount,
                       elementSize.data * BITS_PER_WORD, elementSize.pointers);
  }

  static CAPNPROTO_ALWAYS_INLINE(ListBuilder getWritableListPointer(
      WirePointer* ref, SegmentBuilder* segment, const word* defaultValue)) {
    const WirePointer* defaultRef = reinterpret_cast<const WirePointer*>(defaultValue);
    word* ptr;

    if (ref->isNull()) {
      if (defaultValue == nullptr ||
          reinterpret_cast<const WirePointer*>(defaultValue)->isNull()) {
        return ListBuilder();
      }
      ptr = copyMessage(segment, ref, defaultRef);
    } else {
      ptr = followFars(ref, segment);

      PRECOND(ref->kind() == WirePointer::LIST,
          "Called getList{Field,Element}() but existing pointer is not a list.");
    }

    if (ref->listRef.elementSize() == FieldSize::INLINE_COMPOSITE) {
      // Read the tag to get the actual element count.
      WirePointer* tag = reinterpret_cast<WirePointer*>(ptr);
      PRECOND(tag->kind() == WirePointer::STRUCT,
          "INLINE_COMPOSITE list with non-STRUCT elements not supported.");

      // First list element is at tag + 1 pointer.
      return ListBuilder(segment, tag + 1, tag->structRef.wordSize() * BITS_PER_WORD / ELEMENTS,
                         tag->inlineCompositeListElementCount(),
                         tag->structRef.dataSize.get() * BITS_PER_WORD,
                         tag->structRef.ptrCount.get());
    } else {
      BitCount dataSize = dataBitsPerElement(ref->listRef.elementSize()) * ELEMENTS;
      WirePointerCount pointerCount = pointersPerElement(ref->listRef.elementSize()) * ELEMENTS;
      auto step = (dataSize + pointerCount * BITS_PER_POINTER) / ELEMENTS;
      return ListBuilder(segment, ptr, step, ref->listRef.elementCount(), dataSize, pointerCount);
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(Text::Builder initTextPointer(
      WirePointer* ref, SegmentBuilder* segment, ByteCount size)) {
    // The byte list must include a NUL terminator.
    ByteCount byteSize = size + 1 * BYTES;

    // Allocate the space.
    word* ptr = allocate(ref, segment, roundUpToWords(byteSize), WirePointer::LIST);

    // Initialize the pointer.
    ref->listRef.set(FieldSize::BYTE, byteSize * (1 * ELEMENTS / BYTES));

    // Build the Text::Builder.  This will initialize the NUL terminator.
    return Text::Builder(reinterpret_cast<char*>(ptr), size / BYTES);
  }

  static CAPNPROTO_ALWAYS_INLINE(void setTextPointer(
      WirePointer* ref, SegmentBuilder* segment, Text::Reader value)) {
    initTextPointer(ref, segment, value.size() * BYTES).copyFrom(value);
  }

  static CAPNPROTO_ALWAYS_INLINE(Text::Builder getWritableTextPointer(
      WirePointer* ref, SegmentBuilder* segment,
      const void* defaultValue, ByteCount defaultSize)) {
    if (ref->isNull()) {
      Text::Builder builder = initTextPointer(ref, segment, defaultSize);
      builder.copyFrom(defaultValue);
      return builder;
    } else {
      word* ptr = followFars(ref, segment);

      PRECOND(ref->kind() == WirePointer::LIST,
          "Called getText{Field,Element}() but existing pointer is not a list.");
      PRECOND(ref->listRef.elementSize() == FieldSize::BYTE,
          "Called getText{Field,Element}() but existing list pointer is not byte-sized.");

      // Subtract 1 from the size for the NUL terminator.
      return Text::Builder(reinterpret_cast<char*>(ptr), ref->listRef.elementCount() / ELEMENTS - 1);
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(Data::Builder initDataPointer(
      WirePointer* ref, SegmentBuilder* segment, ByteCount size)) {
    // Allocate the space.
    word* ptr = allocate(ref, segment, roundUpToWords(size), WirePointer::LIST);

    // Initialize the pointer.
    ref->listRef.set(FieldSize::BYTE, size * (1 * ELEMENTS / BYTES));

    // Build the Data::Builder.
    return Data::Builder(reinterpret_cast<char*>(ptr), size / BYTES);
  }

  static CAPNPROTO_ALWAYS_INLINE(void setDataPointer(
      WirePointer* ref, SegmentBuilder* segment, Data::Reader value)) {
    initDataPointer(ref, segment, value.size() * BYTES).copyFrom(value);
  }

  static CAPNPROTO_ALWAYS_INLINE(Data::Builder getWritableDataPointer(
      WirePointer* ref, SegmentBuilder* segment,
      const void* defaultValue, ByteCount defaultSize)) {
    if (ref->isNull()) {
      Data::Builder builder = initDataPointer(ref, segment, defaultSize);
      builder.copyFrom(defaultValue);
      return builder;
    } else {
      word* ptr = followFars(ref, segment);

      PRECOND(ref->kind() == WirePointer::LIST,
          "Called getData{Field,Element}() but existing pointer is not a list.");
      PRECOND(ref->listRef.elementSize() == FieldSize::BYTE,
          "Called getData{Field,Element}() but existing list pointer is not byte-sized.");

      return Data::Builder(reinterpret_cast<char*>(ptr), ref->listRef.elementCount() / ELEMENTS);
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(ObjectBuilder getWritableObjectPointer(
      SegmentBuilder* segment, WirePointer* ref, const word* defaultValue)) {
    word* ptr;

    if (ref->isNull()) {
      if (defaultValue == nullptr ||
          reinterpret_cast<const WirePointer*>(defaultValue)->isNull()) {
        return ObjectBuilder();
      } else {
        ptr = copyMessage(segment, ref, reinterpret_cast<const WirePointer*>(defaultValue));
      }
    } else {
      ptr = followFars(ref, segment);
    }

    if (ref->kind() == WirePointer::LIST) {
      if (ref->listRef.elementSize() == FieldSize::INLINE_COMPOSITE) {
        // Read the tag to get the actual element count.
        WirePointer* tag = reinterpret_cast<WirePointer*>(ptr);
        PRECOND(tag->kind() == WirePointer::STRUCT,
            "INLINE_COMPOSITE list with non-STRUCT elements not supported.");

        // First list element is at tag + 1 pointer.
        return ObjectBuilder(
            ListBuilder(segment, tag + 1, tag->structRef.wordSize() * BITS_PER_WORD / ELEMENTS,
                        tag->inlineCompositeListElementCount(),
                        tag->structRef.dataSize.get() * BITS_PER_WORD,
                        tag->structRef.ptrCount.get()));
      } else {
        BitCount dataSize = dataBitsPerElement(ref->listRef.elementSize()) * ELEMENTS;
        WirePointerCount pointerCount =
            pointersPerElement(ref->listRef.elementSize()) * ELEMENTS;
        auto step = (dataSize + pointerCount * BITS_PER_POINTER) / ELEMENTS;
        return ObjectBuilder(ListBuilder(
            segment, ptr, step, ref->listRef.elementCount(), dataSize, pointerCount));
      }
    } else {
      return ObjectBuilder(StructBuilder(
          segment, ptr,
          reinterpret_cast<WirePointer*>(ptr + ref->structRef.dataSize.get()),
          ref->structRef.dataSize.get() * BITS_PER_WORD,
          ref->structRef.ptrCount.get(),
          0 * BITS));
    }
  }

  // -----------------------------------------------------------------

  static CAPNPROTO_ALWAYS_INLINE(StructReader readStructPointer(
      SegmentReader* segment, const WirePointer* ref, const word* defaultValue,
      int nestingLimit)) {
    const word* ptr;

    if (ref == nullptr || ref->isNull()) {
    useDefault:
      if (defaultValue == nullptr ||
          reinterpret_cast<const WirePointer*>(defaultValue)->isNull()) {
        return StructReader(nullptr, nullptr, nullptr, 0 * BITS, 0 * POINTERS, 0 * BITS,
                            std::numeric_limits<int>::max());
      }
      segment = nullptr;
      ref = reinterpret_cast<const WirePointer*>(defaultValue);
      ptr = ref->target();
    } else if (segment != nullptr) {
      VALIDATE_INPUT(nestingLimit > 0,
            "Message is too deeply-nested or contains cycles.  See capnproto::ReadOptions.") {
        goto useDefault;
      }

      ptr = followFars(ref, segment);
      if (CAPNPROTO_EXPECT_FALSE(ptr == nullptr)) {
        // Already reported the error.
        goto useDefault;
      }

      VALIDATE_INPUT(ref->kind() == WirePointer::STRUCT,
            "Message contains non-struct pointer where struct pointer was expected.") {
        goto useDefault;
      }

      VALIDATE_INPUT(segment->containsInterval(ptr, ptr + ref->structRef.wordSize()),
            "Message contained out-of-bounds struct pointer.") {
        goto useDefault;
      }
    } else {
      // Trusted messages don't contain far pointers.
      ptr = ref->target();
    }

    return StructReader(
        segment, ptr, reinterpret_cast<const WirePointer*>(ptr + ref->structRef.dataSize.get()),
        ref->structRef.dataSize.get() * BITS_PER_WORD,
        ref->structRef.ptrCount.get(),
        0 * BITS, nestingLimit - 1);
  }

  static CAPNPROTO_ALWAYS_INLINE(ListReader readListPointer(
      SegmentReader* segment, const WirePointer* ref, const word* defaultValue,
      FieldSize expectedElementSize, int nestingLimit)) {
    const word* ptr;
    if (ref == nullptr || ref->isNull()) {
    useDefault:
      if (defaultValue == nullptr ||
          reinterpret_cast<const WirePointer*>(defaultValue)->isNull()) {
        return ListReader();
      }
      segment = nullptr;
      ref = reinterpret_cast<const WirePointer*>(defaultValue);
      ptr = ref->target();
    } else if (segment != nullptr) {
      VALIDATE_INPUT(nestingLimit > 0,
            "Message is too deeply-nested or contains cycles.  See capnproto::ReadOptions.") {
        goto useDefault;
      }

      ptr = followFars(ref, segment);
      if (CAPNPROTO_EXPECT_FALSE(ptr == nullptr)) {
        // Already reported error.
        goto useDefault;
      }

      VALIDATE_INPUT(ref->kind() == WirePointer::LIST,
            "Message contains non-list pointer where list pointer was expected.") {
        goto useDefault;
      }
    } else {
      // Trusted messages don't contain far pointers.
      ptr = ref->target();
    }

    if (ref->listRef.elementSize() == FieldSize::INLINE_COMPOSITE) {
      decltype(WORDS/ELEMENTS) wordsPerElement;
      ElementCount size;

      WordCount wordCount = ref->listRef.inlineCompositeWordCount();

      // An INLINE_COMPOSITE list points to a tag, which is formatted like a pointer.
      const WirePointer* tag = reinterpret_cast<const WirePointer*>(ptr);
      ptr += POINTER_SIZE_IN_WORDS;

      if (segment != nullptr) {
        VALIDATE_INPUT(segment->containsInterval(ptr - POINTER_SIZE_IN_WORDS, ptr + wordCount),
              "Message contains out-of-bounds list pointer.") {
          goto useDefault;
        }

        VALIDATE_INPUT(tag->kind() == WirePointer::STRUCT,
              "INLINE_COMPOSITE lists of non-STRUCT type are not supported.") {
          goto useDefault;
        }

        size = tag->inlineCompositeListElementCount();
        wordsPerElement = tag->structRef.wordSize() / ELEMENTS;

        VALIDATE_INPUT(size * wordsPerElement <= wordCount,
              "INLINE_COMPOSITE list's elements overrun its word count.") {
          goto useDefault;
        }

        // If a struct list was not expected, then presumably a non-struct list was upgraded to a
        // struct list.  We need to manipulate the pointer to point at the first field of the
        // struct.  Together with the "stepBits", this will allow the struct list to be accessed as
        // if it were a primitive list without branching.

        // Check whether the size is compatible.
        switch (expectedElementSize) {
          case FieldSize::VOID:
            break;

          case FieldSize::BIT:
            FAIL_VALIDATE_INPUT("Expected a bit list, but got a list of structs.") {
              goto useDefault;
            }
            break;

          case FieldSize::BYTE:
          case FieldSize::TWO_BYTES:
          case FieldSize::FOUR_BYTES:
          case FieldSize::EIGHT_BYTES:
            VALIDATE_INPUT(tag->structRef.dataSize.get() > 0 * WORDS,
                  "Expected a primitive list, but got a list of pointer-only structs.") {
              goto useDefault;
            }
            break;

          case FieldSize::POINTER:
            // We expected a list of pointers but got a list of structs.  Assuming the first field
            // in the struct is the pointer we were looking for, we want to munge the pointer to
            // point at the first element's pointer segment.
            ptr += tag->structRef.dataSize.get();
            VALIDATE_INPUT(tag->structRef.ptrCount.get() > 0 * POINTERS,
                  "Expected a pointer list, but got a list of data-only structs.") {
              goto useDefault;
            }
            break;

          case FieldSize::INLINE_COMPOSITE:
            break;
        }

      } else {
        // Trusted message.
        // This logic is equivalent to the other branch, above, but skipping all the checks.
        size = tag->inlineCompositeListElementCount();
        wordsPerElement = tag->structRef.wordSize() / ELEMENTS;

        if (expectedElementSize == FieldSize::POINTER) {
          ptr += tag->structRef.dataSize.get();
        }
      }

      return ListReader(
          segment, ptr, size, wordsPerElement * BITS_PER_WORD,
          tag->structRef.dataSize.get() * BITS_PER_WORD,
          tag->structRef.ptrCount.get(), nestingLimit - 1);

    } else {
      // This is a primitive or pointer list, but all such lists can also be interpreted as struct
      // lists.  We need to compute the data size and pointer count for such structs.
      BitCount dataSize = dataBitsPerElement(ref->listRef.elementSize()) * ELEMENTS;
      WirePointerCount pointerCount =
          pointersPerElement(ref->listRef.elementSize()) * ELEMENTS;
      auto step = (dataSize + pointerCount * BITS_PER_POINTER) / ELEMENTS;

      if (segment != nullptr) {
        VALIDATE_INPUT(segment->containsInterval(ptr, ptr +
              roundUpToWords(ElementCount64(ref->listRef.elementCount()) * step)),
              "Message contains out-of-bounds list pointer.") {
          goto useDefault;
        }
      }

      if (segment != nullptr) {
        // Verify that the elements are at least as large as the expected type.  Note that if we
        // expected INLINE_COMPOSITE, the expected sizes here will be zero, because bounds checking
        // will be performed at field access time.  So this check here is for the case where we
        // expected a list of some primitive or pointer type.

        BitCount expectedDataBitsPerElement =
            dataBitsPerElement(expectedElementSize) * ELEMENTS;
        WirePointerCount expectedPointersPerElement =
            pointersPerElement(expectedElementSize) * ELEMENTS;

        VALIDATE_INPUT(expectedDataBitsPerElement <= dataSize,
            "Message contained list with incompatible element type.") {
          goto useDefault;
        }
        VALIDATE_INPUT(expectedPointersPerElement <= pointerCount,
            "Message contained list with incompatible element type.") {
          goto useDefault;
        }
      }

      return ListReader(segment, ptr, ref->listRef.elementCount(), step,
                        dataSize, pointerCount, nestingLimit - 1);
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(Text::Reader readTextPointer(
      SegmentReader* segment, const WirePointer* ref,
      const void* defaultValue, ByteCount defaultSize)) {
    if (ref == nullptr || ref->isNull()) {
    useDefault:
      if (defaultValue == nullptr ||
          reinterpret_cast<const WirePointer*>(defaultValue)->isNull()) {
        defaultValue = "";
      }
      return Text::Reader(reinterpret_cast<const char*>(defaultValue), defaultSize / BYTES);
    } else if (segment == nullptr) {
      // Trusted message.
      return Text::Reader(reinterpret_cast<const char*>(ref->target()),
                          ref->listRef.elementCount() / ELEMENTS - 1);
    } else {
      const word* ptr = followFars(ref, segment);

      if (CAPNPROTO_EXPECT_FALSE(ptr == nullptr)) {
        // Already reported error.
        goto useDefault;
      }

      uint size = ref->listRef.elementCount() / ELEMENTS;

      VALIDATE_INPUT(ref->kind() == WirePointer::LIST,
            "Message contains non-list pointer where text was expected.") {
        goto useDefault;
      }

      VALIDATE_INPUT(ref->listRef.elementSize() == FieldSize::BYTE,
            "Message contains list pointer of non-bytes where text was expected.") {
        goto useDefault;
      }

      VALIDATE_INPUT(segment->containsInterval(ptr, ptr +
            roundUpToWords(ref->listRef.elementCount() * (1 * BYTES / ELEMENTS))),
            "Message contained out-of-bounds text pointer.") {
        goto useDefault;
      }

      VALIDATE_INPUT(size > 0, "Message contains text that is not NUL-terminated.") {
        goto useDefault;
      }

      const char* cptr = reinterpret_cast<const char*>(ptr);
      --size;  // NUL terminator

      VALIDATE_INPUT(cptr[size] == '\0', "Message contains text that is not NUL-terminated.") {
        goto useDefault;
      }

      return Text::Reader(cptr, size);
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(Data::Reader readDataPointer(
      SegmentReader* segment, const WirePointer* ref,
      const void* defaultValue, ByteCount defaultSize)) {
    if (ref == nullptr || ref->isNull()) {
    useDefault:
      return Data::Reader(reinterpret_cast<const char*>(defaultValue), defaultSize / BYTES);
    } else if (segment == nullptr) {
      // Trusted message.
      return Data::Reader(reinterpret_cast<const char*>(ref->target()),
                        ref->listRef.elementCount() / ELEMENTS);
    } else {
      const word* ptr = followFars(ref, segment);

      if (CAPNPROTO_EXPECT_FALSE(ptr == nullptr)) {
        // Already reported error.
        goto useDefault;
      }

      uint size = ref->listRef.elementCount() / ELEMENTS;

      VALIDATE_INPUT(ref->kind() == WirePointer::LIST,
            "Message contains non-list pointer where data was expected.") {
        goto useDefault;
      }

      VALIDATE_INPUT(ref->listRef.elementSize() == FieldSize::BYTE,
            "Message contains list pointer of non-bytes where data was expected.") {
        goto useDefault;
      }

      VALIDATE_INPUT(segment->containsInterval(ptr, ptr +
            roundUpToWords(ref->listRef.elementCount() * (1 * BYTES / ELEMENTS))),
            "Message contained out-of-bounds data pointer.") {
        goto useDefault;
      }

      return Data::Reader(reinterpret_cast<const char*>(ptr), size);
    }
  }

  static CAPNPROTO_ALWAYS_INLINE(ObjectReader readObjectPointer(
      SegmentReader* segment, const WirePointer* ref,
      const word* defaultValue, int nestingLimit)) {
    // We can't really reuse readStructPointer() and readListPointer() because they are designed
    // for the case where we are expecting a specific type, and they do validation around that,
    // whereas this method is for the case where we accept any pointer.

    const word* ptr;
    if (ref == nullptr || ref->isNull()) {
    useDefault:
      if (defaultValue == nullptr ||
          reinterpret_cast<const WirePointer*>(defaultValue)->isNull()) {
        return ObjectReader();
      }
      segment = nullptr;
      ref = reinterpret_cast<const WirePointer*>(defaultValue);
      ptr = ref->target();
    } else if (segment != nullptr) {
      ptr = WireHelpers::followFars(ref, segment);
      if (CAPNPROTO_EXPECT_FALSE(ptr == nullptr)) {
        // Already reported the error.
        goto useDefault;
      }
    } else {
      ptr = ref->target();
    }

    switch (ref->kind()) {
      case WirePointer::STRUCT:
        if (segment != nullptr) {
          VALIDATE_INPUT(nestingLimit > 0,
                "Message is too deeply-nested or contains cycles.  See capnproto::ReadOptions.") {
            goto useDefault;
          }

          VALIDATE_INPUT(segment->containsInterval(ptr, ptr + ref->structRef.wordSize()),
                "Message contained out-of-bounds struct pointer.") {
            goto useDefault;
          }
        }
        return ObjectReader(
            StructReader(segment, ptr,
                         reinterpret_cast<const WirePointer*>(ptr + ref->structRef.dataSize.get()),
                         ref->structRef.dataSize.get() * BITS_PER_WORD,
                         ref->structRef.ptrCount.get(),
                         0 * BITS, nestingLimit - 1));
      case WirePointer::LIST: {
        FieldSize elementSize = ref->listRef.elementSize();

        if (segment != nullptr) {
          VALIDATE_INPUT(nestingLimit > 0,
                "Message is too deeply-nested or contains cycles.  See capnproto::ReadOptions.") {
            goto useDefault;
          }
        }

        if (elementSize == FieldSize::INLINE_COMPOSITE) {
          WordCount wordCount = ref->listRef.inlineCompositeWordCount();
          const WirePointer* tag = reinterpret_cast<const WirePointer*>(ptr);
          ptr += POINTER_SIZE_IN_WORDS;

          if (segment != nullptr) {
            VALIDATE_INPUT(segment->containsInterval(ptr - POINTER_SIZE_IN_WORDS,
                                                     ptr + wordCount),
                "Message contains out-of-bounds list pointer.") {
              goto useDefault;
            }

            VALIDATE_INPUT(tag->kind() == WirePointer::STRUCT,
                  "INLINE_COMPOSITE lists of non-STRUCT type are not supported.") {
              goto useDefault;
            }
          }

          ElementCount elementCount = tag->inlineCompositeListElementCount();
          auto wordsPerElement = tag->structRef.wordSize() / ELEMENTS;

          if (segment != nullptr) {
            VALIDATE_INPUT(wordsPerElement * elementCount <= wordCount,
                "INLINE_COMPOSITE list's elements overrun its word count.");
          }

          return ObjectReader(
              ListReader(segment, ptr, elementCount, wordsPerElement * BITS_PER_WORD,
                         tag->structRef.dataSize.get() * BITS_PER_WORD,
                         tag->structRef.ptrCount.get(), nestingLimit - 1));
        } else {
          BitCount dataSize = dataBitsPerElement(elementSize) * ELEMENTS;
          WirePointerCount pointerCount = pointersPerElement(elementSize) * ELEMENTS;
          auto step = (dataSize + pointerCount * BITS_PER_POINTER) / ELEMENTS;
          ElementCount elementCount = ref->listRef.elementCount();
          WordCount wordCount = roundUpToWords(ElementCount64(elementCount) * step);

          if (segment != nullptr) {
            VALIDATE_INPUT(segment->containsInterval(ptr, ptr + wordCount),
                  "Message contains out-of-bounds list pointer.") {
              goto useDefault;
            }
          }

          return ObjectReader(
              ListReader(segment, ptr, elementCount, step, dataSize, pointerCount,
                         nestingLimit - 1));
        }
      }
      default:
        FAIL_VALIDATE_INPUT("Message contained invalid pointer.") {}
        goto useDefault;
    }
  }
};

// =======================================================================================
// StructBuilder

StructBuilder StructBuilder::initRoot(
    SegmentBuilder* segment, word* location, StructSize size) {
  return WireHelpers::initStructPointer(
      reinterpret_cast<WirePointer*>(location), segment, size);
}

StructBuilder StructBuilder::getRoot(
    SegmentBuilder* segment, word* location, StructSize size) {
  return WireHelpers::getWritableStructPointer(
      reinterpret_cast<WirePointer*>(location), segment, size, nullptr);
}

StructBuilder StructBuilder::initStructField(
    WirePointerCount ptrIndex, StructSize size) const {
  return WireHelpers::initStructPointer(pointers + ptrIndex, segment, size);
}

StructBuilder StructBuilder::getStructField(
    WirePointerCount ptrIndex, StructSize size, const word* defaultValue) const {
  return WireHelpers::getWritableStructPointer(
      pointers + ptrIndex, segment, size, defaultValue);
}

ListBuilder StructBuilder::initListField(
    WirePointerCount ptrIndex, FieldSize elementSize, ElementCount elementCount) const {
  return WireHelpers::initListPointer(
      pointers + ptrIndex, segment,
      elementCount, elementSize);
}

ListBuilder StructBuilder::initStructListField(
    WirePointerCount ptrIndex, ElementCount elementCount, StructSize elementSize) const {
  return WireHelpers::initStructListPointer(
      pointers + ptrIndex, segment, elementCount, elementSize);
}

ListBuilder StructBuilder::getListField(
    WirePointerCount ptrIndex, const word* defaultValue) const {
  return WireHelpers::getWritableListPointer(
      pointers + ptrIndex, segment, defaultValue);
}

template <>
Text::Builder StructBuilder::initBlobField<Text>(WirePointerCount ptrIndex, ByteCount size) const {
  return WireHelpers::initTextPointer(pointers + ptrIndex, segment, size);
}
template <>
void StructBuilder::setBlobField<Text>(WirePointerCount ptrIndex, Text::Reader value) const {
  WireHelpers::setTextPointer(pointers + ptrIndex, segment, value);
}
template <>
Text::Builder StructBuilder::getBlobField<Text>(
    WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize) const {
  return WireHelpers::getWritableTextPointer(
      pointers + ptrIndex, segment, defaultValue, defaultSize);
}

template <>
Data::Builder StructBuilder::initBlobField<Data>(WirePointerCount ptrIndex, ByteCount size) const {
  return WireHelpers::initDataPointer(pointers + ptrIndex, segment, size);
}
template <>
void StructBuilder::setBlobField<Data>(WirePointerCount ptrIndex, Data::Reader value) const {
  WireHelpers::setDataPointer(pointers + ptrIndex, segment, value);
}
template <>
Data::Builder StructBuilder::getBlobField<Data>(
    WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize) const {
  return WireHelpers::getWritableDataPointer(
      pointers + ptrIndex, segment, defaultValue, defaultSize);
}

ObjectBuilder StructBuilder::getObjectField(
    WirePointerCount ptrIndex, const word* defaultValue) const {
  return WireHelpers::getWritableObjectPointer(segment, pointers + ptrIndex, defaultValue);
}

StructReader StructBuilder::asReader() const {
  return StructReader(segment, data, pointers,
      dataSize, pointerCount, bit0Offset, std::numeric_limits<int>::max());
}

// =======================================================================================
// StructReader

StructReader StructReader::readRootTrusted(const word* location) {
  return WireHelpers::readStructPointer(nullptr, reinterpret_cast<const WirePointer*>(location),
                                          nullptr, std::numeric_limits<int>::max());
}

StructReader StructReader::readRoot(
    const word* location, SegmentReader* segment, int nestingLimit) {
  VALIDATE_INPUT(segment->containsInterval(location, location + POINTER_SIZE_IN_WORDS),
                 "Root location out-of-bounds.") {
    location = nullptr;
  }

  return WireHelpers::readStructPointer(segment, reinterpret_cast<const WirePointer*>(location),
                                          nullptr, nestingLimit);
}

StructReader StructReader::getStructField(
    WirePointerCount ptrIndex, const word* defaultValue) const {
  const WirePointer* ref = ptrIndex >= pointerCount ? nullptr : pointers + ptrIndex;
  return WireHelpers::readStructPointer(segment, ref, defaultValue, nestingLimit);
}

ListReader StructReader::getListField(
    WirePointerCount ptrIndex, FieldSize expectedElementSize, const word* defaultValue) const {
  const WirePointer* ref = ptrIndex >= pointerCount ? nullptr : pointers + ptrIndex;
  return WireHelpers::readListPointer(
      segment, ref, defaultValue, expectedElementSize, nestingLimit);
}

template <>
Text::Reader StructReader::getBlobField<Text>(
    WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize) const {
  const WirePointer* ref = ptrIndex >= pointerCount ? nullptr : pointers + ptrIndex;
  return WireHelpers::readTextPointer(segment, ref, defaultValue, defaultSize);
}

template <>
Data::Reader StructReader::getBlobField<Data>(
    WirePointerCount ptrIndex, const void* defaultValue, ByteCount defaultSize) const {
  const WirePointer* ref = ptrIndex >= pointerCount ? nullptr : pointers + ptrIndex;
  return WireHelpers::readDataPointer(segment, ref, defaultValue, defaultSize);
}

ObjectReader StructReader::getObjectField(
    WirePointerCount ptrIndex, const word* defaultValue) const {
  return WireHelpers::readObjectPointer(
      segment, pointers + ptrIndex, defaultValue, nestingLimit);
}

const word* StructReader::getTrustedPointer(WirePointerCount ptrIndex) const {
  PRECOND(segment == nullptr, "getTrustedPointer() only allowed on trusted messages.");
  return reinterpret_cast<const word*>(pointers + ptrIndex);
}

// =======================================================================================
// ListBuilder

Text::Builder ListBuilder::asText() {
  VALIDATE_INPUT(structDataSize == 8 * BITS && structPointerCount == 0 * POINTERS,
      "Expected Text, got list of non-bytes.") {
    return Text::Builder();
  }

  size_t size = elementCount / ELEMENTS;

  VALIDATE_INPUT(size > 0, "Message contains text that is not NUL-terminated.") {
    return Text::Builder();
  }

  char* cptr = reinterpret_cast<char*>(ptr);
  --size;  // NUL terminator

  VALIDATE_INPUT(cptr[size] == '\0', "Message contains text that is not NUL-terminated.") {
    return Text::Builder();
  }

  return Text::Builder(cptr, size);
}

Data::Builder ListBuilder::asData() {
  VALIDATE_INPUT(structDataSize == 8 * BITS && structPointerCount == 0 * POINTERS,
      "Expected Text, got list of non-bytes.") {
    return Data::Builder();
  }

  return Data::Builder(reinterpret_cast<char*>(ptr), elementCount / ELEMENTS);
}

StructBuilder ListBuilder::getStructElement(ElementCount index) const {
  BitCount64 indexBit = ElementCount64(index) * step;
  byte* structData = ptr + indexBit / BITS_PER_BYTE;
  return StructBuilder(segment, structData,
      reinterpret_cast<WirePointer*>(structData + structDataSize / BITS_PER_BYTE),
      structDataSize, structPointerCount, indexBit % BITS_PER_BYTE);
}

ListBuilder ListBuilder::initListElement(
    ElementCount index, FieldSize elementSize, ElementCount elementCount) const {
  return WireHelpers::initListPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE),
      segment, elementCount, elementSize);
}

ListBuilder ListBuilder::initStructListElement(
    ElementCount index, ElementCount elementCount, StructSize elementSize) const {
  return WireHelpers::initStructListPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE),
      segment, elementCount, elementSize);
}

ListBuilder ListBuilder::getListElement(ElementCount index) const {
  return WireHelpers::getWritableListPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), segment, nullptr);
}

template <>
Text::Builder ListBuilder::initBlobElement<Text>(ElementCount index, ByteCount size) const {
  return WireHelpers::initTextPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), segment, size);
}
template <>
void ListBuilder::setBlobElement<Text>(ElementCount index, Text::Reader value) const {
  WireHelpers::setTextPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), segment, value);
}
template <>
Text::Builder ListBuilder::getBlobElement<Text>(ElementCount index) const {
  return WireHelpers::getWritableTextPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), segment, "", 0 * BYTES);
}

template <>
Data::Builder ListBuilder::initBlobElement<Data>(ElementCount index, ByteCount size) const {
  return WireHelpers::initDataPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), segment, size);
}
template <>
void ListBuilder::setBlobElement<Data>(ElementCount index, Data::Reader value) const {
  WireHelpers::setDataPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), segment, value);
}
template <>
Data::Builder ListBuilder::getBlobElement<Data>(ElementCount index) const {
  return WireHelpers::getWritableDataPointer(
      reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), segment, nullptr,
      0 * BYTES);
}

ObjectBuilder ListBuilder::getObjectElement(ElementCount index) const {
  return WireHelpers::getWritableObjectPointer(
      segment, reinterpret_cast<WirePointer*>(ptr + index * step / BITS_PER_BYTE), nullptr);
}

ListReader ListBuilder::asReader() const {
  return ListReader(segment, ptr, elementCount, step, structDataSize, structPointerCount,
                    std::numeric_limits<int>::max());
}

// =======================================================================================
// ListReader

Text::Reader ListReader::asText() {
  VALIDATE_INPUT(structDataSize == 8 * BITS && structPointerCount == 0 * POINTERS,
      "Expected Text, got list of non-bytes.") {
    return Text::Reader();
  }

  size_t size = elementCount / ELEMENTS;

  VALIDATE_INPUT(size > 0, "Message contains text that is not NUL-terminated.") {
    return Text::Reader();
  }

  const char* cptr = reinterpret_cast<const char*>(ptr);
  --size;  // NUL terminator

  VALIDATE_INPUT(cptr[size] == '\0', "Message contains text that is not NUL-terminated.") {
    return Text::Reader();
  }

  return Text::Reader(cptr, size);
}

Data::Reader ListReader::asData() {
  VALIDATE_INPUT(structDataSize == 8 * BITS && structPointerCount == 0 * POINTERS,
      "Expected Text, got list of non-bytes.") {
    return Data::Reader();
  }

  return Data::Reader(reinterpret_cast<const char*>(ptr), elementCount / ELEMENTS);
}

StructReader ListReader::getStructElement(ElementCount index) const {
  VALIDATE_INPUT((segment == nullptr) | (nestingLimit > 0),
        "Message is too deeply-nested or contains cycles.  See capnproto::ReadOptions.") {
    return StructReader();
  }

  BitCount64 indexBit = ElementCount64(index) * step;
  const byte* structData = ptr + indexBit / BITS_PER_BYTE;
  const WirePointer* structPointers =
      reinterpret_cast<const WirePointer*>(structData + structDataSize / BITS_PER_BYTE);

  // This check should pass if there are no bugs in the list pointer validation code.
  DCHECK(structPointerCount == 0 * POINTERS ||
         (uintptr_t)structPointers % sizeof(WirePointer) == 0,
         "Pointer segment of struct list element not aligned.");

  return StructReader(
      segment, structData, structPointers,
      structDataSize, structPointerCount,
      indexBit % BITS_PER_BYTE, nestingLimit - 1);
}

static const WirePointer* checkAlignment(const void* ptr) {
  DCHECK((uintptr_t)ptr % sizeof(WirePointer) == 0,
         "Pointer segment of struct list element not aligned.");
  return reinterpret_cast<const WirePointer*>(ptr);
}

ListReader ListReader::getListElement(
    ElementCount index, FieldSize expectedElementSize) const {
  return WireHelpers::readListPointer(
      segment, checkAlignment(ptr + index * step / BITS_PER_BYTE),
      nullptr, expectedElementSize, nestingLimit);
}

template <>
Text::Reader ListReader::getBlobElement<Text>(ElementCount index) const {
  return WireHelpers::readTextPointer(
      segment, checkAlignment(ptr + index * step / BITS_PER_BYTE),
      "", 0 * BYTES);
}

template <>
Data::Reader ListReader::getBlobElement<Data>(ElementCount index) const {
  return WireHelpers::readDataPointer(
      segment, checkAlignment(ptr + index * step / BITS_PER_BYTE),
      nullptr, 0 * BYTES);
}

ObjectReader ListReader::getObjectElement(ElementCount index) const {
  return WireHelpers::readObjectPointer(
      segment, checkAlignment(ptr + index * step / BITS_PER_BYTE), nullptr, nestingLimit);
}

}  // namespace internal
}  // namespace capnproto