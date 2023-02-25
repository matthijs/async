//
// Copyright (c) 2022 Klemens Morgenstern (klemens.morgenstern@gmx.net)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASYNC_DETAIL_SELECT_HPP
#define BOOST_ASYNC_DETAIL_SELECT_HPP

#include <boost/async/detail/await_result_helper.hpp>
#include <boost/async/detail/forward_cancellation.hpp>
#include <boost/async/this_thread.hpp>
#include <boost/async/detail/util.hpp>

#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/container/pmr/monotonic_buffer_resource.hpp>
#include <boost/container/pmr/vector.hpp>
#include <boost/exception/info.hpp>
#include <boost/exception/error_info.hpp>
#include <boost/variant2/variant.hpp>

#include <coroutine>
#include <optional>


namespace boost::async::detail
{

typedef boost::error_info<struct select_index_tag, std::size_t> select_index;


template<asio::cancellation_type Ct, typename ... Args>
struct select_impl
{
  template<typename Arg>
  using result_helper = std::conditional_t<std::is_void_v<Arg>, variant2::monostate, Arg>;
  using result_type = variant2::variant<result_helper<co_await_result_t<Args>>...>;

  template<std::size_t ... Idx>
  auto get_awaitables_(std::index_sequence<Idx...>)
  {
    return std::make_tuple(detail::get_awaitable_type(std::get<Idx>(args))...);
  }

  select_impl(Args && ... p) : args(std::forward<Args>(p)...),
                               awaitables(get_awaitables_(std::make_index_sequence<sizeof...(Args)>{}))
  {
    std::fill(cancel.begin(), cancel.end(), nullptr);
  }
  select_impl(select_impl && lhs) : args(std::move(lhs.args))
                                  , awaitables(get_awaitables_(std::make_index_sequence<sizeof...(Args)>{}))
  {
    std::fill(cancel.begin(), cancel.end(), nullptr);
  }

  template<std::size_t ... Idx>
  bool await_ready_impl(std::index_sequence<Idx...>)
  {
    return ((
        [this]
        {
          auto & r = std::get<Idx>(awaitables);
          if (r.await_ready())
          {
            if constexpr (std::is_void_v<decltype(r.await_resume())>)
            {
              r.await_resume();
              this->result.emplace(variant2::in_place_index<Idx>);
            }
            else
              this->result.emplace(variant2::in_place_index<Idx>, r.await_resume());
            return true;
          }
          return false;
        }()) || ... );
  };

  bool await_ready()
  {
    return await_ready_impl(std::make_index_sequence<sizeof...(Args)>{});
  }

  template<std::size_t Idx>
  struct stub
  {
    struct promise_type
    {
      select_impl & ref;
      using cancellation_slot_type = asio::cancellation_slot;

      asio::cancellation_signal cancel;
      cancellation_slot_type get_cancellation_slot()
      {
        return cancel.slot();
      }

      void* operator new(std::size_t n, select_impl & ref, std::size_t )
      {
        return ref.memory_resource.allocate(n);
      }

      void operator delete(void *, std::size_t) {}

      promise_type(select_impl & ref, std::integral_constant<std::size_t, Idx> idx) : ref(ref)
      {
        ref.cancel[idx] = &cancel;
      }

      constexpr static std::suspend_never initial_suspend() noexcept { return {}; }
      auto final_suspend() noexcept
      {
        ref.cancel[Idx] = nullptr;

        struct final_awaitable
        {
          constexpr static bool await_ready() noexcept {return false;}

          std::coroutine_handle<void> await_suspend(
              std::coroutine_handle<promise_type> h) noexcept
          {
            auto & rf = h.promise().ref;
            std::coroutine_handle<void> res = std::noop_coroutine();

            constexpr std::array<bool, sizeof...(Args)> lval_ref{std::is_lvalue_reference_v<Args>...};
            std::size_t idx = 0u;
            for (auto & r : rf.cancel)
            {
              auto i = idx ++;
              if (r)
              {
                if (lval_ref[i])
                {
                  if (r)
                    r->emit(interrupt_await);
                  if (!r)
                    continue;
                }
                std::exchange(r, nullptr)->emit(Ct);
              }
            }
            h.destroy();
            if (--rf.outstanding == 0u)
              res = std::coroutine_handle<void>::from_address(rf.awaited_from.release());

            return res;
          }
          void await_resume() noexcept {}
        };
        return final_awaitable{};
      }

