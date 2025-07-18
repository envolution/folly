/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/ConstexprMath.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Align.h>
#include <folly/lang/Exception.h>
#include <folly/lang/Thunk.h>
#include <folly/memory/Malloc.h>
#include <folly/portability/Config.h>
#include <folly/portability/Constexpr.h>
#include <folly/portability/Malloc.h>

namespace folly {

namespace access {

/// to_address_fn
/// to_address
///
/// mimic: std::to_address (C++20)
///
/// adapted from: https://en.cppreference.com/w/cpp/memory/to_address, CC-BY-SA
struct to_address_fn {
 private:
  template <template <typename...> typename T, typename A, typename... B>
  static tag_t<A> get_first_arg(tag_t<T<A, B...>>);
  template <typename T>
  using first_arg_of = type_list_element_t<0, decltype(get_first_arg(tag<T>))>;
  template <typename T>
  using detect_element_type = typename T::element_type;
  template <typename T>
  using element_type_of =
      detected_or_t<first_arg_of<T>, detect_element_type, T>;

  template <typename T>
  using detect_to_address =
      decltype(std::pointer_traits<T>::to_address(FOLLY_DECLVAL(T const&)));

  template <typename T>
  static inline constexpr bool use_pointer_traits_to_address = Conjunction<
      is_detected<element_type_of, T>,
      is_detected<detect_to_address, T>>::value;

 public:
  template <typename T>
  constexpr T* operator()(T* p) const noexcept {
    static_assert(!std::is_function_v<T>);
    return p;
  }

