// Copyright 2015-2019 Hans Dembinski
// Copyright 2019 Glen Joseph Fernandes (glenjofe@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt
// or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_HISTOGRAM_UNLIMTED_STORAGE_HPP
#define BOOST_HISTOGRAM_UNLIMTED_STORAGE_HPP

#include <algorithm>
#include <boost/assert.hpp>
#include <boost/cstdint.hpp>
#include <boost/core/alloc_construct.hpp>
#include <boost/histogram/detail/iterator_adaptor.hpp>
#include <boost/histogram/detail/large_int.hpp>
#include <boost/histogram/detail/meta.hpp>
#include <boost/histogram/detail/safe_comparison.hpp>
#include <boost/histogram/detail/static_if.hpp>
#include <boost/histogram/fwd.hpp>
#include <boost/mp11/algorithm.hpp>
#include <boost/mp11/list.hpp>
#include <boost/mp11/utility.hpp>
#include <cmath>
#include <functional>
#include <iterator>
#include <memory>
#include <type_traits>

namespace boost {
namespace histogram {
namespace detail {

template <class T, class = std::enable_if_t<(std::is_arithmetic<T>::value ||
                                             is_large_int<T>::value)>>
struct requires_arithmetic {};

template <class L, class T>
using next_type = mp11::mp_at_c<L, (mp11::mp_find<L, T>::value + 1)>;

template<class Allocator>
class construct_guard {
public:
  using pointer = typename std::allocator_traits<Allocator>::pointer;

  construct_guard(Allocator& a, pointer p, std::size_t n) noexcept
    : a_(a), p_(p), n_(n) {}

  ~construct_guard() {
    if (p_) {
      a_.deallocate(p_, n_);
    }
  }

  void release() {
    p_ = pointer();
  }

  construct_guard(const construct_guard&) = delete;
  construct_guard& operator=(const construct_guard&) = delete;

private:
  Allocator& a_;
  pointer p_;
  std::size_t n_;
};

template <class Allocator>
void* buffer_create(Allocator& a, std::size_t n) {
  auto ptr = a.allocate(n); // may throw
  static_assert(std::is_trivially_copyable<decltype(ptr)>::value,
                "ptr must be trivially copyable");
  construct_guard<Allocator> guard(a, ptr, n);
  boost::alloc_construct_n(a, ptr, n);
  guard.release();
  return static_cast<void*>(ptr);
}

template <class Allocator, class Iterator>
auto buffer_create(Allocator& a, std::size_t n, Iterator iter) {
  BOOST_ASSERT(n > 0u);
  auto ptr = a.allocate(n); // may throw
  static_assert(std::is_trivially_copyable<decltype(ptr)>::value,
                "ptr must be trivially copyable");
  construct_guard<Allocator> guard(a, ptr, n);
  boost::alloc_construct_n(a, ptr, n, iter);
  guard.release();
  return ptr;
}

template <class Allocator>
void buffer_destroy(Allocator& a, typename std::allocator_traits<Allocator>::pointer p,
                    std::size_t n) {
  BOOST_ASSERT(p);
  BOOST_ASSERT(n > 0u);
  boost::alloc_destroy_n(a, p, n);
  a.deallocate(p, n);
}

} // namespace detail

/**
  Memory-efficient storage for integral counters which cannot overflow.

  This storage provides a no-overflow-guarantee if it is filled with integral weights
  only. This storage implementation keeps a contiguous array of elemental counters, one
  for each cell. If an operation is requested, which would overflow a counter, the whole
  array is replaced with another of a wider integral type, then the operation is executed.
  The storage uses integers of 8, 16, 32, 64 bits, and then switches to a multiprecision
  integral type, similar to those in
  [Boost.Multiprecision](https://www.boost.org/doc/libs/develop/libs/multiprecision/doc/html/index.html).

  A scaling operation or adding a floating point number turns the elements into doubles,
  which voids the no-overflow-guarantee.
*/
template <class Allocator>
class unlimited_storage {
  static_assert(
      std::is_same<typename std::allocator_traits<Allocator>::pointer,
                   typename std::allocator_traits<Allocator>::value_type*>::value,
      "unlimited_storage requires allocator with trivial pointer type");

public:
  static constexpr bool has_threading_support = false;

