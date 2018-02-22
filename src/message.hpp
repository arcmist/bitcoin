/**************************************************************************
 * Copyright 2017-2018 ArcMist, LLC                                       *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@arcmist.com>                                    *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#ifndef BITCOIN_MESSAGE_HPP
#define BITCOIN_MESSAGE_HPP

#include "arcmist/base/math.hpp"
#include "arcmist/base/string.hpp"
#include "arcmist/base/hash.hpp"
#include "arcmist/io/stream.hpp"
#include "arcmist/io/buffer.hpp"
#include "arcmist/io/network.hpp"
#include "base.hpp"
#include "info.hpp"
#include "transaction.hpp"
#include "block.hpp"
#include "key.hpp"
#include "bloom_filter.hpp"

#include <cstdint>


namespace BitCoin
{
    namespace Message
    {
        enum Type
        {
            UNKNOWN,

            // Control messages
            VERSION, VERACK, PING, PONG, GET_ADDRESSES, ADDRESSES, ALERT,
            FEE_FILTER, SEND_HEADERS,

            // Data messages
            GET_BLOCKS, BLOCK, GET_DATA, GET_HEADERS, HEADERS, INVENTORY,
            TRANSACTION,

            // Version >= 60002
            MEM_POOL, // BIP-0035 Respond with inventory of all transactions in mempool

            // Version >= 70001
            FILTER_ADD, FILTER_CLEAR, FILTER_LOAD, MERKLE_BLOCK, //BIP-0037
            NOT_FOUND,

            // Version >= 70002
            REJECT, // BIP-0061

            // Version >= 70014
            SEND_COMPACT, COMPACT_BLOCK, GET_BLOCK_TRANSACTIONS, BLOCK_TRANSACTIONS // BIP-0152

        };

        const char *nameFor(Type pType);
        Type typeFor(const char *pCommand);

        class InventoryHash
        {
        public:

            enum Type { UNKNOWN=0x00, TRANSACTION=0x01, BLOCK=0x02, FILTERED_BLOCK=0x03, COMPACT_BLOCK=0x04 };

            InventoryHash() : hash(32) { type = UNKNOWN; }
            InventoryHash(Type pType, const ArcMist::Hash &pHash) { type = pType; hash = pHash; }
            InventoryHash(InventoryHash &pCopy) : hash(pCopy.hash) { type = pCopy.type; }

            bool operator == (const InventoryHash &pRight) { return type == pRight.type && hash == pRight.hash; }
            bool operator != (const InventoryHash &pRight) { return type != pRight.type || hash != pRight.hash; }

            void write(ArcMist::OutputStream *pStream) const;
            bool read(ArcMist::InputStream *pStream);

            Type type;
            ArcMist::Hash hash;

        private:
            InventoryHash &operator = (InventoryHash &pRight);
        };

        class Inventory : public std::vector<InventoryHash *>
        {
        public:

            Inventory() {}
            ~Inventory();

            void write(ArcMist::OutputStream *pStream) const;
            bool read(ArcMist::InputStream *pStream, unsigned int pSize);

            void clear()
            {
                for(iterator hash=begin();hash!=end();++hash)
                    delete *hash;
                std::vector<InventoryHash *>::clear();
            }
            void clearNoDelete() { std::vector<InventoryHash *>::clear(); }

        private:
            Inventory(Inventory &pCopy);
            Inventory &operator = (Inventory &pRight);
        };

        class Data
        {
        public:

            Data(Type pType) { type = pType; }
            virtual ~Data() {}
            virtual void write(ArcMist::OutputStream *pStream) {}
            virtual bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion)
            {
                if(pStream->remaining() < pSize)
                    return false;
                for(unsigned int i=0;i<pSize;i++)
                    pStream->readByte();
                return true;
            }

            Type type;

        };

        class Interpreter
        {
        public:

            Interpreter() { version = 0; pendingBlockStartTime = 0; pendingBlockLastReportTime = 0; pendingBlockUpdateTime = 0; }

            Data *read(ArcMist::Buffer *pInput, const char *pName);
            void write(Data *pData, ArcMist::Buffer *pOutput);

            int32_t version;
            ArcMist::Hash pendingBlockHash;
            uint32_t pendingBlockStartTime, pendingBlockLastReportTime, pendingBlockUpdateTime;
            unsigned int lastPendingBlockSize;

        };

        class VersionData : public Data
        {
        public:

            static const unsigned int FULL_NODE_BIT    = 0x01;
            static const unsigned int GETUTXO_NODE_BIT = 0x02; // BIP-0064
            static const unsigned int BLOOM_NODE_BIT   = 0x04; // BIP-0111 Supports bloom filters and merkle block requests
            static const unsigned int WITNESS_NODE_BIT = 0x08; // Segregated Witness
            static const unsigned int XTHIN_NODE_BIT   = 0x10; // BUIP-0010
            static const unsigned int CASH_NODE_BIT    = 0x20; // Bitcoin Cash
            static const unsigned int LIMITED_NODE_BIT = 0x0400; // BIP-0159 "Full" node serving only the last 288 (2 day) blocks

            VersionData() : Data(VERSION) { }
            VersionData(const uint8_t *pReceivingIP, uint16_t pReceivingPort, uint64_t pReceivingServices,
                        const uint8_t *pTransmittingIP, uint16_t pTransmittingPort,
                        bool pFullNode, bool pCashNode, uint32_t pStartBlockHeight, bool pRelay);

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            int32_t version;
            uint64_t services;
            int64_t time;
            uint64_t receivingServices;
            uint8_t receivingIPv6[16];
            uint16_t receivingPort;
            uint64_t transmittingServices; // Same as services
            uint8_t transmittingIPv6[16];
            uint16_t transmittingPort;
            uint64_t nonce;
            ArcMist::String userAgent;
            int32_t startBlockHeight;
            uint8_t relay; // Announce new transactions/blocks
        };

        class PingData : public Data
        {
        public:

            PingData() : Data(PING) { nonce = ArcMist::Math::randomLong(); }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            uint64_t nonce;
        };

        class PongData : public Data
        {
        public:

            PongData() : Data(PONG) { nonce = 0; }
            PongData(uint64_t pNonce) : Data(PONG) { nonce = pNonce; }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            uint64_t nonce;
        };

        // BIP-0061
        class RejectData : public Data
        {
        public:

            enum Code
            {
                DECODE       = 0x01, // Decode issue, probably message header
                INVALID      = 0x10, // Invalid signature or proof of work
                PROTOCOL     = 0x11, // Unsupported version or protocol
                DUPLICATE    = 0x12, // Duplicate input spend or more than one of the same message
                NON_STANDARD = 0x40, // Transaction is non standard
                BELOW_DUST   = 0x41, // Output is below dust amount
                LOW_FEE      = 0x42, // Fee too low
                WRONG_CHAIN  = 0x43  // Block is for wrong chain
            };

            RejectData() : Data(REJECT) { code = 0; }
            RejectData(const char *pCommand, uint8_t pCode, const char *pReason, ArcMist::Buffer *pExtra) : Data(REJECT)
            {
                command = pCommand;
                code = pCode;
                reason = pReason;

                if(pExtra != NULL)
                    extra.write(pExtra, pExtra->remaining());
            }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            ArcMist::String command;
            uint8_t code;
            ArcMist::String reason;
            ArcMist::Buffer extra;
        };

        class Address
        {
        public:

            Address()
            {
                time = 0;
                services = 0;
                std::memset(ip, 0, 16);
                port = 0;
            }
            Address(const Address &pCopy)
            {
                time = pCopy.time;
                services = pCopy.services;
                std::memcpy(ip, pCopy.ip, 16);
                port = pCopy.port;
            }

            void write(ArcMist::OutputStream *pStream) const;
            bool read(ArcMist::InputStream *pStream);

            Address &operator = (const Address &pRight)
            {
                time = pRight.time;
                services = pRight.services;
                std::memcpy(ip, pRight.ip, 16);
                port = pRight.port;
                return *this;
            }

            Address &operator = (const Peer &pRight)
            {
                time = pRight.time;
                services = pRight.services;
                std::memcpy(ip, pRight.address.ip, 16);
                port = pRight.address.port;
                return *this;
            }

            uint32_t time;
            uint64_t services;
            uint8_t ip[16];
            uint16_t port;
        };

        class AddressesData : public Data
        {
        public:

            AddressesData() : Data(ADDRESSES) { }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            std::vector<Address> addresses;
        };

        class FeeFilterData : public Data
        {
        public:

            FeeFilterData(uint64_t pMinimumFeeRate) : Data(FEE_FILTER) { minimumFeeRate = pMinimumFeeRate; }
            FeeFilterData() : Data(FEE_FILTER) { minimumFeeRate = 0; }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            uint64_t minimumFeeRate; // Satoshis per KiB
        };

        class FilterAddData : public Data
        {
        public:

            FilterAddData() : Data(FILTER_ADD) { }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            ArcMist::Buffer data;
        };

        class FilterLoadData : public Data
        {
        public:

            FilterLoadData() : Data(FILTER_LOAD) { }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            BloomFilter filter;
        };

        // Request block headers
        class GetBlocksData : public Data
        {
        public:

            GetBlocksData() : Data(GET_BLOCKS), stopHeaderHash(32) { version = PROTOCOL_VERSION; }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            uint32_t version;
            std::vector<ArcMist::Hash> blockHeaderHashes; // In reverse order (Highest block first)

            ArcMist::Hash stopHeaderHash; // Zeroized to stop at highest block on chain

        };

        class BlockData : public Data
        {
        public:

            BlockData() : Data(BLOCK) { block = NULL; }
            ~BlockData() { if(block != NULL) delete block; }

            void write(ArcMist::OutputStream *pStream)
            {
                if(block != NULL)
                    block->write(pStream, true, true);
            }
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion)
            {
                if(block == NULL)
                    block = new Block();
                return block->read(pStream, true, true, true);
            }

            Block *block;

        };

        class GetDataData : public Data
        {
        public:

            GetDataData() : Data(GET_DATA) { }

            void write(ArcMist::OutputStream *pStream) { inventory.write(pStream); }
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion)
              { return inventory.read(pStream, pSize); }

            Inventory inventory;

        };

        // Request a Headers message
        class GetHeadersData : public Data
        {
        public:

            GetHeadersData() : Data(GET_HEADERS), stopHeaderHash(32) { version = PROTOCOL_VERSION; }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            uint32_t version;

            // Listing of block hashes that you have in reverse order (Highest block first)
            // Maybe like every 100th block or something
            // First block in this list that they have they will send you headers for
            //   everything after through the stop header
            std::vector<ArcMist::Hash> blockHeaderHashes;

            ArcMist::Hash stopHeaderHash; // Zeroized to stop at highest block on chain

        };

        class HeadersData : public Data
        {
        public:

            HeadersData() : Data(HEADERS) {}

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            BlockList headers;

        };

        class InventoryData : public Data
        {
        public:

            InventoryData() : Data(INVENTORY) { }

            void write(ArcMist::OutputStream *pStream) { inventory.write(pStream); }
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion)
              { return inventory.read(pStream, pSize); }

            Inventory inventory;

        };

        // Used with FILTER_ADD, FILTER_CLEAR, FILTER_LOAD to request specific transactions
        class MerkleBlockData : public Data
        {
        public:

            MerkleBlockData() : Data(MERKLE_BLOCK) { block = NULL; blockNeedsDelete = false; }
            MerkleBlockData(Block *pBlock, BloomFilter &pFilter, std::vector<Transaction *> &pIncludedTransactions);
            ~MerkleBlockData() { if(blockNeedsDelete && block != NULL) delete block; }

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            // Validate hashes and get included "confirmed" transaction hashes.
            // Note: This assumes the block header has already been verified as valid in the most
            //   proof of work chain.
            bool validate(ArcMist::HashList &pIncludedTransactionHashes);

            Block *block;
            bool blockNeedsDelete;
            ArcMist::HashList hashes;
            ArcMist::Buffer flags;

        private:

            // Recursively parse merkle node hashes into a tree
            bool parse(MerkleNode *pNode, unsigned int pDepth, unsigned int &pHashesOffset, unsigned int &pBitOffset,
              unsigned char &pByte, ArcMist::HashList &pIncludedTransactionHashes);

            // Recursively parse merkle tree and add hashes and flags for specified node
            void addNode(MerkleNode *pNode, unsigned int pDepth, unsigned int &pNextBitOffset,
              unsigned char &pNextByte, std::vector<Transaction *> &pIncludedTransactions);

        };

        class TransactionData : public Data
        {
        public:

            TransactionData() : Data(TRANSACTION) { transaction = NULL; }
            ~TransactionData() { if(transaction != NULL) delete transaction; }

            void write(ArcMist::OutputStream *pStream)
            {
                if(transaction != NULL)
                    transaction->write(pStream);
            }
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion)
            {
                if(transaction == NULL)
                    transaction = new Transaction();
                return transaction->read(pStream);
            }

            Transaction *transaction;

        };

        class NotFoundData : public Data
        {
        public:

            NotFoundData() : Data(NOT_FOUND) { }

            void write(ArcMist::OutputStream *pStream) { inventory.write(pStream); }
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion)
              { return inventory.read(pStream, pSize); }

            Inventory inventory;

        };

        class SendCompactData : public Data
        {
        public:

            SendCompactData() : Data(SEND_COMPACT) { }

            void write(ArcMist::OutputStream *pStream)
            {
                pStream->writeByte(sendCompact);
                pStream->writeUnsignedLong(encoding);
            }
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion)
            {
                if(pSize != 9)
                    return false;
                sendCompact = pStream->readByte();
                encoding = pStream->readUnsignedLong();
                return true;
            }

            bool sendCompact;
            uint64_t encoding;

        };

        class PrefilledTransaction
        {
        public:

            PrefilledTransaction() { offset = 0; transaction = 0; }
            PrefilledTransaction(unsigned int pOffset, Transaction *pTransaction)
            {
                offset = pOffset;
                transaction = pTransaction;
            }

            unsigned int offset;
            Transaction *transaction; // Reference to transaction contained in block

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize);
        };

        class CompactBlockData : public Data
        {
        public:

            CompactBlockData();
            ~CompactBlockData();

            // Sets block to be sent in message
            void setBlock(Block *pBlock); // Does not delete block given this way

            // Decodes transaction IDs and puts transactions in the block
            bool fillBlock();

            bool updateShortIDs();

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

            Block *block;
            uint64_t nonce;
            ArcMist::HashList shortIDs;
            std::vector<PrefilledTransaction> prefilledTransactionIDs;

            bool deleteBlock;

        };

        class GetBlockTransactionsData : public Data
        {
        public:

            GetBlockTransactionsData() : Data(GET_BLOCK_TRANSACTIONS) {}

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

        };

        class BlockTransactionsData : public Data
        {
        public:

            BlockTransactionsData() : Data(BLOCK_TRANSACTIONS) {}

            void write(ArcMist::OutputStream *pStream);
            bool read(ArcMist::InputStream *pStream, unsigned int pSize, int32_t pVersion);

        };

        bool test();
    }
}

#endif