  template <typename T>
  constexpr auto operator()(T const& p) const noexcept {
    if constexpr (use_pointer_traits_to_address<T>) {
      static_assert(noexcept(std::pointer_traits<T>::to_address(p)));
      return std::pointer_traits<T>::to_address(p);
    } else {
      static_assert(noexcept(operator()(p.operator->())));
      return operator()(p.operator->());
    }
  }
};
inline constexpr to_address_fn to_address;

} // namespace access

#if (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L) || \
    (defined(_XOPEN_SOURCE) && _XOPEN_SOURCE >= 600) ||         \
    (defined(__ANDROID__) && (__ANDROID_API__ > 16)) ||         \
    (defined(__APPLE__)) || defined(__FreeBSD__) || defined(__wasm32__)

inline void* aligned_malloc(size_t size, size_t align) {
  // use posix_memalign, but mimic the behaviour of memalign
  void* ptr = nullptr;
  int rc = posix_memalign(&ptr, align, size);
  return rc == 0 ? (errno = 0, ptr) : (errno = rc, nullptr);
}

inline void aligned_free(void* aligned_ptr) {
  free(aligned_ptr);
}

#elif defined(_WIN32)

inline void* aligned_malloc(size_t size, size_t align) {
  return _aligned_malloc(size, align);
}

inline void aligned_free(void* aligned_ptr) {
  _aligned_free(aligned_ptr);
}

#else

inline void* aligned_malloc(size_t size, size_t align) {
  return memalign(align, size);
}

inline void aligned_free(void* aligned_ptr) {
  free(aligned_ptr);
}

#endif

namespace detail {
template <typename Alloc, size_t kAlign, bool kAllocate>
void rawOverAlignedImpl(Alloc const& alloc, size_t n, void*& raw) {
  static_assert((kAlign & (kAlign - 1)) == 0, "Align must be a power of 2");

  using AllocTraits = std::allocator_traits<Alloc>;
  using T = typename AllocTraits::value_type;

  constexpr bool kCanBypass = std::is_same<Alloc, std::allocator<T>>::value;

  // BaseType is a type that gives us as much alignment as we need if
  // we can get it naturally, otherwise it is aligned as max_align_t.
  // kBaseAlign is both the alignment and size of this type.
  constexpr size_t kBaseAlign = constexpr_min(kAlign, alignof(max_align_t));
  using BaseType = std::aligned_storage_t<kBaseAlign, kBaseAlign>;
  using BaseAllocTraits =
      typename AllocTraits::template rebind_traits<BaseType>;
  using BaseAlloc = typename BaseAllocTraits::allocator_type;
  static_assert(
      sizeof(BaseType) == kBaseAlign && alignof(BaseType) == kBaseAlign);

#if defined(__cpp_sized_deallocation)
  if (kCanBypass && kAlign == kBaseAlign) {
    // until std::allocator uses sized deallocation, it is worth the
    // effort to bypass it when we are able
    if (kAllocate) {
      raw = ::operator new(n * sizeof(T));
    } else {
      ::operator delete(raw, n * sizeof(T));
    }
    return;
  }
#endif

  if (kCanBypass && kAlign > kBaseAlign) {
    // allocating as BaseType isn't sufficient to get alignment, but
    // since we can bypass Alloc we can use something like posix_memalign.
    if (kAllocate) {
      raw = aligned_malloc(n * sizeof(T), kAlign);
    } else {
      aligned_free(raw);
    }
    return;
  }

  // we're not allowed to bypass Alloc, or we don't want to
  BaseAlloc a(alloc);

  // allocation size is counted in sizeof(BaseType)
  size_t quanta = (n * sizeof(T) + kBaseAlign - 1) / sizeof(BaseType);
  if (kAlign <= kBaseAlign) {
    // rebinding Alloc to BaseType is sufficient to get us the alignment
    // we want, happy path
    if (kAllocate) {
      raw = static_cast<void*>(
          std::addressof(*BaseAllocTraits::allocate(a, quanta)));
    } else {
      BaseAllocTraits::deallocate(
          a,
          std::pointer_traits<typename BaseAllocTraits::pointer>::pointer_to(
              *static_cast<BaseType*>(raw)),
          quanta);
    }
    return;
  }

  // Overaligned and custom allocator, our only option is to
  // overallocate and store a delta to the actual allocation just
  // before the returned ptr.
  //
  // If we give ourselves kAlign extra bytes, then since
  // sizeof(BaseType) divides kAlign we can meet alignment while
  // getting a prefix of one BaseType.  If we happen to get a
  // kAlign-aligned block, then we can return a pointer to underlying
  // + kAlign, otherwise there will be at least kBaseAlign bytes in
  // the unused prefix of the first kAlign-aligned block.
  if (kAllocate) {
    char* base = reinterpret_cast<char*>(std::addressof(
        *BaseAllocTraits::allocate(a, quanta + kAlign / sizeof(BaseType))));
    size_t byteDelta =
        kAlign - (reinterpret_cast<uintptr_t>(base) & (kAlign - 1));
    raw = static_cast<void*>(base + byteDelta);
    static_cast<size_t*>(raw)[-1] = byteDelta;
  } else {
    size_t byteDelta = static_cast<size_t*>(raw)[-1];
    char* base = static_cast<char*>(raw) - byteDelta;
    BaseAllocTraits::deallocate(
        a,
        std::pointer_traits<typename BaseAllocTraits::pointer>::pointer_to(
            *reinterpret_cast<BaseType*>(base)),
        quanta + kAlign / sizeof(BaseType));
  }
}
} // namespace detail

// Works like std::allocator_traits<Alloc>::allocate, but handles
// over-aligned types.  Feel free to manually specify any power of two as
// the Align template arg.  Must be matched with deallocateOverAligned.
// allocationBytesForOverAligned will give you the number of bytes that
// this function actually requests.
template <
    typename Alloc,
    size_t kAlign = alignof(typename std::allocator_traits<Alloc>::value_type)>
typename std::allocator_traits<Alloc>::pointer allocateOverAligned(
    Alloc const& alloc, size_t n) {
  void* raw = nullptr;
  detail::rawOverAlignedImpl<Alloc, kAlign, true>(alloc, n, raw);
  return std::pointer_traits<typename std::allocator_traits<Alloc>::pointer>::
      pointer_to(
          *static_cast<typename std::allocator_traits<Alloc>::value_type*>(
              raw));
}

template <
    typename Alloc,
    size_t kAlign = alignof(typename std::allocator_traits<Alloc>::value_type)>
void deallocateOverAligned(
    Alloc const& alloc,
    typename std::allocator_traits<Alloc>::pointer ptr,
    size_t n) {
  void* raw = static_cast<void*>(std::addressof(*ptr));
  detail::rawOverAlignedImpl<Alloc, kAlign, false>(alloc, n, raw);
}

template <
    typename Alloc,
    size_t kAlign = alignof(typename std::allocator_traits<Alloc>::value_type)>
size_t allocationBytesForOverAligned(size_t n) {
  static_assert((kAlign & (kAlign - 1)) == 0, "Align must be a power of 2");

  using AllocTraits = std::allocator_traits<Alloc>;
  using T = typename AllocTraits::value_type;

  constexpr size_t kBaseAlign = constexpr_min(kAlign, alignof(max_align_t));

  if (kAlign > kBaseAlign && std::is_same<Alloc, std::allocator<T>>::value) {
    return n * sizeof(T);
  } else {
    size_t quanta = (n * sizeof(T) + kBaseAlign - 1) / kBaseAlign;
    if (kAlign > kBaseAlign) {
      quanta += kAlign / kBaseAlign;
    }
    return quanta * kBaseAlign;
  }
}

/**
 * static_function_deleter
 *
 * So you can write this:
 *
 *      using RSA_deleter = folly::static_function_deleter<RSA, &RSA_free>;
 *      auto rsa = std::unique_ptr<RSA, RSA_deleter>(RSA_new());
 *      RSA_generate_key_ex(rsa.get(), bits, exponent, nullptr);
 *      rsa = nullptr;  // calls RSA_free(rsa.get())
 *
 * This would be sweet as well for BIO, but unfortunately BIO_free has signature
 * int(BIO*) while we require signature void(BIO*). So you would need to make a
 * wrapper for it:
 *
 *      inline void BIO_free_fb(BIO* bio) { CHECK_EQ(1, BIO_free(bio)); }
 *      using BIO_deleter = folly::static_function_deleter<BIO, &BIO_free_fb>;
 *      auto buf = std::unique_ptr<BIO, BIO_deleter>(BIO_new(BIO_s_mem()));
 *      buf = nullptr;  // calls BIO_free(buf.get())
 */

template <typename T, void (*f)(T*)>
struct static_function_deleter {
  void operator()(T* t) const { f(t); }
};

/**
 *  to_shared_ptr
 *
 *  Convert unique_ptr to shared_ptr without specifying the template type
 *  parameter and letting the compiler deduce it.
 *
 *  So you can write this:
 *
 *      auto sptr = to_shared_ptr(getSomethingUnique<T>());
 *
 *  Instead of this:
 *
 *      auto sptr = shared_ptr<T>(getSomethingUnique<T>());
 *
 *  Useful when `T` is long, such as:
 *
 *      using T = foobar::FooBarAsyncClient;
 */
template <typename T, typename D>
std::shared_ptr<T> to_shared_ptr(std::unique_ptr<T, D>&& ptr) {
  return std::shared_ptr<T>(std::move(ptr));
}

/**
 *  to_shared_ptr_aliasing
 */
template <typename T, typename U>
std::shared_ptr<U> to_shared_ptr_aliasing(std::shared_ptr<T> const& r, U* ptr) {
  return std::shared_ptr<U>(r, ptr);
}

/**
 *  to_shared_ptr_non_owning
 */
template <typename U>
std::shared_ptr<U> to_shared_ptr_non_owning(U* ptr) {
  return std::shared_ptr<U>(std::shared_ptr<void>{}, ptr);
}

/**
 *  to_weak_ptr
 *
 *  Make a weak_ptr and return it from a shared_ptr without specifying the
 *  template type parameter and letting the compiler deduce it.
 *
 *  So you can write this:
 *
 *      auto wptr = to_weak_ptr(getSomethingShared<T>());
 *
 *  Instead of this:
 *
 *      auto wptr = weak_ptr<T>(getSomethingShared<T>());
 *
 *  Useful when `T` is long, such as:
 *
 *      using T = foobar::FooBarAsyncClient;
 */
template <typename T>
std::weak_ptr<T> to_weak_ptr(const std::shared_ptr<T>& ptr) {
  return ptr;
}

#if defined(__GLIBCXX__)
namespace detail {
void weak_ptr_set_stored_ptr(std::weak_ptr<void>& w, void* ptr);

template <typename Tag, void* std::__weak_ptr<void>::*WeakPtr_Ptr_Field>
struct GenerateWeakPtrInternalsAccessor {
  friend void weak_ptr_set_stored_ptr(std::weak_ptr<void>& w, void* ptr) {
    w.*WeakPtr_Ptr_Field = ptr;
  }
};

// Each template instantiation of GenerateWeakPtrInternalsAccessor must
// be a new type, to avoid ODR problems.  We do this by tagging it with
// a type from an anon namespace.
namespace {
struct MemoryAnonTag {};
} // namespace

template struct GenerateWeakPtrInternalsAccessor<
    MemoryAnonTag,
    &std::__weak_ptr<void>::_M_ptr>;
} // namespace detail
#endif

/**
 *  to_weak_ptr_aliasing
 *
 *  Like to_weak_ptr, but arranges that lock().get() on the returned
 *  pointer points to ptr rather than r.get().
 *
 *  Equivalent to:
 *
 *      to_weak_ptr(std::shared_ptr<U>(r, ptr))
 *
 *  For libstdc++, ABI-specific tricks are used to optimize the
 *  implementation.
 */
template <typename T, typename U>
std::weak_ptr<U> to_weak_ptr_aliasing(const std::shared_ptr<T>& r, U* ptr) {
#if defined(__GLIBCXX__)
  std::weak_ptr<void> wv(r);
  detail::weak_ptr_set_stored_ptr(wv, ptr);
  FOLLY_PUSH_WARNING
  FOLLY_GCC_DISABLE_WARNING("-Wstrict-aliasing")
  return reinterpret_cast<std::weak_ptr<U>&&>(wv);
  FOLLY_POP_WARNING
#else
  return std::shared_ptr<U>(r, ptr);
#endif
}

/**
 *  copy_to_unique_ptr
 *
 *  Move or copy the argument to the heap and return it owned by a unique_ptr.
 *
 *  Like std::make_unique, but deduces the type of the owned object.
 */
template <typename T>
std::unique_ptr<remove_cvref_t<T>> copy_to_unique_ptr(T&& t) {
  return std::make_unique<remove_cvref_t<T>>(static_cast<T&&>(t));
}

/**
 *  copy_to_shared_ptr
 *
 *  Move or copy the argument to the heap and return it owned by a shared_ptr.
 *
 *  Like make_shared, but deduces the type of the owned object.
 */
template <typename T>
std::shared_ptr<remove_cvref_t<T>> copy_to_shared_ptr(T&& t) {
  return std::make_shared<remove_cvref_t<T>>(static_cast<T&&>(t));
}

/**
 *  copy_through_unique_ptr
 *
 *  If the argument is nonnull, allocates a copy of its pointee.
 */
template <typename T>
std::unique_ptr<T> copy_through_unique_ptr(const std::unique_ptr<T>& t) {
  static_assert(
      !std::is_polymorphic<T>::value || std::is_final<T>::value,
      "possibly slicing");
  return t ? std::make_unique<T>(*t) : nullptr;
}

/**
 *  copy_through_shared_ptr
 *
 *  If the argument is nonnull, allocates a copy of its pointee.
 */
template <typename T>
std::shared_ptr<T> copy_through_shared_ptr(const std::shared_ptr<T>& t) {
  static_assert(
      !std::is_polymorphic<T>::value || std::is_final<T>::value,
      "possibly slicing");
  return t ? std::make_shared<T>(*t) : nullptr;
}

//  erased_unique_ptr
//
//  A type-erased smart-ptr with unique ownership to a heap-allocated object.
using erased_unique_ptr = std::unique_ptr<void, void (*)(void*)>;

namespace detail {
// for erased_unique_ptr with types that specialize default_delete
template <typename T>
void erased_unique_ptr_delete(void* ptr) {
  std::default_delete<T>()(static_cast<T*>(ptr));
}
} // namespace detail

//  to_erased_unique_ptr
//
//  Converts an owning pointer to an object to an erased_unique_ptr.
template <typename T>
erased_unique_ptr to_erased_unique_ptr(T* const ptr) noexcept {
  return {ptr, detail::erased_unique_ptr_delete<T>};
}

//  to_erased_unique_ptr
//
//  Converts an owning std::unique_ptr to an erased_unique_ptr.
template <typename T>
erased_unique_ptr to_erased_unique_ptr(std::unique_ptr<T> ptr) noexcept {
  return to_erased_unique_ptr(ptr.release());
}

//  make_erased_unique
//
//  Allocate an object of the T on the heap, constructed with a..., and return
//  an owning erased_unique_ptr to it.
template <typename T, typename... A>
erased_unique_ptr make_erased_unique(A&&... a) {
  return to_erased_unique_ptr(std::make_unique<T>(static_cast<A&&>(a)...));
}

//  copy_to_erased_unique_ptr
//
//  Copy an object to the heap and return an owning erased_unique_ptr to it.
template <typename T>
erased_unique_ptr copy_to_erased_unique_ptr(T&& obj) {
  return to_erased_unique_ptr(copy_to_unique_ptr(static_cast<T&&>(obj)));
}

//  empty_erased_unique_ptr
//
//  Return an empty erased_unique_ptr.
inline erased_unique_ptr empty_erased_unique_ptr() {
  return {nullptr, nullptr};
}

/**
 * SysAllocator
 *
 * Resembles std::allocator, the default Allocator, but wraps std::malloc and
 * std::free.
 */
template <typename T>
class SysAllocator {
 private:
  using Self = SysAllocator<T>;

