namespace koinos { namespace protocol {

enum class header_hash_index
{
   /**
    * Hash of the previous block.
    */
   previous_block_hash_index = 0,

   /**
    * Hash of Merkle root of transactions.
    */
   transaction_merkle_root_hash_index = 1,

   /**
    * Hash of Merkle root of segwit data.
    */
   segwit_merkle_root_hash_index = 2,

   /**
    * Number of header hashes.
    */
   NUM_HEADER_HASHES = 3
};

struct block_topology
{
   multihash_type                 id;
   block_height_type              block_num;
   multihash_type                 previous;
};
    
struct active_block_data
{
   /**
    * Hashes included in the header.
    * All hashes must use the same algorithm.
    */
   multihash_vector               header_hashes;

   /**
    * Block height.  The genesis block has height=1.
    */
   block_height_type              height;

   /**
    * The timestamp.  Must be zero.
    */
   timestamp_type                 timestamp;

   /**
    * A zero byte at the end, reserved for protocol expansion.
    */
   unused_extensions_type         extensions;
};

struct passive_block_data
{
  signature_type block_signature;
};
  
// TODO: Do we need this?
struct block_header
{
   // Block data that can read and write state
   vl_blob            active_bytes;

   // Block data that can only read state
   multihash_type     passive_merkle_root;
};

struct reserved_operation
{
   unused_extensions_type         extensions;
};

struct nop_operation
{
   unused_extensions_type         extensions;
};

struct create_system_contract_operation
{
   contract_id_type               contract_id;
   vl_blob                        bytecode;
   unused_extensions_type         extensions;
};

struct contract_call_operation
{
   contract_id_type               contract_id;
   uint32                         entrypoint;
   vl_blob                        args;
   unused_extensions_type         extensions;
};

typedef std::variant<
   reserved_operation,
   nop_operation,
   create_system_contract_operation,
   contract_call_operation
   > operation;

struct reserved_block_header
{
   unused_extensions_type         extensions;
};

struct transaction_type
{
   std::vector<operation>         operations;

   /**
    * A zero byte at the end, reserved for protocol expansion.
    */
   unused_extensions_type         extensions;
};

struct reserved_segwit
{
   unused_extensions_type         extensions;
};

struct block_ref_segwit
{
   multihash_type                 ref_block_id;
   block_height_type              ref_block_height;
   unused_extensions_type         extensions;
};

struct expiration_segwit
{
   timestamp_type                 expiration_timestamp;
   unused_extensions_type         extensions;
};

struct signatures_segwit
{
   std::vector<signature_type>    signatures;
   unused_extensions_type         extensions;
};

struct contract_source_segwit
{
   multihash_type                 sourcehash;
   unused_extensions_type         extensions;
};

struct custom_segwit
{
   multihash_type                 world;
   vl_blob                        custom_data;
   unused_extensions_type         extensions;
};

typedef std::variant<
   reserved_segwit,
   block_ref_segwit,
   expiration_segwit,
   signatures_segwit,
   contract_source_segwit,
   custom_segwit > segwit_type;

} } // koinos::protocol