      void unhandled_exception()
      {
        if (!ref.exception && !ref.result)
          ref.exception = std::current_exception();
      }

      template<typename T>
      void return_value(T && t)
      {
        if (!ref.result)
          ref.result.emplace(variant2::in_place_index<Idx>, std::forward<T>(t));

        ref.cancel[Idx] = nullptr;

      }

      stub get_return_object() {return {};}
    };

  };

  template<std::size_t Idx>
  stub<Idx> await_suspend_impl_step(std::integral_constant<std::size_t, Idx>)
  try {
    if constexpr (std::is_same_v<variant2::variant_alternative_t<Idx, result_type>, variant2::monostate>)
    {
      co_await std::move(std::get<Idx>(awaitables));
      co_return variant2::monostate{};
    }
    else
      co_return co_await std::move(std::get<Idx>(awaitables));
  }
  catch (boost::exception & e)
  {
    e << select_index(Idx);
    throw;
  }

  template<std::size_t ... Idx>
  void await_suspend_impl(std::index_sequence<Idx...>)
  {
    (await_suspend_impl_step(std::integral_constant<std::size_t, Idx>{}), ...);
  }

  template<typename Promise>
  void await_suspend(std::coroutine_handle<Promise> h)
  {

    auto sl = asio::get_associated_cancellation_slot(h);
    if (sl.is_connected())
    {
      struct do_cancel
      {
        std::array<asio::cancellation_signal*, sizeof...(Args)> &cancel;
        do_cancel(std::array<asio::cancellation_signal*, sizeof...(Args)> &cancel) : cancel(cancel) {}
        void operator()(asio::cancellation_type ct)
        {
          for (auto & cs : cancel)
            if (cs) cs->emit(ct);
        }
      };

      sl.template emplace<do_cancel>(cancel);
    }
    awaited_from.reset(h.address());
    await_suspend_impl(std::make_index_sequence<sizeof...(Args)>{});
  }

  auto await_resume()
  {
    if (cancellation_slot.is_connected())
        cancellation_slot.clear();

    if (exception)
      std::rethrow_exception(exception);

    return std::move(result.value());
  }

 private:
  std::tuple<Args...> args;
  std::tuple<detail::co_awaitable_type<Args>...> awaitables;

  std::size_t outstanding{sizeof...(Args)};
  std::array<asio::cancellation_signal*, sizeof...(Args)> cancel;
  asio::cancellation_slot cancellation_slot;
  std::unique_ptr<void, detail::coro_deleter<void>> awaited_from;

  std::optional<result_type> result;
  std::exception_ptr exception;

  char memory_buffer[((1200 + sizeof(co_await_result_t<Args>)) + ...)];
  container::pmr::monotonic_buffer_resource memory_resource{memory_buffer, sizeof(memory_buffer)};
};


template<asio::cancellation_type Ct, typename PromiseRange>
struct ranged_select_impl
{
  using awaitable_type = detail::co_awaitable_type<std::decay_t<decltype(*std::begin(std::declval<PromiseRange>()))>>;
  using inner_result_type = co_await_result_t<std::decay_t<decltype(*std::begin(std::declval<PromiseRange>()))>>;

  using result_type = std::conditional_t<std::is_void_v<inner_result_type>,
                                         std::size_t, std::pair<std::size_t, inner_result_type>>;

  ranged_select_impl(PromiseRange && p) : range(static_cast<PromiseRange&&>(p))
  {
    awaitables.reserve(std::size(range));
    if constexpr (std::is_rvalue_reference_v<PromiseRange&&>)
      for (auto && v : range)
        awaitables.push_back(get_awaitable_type(std::move(v)));
    else
      for (auto && v : range)
        awaitables.push_back(get_awaitable_type(v));
    std::fill(cancel.begin(), cancel.end(), nullptr);
  }
  ranged_select_impl(ranged_select_impl && lhs) : ranged_select_impl(static_cast<PromiseRange&&>(lhs.range))
  {
  }
  bool await_ready()
  {

    std::size_t idx = 0;
    for (auto & r : awaitables)
    {
      if (r.await_ready())
      {
        if constexpr (std::is_void_v<inner_result_type>)
        {
          r.await_resume();
          this->result.emplace(idx);
        }
        else
          this->result.emplace(idx, r.await_resume());
        return true;
      }
      idx++;
    }
    return false;
  };