 public:
  using value_type = T;

  constexpr SysAllocator() = default;

  constexpr SysAllocator(SysAllocator const&) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  constexpr SysAllocator(SysAllocator<U> const&) noexcept {}

  T* allocate(size_t count) {
    auto const p = std::malloc(sizeof(T) * count);
    if (!p) {
      throw_exception<std::bad_alloc>();
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t count) { sizedFree(p, count * sizeof(T)); }

  friend bool operator==(Self const&, Self const&) noexcept { return true; }
  friend bool operator!=(Self const&, Self const&) noexcept { return false; }
};

class DefaultAlign {
 private:
  using Self = DefaultAlign;
  std::size_t align_;

 public:
  explicit DefaultAlign(std::size_t align) noexcept : align_(align) {
    assert(!(align_ < sizeof(void*)) && bool("bad align: too small"));
    assert(!(align_ & (align_ - 1)) && bool("bad align: not power-of-two"));
  }
  std::size_t operator()() const noexcept { return align_; }

  friend bool operator==(Self const& a, Self const& b) noexcept {
    return a.align_ == b.align_;
  }
  friend bool operator!=(Self const& a, Self const& b) noexcept {
    return a.align_ != b.align_;
  }
};

template <std::size_t Align>
class FixedAlign {
 private:
  static_assert(!(Align < sizeof(void*)), "bad align: too small");
  static_assert(!(Align & (Align - 1)), "bad align: not power-of-two");
  using Self = FixedAlign<Align>;

