
#include <koinos/chain/constants.hpp>
#include <koinos/chain/host_api.hpp>
#include <koinos/chain/thunk_dispatcher.hpp>
#include <koinos/chain/state.hpp>
#include <koinos/chain/system_calls.hpp>
#include <koinos/chain/system_call_ids.pb.h>

#include <koinos/log.hpp>
#include <koinos/util/conversion.hpp>

using namespace std::string_literals;

namespace koinos::chain {

host_api::host_api( execution_context& ctx ) : _ctx( ctx ) {}
host_api::~host_api() {}

uint32_t host_api::invoke_thunk( uint32_t tid, char* ret_ptr, uint32_t ret_len, const char* arg_ptr, uint32_t arg_len )
{
   KOINOS_ASSERT( _ctx.get_privilege() == privilege::kernel_mode, insufficient_privileges, "'invoke_thunk' must be called from a system context" );
   return thunk_dispatcher::instance().call_thunk( tid, _ctx, ret_ptr, ret_len, arg_ptr, arg_len );
}

uint32_t host_api::invoke_system_call( uint32_t sid, char* ret_ptr, uint32_t ret_len, const char* arg_ptr, uint32_t arg_len )
{
   uint32_t bytes_returned = 0;
   auto key = util::converter::as< std::string >( sid );
   database_object object;

   with_stack_frame(
      _ctx,
      stack_frame {
         .sid = sid,
         .call_privilege = privilege::kernel_mode
      },
      [&]() {
         if ( _ctx.system_call_exists( sid ) )
         {
            #pragma message "TODO: Brainstorm how to avoid arg/ret copy and validate pointers"
            std::string args( arg_ptr, arg_len );
            auto ret = _ctx.system_call( sid, args );
            KOINOS_ASSERT( ret.size() <= ret_len, insufficient_return_buffer, "return buffer too small" );
            std::memcpy( ret.data(), ret_ptr, ret.size() );
            bytes_returned = ret.size();
         }
         else
         {
            auto thunk_id = _ctx.thunk_translation( sid );
            KOINOS_ASSERT( thunk_dispatcher::instance().thunk_exists( thunk_id ), thunk_not_found, "thunk ${tid} does not exist", ("tid", thunk_id) );
            auto desc = chain::system_call_id_descriptor();
            auto enum_value = desc->FindValueByNumber( thunk_id );
            KOINOS_ASSERT( enum_value, thunk_not_found, "unrecognized thunk id ${id}", ("id", thunk_id) );
            auto compute = _ctx.get_compute_bandwidth( enum_value->name() );
            _ctx.resource_meter().use_compute_bandwidth( compute );
            bytes_returned = thunk_dispatcher::instance().call_thunk( thunk_id, _ctx, ret_ptr, ret_len, arg_ptr, arg_len );
         }
      }
   );

   return bytes_returned;
}

int64_t host_api::get_meter_ticks() const
{
   auto compute_bandwidth_remaining = _ctx.resource_meter().compute_bandwidth_remaining();

   // If we have more ticks than fizzy can accept
   if ( compute_bandwidth_remaining > std::numeric_limits< int64_t >::max() )
      compute_bandwidth_remaining = std::numeric_limits< int64_t >::max();

   int64_t ticks = compute_bandwidth_remaining;
   return ticks;
}

void host_api::use_meter_ticks( uint64_t meter_ticks )
{
   if ( meter_ticks > _ctx.resource_meter().compute_bandwidth_remaining() )
   {
      _ctx.resource_meter().use_compute_bandwidth( _ctx.resource_meter().compute_bandwidth_remaining() );
      _ctx.resource_meter().use_compute_bandwidth( 1 );
   }
   else
   {
      _ctx.resource_meter().use_compute_bandwidth( meter_ticks );
   }
}

} // koinos::chain
