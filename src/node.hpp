/**************************************************************************
 * Copyright 2017 NextCash, LLC                                           *
 * Contributors :                                                         *
 *   Curtis Ellis <curtis@nextcash.tech>                                  *
 * Distributed under the MIT software license, see the accompanying       *
 * file license.txt or http://www.opensource.org/licenses/mit-license.php *
 **************************************************************************/
#ifndef BITCOIN_NODE_HPP
#define BITCOIN_NODE_HPP

#include "mutex.hpp"
#include "thread.hpp"
#include "log.hpp"
#include "hash.hpp"
#include "buffer.hpp"
#include "network.hpp"
#include "base.hpp"
#include "message.hpp"
#include "chain.hpp"
#include "bloom_filter.hpp"
#include "monitor.hpp"

#include <cstdint>
#include <list>

#define BITCOIN_NODE_LOG_NAME "Node"


namespace BitCoin
{
    class Node
    {
    public:

        Node(NextCash::Network::Connection *pConnection, Chain *pChain, bool pIncoming,
          bool pIsSeed, bool pIsGood, uint64_t pServices, Monitor &pMonitor);
        ~Node();

        static void run();

        unsigned int id() { return mID; }
        bool isOpen();
        void close();

        void process();

        void requestStop();

        const char *name() { return mName.text(); }
        bool isIncoming() const { return mIsIncoming; }
        bool isSeed() const { return mIsSeed; }
        bool isGood() const { return mIsGood; }
        // Versions exchanged and initial ping completed
        bool isReady() const { return mPingRoundTripTime != -1; }
        int32_t pingTime() const { return mPingRoundTripTime; }
        void setPingCutoff(uint32_t pPingCutoff) { mPingCutoff = pPingCutoff; }

        // Time that the node connected
        int32_t connectedTime() { return mConnectedTime; }
        // Last time a message was received from this peer
        int32_t lastReceiveTime() { return mLastReceiveTime; }

        unsigned int blockHeight()
        {
            if(mReceivedVersionData == NULL)
                return 0;
            else
                return (unsigned int)mReceivedVersionData->startBlockHeight;
        }

        // Header requests
        bool requestHeaders();
        bool waitingForHeaderRequests();
        const NextCash::Hash &lastHeaderHash() const { return mLastHeaderHash; }

        // Block requests
        bool requestBlocks(NextCash::HashList &pList);
        bool waitingForBlockRequests();
        unsigned int blocksRequestedCount() { return (unsigned int)mBlocksRequested.size(); }
        unsigned int blocksDownloadedCount() const { return mBlockDownloadCount; }
        unsigned int blocksDownloadedSize() const { return mBlockDownloadSize; }
        unsigned int blocksDownloadedTime() const { return mBlockDownloadTime; }
        double blockDownloadBytesPerSecond() const;

        bool hasTransaction(const NextCash::Hash &pHash);
        bool requestTransactions(NextCash::HashList &pList);

        bool requestPeers();

        // Send notification of a new block on the chain
        bool announceBlock(Block *pBlock);

        // Send notification of a new transaction in the mempool
        bool announceTransaction(Transaction *pTransaction);

        // Used to send transactions created by this wallet
        bool sendTransaction(Transaction *pTransaction);

        bool isNewlyReady()
        {
            if(!mWasReady && isReady())
            {
                mWasReady = true;
                return true;
            }

            return false;
        }

        const IPAddress &address() { return mAddress; }
        const uint8_t *ipv6Bytes() const { return mConnection->ipv6Bytes(); }
        bool wasRejected() const { return mRejected; }

        // Add statistics to collection and clear them
        void collectStatistics(Statistics &pCollection);

    private:

        Message::Interpreter mMessageInterpreter;

        // Check if node should be closed
        //   Returns false if closed.
        bool check();

        bool failedStartBytes();

        bool processMessage();

        // Send all initial messages to prepare the node for communication
        void prepare();

        // Release anything (i.e requests) associated with this node
        void release();

        unsigned int mActiveMerkleRequests;
        bool requestMerkleBlock(NextCash::Hash &pHash);

        bool sendMessage(Message::Data *pData);
        bool sendVersion();
        bool sendPing();
        bool sendFeeFilter();
        bool sendReject(const char *pCommand, Message::RejectData::Code pCode, const char *pReason);
        bool sendRejectWithHash(const char *pCommand, Message::RejectData::Code pCode, const char *pReason,
          const NextCash::Hash &pHash);
        bool sendBlock(Block &pBlock);
        bool sendBloomFilter();
        bool sendMerkleBlock(const NextCash::Hash &pBlockHash);

        unsigned int mID;
        NextCash::String mName;
#ifndef SINGLE_THREAD
        NextCash::Thread *mThread;
#endif
        IPAddress mAddress;
        Chain *mChain;
        Monitor *mMonitor;
        NextCash::Mutex mConnectionMutex;
        NextCash::Network::Connection *mConnection;
        NextCash::Buffer mReceiveBuffer;
        Statistics mStatistics;
        bool mStarted, mStopRequested, mStopped;
        bool mIsIncoming, mIsSeed, mIsGood;
        bool mSendBlocksCompact;
        bool mRejected;
        bool mWasReady;
        bool mReleased;

        Message::VersionData *mSentVersionData, *mReceivedVersionData;
        bool mVersionSent, mVersionAcknowledged, mVersionAcknowledgeSent, mSendHeaders, mPrepared;
        int32_t mLastReceiveTime;
        int32_t mLastCheckTime;
        int32_t mLastPingTime;
        int32_t mPingRoundTripTime;
        int32_t mPingCutoff;
        int32_t mLastBlackListCheck;
        int32_t mLastMerkleCheck;
        int32_t mLastMerkleRequest;

        BloomFilter mFilter; // Bloom filter received from peer
        uint64_t mMinimumFeeRate;
        uint64_t mLastPingNonce;
        unsigned int mBloomFilterID; // ID of bloom filter last sent to peer

        unsigned int mBlockDownloadCount;
        unsigned int mBlockDownloadSize;
        unsigned int mBlockDownloadTime;

        NextCash::Hash mHeaderRequested, mLastBlockAnnounced, mLastHeaderRequested, mLastHeaderHash;
        int32_t mHeaderRequestTime;

        NextCash::Mutex mBlockRequestMutex;
        NextCash::HashList mBlocksRequested;
        int32_t mBlockRequestTime, mBlockReceiveTime;

        NextCash::Mutex mAnnounceMutex;
        NextCash::HashList mAnnounceBlocks, mAnnounceTransactions, mSentTransactions;

        void addAnnouncedBlock(const NextCash::Hash &pHash);
        bool addAnnouncedTransaction(const NextCash::Hash &pHash);

        bool mConnected;
        int32_t mConnectedTime;
        unsigned int mMessagesReceived;
        unsigned int mPingCount;

        uint64_t mServices;

        static unsigned int mNextID;

        Node(const Node &pCopy);
        const Node &operator = (const Node &pRight);

    };
}

#endif