 public:
  constexpr std::size_t operator()() const noexcept { return Align; }

  friend bool operator==(Self const&, Self const&) noexcept { return true; }
  friend bool operator!=(Self const&, Self const&) noexcept { return false; }
};

/**
 * AlignedSysAllocator
 *
 * Resembles std::allocator, the default Allocator, but wraps aligned_malloc and
 * aligned_free.
 *
 * Accepts a policy parameter for providing the alignment, which must:
 *   * be invocable as std::size_t(std::size_t) noexcept
 *     * taking the type alignment and returning the allocation alignment
 *   * be noexcept-copy-constructible
 *   * have noexcept operator==
 *   * have noexcept operator!=
 *   * not be final
 *
 * DefaultAlign and FixedAlign<std::size_t>, provided above, are valid policies.
 */
template <typename T, typename Align = DefaultAlign>
class AlignedSysAllocator : private Align {
 private:
  using Self = AlignedSysAllocator<T, Align>;

  template <typename, typename>
  friend class AlignedSysAllocator;

  constexpr Align const& align() const { return *this; }

 public:
  static_assert(std::is_nothrow_copy_constructible<Align>::value);
  static_assert(is_nothrow_invocable_r_v<std::size_t, Align>);

  using value_type = T;

  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  using Align::Align;