  using allocator_type = Allocator;
  using value_type = double;
  using large_int = detail::large_int<
      typename std::allocator_traits<allocator_type>::template rebind_alloc<uint64_t>>;

  struct buffer_type {
    // cannot be moved outside of scope of unlimited_storage, large_int is dependent type
    using types = mp11::mp_list<uint8_t, uint16_t, uint32_t, uint64_t, large_int, double>;

    template <class T>
    static constexpr unsigned type_index() noexcept {
      return static_cast<unsigned>(mp11::mp_find<types, T>::value);
    }

    template <class F, class... Ts>
    decltype(auto) visit(F&& f, Ts&&... ts) const {
      // this is intentionally not a switch, the if-chain is faster in benchmarks
      if (type == type_index<uint8_t>())
        return f(static_cast<uint8_t*>(ptr), std::forward<Ts>(ts)...);
      if (type == type_index<uint16_t>())
        return f(static_cast<uint16_t*>(ptr), std::forward<Ts>(ts)...);
      if (type == type_index<uint32_t>())
        return f(static_cast<uint32_t*>(ptr), std::forward<Ts>(ts)...);
      if (type == type_index<uint64_t>())
        return f(static_cast<uint64_t*>(ptr), std::forward<Ts>(ts)...);
      if (type == type_index<large_int>())
        return f(static_cast<large_int*>(ptr), std::forward<Ts>(ts)...);
      return f(static_cast<double*>(ptr), std::forward<Ts>(ts)...);
    }

    buffer_type(const allocator_type& a = {}) : alloc(a) {}

    buffer_type(buffer_type&& o) noexcept
        : alloc(std::move(o.alloc)), size(o.size), type(o.type), ptr(o.ptr) {
      o.size = 0;
      o.type = 0;
      o.ptr = nullptr;
    }

    buffer_type& operator=(buffer_type&& o) noexcept {
      if (this != &o) {
        using std::swap;
        swap(alloc, o.alloc);
        swap(size, o.size);
        swap(type, o.type);
        swap(ptr, o.ptr);
      }
      return *this;
    }

    buffer_type(const buffer_type& x) : alloc(x.alloc) {
      x.visit([this, n = x.size](const auto* xp) {
        using T = detail::remove_cvref_t<decltype(*xp)>;
        this->template make<T>(n, xp);
      });
    }

    buffer_type& operator=(const buffer_type& o) {
      *this = buffer_type(o);
      return *this;
    }

    ~buffer_type() noexcept { destroy(); }

    void destroy() noexcept {
      BOOST_ASSERT((ptr == nullptr) == (size == 0));
      if (ptr == nullptr) return;
      visit([this](auto* p) {
        using T = detail::remove_cvref_t<decltype(*p)>;
        using alloc_type =
            typename std::allocator_traits<allocator_type>::template rebind_alloc<T>;
        alloc_type a(alloc); // rebind allocator
        detail::buffer_destroy(a, p, this->size);
      });
      size = 0;
      type = 0;
      ptr = nullptr;
    }

    template <class T>
    void make(std::size_t n) {
      // note: order of commands is to not leave buffer in invalid state upon throw
      destroy();
      if (n > 0) {
        // rebind allocator
        using alloc_type =
            typename std::allocator_traits<allocator_type>::template rebind_alloc<T>;
        alloc_type a(alloc);
        ptr = detail::buffer_create(a, n); // may throw
      }
      size = n;
      type = type_index<T>();
    }

    template <class T, class U>
    void make(std::size_t n, U iter) {
      // note: iter may be current ptr, so create new buffer before deleting old buffer
      void* new_ptr = nullptr;
      const auto new_type = type_index<T>();
      if (n > 0) {
        // rebind allocator
        using alloc_type =
            typename std::allocator_traits<allocator_type>::template rebind_alloc<T>;
        alloc_type a(alloc);
        new_ptr = detail::buffer_create(a, n, iter); // may throw
      }
      destroy();
      size = n;
      type = new_type;
      ptr = new_ptr;
    }

    allocator_type alloc;
    std::size_t size = 0;
    unsigned type = 0;
    mutable void* ptr = nullptr;
  };

  class reference; // forward declare to make friend of const_reference

  /// implementation detail
  class const_reference {
  public:
    const_reference(buffer_type& b, std::size_t i) : bref_(b), idx_(i) {
      BOOST_ASSERT(idx_ < bref_.size);
    }