  struct stub
  {
    struct promise_type
    {
      ranged_select_impl & ref;
      std::size_t idx{0u};
      using cancellation_slot_type = asio::cancellation_slot;

      asio::cancellation_signal cancel;
      cancellation_slot_type get_cancellation_slot()
      {
        return cancel.slot();
      }

      void* operator new(std::size_t n, ranged_select_impl & ref, std::size_t )
      {
        return ref.memory_resource.allocate(n);
      }

      void operator delete(void *, std::size_t) {}

      promise_type(ranged_select_impl & ref, std::size_t idx) : ref(ref), idx(idx)
      {
        ref.cancel[idx] = &cancel;
      }

      constexpr static std::suspend_never initial_suspend() noexcept { return {}; }
      auto final_suspend() noexcept
      {
        ref.cancel[idx] = nullptr;

        struct final_awaitable
        {
          constexpr static bool await_ready() noexcept {return false;}

          std::coroutine_handle<void> await_suspend(
              std::coroutine_handle<promise_type> h) noexcept
          {
            auto & rf = h.promise().ref;
            std::coroutine_handle<void> res = std::noop_coroutine();

            if constexpr(std::is_lvalue_reference_v<PromiseRange>)
              for (auto & r : rf.cancel)
                if (r)
                  r->emit(interrupt_await);

            for (auto & r : rf.cancel)
              if (r)
                std::exchange(r, nullptr)->emit(Ct);

            h.destroy();
            if (--rf.outstanding == 0u)
              res = std::coroutine_handle<void>::from_address(rf.awaited_from.release());

            return res;
          }
          void await_resume() noexcept {}
        };
        return final_awaitable{};
      }

      void unhandled_exception()
      {
        if (!ref.exception && !ref.result)
          ref.exception = std::current_exception();
      }

      template<typename T>
      void return_value(T && t)
      {
        if (!ref.result)
        {
          if constexpr (std::is_same_v<void, inner_result_type>)
            ref.result.emplace(idx);
          else
            ref.result.emplace(idx, std::forward<T>(t));
        }

        ref.cancel[idx] = nullptr;

      }

      stub get_return_object() {return {};}
    };

  };

  stub await_suspend_impl(std::size_t idx)
  try {
    if constexpr (std::is_same_v<void, inner_result_type>)
    {
      co_await std::move(awaitables[idx]);
      co_return variant2::monostate{};
    }
    else
      co_return co_await std::move(*std::next(range.begin(), idx));
  }
  catch (boost::exception & e)
  {
    e << select_index(idx);
    throw;
  }

  template<typename Promise>
  void await_suspend(std::coroutine_handle<Promise> h)
  {
    auto sl = asio::get_associated_cancellation_slot(h);
    if (sl.is_connected())
    {
      struct do_cancel
      {
        container::pmr::vector<asio::cancellation_signal*> &cancel;
        do_cancel(container::pmr::vector<asio::cancellation_signal*> &cancel) : cancel(cancel) {}
        void operator()(asio::cancellation_type ct)
        {
          for (auto & cs : cancel)
            if (cs) cs->emit(ct);
        }
      };

      sl.template emplace<do_cancel>(cancel);
    }
    awaited_from.reset(h.address());
    for (std::size_t i = 0u; i < std::size(range); i++)
      await_suspend_impl(i);
  }

  result_type await_resume()
  {
    if (cancellation_slot.is_connected())
      cancellation_slot.clear();

    if (exception)
      std::rethrow_exception(exception);
    if constexpr (!std::is_void_v<result_type>)
      return std::move(result.value());
  }

 private:
  PromiseRange range;
  std::size_t outstanding{std::size(range)};

  container::pmr::monotonic_buffer_resource memory_resource{((1200 + sizeof(result_type) + sizeof(awaitable_type)) * std::size(range)),
                                                            this_thread::get_default_resource()};
  container::pmr::vector<awaitable_type> awaitables{container::pmr::polymorphic_allocator<asio::cancellation_signal*>(&memory_resource)};
  container::pmr::vector<asio::cancellation_signal*> cancel{std::size(range),
                                                            nullptr,
                                                            awaitables.get_allocator()};

  asio::cancellation_slot cancellation_slot;
  std::unique_ptr<void, detail::coro_deleter<void>> awaited_from;

  std::optional<result_type> result;
  std::exception_ptr exception;
};



}

#endif //BOOST_ASYNC_DETAIL_SELECT_HPP