  // TODO: remove this ctor, which is is no longer required as of under gcc7
  template <
      typename S = Align,
      std::enable_if_t<std::is_default_constructible<S>::value, int> = 0>
  constexpr AlignedSysAllocator() noexcept(noexcept(Align())) : Align() {}

  constexpr AlignedSysAllocator(AlignedSysAllocator const&) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  constexpr AlignedSysAllocator(
      AlignedSysAllocator<U, Align> const& other) noexcept
      : Align(other.align()) {}

  T* allocate(size_t count) {
    auto const a = align()() < alignof(T) ? alignof(T) : align()();
    auto const p = aligned_malloc(sizeof(T) * count, a);
    if (!p) {
      if (FOLLY_UNLIKELY(errno != ENOMEM)) {
        std::terminate();
      }
      throw_exception<std::bad_alloc>();
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t /* count */) { aligned_free(p); }

  friend bool operator==(Self const& a, Self const& b) noexcept {
    return a.align() == b.align();
  }
  friend bool operator!=(Self const& a, Self const& b) noexcept {
    return a.align() != b.align();
  }
};

/**
 * CxxAllocatorAdaptor
 *
 * A type conforming to C++ concept Allocator, delegating operations to an
 * unowned Inner which has this required interface:
 *
 *   void* allocate(std::size_t)
 *   void deallocate(void*, std::size_t)
 *
 * Note that Inner is *not* a C++ Allocator.
 */
template <typename T, class Inner, bool FallbackToStdAlloc = false>
class CxxAllocatorAdaptor : private std::allocator<T> {
 private:
  using Self = CxxAllocatorAdaptor<T, Inner, FallbackToStdAlloc>;

