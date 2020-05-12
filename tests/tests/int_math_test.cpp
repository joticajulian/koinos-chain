
#include <boost/test/unit_test.hpp>

#include "../test_fixtures/int_math_fixture.hpp"

#include <koinos/chain/types.hpp>

#include <algorithm>
#include <iostream>
#include <limits>

BOOST_FIXTURE_TEST_SUITE( int_math_tests, int_math_fixture )

using koinos::chain::uint128_t;

std::string uint128_to_str( uint128_t value )
{
   if( value == 0 )
      return "0";

   std::string result;
   static const char digits[] = "0123456789";
   while( value > 0 )
   {
      result.push_back(digits[value % 10]);
      value /= 10;
   }
   std::reverse( result.begin(), result.end() );
   return result;
}

BOOST_AUTO_TEST_SUITE_END()
