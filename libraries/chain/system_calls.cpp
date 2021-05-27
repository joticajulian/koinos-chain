#include <koinos/chain/apply_context.hpp>
#include <koinos/chain/constants.hpp>
#include <koinos/chain/system_calls.hpp>
#include <koinos/chain/thunk_dispatcher.hpp>
#include <koinos/crypto/multihash.hpp>
#include <koinos/log.hpp>

#include <algorithm>

namespace koinos::chain {

/*
 * This is a list of system calls registered at genesis.
 *
 * For initial Koinos development, this declaration should match THUNK_REGISTER.
 * However, as soon as a new thunk is added as an in band upgrade, it should be
 * added only to THUNK_REGISTER, not here. The registration of that thunk as a
 * syscall happens as an in band upgrade.
 */
SYSTEM_CALL_DEFAULTS(
   (prints)
   (exit_contract)

   (verify_block_signature)
   (verify_merkle_root)

   (apply_block)
   (apply_transaction)
   (apply_reserved_operation)
   (apply_upload_contract_operation)
   (apply_execute_contract_operation)
   (apply_set_system_call_operation)

   (db_put_object)
   (db_get_object)
   (db_get_next_object)
   (db_get_prev_object)

   (execute_contract)

   (get_entry_point)
   (get_contract_args_size)
   (get_contract_args)
   (set_contract_return)

   (get_head_info)
   (hash)
   (recover_public_key)

   (get_transaction_payer)
   (get_max_account_resources)
   (get_transaction_resource_limit)

   (get_last_irreversible_block)

   (get_caller)
   (get_transaction_signature)
   (require_authority)

   (get_contract_id)
   (get_head_block_time)
)

void register_thunks( thunk_dispatcher& td )
{
   THUNK_REGISTER( td,
      (prints)
      (exit_contract)

      (verify_block_signature)
      (verify_merkle_root)

      (apply_block)
      (apply_transaction)
      (apply_reserved_operation)
      (apply_upload_contract_operation)
      (apply_execute_contract_operation)
      (apply_set_system_call_operation)

      (db_put_object)
      (db_get_object)
      (db_get_next_object)
      (db_get_prev_object)

      (execute_contract)

      (get_entry_point)
      (get_contract_args_size)
      (get_contract_args)
      (set_contract_return)

      (get_head_info)
      (hash)
      (recover_public_key)

      (get_transaction_payer)
      (get_max_account_resources)
      (get_transaction_resource_limit)

      (get_last_irreversible_block)

      (get_caller)
      (get_transaction_signature)
      (require_authority)

      (get_contract_id)
      (get_head_block_time)
   )
}

// TODO: Should this be a thunk?
bool is_system_space( const statedb::object_space& space_id )
{
   return space_id == CONTRACT_SPACE_ID ||
          space_id == SYS_CALL_DISPATCH_TABLE_SPACE_ID ||
          space_id == KERNEL_SPACE_ID;
}

THUNK_DEFINE_BEGIN();

THUNK_DEFINE( void, prints, ((const std::string&) str) )
{
   context.console_append( str );
}

THUNK_DEFINE( void, exit_contract, ((uint8_t) exit_code) )
{
   switch( exit_code )
   {
      case KOINOS_EXIT_SUCCESS:
          KOINOS_THROW( exit_success, "" );
      case KOINOS_EXIT_FAILURE:
          KOINOS_THROW( exit_failure, "" );
      default:
          KOINOS_THROW( unknown_exit_code, "Contract specified unknown exit code" );
   }
}

THUNK_DEFINE( bool, verify_block_signature, ((const variable_blob&) signature_data, (const multihash&) digest) )
{
   crypto::recoverable_signature sig;
   pack::from_variable_blob( signature_data, sig );
   return crypto::public_key::from_base58( "5evxVPukp6bUdGNX8XUMD9e2J59j9PjqAVw2xYNw5xrdQPRRT8" ) == crypto::public_key::recover( sig, digest );
}

THUNK_DEFINE( bool, verify_merkle_root, ((const multihash&) root, (const std::vector< multihash >&) hashes) )
{
   std::vector< multihash > tmp = hashes;
   crypto::merkle_hash_leaves_like( tmp, root );
   return (tmp[0] == root);
}

// RAII class to ensure apply context block state is consistent if there is an error applying
// the block.
struct block_setter
{
   block_setter( apply_context& context, const protocol::block& block ) :
      ctx( context )
   {
      ctx.set_block( block );
   }