    const_reference(const const_reference&) = default;

    // references do not rebind
    const_reference& operator=(const const_reference&) = delete;
    const_reference& operator=(const_reference&&) = delete;

    operator double() const {
      return bref_.visit(
          [this](const auto* p) { return static_cast<double>(p[this->idx_]); });
    }

    // minimal operators for partial ordering
    bool operator<(const const_reference& rhs) const { return op<detail::less>(rhs); }
    bool operator>(const const_reference& rhs) const { return op<detail::greater>(rhs); }
    bool operator==(const const_reference& rhs) const { return op<detail::equal>(rhs); }

    // adapted copy from boost/operators.hpp for partial ordering
    friend bool operator<=(const const_reference& x, const const_reference& y) {
      return !(y < x);
    }
    friend bool operator>=(const const_reference& x, const const_reference& y) {
      return !(y > x);
    }
    friend bool operator!=(const const_reference& y, const const_reference& x) {
      return !(x == y);
    }

    template <class U, class = detail::requires_arithmetic<U>>
    bool operator<(const U& rhs) const {
      return op<detail::less>(rhs);
    }

    template <class U, class = detail::requires_arithmetic<U>>
    bool operator>(const U& rhs) const {
      return op<detail::greater>(rhs);
    }

    template <class U, class = detail::requires_arithmetic<U>>
    bool operator==(const U& rhs) const {
      return op<detail::equal>(rhs);
    }