  template <typename U, typename UInner, bool UFallback>
  friend class CxxAllocatorAdaptor;

  Inner* inner_ = nullptr;

 public:
  using value_type = T;

  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  template <bool X = FallbackToStdAlloc, std::enable_if_t<X, int> = 0>
  constexpr explicit CxxAllocatorAdaptor() {}

  constexpr explicit CxxAllocatorAdaptor(Inner& ref) : inner_(&ref) {}

  constexpr CxxAllocatorAdaptor(CxxAllocatorAdaptor const&) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  constexpr CxxAllocatorAdaptor(
      CxxAllocatorAdaptor<U, Inner, FallbackToStdAlloc> const& other)
      : inner_(other.inner_) {}

  CxxAllocatorAdaptor& operator=(CxxAllocatorAdaptor const& other) = default;

  template <typename U, std::enable_if_t<!std::is_same<U, T>::value, int> = 0>
  CxxAllocatorAdaptor& operator=(
      CxxAllocatorAdaptor<U, Inner, FallbackToStdAlloc> const& other) noexcept {
    inner_ = other.inner_;
    return *this;
  }

  T* allocate(std::size_t n) {
    if (FallbackToStdAlloc && inner_ == nullptr) {
      return std::allocator<T>::allocate(n);
    }
    return static_cast<T*>(inner_->allocate(sizeof(T) * n));
  }

