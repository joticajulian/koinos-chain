#pragma once

#include <thread>
#include <memory>

#include <koinos/net/jsonrpc/server.hpp>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

struct net_fixture
{
   boost::asio::io_context ioc;
   boost::filesystem::path unix_socket;

   std::shared_ptr< koinos::net::jsonrpc::request_handler > request_handler;

   std::unique_ptr< boost::asio::local::stream_protocol::socket > socket;
   std::unique_ptr< std::thread > thread;

   net_fixture()
   {
      request_handler = std::make_shared< koinos::net::jsonrpc::request_handler >();

      boost::filesystem::path tmp = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
      boost::filesystem::create_directory( tmp );
      unix_socket = tmp / "unit_test.sock" ;
      boost::asio::local::stream_protocol::endpoint endpoint( unix_socket.string() );

      // Create and launch a listening port
      std::make_shared< koinos::net::jsonrpc::listener >(
         ioc,
         endpoint,
         request_handler
      )->run();

      thread = std::make_unique< std::thread >( [&]{ ioc.run(); } );

      socket = std::make_unique< boost::asio::local::stream_protocol::socket >( ioc, endpoint.protocol() );
      socket->connect( unix_socket.string() );
   }

   ~net_fixture()
   {
      ioc.stop();
      thread->join();
      ::unlink( unix_socket.string().c_str() );
   }

   void write( const std::string& payload )
   {
      boost::asio::write( *socket, boost::asio::buffer( payload ) );
   }

   void write_http( const std::string & payload )
   {
      boost::beast::http::request< boost::beast::http::string_body > req { boost::beast::http::verb::get, "/", 11 };
      req.set( boost::beast::http::field::host, "127.0.0.1" );
      req.set( boost::beast::http::field::user_agent, "koinos_tests/1.0" );
      req.keep_alive( true );
      req.body() = payload;
      req.prepare_payload();
      boost::beast::http::write( *socket, req );
   }

   void write_request( const koinos::net::jsonrpc::request& r )
   {
      koinos::net::jsonrpc::json j = r;
      write_http( j.dump() );
   }

   std::string read()
   {
      boost::system::error_code error;
      boost::asio::streambuf receive_buffer;
      boost::asio::read( *socket, receive_buffer, boost::asio::transfer_all(), error );

      if ( error )
         throw std::runtime_error( "read failed: " + error.message() );

      return std::string( boost::asio::buffer_cast< const char* >( receive_buffer.data() ) );
   }

   boost::beast::http::response< boost::beast::http::string_body > read_http()
   {
      boost::beast::flat_buffer buffer;
      boost::beast::http::response< boost::beast::http::string_body > res;
      boost::beast::http::read( *socket, buffer, res );
      return res;
   }

   koinos::net::jsonrpc::response read_response()
   {
      auto res = read_http();
      koinos::net::jsonrpc::response r = koinos::net::jsonrpc::json::parse( res.body() );
      return r;
   }
};
