/**************************************************************************
 * Copyright 2018 NextCash, LLC                                            *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#ifndef BITCOIN_BLOOM_HPP
#define BITCOIN_BLOOM_HPP

#include "hash.hpp"
#include "math.hpp"
#include "buffer.hpp"
#include "stream.hpp"
#include "digest.hpp"
#include "transaction.hpp"


namespace BitCoin
{
    class BloomFilter
    {
    public:

        enum Format
        {
            STANDARD,
            GRAPHENE
        };

        enum Flags
        {
            UPDATE_NONE = 0,
            UPDATE_ALL = 1,
            UPDATE_P2PUBKEY_ONLY = 2, // Only adds outpoints to the filter if the output is a pay-to-pubkey/pay-to-multisig script
            UPDATE_MASK = 3
        };

        static const unsigned int MAX_SIZE; // bytes
        static const unsigned int MAX_FUNCTIONS;
        static const unsigned int MIN_FUNCTIONS;

        BloomFilter(Format pFormat)
        {
            mFormat = pFormat;
            mData = NULL;
            mDataSize = 0;
            mHashFunctionCount = 0;
            mTweak = 0;
            mFlags = 0;
            mIsFull = false;
            mIsEmpty = true;
        }
        BloomFilter(const BloomFilter &pCopy)
        {
            mFormat = pCopy.mFormat;
            mData = NULL;
            mDataSize = pCopy.mDataSize;
            mHashFunctionCount = pCopy.mHashFunctionCount;
            mTweak = pCopy.mTweak;
            mFlags = pCopy.mFlags;
            mIsFull = pCopy.mIsFull;
            mIsEmpty = pCopy.mIsEmpty;

            if(mDataSize)
            {
                mData = new unsigned char[mDataSize];
                std::memcpy(mData, pCopy.mData, mDataSize);
            }
        }
        BloomFilter(Format pFormat, unsigned int pElementCount, unsigned char pFlags = UPDATE_NONE,
          double pFalsePositiveRate = 0.00001, unsigned int pTweak = NextCash::Math::randomInt());
        ~BloomFilter();

        const BloomFilter &operator = (const BloomFilter &pRight);

        void setup(unsigned int pElementCount, unsigned char pFlags = UPDATE_NONE,
          double pFalsePositiveRate = 0.00001, unsigned int pTweak = NextCash::Math::randomInt());

        bool isEmpty() const { return mIsEmpty; }
        bool isFull() const { return mIsFull; }

        bool flags() const { return mFlags; }
        const unsigned char *data() const { return mData; }
        unsigned int size() const { return mDataSize; }
        unsigned int functionCount() const { return mHashFunctionCount; }
        unsigned int tweak() const { return mTweak; }
        void updateStatus();

        void add(const NextCash::Hash &pHash);
        void add(Outpoint &pOutpoint);
        void addData(NextCash::Buffer &pData);
        void addScript(NextCash::Buffer &pScript);

        bool contains(const NextCash::Hash &pHash) const;
        bool contains(Outpoint &pOutpoint) const;
        bool contains(Transaction &pTransaction) const;
        bool contains(TransactionReference &pTransaction) const;
        bool containsScript(NextCash::Buffer &pScript) const;

        void write(NextCash::OutputStream *pStream) const;
        bool read(NextCash::InputStream *pStream);

        void clear();
        void assign(BloomFilter &pValue); // Removes data from pValue
        void copy(BloomFilter &pValue); // Makes equivalent to pValue

        static bool test();

    private:

        unsigned int bitOffset(unsigned int pHashNum, const NextCash::Hash &pHash) const
        {
            return NextCash::Digest::murMur3(pHash.data(), pHash.size(),
              pHashNum * 0xFBA4C795 + mTweak) % (mDataSize * 8);
        }

        unsigned int bitOffset(unsigned int pHashNum, const uint8_t *pData,
          NextCash::stream_size pDataSize) const
        {
            return NextCash::Digest::murMur3(pData, pDataSize, pHashNum * 0xFBA4C795 + mTweak) %
              (mDataSize * 8);
        }

        unsigned int bitOffset(unsigned int pHashNum, NextCash::Buffer &pData) const
        {
            return NextCash::Digest::murMur3(pData.begin(), pData.length(),
              pHashNum * 0xFBA4C795 + mTweak) % (mDataSize * 8);
        }

        Format mFormat;
        unsigned char *mData;
        unsigned int mDataSize;
        unsigned int mHashFunctionCount;
        unsigned int mTweak;
        unsigned char mFlags;

        bool mIsFull;
        bool mIsEmpty;

    };
}

#endif
