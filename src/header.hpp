/**************************************************************************
 * Copyright 2017-2018 NextCash, LLC                                      *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#ifndef BITCOIN_HEADER_HPP
#define BITCOIN_HEADER_HPP

#include "hash.hpp"
#include "log.hpp"
#include "stream.hpp"
#include "file_stream.hpp"
#include "base.hpp"
#include "forks.hpp"


namespace BitCoin
{
    // Statistical information needed from each header.
    class HeaderStat
    {
    public:

        HeaderStat() : accumulatedWork(32)
        {
            version = 0;
            time = 0;
            targetBits = 0;
        }
        HeaderStat(const HeaderStat &pCopy) : accumulatedWork(pCopy.accumulatedWork)
        {
            version = pCopy.version;
            time = pCopy.time;
            targetBits = pCopy.targetBits;
        }
        HeaderStat(int32_t pVersion, Time pTime, uint32_t pTargetBits) : accumulatedWork(32)
        {
            version = pVersion;
            time = pTime;
            targetBits = pTargetBits;

            NextCash::Hash target(32);
            target.setDifficulty(pTargetBits);
            target.getWork(accumulatedWork);
        }
        HeaderStat(int32_t pVersion, Time pTime, uint32_t pTargetBits,
          NextCash::Hash &pPreviousAccumulatedWork) : accumulatedWork(32)
        {
            version = pVersion;
            time = pTime;
            targetBits = pTargetBits;

            NextCash::Hash target(32);
            target.setDifficulty(pTargetBits);
            target.getWork(accumulatedWork);

            accumulatedWork += pPreviousAccumulatedWork;
        }

        HeaderStat &operator = (const HeaderStat &pRight)
        {
            version = pRight.version;
            time = pRight.time;
            targetBits = pRight.targetBits;
            accumulatedWork = pRight.accumulatedWork;
            return *this;
        }

        int32_t        version;
        Time           time;
        uint32_t       targetBits;
        NextCash::Hash accumulatedWork;
    };

    class Header;
    typedef std::vector<Header> HeaderList;

    class Header
    {
    public:

        Header() : previousHash(32), merkleHash(32)
        {
            version = 4;
            time = 0;
            targetBits = 0;
            nonce = 0;
            transactionCount = 0;
        }
        Header(const Header &pCopy) : previousHash(pCopy.previousHash),
          merkleHash(pCopy.merkleHash), mHash(pCopy.mHash)
        {
            version = pCopy.version;
            time = pCopy.time;
            targetBits = pCopy.targetBits;
            nonce = pCopy.nonce;
            transactionCount = pCopy.transactionCount;
        }

        Header &operator = (const Header &pRight)
        {
            mHash = pRight.mHash;
            version = pRight.version;
            previousHash = pRight.previousHash;
            merkleHash = pRight.merkleHash;
            time = pRight.time;
            targetBits = pRight.targetBits;
            nonce = pRight.nonce;
            transactionCount = pRight.transactionCount;
            return *this;
        }

        // Verify hash is lower than target difficulty specified by targetBits
        bool hasProofOfWork();

        void write(NextCash::OutputStream *pStream, bool pIncludeTransactionCount) const;

        // pCalculateHash will calculate the hash of the block data while it reads it
        bool read(NextCash::InputStream *pStream, bool pIncludeTransactionCount);

        void clear();

        // Print human readable version to log.
        void print(NextCash::Log::Level pLevel = NextCash::Log::DEBUG);

        const NextCash::Hash &hash() { if(mHash.isEmpty()) calculateHash(); return mHash; }

        // Header
        int32_t version;
        NextCash::Hash previousHash;
        NextCash::Hash merkleHash;
        Time     time;
        uint32_t targetBits;
        uint32_t nonce;

        // Optional transaction count.
        uint32_t transactionCount; // Compact Integer, written as 32 bit locally

        void calculateHash();

        static unsigned int totalCount();

        // Get header from appropriate header file.
        static bool getHeader(unsigned int pHeight, Header &pHeader);
        static bool getHeaders(unsigned int pStartHeight, unsigned int pCount,
          HeaderList &pHeaders);

        // Get hash
        static bool getHash(unsigned int pHeight, NextCash::Hash &pHash);

        // Get hashes
        static bool getHashes(unsigned int pStartHeight, unsigned int pCount,
          NextCash::HashList &pList);

        // Get target bits
        static bool getTargetBits(unsigned int pStartHeight, unsigned int pCount,
          std::vector<uint32_t> &pTargetBits);

        // Get block stats (in reverse order)
        static bool getHeaderStatsReverse(unsigned int pStartHeight, unsigned int pCount,
          std::list<HeaderStat> &pHeaderStats);

        // Add header to appropriate header file.
        static bool add(unsigned int pHeight, Header &pHeader);

        static bool revertToHeight(unsigned int pHeight);

        // Validate header file CRCs and revert to last valid.
        // Returns valid header count.
        static unsigned int validate(bool &pAbort);

        static void save(); // Save any unsaved data in files (i.e. update CRCs)
        static void clean();  // Release any static cache data

    private:

        NextCash::Hash mHash;

    };
}

#endif