  void deallocate(T* p, std::size_t n) {
    if (inner_ != nullptr) {
      inner_->deallocate(p, sizeof(T) * n);
    } else {
      assert(FallbackToStdAlloc);
      std::allocator<T>::deallocate(p, n);
    }
  }

  friend bool operator==(Self const& a, Self const& b) noexcept {
    return a.inner_ == b.inner_;
  }
  friend bool operator!=(Self const& a, Self const& b) noexcept {
    return a.inner_ != b.inner_;
  }

  template <typename U>
  struct rebind {
    using other = CxxAllocatorAdaptor<U, Inner, FallbackToStdAlloc>;
  };
};

/*
 * allocator_delete
 *
 * A deleter which automatically works with a given allocator.
 *
 * Derives from the allocator to take advantage of the empty base
 * optimization when possible.
 */
template <typename Alloc>
class allocator_delete : private std::remove_reference<Alloc>::type {
 private:
  using allocator_type = typename std::remove_reference<Alloc>::type;
  using allocator_traits = std::allocator_traits<allocator_type>;
  using value_type = typename allocator_traits::value_type;
  using pointer = typename allocator_traits::pointer;

 public:
  allocator_delete() = default;
  allocator_delete(allocator_delete const&) = default;
  allocator_delete(allocator_delete&&) = default;
  allocator_delete& operator=(allocator_delete const&) = default;
  allocator_delete& operator=(allocator_delete&&) = default;

  explicit allocator_delete(const allocator_type& alloc)
      : allocator_type(alloc) {}

  explicit allocator_delete(allocator_type&& alloc)
      : allocator_type(std::move(alloc)) {}

  template <typename U>
  allocator_delete(const allocator_delete<U>& other)
      : allocator_type(other.get_allocator()) {}

  allocator_type const& get_allocator() const { return *this; }

