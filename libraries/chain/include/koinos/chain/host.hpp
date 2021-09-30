#pragma once

#include <type_traits>

#include <koinos/chain/apply_context.hpp>
#include <koinos/chain/types.hpp>
#include <koinos/chain/wasm/common.hpp>

namespace koinos::chain {

struct host_api final
{
   host_api( apply_context& ctx );
   apply_context& context;

   void invoke_thunk(
      uint32_t tid,
      array_ptr< char > ret_ptr,
      uint32_t ret_len,
      array_ptr< const char > arg_ptr,
      uint32_t arg_len );

   void invoke_system_call(
      uint32_t sid,
      array_ptr< char > ret_ptr,
      uint32_t ret_len,
      array_ptr< const char > arg_ptr,
      uint32_t arg_len );
};

inline void register_host_functions()
{
   registrar_type::add< host_api, &host_api::invoke_thunk, wasm_allocator_type >( "env", "invoke_thunk" );
   registrar_type::add< host_api, &host_api::invoke_system_call, wasm_allocator_type >( "env", "invoke_system_call" );
}

} // koinos::chain
