#pragma once
#include <koinos/state_db/backends/backend.hpp>
#include <koinos/state_db/backends/map/map_backend.hpp>
#include <koinos/state_db/backends/rocksdb/rocksdb_backend.hpp>
#include <koinos/state_db/state_db_types.hpp>

#include <koinos/crypto/multihash.hpp>

#include <any>
#include <filesystem>
#include <memory>
#include <unordered_set>

namespace koinos::state_db::detail {

   class state_delta
   {
      public:
         using backend_type  = backends::abstract_backend;
         using key_type      = backend_type::key_type;
         using value_type    = backend_type::value_type;

      private:
         std::shared_ptr< state_delta >             _parent;

         std::shared_ptr< backend_type >            _backend;
         std::unordered_set< key_type >             _removed_objects;

         state_node_id                              _id;
         uint64_t                                   _revision = 0;
         mutable std::optional< crypto::multihash > _merkle_root;

      public:
         state_delta( std::shared_ptr< state_delta > parent, const state_node_id& id = state_node_id() );
         state_delta( const std::filesystem::path& p );
         ~state_delta() {};

         void put( const key_type& k, const value_type& v );
         void erase( const key_type& k );
         const value_type* find( const key_type& key ) const;

         void squash();
         void commit();

         void clear();

         bool is_modified( const key_type& k ) const;
         bool is_removed( const key_type& k ) const;
         bool is_root() const;
         bool is_empty() const;

         uint64_t revision() const;
         void set_revision( uint64_t revision );

         crypto::multihash get_merkle_root() const;

         const state_node_id& id() const;
         const state_node_id& parent_id() const;
         std::shared_ptr< state_delta > parent() const;

         const std::shared_ptr< backend_type > backend() const;

      private:
         void commit_helper();

         std::shared_ptr< state_delta > get_root();
   };

} // koinos::state_db::detail