   ~block_setter()
   {
      ctx.clear_block();
   }

   apply_context& ctx;
};

THUNK_DEFINE( void, apply_block,
   (
      (const protocol::block&) block,
      (boolean) check_passive_data,
      (boolean) check_block_signature,
      (boolean) check_transaction_signatures)
   )
{
   KOINOS_TODO( "Check previous block hash, height, timestamp, and specify allowed set of hashing algorithms" );

   KOINOS_ASSERT( !context.is_in_user_code(), insufficient_privileges, "Calling privileged thunk from non-privileged code" );

   auto setter = block_setter( context, block );
   block.active_data.unbox();

   const multihash& tx_root = block.active_data->transaction_merkle_root;
   size_t tx_count = block.transactions.size();

   // Check transaction Merkle root
   std::vector< multihash > hashes( tx_count );

   for( std::size_t i = 0; i < tx_count; i++ )
   {
      hashes[i] = crypto::hash_like( tx_root, block.transactions[i].active_data );
   }
   KOINOS_ASSERT( system_call::verify_merkle_root( context, tx_root, hashes ), transaction_root_mismatch, "Transaction Merkle root does not match" );

   if( check_block_signature )
   {
      multihash block_hash;
      block_hash = crypto::hash_n( tx_root.id, block.header, block.active_data );
      KOINOS_ASSERT( system_call::verify_block_signature( context, block.signature_data, block_hash ), invalid_block_signature, "Block signature does not match" );
   }

   auto vkey = pack::to_variable_blob( std::string{ KOINOS_HEAD_BLOCK_TIME_KEY } );
   vkey.resize( 32, char(0) );
   auto key = pack::from_variable_blob< statedb::object_key >( vkey );
   system_call::db_put_object( context, KERNEL_SPACE_ID, key, pack::to_variable_blob( block.header.timestamp ) );

   // Check passive Merkle root
   if( check_passive_data )
   {
      // Passive Merkle root verifies:
      //
      // Block passive
      // Block signature slot (zero hash)
      // Transaction signatures
      //
      // Transaction passive
      // Transaction signature
      //
      // This matches the pattern of the input, except the hash of block_sig is zero because it has not yet been determined
      // during the block building process.

      const multihash& passive_root = block.active_data->passive_data_merkle_root;
      std::size_t passive_count = 2 * ( block.transactions.size() + 1 );
      hashes.resize( passive_count );

      hashes[0] = crypto::hash_like( passive_root, block.passive_data );
      hashes[1] = crypto::empty_hash_like( passive_root );

      // We hash in this order so that the two hashes for each transaction have a common Merkle parent
      for ( std::size_t i = 0; i < tx_count; i++ )
      {
         hashes[2*(i+1)]   = crypto::hash_like( passive_root, block.transactions[i].passive_data );
         hashes[2*(i+1)+1] = crypto::hash_blob_like( passive_root, block.transactions[i].signature_data );
      }

      KOINOS_ASSERT( system_call::verify_merkle_root( context, passive_root, hashes ), passive_root_mismatch, "Passive Merkle root does not match" );
   }

   //
   // +-----------+      +--------------+      +-------------------------+      +---------------------+
   // | Block sig | ---> | Block active | ---> | Transaction merkle root | ---> | Transaction actives |
   // +-----------+      +--------------+      +-------------------------+      +---------------------+
   //                           |
   //                           V
   //                +----------------------+      +----------------------+
   //                |                      | ---> |     Block passive    |
   //                |                      |      +----------------------+
   //                |                      |
   //                |                      |      +----------------------+
   //                | Passives merkle root | ---> | Transaction passives |
   //                |                      |      +----------------------+
   //                |                      |
   //                |                      |      +----------------------+
   //                |                      | ---> |   Transaction sigs   |
   //                +----------------------+      +----------------------+
   //

   for( const auto& tx : block.transactions )
   {
      system_call::apply_transaction( context, tx );
   }
}

// RAII class to ensure apply context transaction state is consistent if there is an error applying
// the transaction.
struct transaction_setter
{
   transaction_setter( apply_context& context, const protocol::transaction& trx ) :
      ctx( context )
   {
      ctx.set_transaction( trx );
   }

   ~transaction_setter()
   {
      ctx.clear_transaction();
   }

