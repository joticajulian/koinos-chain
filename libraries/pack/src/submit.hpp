namespace koinos { namespace chain {

// Every block has a cryptographic ID.
// Check the claimed ID against the block content.

struct block_topology
{
   protocol::multihash_type                 id;
   protocol::block_height_type              block_num;
   protocol::multihash_type                 previous;
};

struct reserved_submission {};

struct block_submission
{
   block_topology                                topology;

   protocol::variable_blob                       header_bytes;
   std::vector< protocol::variable_blob >        transactions_bytes;
   std::vector< protocol::variable_blob >        passives_bytes;
};

struct transaction_submission
{
   protocol::variable_blob                       active_bytes;
   protocol::variable_blob                       passive_bytes;
};

struct query_submission
{
   protocol::variable_blob                       query;
};

typedef std::variant<
   reserved_submission,
   block_submission,
   transaction_submission,
   query_submission > submission_item;

struct get_head_info_params {};

typedef std::variant<
   get_head_info_params > query_param_item;

struct reserved_submission_result {};

struct block_submission_result {};

struct transaction_submission_result {};

struct query_submission_result
{
   protocol::variable_blob result;
};

struct submission_error_result
{
   protocol::variable_blob error_text;
};

typedef std::variant<
   reserved_submission_result,
   block_submission_result,
   transaction_submission_result,
   query_submission_result,
   submission_error_result > submission_result;

typedef query_submission_result query_error;

struct get_head_info_result
{
   protocol::multihash_type    id;
   protocol::block_height_type height;
};

typedef std::variant<
   query_error,
   get_head_info_result > query_item_result;

} } // koinos::chain_control
