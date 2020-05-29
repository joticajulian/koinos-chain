#include <boost/test/unit_test.hpp>

#include <koinos/crypto/multihash.hpp>
#include <koinos/log/log.hpp>
#include <koinos/pack/rt/json.hpp>
#include <koinos/statedb/koinos_object_types.hpp>
#include <koinos/statedb/merge_iterator.hpp>
#include <koinos/statedb/multi_index_types.hpp>
#include <koinos/statedb/state_delta.hpp>
#include <koinos/statedb/statedb.hpp>

#include <mira/database_configuration.hpp>

#include <boost/container/deque.hpp>

#include <iostream>

using namespace koinos::crypto;
using namespace koinos::statedb;

struct test_block
{
   multihash_type previous;
   uint64_t       block_num = 0;
   uint64_t       nonce = 0;

   void get_id( multihash_type& mh ) const;
};

KOINOS_REFLECT( test_block, (previous)(block_num)(nonce) )

struct book
{
   typedef uint64_t id_type;

   template<typename Constructor, typename Allocator>
   book( Constructor&& c, Allocator&& a )
   {
      c(*this);
   }

   book() = default;

   id_type id;
   int a = 0;
   int b = 1;

   int sum()const { return a + b; }
};

struct by_id;
struct by_a;
struct by_b;
struct by_sum;

typedef koinos::statedb::multi_index_container<
   book,
   koinos::statedb::indexed_by<
      koinos::statedb::ordered_unique< koinos::statedb::tag< by_id >, koinos::statedb::member< book, book::id_type, &book::id > >,
      koinos::statedb::ordered_unique< koinos::statedb::tag< by_a >,  koinos::statedb::member< book, int,           &book::a  > >,
      koinos::statedb::ordered_unique< koinos::statedb::tag< by_b >,
         koinos::statedb::composite_key< book,
            koinos::statedb::member< book, int, &book::b >,
            koinos::statedb::member< book, int, &book::a >
         >,
         koinos::statedb::composite_key_compare< std::less< int >, std::less< int > >
      >,
      koinos::statedb::ordered_unique< koinos::statedb::tag< by_sum >, koinos::statedb::const_mem_fun< book, int, &book::sum > >
  >
> book_index;

FC_REFLECT( book, (id)(a)(b) )

void test_block::get_id( multihash_type& mh )const
{
   return hash( mh, CRYPTO_SHA2_256_ID, *this );
}

struct statedb_fixture
{
   statedb_fixture()
   {
      temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
      boost::filesystem::create_directory( temp );
      boost::any cfg = mira::utilities::default_database_configuration();

      db.open( temp, cfg );
   }

   ~statedb_fixture()
   {
      db.close();
      boost::filesystem::remove_all( temp );
   }

   state_db db;
   boost::filesystem::path temp;
};

BOOST_FIXTURE_TEST_SUITE( statedb_tests, statedb_fixture )