   apply_context& ctx;
};

inline void require_payer_transaction_nonce( apply_context& ctx, account_type payer, uint64 nonce )
{
   variable_blob vkey;
   pack::to_variable_blob( vkey, payer );
   pack::to_variable_blob( vkey, std::string{ KOINOS_TRANSACTION_NONCE_KEY }, true );

   statedb::object_key key;
   key = pack::from_variable_blob< statedb::object_key >( vkey );
   auto obj = system_call::db_get_object( ctx, KERNEL_SPACE_ID, key );
   if ( obj.size() > 0 )
   {
      uint64 unpacked_nonce = pack::from_variable_blob< uint64 >( obj );
      KOINOS_ASSERT(
         unpacked_nonce == (nonce - 1),
         chain::chain_exception,
         "Mismatching transaction nonce - last nonce: ${d}, expected: ${e}", ("d", unpacked_nonce)("e", unpacked_nonce + 1)
      );
   }
   else
   {
      KOINOS_ASSERT( nonce == 0, chain::chain_exception, "Initial transaction nonce should be 0" );
   }
}

inline void update_payer_transaction_nonce( apply_context& ctx, account_type payer, uint64 nonce )
{
   variable_blob vkey;
   pack::to_variable_blob( vkey, payer );
   pack::to_variable_blob( vkey, std::string{ KOINOS_TRANSACTION_NONCE_KEY }, true );

   statedb::object_key key;
   key = pack::from_variable_blob< statedb::object_key >( vkey );

   variable_blob obj;
   pack::to_variable_blob( obj, nonce );
   system_call::db_put_object( ctx, KERNEL_SPACE_ID, key, obj );
}

THUNK_DEFINE( void, apply_transaction, ((const protocol::transaction&) trx) )
{
   KOINOS_ASSERT( !context.is_in_user_code(), insufficient_privileges, "Calling privileged thunk from non-privileged code" );

   using namespace koinos::protocol;

   auto setter = transaction_setter( context, trx );
   trx.active_data.unbox();

   auto payer = system_call::get_transaction_payer( context, trx );
   system_call::require_authority( context, payer );
   require_payer_transaction_nonce( context, payer, trx.active_data->nonce );

   for( const auto& o : trx.active_data->operations )
   {
      std::visit( koinos::overloaded {
         [&]( const nop_operation& op ) { /* intentional fallthrough */ },
         [&]( const reserved_operation& op )
         {
            system_call::apply_reserved_operation( context, op );
         },
         [&]( const create_system_contract_operation& op )
         {
            system_call::apply_upload_contract_operation( context, op );
         },
         [&]( const call_contract_operation& op )
         {
            system_call::apply_execute_contract_operation( context, op );
         },
         [&]( const set_system_call_operation& op )
         {
            system_call::apply_set_system_call_operation( context, op );
         },
      }, o );
   }

   update_payer_transaction_nonce( context, payer, trx.active_data->nonce );
}

THUNK_DEFINE( void, apply_reserved_operation, ((const protocol::reserved_operation&) o) )
{
   KOINOS_ASSERT( !context.is_in_user_code(), insufficient_privileges, "Calling privileged thunk from non-privileged code" );
   KOINOS_THROW( reserved_operation_exception, "Unable to apply reserved operation" );
}

THUNK_DEFINE( void, apply_upload_contract_operation, ((const protocol::create_system_contract_operation&) o) )
{
   KOINOS_ASSERT( !context.is_in_user_code(), insufficient_privileges, "Calling privileged thunk from non-privileged code" );

   // Contract id is a ripemd160. It needs to be copied in to a uint256_t
   uint256_t contract_id = pack::from_fixed_blob< uint160_t >( o.contract_id );
   system_call::db_put_object( context, CONTRACT_SPACE_ID, contract_id, o.bytecode );
}

THUNK_DEFINE( void, apply_execute_contract_operation, ((const protocol::call_contract_operation&) o) )
{
   KOINOS_ASSERT( !context.is_in_user_code(), insufficient_privileges, "Calling privileged thunk from non-privileged code" );

   with_stack_frame(
      context,
      stack_frame {
         .call = crypto::hash( CRYPTO_RIPEMD160_ID, std::string( "apply_execute_contract_operation" ) ).digest,
         .call_privilege = privilege::user_mode,
      },
      [&]() {
         // execute_contract cannot be overridden
         thunk::execute_contract( context, o.contract_id, o.entry_point, o.args );
      }
   );
}

THUNK_DEFINE( void, apply_set_system_call_operation, ((const protocol::set_system_call_operation&) o) )
{
   KOINOS_ASSERT( !context.is_in_user_code(), insufficient_privileges, "Calling privileged thunk from non-privileged code" );

   // Ensure override exists
   std::visit(
   koinos::overloaded{
      [&]( const thunk_id& tid ) {
         KOINOS_ASSERT( thunk_dispatcher::instance().thunk_exists( static_cast< thunk_id >( tid ) ), thunk_not_found, "Thunk ${tid} does not exist", ("tid", (uint32_t)tid) );
      },
      [&]( const contract_call_bundle& scb ) {
         uint256_t contract_key = pack::from_fixed_blob< uint160_t >( scb.contract_id );
         auto contract = db_get_object( context, CONTRACT_SPACE_ID, contract_key );
         KOINOS_ASSERT( contract.size(), invalid_contract, "Contract does not exist" );
         KOINOS_ASSERT( ( o.call_id != static_cast< uint32_t >( system_call_id::execute_contract ) ), forbidden_override, "Cannot override execute_contract." );
      },
      [&]( const auto& ) {
         KOINOS_THROW( unknown_system_call, "set_system_call invoked with unimplemented type ${tag}",
                      ("tag", (uint64_t)o.target.index()) );
      } }, o.target );

   // Place the override in the database
   system_call::db_put_object( context, SYS_CALL_DISPATCH_TABLE_SPACE_ID, o.call_id, pack::to_variable_blob( o.target ) );
}

void check_db_permissions( const apply_context& context, const statedb::object_space& space )
{
   auto privilege = context.get_privilege();
   auto caller = pack::from_variable_blob< uint160 >( context.get_caller() );
   LOG(info) << "Privilege: " << privilege;
   LOG(info) << "Space: " << space;
   LOG(info) << "Caller: " << caller;
   if ( space != caller )
   {
      if ( context.get_privilege() == privilege::kernel_mode )
      {
         KOINOS_ASSERT( is_system_space( space ), insufficient_privileges, "privileged code can only accessed system space" );
      }
      else
      {
         KOINOS_THROW( out_of_bounds, "contract attempted access of non-contract database space" );
      }
   }
}

THUNK_DEFINE( bool, db_put_object, ((const statedb::object_space&) space, (const statedb::object_key&) key, (const variable_blob&) obj) )
{
   KOINOS_ASSERT( !context.is_read_only(), read_only_context, "Cannot put object during read only call" );
   check_db_permissions( context, space );

   auto state = context.get_state_node();
   KOINOS_ASSERT( state, state_node_not_found, "Current state node does not exist" );
   statedb::put_object_args put_args;
   put_args.space = space;
   put_args.key = key;
   put_args.buf = obj.data();
   put_args.object_size = obj.size();

   statedb::put_object_result put_res;
   state->put_object( put_res, put_args );

   return put_res.object_existed;
}

THUNK_DEFINE( variable_blob, db_get_object, ((const statedb::object_space&) space, (const statedb::object_key&) key, (int32_t) object_size_hint) )
{
   LOG(info) << "Key: " << key;
   check_db_permissions( context, space );

   auto state = context.get_state_node();
   KOINOS_ASSERT( state, state_node_not_found, "Current state node does not exist" );

   statedb::get_object_args get_args;
   get_args.space = space;
   get_args.key = key;
   get_args.buf_size = object_size_hint > 0 ? object_size_hint : STATE_DB_MAX_OBJECT_SIZE;

   variable_blob object_buffer;
   object_buffer.resize( get_args.buf_size );
   get_args.buf = object_buffer.data();

   statedb::get_object_result get_res;
   state->get_object( get_res, get_args );

   if( get_res.key == get_args.key && get_res.size > 0 )
      object_buffer.resize( get_res.size );
   else
      object_buffer.clear();

   return object_buffer;
}

THUNK_DEFINE( variable_blob, db_get_next_object, ((const statedb::object_space&) space, (const statedb::object_key&) key, (int32_t) object_size_hint) )
{
   check_db_permissions( context, space );

   auto state = context.get_state_node();
   KOINOS_ASSERT( state, state_node_not_found, "Current state node does not exist" );
   statedb::get_object_args get_args;
   get_args.space = space;
   get_args.key = key;
   get_args.buf_size = object_size_hint > 0 ? object_size_hint : STATE_DB_MAX_OBJECT_SIZE;

   variable_blob object_buffer;
   object_buffer.resize( get_args.buf_size );
   get_args.buf = object_buffer.data();

   statedb::get_object_result get_res;
   state->get_next_object( get_res, get_args );

   if( get_res.size > 0 )
      object_buffer.resize( get_res.size );
   else
      object_buffer.clear();

   return object_buffer;
}

THUNK_DEFINE( variable_blob, db_get_prev_object, ((const statedb::object_space&) space, (const statedb::object_key&) key, (int32_t) object_size_hint) )
{
   check_db_permissions( context, space );

   auto state = context.get_state_node();
   KOINOS_ASSERT( state, state_node_not_found, "Current state node does not exist" );
   statedb::get_object_args get_args;
   get_args.space = space;
   get_args.key = key;
   get_args.buf_size = object_size_hint > 0 ? object_size_hint : STATE_DB_MAX_OBJECT_SIZE;

   variable_blob object_buffer;
   object_buffer.resize( get_args.buf_size );
   get_args.buf = object_buffer.data();

   statedb::get_object_result get_res;
   state->get_prev_object( get_res, get_args );

   if( get_res.size > 0 )
      object_buffer.resize( get_res.size );
   else
      object_buffer.clear();

   return object_buffer;
}

THUNK_DEFINE( variable_blob, execute_contract, ((const contract_id_type&) contract_id, (uint32_t) entry_point, (const variable_blob&) args) )
{
   uint256_t contract_key = pack::from_fixed_blob< uint160_t >( contract_id );

   // We need to be in kernel mode to read the contract data
   variable_blob bytecode;
   with_stack_frame(
      context,
      stack_frame {
         .call = crypto::hash( CRYPTO_RIPEMD160_ID, std::string( "execute_contract" ) ).digest,
         .call_privilege = privilege::kernel_mode,
      },
      [&]()
      {
         bytecode = system_call::db_get_object( context, CONTRACT_SPACE_ID, contract_key );
         KOINOS_ASSERT( bytecode.size(), invalid_contract, "Contract does not exist" );
      }
   );

   wasm_allocator_type wa;
   wasm_code_ptr bytecode_ptr( (uint8_t*)bytecode.data(), bytecode.size() );
   backend_type backend( bytecode_ptr, bytecode_ptr.bounds(), registrar_type{} );

   backend.set_wasm_allocator( &wa );
   backend.initialize();

   context.push_frame( stack_frame {
      .call = pack::to_variable_blob( contract_id ),
      .call_privilege = context.get_privilege(),
      .call_args = args,
      .entry_point = entry_point
   } );

   try
   {
      backend( &context, "env", "_start" );
   }
   catch( const exit_success& ) {}
   catch( ... ) {
      context.pop_frame();
      throw;
   }

   return context.pop_frame().call_return;
}

THUNK_DEFINE_VOID( uint32_t, get_entry_point )
{
   LOG(info) << context.get_contract_entry_point();
   return context.get_contract_entry_point();
}

THUNK_DEFINE_VOID( uint32_t, get_contract_args_size )
{
   return (uint32_t)context.get_contract_call_args().size();
}

THUNK_DEFINE_VOID( variable_blob, get_contract_args )
{
   return context.get_contract_call_args();
}

THUNK_DEFINE( void, set_contract_return, ((const variable_blob&) ret) )
{
   pack::json j;
   pack::to_json( j, ret );
   LOG(info) << j.dump();
   context.set_contract_return( ret );
}

THUNK_DEFINE_VOID( chain::head_info, get_head_info )
{
   auto head = context.get_state_node();
   const block_height_type IRREVERSIBLE_THRESHOLD = block_height_type{ 6 };

   chain::head_info hi;
   hi.head_topology.id       = head->id();
   hi.head_topology.previous = head->parent_id();
   hi.head_topology.height   = head->revision();
   hi.last_irreversible_height = get_last_irreversible_block( context );

   return hi;
}

THUNK_DEFINE( multihash, hash, ((uint64_t) id, (const variable_blob&) obj, (uint64_t) size) )
{
   LOG(info) << "Hashing";
   KOINOS_ASSERT( crypto::multihash_id_is_known( id ), unknown_hash_code, "Unknown hash code" );
   auto hash = crypto::hash_str( id, obj.data(), obj.size(), size );
   LOG(info) << hash;
   return hash;
}

THUNK_DEFINE( variable_blob, recover_public_key, ((const variable_blob&) signature_data, (const multihash&) digest) )
{
   KOINOS_ASSERT( signature_data.size() == 65, invalid_signature, "Unexpected signature length" );
   crypto::recoverable_signature signature;
   std::copy_n( signature_data.begin(), signature_data.size(), signature.begin() );

   KOINOS_ASSERT( crypto::public_key::is_canonical( signature ), invalid_signature, "Signature must be canonical" );

   auto pub_key = crypto::public_key::recover( signature, digest );
   KOINOS_ASSERT( pub_key.valid(), invalid_signature, "Public key is invalid" );

   auto address = pub_key.to_address();
   return variable_blob( address.begin(), address.end() );
}

THUNK_DEFINE( account_type, get_transaction_payer, ((const protocol::transaction&) transaction) )
{
   multihash digest = crypto::hash( CRYPTO_SHA2_256_ID, transaction.active_data );
   account_type account = system_call::recover_public_key( context, transaction.signature_data, digest );

   LOG(debug) << "(get_transaction_payer) transaction: " << transaction;
   KOINOS_TODO( "stream override for variable_blob needs to be updated" );
   pack::json j;
   pack::to_json( j, account );
   LOG(debug) << "(get_transaction_payer) public_key: " << j.dump();

   return account;
}

THUNK_DEFINE( uint128, get_max_account_resources, ((const account_type&) account) )
{
   uint128 max_resources = 1000000000000;
   return max_resources;
}

THUNK_DEFINE( uint128, get_transaction_resource_limit, ((const protocol::transaction&) transaction) )
{
   transaction.active_data.unbox();
   const auto& active_data = transaction.active_data.get_const_native();

   return active_data.resource_limit;
}

THUNK_DEFINE_VOID( block_height_type, get_last_irreversible_block )
{
   static const block_height_type IRREVERSIBLE_THRESHOLD = block_height_type{ 6 };

   auto head = context.get_state_node();
   return block_height_type( head->revision() > IRREVERSIBLE_THRESHOLD ? head->revision() - IRREVERSIBLE_THRESHOLD : 0 );
}

THUNK_DEFINE_VOID( get_caller_return, get_caller )
{
   get_caller_return ret;
   auto frame0 = context.pop_frame(); // get_caller frame
   auto frame1 = context.pop_frame(); // contract frame
   ret.caller = context.get_caller();
   ret.caller_privilege = context.get_caller_privilege();
   context.push_frame( std::move( frame1 ) );
   context.push_frame( std::move( frame0 ) );
   return ret;
}

THUNK_DEFINE_VOID( variable_blob, get_transaction_signature )
{
   return context.get_transaction().signature_data;
}

THUNK_DEFINE( void, require_authority, ((const account_type&) account) )
{
   auto digest = crypto::hash( CRYPTO_SHA2_256_ID, context.get_transaction().active_data );
   account_type sig_account = system_call::recover_public_key( context, get_transaction_signature( context ), digest );
   KOINOS_ASSERT( sig_account.size() == account.size() &&
      std::equal(sig_account.begin(), sig_account.end(), account.begin()), invalid_signature, "signature does not match",
      ("account", account)("sig_account", sig_account) );
}

THUNK_DEFINE_VOID( contract_id_type, get_contract_id )
{
   LOG(info) << pack::from_variable_blob< uint160_t >( context.get_caller() );
   return pack::from_variable_blob< contract_id_type >( context.get_caller() );
}

THUNK_DEFINE_VOID( timestamp_type, get_head_block_time )
{
   auto block_ptr = context.get_block();
   if ( block_ptr )
   {
      return block_ptr->header.timestamp;
   }

   auto vkey = pack::to_variable_blob( std::string{ KOINOS_HEAD_BLOCK_TIME_KEY } );
   vkey.resize( 32, char(0) );
   auto key = pack::from_variable_blob< statedb::object_key >( vkey );
   return pack::from_variable_blob< timestamp_type >( db_get_object( context, KERNEL_SPACE_ID, key ) );
}

THUNK_DEFINE_END();

} // koinos::chain
