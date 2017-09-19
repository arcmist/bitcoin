/**************************************************************************
 * Copyright 2017 ArcMist, LLC                                            *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@arcmist.com>                                    *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#ifndef BITCOIN_NODE_HPP
#define BITCOIN_NODE_HPP

#include "arcmist/base/mutex.hpp"
#include "arcmist/base/thread.hpp"
#include "arcmist/io/buffer.hpp"
#include "arcmist/io/network.hpp"
#include "base.hpp"
#include "message.hpp"
#include "chain.hpp"

#include <cstdint>
#include <list>


namespace BitCoin
{
    class BlockHashInfo
    {
    public:
        BlockHashInfo(const Hash &pHash, unsigned int pHeight)
        {
            hash = pHash;
            height = pHeight;
        }

        Hash hash;
        unsigned int height;

    private:
        BlockHashInfo(BlockInfo &pCopy);
        BlockHashInfo &operator = (BlockInfo &pRight);
    };

    class Node
    {
    public:

        Node(ArcMist::Network::Connection *pConnection, Chain *pChain, bool pIncoming, bool pIsSeed = false);
        ~Node();

        static void run();

        unsigned int id() { return mID; }
        bool isOpen();
        void close();

        void process(Chain &pChain);

        void stop();

        bool isIncoming() { return mIncoming; }

        // Time that the node connected
        uint32_t connectedTime() { return mConnectedTime; }
        // Last time a message was received from this peer
        uint32_t lastReceiveTime() { return mLastReceiveTime; }

        unsigned int blockHeight() { if(mVersionData == NULL) return 0; else return mVersionData->startBlockHeight; }

        // True if the node is not responding to block hash/header/full requests
        bool notResponding() const;

        bool requestHeaders(Chain &pChain, const Hash &pStartingHash);
        bool waitingForHeaders() { return !mHeaderRequested.isEmpty() && getTime() - mLastHeaderRequest < 300; }

        bool requestBlocks(Chain &pChain, unsigned int pCount, bool pReduceOnly);
        bool waitingForBlocks() { return mBlocksRequested.size() > 0; }

        uint32_t lastBlockRequestTime() { return mLastBlockRequest; }
        uint32_t lastBlockReceiveTime() { return mLastBlockReceiveTime; }
        unsigned int blocksRequestedCount() const { return mBlocksRequestedCount; }
        unsigned int blocksReceivedCount() const { return mBlocksReceivedCount; }

        const IPAddress &address() { return mAddress; }

        // Add statistics to collection and clear them
        void collectStatistics(Statistics &pCollection);

    private:

        int mSocketID;

        bool versionSupported(int32_t pVersion);

        bool sendMessage(Message::Data *pData);
        bool sendVersion(Chain &pChain);
        bool sendReject(const char *pCommand, Message::RejectData::Code pCode, const char *pReason);
        bool sendBlock(Block &pBlock);

        unsigned int mID;
        ArcMist::String mName;
        ArcMist::Thread *mThread;
        IPAddress mAddress;
        Chain *mChain;
        ArcMist::Mutex mConnectionMutex;
        ArcMist::Network::Connection *mConnection;
        ArcMist::Buffer mReceiveBuffer;
        Statistics mStatistics;
        bool mStop, mStopped;
        bool mIncoming, mIsSeed;

        Message::VersionData *mVersionData;
        bool mVersionSent, mVersionAcknowledged, mVersionAcknowledgeSent, mSendHeaders;
        uint32_t mLastReceiveTime;
        uint32_t mLastPingTime;
        uint64_t mPingNonce;
        uint64_t mMinimumFeeRate;

        Hash mHeaderRequested;
        uint32_t mLastHeaderRequest;

        ArcMist::Mutex mBlockRequestMutex;
        HashList mBlocksRequested;
        uint32_t mLastBlockRequest;
        uint32_t mLastBlockReceiveTime;
        unsigned int mBlocksRequestedCount;
        unsigned int mBlocksReceivedCount;

        bool mConnected;
        uint32_t mConnectedTime;
        unsigned int mMessagesReceived;

        static unsigned int mNextID;

    };
}

#endif