BOOST_AUTO_TEST_CASE( fork_tests )
{ try {
   BOOST_TEST_MESSAGE( "Basic fork tests on statedb" );
   multihash_type id, prev_id, block_1000_id;
   test_block b;

   prev_id = db.get_root()->id();

   for( uint64_t i = 1; i <= 2000; ++i )
   {
      b.previous = prev_id;
      b.block_num = i;
      b.get_id( id );

      auto new_block = db.create_writable_node( prev_id, id );
      BOOST_CHECK_EQUAL( b.block_num, new_block->revision() );
      db.finalize_node( id );

      prev_id = id;

      if( i == 1000 ) block_1000_id = id;
   }

   BOOST_REQUIRE( db.get_root()->id() == zero_hash( CRYPTO_SHA2_256_ID ) );
   BOOST_REQUIRE( db.get_root()->revision() == 0 );

   BOOST_REQUIRE( db.get_head()->id() == prev_id );
   BOOST_REQUIRE( db.get_head()->revision() == 2000 );

   BOOST_REQUIRE( db.get_node( block_1000_id )->id() == block_1000_id );
   BOOST_REQUIRE( db.get_node( block_1000_id )->revision() == 1000 );

   BOOST_TEST_MESSAGE( "Test commit" );
   db.commit_node( block_1000_id );
   BOOST_REQUIRE( db.get_root()->id() == block_1000_id );
   BOOST_REQUIRE( db.get_root()->revision() == 1000 );

   multihash_type block_2000_id = id;

   BOOST_TEST_MESSAGE( "Test discard" );
   b.previous = db.get_head()->id();
   b.block_num = db.get_head()->revision() + 1;
   b.get_id( id );
   db.create_writable_node( b.previous, id );
   auto new_block = db.get_node( id );
   BOOST_REQUIRE( new_block );

   db.discard_node( id );

   BOOST_REQUIRE( db.get_head()->id() == prev_id );
   BOOST_REQUIRE( db.get_head()->revision() == 2000 );

   // Shared ptr should still exist, but not be returned with get_node
   BOOST_REQUIRE( new_block );
   BOOST_REQUIRE( !db.get_node( id ) );
   new_block.reset();

   // Cannot discard head
   BOOST_REQUIRE_THROW( db.discard_node( prev_id ), cannot_discard );

   BOOST_TEST_MESSAGE( "Check duplicate node creation" );
   BOOST_REQUIRE( !db.create_writable_node( db.get_head()->parent_id(), db.get_head()->id() ) );

   BOOST_TEST_MESSAGE( "Check failed linking" );
   multihash_type zero;
   zero_hash( zero, CRYPTO_SHA2_256_ID );
   BOOST_REQUIRE( !db.create_writable_node( zero, id ) );

   multihash_type head_id = db.get_head()->id();
   uint64_t head_rev = db.get_head()->revision();

   BOOST_TEST_MESSAGE( "Test minority fork" );
   auto fork_node = db.get_node_at_revision( 1995 );
   prev_id = fork_node->id();
   b.nonce = 1;

   for( uint64_t i = 1; i <= 5; ++i )
   {
      b.previous = prev_id;
      b.block_num = fork_node->revision() + i;
      b.get_id( id );

      auto new_block = db.create_writable_node( prev_id, id );
      BOOST_CHECK_EQUAL( b.block_num, new_block->revision() );
      db.finalize_node( id );

      BOOST_CHECK( db.get_head()->id() == head_id );
      BOOST_CHECK( db.get_head()->revision() == head_rev );

      prev_id = id;
   }

   b.previous = prev_id;
   b.block_num = head_rev + 1;
   b.get_id( id );

   // When this node finalizes, it will be the longest path and should become head
   new_block = db.create_writable_node( prev_id, id );
   BOOST_CHECK_EQUAL( b.block_num, new_block->revision() );

   BOOST_CHECK( db.get_head()->id() == head_id );
   BOOST_CHECK( db.get_head()->revision() == head_rev );

   db.finalize_node( id );

   BOOST_CHECK( db.get_head()->id() == id );
   BOOST_CHECK( db.get_head()->revision() == b.block_num );

} catch( const koinos::exception::koinos_exception& e ) { LOG(info) << e.to_string(); throw e; } }