  void operator()(pointer p) const {
    auto alloc = get_allocator();
    allocator_traits::destroy(alloc, p);
    allocator_traits::deallocate(alloc, p, 1);
  }
};

/**
 * allocate_unique, like std::allocate_shared but for std::unique_ptr
 */
template <typename T, typename Alloc, typename... Args>
std::unique_ptr<
    T,
    allocator_delete<
        typename std::allocator_traits<Alloc>::template rebind_alloc<T>>>
allocate_unique(Alloc const& alloc, Args&&... args) {
  using TAlloc =
      typename std::allocator_traits<Alloc>::template rebind_alloc<T>;

  using traits = std::allocator_traits<TAlloc>;
  struct DeferCondDeallocate {
    bool& cond;
    TAlloc& copy;
    T* p;
    ~DeferCondDeallocate() {
      if (FOLLY_UNLIKELY(!cond)) {
        traits::deallocate(copy, p, 1);
      }
    }
  };
  auto copy = TAlloc(alloc);
  auto const p = traits::allocate(copy, 1);
  {
    bool constructed = false;
    DeferCondDeallocate handler{constructed, copy, p};
    traits::construct(copy, p, static_cast<Args&&>(args)...);
    constructed = true;
  }
  return {p, allocator_delete<TAlloc>(std::move(copy))};
}

struct SysBufferDeleter {
  void operator()(void* ptr) { std::free(ptr); }
};
using SysBufferUniquePtr = std::unique_ptr<void, SysBufferDeleter>;

inline SysBufferUniquePtr allocate_sys_buffer(std::size_t size) {
  auto p = std::malloc(size);
  if (!p) {
    throw_exception<std::bad_alloc>();
  }
  return {p, {}};
}

/**
 * AllocatorHasTrivialDeallocate
 *
 * Unambiguously inherits std::integral_constant<bool, V> for some bool V.
 *
 * Describes whether a C++ Aallocator has trivial, i.e. no-op, deallocate().
 *
 * Also may be used to describe types which may be used with
 * CxxAllocatorAdaptor.
 */
template <typename Alloc>
struct AllocatorHasTrivialDeallocate : std::false_type {};

template <typename T, class Alloc>
struct AllocatorHasTrivialDeallocate<CxxAllocatorAdaptor<T, Alloc>>
    : AllocatorHasTrivialDeallocate<Alloc> {};

namespace detail {
// note that construct and destroy here are methods, not short names for
// the constructor and destructor
FOLLY_CREATE_MEMBER_INVOKER(AllocatorConstruct_, construct);
FOLLY_CREATE_MEMBER_INVOKER(AllocatorDestroy_, destroy);

template <typename Void, typename Alloc, typename... Args>
struct AllocatorCustomizesConstruct_
    : folly::is_invocable<AllocatorConstruct_, Alloc, Args...> {};

template <typename Alloc, typename... Args>
struct AllocatorCustomizesConstruct_<
    void_t<typename Alloc::folly_has_default_object_construct>,
    Alloc,
    Args...> : Negation<typename Alloc::folly_has_default_object_construct> {};

template <typename Void, typename Alloc, typename... Args>
struct AllocatorCustomizesDestroy_
    : folly::is_invocable<AllocatorDestroy_, Alloc, Args...> {};

template <typename Alloc, typename... Args>
struct AllocatorCustomizesDestroy_<
    void_t<typename Alloc::folly_has_default_object_destroy>,
    Alloc,
    Args...> : Negation<typename Alloc::folly_has_default_object_destroy> {};
} // namespace detail

/**
 * AllocatorHasDefaultObjectConstruct
 *
 * AllocatorHasDefaultObjectConstruct<A, T, Args...> unambiguously
 * inherits std::integral_constant<bool, V>, where V will be true iff
 * the effect of std::allocator_traits<A>::construct(a, p, args...) is
 * the same as new (static_cast<void*>(p)) T(args...).  If true then
 * any optimizations applicable to object construction (relying on
 * std::is_trivially_copyable<T>, for example) can be applied to objects
 * in an allocator-aware container using an allocation of type A.
 *
 * Allocator types can override V by declaring a type alias for
 * folly_has_default_object_construct.  It is helpful to do this if you
 * define a custom allocator type that defines a construct method, but
 * that method doesn't do anything except call placement new.
 */
template <typename Alloc, typename T, typename... Args>
struct AllocatorHasDefaultObjectConstruct
    : Negation<
          detail::AllocatorCustomizesConstruct_<void, Alloc, T*, Args...>> {};

template <typename Value, typename T, typename... Args>
struct AllocatorHasDefaultObjectConstruct<std::allocator<Value>, T, Args...>
    : std::true_type {};

/**
 * AllocatorHasDefaultObjectDestroy
 *
 * AllocatorHasDefaultObjectDestroy<A, T> unambiguously inherits
 * std::integral_constant<bool, V>, where V will be true iff the effect
 * of std::allocator_traits<A>::destroy(a, p) is the same as p->~T().
 * If true then optimizations applicable to object destruction (relying
 * on std::is_trivially_destructible<T>, for example) can be applied to
 * objects in an allocator-aware container using an allocator of type A.
 *
 * Allocator types can override V by declaring a type alias for
 * folly_has_default_object_destroy.  It is helpful to do this if you
 * define a custom allocator type that defines a destroy method, but that
 * method doesn't do anything except call the object's destructor.
 */
template <typename Alloc, typename T>
struct AllocatorHasDefaultObjectDestroy
    : Negation<detail::AllocatorCustomizesDestroy_<void, Alloc, T*>> {};

template <typename Value, typename T>
struct AllocatorHasDefaultObjectDestroy<std::allocator<Value>, T>
    : std::true_type {};

} // namespace folly
