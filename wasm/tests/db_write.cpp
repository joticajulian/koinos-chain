#include <koinos/system/system_calls.hpp>

#include <string>

using namespace std::string_literals;

int main()
{
   koinos::system::object_space space;
   space.set_system( true );
   koinos::system::detail::put_object( space, "\x00"s, koinos::system::get_contract_arguments() );
   return 0;
}