BOOST_AUTO_TEST_CASE( merge_iterator )
{ try {
   /**
    * The merge iterator test was originally written to work with chainbase.
    * The state delta code has since been moved to state db, where the interface
    * has changed. Because this test is intended to test to correctness of the
    * merge iterators only, they will operate directly on state deltas, outside
    * of state_db.
    */
   boost::filesystem::path temp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
   boost::filesystem::create_directory( temp );
   boost::any cfg = mira::utilities::default_database_configuration();

   using state_delta_type = state_delta< book_index >;
   using state_delta_ptr = std::shared_ptr< state_delta_type >;

   boost::container::deque< state_delta_ptr > delta_deque;
   delta_deque.emplace_back( std::make_shared< state_delta_type >( temp, cfg ) );

   // Book 0: a: 5, b: 10, sum: 15
   // Book 1: a: 1, b: 7, sum: 8
   // Book 2: a: 10, b:3, sum 13
   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 5;
      b.b = 10;
   });

   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 1;
      b.b = 7;
   });

   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 10;
      b.b = 3;
   });

   // Undo State 0 orders:
   // by_a: 1, 0, 2
   // by_b: 2, 1, 0
   // by_sum: 1, 2, 0
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 10 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 7 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 7 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 10 );

      const auto id_ptr = delta_deque.back()->template find< by_id >( 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 7 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 7 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 10 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 10 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 7 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 7 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 10 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 10 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 7 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 7 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 10 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 5 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 10 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 7 );
   }

   // Book 0: a: 2, b: 13, sum: 15
   // Book 1: a: 3, b: 5, sum: 8
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   const auto book_0 = delta_deque.back()->template find< by_id >( 0 );
   BOOST_REQUIRE( book_0 != nullptr );
   BOOST_REQUIRE_EQUAL( book_0->id, 0 );
   BOOST_REQUIRE_EQUAL( book_0->a, 5 );
   BOOST_REQUIRE_EQUAL( book_0->b, 10 );
   delta_deque.back()->modify( *book_0, [&]( book& b )
   {
      b.a = 2;
      b.b = 13;
   });

   const auto book_1 = delta_deque.back()->template find< by_id >( 1 );
   BOOST_REQUIRE( book_1 != nullptr );
   BOOST_REQUIRE_EQUAL( book_1->id, 1 );
   BOOST_REQUIRE_EQUAL( book_1->a, 1 );
   BOOST_REQUIRE_EQUAL( book_1->b, 7 );
   delta_deque.back()->modify( *book_1, [&]( book& b )
   {
      b.a = 3;
      b.b = 5;
   });

   // Undo State 1 orders:
   // by_a: 0, 1, 2
   // by_b: 2, 1, 0 (not changed)
   // by_sum: 1, 2, 0 (not changed)
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 5 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 5 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );

      const auto id_ptr = delta_deque.back()->template find< by_id >( 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 3 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 5 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 5 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 5 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 5 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 5 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 5 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 5 );
   }

   // Book 0: a: 2, b: 13, sum: 15 (not changed)
   // Book 1: a: 1, b: 20, sum: 21
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   delta_deque.back()->modify( *(delta_deque.back()->template find< by_id >( 1 )), [&]( book& b )
   {
      b.a = 1;
      b.b = 20;
   });

   // Undo State 2 orders:
   // by_a: 1, 0, 2
   // by_b: 2, 0, 1
   // by_sum: 2, 0, 1
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );

      const auto id_ptr = delta_deque.back()->template find< by_id >( 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 20 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 0 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   // Book: 0 (removed)
   // Book 1: a: 1, b: 20, sum: 21 (not changed)
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   delta_deque.back()->erase( *(delta_deque.back()->template find< by_id >( 0 )) );

   // Undo State 3 orders:
   // by_a: 1, 2
   // by_b: 2, 1
   // by_sum: 2, 1
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );

      const auto id_ptr = delta_deque.back()->template find< by_id >( 0 );
      BOOST_REQUIRE_EQUAL( id_ptr, nullptr );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   // Book 1: a: 1, b: 20, sum: 21 (not changed)
   // Book 2: a: 10, b: 3, sum: 13 (not changed)
   // Book 3: a: 2, b: 13, sum: 15 (old book 0)
   delta_deque.emplace_back( std::make_shared< state_delta_type >( delta_deque.back(), delta_deque.back()->id() ) );
   delta_deque.back()->emplace( [&]( book& b )
   {
      b.a = 2;
      b.b = 13;
   });

   // Undo State 4 orders:
   // by_a: 1, 3, 2
   // by_b: 2, 3, 1
   // by_sum: 2, 3, 1
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );

      const auto id_ptr = delta_deque.back()->template find< by_id >( 3 );
      BOOST_REQUIRE_EQUAL( id_ptr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_ptr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_ptr->b, 13 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   delta_deque.pop_front();
   delta_deque.pop_front();
   delta_deque.front()->commit();
   {
      auto by_id_idx = merge_index< book_index, by_id >( delta_deque );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      ++id_itr;
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
   }

   while( delta_deque.size() > 1 )
   {
      delta_deque.pop_front();
      delta_deque.front()->commit();

      auto by_id_idx = merge_index< book_index, by_id >( delta_deque );
      auto id_itr = by_id_idx.begin();

      BOOST_REQUIRE( id_itr != by_id_idx.end() );
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      ++id_itr;
      BOOST_REQUIRE( id_itr == by_id_idx.end() );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 13 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 20 );
      ++id_itr;
      ++id_itr;
      --id_itr;
      BOOST_REQUIRE_EQUAL( id_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( id_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( id_itr->b, 3 );

      auto by_a_idx = merge_index< book_index, by_a >( delta_deque );
      auto a_itr = by_a_idx.begin();

      BOOST_REQUIRE( a_itr != by_a_idx.end() );
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      ++a_itr;
      BOOST_REQUIRE( a_itr == by_a_idx.end() );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 3 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 20 );
      ++a_itr;
      ++a_itr;
      --a_itr;
      BOOST_REQUIRE_EQUAL( a_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( a_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( a_itr->b, 13 );

      auto by_b_idx = merge_index< book_index, by_b >( delta_deque );
      auto b_itr = by_b_idx.begin();

      BOOST_REQUIRE( b_itr != by_b_idx.end() );
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      ++b_itr;
      BOOST_REQUIRE( b_itr == by_b_idx.end() );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 20 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 3 );
      ++b_itr;
      ++b_itr;
      --b_itr;
      BOOST_REQUIRE_EQUAL( b_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( b_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( b_itr->b, 13 );

      auto by_sum_idx = merge_index< book_index, by_sum >( delta_deque );
      auto sum_itr = by_sum_idx.begin();

      BOOST_REQUIRE( sum_itr != by_sum_idx.end() );
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      ++sum_itr;
      BOOST_REQUIRE( sum_itr == by_sum_idx.end() );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 1 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 20 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 10 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 3 );
      ++sum_itr;
      ++sum_itr;
      --sum_itr;
      BOOST_REQUIRE_EQUAL( sum_itr->id, 3 );
      BOOST_REQUIRE_EQUAL( sum_itr->a, 2 );
      BOOST_REQUIRE_EQUAL( sum_itr->b, 13 );
   }
} FC_LOG_AND_RETHROW() }

BOOST_AUTO_TEST_SUITE_END()