#pragma once

#include <koinos/state_db/backends/iterator.hpp>

#include <map>

namespace koinos::state_db::backends::map {

class map_backend;

class map_iterator final : public abstract_iterator
{
   public:
      using value_type    = abstract_iterator::value_type;
      using map_impl      = std::map< detail::key_type, detail::value_type >;
      using iterator_impl = map_impl::iterator;

      map_iterator( std::unique_ptr< iterator_impl > itr, const map_impl& map );
      virtual ~map_iterator();

      virtual const value_type& operator*()const;

      virtual abstract_iterator& operator++();
      virtual abstract_iterator& operator--();

   private:
      virtual bool valid()const;
      virtual std::unique_ptr< abstract_iterator > copy()const;

      std::unique_ptr< iterator_impl > _itr;
      const map_impl&                  _map;
};

} // koinos::state_db::backends::map
