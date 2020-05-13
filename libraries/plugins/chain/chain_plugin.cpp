#include <koinos/plugins/chain/chain_plugin.hpp>

#include <koinos/chain/debug_state.hpp>

#include <mira/database_configuration.hpp>

#include <fc/io/json.hpp>
#include <fc/exception/exception.hpp>

namespace koinos::plugins::chain {

namespace detail {

class chain_plugin_impl
{
   public:
      chain_plugin_impl()  {}
      ~chain_plugin_impl() {}

      void write_default_database_config( bfs::path &p );

      uint32_t             chainbase_flags = 0;
      bfs::path            state_dir;
      bfs::path            database_cfg;

      chainbase::database  db;
};

void chain_plugin_impl::write_default_database_config( bfs::path &p )
{
   ilog( "writing database configuration: ${p}", ("p", p.string()) );
   fc::json::save_to_file( mira::utilities::default_database_configuration(), p );
}

} // detail


chain_plugin::chain_plugin() : my( new detail::chain_plugin_impl() ) {}
chain_plugin::~chain_plugin(){}

chainbase::database& chain_plugin::db() { return my->db; }
const chainbase::database& chain_plugin::db() const { return my->db; }

bfs::path chain_plugin::state_dir() const
{
   return my->state_dir;
}

void chain_plugin::set_program_options(options_description& cli, options_description& cfg)
{
   cfg.add_options()
         ("state-dir", bpo::value<bfs::path>()->default_value("blockchain"),
            "the location of the blockchain state files (absolute path or relative to application data dir)")
         ("database-config", bpo::value<bfs::path>()->default_value("database.cfg"), "The database configuration file location")
         ;
   cli.add_options()
         ("force-open", bpo::bool_switch()->default_value(false), "force open the database, skipping the environment check")
         ;
}

void chain_plugin::plugin_initialize(const variables_map& options)
{
   my->state_dir = app().data_dir() / "blockchain";

   if( options.count("state-dir") )
   {
      auto sfd = options.at("state-dir").as<bfs::path>();
      if(sfd.is_relative())
         my->state_dir = app().data_dir() / sfd;
      else
         my->state_dir = sfd;
   }

   my->chainbase_flags |= options.at( "force-open" ).as< bool >() ? chainbase::skip_env_check : chainbase::skip_nothing;

   my->database_cfg = options.at( "database-cfg" ).as< bfs::path >();

   if( my->database_cfg.is_relative() )
      my->database_cfg = app().data_dir() / my->database_cfg;

   if( !bfs::exists( my->database_cfg ) )
   {
      my->write_default_database_config( my->database_cfg );
   }
}

void chain_plugin::plugin_startup()
{
   fc::variant database_config;

   try
   {
      database_config = fc::json::from_file( my->database_cfg, fc::json::strict_parser );
   }
   catch ( const std::exception& e )
   {
      elog( "Error while parsing database configuration: ${e}", ("e", e.what()) );
      exit( EXIT_FAILURE );
   }
   catch ( const fc::exception& e )
   {
      elog( "Error while parsing database configuration: ${e}", ("e", e.what()) );
      exit( EXIT_FAILURE );
   }

   try
   {
      my->db.open( my->state_dir, my->chainbase_flags, database_config );
      my->db.add_index< koinos::chain::debug_state_index >();
   }
   catch( fc::exception& e )
   {
      wlog( "Error opening database.");
      wlog( " Error: ${e}", ("e", e) );
      exit( EXIT_FAILURE );
   }
}

void chain_plugin::plugin_shutdown()
{
   ilog("closing chain database");
   my->db.close();
   ilog("database closed successfully");
}

} // namespace koinos::plugis::chain