    // adapted copy from boost/operators.hpp
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator<=(const const_reference& x, const U& y) {
      if (detail::is_unsigned_integral<U>::value) return !(x > y);
      return (x < y) || (x == y);
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator>=(const const_reference& x, const U& y) {
      if (detail::is_unsigned_integral<U>::value) return !(x < y);
      return (x > y) || (x == y);
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator>(const U& x, const const_reference& y) {
      return y < x;
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator<(const U& x, const const_reference& y) {
      return y > x;
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator<=(const U& x, const const_reference& y) {
      if (detail::is_unsigned_integral<U>::value) return !(y < x);
      return (y > x) || (y == x);
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator>=(const U& x, const const_reference& y) {
      if (detail::is_unsigned_integral<U>::value) return !(y > x);
      return (y < x) || (y == x);
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator==(const U& y, const const_reference& x) {
      return x == y;
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator!=(const U& y, const const_reference& x) {
      return !(x == y);
    }
    template <class U, class = detail::requires_arithmetic<U>>
    friend bool operator!=(const const_reference& y, const U& x) {
      return !(y == x);
    }

  private:
    template <class Binary>
    bool op(const const_reference& x) const {
      return x.bref_.visit(
          [this, ix = x.idx_](const auto* xp) { return this->op<Binary>(xp[ix]); });
    }

    template <class Binary, class U>
    bool op(const U& x) const {
      return bref_.visit([i = idx_, &x](const auto* p) { return Binary()(p[i], x); });
    }

  protected:
    buffer_type& bref_;
    std::size_t idx_;
    friend class reference;
  };

  /// implementation detail
  class reference : public const_reference {
  public:
    reference(buffer_type& b, std::size_t i) : const_reference(b, i) {}

    // references do copy-construct
    reference(const reference& x) = default;

    // references do not rebind, assign through
    reference& operator=(const reference& x) {
      return operator=(static_cast<const_reference>(x));
    }

    reference& operator=(const const_reference& x) {
      x.bref_.visit([this, ix = x.idx_](const auto* xp) {
        auto xi = xp[ix]; // convert reference to value
        this->operator=(xi);
      });
      return *this;
    }

    template <class U, class = detail::requires_arithmetic<U>>
    reference& operator=(const U& x) {
      this->bref_.visit([this, &x](auto* p) {
        p[this->idx_] = 0;
        adder()(p, this->bref_, this->idx_, x);
      });
      return *this;
    }

    reference& operator=(const large_int& x) {
      this->bref_.visit([this, &x](auto* p) {
        p[this->idx_] = 0;
        adder()(p, this->bref_, this->idx_, x);
      });
      return *this;
    }

    reference& operator+=(const const_reference& x) {
      x.bref_.visit([this, ix = x.idx_](const auto* xp) { this->operator+=(xp[ix]); });
      return *this;
    }

    template <class U, class = detail::requires_arithmetic<U>>
    reference& operator+=(const U& x) {
      this->bref_.visit(adder(), this->bref_, this->idx_, x);
      return *this;
    }

    reference& operator+=(const large_int& x) {
      this->bref_.visit(adder(), this->bref_, this->idx_, x);
      return *this;
    }

    reference& operator-=(const double x) { return operator+=(-x); }

    reference& operator*=(const double x) {
      this->bref_.visit(multiplier(), this->bref_, this->idx_, x);
      return *this;
    }

    reference& operator/=(const double x) { return operator*=(1.0 / x); }

    reference& operator++() {
      this->bref_.visit(incrementor(), this->bref_, this->idx_);
      return *this;
    }
  };

private:
  template <class Value, class Reference>
  class iterator_impl : public detail::iterator_adaptor<iterator_impl<Value, Reference>,
                                                        std::size_t, Reference, Value> {
  public:
    iterator_impl() = default;
    template <class V, class R>
    iterator_impl(const iterator_impl<V, R>& it)
        : iterator_impl::iterator_adaptor_(it.base()), buffer_(it.buffer_) {}
    iterator_impl(buffer_type* b, std::size_t i) noexcept
        : iterator_impl::iterator_adaptor_(i), buffer_(b) {}

    Reference operator*() const noexcept { return {*buffer_, this->base()}; }

    template <class V, class R>
    friend class iterator_impl;

  private:
    mutable buffer_type* buffer_ = nullptr;
  };

public:
  using const_iterator = iterator_impl<const value_type, const_reference>;
  using iterator = iterator_impl<value_type, reference>;

  explicit unlimited_storage(const allocator_type& a = {}) : buffer_(a) {}
  unlimited_storage(const unlimited_storage&) = default;
  unlimited_storage& operator=(const unlimited_storage&) = default;
  unlimited_storage(unlimited_storage&&) = default;
  unlimited_storage& operator=(unlimited_storage&&) = default;

  // TODO
  // template <class Allocator>
  // unlimited_storage(const unlimited_storage<Allocator>& s)

  template <class Iterable, class = detail::requires_iterable<Iterable>>
  explicit unlimited_storage(const Iterable& s) {
    using std::begin;
    using std::end;
    auto s_begin = begin(s);
    auto s_end = end(s);
    using V = typename std::iterator_traits<decltype(begin(s))>::value_type;
    constexpr auto ti = buffer_type::template type_index<V>();
    constexpr auto nt = mp11::mp_size<typename buffer_type::types>::value;
    const std::size_t size = static_cast<std::size_t>(std::distance(s_begin, s_end));
    detail::static_if_c<(ti < nt)>(
        [this, &size, &s_begin](auto) { buffer_.template make<V>(size, s_begin); },
        [this, &size, &s_begin](auto) { buffer_.template make<double>(size, s_begin); },
        0);
  }

  template <class Iterable, class = detail::requires_iterable<Iterable>>
  unlimited_storage& operator=(const Iterable& s) {
    *this = unlimited_storage(s);
    return *this;
  }

  allocator_type get_allocator() const { return buffer_.alloc; }

  void reset(std::size_t n) { buffer_.template make<uint8_t>(n); }

  std::size_t size() const noexcept { return buffer_.size; }

  reference operator[](std::size_t i) noexcept { return {buffer_, i}; }
  const_reference operator[](std::size_t i) const noexcept { return {buffer_, i}; }

  bool operator==(const unlimited_storage& x) const noexcept {
    if (size() != x.size()) return false;
    return buffer_.visit([&x](const auto* p) {
      return x.buffer_.visit([p, n = x.size()](const auto* xp) {
        return std::equal(p, p + n, xp, detail::equal{});
      });
    });
  }

  template <class Iterable>
  bool operator==(const Iterable& iterable) const {
    if (size() != iterable.size()) return false;
    return buffer_.visit([&iterable](const auto* p) {
      return std::equal(p, p + iterable.size(), std::begin(iterable), detail::equal{});
    });
  }

  unlimited_storage& operator*=(const double x) {
    buffer_.visit(multiplier(), buffer_, x);
    return *this;
  }

  iterator begin() noexcept { return {&buffer_, 0}; }
  iterator end() noexcept { return {&buffer_, size()}; }
  const_iterator begin() const noexcept { return {&buffer_, 0}; }
  const_iterator end() const noexcept { return {&buffer_, size()}; }

  /// @private used by unit tests, not part of generic storage interface
  template <class T>
  unlimited_storage(std::size_t s, const T* p, const allocator_type& a = {})
      : buffer_(std::move(a)) {
    buffer_.template make<T>(s, p);
  }

private:
  struct incrementor {
    template <class T>
    void operator()(T* tp, buffer_type& b, std::size_t i) {
      BOOST_ASSERT(tp && i < b.size);
      if (!detail::safe_increment(tp[i])) {
        using U = detail::next_type<typename buffer_type::types, T>;
        b.template make<U>(b.size, tp);
        ++static_cast<U*>(b.ptr)[i];
      }
    }

    void operator()(large_int* tp, buffer_type&, std::size_t i) { ++tp[i]; }

    void operator()(double* tp, buffer_type&, std::size_t i) { ++tp[i]; }
  };

  struct adder {
    template <class U>
    void operator()(double* tp, buffer_type&, std::size_t i, const U& x) {
      tp[i] += static_cast<double>(x);
    }

    void operator()(large_int* tp, buffer_type&, std::size_t i, const large_int& x) {
      tp[i] += x; // potentially adding large_int to itself is safe
    }

    template <class T, class U>
    void operator()(T* tp, buffer_type& b, std::size_t i, const U& x) {
      is_x_integral(std::is_integral<U>{}, tp, b, i, x);
    }

    template <class T, class U>
    void is_x_integral(std::false_type, T* tp, buffer_type& b, std::size_t i,
                       const U& x) {
      // x could be reference to buffer we manipulate, make copy before changing buffer
      const auto v = static_cast<double>(x);
      b.template make<double>(b.size, tp);
      operator()(static_cast<double*>(b.ptr), b, i, v);
    }

    template <class T>
    void is_x_integral(std::false_type, T* tp, buffer_type& b, std::size_t i,
                       const large_int& x) {
      // x could be reference to buffer we manipulate, make copy before changing buffer
      const auto v = static_cast<large_int>(x);
      b.template make<large_int>(b.size, tp);
      operator()(static_cast<large_int*>(b.ptr), b, i, v);
    }

    template <class T, class U>
    void is_x_integral(std::true_type, T* tp, buffer_type& b, std::size_t i, const U& x) {
      is_x_unsigned(std::is_unsigned<U>{}, tp, b, i, x);
    }

    template <class T, class U>
    void is_x_unsigned(std::false_type, T* tp, buffer_type& b, std::size_t i,
                       const U& x) {
      if (x >= 0)
        is_x_unsigned(std::true_type{}, tp, b, i, detail::make_unsigned(x));
      else
        is_x_integral(std::false_type{}, tp, b, i, static_cast<double>(x));
    }

    template <class T, class U>
    void is_x_unsigned(std::true_type, T* tp, buffer_type& b, std::size_t i, const U& x) {
      if (detail::safe_radd(tp[i], x)) return;
      // x could be reference to buffer we manipulate, need to convert to value
      const auto y = x;
      using TN = detail::next_type<typename buffer_type::types, T>;
      b.template make<TN>(b.size, tp);
      is_x_unsigned(std::true_type{}, static_cast<TN*>(b.ptr), b, i, y);
    }

    template <class U>
    void is_x_unsigned(std::true_type, large_int* tp, buffer_type&, std::size_t i,
                       const U& x) {
      tp[i] += x;
    }
  };

  struct multiplier {
    template <class T>
    void operator()(T* tp, buffer_type& b, const double x) {
      // potential lossy conversion that cannot be avoided
      b.template make<double>(b.size, tp);
      operator()(static_cast<double*>(b.ptr), b, x);
    }

    void operator()(double* tp, buffer_type& b, const double x) {
      for (auto end = tp + b.size; tp != end; ++tp) *tp *= x;
    }

    template <class T>
    void operator()(T* tp, buffer_type& b, std::size_t i, const double x) {
      b.template make<double>(b.size, tp);
      operator()(static_cast<double*>(b.ptr), b, i, x);
    }

    void operator()(double* tp, buffer_type&, std::size_t i, const double x) {
      tp[i] *= static_cast<double>(x);
    }
  };

  mutable buffer_type buffer_;
  friend struct unsafe_access;
};

} // namespace histogram
} // namespace boost

#endif
